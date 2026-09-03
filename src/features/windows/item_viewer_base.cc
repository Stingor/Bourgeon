#include "features/windows/item_viewer_base.h"

#include <Windows.h>  // SEH autour du déréférencement de la fenêtre native

#include <algorithm>  // std::min / std::max (rabattre le spawn dans l'écran)
#include <cmath>      // std::fabs (recalage de la taille verrouillée)
#include <iterator>   // std::size (les emplacements de carte d'une ligne)

#include "bourgeon.h"        // IsMapLoading + les trois viewers, qui se répondent
#include "features/moonlight_ui/moonlight_ui.h"  // OpenInterfaceSection
#include "features/windows/cart_viewer.h"
#include "features/windows/inventory_viewer.h"
#include "features/windows/storage_window.h"
#include "features/windows/viewer_probes.h"  // VendingComposing
#include "imgui.h"
#include "ragnarok/item_info.h"   // rag::itemlist::kMaxOpts
#include "ragnarok/uiwnd.h"  // uiwnd::kOffVisible
#include "ui/item_grid_chrome.h"  // ro::grid::Snap : le snap par palier de case
#include "ui/qty_prompt.h"        // ro::QuantityPrompt (dialogue « combien ? »)
#include "ui/ro_imgui.h"     // ro::TitleBarButton
#include "utils/i18n.h"

void ItemViewerBase::HandleNativeToggle(void* win, uintptr_t expected_vtable) {
  if (!win || !imgui_enabled_) return;

  // ⚠ La vtable est une GARDE, pas une décoration : le hook MakeWindow voit
  // naître TOUTES les fenêtres du client, et écrire `+kOffVisible` sur celle
  // d'un autre id toucherait un champ qui n'a rien à voir.
  __try {
    if (*reinterpret_cast<uintptr_t*>(win) != expected_vtable) return;
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + uiwnd::kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

  // Reconstruction du HUD au changement de carte : ce n'est PAS le joueur qui
  // demande, on ne touche donc pas à l'état du viewer.
  if (Bourgeon::Instance().IsMapLoading()) return;

  if (open_) { open_ = false; return; }
  open_ = true;
  show_panel_ = true;
  need_pos_ = true;
}

// ── Les raccourcis d'une fenêtre vers ses deux sœurs ─────────────────────────

namespace {

// Le viewer que désigne `peer`, ou nul s'il n'existe pas encore (chargement des
// plugins, mode natif). Rendu par la base plutôt que par chaque appelant : c'est
// le seul endroit qui ait besoin de connaître les trois.
ItemViewerBase* PeerViewer(ItemViewerBase::Peer peer) {
  Bourgeon& bourgeon = Bourgeon::Instance();
  switch (peer) {
    case ItemViewerBase::Peer::kInventory: return bourgeon.inventory_viewer();
    case ItemViewerBase::Peer::kCart:      return bourgeon.cart_viewer();
    case ItemViewerBase::Peer::kStorage:   return bourgeon.storage_window();
  }
  return nullptr;
}

// 🔴 Les libellés ne sont PAS traduits ici : « Cart » et « Storage » sont des
// termes du JEU, et les fenêtres qu'ils désignent portent déjà ces noms-là.
// Seul « Inventaire » passe par le catalogue, comme le titre de sa fenêtre.
const char* PeerLabel(ItemViewerBase::Peer peer) {
  switch (peer) {
    case ItemViewerBase::Peer::kInventory: return i18n::Tr("Inventaire");
    case ItemViewerBase::Peer::kCart:      return "Cart";
    case ItemViewerBase::Peer::kStorage:   return "Storage";
  }
  return "";
}

// ── Les nombres du squelette de rendu ──────────────────────────────────────
// Plancher de la grille, en cases : en deçà la fenêtre ne montrerait plus rien
// d'utile. Sert à la fois de contrainte de resize et de garde au recalage.
constexpr int kMinGridCells = 5;
// Borne haute des contraintes de resize : « pas de maximum », en pixels.
constexpr float kMaxWindowPx = 10000.0f;
// En deçà, la taille verrouillée est déjà sur son palier : ne rien forcer.
constexpr float kSnapEpsilonPx = 0.5f;
// Le bandeau qui annonce qu'une composition d'échoppe gèle les transferts.
// ⚠ `ImVec4` n'a pas de constructeur `constexpr` : `inline const`, comme la
// palette (ui/ui_palette.h) le fait pour les siennes.
const ImVec4 kVendingWarnColor(0.85f, 0.15f, 0.15f, 1.0f);

}  // namespace

void ItemViewerBase::DrawPeerButtons(Peer self) {
  if (!peer_buttons_ || !imgui_enabled_) return;

  // L'ordre de POSE est l'inverse de l'ordre AFFICHÉ : chaque bouton devient la
  // butée du suivant, qui se place à sa gauche.
  Peer order[2] = {Peer::kCart, Peer::kStorage};  // affiché : [Storage][Cart]
  switch (self) {
    case Peer::kInventory: order[0] = Peer::kCart;      order[1] = Peer::kStorage;   break;
    case Peer::kStorage:   order[0] = Peer::kCart;      order[1] = Peer::kInventory; break;
    case Peer::kCart:      order[0] = Peer::kInventory; order[1] = Peer::kStorage;   break;
  }

  for (const Peer peer : order) {
    ItemViewerBase* target = PeerViewer(peer);
    // ⚠ Un viewer resté au natif ne se pilote pas : son `open_` ne veut alors
    // plus rien dire, c'est la fenêtre du client qui porte l'état. Le bouton
    // disparaît plutôt que de mentir. (En pratique les trois basculent en
    // groupe — cf. SetModernInterface — mais rien ne l'impose ici.)
    if (target == nullptr || !target->imgui_enabled_) continue;

    const char* blocked = target->PeerBlockedReason();
    if (blocked != nullptr) ImGui::BeginDisabled();
    const char* label = PeerLabel(peer);
    // L'infobulle dit ce que le clic VA faire — ouvrir ou refermer — parce que
    // le bouton, lui, ne le montre pas : son libellé est le nom de la fenêtre.
    const char* tip = blocked           ? blocked
                      : target->IsOpen() ? i18n::Tr("Refermer cette fenêtre")
                                         : i18n::Tr("Ouvrir cette fenêtre");
    if (ro::TitleBarButton(label, tip)) {
      if (target->IsOpen()) target->PeerClose();
      else                  target->PeerOpen();
    }
    if (blocked != nullptr) ImGui::EndDisabled();
  }
}

// ── Le squelette de rendu ────────────────────────────────────────────────────

bool ItemViewerBase::ShouldRender() {
  if (open_ && imgui_enabled_) return true;
  win_rect_.Invalidate();
  return false;
}

bool ItemViewerBase::BeginViewerWindow(const WindowChrome& chrome) {
  if (need_pos_) {
    // FirstUseEver : simple DÉFAUT de première ouverture ; ensuite ImGui garde
    // la position déplacée par le joueur. Ce défaut se lisait sur la fenêtre
    // native, qui ne naît plus — on le fixe, rabattu dans l'écran sur petite
    // résolution.
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(
        ImVec2(std::min(chrome.spawn_x, std::max(0.0f, screen.x - chrome.spawn_w)),
               std::min(chrome.spawn_y, std::max(0.0f, screen.y - chrome.spawn_h))),
        ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(chrome.spawn_w, chrome.spawn_h),
                           ImGuiCond_FirstUseEver);
  // Resize par PALIER de case (chrome mesuré la frame précédente) : la largeur
  // et la hauteur sautent d'une colonne ou d'une ligne entière -> jamais de
  // case partielle. Inutile quand la taille est verrouillée.
  ro::grid::SnapState& snap = ro::grid::Snap();
  if (snap.valid && !chrome.lock_size) {
    const float minGrid = kMinGridCells * (snap.cell + snap.gap) - snap.gap;
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(snap.chromew + minGrid, snap.chromeh + minGrid),
        ImVec2(kMaxWindowPx, kMaxWindowPx), ro::grid::SnapWindowSize);
  } else if (snap.valid && win_rect_.valid()) {
    // Taille VERROUILLÉE : le callback de snap ne tourne plus — il n'agit que
    // PENDANT un redimensionnement — donc la fenêtre reste figée sur la hauteur
    // qu'elle avait, presque jamais un multiple exact de cases : d'où une
    // dernière ligne coupée, objets ou pas. On la recale UNE fois sur le palier
    // le plus proche ; la frame suivante la taille correspond déjà et plus rien
    // n'est forcé.
    const float step = snap.cell + snap.gap;
    int cols = static_cast<int>(
        (win_rect_.w() - snap.chromew + snap.gap) / step + 0.5f);
    int rows = static_cast<int>(
        (win_rect_.h() - snap.chromeh + snap.gap) / step + 0.5f);
    if (cols < kMinGridCells) cols = kMinGridCells;
    if (rows < kMinGridCells) rows = kMinGridCells;
    const ImVec2 snapped(snap.chromew + cols * step - snap.gap,
                         snap.chromeh + rows * step - snap.gap);
    if (std::fabs(snapped.x - win_rect_.w()) > kSnapEpsilonPx ||
        std::fabs(snapped.y - win_rect_.h()) > kSnapEpsilonPx)
      ImGui::SetNextWindowSize(snapped, ImGuiCond_Always);
  }

  // Puce de la barre de titre = raccourci vers la config de CETTE fenêtre.
  ro::SetNextWindowTitleBullet(chrome.bullet_tip);
  ro::SetNextWindowPinnable();  // épingle : Échap ne referme plus la fenêtre
  // Pas de NoCollapse -> le skin RO affiche le bouton minimiser (repli de la
  // barre de titre), comme le natif.
  const bool begun = ro::BeginRoWindow(
      chrome.title, &show_panel_,
      chrome.lock_size ? ImGuiWindowFlags_NoResize : 0);
  // À appeler que `Begin` ait rendu true ou non : la barre de titre existe même
  // fenêtre repliée, et la demande est consommée par `BeginRoWindow`.
  if (ro::TitleBulletClicked())
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(chrome.iface_section);
  // X du viewer : l'état d'ouverture est le NÔTRE maintenant, il n'y a plus de
  // fenêtre native à fermer. Réarme `show_panel_` pour la prochaine ouverture.
  if (!show_panel_) { open_ = false; show_panel_ = true; }
  // Repliée ou clippée : ImGui ne l'a pas dessinée, son rect déplié ne vaut plus
  // rien comme cible de dépôt.
  if (!begun) { win_rect_.Invalidate(); ro::EndRoWindow(); return false; }

  // Pendant la composition d'une échoppe, le serveur refuse TOUT mouvement
  // touchant le chariot : autant l'annoncer une fois en clair, en plus des
  // entrées grisées du menu contextuel. C'est dans l'inventaire que ce bandeau
  // compte le plus — ses entrées « Vers le cart » / « Vers le storage »
  // n'existent que si la fenêtre correspondante est ouverte, donc sans lui il
  // n'avertissait de rien du tout.
  if (viewers::VendingComposing())
    ImGui::TextColored(kVendingWarnColor, "%s",
                       i18n::Tr("Shop en composition : les transferts sont figés."));

  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  win_rect_.Capture(wp.x, wp.y, ws.x, ws.y);
  return true;
}

void ItemViewerBase::EndViewerWindow(Peer self, const itemcell::ItemRow* hovered) {
  // Les raccourcis vers les deux autres viewers (opt-in).
  // 🔴 EN DERNIER dans la fenêtre : ils vivent dans la barre de titre et ne
  // restaurent pas le curseur de layout (cf. ro::TitleBarButton).
  DrawPeerButtons(self);
  ro::EndRoWindow();

  // Aperçu de description : dessiné APRÈS la fenêtre — c'est un TOOLTIP, il doit
  // passer AU-DESSUS d'elle — et donc hors de tout Begin/End.
  //
  // Pas de test sur `show_desc_tooltip_` : `hover_desc_id_` n'est armé que sous
  // ce réglage. Et il est posé EN MÊME TEMPS que l'index de la ligne, donc un id
  // non nul avec un `hovered` nul n'arrive pas dans une même frame — on ne
  // dessine alors rien, plutôt qu'un aperçu sans nom ni cartes.
  if (hover_desc_id_ == 0 || hovered == nullptr) return;
  itemdesc::SimpleOpt sopts[rag::itemlist::kMaxOpts];
  // `ExtractList` borne déjà `opt_count` à `kMaxOpts` ; la borne est reprise ici
  // parce que c'est CE tableau-là qu'elle protège.
  int nopts = hovered->opt_count;
  if (nopts > rag::itemlist::kMaxOpts) nopts = rag::itemlist::kMaxOpts;
  for (int i = 0; i < nopts; ++i) {
    sopts[i].index = hovered->opts[i].index;
    sopts[i].value = hovered->opts[i].value;
    sopts[i].param = hovered->opts[i].param;
  }
  itemcell::DrawTooltip(hover_desc_id_, hovered->cards,
                        static_cast<int>(std::size(hovered->cards)), sopts, nopts,
                        hovered->refine, hovered->name, hovered->damaged != 0);
}

int ItemViewerBase::TakePendingAmount() {
  if (pend_id_ == 0) return 0;
  if (pend_open_prompt_) {
    ro::OpenQuantityPrompt(this);
    pend_open_prompt_ = false;
    return 0;
  }
  if (pend_max_ > 1) return 0;  // le dialogue est ouvert : c'est lui qui tranche
  pend_id_ = 0;
  return 1;
}

int ItemViewerBase::PumpQuantityPrompt(const char* verb) {
  bool cancelled = false;
  const int qty = ro::QuantityPrompt(this, verb, pend_max_, &cancelled);
  if (qty > 0) { pend_id_ = 0; return qty; }
  if (cancelled) pend_id_ = 0;
  return 0;
}

bool ItemViewerBase::DragReleased(const char* payload_type) {
  if (!drag_active_) return false;
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (pl != nullptr && pl->IsDataType(payload_type)) {
    // Le glisser court encore : on suit la souris.
    const ImVec2 m = ImGui::GetMousePos();
    drag_mx_ = m.x;
    drag_my_ = m.y;
    return false;
  }
  drag_active_ = false;  // relâché ce frame
  return true;
}

void ItemViewerBase::ArmDraggedAction(int action) {
  pend_id_ = drag_index_;
  pend_index_ = drag_index_;
  pend_max_ = drag_amount_ > 0 ? drag_amount_ : 1;
  pend_action_ = action;
  pend_open_prompt_ = (pend_max_ > 1);
}
