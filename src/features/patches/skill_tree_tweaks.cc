#include "features/patches/skill_tree_tweaks.h"

#include <Windows.h>

#include <cstdint>

#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// ── Skill grimoire ("modern" grid view) FPS fix ──────────────────────────────
// Full RE: docs/skill_tree_re.md. Measured live: UINewSkillListWnd::DrawContent
// (0x00977E80) runs EVERY frame (~= the FPS) and costs 9–15 ms per call, scaling
// with the number of skill icons in the active tab — it dragged 144 Hz down to
// ~50 on a full tab. (NOT the animated avatar: removing FUN_00974700 changed
// nothing.) The cost is the per-node loop rebuilding the whole grid in immediate
// mode: per skill, a texture-key build + TextureMgr_Load + an O(n) GetSkillInfoAt
// walk-and-copy — O(n²) + hundreds of heap allocs, every frame. But the grid is
// static unless the player hovers / scrolls / switches tab / spends a point.
//
// Fix: hook DrawContent and REPAINT ONLY WHEN A RENDER INPUT CHANGES — the game
// dirty byte (this+0x271) plus a snapshot of {tab, hovered, scroll, points, size,
// skill-point level}. On static frames the heavy rebuild is skipped and the last
// geometry is kept. Validated in-game: FPS back to native (144), display correct.
//
// If a rare update path ever leaves the grid stale (a change we don't watch), add
// its field to Snap below — the gate fails safe toward correctness (paint), not
// toward staleness, only for the fields it watches.

namespace {

constexpr uintptr_t kDrawContent = 0x00977e80;  // UINewSkillListWnd::DrawContent
using DrawContent_t = void(__fastcall*)(void*, void*);  // __thiscall(this)

// Watched instance fields (offsets from docs/skill_tree_re.md §3.2).
constexpr int kOffWidth   = 0x14;
constexpr int kOffHeight  = 0x18;
constexpr int kOffHovered = 0x250;
constexpr int kOffTab     = 0x254;
constexpr int kOffScroll  = 0x258;   // 600: scroll row (this+600 in FUN_00975730)
constexpr int kOffPoints  = 0x26c;   // "has skill points" gate
constexpr int kOffDirty   = 0x271;   // game dirty byte (set by input handlers)
constexpr uintptr_t kSkillPointLvl = 0x015fb9fc;  // changes when points are spent

DrawContent_t g_orig_draw = nullptr;

// Last-painted snapshot (init to impossible values -> first call always paints).
struct Snap {
  int width = -1, height = -1, hovered = -2, tab = -2, scroll = -2, points = -2, sp = -2;
} g_snap;

inline int RdI(void* base, int off) {
  return *reinterpret_cast<int*>(static_cast<uint8_t*>(base) + off);
}

void __fastcall DrawContentHook(void* self, void* edx) {
  const uint8_t dirty = *(static_cast<uint8_t*>(self) + kOffDirty);
  const Snap cur{RdI(self, kOffWidth),  RdI(self, kOffHeight), RdI(self, kOffHovered),
                 RdI(self, kOffTab),    RdI(self, kOffScroll), RdI(self, kOffPoints),
                 *reinterpret_cast<int*>(kSkillPointLvl)};

  const bool changed =
      dirty != 0 || cur.width != g_snap.width || cur.height != g_snap.height ||
      cur.hovered != g_snap.hovered || cur.tab != g_snap.tab ||
      cur.scroll != g_snap.scroll || cur.points != g_snap.points ||
      cur.sp != g_snap.sp;

  if (!changed) return;  // static frame -> keep last geometry, skip the rebuild

  g_orig_draw(self, edx);  // heavy rebuild (also clears the dirty byte)
  g_snap = cur;            // baseline to the just-painted state
}

}  // namespace

SkillTreeTweaks::SkillTreeTweaks() {
  g_orig_draw = reinterpret_cast<DrawContent_t>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kDrawContent),
          reinterpret_cast<uint8_t*>(&DrawContentHook)));
  // LogInfo("[SkillTree] repaint-on-change gate {}",
          // g_orig_draw ? "installed" : "FAILED");
}
