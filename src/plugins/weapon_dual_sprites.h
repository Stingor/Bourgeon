#pragma once

#include "plugins/plugin.h"

// Restores each weapon's OWN item sprite/animation while dual-wielding (or when
// only an off-hand weapon is equipped). Opt-in: default OFF.
//
// Why it's needed (RE, 20250716 client, no-ASLR addr == live):
//  - In-world characters build their weapon layers in
//    CActorSprite_BuildWeaponLayers (0x00D403A0). For a normal single weapon it
//    resolves that item's real per-item SPR+ACT pair into slot 5. But when the
//    off-hand also holds a weapon (assassin/kagerou dual-wield), it collapses
//    BOTH weapons to a single GENERIC combined bucket (weapon-class 25..30 via
//    Weapon_ResolveTypeBucket) — so the two daggers lose their individual
//    sprites and show a generic pair.
//  - Two resource vectors hang off the CActorSprite: ACT vector at this+0x4AC,
//    SPR vector at this+0x4B8 (confirmed: SetSlotAct 0x00D3FD90 writes 0x4AC,
//    SetSlotSprite 0x00D3D7C0 writes 0x4B8). Element N lives at vec[N] (slot 5 =
//    +0x14, slot 6 = +0x18).
//
// How it works (a faithful, offset-corrected port of the standalone WARP patch
// AllowDualWeaponSprites.qjs):
//  1) Equip hook on 0x00D403A0 — after running the stock build once to keep all
//     its side effects, it RE-invokes the stock builder with single-item
//     argument pairs to make it resolve each weapon's real pair into slot 5,
//     retains those resources with the client's own AddRef/Release, and installs
//     the off-hand pair into slot 6. On any failure it releases what it retained
//     and restores the exact stock state.
//  2) Action bridge on the ACT-frame call at 0x00D36EE4 — the blank combined
//     attack actions 88..95 are remapped to the direction-matched individual
//     actions 80..87, but ONLY when the live actor actually carries an
//     item-specific SPR+ACT pair in slot 6 (our patched state); every stock
//     layout passes straight through untouched.
//
// Both hooks are installed once in the ctor; the visible behaviour is gated by a
// single runtime flag so the setting can be toggled live. See the WARP script
// and the CActorSprite RE notes for the full contract.
class WeaponDualSprites : public Plugin {
 public:
  WeaponDualSprites();

  const char* name() const override { return "WeaponDualSprites"; }

  // Live toggle, bound to the Moonlight-Destiny settings checkbox and persisted
  // in bourgeon_settings.yaml. Backed by the file-scope flag the hooks read.
  bool& enabled();
};
