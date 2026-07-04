#pragma once

#include "plugins/plugin.h"

// Generic UIWindow position persistence (20250716 client, base 0x400000).
//
// Many native windows never persist their screen position across a client
// restart: the engine only saves a window's position if its ctor sets the
// save-enable byte [this+0xb4]=1 (see project_window_position_persistence).
// Status (0xb) and Equip (0xa) were fixed one-off in their own plugins; this
// plugin generalises that mechanism for every OTHER movable window (Achievement,
// Bank, Mail, ...) with a single table-driven engine — no per-window RE of the
// message handler is required, only the window id.
//
// Mechanism (table-driven over {id, key}):
//   * RESTORE — a single jmp-hook on UIWindowMgr_MakeWindow overrides the window's
//     position (SetPos, vtable+0x10) with the saved one right after the factory
//     builds it, BEFORE the first render → no flicker. Works whatever way the
//     window positions itself internally (direct SetPos or msg 0x22). Restore
//     happens here, not in OnTick.
//   * SAVE — OnTick reads the live x/y (win+0x1c / win+0x20) via FindWindow and
//     persists drags (throttled ~200ms).
// The saved positions round-trip through MoonlightUi's settings yaml via the
// enumeration accessors below (one <key>_pos_x / <key>_pos_y pair per window).
//
// To make a new window persist: add one {id, "key"} row to the table in
// window_pos_tweaks.cc.
class WindowPosTweaks : public Plugin {
 public:
  WindowPosTweaks();

  const char* name() const override { return "Window Positions"; }

  // Reads each tracked window's live position (throttled) and restores a
  // freshly-loaded position on the first tick the window is open.
  void OnTick() override;
};

// ── Enumeration API for MoonlightUi Save/LoadSettings ───────────────────────
// MoonlightUi loops over these so adding a window needs no yaml edit there.
int         WindowPosTweaks_Count();
const char* WindowPosTweaks_Key(int i);   // yaml key prefix, e.g. "achievement"
int         WindowPosTweaks_X(int i);     // saved x (INT_MIN = unset)
int         WindowPosTweaks_Y(int i);     // saved y (INT_MIN = unset)
void        WindowPosTweaks_SetSavedPos(int i, int x, int y);  // called on load
