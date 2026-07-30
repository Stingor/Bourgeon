#include "ragnarok/player_skills.h"

#include <Windows.h>

#include <cstdint>

namespace rag {
namespace {

// Adresses et offsets du bundle de compétences (20250716, base 0x400000).
constexpr uintptr_t kSkillBundle     = 0x015fa3cc;  // CPlayerSkillBundle (session+0x0C)
constexpr uintptr_t kSkillFlatList   = 0x015fa3e0;  // bundle+0x14 : onglet « divers »
constexpr uintptr_t kSkillGetTabList = 0x00738370;  // __thiscall(bundle, tab) -> std::list*
using GetTabList_t = void* (__fastcall*)(void*, void*, int);

constexpr int kNodeValue  = 0x08;
constexpr int kOffValid   = 0x04;
constexpr int kOffId      = 0x08;
constexpr int kOffLvLocal = 0x10;
constexpr int kOffLearned = 0x30;  // int16, VÉRITÉ SERVEUR
constexpr int kJobTabs    = 4;
constexpr int kMaxNodes   = 256;

}  // namespace

int LearnedSkillLevel(int skill_id) {
  if (skill_id <= 0) return 0;
  int found = 0;
  __try {
    // Les cinq listes du bundle : quatre onglets de job + la liste plate. Une
    // compétence peut n'apparaître que dans l'une d'elles selon la classe, d'où le
    // balayage complet plutôt qu'un seul onglet « probable ».
    for (int tab = -1; tab < kJobTabs && !found; ++tab) {
      uint8_t* list_obj = reinterpret_cast<uint8_t*>(kSkillFlatList);
      if (tab >= 0)
        list_obj = reinterpret_cast<uint8_t*>(reinterpret_cast<GetTabList_t>(
            kSkillGetTabList)(reinterpret_cast<void*>(kSkillBundle), nullptr, tab));
      if (!list_obj) continue;
      uint8_t* head = *reinterpret_cast<uint8_t**>(list_obj);
      if (!head) continue;
      uint8_t* node = *reinterpret_cast<uint8_t**>(head);
      int guard = 0;
      while (node && node != head && guard++ < kMaxNodes) {
        const uint8_t* value = node + kNodeValue;
        node = *reinterpret_cast<uint8_t**>(node);  // avancer AVANT de lire
        if (*reinterpret_cast<const int*>(value + kOffValid) == 0) continue;
        if (*reinterpret_cast<const int*>(value + kOffId) != skill_id) continue;
        int level = *reinterpret_cast<const int16_t*>(value + kOffLearned);
        if (level <= 0) level = *reinterpret_cast<const int*>(value + kOffLvLocal);
        found = level;
        break;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return found;
}

}  // namespace rag
