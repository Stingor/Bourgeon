#include "ragnarok/social.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "ragnarok/game_scene.h"
#include "ui/game_texture.h"  // ro::uipath::kUiRoot (racine CP949 des bitmaps d'interface)
#include "utils/i18n.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ragnarok/stl_node.h"  // rag::listnode
#include "ragnarok/actor.h"  // rag::actor

namespace rag::social {
namespace {

// ── Le manager social : des champs de la SESSION ─────────────────────────────
// Les deux listes sont des std::list circulaires dont la session ne garde que le
// POINTEUR de sentinelle ; `next` est le premier dword du nœud et la donnée
// commence à `nœud+8`. Confirmé par trois chemins indépendants : les accesseurs
// (0x00d5a0d0 / 0x00d5da80), le vidage de session (0x00d70220) et le départ de
// groupe (0x00d56530). Cf. docs/party_friend_re.md §2.
constexpr int kSes_PartyListPtr  = 0x17bc;
constexpr int kSes_PartyCount    = 0x17c0;
constexpr int kSes_FriendListPtr = 0x17c4;
constexpr int kSes_FriendCount   = 0x17c8;

// ── L'entrée sociale (0x50 octets) ──────────────────────────────────────────
constexpr int kEnt_Gid     = 0x04;
constexpr int kEnt_Id2     = 0x08;
constexpr int kEnt_Name    = 0x0c;  // std::string
constexpr int kEnt_Map     = 0x24;  // std::string
constexpr int kEnt_Leader  = 0x3c;  // 🔴 0 = CHEF
constexpr int kEnt_Offline = 0x40;
constexpr int kEnt_Color   = 0x44;
constexpr int kEnt_Job     = 0x48;  // u16
constexpr int kEnt_Level   = 0x4a;  // u16

// ── L'acteur, pour les PV ────────────────────────────────────────────────────
// `Actor_FindByGid(gid)` __stdcall : raccourci global qui résout le mode lui-même
// (le même que target_frame). La `UIPcGage` de +0x488 est celle que le client pose
// justement pour LES MEMBRES DE PARTY ; PV courants en +0xA0, maximum en +0xA4.
constexpr int       kAct_PcGage     = 0x488;

// Le natif dimensionne 40 jauges et 40 boutons de job : c'est sa borne de lignes.
constexpr int kMaxRows = 64;

// ── Lectures brutes, toutes sous SEH ─────────────────────────────────────────
// 🔴 Aucune de ces fonctions ne manipule d'objet à destructeur : MSVC refuse
// `__try` dans une fonction qui demande du déroulement. La conversion vers des
// std::string se fait chez l'appelant, hors du bloc protégé.

struct RawRow {
  uint32_t gid = 0, id2 = 0, color = 0;
  uint16_t job = 0, level = 0;
  bool     leader = false;
  bool     offline = false;
  char     name[64] = {0};
  char     map[32]  = {0};
};

// Collecte les NŒUDS de la liste (pointeurs seulement). Renvoie le nombre lu.
// Trois bornes, et c'est voulu : la sentinelle (fin normale du tour), le compteur
// que la session tient à côté de la liste, et `cap`. Le compteur seul ne suffirait
// pas — il peut être en avance ou en retard d'un élément le temps d'un ajout — et
// la sentinelle seule laisserait boucler sans fin sur une liste remaniée pendant
// qu'on la parcourt.
int CollectNodesSEH(int list_ptr_offset, int count_offset, const void** nodes,
                    int cap) {
  int n = 0;
  __try {
    const uintptr_t sentinel = rag::SessionField<uintptr_t>(list_ptr_offset);
    if (!sentinel) return 0;
    const int announced = rag::SessionField<int>(count_offset);
    int limit = cap;
    if (announced > 0 && announced < limit) limit = announced;
    uintptr_t node = *reinterpret_cast<const uintptr_t*>(sentinel);
    while (node && node != sentinel && n < limit) {
      nodes[n++] = reinterpret_cast<const void*>(node);
      node = *reinterpret_cast<const uintptr_t*>(node);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { /* liste en cours de remaniement */ }
  return n;
}

bool ReadNodeSEH(const void* node, RawRow& out) {
  uintptr_t data = 0;
  __try {
    data = reinterpret_cast<uintptr_t>(node) + rag::listnode::kValue;
    out.gid     = *reinterpret_cast<const uint32_t*>(data + kEnt_Gid);
    out.id2     = *reinterpret_cast<const uint32_t*>(data + kEnt_Id2);
    out.color   = *reinterpret_cast<const uint32_t*>(data + kEnt_Color);
    out.job     = *reinterpret_cast<const uint16_t*>(data + kEnt_Job);
    out.level   = *reinterpret_cast<const uint16_t*>(data + kEnt_Level);
    // 🔴 Le natif code le chef par ZÉRO, pas par un.
    out.leader  = (*reinterpret_cast<const uint32_t*>(data + kEnt_Leader) == 0);
    out.offline = (*reinterpret_cast<const uint32_t*>(data + kEnt_Offline) != 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  rag::clientstr::CopyTruncating(reinterpret_cast<const void*>(data + kEnt_Name), out.name, sizeof(out.name));
  rag::clientstr::CopyTruncating(reinterpret_cast<const void*>(data + kEnt_Map),  out.map,  sizeof(out.map));
  return true;
}

// PV d'un membre. Rend false quand l'acteur n'est pas chargé — ce qui n'est PAS
// une erreur : c'est l'état normal d'un membre hors de portée.
bool ReadHpSEH(uint32_t gid, int* hp, int* max_hp) {
  __try {
    if (gid && gid == rag::OwnAccountId()) {
      *hp     = rag::OwnHp();
      *max_hp = rag::OwnMaxHp();
      return *max_hp > 0;
    }
    using FindActorFn = void* (__stdcall*)(uint32_t);
    void* actor = reinterpret_cast<FindActorFn>(gamescene::kFindActorByGidAddr)(gid);
    if (!actor) return false;
    void* gage = *reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(actor) + kAct_PcGage);
    if (!gage) return false;
    const uint8_t* g = reinterpret_cast<const uint8_t*>(gage);
    const int cur = *reinterpret_cast<const int*>(g + rag::actor::kGageHp);
    const int max = *reinterpret_cast<const int*>(g + rag::actor::kGageHpMax);
    if (max <= 0) return false;
    *hp = cur;
    *max_hp = max;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}


void ReadList(bool party, std::vector<Entry>& out) {
  out.clear();
  const void* nodes[kMaxRows] = {nullptr};
  const int n = CollectNodesSEH(party ? kSes_PartyListPtr : kSes_FriendListPtr,
                                party ? kSes_PartyCount : kSes_FriendCount,
                                nodes, kMaxRows);
  out.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    RawRow raw;
    if (!ReadNodeSEH(nodes[i], raw)) continue;
    Entry e;
    e.gid       = raw.gid;
    e.id2       = raw.id2;
    e.name      = raw.name;
    e.map       = raw.map;
    e.is_leader = raw.leader;
    e.offline   = raw.offline;
    e.color     = raw.color;
    e.job       = raw.job;
    e.level     = raw.level;
    if (party && !e.offline) {
      // Un membre hors ligne n'a pas d'acteur : le natif masque sa jauge sans
      // même chercher. On fait pareil, pour ne pas afficher les PV d'un homonyme.
      int hp = 0, max_hp = 0;
      e.has_hp = ReadHpSEH(e.gid, &hp, &max_hp);
      e.hp = hp;
      e.max_hp = max_hp;
    }
    out.push_back(std::move(e));
  }
}

}  // namespace

void ReadParty(std::vector<Entry>& out)   { ReadList(true, out); }
void ReadFriends(std::vector<Entry>& out) { ReadList(false, out); }

uint32_t OwnAid() { return rag::OwnAccountIdSafe(); }

int PartyMemberCount() {
  __try {
    using Fn = int(__thiscall*)(void*);
    return reinterpret_cast<Fn>(rag::kPartyMemberCountAddr)(
        reinterpret_cast<void*>(rag::kSessionAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void JobIconPath(int job_id, char* out, size_t cap) {
  if (!out || cap == 0) return;
  std::snprintf(out, cap, "%s\\renewalparty\\icon_jobs_%u.bmp", ro::uipath::kUiRoot,
                static_cast<unsigned>(job_id));
}

const char* JobName(int job_id) {
  // Le résolveur passe par la table Lua des classes : on met en cache. Ce cache
  // sert AUSSI le roster de guilde de la feuille de personnage — qui portait sa
  // copie, cache et repli compris, jusqu'au 2026-08-24.
  static std::unordered_map<int, std::string> cache;
  auto it = cache.find(job_id);
  if (it != cache.end()) return it->second.c_str();
  // 🔴 `rag::JobName` et non `rag::JobNameMySex` : une ligne de groupe ou d'amis
  // désigne QUELQU'UN D'AUTRE (cf. globals.h).
  const char* n = rag::JobName(job_id);
  char buf[64];
  if (n && n[0]) {
    std::strncpy(buf, n, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
  } else {
    std::snprintf(buf, sizeof(buf), i18n::Tr("Classe %d"), job_id);
  }
  return (cache[job_id] = buf).c_str();
}

}  // namespace rag::social
