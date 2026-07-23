#include "plugins/player_jump.h"

#include <Windows.h>

#include <cstdint>

#include "bourgeon.h"

// ── Adresses (client 20250716, no-ASLR : addr Ghidra == live) ────────────────
namespace {
constexpr uintptr_t kGameModeGet   = 0x00a75340;  // GameMode_GetActive(mgr)
constexpr uintptr_t kModeMgr       = 0x1213338;   // arg (CModeMgr)
constexpr uintptr_t kTerrainHeight = 0x007110c0;  // Terrain_GetHeightAt(world,x,z)->float
constexpr int kOffActorMgr  = 0xcc;   // CMode    -> actorMgr
constexpr int kOffOwnActor  = 0x2c;   // actorMgr -> acteur joueur
constexpr int kOffWorld     = 0x30;   // actorMgr -> objet monde/terrain (.gnd)
constexpr int kOffPosX      = 0x10;   // acteur -> position monde X (float)
constexpr int kOffPosY      = 0x14;   // acteur -> position monde Y = HAUTEUR (float)
constexpr int kOffPosZ      = 0x18;   // acteur -> position monde Z (float)
constexpr int kOffHeightOff = 0x3f4;  // acteur -> offset hauteur (float ; vec3 +0x3f0/f4/f8)

using TerrainHeightFn = float(__thiscall*)(void*, float, float);

// Acteur joueur + objet monde, ou nullptr hors-jeu. SEH-gardé : la chaîne de
// pointeurs n'est valide qu'une fois dans le monde. (Même chaîne que basic_info
// GetOwnActorLive, plus le monde pour échantillonner le terrain.)
struct ActorRefs {
  void* actor = nullptr;
  void* world = nullptr;
};

ActorRefs GetOwnActorRefs() {
  ActorRefs refs;
  __try {
    void* gm = reinterpret_cast<void*(__fastcall*)(int)>(kGameModeGet)(
        static_cast<int>(kModeMgr));
    if (gm) {
      void* mgr =
          *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) + kOffActorMgr);
      if (mgr) {
        refs.actor =
            *reinterpret_cast<void**>(reinterpret_cast<char*>(mgr) + kOffOwnActor);
        refs.world =
            *reinterpret_cast<void**>(reinterpret_cast<char*>(mgr) + kOffWorld);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    refs.actor = nullptr;
    refs.world = nullptr;
  }
  return refs;
}

// Pose la hauteur de saut sur l'acteur. offY = 0 -> retour au sol.
//
// Il FAUT les deux écritures, parce que le moteur ne recalcule la position que
// par intermittence (cf. CActorSprite_UpdateMotionAndPosition 0x00c47700 : le bloc
// LAB_00c47c9c n'est atteint que si local_10 != 0, donc UNIQUEMENT en marche
// (case 1) et knockback (case 4) — à l'ARRÊT, case 0/STAND sort par LAB_00c47892
// sans jamais retoucher +0x10/+0x14/+0x18) :
//   1. +0x3f4 = l'offset que le moteur applique lui-même *quand* il recalcule
//      (pos.y = terrain + offset). C'est ce qui rend le saut correct en marchant,
//      y compris sur terrain accidenté.
//   2. +0x14 écrit à la main = le MÊME calcul refait par nous, indispensable à
//      l'arrêt (sinon la position reste figée sur sa dernière valeur : le perso
//      restait bloqué en l'air en fin de saut immobile).
// Les deux produisent la valeur IDENTIQUE (terrain + offY), donc quel que soit
// l'ordre moteur/plugin dans la frame il n'y a ni conflit ni scintillement.
//
// ⚠ Ne JAMAIS écrire +0x3f0 / +0x3f8 (offsets X/Z) : le moteur les ACCUMULE
// (pos.x = pos.x + offset chaque frame) -> dérive. Seul +0x3f4 est non-cumulatif,
// car pos.y est repris du terrain avant l'addition.
void ApplyJumpHeight(const ActorRefs& refs, float offY) {
  if (!refs.actor) return;
  __try {
    char* a = reinterpret_cast<char*>(refs.actor);
    *reinterpret_cast<float*>(a + kOffHeightOff) = offY;
    if (refs.world) {
      const float x = *reinterpret_cast<float*>(a + kOffPosX);
      const float z = *reinterpret_cast<float*>(a + kOffPosZ);
      const float ground =
          reinterpret_cast<TerrainHeightFn>(kTerrainHeight)(refs.world, x, z);
      *reinterpret_cast<float*>(a + kOffPosY) = ground + offY;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
}  // namespace

void PlayerJumpTweaks::SetEnabled(bool on) {
  if (on == enabled_) return;
  enabled_ = on;
  if (!enabled_) ResetOffset();  // coupe un saut en cours et remet le sprite au sol
}

void PlayerJumpTweaks::ResetOffset() {
  jumping_ = false;
  ApplyJumpHeight(GetOwnActorRefs(), 0.0f);
}

void PlayerJumpTweaks::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  // L'acteur (et donc son offset) est détruit/recréé au changement de map ; on
  // oublie simplement l'état de saut. Le nouvel acteur naît avec +0x3f4 = 0.
  if (mode_type != ModeMgr::ModeType::kGame) jumping_ = false;
}

void PlayerJumpTweaks::OnKeyDown(unsigned long vkey, int, int) {
  if (!enabled_) return;
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  if (vkey != VK_SPACE) return;
  if (jumping_) return;  // déjà en l'air : on ignore jusqu'à l'atterrissage
  if (!Bourgeon::Instance().IsGameActive()) return;
  if (Bourgeon::Instance().IsMapLoading()) return;
  jumping_ = true;
  jump_start_ms_ = GetTickCount();
}

void PlayerJumpTweaks::OnRenderUI() {
  // OnRenderUI n'est dispatché qu'en jeu et jamais pendant un chargement de map,
  // donc pas de garde supplémentaire nécessaire ici.
  if (!jumping_) return;

  const uint32_t dur = jump_ms_ > 50 ? static_cast<uint32_t>(jump_ms_) : 50u;
  const uint32_t elapsed = GetTickCount() - jump_start_ms_;

  const ActorRefs refs = GetOwnActorRefs();
  if (!refs.actor) {  // acteur parti (warp/déco) : on arrête sans rien écrire
    jumping_ = false;
    return;
  }
  if (elapsed >= dur) {  // atterri : on repose le sprite au sol
    ApplyJumpHeight(refs, 0.0f);
    jumping_ = false;
    return;
  }

  const float t = static_cast<float>(elapsed) / static_cast<float>(dur);  // 0..1
  const float arc = 4.0f * t * (1.0f - t);  // parabole : 0 -> 1 (t=0.5) -> 0
  // Y pointe vers le bas -> monter = offset NÉGATIF.
  ApplyJumpHeight(refs, -jump_height_ * arc);
}
