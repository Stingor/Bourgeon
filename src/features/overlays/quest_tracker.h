#pragma once

#include "features/plugin.h"

// Persisted on-screen quest-tracker configuration (plain scalars so moonlight_ui
// can serialize it without exposing internals). Defaults roughly reproduce the
// stock overlay's colors.
struct QuestTrackerConfig {
  bool enabled  = false;   // master switch: hide native overlay + draw ours
  bool show_titlebar = true;  // title bar "Suivi de quête (N)" + close button
  bool locked   = true;    // (borderless mode only) click-through, no drag
  int  pos_x    = 120;     // ImGui window position (persisted)
  int  pos_y    = 150;
  int  width    = 280;     // panel width in px (text wraps to this)
  int  max_quests = 8;     // native caps at 4; we allow more
  int  title_rgb = 0x00D6E1;  // quest title color (native = 0xd6e1)
  int  desc_rgb  = 0xFFFFFF;  // objective/description color (native = white)
  int  hunt_rgb  = 0xFFE060;  // hunt progress lines ("mob ( x / y )")
  int  font_scale = 100;   // % (100 = ImGui default)
  bool show_bg   = true;   // translucent window background
  int  bg_alpha  = 55;     // % background alpha when show_bg
  bool show_objective = true;  // draw the objective/description line
};

// On-screen QUEST TRACKER reimplementation for the 20250716 client
// (Moonlight-Destiny.exe, base 0x400000).  See project_quest_tracking_re.
//
// The native tracker is UIWindow id 0xcc (ptr @ g_UIWindowMgr+0x428).  Its
// DrawContent (QuestTracker_DrawContent @ 0x009137a0, vtable+0x50) walks the
// active-quest map (*(0x01254d90)) and hard-codes colors (title 0xd6e1, rest
// white), font size, a 4-quest cap and a fixed top-right anchor.
//
// Instead of patching all those immediates, this plugin JMP-hooks the native
// DrawContent to a no-op (so the native panel disappears) and re-draws the same
// data in a fully configurable ImGui window (position, colors, font, width,
// max quests).  When disabled the hook forwards to the original, so the stock
// overlay is byte-for-byte preserved.  Data is read straight from the quest
// records — no engine string builders are called — under SEH guards.
//
// Quest record (std::_Tree node) fields.  The Quest struct starts at node+0x18;
// id/active/title/tracked are read node-absolute, objective + hunt fields via
// the node+0x18 base (so node-absolute = 0x18 + Quest-relative):
//   +0x10 id, +0x18(byte) active, +0x1c title(std::string), +0x64 objective,
//   +0xcc(byte) tracked, +0xea(byte) hunt count, +0xf8[i] cur, +0x198[i] tgt.
// (node+0x4c is an internal category tag, e.g. "QUEST" — NOT the objective.)
// The hunt-target monster name is not a plain field (native builds it from
// level-range/format logic); TODO once REd against a live hunt quest.
class QuestTracker : public Plugin {
 public:
  QuestTracker();

  const char* name() const override { return "QuestTracker"; }

  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Persisted config access (used by moonlight_ui's settings save/load).
  QuestTrackerConfig& config();

  // Settings controls (no window of its own); hosted by moonlight_ui.
  void DrawSettings();
};
