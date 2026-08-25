#include "ragnarok/globals.h"
#include "features/gameplay/player_jump.h"
#include "ragnarok/actor.h"  // rag::actor : les offsets de CActorSprite

#include "ragnarok/game_scene.h"
#include "ragnarok/own_actor.h"  // rag::OwnActor / rag::OwnActorOf
#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "bourgeon.h"
#include "imgui.h"
#include "features/systems/bourgeon_opcodes.h"
#include "features/hotkey_util.h"  // capture d'un combo en cours (remappage)

// ── Adresses (client 20250716, no-ASLR : addr Ghidra == live) ────────────────
namespace {
constexpr uintptr_t kTerrainHeight = 0x007110c0;  // Terrain_GetHeightAt(world,x,z)->float
constexpr int kOffWorld     = 0x30;   // actorMgr -> objet monde/terrain (.gnd)
constexpr int kOffPosZ      = 0x18;   // acteur -> position monde Z (float)
constexpr int kOffHeightOff = 0x3f4;  // acteur -> offset hauteur (float ; vec3 +0x3f0/f4/f8)

using TerrainHeightFn = float(__thiscall*)(void*, float, float);
using FindByGidFn     = void*(__thiscall*)(void*, uint32_t);

// actorMgr + objet monde, ou nullptr hors-jeu. SEH-gardé : la chaîne de
// pointeurs n'est valide qu'une fois dans le monde.
struct WorldRefs {
  void* actor_mgr = nullptr;
  void* world     = nullptr;
};

WorldRefs GetWorldRefs() {
  WorldRefs refs;
  __try {
    void* gm = rag::ActiveModeIfReady();
    if (gm) {
      void* mgr =
          *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) + gamescene::kGmActorMgr);
      if (mgr) {
        refs.actor_mgr = mgr;
        refs.world =
            *reinterpret_cast<void**>(reinterpret_cast<char*>(mgr) + kOffWorld);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    refs.actor_mgr = nullptr;
    refs.world     = nullptr;
  }
  return refs;
}

// Acteur cible d'un saut : gid == 0 => joueur local (*(actorMgr+0x2c)), sinon
// recherche dans la liste d'acteurs. nullptr si l'acteur n'est pas/plus en vue
// (le joueur distant a pu sortir de l'écran pendant son saut).
void* ResolveActor(const WorldRefs& refs, uint32_t gid) {
  if (!refs.actor_mgr) return nullptr;
  void* actor = nullptr;
  __try {
    if (gid == 0) {
      actor = *reinterpret_cast<void**>(
          reinterpret_cast<char*>(refs.actor_mgr) + gamescene::kAmOwnPlayer);
    } else {
      actor = reinterpret_cast<FindByGidFn>(gamescene::kActorListFindByGidAddr)(refs.actor_mgr, gid);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    actor = nullptr;
  }
  return actor;
}

// Pose la hauteur de saut sur l'acteur. offY = 0 -> retour au sol.
//
// Il FAUT les deux écritures, parce que le moteur ne recalcule la position que
// par intermittence (cf. le PIÈGE documenté dans player_jump.h) :
//   1. +0x3f4 = l'offset que le moteur applique lui-même *quand* il recalcule
//      (pos.y = terrain + offset) -> saut correct en marchant, y compris en
//      terrain accidenté.
//   2. +0x14 écrit à la main = le MÊME calcul refait par nous, indispensable à
//      l'arrêt (sinon la position reste figée : perso bloqué en l'air).
// Les deux produisent la valeur IDENTIQUE (terrain + offY), donc quel que soit
// l'ordre moteur/plugin dans la frame il n'y a ni conflit ni scintillement.
//
// ⚠ Ne JAMAIS écrire +0x3f0 / +0x3f8 (offsets X/Z) : le moteur les ACCUMULE
// (pos.x = pos.x + offset chaque frame) -> dérive. Seul +0x3f4 est non-cumulatif,
// car pos.y est repris du terrain avant l'addition.
void ApplyJumpHeight(void* actor, void* world, float offY) {
  if (!actor) return;
  __try {
    char* a = reinterpret_cast<char*>(actor);
    *reinterpret_cast<float*>(a + kOffHeightOff) = offY;
    if (world) {
      const float x = *reinterpret_cast<float*>(a + rag::actor::kPosX);
      const float z = *reinterpret_cast<float*>(a + kOffPosZ);
      const float ground =
          reinterpret_cast<TerrainHeightFn>(kTerrainHeight)(world, x, z);
      *reinterpret_cast<float*>(a + rag::actor::kPosY) = ground + offY;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// La garde « une saisie native a le focus » vit dans hotkey_util : elle vaut pour
// TOUT raccourci global, et en garder une copie par module, c'est se condamner à
// les corriger une par une le jour où un offset bouge.
}  // namespace

PlayerJump::PlayerJump() {
  // Saut d'un autre joueur (ZC 0x0F1B) : zone custom sûre (>0x0C35) => livré par
  // le reader-hook de RagConnection. cf. bourgeon_opcodes.h.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kJumpNotify);
}

void PlayerJump::SetEnabled(bool on) {
  if (on == enabled_) return;
  enabled_ = on;
  if (!enabled_) ClearAll();  // coupe les sauts en cours et repose les sprites
}

void PlayerJump::ClearAll() {
  const WorldRefs refs = GetWorldRefs();
  for (const Jump& j : jumps_)
    ApplyJumpHeight(ResolveActor(refs, j.gid), refs.world, 0.0f);
  jumps_.clear();
}

void PlayerJump::StartJump(uint32_t gid) {
  for (const Jump& j : jumps_)
    if (j.gid == gid) return;  // déjà en l'air : on laisse l'arc en cours finir
  jumps_.push_back({gid, GetTickCount()});
}

void PlayerJump::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  // Les acteurs (et donc leurs offsets) sont détruits/recréés au changement de
  // map ; on oublie simplement l'état. Les nouveaux acteurs naissent à +0x3f4=0.
  if (mode_type != ModeMgr::ModeType::kGame) jumps_.clear();
}

void PlayerJump::OnKeyDown(unsigned long vkey, int, int) {
  if (!enabled_) return;
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  if (key_vk_ == 0 || static_cast<int>(vkey) != key_vk_) return;
  // Modificateurs : ProcessPushButton ne transmet que la touche principale, on
  // lit donc l'état clavier du message en cours (GetKeyState, pas Async).
  const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  if (ctrl != key_ctrl_ || alt != key_alt_ || shift != key_shift_) return;
  // Un remappage est en cours (ici ou dans la fiche de perso) : la touche sert à
  // choisir le raccourci, pas à sauter. On interroge le drapeau PARTAGÉ, qui
  // expire tout seul, et non key_capturing_ : un panneau replié en pleine
  // capture laisse ce dernier armé et tuerait le saut jusqu'au redémarrage.
  if (hotkeys::CaptureInProgress()) return;
  if (!Bourgeon::Instance().IsGameActive()) return;
  if (Bourgeon::Instance().IsMapLoading()) return;

  // Le clavier appartient à une saisie ImGui ou à une textbox native (chat,
  // message privé, montant de vente…) : la touche est un caractère, pas un saut.
  if (ImGui::GetCurrentContext() != nullptr &&
      ImGui::GetIO().WantCaptureKeyboard)
    return;
  if (hotkeys::NativeTextInputHasFocus()) return;

  bool already = false;
  for (const Jump& j : jumps_)
    if (j.gid == 0) already = true;
  if (already) return;  // déjà en l'air : pas de relance ni d'envoi réseau

  StartJump(0);  // 0 = joueur local

  // Prévient le serveur, qui rediffusera aux autres clients Bourgeon de la zone
  // (avec son propre cooldown anti-flood — la garde ci-dessus est du confort,
  // pas une sécurité : un client modifié la retirerait).
  const uint16_t op  = bopcodes::kJump;
  const uint16_t len = 4;  // [type:2][len:2], aucun payload : le serveur sait qui parle
  uint8_t pkt[4];
  std::memcpy(pkt + 0, &op, 2);
  std::memcpy(pkt + 2, &len, 2);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// Fil RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h).
void PlayerJump::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                              uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

// Fil PRINCIPAL : le décodage, rejoué à chaque frame, dans l'ordre d'arrivée.
void PlayerJump::HandlePacket(uint16_t opcode, const uint8_t* data,
                              uint16_t len) {
  if (opcode != bopcodes::kJumpNotify) return;
  if (!enabled_ || data == nullptr || len < 4) return;
  // data = octets APRÈS [type:2][len:2] -> [gid:4].
  uint32_t gid = 0;
  std::memcpy(&gid, data, 4);
  if (gid == 0) return;

  // Garde défensive : le serveur diffuse déjà « sans self », mais si notre propre
  // GID nous revenait un jour, on aurait DEUX sauts (gid 0 et notre AID) écrivant
  // sur le MÊME acteur — donc deux offsets qui se battent. On compare les acteurs
  // résolus plutôt que les GID, ce qui évite d'avoir à lire notre propre AID.
  const WorldRefs refs = GetWorldRefs();
  void* remote = ResolveActor(refs, gid);
  if (remote == nullptr) return;                       // pas (ou plus) en vue
  if (remote == ResolveActor(refs, 0)) return;         // c'est nous : déjà animé

  StartJump(gid);
}

void PlayerJump::OnRenderUI() {
  // OnRenderUI n'est dispatché qu'en jeu et jamais pendant un chargement de map,
  // donc pas de garde supplémentaire nécessaire ici.
  if (jumps_.empty()) return;

  const uint32_t dur  = jump_ms_ > 50 ? static_cast<uint32_t>(jump_ms_) : 50u;
  const uint32_t now  = GetTickCount();
  const WorldRefs refs = GetWorldRefs();
  if (!refs.actor_mgr) {  // hors-jeu : on oublie tout sans rien écrire
    jumps_.clear();
    return;
  }

  for (size_t i = 0; i < jumps_.size();) {
    const Jump& j = jumps_[i];
    const uint32_t elapsed = now - j.start_ms;
    void* actor = ResolveActor(refs, j.gid);

    // Acteur parti (warp, hors de vue, déco) : on abandonne son saut. Rien à
    // reposer — l'acteur n'existe plus, et s'il revient il naîtra à +0x3f4=0.
    if (!actor) {
      jumps_.erase(jumps_.begin() + i);
      continue;
    }
    if (elapsed >= dur) {  // atterri : on repose le sprite au sol
      ApplyJumpHeight(actor, refs.world, 0.0f);
      jumps_.erase(jumps_.begin() + i);
      continue;
    }

    const float t = static_cast<float>(elapsed) / static_cast<float>(dur);  // 0..1
    const float arc = 4.0f * t * (1.0f - t);  // parabole : 0 -> 1 (t=0.5) -> 0
    // Y pointe vers le bas -> monter = offset NÉGATIF.
    ApplyJumpHeight(actor, refs.world, -jump_height_ * arc);
    ++i;
  }
}
