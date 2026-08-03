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
// The order we enforce is the one the doll composer draws (src/ui/doll.cc), so
// that the world and the character sheet agree:
//
//   BACK  {2,3,4,5} : shield · BODY · head · head-gears · weapon · garment
//   FRONT {0,1,6,7} : BODY · head · head-gears · garment · weapon · shield
//
// FRONT: at the branch-1 key write (0x006049e4) force a large key so the order
// becomes head-gears/garment < weapon (part 5 -> 0x40000000) < shield (part 7 ->
// 0x40000010). Both beat any lua priority, which tops out in the hundreds.
//
// BACK: the weapon is already right natively — it sits under the head-gears,
// which is where the table wants it — so it is left alone. The SHIELD is not:
// it must sink below the BODY itself, since the character carries it on the far
// side and its own back hides it.
//
// 🔴 Do NOT expect the emission order to deliver that on its own. Reading
// Actor_SelectPartLayerByDir, part 7 comes out FIRST in the back group, which
// ought to put the shield at the very bottom — measured in game, it still draws
// ABOVE the body. Something between emission and flush lifts it, and until that
// is pinned down the only trustworthy statement is the observed one. Hence a
// forced key rather than a reliance on native order.
//
// The branch-1 key is a plain insertion counter compared as a SIGNED int, so a
// negative key (0x80000001) sorts below every natural key, body included.
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
constexpr uintptr_t kSubmitQuad = 0x00c4a0d0;  // Actor_SubmitQuad_RenderQueue

// 🔴 NE PAS hooker AUSSI Actor_SubmitQuad_VerticalFlip (0x00c4a670).
//
// C'est bien l'autre branche du flush — `RenderLayered` la prend quand
// `Scene_IsVerticalFlipMode()` (0x00d72d10) vaut 1 — mais elle ne soumet rien
// elle-même : elle retourne les coordonnées, pose l'angle à 180 et DÉLÈGUE à
// `Actor_SubmitQuad_RenderQueue` avec le MÊME `param_1`. Le détour ci-dessous
// la couvre donc déjà.
//
// Le hooker en plus applique l'abaissement DEUX FOIS sur le même quad : -0,0006
// au lieu de -0,0003, et le bouclier disparaît derrière le décor. Mesuré en jeu
// le 2026-08-03, après l'avoir posé « pour être complet ».

// 🔴 Ce que le tri regarde vraiment — mesuré, pas déduit. L'ordre de l'arbre
// n'est PAS l'ordre de dessin.
//
// Le flush de RenderLayered ne dessine pas : il SOUMET, dans l'ordre des clés,
// avec une profondeur. Juste avant, chaque noeud reçoit
//     node[9]  = node[7] * helper[41] + helper[42]   (node[7] = X droit du quad)
//     node[10] = node[5] * helper[41] + helper[42]   (node[5] = X gauche)
// puis Actor_SubmitQuad_RenderQueue écrit dans le vertex
//     z   = node[9] + rang * 0.000001
//     rhw = node[9] + rang * 0.0002
//
// La file RETRIE ensuite, par z CROISSANT : petit z = dessiné en premier = au
// fond. Le rang ne pèse que 1e-6 là où les écarts entre pièces valent ~2e-4 —
// il départage, il n'ordonne pas. Ce qui ordonne, c'est le X DROIT du quad,
// avec helper[41] NÉGATIF (~-2.6e-6) : plus une pièce s'étend vers la droite,
// plus elle est au fond.
//
// ⚠ Ne PAS comparer les intervalles [node[9], node[10]] de deux pièces : `a` et
// `b` sont communs à tout l'acteur, donc à un X donné toutes les pièces ont la
// même profondeur. Les plages ne diffèrent que parce que chaque pièce couvre
// une portion d'écran différente. C'est le X droit, seul, qui décide.
//
// Combien abaisser — calculé, pas tâtonné.
//
// Relevé à la soumission, sur un Novice de dos (les clés identifient les
// pièces : 0x2 corps, 0x4 tête, 0x7 arme, 0xab/0xc8 coiffes par priorité lua,
// 0x190 cape) :
//     corps    0.00251     au fond
//     cape     0.00255
//     arme     0.00261     devant le corps
//     coiffe   0.00267
//     BOUCLIER 0.00269     devant le corps  <- le défaut
//     coiffe   0.00277     recouvre le bouclier
//
// L'écart à combler est corps - bouclier = 1.8e-4, et il vient entièrement de
// la largeur : le corps s'étend jusqu'à X 510.8, le bouclier s'arrête à 446.1,
// et 64.7 px valent 1.7e-4 de profondeur. Le bouclier est plus étroit, donc
// mécaniquement moins profond, donc devant.
//
// 3e-4 le passe sous le corps avec ~40 % de marge, tout en restant bien
// au-dessus du décor — 🔴 mesuré : à 5e-4 le bouclier disparaît complètement,
// il passe DERRIÈRE le sol. Cette profondeur est celle de la scène entière, pas
// un rang local à l'acteur.
constexpr float kShieldDepthSink = 0.0003f;

// La clé forcée ne trie plus rien — elle ÉTIQUETTE. Le noeud porte sa clé à
// node+16, et le flush passe node+20 comme param_1 : `param_1[-1]` rend donc
// l'étiquette au moment de la soumission, seul endroit où la profondeur existe.
constexpr int kShieldTag = static_cast<int>(0x80000001);
}  // namespace

// File scope (NOT namespaced) so the naked __asm can resolve them by name.
// 0=none, 1=MID, 2=TOP, 3=OFF-HAND, 4=BOTTOM (per call)
static unsigned char g_weapon_top_force = 0;
static void* g_tramp_defer  = nullptr;         // -> relocated prologue + body
static void* g_tramp_key    = nullptr;         // -> relocated key writes + rest
static void* g_tramp_submit = nullptr;         // -> relocated submit prologue

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
// head-gears), 2 = TOP key (above the weapon), 4 = BOTTOM = l'étiquette du
// bouclier de dos, relue à la soumission par SinkTaggedQuad.
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
        // BACK view: the shield must be drawn FIRST — below the body, which
        // then covers it. It natively draws above instead, and no key can fix
        // that (cf. l'en-tête) — so this code only TAGS it, and the real work
        // happens in SinkTaggedQuad.
        //
        // The weapon is deliberately left alone here: under the head-gears is
        // exactly where the table wants it in this view.
        if (partIdx == 7) g_weapon_top_force = 4;    // shield -> tag
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
// head-gears, the weapon), 2 = TOP (above the weapon, the shield), 3 = OFF-HAND,
// 4 = BOTTOM (below the body, the shield seen from behind). EAX/ESI are live
// here (node ptr / tree link) — only EDI + flags may be touched.
//
// 🔴 The tree compares keys as SIGNED ints (`v11 < *(int*)(node+16)` at
// 0x00604878), which is what makes 0x80000001 a floor rather than a ceiling. It
// also DROPS any quad whose key collides with one already in the tree, hence the
// spaced-out constants.
__declspec(naked) static void KeyWriteStub() {
  __asm {
    cmp  byte ptr g_weapon_top_force, 0
    je   sk_done
    cmp  byte ptr g_weapon_top_force, 1
    je   sk_mid
    cmp  byte ptr g_weapon_top_force, 3
    je   sk_off
    cmp  byte ptr g_weapon_top_force, 4
    je   sk_bottom
    mov  edi, 0x40000010        // TOP: the shield, above the weapon
    jmp  sk_done
  sk_bottom:
    mov  edi, 0x80000001        // BOTTOM: the shield seen from behind, under the body
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

// Abaisse la profondeur du quad ÉTIQUETÉ, juste avant sa soumission. C'est la
// seule prise possible : la profondeur n'existe qu'ici, et le tri de la file de
// rendu ne connaît qu'elle.
//
// `param_1` pointe sur node+20 ; `param_1[4]` et `param_1[5]` sont les deux
// bornes de profondeur que RenderLayered vient de calculer, et `param_1[-1]`
// est la clé du noeud, donc notre étiquette.
//
// ⚠ Les noeuds sont recréés à chaque frame (operator_new dans DeferQuadSorted)
// et leurs profondeurs recalculées avant chaque soumission : écrire en place ne
// s'accumule pas d'une frame à l'autre.
//
// 🔴 __try obligatoire : cette fonction est appelée pour TOUS les quads
// d'acteur, y compris ceux qui ne viennent pas d'un noeud d'arbre — lire
// `param_1[-1]` sur ceux-là n'a pas de sens et peut sortir de la page.
//
// Les DEUX branches du flush finissent ici : la branche « flip vertical »
// délègue à celle-ci (cf. la note en tête de fichier). Un seul détour suffit —
// en poser un second abaisserait deux fois.
void __fastcall SinkTaggedQuad(float* quad) {
  __try {
    if (!quad) return;
    if (reinterpret_cast<const int*>(quad)[-1] != kShieldTag) return;
    quad[4] -= kShieldDepthSink;
    quad[5] -= kShieldDepthSink;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Naked stub sur Actor_SubmitQuad_RenderQueue (__stdcall, 6 args) : param_1 est
// à [esp+4] avant nos push.
__declspec(naked) static void SubmitQuadStub() {
  __asm {
    push eax
    push ecx
    push edx
    mov  ecx, [esp+0x10]       // param_1 ([esp]=edx,+4=ecx,+8=eax,+0xc=ret,+0x10=param_1)
    call SinkTaggedQuad        // __fastcall(ecx=param_1)
    pop  edx
    pop  ecx
    pop  eax
    jmp  [g_tramp_submit]
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
  g_tramp_submit = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kSubmitQuad),
      reinterpret_cast<uint8_t*>(&SubmitQuadStub));
  // LogInfo("[WeaponLayer] z-order hooks installed (defer={} key={})",
          // g_tramp_defer != nullptr, g_tramp_key != nullptr);
}
