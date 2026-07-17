#include "plugins/hat_effect_depth.h"

#include <Windows.h>

#include <cstdint>
#include <vector>

#include "utils/log_console.h"

// Portage à l'exécution du patch WARP « AllHatEffectsWorldDepthOracle » (bytes
// validés en jeu). On reproduit le code machine à l'identique ; voir le header
// et le .qjs d'origine pour la description algébrique. Le client 20250716 n'est
// pas relogé (base préférée 0x00400000), mais on applique un delta de rebase
// par sécurité en lisant la vraie base module.

namespace {

// ── Adresses natives (VA image base 0x00400000, avant rebase) ────────────────
constexpr uint32_t kBuildBase = 0x00400000;

// Deux sites d'appel de rendu HatEffect à envelopper (E8 rel32 -> boucle STR).
constexpr uint32_t kRenderCallNoArgVa = 0x00AFE41C;
constexpr uint32_t kRenderCallArgVa = 0x00AFE52B;
// Boucle de rendu screen-STR (possède position/taille/anim/texture/UV/blend).
constexpr uint32_t kScreenLayerLoopVa = 0x00AD8750;
// Chargement de profondeur réciproque à détourner (movss xmm0,[ebp-0x64]).
constexpr uint32_t kDepthHookVa = 0x00AD93D4;
constexpr uint32_t kDepthStockContinueVa = 0x00AD93D9;      // path natif
constexpr uint32_t kDepthConvertedContinueVa = 0x00AD93E2;  // path converti
// Depth_NormalizeToClip : normalise la profondeur monde en profondeur clip.
constexpr uint32_t kWorldDepthConvertVa = 0x00554040;
// Contexte de rendu (matrice de projection à +0x08/+0x24) et constantes SSE.
constexpr uint32_t kRenderContextPtrVa = 0x012515F8;
constexpr uint32_t kNativeDepthQuantumVa = 0x00FD6AE4;
constexpr uint32_t kNativeAbsMaskVa = 0x00FD5D60;

// Biais de contact (4 quanta natifs) : évite que le sol/mur avale le sprite.
constexpr uint32_t kContactDepthBiasBits = 0x3827C5AC;

// ── Signatures attendues sur les sites patchés (garde build-specific) ────────
struct SigCheck {
  uint32_t va;
  std::vector<uint8_t> expected;
  const char* label;
};

// Layout du code cave (mêmes offsets que le .qjs).
constexpr uint32_t kCaveSize = 0x120;
constexpr uint32_t kContactBiasOffset = 0x04;
constexpr uint32_t kWrapperOffset = 0x08;
constexpr uint32_t kDepthBridgeOffset = 0x30;
constexpr uint32_t kWrapperSize = 32;
constexpr uint32_t kDepthBridgeSize = 228;

// Petit émetteur d'octets (mime les helpers du .qjs).
struct Emit {
  std::vector<uint8_t> b;
  void Byte(uint8_t x) { b.push_back(x); }
  void Bytes(std::initializer_list<uint8_t> xs) {
    for (uint8_t x : xs) b.push_back(x);
  }
  void U32(uint32_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 24));
  }
  void Append(const Emit& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
  uint32_t Size() const { return static_cast<uint32_t>(b.size()); }
};

bool VerifySig(intptr_t delta, const SigCheck& s) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s.va + delta);
  __try {
    for (size_t i = 0; i < s.expected.size(); ++i) {
      if (p[i] != s.expected[i]) return false;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// Écrit `bytes` (5 o) à `va+delta` en levant temporairement la protection.
bool PatchSite(intptr_t delta, uint32_t va, const uint8_t* bytes, size_t n) {
  void* dst = reinterpret_cast<void*>(va + delta);
  DWORD old = 0;
  if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
  memcpy(dst, bytes, n);
  VirtualProtect(dst, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), dst, n);
  return true;
}

// Construit le wrapper 32 o (arme le flag autour de l'appel boucle STR).
Emit BuildWrapper(uint32_t wrapper_va, uint32_t flag_va, uint32_t loop_va) {
  Emit e;
  // mov dword[flag],1
  e.Bytes({0xC7, 0x05});
  e.U32(flag_va);
  e.U32(1);
  // push [esp+4]
  e.Bytes({0xFF, 0x74, 0x24, 0x04});
  // call ScreenLayerLoop  (rel32, source = wrapper_va + offset courant)
  {
    uint32_t src = wrapper_va + e.Size();
    e.Byte(0xE8);
    e.U32(loop_va - (src + 5));
  }
  // mov dword[flag],0
  e.Bytes({0xC7, 0x05});
  e.U32(flag_va);
  e.U32(0);
  // ret 4
  e.Bytes({0xC2, 0x04, 0x00});
  return e;
}

// Construit le depth bridge 228 o. `*_va` sont les adresses runtime (cave pour
// flag/contact_bias/bridge, module rebasé pour le reste).
Emit BuildDepthBridge(uint32_t bridge_va, uint32_t flag_va, uint32_t contact_va,
                      uint32_t render_ctx_va, uint32_t abs_mask_va,
                      uint32_t depth_quantum_va, uint32_t world_convert_va,
                      uint32_t stock_continue_va, uint32_t converted_continue_va) {
  // Fragment « head » : gate sur le flag + prépare le calcul de profondeur.
  Emit head;
  {
    auto src = [&]() { return bridge_va + head.Size(); };
    // cmp dword[flag],0
    head.Bytes({0x83, 0x3D});
    head.U32(flag_va);
    head.Byte(0x00);
    // jne +0x0A  (saute vers la 2e movss, après le E9 stock)
    head.Bytes({0x75, 0x0A});
    // movss xmm0,[ebp-0x64]  (instruction native reconstituée)
    head.Bytes({0xF3, 0x0F, 0x10, 0x45, 0x9C});
    // jmp DepthStockContinue  (rel32)
    {
      uint32_t s = src();
      head.Byte(0xE9);
      head.U32(stock_continue_va - (s + 5));
    }
    // movss xmm0,[ebp-0x64]
    head.Bytes({0xF3, 0x0F, 0x10, 0x45, 0x9C});
    // subss xmm0,xmm3
    head.Bytes({0xF3, 0x0F, 0x5C, 0xC3});
    // movss [edx-0x10],xmm0
    head.Bytes({0xF3, 0x0F, 0x11, 0x42, 0xF0});
    // sub esp,0x14
    head.Bytes({0x83, 0xEC, 0x14});
    // movss [esp+4],xmm1 / [esp+8],xmm4 / [esp+0xc],xmm5 / [esp+0x10],xmm6
    head.Bytes({0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x04});
    head.Bytes({0xF3, 0x0F, 0x11, 0x64, 0x24, 0x08});
    head.Bytes({0xF3, 0x0F, 0x11, 0x6C, 0x24, 0x0C});
    head.Bytes({0xF3, 0x0F, 0x11, 0x74, 0x24, 0x10});
    // movss xmm0,[ebp-0x64]
    head.Bytes({0xF3, 0x0F, 0x10, 0x45, 0x9C});
    // addss xmm0,[contact_bias]
    head.Bytes({0xF3, 0x0F, 0x58, 0x05});
    head.U32(contact_va);
    // movss xmm4,[edx-0x18]
    head.Bytes({0xF3, 0x0F, 0x10, 0x62, 0xE8});
    // comiss xmm4,[ebp-0x3c]
    head.Bytes({0x0F, 0x2F, 0x65, 0xC4});
  }

  // Fragment « upright head » : reconstruit la profondeur réciproque Y-up.
  Emit up_head;
  {
    // mov eax,[render_ctx]
    up_head.Byte(0xA1);
    up_head.U32(render_ctx_va);
    // movd xmm5,[eax+0x24] ; cvtdq2ps xmm5,xmm5
    up_head.Bytes({0x66, 0x0F, 0x6E, 0x68, 0x24});
    up_head.Bytes({0x0F, 0x5B, 0xED});
    // subss xmm4,xmm5 ; movss xmm6,[ebp-0x3c] ; subss xmm6,xmm5
    up_head.Bytes({0xF3, 0x0F, 0x5C, 0xE5});
    up_head.Bytes({0xF3, 0x0F, 0x10, 0x75, 0xC4});
    up_head.Bytes({0xF3, 0x0F, 0x5C, 0xF5});
    // mov ecx,[ebp+0x10] ; movss xmm5,[ecx+0x14]
    up_head.Bytes({0x8B, 0x4D, 0x10});
    up_head.Bytes({0xF3, 0x0F, 0x10, 0x69, 0x14});
    // mulss xmm4,xmm5 ; mulss xmm6,xmm5
    up_head.Bytes({0xF3, 0x0F, 0x59, 0xE5});
    up_head.Bytes({0xF3, 0x0F, 0x59, 0xF5});
    // movss xmm5,[ecx+0x10] ; mulss xmm5,[eax+8]
    up_head.Bytes({0xF3, 0x0F, 0x10, 0x69, 0x10});
    up_head.Bytes({0xF3, 0x0F, 0x59, 0x68, 0x08});
    // subss xmm4,xmm5 ; subss xmm6,xmm5
    up_head.Bytes({0xF3, 0x0F, 0x5C, 0xE5});
    up_head.Bytes({0xF3, 0x0F, 0x5C, 0xF5});
  }

  // Fragment « guard » : borne le dénominateur (|xmm6| vs quantum natif).
  Emit guard;
  {
    // movaps xmm5,xmm6
    guard.Bytes({0x0F, 0x28, 0xEE});
    // andps xmm5,[abs_mask]
    guard.Bytes({0x0F, 0x54, 0x2D});
    guard.U32(abs_mask_va);
    // comiss xmm5,[depth_quantum]
    guard.Bytes({0x0F, 0x2F, 0x2D});
    guard.U32(depth_quantum_va);
  }

  // Fragment « tail » : rhw' = rhw * ratio + biais de contact.
  Emit tail;
  {
    // divss xmm4,xmm6 ; mulss xmm4,[ebp-0x64] ; addss xmm4,[contact_bias]
    tail.Bytes({0xF3, 0x0F, 0x5E, 0xE6});
    tail.Bytes({0xF3, 0x0F, 0x59, 0x65, 0x9C});
    tail.Bytes({0xF3, 0x0F, 0x58, 0x25});
    tail.U32(contact_va);
    // movaps xmm0,xmm4
    tail.Bytes({0x0F, 0x28, 0xC4});
  }

  // Cible commune (jae/jbe) = le movss [esp],xmm0 juste avant l'appel convert.
  const uint32_t depth_convert_va = bridge_va + head.Size() + 2 +
                                    up_head.Size() + guard.Size() + 2 +
                                    tail.Size();

  Emit d;
  d.Append(head);
  // jae depth_convert  (rel8) : profondeur "always-on-top" conservée sous l'ancre
  {
    uint32_t s = bridge_va + d.Size();
    d.Byte(0x73);
    d.Byte(static_cast<uint8_t>(depth_convert_va - (s + 2)));
  }
  d.Append(up_head);
  d.Append(guard);
  // jbe depth_convert  (rel8) : dénominateur trop petit -> pas de reconstruction
  {
    uint32_t s = bridge_va + d.Size();
    d.Byte(0x76);
    d.Byte(static_cast<uint8_t>(depth_convert_va - (s + 2)));
  }
  d.Append(tail);
  // movss [esp],xmm0   (= depth_convert_va)
  d.Bytes({0xF3, 0x0F, 0x11, 0x04, 0x24});
  // call Depth_NormalizeToClip  (rel32)
  {
    uint32_t s = bridge_va + d.Size();
    d.Byte(0xE8);
    d.U32(world_convert_va - (s + 5));
  }
  // restaure xmm1/xmm4/xmm5/xmm6, stocke le résultat, add esp,0x14
  d.Bytes({0xF3, 0x0F, 0x10, 0x4C, 0x24, 0x04});
  d.Bytes({0xF3, 0x0F, 0x10, 0x64, 0x24, 0x08});
  d.Bytes({0xF3, 0x0F, 0x10, 0x6C, 0x24, 0x0C});
  d.Bytes({0xF3, 0x0F, 0x10, 0x74, 0x24, 0x10});
  d.Bytes({0xD9, 0x5A, 0xEC});  // fstp / store vers le vertex
  d.Bytes({0x83, 0xC4, 0x14});
  // jmp DepthConvertedContinue  (rel32)
  {
    uint32_t s = bridge_va + d.Size();
    d.Byte(0xE9);
    d.U32(converted_continue_va - (s + 5));
  }
  return d;
}

}  // namespace

HatEffectDepthTweaks::HatEffectDepthTweaks() { InstallDepthPatch(); }

void HatEffectDepthTweaks::InstallDepthPatch() {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const intptr_t delta = static_cast<intptr_t>(base) - kBuildBase;

  // 1. Vérifier toutes les signatures build-specific. Le moindre écart (autre
  //    build OU exe déjà patché par WARP) -> on ne touche à rien.
  const SigCheck sigs[] = {
      {kRenderCallNoArgVa, {0xE8, 0x2F, 0xA3, 0xFD, 0xFF}, "render call (no-arg)"},
      {kRenderCallArgVa, {0xE8, 0x20, 0xA2, 0xFD, 0xFF}, "render call (arg)"},
      {0x00AD8D91, {0xE8, 0x1A, 0xB4, 0xA7, 0xFF}, "anchor projection"},
      {0x00AD93C5, {0xC7, 0x42, 0xEC, 0xC8, 0x1C, 0x02, 0x38}, "constant near depth"},
      {0x00AD9308, {0xF3, 0x0F, 0x59, 0x1D, 0xE4, 0x6A, 0xFD, 0x00}, "layer-depth quantum"},
      {kDepthHookVa, {0xF3, 0x0F, 0x10, 0x45, 0x9C}, "reciprocal-depth load"},
      {0x00AD8B7D, {0xE8, 0xBE, 0xB4, 0xA7, 0xFF}, "normalized-depth conversion call"},
  };
  for (const SigCheck& s : sigs) {
    if (!VerifySig(delta, s)) {
      LogInfo("[HatEffectDepth] signature '{}' absente (build non supporte ou "
              "deja patche) — plugin inactif", s.label);
      return;
    }
  }

  // 2. Réserver le code cave (RWX) et découper le layout.
  void* cave = VirtualAlloc(nullptr, kCaveSize, MEM_COMMIT | MEM_RESERVE,
                            PAGE_EXECUTE_READWRITE);
  if (!cave) {
    LogInfo("[HatEffectDepth] VirtualAlloc du code cave a echoue — plugin inactif");
    return;
  }
  const uint32_t cave_va = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(cave));
  const uint32_t flag_va = cave_va;
  const uint32_t contact_va = cave_va + kContactBiasOffset;
  const uint32_t wrapper_va = cave_va + kWrapperOffset;
  const uint32_t bridge_va = cave_va + kDepthBridgeOffset;

  // Données du cave : flag=0, biais de contact.
  *reinterpret_cast<uint32_t*>(flag_va) = 0;
  *reinterpret_cast<uint32_t*>(contact_va) = kContactDepthBiasBits;

  // 3. Générer les deux stubs (adresses module rebasées).
  const Emit wrapper = BuildWrapper(
      wrapper_va, flag_va, static_cast<uint32_t>(kScreenLayerLoopVa + delta));
  const Emit bridge = BuildDepthBridge(
      bridge_va, flag_va, contact_va,
      static_cast<uint32_t>(kRenderContextPtrVa + delta),
      static_cast<uint32_t>(kNativeAbsMaskVa + delta),
      static_cast<uint32_t>(kNativeDepthQuantumVa + delta),
      static_cast<uint32_t>(kWorldDepthConvertVa + delta),
      static_cast<uint32_t>(kDepthStockContinueVa + delta),
      static_cast<uint32_t>(kDepthConvertedContinueVa + delta));

  if (wrapper.Size() != kWrapperSize || bridge.Size() != kDepthBridgeSize) {
    LogInfo("[HatEffectDepth] taille stub inattendue (wrapper={} bridge={}) — "
            "plugin inactif", wrapper.Size(), bridge.Size());
    VirtualFree(cave, 0, MEM_RELEASE);
    return;
  }
  memcpy(reinterpret_cast<void*>(wrapper_va), wrapper.b.data(), wrapper.Size());
  memcpy(reinterpret_cast<void*>(bridge_va), bridge.b.data(), bridge.Size());
  FlushInstructionCache(GetCurrentProcess(), cave, kCaveSize);

  // 4. Activer : rediriger les deux CALL vers le wrapper et le JMP vers le bridge.
  bool ok = true;
  auto make_call = [&](uint32_t site_va, uint32_t target_va) {
    const uint32_t site = static_cast<uint32_t>(site_va + delta);
    uint8_t bytes[5] = {0xE8};
    const uint32_t rel = target_va - (site + 5);
    memcpy(bytes + 1, &rel, 4);
    ok = PatchSite(delta, site_va, bytes, 5) && ok;
  };
  make_call(kRenderCallNoArgVa, wrapper_va);
  make_call(kRenderCallArgVa, wrapper_va);
  {
    const uint32_t site = static_cast<uint32_t>(kDepthHookVa + delta);
    uint8_t bytes[5] = {0xE9};
    const uint32_t rel = bridge_va - (site + 5);
    memcpy(bytes + 1, &rel, 4);
    ok = PatchSite(delta, kDepthHookVa, bytes, 5) && ok;
  }

  LogInfo("[HatEffectDepth] {} (cave=0x{:08X}) — profondeur monde active sur les "
          "hat-effects .str", ok ? "installe" : "ECHEC patch", cave_va);
}
