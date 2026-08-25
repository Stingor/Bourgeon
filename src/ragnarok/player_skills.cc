#include "ragnarok/player_skills.h"
#include "ragnarok/stl_node.h"  // rag::listnode
#include "ragnarok/skill_info.h"  // rag::skillinfo

#include <Windows.h>

#include <cstdint>

namespace rag {
namespace {

// Adresses et offsets du bundle de compétences (20250716, base 0x400000).
using GetTabList_t = void* (__fastcall*)(void*, void*, int);

constexpr int kJobTabs    = 4;
constexpr int kMaxNodes   = 256;

}  // namespace

int LearnedSkillLevel(int skill_id, int* sp_cost) {
  if (sp_cost) *sp_cost = 0;
  if (skill_id <= 0) return 0;
  int found = 0;
  int sp = 0;
  __try {
    // Les cinq listes du bundle : quatre onglets de job + la liste plate. Une
    // compétence peut n'apparaître que dans l'une d'elles selon la classe, d'où le
    // balayage complet plutôt qu'un seul onglet « probable ».
    for (int tab = -1; tab < kJobTabs && !found; ++tab) {
      uint8_t* list_obj = reinterpret_cast<uint8_t*>(rag::kSkillFlatListAddr);
      if (tab >= 0)
        list_obj = reinterpret_cast<uint8_t*>(reinterpret_cast<GetTabList_t>(
            rag::kSkillGetTabListAddr)(reinterpret_cast<void*>(rag::kSkillBundleAddr), nullptr, tab));
      if (!list_obj) continue;
      uint8_t* head = *reinterpret_cast<uint8_t**>(list_obj);
      if (!head) continue;
      uint8_t* node = *reinterpret_cast<uint8_t**>(head);
      int guard = 0;
      while (node && node != head && guard++ < kMaxNodes) {
        const uint8_t* value = node + rag::listnode::kValue;
        node = *reinterpret_cast<uint8_t**>(node);  // avancer AVANT de lire
        if (*reinterpret_cast<const int*>(value + rag::skillinfo::kValid) == 0) continue;
        if (*reinterpret_cast<const int*>(value + rag::skillinfo::kId) != skill_id) continue;
        int level = *reinterpret_cast<const int16_t*>(value + rag::skillinfo::kLearned);
        if (level <= 0) level = *reinterpret_cast<const int*>(value + rag::skillinfo::kLevel);
        // 🔴 PAS DE `break` ICI, ET C'EST UN CORRECTIF, pas un détail de style.
        //
        // Une liste d'onglet peut contenir DEUX FOIS la même compétence :
        // l'insertion native (sub_007381D0) ne cherche jamais l'id avant
        // d'empiler, et la construction rejoue InitSkillTreeView pour chaque job
        // de la chaîne d'héritage — plusieurs peuvent viser le même onglet (le
        // cas Doram, où les jobs 4218/4220 et le sommet de chaîne vont tous dans
        // l'onglet 0). Le niveau appris peut n'avoir été écrit que sur la
        // SECONDE fiche.
        //
        // S'arrêter au premier match rendait alors 0 pour une compétence
        // réellement connue. Le Grimoire de la feuille de personnage avait
        // rencontré et documenté ce doublon ; les deux lectures « un seul
        // niveau », elles, ne l'avaient jamais appris. On garde le MEILLEUR
        // niveau vu, et le coût SP de la fiche qui le porte.
        if (level > found) {
          found = level;
          sp = *reinterpret_cast<const int*>(value + rag::skillinfo::kSp);
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  if (sp_cost) *sp_cost = sp > 0 ? sp : 0;  // négatif ou absent = « je ne sais pas »
  return found;
}

}  // namespace rag
