#include "features/fx/weapon_layer.h"

#include <Windows.h>

#include <cstdint>

#include "bourgeon.h"
#include "features/fx/weapon_dual_sprites.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// ── Weapon/shield z-order fix (in-world CActorSprite layered render) ──────────
// RE summary (Ghidra, no-ASLR addr==live; full map in project_weapon_zorder):
//  - In-world actors draw via CActorSprite_RenderLayered 0x00604220: each part's
//    quad is deferred into a sorted RB-tree (helper+0x9c) by
//    CActorSprite_DeferQuadSorted 0x006046e0, then flushed ASCENDING (small key =
//    drawn first = behind; large key = on top).
//  - DeferQuadSorted keys node[4] in two branches gated by the part-priority map
//    helper+0x7c (seeded ONLY for head-gear layers 2/3/4/8): branch 2 = lua
//    s_GetLayerPriority (large for "DrawOnTop" head-gears); branch 1 (weapon part
//    5, shield part 7, body) = a small build-sequence key. So a DrawOnTop
//    head-gear paints over the weapon, and (front-facing) the weapon paints over
//    the shield.
//  - The SHIELD renders at part 7, NOT 6: BuildShieldWeaponLayers clears slot 6
//    when a shield is equipped (confirmed live via the partIdx bitmask 0xbf).
//
// Fix (FRONT group {0,1,6,7} only — the BACK view is already correct natively):
// at the branch-1 key write (0x006049e4) force a large key so the order becomes
// head-gears < weapon (part 5 -> 0x40000000) < shield (part 7 -> 0x40000010).
// Two trampoline hooks on DeferQuadSorted: entry computes the force code into a
// global; the key-write detour applies it to EDI.
//
// When WeaponDualSprites is on, the off-hand weapon is drawn as an extra layer at
// part 6 (normally a weapon-trail slot, hence unhandled here); it would fall to
// the default small key and sink behind the head-gears. We lift it to just above
// the main weapon (part 6 -> 0x40000008), FRONT-facing only and only while that
// feature is active, so the two weapons sit together at the weapon level and
// stock trail effects keep their native z. Keys are kept distinct (…00/…08/…10)
// because DeferQuadSorted DROPS a quad whose key collides with one already in the
// tree.

namespace {
constexpr uintptr_t kDeferEntry = 0x006046e0;  // CActorSprite_DeferQuadSorted
constexpr uintptr_t kKeyWrite   = 0x006049e4;  // branch-1 key write (MOV [EAX+0x10],EDI)
}  // namespace

// File scope (NOT namespaced) so the naked __asm can resolve them by name.
static unsigned char g_weapon_top_force = 0;  // 0=none,1=MID,2=TOP,3=OFF-HAND (per call)
static void* g_tramp_defer = nullptr;          // -> relocated prologue + body
static void* g_tramp_key   = nullptr;          // -> relocated key writes + rest

// True only while the dual-weapon-sprites feature is on. The off-hand weapon is
// drawn as an EXTRA layer at partIdx 6 (resource slot 6), which normally holds a
// weapon-trail/glow — so we only lift part 6 to weapon level when this is set,
// leaving stock trail effects untouched.
static bool OffhandWeaponActive() {
  auto* wds = Bourgeon::Instance().weapon_dual_sprites();
  return wds && wds->enabled();
}

// Decision logic in clean C (no fragile naked asm). Called from the entry stub
// with the render-helper + partIdx. Sets g_weapon_top_force: 1 = MID key (above
// head-gears), 2 = TOP key (above the weapon). Front-facing only.
void __fastcall WeaponDecide(void* helper, int partIdx) {
  g_weapon_top_force = 0;
  __try {
    char* cas = *reinterpret_cast<char**>(reinterpret_cast<char*>(helper) + 0x3c);
    if (!cas) return;
    // facing = CActorSprite+0x38 - +0x34 (the value Actor_SelectPartLayerByDir
    // 0x00d38850 switches on). FRONT group {0,1,6,7}; BACK group {2,3,4,5}.
    const int facing = *reinterpret_cast<int*>(cas + 0x38) -
                       *reinterpret_cast<int*>(cas + 0x34);
    if (facing != 0 && facing != 1 && facing != 6 && facing != 7)
      return;  // back/side view is already correct natively — leave it untouched
    if (partIdx == 5) g_weapon_top_force = 1;        // weapon -> above head-gears
    else if (partIdx == 7) g_weapon_top_force = 2;   // shield -> above the weapon
    else if (partIdx == 6 && OffhandWeaponActive())
      g_weapon_top_force = 3;                        // off-hand weapon -> weapon level
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Naked entry stub: marshal (helper=ECX, partIdx=[esp+4]) into the __fastcall C
// decision fn, then continue into the original via the trampoline (ECX/stack
// preserved). All real logic lives in WeaponDecide above.
__declspec(naked) static void DeferEntryStub() {
  __asm {
    push eax
    push ecx                   // save helper
    push edx
    mov  edx, [esp+0x10]       // partIdx ([esp]=edx,+4=ecx,+8=eax,+0xc=ret,+0x10=param_1)
    call WeaponDecide          // __fastcall(ecx=helper, edx=partIdx)
    pop  edx
    pop  ecx                   // restore helper
    pop  eax
    jmp  [g_tramp_defer]
  }
}

// Branch-1 key write. If forced, overwrite EDI (the small build-seq key) with a
// large constant so the part flushes last (on top): 1 = MID (above head-gears,
// the weapon), 2 = TOP (above the weapon, the shield). EAX/ESI are live here
// (node ptr / tree link) — only EDI + flags may be touched.
__declspec(naked) static void KeyWriteStub() {
  __asm {
    cmp  byte ptr g_weapon_top_force, 0
    je   sk_done
    cmp  byte ptr g_weapon_top_force, 1
    je   sk_mid
    cmp  byte ptr g_weapon_top_force, 3
    je   sk_off
    mov  edi, 0x40000010        // TOP: the shield, above the weapon
    jmp  sk_done
  sk_off:
    mov  edi, 0x40000008        // OFF-HAND: the second weapon, just above the main
    jmp  sk_done
  sk_mid:
    mov  edi, 0x40000000        // MID: the (main) weapon, above head-gears
  sk_done:
    jmp  [g_tramp_key]
  }
}

WeaponLayer::WeaponLayer() {
  using namespace hooking;
  g_tramp_defer = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kDeferEntry),
      reinterpret_cast<uint8_t*>(&DeferEntryStub));
  g_tramp_key = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kKeyWrite),
      reinterpret_cast<uint8_t*>(&KeyWriteStub));
  // LogInfo("[WeaponLayer] z-order hooks installed (defer={} key={})",
          // g_tramp_defer != nullptr, g_tramp_key != nullptr);
}
