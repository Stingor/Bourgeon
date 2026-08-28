#include "features/overlays/target_status_bar.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/overlays/target_frame.h"  // la cible courante
#include "imgui.h"
#include "ui/game_texture.h"
#include "ui/ro_imgui.h"

namespace {

ImU32 Col(const float rgba[4]) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
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

}  // namespace

// Ce qu'il y a à peindre, trié et tronqué.
void TargetStatusBar::Collect(std::vector<StatusEffects::Entry>* out) const {
  out->clear();
  auto* fx = Bourgeon::Instance().status_effects();
  if (fx == nullptr) return;

  const uint32_t gid = TargetFrame::CurrentSelectionGid();
  if (gid == 0) return;
  // Le registre applique déjà sa règle d'accès : un adversaire PVP n'y est
  // jamais entré, donc il n'y a rien à filtrer de plus ici.
  if (!fx->Effects(gid, out)) return;

  const uint32_t now = ::timeGetTime();
  if (sort_ != kSortArrival) {
    const bool shortest = (sort_ == kSortShortest);
    std::stable_sort(
        out->begin(), out->end(),
        [now, shortest](const StatusEffects::Entry& a,
                        const StatusEffects::Entry& b) {
          // ⚠ Un état SANS échéance n'a pas de durée à comparer. Il va en FIN
          // dans les DEUX sens : le traiter comme « très long » l'aurait mis en
          // tête du tri décroissant, chassant de l'écran ce qui presse.
          const bool a_inf = (a.expires_ms == 0);
          const bool b_inf = (b.expires_ms == 0);
          if (a_inf != b_inf) return b_inf;
          if (a_inf) return false;  // deux permanents : l'ordre d'arrivée tient
          const uint32_t la = a.expires_ms - now;
          const uint32_t lb = b.expires_ms - now;
          return shortest ? (la < lb) : (la > lb);
        });
  }

  const size_t cap = static_cast<size_t>(std::max(1, max_icons_));
  if (out->size() > cap) out->resize(cap);
}

void TargetStatusBar::OnRenderUI() {
  if (!enabled_) return;
  if (Bourgeon::Instance().IsMapLoading()) return;

  // Le registre ne sonde que si quelqu'un affiche : demande VIVANTE, réarmée à
  // chaque frame. Elle part AVANT le test du contenu — sans elle, une barre
  // vide ne demanderait jamais rien et resterait vide pour toujours.
  if (auto* fx = Bourgeon::Instance().status_effects()) fx->RequestPolling();

  std::vector<StatusEffects::Entry> list;
  Collect(&list);

  // 🔴 Rien à montrer, rien à l'écran. Sauf quand on la déverrouille : il faut
  // bien pouvoir la placer, et un cadre invisible ne se saisit pas.
  const bool placing = ImGui::GetIO().KeyShift || !locked_;
  if (list.empty() && !placing) return;

  ro::HudFrameOpts opts;
  // Même geste que les autres cadres : MAJ libère, mais seulement quand le
  // curseur est dessus — en RO, MAJ+clic est l'attaque forcée, et une barre qui
  // reprendrait la souris partout rendrait ce geste muet.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool over =
      mouse.x >= static_cast<float>(rect_.x) &&
      mouse.y >= static_cast<float>(rect_.y) &&
      mouse.x <  static_cast<float>(rect_.x + rect_.w) &&
      mouse.y <  static_cast<float>(rect_.y + rect_.h);
  const bool unlock_override =
      ImGui::GetIO().KeyShift && (over || ro::HudFrameDragging());
  opts.locked   = locked_ && !unlock_override;
  opts.border   = border_;
  opts.rounding = ro::Px(3.0f);
  opts.bg       = col_bg_;
  if (auto* mui = Bourgeon::Instance().moonlight_ui()) opts.grid = &mui->grid_;
  opts.min_w    = ro::Px(24.0f);
  opts.min_h    = ro::Px(16.0f);

  if (ro::BeginHudFrame("##target_status_bar", &rect_, opts,
                        &geometry_dirty_)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad  = ro::Px(3.0f);
    const float side = ro::Px(static_cast<float>(std::max(8, icon_px_)));
    const float gap  = ro::Px(static_cast<float>(std::max(0, gap_px_)));
    // 🔴 L'origine vient de la FENÊTRE, pas du curseur ImGui. Déverrouillé,
    // `BeginHudFrame` pose un `InvisibleButton` sur toute sa surface qui laisse
    // le curseur EN DESSOUS : des icônes posées à `GetCursorScreenPos()`
    // tombaient hors du clip et disparaissaient dès qu'on tenait MAJ.
    const ImVec2 win = ImGui::GetWindowPos();
    const float left = win.x + pad;
    const float right = win.x + static_cast<float>(rect_.w) - pad;

    const uint32_t now = ::timeGetTime();
    ImFont* font = ImGui::GetFont();
    const float fsz = ro::Px(static_cast<float>(std::max(7, time_px_)));

    float x = left;
    float y = win.y + pad;
    for (const StatusEffects::Entry& e : list) {
      // Repli à la ligne suivante quand la largeur est atteinte : la barre se
      // règle en largeur, c'est donc elle qui décide du nombre par rangée.
      if (x + side > right && x > left) {
        x = left;
        y += side + gap + (show_time_ ? fsz : 0.0f);
      }
      const char* path = StatusEffects::IconPath(e.efst);
      if (path == nullptr) continue;
      const ro::GameTexture icon = ro::CachedTextureFromGameFile(path);
      if (!icon.tex) continue;

      dl->AddImage(reinterpret_cast<ImTextureID>(icon.tex), ImVec2(x, y),
                   ImVec2(x + side, y + side));

      // Le compte à rebours, sous l'icône. Un état SANS échéance n'en porte
      // pas : « 0 » sous un buff permanent le ferait croire fini.
      if (show_time_ && e.expires_ms != 0) {
        const int32_t left_ms = static_cast<int32_t>(e.expires_ms - now);
        if (left_ms > 0) {
          char txt[16];
          FormatRemain(static_cast<uint32_t>(left_ms), txt, sizeof(txt));
          const ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, txt);
          const ImVec2 tp(x + (side - ts.x) * 0.5f, y + side);
          // Ombre portée : ces chiffres se lisent sur n'importe quel décor.
          dl->AddText(font, fsz, ImVec2(tp.x + 1.0f, tp.y + 1.0f),
                      IM_COL32(0, 0, 0, 200), txt);
          dl->AddText(font, fsz, tp, IM_COL32_WHITE, txt);
        }
      }
      x += side + gap;
    }
  }
  ro::EndHudFrame();

  if (geometry_dirty_) {
    geometry_dirty_ = false;
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}
