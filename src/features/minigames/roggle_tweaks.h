#pragma once

#include "features/plugin.h"

// Peggle-like mini-game rendered entirely with ImGui's ImDrawList — pure shapes,
// no RO assets yet (art pass = iteration 2), no vendored engine, no server.
// A ball is launched from a top cannon, bounces off pegs (orange = must clear,
// blue = bonus) under gravity, and a moving bucket at the bottom grants a free
// ball when it catches one. The 2D physics is ticked from OnRenderUI on a fixed
// timestep (framerate-independent). Toggle from the Moonlight menu.
//
// Mouse over the window is already captured by ImGui (the WndProc gate blocks
// the game's click-to-move while the cursor is over an ImGui window), so playing
// never moves your RO character. DX9 or DX7 — it's just ImGui drawing.
class RoggleTweaks : public Plugin {
 public:
  const char* name() const override { return "Roggle"; }

  void OnRenderUI() override;

  // A mode/map switch can reset or recreate the D3D device, which would leave
  // our cached D3DPOOL_DEFAULT icon textures dangling (drawing one then crashes
  // the DX9 backend). We drop them here so they reload fresh on the new device.
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  bool enabled() const { return enabled_; }
  void SetEnabled(bool on) { enabled_ = on; }

 private:
  bool enabled_ = false;
};
