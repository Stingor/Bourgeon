#pragma once

#include "plugins/plugin.h"

// Client-side tweaks for the "Basic Info" character window (UIBasicInfoWnd) and
// its surrounding HUD.  Renders standalone, freely movable & resizable bars for
// Base EXP, Job EXP, HP and SP as an ImGui overlay (the client already feeds
// mouse input to ImGui, so the windows are draggable/resizable out of the box);
// more Basic-Info customizations will be added here over time.
//
// Each bar is an INDEPENDENT widget with its own position/size/colour/show flag.
// A global "lock" flips the windows to NoMove|NoResize|NoInputs, which freezes
// them AND makes them click-through (locked bars never set io.WantCaptureMouse,
// so clicks fall through to the game).  "Sticky" is a magnetic snap: while you
// drag a bar, its edges snap-align to any other shown bar (pull away to detach).
//
// Values are read live from the session globals the native UIBasicInfoWnd gauges
// use: Base EXP cur/max @0x015fb9d0/0x015fb9d8 and Job EXP cur/max
// @0x015fb9e8/0x015fb9e0 (INT64); HP cur/max @0x015ff908/0x015ff90c and SP
// cur/max @0x015ff910/0x015ff914 (INT32, like the client's own BasicInfo draw).
//
// This plugin owns all the settings state; the settings UI + persistence live
// in MoonlightUi's "EXP Bar" section, which reaches in via Bourgeon::basic_info().
class BasicInfoTweaks : public Plugin {
 public:
  const char* name() const override { return "Basic Info"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRenderUI() override;

  // ── Bars (read/written by MoonlightUi) ────────────────────────────────────
  struct Bar {
    bool  show;
    int   x, y, w, h;
    float fill[4];  // ImGui RGBA picker state
  };

  enum BarId { kBaseExp = 0, kJobExp, kHp, kSp, kBarCount };

  // Persistence key suffix + UI label, indexed by BarId.
  static constexpr const char* kBarKeys[kBarCount]   = {"base", "job", "hp", "sp"};
  static constexpr const char* kBarLabels[kBarCount] = {"Base", "Job", "HP", "SP"};

  // Global style shared by every bar.
  bool  visible_   = true;   // master toggle for the whole feature
  bool  locked_    = false;  // freeze + click-through (all bars)
  bool  sticky_    = false;  // magnetic edge-snap between bars on drag
  int   text_mode_ = 1;      // 0=none 1=percent 2=values 3=both
  bool  vertical_  = false;  // false = horizontal fill, true = vertical fill
  float rounding_  = 4.0f;   // corner rounding of the drawn bars (0..16)
  float bg_color_[4] = {0.05f, 0.05f, 0.07f, 0.70f};  // shared background + alpha

  // Full-screen alignment grid overlay (WoW-style HUD alignment helper).
  bool  grid_show_  = false;
  bool  grid_snap_  = false;  // snap bar position/size to grid cells
  int   grid_size_  = 32;  // cell size in px (4..128)
  float grid_color_[4] = {1.0f, 1.0f, 1.0f, 0.15f};

  Bar bars_[kBarCount] = {
    /* Base EXP */ {true, 100, 76, 220, 16, {0.36f, 0.78f, 1.00f, 1.00f}},
    /* Job EXP  */ {true, 100, 94, 220, 16, {1.00f, 0.82f, 0.30f, 1.00f}},
    /* HP       */ {true, 100, 40, 220, 16, {0.85f, 0.27f, 0.27f, 1.00f}},
    /* SP       */ {true, 100, 58, 220, 16, {0.30f, 0.62f, 0.95f, 1.00f}},
  };

  // Set by MoonlightUi when a size preset is applied so DrawBar force-applies
  // the new geometry for one frame even while unlocked.
  bool force_apply_ = false;

  // Set after the user finishes a drag/resize; MoonlightUi drains it (saves the
  // YAML once) so we never write the file every frame.
  bool geometry_dirty_ = false;

 private:
  bool in_game_      = false;
  bool drag_pending_ = false;  // geometry changed mid-drag, awaiting mouse-up

  // Custom drag/resize state (only one bar is ever dragged at a time).
  int   drag_mode_  = 0;     // 0=none, 1=move, 2=resize (bottom-right)
  float drag_off_x_ = 0.0f;  // mouse-to-anchor offset captured at drag start
  float drag_off_y_ = 0.0f;

  // Draws bar `id` with the given current/max value. Returns true if its stored
  // geometry changed this frame (user drag/resize).
  bool DrawBar(BarId id, long long cur, long long max);

  // Nearest magnetic alignment of value `v` (extent `ext`) on one axis to any
  // other shown bar's edges, within the snap threshold; else returns `v`.
  float SnapValue(float v, float ext, int self_id, bool y_axis) const;

  // Draws the full-screen alignment grid (background draw list).
  void DrawAlignmentGrid() const;
};
