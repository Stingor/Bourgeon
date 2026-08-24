#include "features/windows/item_viewer_base.h"

#include <Windows.h>  // SEH autour du déréférencement de la fenêtre native

#include "bourgeon.h"        // IsMapLoading
#include "ragnarok/uiwnd.h"  // uiwnd::kOffVisible

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
