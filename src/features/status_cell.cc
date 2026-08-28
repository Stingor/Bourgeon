#include "features/status_cell.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "features/systems/bourgeon_opcodes.h"  // la plage des altérations
#include "ragnarok/lua.h"
#include "ragnarok/msgstring.h"
#include "ui/game_texture.h"
#include "ui/ro_imgui.h"
#include "utils/i18n.h"

namespace statuscell {
namespace {

// ── Le voile de la part ÉCOULÉE ─────────────────────────────────────────────
//
// `spent` va de 0 (l'état vient de commencer) à 1 (il finit).
//
// 🔴 Un ÉVENTAIL DE TRIANGLES, pas un `PathFillConvex`. Un secteur de plus d'un
// demi-tour n'est pas convexe : le remplissage convexe d'ImGui l'aurait tronqué
// en travers dès que l'état dépasse la moitié de sa course — c'est-à-dire la
// plupart du temps.
//
// 🔴 L'anti-aliasing du remplissage est COUPÉ le temps de l'éventail. Deux
// triangles voisins y posent chacun leur bord adouci, et ces bords se
// superposent : on verrait des rayons plus clairs partir du centre, comme les
// rayons d'une roue.
//
// Le rayon déborde volontairement de la case, et c'est le CLIP qui donne au
// balayage sa forme carrée. Sans ce débordement, les coins resteraient clairs
// alors que le secteur les a dépassés.
void RadialSweep(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float spent, ImU32 col) {
  // Pas `IM_PI` : il vit dans imgui_internal.h, que ce fichier n'inclut pas.
  constexpr float kPi = 3.14159265358979323846f;
  const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  const float radius = (p1.x - p0.x) * 0.78f;  // > demi-diagonale (0.707)
  const int segments = 48;
  const int used = static_cast<int>(segments * spent + 0.5f);
  if (used <= 0) return;

  const ImDrawListFlags saved = dl->Flags;
  dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
  dl->PushClipRect(p0, p1, true);
  const float start = -kPi * 0.5f;  // midi
  for (int i = 0; i < used; ++i) {
    const float a0 = start + (2.0f * kPi) * (static_cast<float>(i) / segments);
    const float a1 =
        start + (2.0f * kPi) * (static_cast<float>(i + 1) / segments);
    dl->AddTriangleFilled(
        c, ImVec2(c.x + cosf(a0) * radius, c.y + sinf(a0) * radius),
        ImVec2(c.x + cosf(a1) * radius, c.y + sinf(a1) * radius), col);
  }
  dl->PopClipRect();
  dl->Flags = saved;
}

// « 12 », « 1:05 », « 12:30 ». Le format COMPACT, celui qui tient sous une
// icône : une durée s'y lit d'un coup d'œil ou n'y sert à rien.
void FormatRemain(uint32_t ms, char* out, size_t cap) {
  const unsigned total_s = ms / 1000u;
  if (total_s < 60u) {
    std::snprintf(out, cap, "%u", total_s);
    return;
  }
  std::snprintf(out, cap, "%u:%02u", total_s / 60u, total_s % 60u);
}

// Le format LONG, mot pour mot celui du client : « 2 minute(s) 53 seconde(s) ».
//
// 🔴 Les unités viennent de SES msgstring, pas de nous : 0x6A1 heure, 0x70F
// minute, 0x710 seconde — les trois que `sub_C85A10` assemble pour son propre
// tooltip d'état, chacune précédée du nombre et d'une espace (« %d %s »).
//
// Les prendre là plutôt que d'écrire « min » et « s » a deux effets : le texte
// est exactement celui que le joueur lit déjà partout ailleurs dans le client,
// et il suit la traduction — le catalogue msgstring de Bourgeon les traduit
// comme le reste.
//
// ⚠ On ne peut PAS appeler `sub_C85A10` : elle cherche l'état dans
// `g_StatusEffectList`, le vecteur global — c'est-à-dire MES états à moi. Sur
// une cible tierce elle ne trouverait rien et rendrait une chaîne vide.
void FormatRemainLong(uint32_t ms, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  const unsigned total_s = ms / 1000u;
  const unsigned h = total_s / 3600u;
  const unsigned m = (total_s % 3600u) / 60u;
  const unsigned sec = total_s % 60u;

  char tmp[128];
  int n = 0;
  if (h > 0)
    n += std::snprintf(tmp + n, sizeof(tmp) - n, "%u %s", h,
                       msgstr::Utf8Or(0x6A1, "h"));
  if (m > 0)
    n += std::snprintf(tmp + n, sizeof(tmp) - n, "%s%u %s", n ? " " : "", m,
                       msgstr::Utf8Or(0x70F, "min"));
  // Les secondes s'affichent aussi quand TOUT est à zéro : « 0 seconde(s) » se
  // lit, une ligne vide non.
  if (sec > 0 || n == 0)
    std::snprintf(tmp + n, sizeof(tmp) - n, "%s%u %s", n ? " " : "", sec,
                  msgstr::Utf8Or(0x710, "s"));

  std::snprintf(out, cap, "%s", tmp);
}

// ── Les alterations, faute d'icone ──────────────────────────────────────────
//
// 🔴 Le client n'a AUCUNE image pour elles : ni EFST, ni fichier. Il les rend
// sur le sprite (les Z du sommeil, la bulle du silence), ce qu'une liste d'états
// ne peut pas reproduire. On dessine donc une pastille : trois lettres sur un
// fond coloré, lisible à toute taille et sans dépendre du moindre asset.
//
// La couleur groupe par FAMILLE — immobilisation en bleu, poison en vert,
// aveuglement en gris — pour qu'on lise la nature d'un coup d'œil avant même de
// déchiffrer les lettres.
struct AilmentLook {
  const char* abbrev;
  const char* name;   // clé de traduction, développée par l'appelant
  ImU32       color;
};

const AilmentLook* LookupAilment(uint16_t efst) {
  // ⚠ Indexé par la valeur de `bopcodes::Ailment`, donc l'ORDRE compte : il
  // suit celui de `e_bourgeon_ailment` côté serveur.
  static const AilmentLook kLooks[] = {
      {"", "", 0},                                                  // 0 inutilisé
      {"PET", "Pétrifié",       IM_COL32(120, 120, 130, 235)},      // kAilStone
      {"GEL", "Gelé",           IM_COL32( 70, 150, 210, 235)},      // kAilFreeze
      {"ETD", "Étourdi",        IM_COL32(210, 175,  60, 235)},      // kAilStun
      {"DOR", "Endormi",        IM_COL32( 90, 110, 190, 235)},      // kAilSleep
      {"PET", "Pétrification",  IM_COL32(140, 130, 110, 235)},      // kAilStoneWait
      {"FEU", "En feu",         IM_COL32(210,  90,  45, 235)},      // kAilBurning
      {"EMP", "Emprisonné",     IM_COL32(100,  90, 140, 235)},      // kAilImprison
      {"PSN", "Empoisonné",     IM_COL32( 95, 165,  80, 235)},      // kAilPoison
      {"MAL", "Maudit",         IM_COL32(160,  70, 150, 235)},      // kAilCurse
      {"SIL", "Silence",        IM_COL32(130, 130, 175, 235)},      // kAilSilence
      {"CNF", "Confus",         IM_COL32(180, 120, 200, 235)},      // kAilConfusion
      {"AVG", "Aveuglé",        IM_COL32( 80,  80,  90, 235)},      // kAilBlind
      {"SNG", "Saignement",     IM_COL32(185,  55,  55, 235)},      // kAilBleeding
      {"PSN", "Poison mortel",  IM_COL32( 60, 130,  60, 235)},      // kAilDeadlyPoison
      {"PEU", "Terrifié",       IM_COL32(150, 100,  60, 235)},      // kAilFear
  };
  if (!bopcodes::IsAilment(efst)) return nullptr;
  const uint16_t idx = efst - bopcodes::kAilmentBase;
  if (idx == 0 || idx >= IM_ARRAYSIZE(kLooks)) return nullptr;
  return &kLooks[idx];
}

// Cette ligne ne porte-t-elle QUE des marqueurs de format ?
//
// On retire les `%s`, `%d` et consorts, puis la ponctuation et les espaces : ce
// qui reste est le texte véritable. Vide, la ligne était un emplacement destiné
// au client, pas une phrase à lire.
//
// ⚠ Une ligne qui contient un marqueur ET du texte est GARDÉE, marqueur compris.
// La supprimer perdrait l'information (« Réduit le temps de %s »), et nous n'avons
// pas de quoi la remplir : le client tire ces valeurs d'un contexte qu'il est
// seul à avoir.
bool OnlyFormatMarkers(const char* s) {
  if (s == nullptr) return true;
  for (const char* p = s; *p; ++p) {
    if (*p == '%') {
      // Sauter le marqueur : `%` suivi d'éventuels chiffres puis d'une lettre.
      ++p;
      while (*p && (*p >= '0' && *p <= '9')) ++p;
      if (*p == '\0') break;   // un `%` final, seul : rien de plus à sauter
      continue;                // la lettre du marqueur est consommée par la boucle
    }
    const unsigned char c = static_cast<unsigned char>(*p);
    // Tout ce qui n'est ni espace ni ponctuation compte comme du texte. Le test
    // porte sur les octets >= 0x80 aussi : une description en coréen ou en
    // français accentué ne doit pas passer pour vide.
    if (c >= 0x80) return false;
    if (c != ' ' && c != '\t' && c != ':' && c != '-' && c != ',' && c != '.' &&
        c != '/' && c != '(' && c != ')' && c != '[' && c != ']')
      return false;
  }
  return true;
}

// Le texte du client pour cet état, ligne par ligne, mémorisé.
//
// 🔴 Mémorisé PARCE QUE c'est du Lua : `GetStateIconDescript` traverse
// l'interpréteur à chaque appel, et une barre pleine d'icônes le rappellerait
// pour chacune, à chaque frame. Le résultat, lui, ne change jamais.
struct Text {
  std::string name;
  std::vector<std::string> lines;  // la description, ligne par ligne
  // 🔴 L'index où le CLIENT met la durée, ou -1 s'il n'en réserve pas.
  //
  // Une ligne qui n'est qu'un marqueur de format (« %s ») n'est pas du texte
  // manquant : c'est l'emplacement que `StatusIcon_BuildTooltip` remplit avec le
  // temps restant. On retient donc SA PLACE au lieu de la jeter — le texte se
  // mémorise une fois, mais la durée change à chaque frame et ne peut pas être
  // mise en cache avec lui.
  int time_line = -1;
};

const Text& Lookup(uint16_t efst) {
  static std::unordered_map<uint16_t, Text> cache;
  auto it = cache.find(efst);
  if (it != cache.end()) return it->second;

  Text t;
  char buf[256];
  // Ligne 1 = le nom, les suivantes = la description. C'est le découpage du
  // client lui-même (`StatusIcon_BuildTooltip` boucle jusqu'à l'échec).
  for (int line = 1; line <= 8; ++line) {
    if (!lua::StateIconLine(static_cast<int>(efst), line, buf, sizeof(buf)))
      break;
    if (line == 1) {
      t.name = buf;
    } else if (OnlyFormatMarkers(buf)) {
      // L'emplacement du temps. On garde sa PLACE (la première, s'il y en avait
      // plusieurs) et on y mettra la durée au rendu.
      if (t.time_line < 0) t.time_line = static_cast<int>(t.lines.size());
    } else {
      t.lines.push_back(buf);
    }
  }
  return cache.emplace(efst, std::move(t)).first->second;
}

}  // namespace

const char* Name(uint16_t efst) {
  if (const AilmentLook* look = LookupAilment(efst)) return i18n::Tr(look->name);
  return Lookup(efst).name.c_str();
}

void Tooltip(const StatusEffects::Entry& e) {
  // Une altération n'a pas de texte Lua : son nom est le nôtre.
  if (const AilmentLook* look = LookupAilment(e.efst)) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(i18n::Tr(look->name));
    if (e.expires_ms != 0) {
      char when[160];
      const int32_t left =
          static_cast<int32_t>(e.expires_ms - ::timeGetTime());
      FormatRemainLong(left > 0 ? static_cast<uint32_t>(left) : 0u, when,
                       sizeof(when));
      ImGui::TextDisabled("%s", when);
    }
    ImGui::EndTooltip();
    return;
  }

  const Text& t = Lookup(e.efst);
  ImGui::BeginTooltip();

  // Le texte du client arrive dans SA code-page : LocalToUtf8, jamais
  // Cp949ToUtf8 — c'est le client qui parle, pas le protocole.
  if (!t.name.empty()) {
    ImGui::TextUnformatted(ro::LocalToUtf8(t.name.c_str()));
  } else {
    // Sans nom, on montre au moins de QUOI on parle : un identifiant se cherche,
    // un carré vide ne se cherche pas.
    ImGui::Text("%s %u", i18n::Tr("État"), static_cast<unsigned>(e.efst));
  }

  // La durée, au format long du client (« 2 minute(s) 53 seconde(s) »).
  char when[160];
  when[0] = '\0';
  if (e.expires_ms != 0) {
    const int32_t left_ms = static_cast<int32_t>(e.expires_ms - ::timeGetTime());
    FormatRemainLong(left_ms > 0 ? static_cast<uint32_t>(left_ms) : 0u, when,
                     sizeof(when));
  }

  // 🔴 À LA PLACE QUE LE CLIENT LUI DONNE quand il en réserve une. Son propre
  // tooltip la met au milieu de la description, pas en tête : suivre sa mise en
  // page évite que deux infobulles du même jeu se lisent différemment.
  //
  // Sans emplacement réservé, la durée passe juste sous le nom — il faut bien
  // qu'elle soit quelque part, et c'est là qu'on la cherche.
  const bool has_slot = (t.time_line >= 0);
  if (!has_slot) {
    if (when[0]) {
      ImGui::TextDisabled("%s", when);
    } else {
      // ⚠ Pas « 0 » : un état sans échéance ne finit pas, et un zéro le ferait
      // croire terminé.
      ImGui::TextDisabled("%s", i18n::Tr("Sans durée"));
    }
  }

  if (!t.lines.empty() || (has_slot && when[0])) {
    ImGui::Separator();
    for (size_t i = 0; i < t.lines.size(); ++i) {
      if (has_slot && static_cast<int>(i) == t.time_line && when[0])
        ImGui::TextDisabled("%s", when);
      ImGui::TextUnformatted(ro::LocalToUtf8(t.lines[i].c_str()));
    }
    // L'emplacement était en DERNIÈRE position : la boucle ne l'a pas atteint.
    if (has_slot && t.time_line >= static_cast<int>(t.lines.size()) && when[0])
      ImGui::TextDisabled("%s", when);
  }
  ImGui::EndTooltip();
}

bool Draw(const StatusEffects::Entry& e, ImVec2 p0, ImVec2 p1,
          const Style& style, bool tooltip) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 tint =
      style.dim ? IM_COL32(140, 140, 140, 220) : IM_COL32_WHITE;

  // Une ALTÉRATION n'a pas d'image : pastille. Tout le reste — grisage, compte à
  // rebours, infobulle — s'applique ensuite à l'identique, c'est bien pour ça
  // que ces deux rendus vivent dans la même fonction.
  if (const AilmentLook* look = LookupAilment(e.efst)) {
    const float rounding = (p1.x - p0.x) * 0.18f;
    dl->AddRectFilled(p0, p1, look->color, rounding);
    dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 170), rounding);
    ImFont* font = ImGui::GetFont();
    // La police occupe 55 % de la case : trois lettres y tiennent en largeur
    // sans qu'on ait à mesurer deux fois.
    const float fsz = (p1.y - p0.y) * 0.55f;
    const ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, look->abbrev);
    const ImVec2 tp(p0.x + ((p1.x - p0.x) - ts.x) * 0.5f,
                    p0.y + ((p1.y - p0.y) - ts.y) * 0.5f);
    dl->AddText(font, fsz, ImVec2(tp.x + 1.0f, tp.y + 1.0f),
                IM_COL32(0, 0, 0, 190), look->abbrev);
    dl->AddText(font, fsz, tp, IM_COL32_WHITE, look->abbrev);
  } else {
    const char* path = StatusEffects::IconPath(e.efst);
    if (path == nullptr) return false;
    const ro::GameTexture icon = ro::CachedTextureFromGameFile(path);
    if (!icon.tex) return false;
    dl->AddImage(reinterpret_cast<ImTextureID>(icon.tex), p0, p1, ImVec2(0, 0),
                 ImVec2(1, 1), tint);
  }

  const uint32_t now = ::timeGetTime();
  const int32_t left_ms =
      (e.expires_ms == 0) ? 0 : static_cast<int32_t>(e.expires_ms - now);

  // ── Le grisage ────────────────────────────────────────────────────────────
  // ⚠ Un état SANS échéance n'a pas de part écoulée : on ne le voile pas. Le
  // griser à moitié le ferait croire à mi-course, alors qu'il ne finira pas.
  if (style.sweep != kSweepNone && e.expires_ms != 0 && e.total_ms > 0) {
    const float left = (left_ms > 0) ? static_cast<float>(left_ms) : 0.0f;
    float spent = 1.0f - left / static_cast<float>(e.total_ms);
    spent = std::max(0.0f, std::min(1.0f, spent));
    if (style.sweep == kSweepRadial) {
      RadialSweep(dl, p0, p1, spent, style.sweep_color);
    } else {
      // Le voile DESCEND : la part sombre coiffe la case et sa frontière glisse
      // vers le bas, le clair qui subsiste étant ce qu'il reste de temps. Le
      // commentaire disait « monte », ce qui décrivait l'inverse du code juste
      // en dessous — et le libellé du réglage répétait l'erreur.
      dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + (p1.y - p0.y) * spent),
                        style.sweep_color);
    }
  }

  // ── Le compte à rebours ───────────────────────────────────────────────────
  if (style.time_px > 0.0f && e.expires_ms != 0 && left_ms > 0) {
    char txt[16];
    FormatRemain(static_cast<uint32_t>(left_ms), txt, sizeof(txt));
    ImFont* font = ImGui::GetFont();
    const ImVec2 ts = font->CalcTextSizeA(style.time_px, FLT_MAX, 0.0f, txt);
    const ImVec2 tp(p0.x + ((p1.x - p0.x) - ts.x) * 0.5f, p1.y);
    // Ombre portée : ces chiffres se lisent sur n'importe quel décor.
    dl->AddText(font, style.time_px, ImVec2(tp.x + 1.0f, tp.y + 1.0f),
                IM_COL32(0, 0, 0, 200), txt);
    dl->AddText(font, style.time_px, tp, IM_COL32_WHITE, txt);
  }

  // ⚠ `IsMouseHoveringRect` et non `IsItemHovered` : on n'a posé AUCUN item
  // ImGui, seulement des primitives de dessin. Il n'y a donc rien à interroger.
  if (tooltip && ImGui::IsMouseHoveringRect(p0, p1)) Tooltip(e);
  return true;
}

}  // namespace statuscell
