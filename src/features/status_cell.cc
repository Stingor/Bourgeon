#include "features/status_cell.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "ragnarok/lua.h"
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

// « 12 », « 1:05 », « 12:30 ». Une durée se lit d'un coup d'œil ou ne sert à
// rien : sous la minute on donne les secondes seules, au-delà on passe en
// minutes plutôt que d'afficher « 305 ».
void FormatRemain(uint32_t ms, char* out, size_t cap) {
  const unsigned total_s = ms / 1000u;
  if (total_s < 60u) {
    std::snprintf(out, cap, "%u", total_s);
    return;
  }
  std::snprintf(out, cap, "%u:%02u", total_s / 60u, total_s % 60u);
}

// Le texte du client pour cet état, ligne par ligne, mémorisé.
//
// 🔴 Mémorisé PARCE QUE c'est du Lua : `GetStateIconDescript` traverse
// l'interpréteur à chaque appel, et une barre pleine d'icônes le rappellerait
// pour chacune, à chaque frame. Le résultat, lui, ne change jamais.
struct Text {
  std::string name;
  std::string desc;  // les lignes suivantes, recollées
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
    } else {
      if (!t.desc.empty()) t.desc += '\n';
      t.desc += buf;
    }
  }
  return cache.emplace(efst, std::move(t)).first->second;
}

}  // namespace

const char* Name(uint16_t efst) { return Lookup(efst).name.c_str(); }

void Tooltip(const StatusEffects::Entry& e) {
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

  if (e.expires_ms != 0) {
    const int32_t left_ms = static_cast<int32_t>(e.expires_ms - ::timeGetTime());
    char txt[16];
    FormatRemain(left_ms > 0 ? static_cast<uint32_t>(left_ms) : 0u, txt,
                 sizeof(txt));
    ImGui::TextDisabled("%s %s", i18n::Tr("Reste"), txt);
  } else {
    // ⚠ Pas « 0 » : un état sans échéance ne finit pas, et un zéro le ferait
    // croire terminé.
    ImGui::TextDisabled("%s", i18n::Tr("Sans durée"));
  }

  if (!t.desc.empty()) {
    ImGui::Separator();
    ImGui::TextUnformatted(ro::LocalToUtf8(t.desc.c_str()));
  }
  ImGui::EndTooltip();
}

bool Draw(const StatusEffects::Entry& e, ImVec2 p0, ImVec2 p1,
          const Style& style, bool tooltip) {
  const char* path = StatusEffects::IconPath(e.efst);
  if (path == nullptr) return false;
  const ro::GameTexture icon = ro::CachedTextureFromGameFile(path);
  if (!icon.tex) return false;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 tint =
      style.dim ? IM_COL32(140, 140, 140, 220) : IM_COL32_WHITE;
  dl->AddImage(reinterpret_cast<ImTextureID>(icon.tex), p0, p1, ImVec2(0, 0),
               ImVec2(1, 1), tint);

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
      // Le voile MONTE : il occupe le haut, et le clair qui reste en bas est ce
      // qu'il reste de temps. Descendre aurait fait grandir la part sombre par
      // le bas, où l'œil cherche le niveau d'un réservoir.
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
