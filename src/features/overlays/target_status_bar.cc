#include "features/overlays/target_status_bar.h"

#include <windows.h>

#include <algorithm>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/overlays/target_frame.h"  // la cible courante
#include "features/status_cell.h"  // le rendu d'UNE case d'état
#include "imgui.h"
#include "ragnarok/social.h"  // OwnAid : ne jamais se montrer soi-même
#include "ui/ro_imgui.h"

namespace {

ImU32 Col(const float rgba[4]) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
}


}  // namespace

// Ce qu'il y a à peindre, trié et tronqué.
void TargetStatusBar::Collect(std::vector<StatusEffects::Entry>* out) const {
  out->clear();
  auto* fx = Bourgeon::Instance().status_effects();
  if (fx == nullptr) return;

  auto* tf = Bourgeon::Instance().target_frame();
  // La cible AFFICHÉE par le HUD, pas la dernière cliquée : un sort lancé
  // directement sur un monstre le cible sans passer par `CGameMode+0xF4`.
  const uint32_t gid =
      (tf != nullptr) ? tf->ActiveTargetGid() : TargetFrame::CurrentSelectionGid();
  if (gid == 0) return;
  // 🔴 JAMAIS SOI-MÊME. Se cibler — depuis une tuile de la grille, par exemple —
  // faisait apparaître ici ses PROPRES états, que la barre d'icônes du client
  // montre déjà en permanence. Deux affichages de la même chose, dont l'un
  // surgit et disparaît au gré des clics.
  //
  // Le sondage saute déjà mon GID pour la même raison ; ce test-ci couvre
  // l'affichage, que la diffusion AREA alimente de son côté.
  if (gid == rag::social::OwnAid()) return;
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

  // 🔴 SUIT LE CIBLAGE, comme la barre de vie de la cible. Les états d'une
  // entité sont une information sur LA CIBLE : ils n'ont pas à survivre au mode
  // qui décide qu'une cible existe. Un joueur qui éteint le ciblage éteint tout
  // ce qui en parle, sans avoir à décocher trois réglages.
  auto* tf = Bourgeon::Instance().target_frame();
  if (tf == nullptr || !tf->TargetingEnabled()) return;

  // Le registre ne sonde que si quelqu'un affiche : demande VIVANTE, réarmée à
  // chaque frame. Elle part AVANT le test du contenu — sans elle, une barre
  // vide ne demanderait jamais rien et resterait vide pour toujours.
  //
  // ⚠ `RequestTargetPolling` et non `RequestPolling` : ce qui nous intéresse est
  // la CIBLE, pas le groupe. Les confondre faisait interroger 24 membres pour
  // une barre qui n'en montre aucun.
  if (auto* fx = Bourgeon::Instance().status_effects())
    fx->RequestTargetPolling();

  std::vector<StatusEffects::Entry> list;
  Collect(&list);

  // Le curseur est-il sur l'emplacement de la barre ? Le rectangle est connu
  // même quand rien n'est dessiné : c'est ce qui permet de retrouver une barre
  // vide pour la déplacer, sans la faire surgir partout ailleurs.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool over =
      mouse.x >= static_cast<float>(rect_.x) &&
      mouse.y >= static_cast<float>(rect_.y) &&
      mouse.x <  static_cast<float>(rect_.x + rect_.w) &&
      mouse.y <  static_cast<float>(rect_.y + rect_.h);

  // 🔴 MAJ SEULE NE SUFFIT PAS À LA FAIRE APPARAÎTRE. En RO, MAJ+clic est
  // l'attaque forcée : une barre vide qui surgissait à chaque appui sur MAJ
  // clignotait à l'écran pendant tout un combat. Il faut en plus que le curseur
  // soit sur SON emplacement — ou qu'un déplacement soit déjà en cours, sinon
  // tirer le cadre vers l'extérieur le ferait disparaître en pleine saisie.
  //
  // Déverrouillée pour de bon (`!locked_`), elle reste visible : on est alors
  // explicitement en train de placer ses cadres, et les chercher un par un avec
  // le curseur n'aurait aucun sens.
  const bool grabbing =
      ImGui::GetIO().KeyShift && (over || ro::HudFrameDragging());
  const bool placing = !locked_ || grabbing;

  // Rien à montrer, rien à l'écran — et surtout pas un cadre vide qu'on
  // prendrait pour une fonction en panne.
  if (list.empty() && !placing) return;

  ro::HudFrameOpts opts;
  opts.locked   = locked_ && !grabbing;
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

    const float fsz = ro::Px(static_cast<float>(std::max(7, time_px_)));

    // Deux causes de retour à la ligne, et il faut les deux : la largeur du
    // cadre (on ne déborde jamais), et le nombre de lignes demandé (on peut
    // vouloir une barre haute et étroite alors que la place ne l'impose pas).
    const int rows = std::max(1, rows_);
    const int per_row = (std::max(1, max_icons_) + rows - 1) / rows;

    float x = left;
    float y = win.y + pad;
    int drawn = 0;
    for (const StatusEffects::Entry& e : list) {
      const bool row_full = (drawn > 0 && drawn % per_row == 0);
      if (row_full || (x + side > right && x > left)) {
        x = left;
        y += side + gap + (show_time_ ? fsz : 0.0f);
      }
      statuscell::Style st;
      st.sweep       = sweep_;
      st.sweep_color = Col(col_sweep_);
      st.time_px     = show_time_ ? fsz : 0.0f;
      // Une case qui ne se dessine pas ne prend pas de place : sans ce test, un
      // état sans icône laissait un trou dans la rangée.
      if (!statuscell::Draw(e, ImVec2(x, y), ImVec2(x + side, y + side), st,
                            true))
        continue;
      x += side + gap;
      ++drawn;
    }
  }
  ro::EndHudFrame();

  if (geometry_dirty_) {
    geometry_dirty_ = false;
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}
