#include "features/windows/viewer_probes.h"

#include "bourgeon.h"
#include "features/windows/cart_viewer.h"
#include "features/windows/inventory_viewer.h"
#include "features/windows/storage_window.h"
#include "features/windows/vending_window.h"

namespace viewers {
namespace {

// Le hit-test est le même pour les trois : la visionneuse existe, et le point
// tombe dedans. `PointOverViewer` prend des entiers — la conversion est faite
// ici, une fois, plutôt qu'à chacun des six anciens sites.
template <typename Viewer>
bool Over(Viewer* viewer, float x, float y) {
  return viewer && viewer->PointOverViewer(static_cast<int>(x),
                                           static_cast<int>(y));
}

}  // namespace

bool InventoryOpen() {
  auto* inventory = Bourgeon::Instance().inventory_viewer();
  return inventory && inventory->IsOpen();
}

bool CartOpen() {
  auto* cart = Bourgeon::Instance().cart_viewer();
  return cart && cart->IsOpen();
}

bool StorageOpen() {
  auto* storage = Bourgeon::Instance().storage_window();
  return storage && storage->IsOpen();
}

bool VendingComposing() {
  auto* vending = Bourgeon::Instance().vending_window();
  return vending && vending->IsComposing();
}

bool MouseOverInventory(float x, float y) {
  return Over(Bourgeon::Instance().inventory_viewer(), x, y);
}

bool MouseOverCart(float x, float y) {
  return Over(Bourgeon::Instance().cart_viewer(), x, y);
}

bool MouseOverStorage(float x, float y) {
  return Over(Bourgeon::Instance().storage_window(), x, y);
}

}  // namespace viewers
