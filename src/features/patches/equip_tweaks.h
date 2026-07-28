#pragma once

#include "features/plugin.h"

// Equipment-window (UIEquipWnd, window id 0xa) client-side widening for the
// 20250716 client.  The stock window is created 280x167 and the slot grid
// (FUN_00897d60) draws each equipped item's icon + NAME with the name text
// width-capped at 0x46 = 70px, which truncates longer names ("Protection Co
// at" -> "Protection Coat").  Layout is mirrored: the left column is
// [icon@6][name@33->center], the right column is [<-name@172][icon@251].
//
// This plugin widens the window and the right column so names fit, driven by a
// single knob (kExtraWidth, in equip_tweaks.cc).  All changes are one-time,
// guarded immediate patches in the existing engine functions — NO DrawContent
// rewrite is needed:
//   - window width   push 0x118 (own @0x00a3a2e4, other-view @0x00a3f0f6)
//   - right icon x   mov imm 0xfb=251  @0x00897df1  (+= kExtraWidth)
//   - name width cap push imm8 0x46=70 @0x00897ffb  (raised, clamped to 120)
// The slot hit-test (FUN_0089ec70) buckets the cursor into the left/right
// column zone by x and picks the slot by ROW (y), with the right zone ending at
// (width-6), so it AUTO-FOLLOWS the widening — no hit-test patch required.
//
// IMPORTANT: the window body background is the fixed-size bitmap
//   data\texture\<ui>\basic_interface\equipwin_bg.bmp   (own / General+Costume)
//   data\texture\<ui>\basic_interface\equipwin_bg2.bmp  (costume / other player)
// blitted at its own width.  Widening the window past the bitmap width leaves an
// unpainted strip on the right, so kExtraWidth stays 0 (no-op) until those two
// bmps are widened to (280 + kExtraWidth) px in the data\ folder.  See the knob
// comment in equip_tweaks.cc.
class EquipTweaks : public Plugin {
 public:
  EquipTweaks();

  const char* name() const override { return "Equip"; }

  // Persists the OWN equipment window's position when it changes (reads it live
  // via FindWindow, throttled 200ms) — covers drag-end and every close path. The
  // engine never saves window id 0xa's position, so the message-handler hook
  // re-applies our saved x/y on open (msg 0x22). See status_tweaks for the
  // template this mirrors.
  void OnTick() override;
};

// OWN equipment-window saved position, persisted through MoonlightUi's settings
// yaml (only the own view, this+0xb4 == 0 — not the other-player view id 0x8b).
// INT_MIN = no saved position.
int  EquipTweaks_SavedX();
int  EquipTweaks_SavedY();
void EquipTweaks_SetSavedPos(int x, int y);
