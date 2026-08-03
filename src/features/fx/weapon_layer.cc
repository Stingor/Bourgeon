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
// FRONT {0,1,6,7} — corrigé : au moment où la branche 1 écrit sa clé
// (0x006049e4), on en force une grande, de sorte que l'ordre devienne
// coiffes/cape < arme (partie 5 -> 0x40000000) < bouclier (partie 7 ->
// 0x40000010). Ces clés écrasent n'importe quelle priorité lua, qui plafonne
// dans les centaines.
//
// BACK {2,3,4,5} — laissé au NATIF. Le bouclier y passe devant le corps, ce qui
// est faux (le personnage le porte de l'autre côté), mais c'est un défaut avec
// lequel on vit sciemment. Voir ci-dessous.
//
// 🔴 POURQUOI ON NE CORRIGE PAS LA VUE DE DOS (tranché le 2026-08-03, après
// quatre tentatives et autant de mesures).
//
// L'ordre de l'arbre n'est PAS l'ordre de dessin. Le flush ne dessine pas : il
// SOUMET, avec une profondeur. Juste avant, chaque noeud reçoit
//     node[9]  = node[7] * helper[41] + helper[42]   (node[7] = X droit du quad)
//     node[10] = node[5] * helper[41] + helper[42]   (node[5] = X gauche)
// puis Actor_SubmitQuad_RenderQueue écrit z = node[9] + rang * 1e-6. La file
// retrie par z croissant, et le rang ne pèse que 1e-6 là où les écarts entre
// pièces valent 2e-4 : il départage, il n'ordonne pas.
//
// Ce qui ordonne est donc le X DROIT du quad, avec helper[41] NÉGATIF : plus une
// pièce s'étend vers la droite, plus elle est au fond. Le bouclier est plus
// ÉTROIT que le corps (mesuré : X 446 contre 511), donc mécaniquement moins
// profond, donc devant. C'est structurel, pas accidentel.
//
// Le faire passer derrière demande de le déplacer d'une profondeur du même ordre
// que celle qui sépare le personnage du DÉCOR. Trois calibrages l'ont montré :
//   - 5e-4 fixe        -> disparaît (derrière le sol) ;
//   - 3e-4 fixe        -> correct au zoom de référence, disparaît au dézoom
//                         (l'unité de profondeur suit la caméra, pas la
//                          constante) ;
//   - 100 px * |a|     -> suit le zoom, mais reste un forfait : au zoom fort il
//                         replonge, et le quad ayant un gradient de profondeur,
//                         un bord passe sous le sol avant l'autre — bouclier à
//                         moitié disparu, sprite déformé au ras du sol.
// Viser exactement le corps (X_droit_corps * a + b) donne un résultat constant
// PAR RAPPORT AU CORPS mais pas par rapport au sol, dont la marge, elle, ne suit
// pas le zoom.
//
// Conclusion : la profondeur ne permet pas les deux à la fois. Décision de
// l'utilisateur — garder le défaut natif de dos plutôt qu'un sprite qui
// disparaît ou se déforme. NE PAS retenter par ce chemin sans une idée neuve sur
// ce qui trie réellement (l'état de rendu passé à RenderQueue_InsertPrimitive
// n'a jamais été instrumenté).
//
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
// 0=none, 1=MID, 2=TOP, 3=OFF-HAND (per call)
static unsigned char g_weapon_top_force = 0;
static void* g_tramp_defer  = nullptr;         // -> relocated prologue + body
static void* g_tramp_key    = nullptr;         // -> relocated key writes + rest

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
// head-gears), 2 = TOP key (above the weapon), 3 = OFF-HAND. Front-facing only —
// la vue de dos est laissée au natif (cf. l'en-tête).
void __fastcall WeaponDecide(void* helper, int partIdx) {
  g_weapon_top_force = 0;
  __try {
    char* cas = *reinterpret_cast<char**>(reinterpret_cast<char*>(helper) + 0x3c);
    if (cas) {
      // facing = CActorSprite+0x38 - +0x34 (the value Actor_SelectPartLayerByDir
      // 0x00d38850 switches on). FRONT group {0,1,6,7}; BACK group {2,3,4,5}.
      const int facing = *reinterpret_cast<int*>(cas + 0x38) -
                         *reinterpret_cast<int*>(cas + 0x34);
      const bool front = (facing == 0 || facing == 1 || facing == 6 ||
                          facing == 7);
      if (!front) {
        // BACK view: laissée au NATIF, décision prise le 2026-08-03 après
        // mesure. Cf. la note en tête de fichier — le bouclier y passe devant le
        // corps, et on ne peut pas l'en déloger sans le heurter au sol.
      } else if (partIdx == 5) {
        g_weapon_top_force = 1;                      // weapon -> above head-gears
      } else if (partIdx == 7) {
        g_weapon_top_force = 2;                      // shield -> above the weapon
      } else if (partIdx == 6 && OffhandWeaponActive()) {
        g_weapon_top_force = 3;                      // off-hand -> weapon level
      }
    }
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
// constant that places the part where the table wants it: 1 = MID (above the
// head-gears, the weapon), 2 = TOP (above the weapon, the shield), 3 = OFF-HAND.
// EAX/ESI are live here (node ptr / tree link) — only EDI + flags may be
// touched.
//
// 🔴 DeferQuadSorted DROPS any quad whose key collides with one already in the
// tree — d'où les constantes espacées (…00 / …08 / …10).
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
