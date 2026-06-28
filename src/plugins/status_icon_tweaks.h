#pragma once

#include "plugins/plugin.h"

// Persisted status-icon-bar configuration (plain ints so moonlight_ui can
// serialize it without the internal enums).  Defaults reproduce the stock bar.
struct StatusIconConfig {
  bool enabled    = false;
  int  corner     = 1;    // 0=top-left 1=top-right 2=bottom-left 3=bottom-right
  int  margin_x   = 32;   // px from the anchor corner
  int  margin_y   = 185;
  int  step_dir   = 0;    // within-line: 0=down 1=up 2=right 3=left
  int  wrap_dir   = 3;    // new line:    0=down 1=up 2=right 3=left
  int  per_line   = 17;
  int  icon_pitch = 35;
  int  line_pitch = 45;
  int  sort_mode  = 0;    // 0=none 1=longest-first 2=shortest-first
  bool show_remaining = false;
  bool time_bg = false;   // draw a dark background behind the remaining-time text
};

// Buff/debuff status-icon BAR relayout for the 20250716 client.
//
// The on-screen status icons (top-right cluster of small effect icons) are NOT
// a UIWindow — there is no UIStateIconWnd.  They are individual effect-sprite
// nodes built once per change by FUN_00bd4230(scene), which lays them out
// anchored to the bottom/right of the screen: each icon stacks DOWN 35px, and
// when a column fills it wraps to a new column 45px to the LEFT.
//
// This plugin JMP-hooks FUN_00bd4230 and reimplements the build with a fully
// configurable layout model:
//   pos(i) = corner-origin + (i / perLine)*wrapVec + (i % perLine)*stepVec
// so the bar can be anchored to any corner, stacked in any direction (down/up/
// left/right, vertical- or horizontal-first) with arbitrary spacing and icons
// per line.  Node creation reuses the engine's own helpers, so textures /
// lifetime / tooltips behave exactly as stock.  When disabled the hook simply
// forwards to the original, so the stock layout is byte-for-byte preserved.
//
// A live ImGui panel ("Status Icons") drives the settings and forces a rebuild
// on change for instant preview.
//
// KNOWN LIMITATION (v1): the mouseover tooltip hit-test (FUN_00c93cb0) keeps its
// own copy of the stock right-anchored layout constants, so when the bar is
// moved the icon tooltips will be offset.  Syncing the hit-test is a follow-up.
class StatusIconTweaks : public Plugin {
 public:
  StatusIconTweaks();

  const char* name() const override { return "StatusIcons"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Persisted config access (used by moonlight_ui's settings save/load).
  StatusIconConfig& config();
  void MarkDirty();  // request a bar rebuild after an external config change

  // Renders the status-icon settings controls (no window of its own); hosted
  // by moonlight_ui inside its "Icônes de statut" section.
  void DrawSettings();
};
