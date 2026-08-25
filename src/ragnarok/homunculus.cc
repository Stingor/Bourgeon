#include "ragnarok/homunculus.h"

#include <Windows.h>

#include <cstring>

#include "ragnarok/globals.h"
#include "ragnarok/item_db.h"
#include "ragnarok/uiwnd.h"

namespace rag {
namespace homun {
namespace {

// ── Globals plats (client 20250716, base 0x400000) ──────────────────────────
// Écrits par le décodeur de ZC_PROPERTY_HOMUN 0x0BA4 (0x00CD1ED0), le SEUL que
// Moonlight émette. ⚠ Quatre autres décodeurs cohabitent dans le binaire avec des
// offsets de champs DIFFÉRENTS : ne jamais en déduire un format depuis eux.
constexpr uintptr_t kAid      = 0x015ff918;  // GID de l'homoncule
constexpr uintptr_t kName     = 0x015ff91c;  // char[24]
constexpr uintptr_t kAtk      = 0x015ff93c;  // puis MATK/HIT/CRI/DEF/MDEF/FLEE de 4 en 4
constexpr uintptr_t kAmotion  = 0x015ff958;
constexpr uintptr_t kClass    = 0x015ff95c;  // -1 = aucun homoncule connu
constexpr uintptr_t kLevel    = 0x015ff960;
constexpr uintptr_t kHp       = 0x015ff964;
constexpr uintptr_t kMaxHp    = 0x015ff968;
constexpr uintptr_t kSp       = 0x015ff96c;
constexpr uintptr_t kMaxSp    = 0x015ff970;
constexpr uintptr_t kIntimacy = 0x015ff974;
constexpr uintptr_t kExp      = 0x015ff980;  // i64
constexpr uintptr_t kExpNext  = 0x015ff988;  // i64
constexpr uintptr_t kHunger   = 0x015ff990;
constexpr uintptr_t kFlags    = 0x015ff998;
constexpr uintptr_t kOrderBlk = 0x015ff99c;  // ctx+0x55DC, six dwords (ordre à l'homoncule)
constexpr int       kOrderWords = 6;
constexpr uintptr_t kPresent  = 0x015ff9b4;  // != 0 <=> un homoncule est invoqué
constexpr uintptr_t kAtkRange = 0x015ff9c4;
constexpr uintptr_t kSkillPts = 0x015fba04;
constexpr uintptr_t kAutoFeed = 0x0160231c;  // octet

// Liste de compétences : {tête, taille} = session+0x64 / +0x68. PAS le bundle du perso.
constexpr uintptr_t kSkillHead = 0x015fa424;
constexpr uintptr_t kSkillSize = 0x015fa428;

// jobname.lub : std::vector<const char*> {begin, end}, indexé par la classe. C'est le
// chemin que prend Job_GetDisplayNameOrResName (0x00D5BB40) pour une classe > 30 hors
// 4001..5999 — donc pour tout homoncule (6001..6016, 6048..6052).
constexpr uintptr_t kJobNameBeg = 0x015fb348;
constexpr uintptr_t kJobNameEnd = 0x015fb34c;

// Nœud de std::list MSVC : {next, prev, valeur} — la valeur commence à nœud+8. Les
// offsets internes sont ceux de l'ItemSkillInfo, partagé avec le bundle du personnage.
constexpr int kNodeValue  = 0x08;
constexpr int kOffValid   = 0x04;
constexpr int kOffId      = 0x08;
constexpr int kOffInf     = 0x0c;
constexpr int kOffLevel   = 0x10;
constexpr int kOffSp      = 0x14;
constexpr int kOffUpgrade = 0x18;
constexpr int kOffRange   = 0x1c;

// Accesseur natif par index dans la liste de l'homoncule : __thiscall(ctx, &out, i).
// C'est CELUI qu'utilise la fenêtre native 114 pour bâtir la struct qu'elle passe au
// dispatcher — on emprunte le même, la struct étant un objet C++ qu'on ne sait pas
// construire soi-même.
constexpr uintptr_t kSkillGetAt = 0x00d80810;
constexpr int kInfoOffFound = 0x04;               // fiche utilisable
constexpr int kInfoOffLevel = 0x10;               // niveau appris
constexpr int kCmdUseSkillSlot = 0x71;            // « lancer, routé par l'INF »

using GetAt_t   = void* (__fastcall*)(void*, void*, void*, int);

// Parcours brut : remplit `out` et rend le nombre d'entrées. `pos` suit le rang dans
// la liste NON filtrée, seul indice que l'accesseur natif comprenne.
int ReadSkillsSEH(Skill* out, int cap) {
  int n = 0;
  __try {
    if (*reinterpret_cast<const int*>(kSkillSize) <= 0) return 0;
    uint8_t* head = *reinterpret_cast<uint8_t**>(kSkillHead);
    if (!head) return 0;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head);
    int guard = 0, pos = -1;
    while (node && node != head && n < cap && guard++ < kMaxSkills * 2) {
      const uint8_t* v = node + kNodeValue;
      node = *reinterpret_cast<uint8_t**>(node);  // avancer AVANT de lire la valeur
      ++pos;
      if (*reinterpret_cast<const int*>(v + kOffValid) == 0) continue;
      const int id = *reinterpret_cast<const int*>(v + kOffId);
      if (id <= 0) continue;
      Skill& r = out[n++];
      r.pos        = pos;
      r.id         = id;
      r.inf        = *reinterpret_cast<const int*>(v + kOffInf);
      r.level      = *reinterpret_cast<const int*>(v + kOffLevel);
      r.sp         = *reinterpret_cast<const int*>(v + kOffSp);
      r.upgradable = *reinterpret_cast<const int*>(v + kOffUpgrade);
      r.range      = *reinterpret_cast<const int*>(v + kOffRange);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return n;
}

bool ReadStateSEH(State* s) {
  __try {
    s->cls = *reinterpret_cast<const int*>(kClass);
    if (s->cls == -1) return false;
    s->aid    = *reinterpret_cast<const int*>(kAid);
    s->level  = *reinterpret_cast<const int*>(kLevel);
    s->hp     = *reinterpret_cast<const int*>(kHp);
    s->max_hp = *reinterpret_cast<const int*>(kMaxHp);
    s->sp     = *reinterpret_cast<const int*>(kSp);
    s->max_sp = *reinterpret_cast<const int*>(kMaxSp);
    const int* st = reinterpret_cast<const int*>(kAtk);
    s->atk = st[0]; s->matk = st[1]; s->hit = st[2];  s->crit = st[3];
    s->def = st[4]; s->mdef = st[5]; s->flee = st[6];
    s->amotion      = *reinterpret_cast<const int*>(kAmotion);
    s->intimacy     = *reinterpret_cast<const int*>(kIntimacy);
    s->hunger       = *reinterpret_cast<const int*>(kHunger);
    s->flags        = *reinterpret_cast<const int*>(kFlags);
    s->range        = *reinterpret_cast<const int*>(kAtkRange);
    s->skill_points = *reinterpret_cast<const int*>(kSkillPts);
    s->auto_feed    = *reinterpret_cast<const uint8_t*>(kAutoFeed) != 0;
    s->exp          = *reinterpret_cast<const long long*>(kExp);
    s->exp_next     = *reinterpret_cast<const long long*>(kExpNext);
    std::strncpy(s->name, reinterpret_cast<const char*>(kName), 24);
    s->name[24] = '\0';
    const char* const* beg = *reinterpret_cast<const char* const* const*>(kJobNameBeg);
    const char* const* end = *reinterpret_cast<const char* const* const*>(kJobNameEnd);
    if (beg && end && s->cls >= 0 && s->cls < static_cast<int>(end - beg)) {
      const char* jn = beg[s->cls];
      if (jn) { std::strncpy(s->job, jn, 31); s->job[31] = '\0'; }
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool LaunchSEH(int pos, int level) {
  bool sent = false;
  __try {
    void* d = *reinterpret_cast<void**>(rag::kActiveModePtr);
    if (!d || pos < 0) return false;
    alignas(8) uint8_t info[0xC0] = {};
    reinterpret_cast<GetAt_t>(kSkillGetAt)(reinterpret_cast<void*>(rag::kSessionAddr), nullptr, info, pos);
    if (*reinterpret_cast<const int*>(info + kInfoOffFound)) {
      const int owned = *reinterpret_cast<const int*>(info + kInfoOffLevel);
      int lv = level < 1 ? 1 : level;
      if (owned > 0 && lv > owned) lv = owned;  // le natif refuse au-dessus de l'appris
      rag::ModeSendMsg(d, kCmdUseSkillSlot,
                                          static_cast<int>(reinterpret_cast<uintptr_t>(info)),
                                          lv, 0, 0);
      sent = true;
    }
    // Objet C++ : détruit dans tous les cas, y compris quand rien n'a été envoyé.
    reinterpret_cast<void(__fastcall*)(void*)>(itemdb::kFilledInfoDtorAddr)(info);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  return sent;
}

bool PresentSEH() {
  __try {
    return *reinterpret_cast<const int*>(kPresent) != 0 &&
           *reinterpret_cast<const int*>(kClass) != -1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void NotifyDeletedSEH() {
  __try {
    int* block = reinterpret_cast<int*>(kOrderBlk);
    for (int i = 0; i < kOrderWords; ++i) block[i] = 0;
    *reinterpret_cast<int*>(kPresent) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

}  // namespace

bool Present() { return PresentSEH(); }

bool ReadState(State* out) {
  if (!out) return false;
  *out = State{};
  return ReadStateSEH(out);
}

int ReadSkills(Skill* out, int cap) {
  if (!out || cap <= 0) return 0;
  return ReadSkillsSEH(out, cap);
}

int SkillLevel(int skill_id) {
  if (!IsSkillId(skill_id)) return 0;
  Skill list[kMaxSkills];
  const int n = ReadSkillsSEH(list, kMaxSkills);
  for (int i = 0; i < n; ++i)
    if (list[i].id == skill_id) return list[i].level;
  return 0;
}

bool LaunchSkill(int skill_id, int level) {
  // L'accesseur natif travaille par INDEX : on retrouve donc d'abord le rang brut de
  // la compétence, seul lien entre son id et la position que le client comprend.
  Skill list[kMaxSkills];
  const int n = ReadSkillsSEH(list, kMaxSkills);
  for (int i = 0; i < n; ++i)
    if (list[i].id == skill_id) return LaunchSEH(list[i].pos, level);
  return false;
}

void NotifyDeleted() { NotifyDeletedSEH(); }

}  // namespace homun
}  // namespace rag
