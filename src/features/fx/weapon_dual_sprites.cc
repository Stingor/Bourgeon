#include "ragnarok/render.h"
#include "ragnarok/actor.h"  // rag::actor : les offsets de CActorSprite
#include "features/fx/weapon_dual_sprites.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"
#include "ragnarok/item_db.h"  // itemdb::kItemIdToWeaponClassAddr
#include "ui/game_texture.h"  // ro::texmgr::kAddRefAddr / kReleaseAddr

// ── Per-item weapon sprites while dual-wielding ──────────────────────────────
// Offset-corrected port of the standalone WARP patch AllowDualWeaponSprites.qjs.
// The WARP script's header labels the two resource vectors backwards; the live
// binary is unambiguous (SetSlotAct 0x00D3FD90 -> this+0x4AC, SetSlotSprite
// 0x00D3D7C0 -> this+0x4B8), so ACT = 0x4AC and SPR = 0x4B8 here.

namespace {
constexpr uintptr_t kBuildWeaponLayers = 0x00D403A0;  // CActorSprite_BuildWeaponLayers
constexpr uintptr_t kActFrameCall      = 0x00D36EE4;  // CALL Act_GetFrame (E8 rel32)

// Les offsets de CActorSprite viennent du foyer (`rag::actor`) : vecteurs de
// couches, vue d'arme et vue de bouclier y sont documentes ensemble.
constexpr uint32_t kSlot5     = 5 * 4;  // +0x14
constexpr uint32_t kSlot6     = 6 * 4;  // +0x18
constexpr uint32_t kMinSpan   = 7 * 4;  // slots 0..6 must exist (end-begin >= 0x1C)

// Expected stock bytes — refuse to install on any other build.
const uint8_t kEquipStolen[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};  // push ebp;mov ebp,esp;push -1
}  // namespace

// __thiscall pointer to the stock CActorSprite_BuildWeaponLayers, reached via the
// jmp-hook trampoline. Calling it runs the full original (RET 8) and returns.
typedef void(__thiscall* BuildFn)(void* self, uint32_t main_view, uint32_t off_view);
typedef void(__fastcall* RefFn)(void* res);
// Maps an equip view/item id to its weapon sprite class. Returns <= 0 when the
// item is NOT a weapon (e.g. a shield). Mirrors the stock BuildWeaponLayers
// discriminator `0 < Weapon_ItemIdToWeaponClass(off)` used to tell a dual-wield
// off-hand weapon apart from a shield sharing the same this+0x444 field.
typedef int(__cdecl* ItemClassFn)(int item_view);

// File scope (NOT namespaced) so the naked bridge can resolve them by name.
// 🔴 DÉFAUT ON depuis 2026-08-15. Ce n'est plus un réglage de joueur mais le
// comportement normal du client : une arme porte son sprite, y compris en main
// gauche. La bascule survit uniquement dans Staff Tools, pour comparer avec le
// rendu d'origine quand on débogue les couches d'armes.
static bool    g_dual_enabled = true;               // live toggle read by both hooks
static void*   g_tramp_build  = nullptr;            // -> stock BuildWeaponLayers
static BuildFn g_stock_build  = nullptr;
static void*   g_act_get_frame = reinterpret_cast<void*>(render::kActionGetFrameAddr);

static const RefFn Res_AddRef  = reinterpret_cast<RefFn>(ro::texmgr::kAddRefAddr);
static const RefFn Res_Release = reinterpret_cast<RefFn>(ro::texmgr::kReleaseAddr);
static const ItemClassFn Weapon_ItemIdToWeaponClass =
    reinterpret_cast<ItemClassFn>(itemdb::kItemIdToWeaponClassAddr);

namespace {

inline uint32_t* VecBegin(void* actor, uint32_t field) {
  return *reinterpret_cast<uint32_t**>(reinterpret_cast<char*>(actor) + field);
}

// A weapon vector is usable only if allocated and holding at least 7 elements,
// so slots 5 and 6 are in bounds.
inline bool VecOk(void* actor, uint32_t field) {
  char* base = reinterpret_cast<char*>(actor) + field;
  uintptr_t begin = *reinterpret_cast<uintptr_t*>(base);
  uintptr_t end = *reinterpret_cast<uintptr_t*>(base + 4);
  return begin != 0 && end >= begin && (end - begin) >= kMinSpan;
}

inline uint32_t Slot(void* actor, uint32_t field, uint32_t slot_off) {
  return *reinterpret_cast<uint32_t*>(
      reinterpret_cast<char*>(VecBegin(actor, field)) + slot_off);
}
inline void SetSlot(void* actor, uint32_t field, uint32_t slot_off, uint32_t v) {
  *reinterpret_cast<uint32_t*>(
      reinterpret_cast<char*>(VecBegin(actor, field)) + slot_off) = v;
}

// Coaxes the stock builder into resolving each weapon's REAL per-item pair and
// leaves the main pair in slot 5 with the off-hand pair in slot 6. Returns false
// (after releasing anything it retained) if the resources can't be resolved, so
// the caller can fall back to the untouched stock state. No SEH here — the
// worker wraps the whole thing.
bool BuildSplit(void* actor, uint32_t main_view, uint32_t off_view) {
  auto slots_ok = [&] { return VecOk(actor, rag::actor::kActVec) && VecOk(actor, rag::actor::kSprVec); };

  // (a) Resolve the off-hand's real pair by building it as the sole main weapon.
  g_stock_build(actor, off_view, 0);
  if (!slots_ok()) return false;
  uint32_t ret_act = Slot(actor, rag::actor::kActVec, kSlot5);
  uint32_t ret_spr = Slot(actor, rag::actor::kSprVec, kSlot5);
  if (!ret_act || !ret_spr) return false;
  Res_AddRef(reinterpret_cast<void*>(ret_act));
  Res_AddRef(reinterpret_cast<void*>(ret_spr));

  auto release_retained = [&] {
    Res_Release(reinterpret_cast<void*>(ret_act));
    Res_Release(reinterpret_cast<void*>(ret_spr));
  };
  auto release_slot = [&](uint32_t field, uint32_t slot_off) {
    uint32_t p = Slot(actor, field, slot_off);
    if (p) Res_Release(reinterpret_cast<void*>(p));
  };

  if (main_view != 0) {
    // ── DUAL WIELD ── main's real pair into slot 5, off pair into slot 6.
    g_stock_build(actor, main_view, 0);
    if (!slots_ok()) { release_retained(); return false; }
    // If BOTH hands resolve to the SAME sprite (two identical weapons, e.g. two
    // Axe #1301, or two different items that fall back to the same base-class
    // sprite _도끼.spr), the split would put an identical layer in slots 5 and 6.
    // Both draw at the main-hand anchor -> they overlap perfectly -> only ONE
    // weapon is visible. The stock COMBINED bucket (_xxx_xxx.spr, positions both
    // hands correctly) is strictly better here, so bail out: the worker restores
    // the stock combined build via our `false` return. Per-item splitting stays
    // active only when the two weapons genuinely differ in sprite.
    uint32_t main_spr = Slot(actor, rag::actor::kSprVec, kSlot5);
    if (main_spr == ret_spr) { release_retained(); return false; }
    release_slot(rag::actor::kActVec, kSlot6);
    release_slot(rag::actor::kSprVec, kSlot6);
    SetSlot(actor, rag::actor::kActVec, kSlot6, ret_act);
    SetSlot(actor, rag::actor::kSprVec, kSlot6, ret_spr);
  } else {
    // ── OFF-HAND ONLY ── clear the generic slot 5, keep the real pair in slot 6.
    g_stock_build(actor, 0, off_view);
    if (!slots_ok()) { release_retained(); return false; }
    release_slot(rag::actor::kActVec, kSlot5);
    release_slot(rag::actor::kSprVec, kSlot5);
    release_slot(rag::actor::kActVec, kSlot6);
    release_slot(rag::actor::kSprVec, kSlot6);
    SetSlot(actor, rag::actor::kActVec, kSlot5, 0);
    SetSlot(actor, rag::actor::kSprVec, kSlot5, 0);
    SetSlot(actor, rag::actor::kActVec, kSlot6, ret_act);
    SetSlot(actor, rag::actor::kSprVec, kSlot6, ret_spr);
  }

  // Restore the view fields the multi-call dance overwrote (main is 0 in the
  // off-hand-only case, which is exactly what stock leaves there).
  *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(actor) + rag::actor::kWeaponView) = main_view;
  *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(actor) + rag::actor::kShieldView) = off_view;
  return true;
}

}  // namespace

// __fastcall worker holding all equip-hook logic. ecx = actor; the two view args
// arrive on the stack. Always runs the plain stock build first (so every stock
// side effect is preserved), then splits the resources when dual/off-hand.
// File scope (external linkage) so the naked entry stub can resolve it by name.
void __fastcall DualEquipWorker(void* actor, void* /*edx*/,
                                uint32_t main_view, uint32_t off_view) {
  if (!g_dual_enabled) {
    g_stock_build(actor, main_view, off_view);
    return;
  }
  __try {
    g_stock_build(actor, main_view, off_view);  // stock first, keep side effects
    if (off_view == 0) return;                  // single weapon: nothing to split
    // this+0x444 (off_view) doubles as the SHIELD view id. Only split when it is
    // a real weapon-class off-hand — exactly the stock `0 < class` dual-wield
    // gate. A shield here keeps the stock combined weapon+shield build; splitting
    // it would forge a phantom left-hand weapon sprite alongside the shield.
    if (Weapon_ItemIdToWeaponClass(static_cast<int>(off_view)) <= 0) return;
    if (!BuildSplit(actor, main_view, off_view))
      g_stock_build(actor, main_view, off_view);  // restore exact stock state
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Never leave a half-built actor: fall back to the plain stock build.
    g_stock_build(actor, main_view, off_view);
  }
}

// Naked entry that the jmp-hook lands on. Marshals (ecx = actor, [esp+4] = main,
// [esp+8] = off) into the __fastcall worker, then RET 8 like the stock function.
//
// The stock vtbl+0x60 (CActorSprite_BuildWeaponLayers) is an SEH function whose
// native caller keeps `this` in EDI across the call — EDI/ESI/EBX/EBP are all
// callee-saved. When the worker's __except swallows a fault raised deep inside a
// re-entrant stock rebuild, x86 MSVC only reloads a nonvolatile at the epilogue
// IF the prologue pushed it, so a compiler that didn't spill EDI would let the
// interrupted stock function's scratch value (e.g. the resolved job id 0xfad)
// leak back to the caller -> crash at its next `mov eax,[edi]`. Spilling all four
// nonvolatiles HERE, in code we control, guarantees the caller is made whole
// regardless of the worker's codegen or any swallowed exception.
__declspec(naked) static void DualEquipEntry() {
  __asm {
    push ebp                  // preserve nonvolatiles for the native caller
    push ebx
    push esi
    push edi
    mov  eax, [esp+0x14]      // main_view  (was [esp+4], +0x10 for the 4 pushes)
    mov  edx, [esp+0x18]      // off_view   (was [esp+8])
    push edx                  // fastcall stack arg 2 (off_view)
    push eax                  // fastcall stack arg 1 (main_view)
    // ecx still = actor (untouched by the pushes); edx = dummy 2nd reg arg
    call DualEquipWorker      // callee-cleans its 8 bytes of stack args
    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    ret  8                    // clean the stock function's 2 args
  }
}

// Action bridge, installed by repointing the CALL at 0x00D36EE4 at this stub.
// Entry: ecx = ACT resource, edi = actor, [esp+4] = action, [esp+8] = frame.
// Remaps the blank combined attack actions 88..95 to the individual 80..87, but
// only when the actor carries a real item-specific pair in slot 6 (our patched
// state), then tail-jumps into Act_GetFrame which returns to the stock caller.
__declspec(naked) static void ActFrameBridge() {
  __asm {
    cmp  byte ptr g_dual_enabled, 0
    je   passthrough
    cmp  dword ptr [esp+4], 0x58       // action >= 88 ?
    jb   passthrough
    cmp  dword ptr [esp+4], 0x5F       // action <= 95 ?
    ja   passthrough
    mov  eax, [edi+0x4B8]              // SPR vector begin (existence gate)
    test eax, eax
    jz   passthrough
    mov  edx, [edi+0x4AC]              // ACT vector begin (compare target)
    test edx, edx
    jz   passthrough

    cmp  ecx, [edx+0x18]              // live resource == ACT[6] ?
    jne  try_slot5
    cmp  dword ptr [eax+0x18], 0      // SPR[6] present ?
    je   passthrough
    sub  dword ptr [esp+4], 8         // 88..95 -> 80..87
    jmp  passthrough

  try_slot5:
    cmp  ecx, [edx+0x14]             // live resource == ACT[5] ?
    jne  passthrough
    cmp  dword ptr [eax+0x14], 0     // SPR[5] present ?
    je   passthrough
    cmp  dword ptr [eax+0x18], 0     // and SPR[6] present (our dual state) ?
    je   passthrough
    sub  dword ptr [esp+4], 8

  passthrough:
    jmp  [g_act_get_frame]
  }
}

namespace {

// Rewrites the 4-byte rel32 of the existing CALL at kActFrameCall so it targets
// dest instead of Act_GetFrame. The 0xE8 opcode is left in place.
void RepointCall(uintptr_t call_site, void* dest) {
  const uintptr_t rel_addr = call_site + 1;
  const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(dest) -
                                           (call_site + 5));
  DWORD old_protect;
  if (VirtualProtect(reinterpret_cast<void*>(rel_addr), sizeof(int32_t),
                     PAGE_EXECUTE_READWRITE, &old_protect)) {
    *reinterpret_cast<int32_t*>(rel_addr) = rel;
    VirtualProtect(reinterpret_cast<void*>(rel_addr), sizeof(int32_t),
                   old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(call_site), 5);
  }
}

}  // namespace

WeaponDualSprites::WeaponDualSprites() {
  using namespace hooking;

  // Guard against the wrong client: the equip entry must start with the exact
  // stolen prologue and the ACT-frame site must be a CALL rel32 to Act_GetFrame.
  if (std::memcmp(reinterpret_cast<void*>(kBuildWeaponLayers), kEquipStolen,
                  sizeof(kEquipStolen)) != 0) {
    // LogWarn("[WeaponDualSprites] equip prologue mismatch; not installing");
    return;
  }
  const uint8_t call_op = *reinterpret_cast<uint8_t*>(kActFrameCall);
  const int32_t call_rel = *reinterpret_cast<int32_t*>(kActFrameCall + 1);
  if (call_op != 0xE8 ||
      static_cast<uintptr_t>(kActFrameCall + 5 + call_rel) != render::kActionGetFrameAddr) {
    // LogWarn("[WeaponDualSprites] ACT-frame call mismatch; not installing");
    return;
  }

  g_tramp_build = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kBuildWeaponLayers),
      reinterpret_cast<uint8_t*>(&DualEquipEntry));
  g_stock_build = reinterpret_cast<BuildFn>(g_tramp_build);

  // Only arm the action bridge once the trampoline exists — without a working
  // equip hook there is never a slot-6 pair for the bridge to key off, so it
  // would be a pure no-op anyway.
  if (g_stock_build)
    RepointCall(kActFrameCall, reinterpret_cast<void*>(&ActFrameBridge));

  // LogInfo("[WeaponDualSprites] installed (tramp={})", g_tramp_build != nullptr);
}

bool& WeaponDualSprites::enabled() { return g_dual_enabled; }
