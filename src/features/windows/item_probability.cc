#include "features/windows/item_probability.h"

#include <Windows.h>

#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ragnarok/stl_node.h"       // rag::treenode / rag::listnode

namespace itemprob {
namespace {

// ── Adresses RE (client 20250716) ───────────────────────────────────────────
// `CNeoPackageItemMgr` est créé paresseusement : le pointeur est nul tant que le
// lub n'a pas été chargé.
constexpr uintptr_t kMgrPtr = 0x01255108;  // ptr -> CNeoPackageItemMgr
constexpr uintptr_t kFetch  = 0x0069f480;  // ItemProbabilityDB_Fetch
// __thiscall(mgr, out[2], id) : out[0] = &enregistrement, out[1] octet = trouvé.
// L'id est normalisé par le manager (costume / variante -> objet de base).
using Fetch_t = void(__thiscall*)(void*, int*, int);

// L'enregistrement rendu par la recherche : la map des groupes, puis sa taille.
constexpr int kRecordGroupHead = 0x00;

// Valeur d'un nœud de la map des GROUPES. La clé (l'identifiant de groupe) est en
// tête de `_Myval`, la donnée derrière elle.
constexpr int kGroupId    = rag::treenode::kValue + 0x00;
constexpr int kGroupTotal = rag::treenode::kValue + 0x04;  // dénominateur du tirage
constexpr int kGroupList  = rag::treenode::kValue + 0x08;  // -> sentinelle std::list
constexpr int kGroupCount = rag::treenode::kValue + 0x0c;  // nb d'entrées

// Valeur d'un nœud de la liste des ENTRÉES.
constexpr int kEntryId     = rag::listnode::kValue + 0x00;
constexpr int kEntryLabel  = rag::listnode::kValue + 0x04;  // std::string
constexpr int kEntryWeight = kEntryLabel + rag::clientstr::kFieldSize;

// Garde-fous. Un arbre sain ne descend pas à trente niveaux, et la plus grosse
// boîte livrée compte un millier d'entrées : au-delà, la structure lue n'est plus
// celle qu'on croit et il vaut mieux s'arrêter que boucler.
constexpr int kMaxTreeDepth = 64;
constexpr int kMaxGroups    = 32;
constexpr int kMaxEntries   = 8192;
constexpr int kLabelMax     = 256;  // libellés observés : ~110 octets

// Copies BRUTES, sans destructeur : elles traversent le `__try` (C2712 interdit
// tout objet à déroulement dans une fonction qui en contient un).
struct RawGroup {
  int            id    = 0;
  int            total = 0;
  const uint8_t* list  = nullptr;
  int            count = 0;
};
struct RawEntry {
  int  id     = 0;
  int  weight = 0;
  char label[kLabelMax] = {};
};

// ── Parcours d'un `std::_Tree` MSVC ─────────────────────────────────────────
// Appelées DEPUIS un `__try` : elles n'ont pas le leur, c'est celui de
// l'appelant qui couvre les déréférencements.

inline const uint8_t* TreeChild(const uint8_t* node, int off) {
  return *reinterpret_cast<const uint8_t* const*>(node + off);
}
inline bool TreeIsNil(const uint8_t* node) {
  return node == nullptr || node[rag::treenode::kIsNil] != 0;
}

// Le plus à gauche du sous-arbre : le premier en ordre croissant.
inline const uint8_t* TreeLeftmost(const uint8_t* node) {
  for (int depth = 0; depth < kMaxTreeDepth; ++depth) {
    const uint8_t* left = TreeChild(node, rag::treenode::kLeft);
    if (TreeIsNil(left)) break;
    node = left;
  }
  return node;
}

// Le suivant en ordre croissant. Rend la SENTINELLE une fois le dernier passé —
// c'est elle, et non un pointeur nul, qui termine le parcours.
inline const uint8_t* TreeNext(const uint8_t* node) {
  const uint8_t* right = TreeChild(node, rag::treenode::kRight);
  if (!TreeIsNil(right)) return TreeLeftmost(right);
  for (int depth = 0; depth < kMaxTreeDepth; ++depth) {
    const uint8_t* parent = TreeChild(node, rag::treenode::kParent);
    if (TreeIsNil(parent) || TreeChild(parent, rag::treenode::kRight) != node)
      return parent;
    node = parent;
  }
  return nullptr;
}

// ── Lectures gardées ────────────────────────────────────────────────────────

// L'enregistrement de l'objet, ou nullptr. `out_found` reçoit l'octet du natif.
inline const uint8_t* FetchRecord(uint32_t item_id) {
  void* mgr = *reinterpret_cast<void* const*>(kMgrPtr);
  if (!mgr) return nullptr;
  int fetched[2] = {0, 0};
  reinterpret_cast<Fetch_t>(kFetch)(mgr, fetched, static_cast<int>(item_id));
  if ((fetched[1] & 0xff) == 0) return nullptr;
  return reinterpret_cast<const uint8_t*>(fetched[0]);
}

bool HasRecord(uint32_t item_id) {
  bool found = false;
  __try {
    found = FetchRecord(item_id) != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    found = false;
  }
  return found;
}

int ReadGroupsRaw(uint32_t item_id, RawGroup* out, int max) {
  int count = 0;
  __try {
    const uint8_t* record = FetchRecord(item_id);
    if (record) {
      const uint8_t* head =
          *reinterpret_cast<const uint8_t* const*>(record + kRecordGroupHead);
      if (head) {
        const uint8_t* node = TreeChild(head, rag::treenode::kLeft);
        while (count < max && !TreeIsNil(node)) {
          out[count].id    = *reinterpret_cast<const int*>(node + kGroupId);
          out[count].total = *reinterpret_cast<const int*>(node + kGroupTotal);
          out[count].list =
              *reinterpret_cast<const uint8_t* const*>(node + kGroupList);
          out[count].count = *reinterpret_cast<const int*>(node + kGroupCount);
          ++count;
          node = TreeNext(node);
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  // Une exception laisse `count` dans l'état où le compilateur l'avait posé : on
  // ne rend jamais plus que ce que l'appelant a alloué.
  return (count < 0 || count > max) ? 0 : count;
}

// ⚠ La liste est CIRCULAIRE et `list_head` EST sa sentinelle (le champ du groupe
// pointe dessus) : on s'arrête en y revenant, jamais sur un pointeur nul.
int ReadEntriesRaw(const uint8_t* list_head, RawEntry* out, int max) {
  int count = 0;
  __try {
    if (list_head) {
      const uint8_t* node =
          *reinterpret_cast<const uint8_t* const*>(list_head + rag::listnode::kNext);
      while (count < max && node && node != list_head) {
        out[count].id     = *reinterpret_cast<const int*>(node + kEntryId);
        out[count].weight = *reinterpret_cast<const int*>(node + kEntryWeight);
        const void*    field = node + kEntryLabel;
        const char*    text  = rag::clientstr::Data(field);
        const uint32_t len   = rag::clientstr::Size(field);
        if (text && len > 0 && len < kLabelMax) {
          std::memcpy(out[count].label, text, len);
          out[count].label[len] = '\0';
        } else {
          out[count].label[0] = '\0';
        }
        ++count;
        node = *reinterpret_cast<const uint8_t* const*>(node + rag::listnode::kNext);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return (count < 0 || count > max) ? 0 : count;
}

// ── Le pourcentage, exactement comme le natif ───────────────────────────────
//
// 🔴🔴 Deux échelles DIFFÉRENTES, et se tromper donne un résultat dix fois faux
// mais parfaitement plausible : le groupe garanti passe par 10^6, le tirage
// pondéré par 10^7. Le témoin : un poids de 2 sur un total de 1787 vaut
// 0,1119 % — pas 0,0112 %. L'arrondi entier `(x + 5) / 10` est celui du client,
// et c'est lui qui fixe l'affichage à quatre décimales.
constexpr double kScaleGuaranteed = 1000000.0;   // pow(10, 6)
constexpr double kScaleWeighted   = 10000000.0;  // pow(10, 7)
constexpr double kScaleToPercent  = 10000.0;

double Percent(int group_id, int weight, int total) {
  if (group_id == 0 || total <= 0) return kScaleGuaranteed / kScaleToPercent;
  const int64_t scaled = static_cast<int64_t>(
      kScaleWeighted * static_cast<double>(weight) / static_cast<double>(total));
  return static_cast<double>((scaled + 5) / 10) / kScaleToPercent;
}

// ── Le libellé riche ────────────────────────────────────────────────────────

constexpr char kIconTag[]  = "^i[";
constexpr char kUrlOpen[]  = "<URL>";
constexpr char kInfoOpen[] = "<INFO>";
constexpr char kInfoClose[] = "</INFO>";
constexpr char kUrlMobId[]  = "mobid=";
constexpr char kUrlItemId[] = "itemid=";
constexpr int  kColorCodeLen = 7;  // '^' + six chiffres hexadécimaux

bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Recopie en laissant tomber les codes couleur `^RRGGBB`, que rien ne rend ici.
void AppendStripped(std::string* out, const char* begin, const char* end) {
  for (const char* p = begin; p < end;) {
    if (*p == '^' && end - p >= kColorCodeLen) {
      bool hex = true;
      for (int i = 1; i < kColorCodeLen; ++i)
        if (!IsHexDigit(p[i])) { hex = false; break; }
      if (hex) { p += kColorCodeLen; continue; }
    }
    out->push_back(*p++);
  }
}

// L'entier qui suit `key` dans `url`, ou 0.
uint32_t IdFromUrl(const char* url, const char* key) {
  const char* at = std::strstr(url, key);
  if (!at) return 0;
  return static_cast<uint32_t>(std::strtoul(at + std::strlen(key), nullptr, 10));
}

// Découpe `^i[id] <URL>nom<INFO>url</INFO></URL>` en nom + identifiant + nature.
// Un libellé sans balise reste affichable : c'est le nom, et l'entrée n'est
// alors pas cliquable (id 0).
void ParseLabel(const char* raw, Entry* out) {
  const char* p = raw;
  // Icône d'objet en tête : elle porte déjà l'identifiant, qui sert de repli si
  // l'URL manque.
  uint32_t icon_id = 0;
  if (std::strncmp(p, kIconTag, std::strlen(kIconTag)) == 0) {
    const char* digits = p + std::strlen(kIconTag);
    const char* close  = std::strchr(digits, ']');
    if (close) {
      icon_id = static_cast<uint32_t>(std::strtoul(digits, nullptr, 10));
      p = close + 1;
      while (*p == ' ') ++p;
    }
  }

  const char* url_open = std::strstr(p, kUrlOpen);
  if (url_open) {
    const char* name_begin = url_open + std::strlen(kUrlOpen);
    const char* info_open  = std::strstr(name_begin, kInfoOpen);
    const char* name_end   = info_open ? info_open : (name_begin + std::strlen(name_begin));
    AppendStripped(&out->name, name_begin, name_end);
    if (info_open) {
      const char* href_begin = info_open + std::strlen(kInfoOpen);
      const char* href_end   = std::strstr(href_begin, kInfoClose);
      std::string href;
      AppendStripped(&href, href_begin,
                     href_end ? href_end : (href_begin + std::strlen(href_begin)));
      // 🔴 C'est l'URL qui tranche objet / monstre, pas la plage d'identifiants :
      // les deux espaces se chevauchent.
      const uint32_t mob = IdFromUrl(href.c_str(), kUrlMobId);
      if (mob != 0) {
        out->id     = mob;
        out->is_mob = true;
        return;
      }
      const uint32_t item = IdFromUrl(href.c_str(), kUrlItemId);
      if (item != 0) {
        out->id = item;
        return;
      }
    }
  } else {
    AppendStripped(&out->name, p, p + std::strlen(p));
  }
  out->id = icon_id;  // repli : l'icône, donc un objet
}

std::unordered_map<uint32_t, Table>& Cache() {
  static std::unordered_map<uint32_t, Table> cache;
  return cache;
}

}  // namespace

bool Has(uint32_t item_id) { return HasRecord(item_id); }

const Table* Get(uint32_t item_id) {
  auto& cache = Cache();
  auto  it    = cache.find(item_id);
  if (it == cache.end()) {
    Table    table;
    RawGroup groups[kMaxGroups];
    const int group_count = ReadGroupsRaw(item_id, groups, kMaxGroups);
    std::vector<RawEntry> raw;
    for (int g = 0; g < group_count; ++g) {
      // `count` est ce que le client annonce : on s'en sert pour dimensionner,
      // jamais pour décider de la fin du parcours (c'est la sentinelle qui la
      // dit). Un compte aberrant ne fait donc que rater la réservation.
      const int hint = (groups[g].count > 0 && groups[g].count < kMaxEntries)
                           ? groups[g].count
                           : kMaxEntries;
      raw.assign(static_cast<size_t>(hint), RawEntry{});
      const int n = ReadEntriesRaw(groups[g].list, raw.data(), hint);
      Group out_group;
      out_group.id    = groups[g].id;
      out_group.total = groups[g].total;
      out_group.entries.reserve(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        Entry e;
        e.weight = raw[i].weight;
        e.pct    = Percent(groups[g].id, raw[i].weight, groups[g].total);
        ParseLabel(raw[i].label, &e);
        // Le libellé porte l'identifiant deux fois (l'icône et l'URL) ; celui du
        // nœud reste le repli quand aucune balise ne l'a donné.
        if (e.id == 0) e.id = static_cast<uint32_t>(raw[i].id);
        out_group.entries.push_back(std::move(e));
      }
      table.entry_count += out_group.entries.size();
      table.groups.push_back(std::move(out_group));
    }
    it = cache.emplace(item_id, std::move(table)).first;
  }
  return it->second.groups.empty() ? nullptr : &it->second;
}

}  // namespace itemprob
