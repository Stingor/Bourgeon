#pragma once

#include "plugins/plugin.h"

// Skill-window ("grimoire") FPS fix. The skill window opened by the Skill button/
// hotkey is always UINewSkillListWnd (id 0x25); its "modern" grid view
// (Simplicity_SkillList == 0, the default) rebuilds the whole grid in immediate
// mode EVERY frame. Measured live, that DrawContent (0x00977E80) costs 9–15 ms
// per call and scales with the number of skill icons in the tab — dragging 144 Hz
// down to ~50 on a full tab. But the grid is static unless the player hovers /
// scrolls / switches tab / spends a point.
//
// This plugin hooks DrawContent and repaints ONLY WHEN A RENDER INPUT CHANGES
// (dirty byte + tab/hover/scroll/points/size snapshot), skipping the heavy rebuild
// on static frames. Timing is logged (LogDiag) as ran/s + skipped/s + avg ms.
//
// Full RE + fix rationale: docs/skill_tree_re.md.
class SkillTreeTweaks : public Plugin {
 public:
  SkillTreeTweaks();

  const char* name() const override { return "SkillTree"; }
};
