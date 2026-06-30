#pragma once

#include <climits>

#include "plugins/plugin.h"
#include "d3d9/d3d9_hook.h"  // D3D9PostFx

// Client "Game Settings"-style tweaks the native options window can't do.
//
// Real-time post-processing of the engine's rendered frame via the DX9 hook
// (see d3d9/d3d9_hook.cc): colour grade (brightness/contrast/gamma/saturation/
// temperature), filters (B&W / sepia / negative / colourblind), vignette, film
// grain, chromatic aberration, sharpen, and FXAA. Plus a clean PNG screenshot
// (no overlay) and an FPS/frametime overlay. Config persists through MoonlightUi's
// shared bourgeon_settings.yaml (same pattern as StatusIconTweaks). DX9 only.
class SettingsTweaks : public Plugin {
 public:
  SettingsTweaks();

  const char* name() const override { return "SettingsTweaks"; }

  // Persisted config access (used by moonlight_ui's settings save/load).
  D3D9PostFx& fx() { return fx_; }
  bool& fps_overlay() { return fps_overlay_; }
  bool& zoom_enabled() { return zoom_enabled_; }
  float& zoom_factor() { return zoom_factor_; }
  float& zoom_speed() { return zoom_speed_; }
  int& tex_filter() { return tex_filter_; }
  int& gopt_x() { return gopt_x_; }
  int& gopt_y() { return gopt_y_; }
  int& esc_x() { return esc_x_; }
  int& esc_y() { return esc_y_; }

  // Pushes the current post-fx config to the d3d9 layer. Safe before the device
  // exists (values are just stored).
  void Apply();

  // Renders the graphics-tweak controls (no window of its own); hosted by
  // moonlight_ui inside its "Graphismes" section.
  void DrawSettings();

  // Always-on FPS / frametime overlay (its own small window).
  void OnRenderUI() override;

  // Maintains the extended camera zoom-out each tick (patches the engine's max
  // view-distance clamp globals). 20250716 only.
  void OnTick() override;

 private:
  D3D9PostFx fx_;
  int        tex_filter_ = 0;       // 0=default, 1=point (crisp), 2=linear (smooth)
  bool       fps_overlay_ = false;
  float      fps_hist_[120] = {};  // frametime ring buffer (ms)
  int        fps_head_ = 0;

  // ── Camera zoom-out extension ────────────────────────────────────────────
  // Camera_ApplyViewDistanceClamp (0x00c82340) clamps the zoom to the engine's
  // max view-distance globals g_cam_zoomMaxOutdoor (0x012291c0) / Indoor
  // (0x012291c4). We capture their post-OptionInfo-load defaults once, then each
  // tick set them to default * zoom_factor_ to allow zooming out further.
  bool  zoom_enabled_      = false;
  float zoom_factor_       = 1.0f;   // 1.0 = stock, up to ~2.5 (max view distance)
  float zoom_speed_        = 1.0f;   // wheel step multiplier 1..4 (responsiveness)
  bool  zoom_base_ok_      = false;  // max-clamp base captured?
  float zoom_base_out_     = 0.0f;
  float zoom_base_in_      = 0.0f;
  // Wheel step constants DAT_01091520 / 01091528 (.rdata, read by the wheel-zoom
  // handler FUN_00c7d4f0); scaled by zoom_speed_ so each notch moves further.
  bool  zoom_step_ok_      = false;
  float zoom_step_base1_   = 0.0f;
  float zoom_step_base2_   = 0.0f;

  // ── Game Settings window (ESC > Game Option, id 0x271e) position persistence ─
  // The engine never saves this window's position. We restore it on open and
  // persist it on move/close via OnTick (FindWindow live read), like StatusTweaks.
  int           gopt_x_ = INT_MIN;   // saved position (INT_MIN = none)
  int           gopt_y_ = INT_MIN;
  bool          gopt_was_open_ = false;
  unsigned long gopt_last_save_ = 0;  // GetTickCount of last save (throttle)

  // ── ESC "Game Options" window (UIEscOptionWnd) position persistence ──────────
  // The ESC pop-up menu (Character Select / game settings / Shortcut Config /
  // Exit to Windows / Return to game). A DIFFERENT native window from the Game
  // Settings one above. It has no FindWindow id in the switch; its live pointer
  // sits in the window manager's dedicated slot g_UIWindowMgr(0x0131f4e8)+0x408
  // (null when the menu is closed). vtable 0x010384a0; standard UIWindow base
  // (pos win+0x1c/0x20, SetPos vtable+0x10 = 0x00874af0, same as Game Settings).
  int           esc_x_ = INT_MIN;
  int           esc_y_ = INT_MIN;
  bool          esc_was_open_ = false;
  unsigned long esc_last_save_ = 0;
};
