#include "features/windows/item_viewer_base.h"

#include <Windows.h>  // SEH autour du déréférencement de la fenêtre native

#include "bourgeon.h"        // IsMapLoading + les trois viewers, qui se répondent
#include "features/windows/cart_viewer.h"
#include "features/windows/inventory_viewer.h"
#include "features/windows/storage_window.h"
#include "imgui.h"
#include "ragnarok/uiwnd.h"  // uiwnd::kOffVisible
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
