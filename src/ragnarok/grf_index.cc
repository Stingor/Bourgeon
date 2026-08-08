#include "ragnarok/grf_index.h"

#include <Windows.h>

#include "utils/log_console.h"

namespace rag {
namespace {

// ── Structures du client (20250716, base 0x400000) ───────────────────────────
// Les décalages et leur provenance sont détaillés dans l'en-tête. Tout est lu,
// rien n'est écrit : on ne fait que relire ce que le montage des archives a
// déjà construit.
constexpr uintptr_t kFileMgr = 0x0159d410;  // g_FileMgr (l'OBJET)

constexpr size_t kMgrListHead   = 0x00;   // tête de liste circulaire
constexpr size_t kNodeNext      = 0x00;
constexpr size_t kNodeGrf       = 0x0C;   // l'objet CGrf porté par le nœud
constexpr size_t kGrfEntryBegin = 0x24;
constexpr size_t kGrfEntryEnd   = 0x28;
constexpr size_t kEntryStride   = 0x120;
constexpr size_t kEntryRealSize = 0x08;   // taille décompressée
constexpr size_t kEntryName     = 0x20;   // char[256], minuscules, antislashs
constexpr size_t kEntryNameCap  = kEntryStride - kEntryName;  // 256

// Garde-fous. Ils ne protègent pas d'un pointeur INVALIDE — c'est le rôle du
// SEH — mais d'un pointeur valide et absurde, qui ferait tourner la boucle des
// heures sans jamais lever d'exception. `data.grf` compte de l'ordre de 10^5 à
// 10^6 entrées ; au-delà de quelques millions, ce n'est plus une table.
constexpr int    kMaxArchives = 64;
constexpr size_t kMaxEntries  = 4u * 1000u * 1000u;

// Repliement ASCII : casse ignorée, et « / » vaut « \ ». Les octets de la
// code-page coréenne (>= 0x80, donc négatifs) traversent inchangés — c'est
// voulu, on ne cherche à interpréter que la partie ASCII d'un chemin.
char FoldAscii(char c) {
  if (c == '/') return '\\';
  if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
  return c;
}

std::string Fold(const char* s) {
  std::string r;
  for (const char* p = s; p && *p; ++p) r.push_back(FoldAscii(*p));
  return r;
}

// Un nom d'entrée occupe un champ de taille FIXE : s'il n'y a pas de NUL dans
// les 256 octets, ce n'est pas un nom exploitable et l'appelant l'écarte.
size_t BoundedLen(const char* s, size_t cap) {
  size_t n = 0;
  while (n < cap && s[n]) ++n;
  return n;
}

// `prefix` et `suffix` sont déjà repliés par l'appelant ; `name` l'est à la
// volée, pour ne pas recopier 256 octets par entrée examinée.
bool Matches(const char* name, size_t len,
             const char* prefix, size_t prefix_len,
             const char* suffix, size_t suffix_len) {
  if (len < prefix_len || len < suffix_len) return false;
  for (size_t i = 0; i < prefix_len; ++i)
    if (FoldAscii(name[i]) != prefix[i]) return false;
  for (size_t i = 0; i < suffix_len; ++i)
    if (FoldAscii(name[len - suffix_len + i]) != suffix[i]) return false;
  return true;
}

struct RawHit {
  char     path[kEntryNameCap];
  uint32_t size;
};

// Ce que le balayage a VU, indépendamment de ce qu'il a retenu. Sert de mesure :
// des offsets devenus faux se voient à un compte d'archives nul ou à un nombre
// d'entrées grotesque, pas à une liste vide (qui, elle, veut peut-être seulement
// dire « personne n'a rien mis dans ce dossier »).
struct ScanStats {
  int    archives = 0;
  size_t entries  = 0;
  int    hits     = 0;  // total trouvé, avant plafonnement à `cap`
};

// Parcours brut de la table.
//
// ⚠ Aucun objet C++ ici, pas même un std::string : MSVC refuse `__try` dans une
// fonction qui doit dérouler des destructeurs (C2712). La sortie est donc un
// tableau POD que l'appelant convertit. Le SEH n'est pas de la paranoïa de
// principe : ces décalages appartiennent à une version précise du client, et
// une galerie sans les décors packés vaut mieux qu'un plantage au char-select.
//
// Rend le nombre d'entrées ÉCRITES dans `out` (<= cap), ou -1 si la table n'a
// pas pu être lue.
int CollectRaw(const char* prefix, size_t prefix_len,
               const char* suffix, size_t suffix_len,
               RawHit* out, int cap, ScanStats* st) {
  int written = 0;
  __try {
    const uint8_t* const mgr = reinterpret_cast<const uint8_t*>(kFileMgr);
    const uint8_t* const head =
        *reinterpret_cast<const uint8_t* const*>(mgr + kMgrListHead);
    if (!head) return 0;  // aucune archive montée (jamais vu, mais pas une erreur)

    const uint8_t* node = *reinterpret_cast<const uint8_t* const*>(head + kNodeNext);
    for (int a = 0; node && node != head && a < kMaxArchives; ++a) {
      const uint8_t* const grf =
          *reinterpret_cast<const uint8_t* const*>(node + kNodeGrf);
      node = *reinterpret_cast<const uint8_t* const*>(node + kNodeNext);
      if (!grf) continue;

      const uint8_t* const begin =
          *reinterpret_cast<const uint8_t* const*>(grf + kGrfEntryBegin);
      const uint8_t* const end =
          *reinterpret_cast<const uint8_t* const*>(grf + kGrfEntryEnd);
      if (!begin || end < begin) continue;
      const size_t span = static_cast<size_t>(end - begin);
      // Un reste non nul signifie que ce n'est pas le vecteur qu'on croit : on
      // passe l'archive au lieu d'y lire des noms à des adresses décalées.
      if (span % kEntryStride != 0) continue;
      const size_t count = span / kEntryStride;
      if (count > kMaxEntries) continue;

      ++st->archives;
      st->entries += count;
      for (size_t i = 0; i < count; ++i) {
        const uint8_t* const entry = begin + i * kEntryStride;
        const char* const name = reinterpret_cast<const char*>(entry + kEntryName);
        const size_t len = BoundedLen(name, kEntryNameCap);
        if (len == 0 || len >= kEntryNameCap) continue;  // champ non terminé
        if (!Matches(name, len, prefix, prefix_len, suffix, suffix_len)) continue;

        ++st->hits;
        if (written >= cap) continue;  // on compte quand même : cf. le journal
        RawHit& hit = out[written];
        for (size_t k = 0; k < len; ++k) hit.path[k] = name[k];
        hit.path[len] = '\0';
        hit.size = *reinterpret_cast<const uint32_t*>(entry + kEntryRealSize);
        ++written;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
  return written;
}

}  // namespace

bool ListGrfFiles(const char* prefix, const char* suffix,
                  std::vector<GrfFile>* out, int limit) {
  if (!out) return false;
  out->clear();
  if (limit <= 0) return false;

  const std::string pre = Fold(prefix);
  const std::string suf = Fold(suffix);
  std::vector<RawHit> hits(static_cast<size_t>(limit));
  ScanStats st;
  const int n = CollectRaw(pre.c_str(), pre.size(), suf.c_str(), suf.size(),
                           hits.data(), limit, &st);
  if (n < 0) {
    LogError("[GRF] table de fichiers illisible (structures du client déplacées ?) "
             "- rien ne sera listé depuis les archives");
    return false;
  }
  if (st.archives == 0) {
    // Pas une erreur en soi (un client tout-disque est légitime), mais si des
    // GRF sont bel et bien montés, c'est le signe que les décalages ont bougé.
    LogDiag("[GRF] aucune archive lisible dans g_FileMgr");
    return false;
  }
  for (int i = 0; i < n; ++i)
    out->push_back({std::string(hits[i].path), hits[i].size});
  LogDiag("[GRF] {} archive(s), {} entrée(s) balayées, {} sous « {}{} »{}",
          st.archives, st.entries, st.hits, pre, suf,
          st.hits > n ? " (liste plafonnée)" : "");
  return true;
}

}  // namespace rag
