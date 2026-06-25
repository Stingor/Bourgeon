#pragma once

#include "plugins/plugin.h"

// Status-window (UIStatusWnd, window id 0xb) client-side relayout for the
// 20250716 client.  The stock window is created 280x141 and its DrawContent
// (vtable 0x010329d4 +0x50) draws every stat value at hardcoded coordinates
// tuned for the old background art.  This plugin retargets the layout to the
// custom 302x115 bitmap (data\texture\<ui>\statuswnd\w_statwin_bg.bmp):
//   - patches the creation SetSize immediates  280x141 -> 302x132
//   - swaps the DrawContent vtable slot for our own relayout that draws the
//     values at the new label/box positions and repositions the 6 stat-up
//     arrow buttons into the box arrow-cell.
// All patches are installed once in the constructor (engine is single-window,
// one shared vtable).  Compiled into the DLL; register in LoadPlugins().
//
// NOTE: only the NORMAL view is relaid out; the expanded 4th-job trait view
// (this+0xfc == 1) falls back to the original DrawContent untouched.
class StatusTweaks : public Plugin {
 public:
  StatusTweaks();

  const char* name() const override { return "Status"; }
};
