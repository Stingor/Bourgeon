#pragma once

#include "features/plugin.h"

// Draws the player's WEAPON on top of the head-gears when the character is
// front-facing. In-world actors are drawn by CActorSprite_RenderLayered
// (0x00604220), which defers each layer's quad into a sorted RB-tree keyed by a
// z-value (CActorSprite_DeferQuadSorted 0x006046e0) and flushes ascending
// (small key = behind). Head-gears flagged "DrawOnTop" take the lua-priority
// branch (large key); the weapon (layer 5) takes the branch-1 fallback (small
// build-order key) and is excluded from the priority map, so a big head-gear
// paints over it. We raise the weapon's key (front-facing only) so it flushes
// last = on top. Two trampoline hooks installed once in the ctor; see
// project_char_portrait_re memory for the full RE.
class WeaponLayerTweaks : public Plugin {
 public:
  WeaponLayerTweaks();

  const char* name() const override { return "WeaponLayer"; }
};
