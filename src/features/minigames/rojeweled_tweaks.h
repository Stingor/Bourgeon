#pragma once

#include "features/plugin.h"

// Rojeweled — a Bejeweled/match-3 mini-game whose gems are real RO MONSTER
// sprites (the Poring family: Poring, Drops, Poporing, Marin, Angeling,
// Deviling). Rendered entirely with ImGui's ImDrawList; the monster sprites are
// loaded standalone through the game's sprite system (몬스터\<name>.spr/.act ->
// SpriteAtlas_BuildTexture) exactly like Roggle's Poring ball, and each type
// falls back to a coloured tile if its sprite can't load. Swap two adjacent
// monsters to line up 3+ of a kind; matches clear, columns collapse, and the
// board refills (cascades chain). Toggle from the Moonlight menu. DX9 only.
class RojeweledTweaks : public Plugin {
 public:
  const char* name() const override { return "Rojeweled"; }

  void OnRenderUI() override;

  // A mode/map switch can recreate the D3D device; drop cached sprite handles so
  // they reload on the new device (mirrors RoggleTweaks::OnModeSwitch).
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  bool enabled() const { return enabled_; }
  void SetEnabled(bool on) { enabled_ = on; }

 private:
  bool enabled_ = false;
};
