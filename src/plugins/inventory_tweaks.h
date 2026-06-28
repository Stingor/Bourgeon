#pragma once

#include "plugins/plugin.h"

// Inventory window (UIItemWnd, window id 8) tweaks for the 20250716 client:
//   1. a current/max weight readout drawn bottom-RIGHT — the cart's scale icon
//      followed by "cur / max (NN%)" (red when overweight), beside the stock
//      item count "X/200" which the original keeps drawing bottom-left.
//   2. raises the user-resize max clamp so the window can grow large.
// Both are one-time patches in the constructor (DrawContent vtable swap +
// two resize-clamp immediates).  Compiled into the DLL; register in
// Bourgeon::LoadPlugins().
class InventoryTweaks : public Plugin {
 public:
  InventoryTweaks();

  const char* name() const override { return "Inventory"; }

  // Appends the extra "Cards" tab once per inventory instance, in the LOGIC phase
  // (not during render) so the tab-control node recreate can't corrupt frame state.
  void OnTick() override;
};
