#include "ragnarok/skill_cooldowns.h"

#include <Windows.h>
#include <mmsystem.h>  // timeGetTime (winmm, déjà lié par la cible)

#include <vector>

#include "bourgeon.h"
#include "ragnarok/globals.h"  // rag::kReplayActiveAddr

namespace ro {
namespace {

// ZC_SKILL_POSTDELAY {skillId.W, durée.L} — 6 octets après l'opcode. Le seul
// paquet de la famille que le serveur émette (rAthena n'a pas d'équivalent au
// 0x043E « liste » que le client sait pourtant lire, cf. 0x00cd6cb0).
constexpr uint16_t kOpSkillCooldown  = 0x043d;
constexpr uint16_t kSkillCooldownLen = 6;

// Horloge du jeu, choisie exactement comme le handler natif : en relecture de
// replay le temps vient du gestionnaire de réassemblage, sinon de timeGetTime.
// Comparer une échéance posée sur une horloge avec un « maintenant » lu sur
// l'autre donnerait des cooldowns fantômes de plusieurs heures.
constexpr uintptr_t kReplayClock   = 0x00b1fac0;  // CReassemblyPacketMgr_GetInstance
constexpr int       kReplayClockMs = 0x20;        // instance+0x20 = tick courant

// Liste native, utilisée en REPLI seul (voir le pourquoi dans le header).
constexpr uintptr_t kNativeList = 0x015ff7e0;  // g_ShortCutCooldownList (objet std::list)
struct NativeNode {        // nœud de 0x24 octets
  NativeNode* next;        // +0x00
  NativeNode* prev;        // +0x04
  uint32_t    skill_id;    // +0x08
  uint32_t    end_tick;    // +0x0C = horloge + durée
  uint32_t    duration;    // +0x10 (ms)
  // +0x14.. : listes d'animation de balayage, sans intérêt ici.
};

struct Cooldown {
  uint16_t      skill_id = 0;
  unsigned long end_tick = 0;  // échéance sur l'horloge de GameClockMs()
  unsigned long duration = 0;  // durée annoncée, pour la fraction affichée
};
std::vector<Cooldown> g_cooldowns;

using ReplayClock_t = void*(__cdecl*)();

unsigned long GameClockMs() {
  if (*reinterpret_cast<int*>(rag::kReplayActiveAddr) == 0) return timeGetTime();
  void* mgr = reinterpret_cast<ReplayClock_t>(kReplayClock)();
  return mgr ? *reinterpret_cast<unsigned long*>(reinterpret_cast<uint8_t*>(mgr) +
                                                 kReplayClockMs)
             : timeGetTime();
}

// Soustraction NON signée, jamais `end > now` : l'horloge reboucle au bout de
// 49,7 jours et une comparaison directe figerait alors un cooldown éternel.
unsigned long RemainingFrom(unsigned long end_tick, unsigned long now) {
  const unsigned long left = end_tick - now;
  return left > 0x7fffffffUL ? 0 : left;
}

// Parcours de la liste native. Isolé dans sa propre fonction : le SEH interdit
// les objets à destructeur dans la fonction qui le porte.
//
// ⚠ 0x015ff7e0 est l'OBJET std::list, pas la sentinelle : la sentinelle (_Myhead)
// est *(0x015ff7e0), et c'est contre ELLE que la fin de boucle se teste. Comparer
// contre 0x015ff7e0 boucle à l'infini (freeze client). Garde d'itérations + SEH
// par-dessus, la liste appartenant à du code que l'on ne contrôle pas.
float NativeFraction(uint32_t skill_id, unsigned long now) {
  __try {
    NativeNode* head = *reinterpret_cast<NativeNode**>(kNativeList);  // _Myhead
    if (!head) return 0.0f;
    int guard = 0;
    for (NativeNode* node = head->next; node && node != head && guard < 256;
         node = node->next, ++guard) {
      if (node->skill_id != skill_id) continue;
      if (node->duration == 0) return 0.0f;
      const unsigned long left = RemainingFrom(node->end_tick, now);
      if (left == 0) return 0.0f;
      const unsigned long clamped = left > node->duration ? node->duration : left;
      return static_cast<float>(clamped) / static_cast<float>(node->duration);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return 0.0f;
}

// Range une échéance ; `ms` == 0 purge l'entrée (le serveur annonce ainsi une
// compétence redevenue prête).
void SetCooldown(uint16_t skill_id, uint32_t ms) {
  const unsigned long end = GameClockMs() + ms;
  for (Cooldown& cooldown : g_cooldowns) {
    if (cooldown.skill_id != skill_id) continue;
    cooldown.end_tick = end;  // ms == 0 -> échéance déjà atteinte, donc « prête »
    cooldown.duration = ms;
    return;
  }
  if (ms != 0) g_cooldowns.push_back({skill_id, end, ms});
}

}  // namespace

void InstallSkillCooldowns() {
  Bourgeon::Instance().RegisterObserveOpcode(kOpSkillCooldown, kSkillCooldownLen);
}

void FeedSkillCooldownPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  if (opcode != kOpSkillCooldown || len < kSkillCooldownLen) return;
  SetCooldown(*reinterpret_cast<const uint16_t*>(data),
              *reinterpret_cast<const uint32_t*>(data + 2));
}

void ClearSkillCooldowns() { g_cooldowns.clear(); }

unsigned long SkillCooldownRemainingMs(uint16_t skill_id) {
  const unsigned long now = GameClockMs();
  for (const Cooldown& cooldown : g_cooldowns) {
    if (cooldown.skill_id != skill_id) continue;
    return RemainingFrom(cooldown.end_tick, now);
  }
  return 0;
}

float SkillCooldownFraction(uint16_t skill_id) {
  const unsigned long now = GameClockMs();
  for (const Cooldown& cooldown : g_cooldowns) {
    if (cooldown.skill_id != skill_id) continue;
    if (cooldown.duration == 0) return 0.0f;
    const unsigned long left = RemainingFrom(cooldown.end_tick, now);
    if (left == 0) return 0.0f;
    const unsigned long clamped =
        left > cooldown.duration ? cooldown.duration : left;
    return static_cast<float>(clamped) / static_cast<float>(cooldown.duration);
  }
  return NativeFraction(skill_id, now);
}

}  // namespace ro
