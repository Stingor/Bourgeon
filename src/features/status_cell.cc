#include "features/status_cell.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "features/link_gesture.h"  // MAJ + clic : le lien
#include "ragnarok/lua.h"
#include "ragnarok/msgstring.h"
#include "ui/game_texture.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"  // mui::WheelSliderInt / HelpMarker / SameLine
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

// ── Les DEUX états que le client ne sait pas dessiner ───────────────────────
//
// Tout le reste des altérations a désormais sa VRAIE icône : le serveur envoie
// `EFST_BODYSTATE_SLEEP`, `EFST_HEALTHSTATE_SILENCE`… — de vrais EFST, que le
// client résout tout seul en `BD_Sleep.tga`, `HL_Silence.tga`, avec son nom et
// son texte. Il n'y avait rien à inventer, juste à les nommer côté serveur.
//
// 🔴 Restent BLIND (887) et BLOODING (889) : ils existent dans l'énumération,
// mais `efstids.lub` ne les déclare pas et `stateiconimginfo.lub` n'a pas
// d'image pour eux. Sans ce repli, l'aveuglement et le saignement seraient
// invisibles — deux altérations qu'on veut justement voir sur une cible.
//
// ⚠ Ces valeurs sont celles du CLIENT (efstids.lub), pas des constantes à nous.
constexpr uint16_t kEfstBlind    = 887;
constexpr uint16_t kEfstBleeding = 889;

struct AilmentLook {
  const char* abbrev;
  const char* name;   // clé de traduction, développée par l'appelant
  ImU32       color;
};

const AilmentLook* LookupAilment(uint16_t efst) {
  static const AilmentLook kBlind = {"AVG", "Aveuglé",
                                     IM_COL32(80, 80, 90, 235)};
  static const AilmentLook kBleed = {"SNG", "Saignement",
                                     IM_COL32(185, 55, 55, 235)};
  if (efst == kEfstBlind) return &kBlind;
  if (efst == kEfstBleeding) return &kBleed;
  return nullptr;
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

// ── L'aperçu ────────────────────────────────────────────────────────────────
//
// Régler un affichage d'états suppose d'en AVOIR : sans buff, il n'y a rien à
// dimensionner, et attendre d'être en combat pour choisir une taille d'icône
// n'est pas un réglage, c'est une devinette.
//
// 🔴 On ne fabrique pas d'ids au hasard : un EFST sans image ne dessine RIEN
// (`Draw` rend faux), et l'aperçu montrerait un affichage à moitié vide en
// prétendant qu'il est plein. On balaie donc l'énumération et on ne garde que
// ceux dont le client sait sortir une icône.
//
// ⚠ Le balayage passe par le Lua du client : il est fait UNE FOIS et gardé. Le
// refaire à chaque frame coûterait un millier d'appels Lua par image.
const std::vector<uint16_t>& PreviewIds() {
  static std::vector<uint16_t> ids;
  static bool scanned = false;
  if (!scanned) {
    scanned = true;
    // La borne couvre l'énumération du client avec de la marge ; au-delà il n'y
    // a plus d'EFST déclaré.
    for (uint16_t id = 1; id < 1200 && ids.size() < 100; ++id) {
      if (StatusEffects::IconPath(id) != nullptr) ids.push_back(id);
    }
  }
  return ids;
}

}  // namespace

// ── La rangée ───────────────────────────────────────────────────────────────
RowResult DrawRow(const std::vector<StatusEffects::Entry>& list, ImVec2 origin,
                  const RowOpts& opts, const Style& style, bool tooltip,
                  bool* took_hover) {
  RowResult out;
  out.edge = origin.x;
  if (list.empty()) return out;

  const float side = (opts.side > 1.0f) ? opts.side : 1.0f;
  const float gap  = (opts.gap  > 0.0f) ? opts.gap  : 0.0f;
  const int   rows = (opts.rows > 1) ? opts.rows : 1;
  const int   max  = (opts.max  > 1) ? opts.max  : 1;
  // Le plafond se RÉPARTIT sur les lignes : six icônes sur deux lignes font
  // trois par ligne, et non six puis six.
  const int per_row = (max + rows - 1) / rows;

  // Le pas vertical embarque le texte : sans lui, un compte à rebours débordait
  // sur la rangée du dessous dès qu'on ajoutait une ligne.
  const float line_h = side + gap + style.time_px;

  // ── L'ordre ───────────────────────────────────────────────────────────────
  //
  // On trie des INDEX, pas la liste : elle appartient à l'appelant, et deux
  // surfaces peuvent la présenter différemment sans se marcher dessus.
  std::vector<size_t> order;
  order.reserve(list.size());
  for (size_t i = 0; i < list.size(); ++i) order.push_back(i);

  if (opts.sort == kSortEndingSoon || opts.sort == kSortLongest) {
    const uint32_t now = ::timeGetTime();
    const bool soon = (opts.sort == kSortEndingSoon);
    std::stable_sort(
        order.begin(), order.end(), [&](size_t ia, size_t ib) {
          const StatusEffects::Entry& a = list[ia];
          const StatusEffects::Entry& b = list[ib];
          // 🔴 Un état SANS échéance n'a pas de durée à comparer, et il va en
          // FIN dans les DEUX sens. Le traiter comme « très long » le mettrait
          // en tête du tri décroissant, chassant de l'écran ce qui presse —
          // c'est la règle que la barre de cible avait déjà apprise, et elle
          // gagne ici pour les quatre surfaces.
          const bool a_inf = (a.expires_ms == 0);
          const bool b_inf = (b.expires_ms == 0);
          if (a_inf != b_inf) return b_inf;
          if (a_inf) return false;  // deux permanents : l'ordre d'arrivée tient
          const uint32_t la = a.expires_ms - now;
          const uint32_t lb = b.expires_ms - now;
          return soon ? (la < lb) : (la > lb);
        });
  } else if (opts.newest_first) {
    // Pas un tri : l'ordre d'arrivée, pris par la fin. `Effects` rend les états
    // du plus ancien au plus récent, et quand il y en a plus que la place, ce
    // sont les derniers tombés qu'il faut garder.
    std::reverse(order.begin(), order.end());
  }

  // ── La pose ───────────────────────────────────────────────────────────────
  const float step = opts.rtl ? -(side + gap) : (side + gap);
  float x = origin.x;
  // ⚠ Ancrée en BAS, la rangée doit remonter de la hauteur du compte à
  // rebours : celui-ci se dessine SOUS l'icône, et la première ligne collée au
  // bord l'aurait poussé hors du cadre.
  float y = origin.y - (opts.up ? style.time_px : 0.0f);
  float far_edge = origin.x;
  int   line = 0;        // ligne COURANTE, comptée à part
  int   on_line = 0;     // cases posées sur cette ligne

  for (size_t k = 0; k < order.size() && out.drawn < max; ++k) {
    const StatusEffects::Entry& e = list[order[k]];

    // Deux causes de retour à la ligne, et il faut les deux : le nombre par
    // ligne (on peut vouloir une rangée haute et étroite alors que la place ne
    // l'impose pas), et le bord du cadre (on ne déborde jamais).
    //
    // ⚠ La ligne se compte À PART, et non par `drawn / per_row` : quand c'est
    // la LARGEUR qui force le retour, une ligne se termine avant son quota et
    // la division sous-estime alors le nombre de lignes réellement occupées —
    // la rangée débordait du cadre par le bas.
    bool past_limit = false;
    if (opts.limit != 0.0f && on_line > 0) {
      past_limit = opts.rtl ? (x - side < opts.limit) : (x + side > opts.limit);
    }
    if ((on_line >= per_row) || past_limit) {
      if (line + 1 >= rows) break;  // plus de ligne disponible
      ++line;
      on_line = 0;
      x = origin.x;
      y += opts.up ? -line_h : line_h;
    }

    const float x0 = opts.rtl ? (x - side) : x;
    const float y0 = opts.up  ? (y - side) : y;
    // ⚠ Une case qui ne se dessine pas ne prend PAS sa place : sans ce test, un
    // état sans icône laissait un trou dans la rangée.
    if (!Draw(e, ImVec2(x0, y0), ImVec2(x0 + side, y0 + side), style, tooltip,
              took_hover))
      continue;

    x += step;
    if (opts.rtl ? (x < far_edge) : (x > far_edge)) far_edge = x;
    ++on_line;
    ++out.drawn;
  }

  if (out.drawn > 0) {
    // `far_edge` est le curseur APRÈS le dernier pas, donc un `gap` au-delà de
    // la case. Le retirer rend le bord de la case elle-même, dans les deux sens.
    out.edge = opts.rtl ? (far_edge + gap) : (far_edge - gap);
    const int widest = (out.drawn < per_row) ? out.drawn : per_row;
    out.size.x = static_cast<float>(widest) * (side + gap) - gap;
    out.size.y = static_cast<float>(line + 1) * line_h - gap;
  }
  return out;
}

// Les durées de l'aperçu.
//
// 🔴 CHOISIES POUR CE QU'ELLES FONT VOIR, pas tirées d'une formule. La première
// version étageait mécaniquement les totaux, et tous les restants tombaient
// entre quinze et vingt-six secondes : le compte à rebours ne montrait qu'une
// seule largeur de texte, et le voile qu'un seul taux de remplissage. On ne
// réglait donc rien qu'on pût vérifier.
//
// Chaque ligne couvre un cas d'affichage distinct — et les ordres de grandeur
// sont ceux du jeu : une altération dure quelques secondes, un buff de soutien
// quelques minutes, une bénédiction de guilde une heure.
struct Fake {
  uint32_t total_s;  // 0 = permanent
  uint32_t left_s;
};
const Fake kFakes[] = {
    {  240,  183},  // buff frais : voile à peine entamé, « 3m » à deux chiffres
    {   30,    7},  // altération courte, voile aux trois quarts
    { 1800, 1324},  // 22 min — la plus grande largeur de texte courante
    {    5,    2},  // le chiffre qui change à vue d'œil, voile presque plein
    {   60,   28},  // à mi-course, le cas le plus lisible du grisage
    { 3600, 3011},  // ~50 min : le passage aux dizaines de minutes
    {    0,    0},  // permanent : ni voile ni compte à rebours
    {  240,   45},  // même total que la première, en FIN de course
    {  600,  512},  // long et presque plein
    {  120,    1},  // sur le point de tomber — le cas limite du voile
};

void AppendPreview(std::vector<StatusEffects::Entry>* out, int want) {
  if (out == nullptr) return;
  const std::vector<uint16_t>& ids = PreviewIds();
  if (ids.empty()) return;
  const uint32_t now = ::timeGetTime();
  const size_t target = static_cast<size_t>(want > 1 ? want : 1);

  for (size_t k = 0; out->size() < target && k < ids.size(); ++k) {
    const uint16_t id = ids[k];
    // Ne pas doubler un état réellement actif : l'affichage montrerait deux fois
    // la même icône, et le tri semblerait cassé.
    bool already = false;
    for (const StatusEffects::Entry& e : *out)
      if (e.efst == id) { already = true; break; }
    if (already) continue;

    StatusEffects::Entry e;
    e.efst = id;
    const Fake& f = kFakes[k % (sizeof(kFakes) / sizeof(kFakes[0]))];
    if (f.total_s == 0) {
      e.expires_ms = 0;  // permanent : ni voile ni compte à rebours
      e.total_ms   = 0;
    } else {
      // 🔴 CYCLIQUE, et c'est tout l'intérêt. Recalculer `now + restant` à
      // chaque frame FIGERAIT le compte à rebours sur sa valeur de départ : ni
      // le texte ni le voile ne bougeraient, et on réglerait un affichage
      // immobile qui ne ressemble pas à celui qu'on aura. À l'inverse, une
      // échéance posée une fois pour toutes viderait l'aperçu en quelques
      // secondes — les entrées courtes sont justement celles qu'on veut voir.
      //
      // On dérive donc l'écoulé d'une horloge MODULO la durée : l'état s'écoule
      // vraiment, et se relance tout seul quand il tombe.
      const uint32_t period  = f.total_s * 1000u;
      const uint32_t elapsed = (now + (period - f.left_s * 1000u)) % period;
      e.expires_ms = now + (period - elapsed);
      e.total_ms   = period;
    }
    out->push_back(e);
  }
}

bool HasFallback(uint16_t efst) { return LookupAilment(efst) != nullptr; }

const char* Name(uint16_t efst) {
  if (const AilmentLook* look = LookupAilment(efst)) return i18n::Tr(look->name);
  return Lookup(efst).name.c_str();
}

// ── Les couleurs de l'infobulle ─────────────────────────────────────────────
//
// 🔴 ELLE IMPOSE SON FOND, et ce n'est pas de la coquetterie : `PushSkinColors`
// pose un `PopupBg` CLAIR dans toute fenêtre RO, alors qu'un overlay garde le
// fond sombre d'ImGui. La MÊME infobulle avait donc deux fonds selon l'endroit
// d'où on la survolait — claire dans la fenêtre Groupe/Amis et dans un lien de
// chat, sombre au-dessus des barres — pendant que son texte, lui, gardait la
// couleur par défaut. Blanc sur beige : illisible une fois sur deux.
//
// ⚠ `TextDisabled` d'ImGui est calibré pour un fond SOMBRE ; sur ce beige il
// s'efface presque entièrement. D'où un gris à part pour le texte secondaire,
// celui de la palette du projet.
constexpr ImU32 kTipBg   = IM_COL32(0xF2, 0xF3, 0xF6, 255);
constexpr ImU32 kTipText = IM_COL32(24, 22, 20, 255);
constexpr ImU32 kTipDim  = IM_COL32(89, 89, 107, 255);

// ── Le CADRE de l'infobulle ─────────────────────────────────────────────────
//
// 🔴 ELLE IMPOSE AUSSI SA GÉOMÉTRIE, et pour la même raison qu'elle impose son
// fond : une infobulle hérite des `PushStyleVar` de la fenêtre D'OÙ ON SURVOLE.
// La barre d'états est un `BeginHudFrame`, qui pousse une marge NULLE, aucune
// bordure et aucun arrondi — ce que veut un cadre HUD collé à son art, mais
// l'infobulle en héritait : texte collé au bord du panneau, sans filet ni coins.
// Survolée depuis le chat, la MÊME infobulle était correcte parce que
// `BeginRoChatWindow` pousse, lui, une marge et un filet.
//
// La marge et l'arrondi passent par `ro::Px` : ce sont des longueurs d'écran, et
// une infobulle à police doublée garderait sinon une marge de 6 px.
constexpr float kTipPad    = 6.0f;   // marge intérieure, LES DEUX AXES
constexpr float kTipRound  = 3.0f;   // l'arrondi des fenêtres du toolkit
constexpr ImU32 kTipBorder = IM_COL32(0xC5, 0xC5, 0xC5, 255);  // filet RO

// 4 couleurs + 3 vars, à dépiler par `PopTipStyle` sur CHAQUE sortie.
static void PushTipStyle() {
  ImGui::PushStyleColor(ImGuiCol_PopupBg, kTipBg);
  ImGui::PushStyleColor(ImGuiCol_Text, kTipText);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, kTipDim);
  ImGui::PushStyleColor(ImGuiCol_Border, kTipBorder);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(ro::Px(kTipPad), ro::Px(kTipPad)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ro::Px(kTipRound));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
}

static void PopTipStyle() {
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(4);
}

void Tooltip(const StatusEffects::Entry& e) {
  PushTipStyle();

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
    PopTipStyle();
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
  PopTipStyle();
}

bool Draw(const StatusEffects::Entry& e, ImVec2 p0, ImVec2 p1,
          const Style& style, bool tooltip, bool* took_hover) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // L'opacité s'applique en multipliant l'alpha de CHAQUE couleur : c'est le
  // seul chemin qui atteigne des primitives (cf. `Style::alpha`).
  const float ca = (style.alpha < 0.0f) ? 0.0f
                 : (style.alpha > 1.0f) ? 1.0f : style.alpha;
  auto fade = [ca](ImU32 c) -> ImU32 {
    const unsigned a = (c >> IM_COL32_A_SHIFT) & 0xFFu;
    const unsigned na = static_cast<unsigned>(static_cast<float>(a) * ca + 0.5f);
    return (c & ~(0xFFu << IM_COL32_A_SHIFT)) | (na << IM_COL32_A_SHIFT);
  };
  const ImU32 tint =
      fade(style.dim ? IM_COL32(140, 140, 140, 220) : IM_COL32_WHITE);

  // Une ALTÉRATION n'a pas d'image : pastille. Tout le reste — grisage, compte à
  // rebours, infobulle — s'applique ensuite à l'identique, c'est bien pour ça
  // que ces deux rendus vivent dans la même fonction.
  if (const AilmentLook* look = LookupAilment(e.efst)) {
    const float rounding = (p1.x - p0.x) * 0.18f;
    dl->AddRectFilled(p0, p1, fade(look->color), rounding);
    dl->AddRect(p0, p1, fade(IM_COL32(0, 0, 0, 170)), rounding);
    ImFont* font = ImGui::GetFont();
    // La police occupe 55 % de la case : trois lettres y tiennent en largeur
    // sans qu'on ait à mesurer deux fois.
    const float fsz = (p1.y - p0.y) * 0.55f;
    const ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, look->abbrev);
    const ImVec2 tp(p0.x + ((p1.x - p0.x) - ts.x) * 0.5f,
                    p0.y + ((p1.y - p0.y) - ts.y) * 0.5f);
    dl->AddText(font, fsz, ImVec2(tp.x + 1.0f, tp.y + 1.0f),
                fade(IM_COL32(0, 0, 0, 190)), look->abbrev);
    dl->AddText(font, fsz, tp, fade(IM_COL32_WHITE), look->abbrev);
  } else {
    const char* path = StatusEffects::IconPath(e.efst);
    if (path == nullptr) return false;
    ro::GameTexture icon = ro::CachedTextureFromGameFile(path);

    // 🔴 LE LUA REND UN NOM NU. `stateiconimginfo.lub` associe
    // `EFST_BODYSTATE_SLEEP` à « BD_Sleep.tga » — sans dossier — alors que la
    // table EN DUR du client rend des chemins complets (« effect\\XXX.TGA »).
    // Le client s'en sort parce qu'il passe par son cache de SPRITES, qui
    // résout les noms nus ; notre chargeur, lui, parle au TexMgr et veut le
    // chemin. Sans ce repli, toutes les icônes venues du Lua échouaient en
    // silence — et c'est le cas de TOUTES les altérations.
    if (!icon.tex && strchr(path, '\\') == nullptr) {
      char full[192];
      std::snprintf(full, sizeof(full), "effect\\%s", path);
      icon = ro::CachedTextureFromGameFile(full);
    }
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
      RadialSweep(dl, p0, p1, spent, fade(style.sweep_color));
    } else {
      // Le voile DESCEND : la part sombre coiffe la case et sa frontière glisse
      // vers le bas, le clair qui subsiste étant ce qu'il reste de temps. Le
      // commentaire disait « monte », ce qui décrivait l'inverse du code juste
      // en dessous — et le libellé du réglage répétait l'erreur.
      dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + (p1.y - p0.y) * spent),
                        fade(style.sweep_color));
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
                fade(IM_COL32(0, 0, 0, 200)), txt);
    dl->AddText(font, style.time_px, tp, fade(IM_COL32_WHITE), txt);
  }

  // ⚠ `IsMouseHoveringRect` et non `IsItemHovered` : on n'a posé AUCUN item
  // ImGui, seulement des primitives de dessin. Il n'y a donc rien à interroger.
  const bool over = ImGui::IsMouseHoveringRect(p0, p1);
  if (took_hover != nullptr && over) *took_hover = true;
  if (tooltip && over) Tooltip(e);

  // ── MAJ + clic : poser le lien de cet état dans le chat ───────────────────
  //
  // Le geste vit ICI, donc les quatre surfaces qui affichent des états l'ont
  // d'un coup — la barre de mes états, celle de la cible, la grille de groupe et
  // la liste Groupe/Amis. Le poser chez chacune en aurait fait la cinquième
  // copie du même geste.
  //
  // ⚠ Réservé aux surfaces qui ont déjà le droit d'interagir (`tooltip`) : une
  // case posée en décoration ne doit pas capter de clic.
  //
  // 🔴 On IGNORE le retour de `Gestures` — le « il faut ouvrir un menu » du clic
  // DROIT. Sur une tuile de groupe ou une ligne de liste, ce bouton appartient
  // déjà au membre, et lui voler son menu pour une icône de deux pixels serait
  // une régression. Le clic gauche simple, lui, ne fait rien : un état n'a pas
  // de description à ouvrir, et c'est l'infobulle ci-dessus qui porte le lien.
  if (tooltip && over) {
    const links::Target t = links::FromStatus(e.efst);
    if (t.valid()) links::Gestures(t, true);
  }
  return true;
}

bool DrawSettings(const SettingsRefs& refs, int size_min_px, int size_max_px) {
  bool changed = false;

  // ⚠ PAS dans `changed` : l'aperçu ne se persiste pas, et le marquer
  // réécrirait le fichier de réglages à chaque clic.
  ro::RoCheckbox(i18n::Tr("Aperçu (faux statuts)"), refs.preview);
  mui::SameLine();
  mui::HelpMarker(i18n::Tr(
      "Remplit l'affichage de faux états, aux durées étagées, le "
      "temps de le régler — sans attendre d'en avoir de "
      "vrais.\n\nNe se garde pas d'une session à l'autre."));

  // 🔴 PushID : l'identifiant ImGui d'un widget est son LIBELLÉ, donc sa
  // TRADUCTION. « Taille des icônes » (états) et « Taille de l'icône » (classe)
  // sont distincts en français et deviennent tous deux « Icon size » en
  // anglais : deux widgets, un seul identifiant, et ImGui lève une erreur en
  // plein jeu.
  //
  // ⚠ Le code relu en français ne montre RIEN — c'est le catalogue qui crée la
  // collision, et il peut la recréer demain sur un autre couple. D'où une
  // isolation par BLOC, pas un libellé rebaptisé qui ne protégerait que ce
  // cas-ci.
  ImGui::PushID("status_icons");
  changed |= mui::WheelSliderInt(i18n::Tr("Taille des icônes"), refs.size_px,
                                 size_min_px, size_max_px, "%d px");
  changed |= mui::WheelSliderInt(i18n::Tr("Icônes au plus"), refs.max_icons,
                                 1, 24, "%d");
  changed |= mui::WheelSliderInt(i18n::Tr("Lignes d'icônes"), refs.rows,
                                 1, 4, "%d");
  mui::SameLine();
  mui::HelpMarker(i18n::Tr(
      "Une rangée unique s'allonge jusqu'à manger la place du nom ; "
      "en deux lignes, le même nombre d'états tient sur moitié moins "
      "de largeur.\n\n"
      "Le compte maximum se répartit entre les lignes — six icônes "
      "sur deux lignes font trois par ligne."));
  changed |= ro::RoCheckbox(i18n::Tr("Temps restant sous l'icône"),
                            refs.show_time);
  {
    // 🔴 Libellés NUS : `ro::RoCombo` traduit ses items lui-même, à la lecture.
    // L'ORDRE est celui de `Sweep`, et c'est un index persisté.
    const char* kSweeps[] = {"Aucun", "Balayage horaire", "Voile descendant"};
    static_assert(kSweepVertical == 2,
                  "un libellé par valeur de Sweep, dans le même ordre");
    changed |= ro::RoCombo(i18n::Tr("Grisage de la case"), refs.sweep, kSweeps,
                           IM_ARRAYSIZE(kSweeps));
  }
  mui::SameLine();
  mui::HelpMarker(i18n::Tr(
      "La case s'assombrit à mesure que l'état s'écoule.\n\n"
      "⚠ La durée d'origine n'est portée par AUCUN paquet : elle est "
      "exacte quand on a vu l'état commencer, et repart de « plein » "
      "quand on le découvre en route."));
  ImGui::PopID();
  return changed;
}

}  // namespace statuscell
