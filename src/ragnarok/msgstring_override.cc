#include "ragnarok/msgstring_override.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ragnarok/msgstring.h"
#include "ui/ro_imgui.h"  // ro::Utf8ToLocal
#include "utils/game_paths.h"
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"
#include "utils/log_console.h"

namespace msgoverride {
namespace {

using GetById_t = const char*(__cdecl*)(unsigned int);

GetById_t g_orig_get_by_id = nullptr;

// id -> texte déjà converti dans la code-page du client.
std::unordered_map<int, const char*> g_by_id;

// 🔴 STOCKAGE PERSISTANT DES CHAÎNES. `std::deque` et non `vector` : les
// pointeurs rendus au client doivent rester valides, et un vector qui réalloue
// les invaliderait tous d'un coup — le jeu tiendrait alors des pointeurs vers de
// la mémoire libérée, sans le moindre signe avant le crash.
// ⚠ Rien n'est jamais libéré, même à un changement de langue : le client peut
// avoir gardé un pointeur de l'ancienne. Quelques centaines de kilo-octets pour
// une garantie de durée de vie, c'est le bon échange.
std::deque<std::string> g_storage;

Stats g_stats;

// ── Spécificateurs printf ────────────────────────────────────────────────────
// La SÉQUENCE seule compte : même nombre, même ordre, mêmes types. On normalise
// en retirant drapeaux, largeur et précision — traduire « %-20s » en « %s » est
// une différence de mise en forme, pas de contrat, et refuser ça bloquerait des
// traductions parfaitement sûres.
//
// 🔴 DEUX PRÉCAUTIONS APPRISES SUR LE TAS (2026-08-14), sans quoi ce contrôle
// refuse des traductions correctes :
//
// 1. **L'ESPACE N'EST PAS TRAITÉ COMME UN DRAPEAU**, alors que `printf` l'accepte.
//    La table du client est pleine de POURCENTAGES littéraux — « your Weight is
//    over 50% of the Weight Limit ». En comptant l'espace comme drapeau, `% o` se
//    lit comme un `%o` octal ; la traduction « dépasse 50% de la limite » donne
//    alors `%d`, les séquences diffèrent, et la ligne est refusée pour rien. Le
//    drapeau espace de `printf` est rarissime, le pourcentage suivi d'un mot est
//    partout : l'arbitrage est vite fait.
//
// 2. **Le caractère final doit être une VRAIE lettre de conversion.** Sans ce
//    test, « 50% de » pousserait l'espace ou le « d » d'un mot dans la séquence.
std::vector<char> FormatSequence(const std::string& text) {
  static const char kFlags[] = "-+#0123456789.*";
  static const char kLength[] = "hlLzjt";
  static const char kConversions[] = "diouxXeEfgGaAcspn";

  std::vector<char> seq;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') continue;
    if (i + 1 < text.size() && text[i + 1] == '%') { ++i; continue; }  // %% littéral
    std::size_t j = i + 1;
    while (j < text.size() && std::strchr(kFlags, text[j]) != nullptr) ++j;
    // Modificateurs de longueur : ils ne changent pas la nature de l'argument
    // pour ce contrôle, mais doivent être franchis pour atteindre le type.
    while (j < text.size() && std::strchr(kLength, text[j]) != nullptr) ++j;
    if (j < text.size() && std::strchr(kConversions, text[j]) != nullptr) {
      seq.push_back(text[j]);
      i = j;
    }
    // Sinon ce n'était pas un spécificateur : on laisse `i` avancer d'un cran,
    // sans rien enregistrer.
  }
  return seq;
}

// ── Lecture du csv du client ─────────────────────────────────────────────────
// Deux colonnes en base64 : clé `MSI_*`, texte. L'index de LIGNE est l'id.

int Base64Value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::string Base64Decode(const char* data, std::size_t len) {
  std::string out;
  out.reserve(len * 3 / 4 + 3);
  int acc = 0, bits = 0;
  for (std::size_t i = 0; i < len; ++i) {
    const int v = Base64Value(static_cast<unsigned char>(data[i]));
    if (v < 0) continue;  // '=' de bourrage, espaces, retours chariot
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

// ── Le VFS du client (cf. ui/spr_act.cc, qui l'emploie pour les .spr/.act) ────
//
// 🔴 PAS un `std::ifstream` SUR LE DISQUE. Chez le développeur, le csv est
// effectivement dans `data\` ; chez le JOUEUR il est dans le GRF, et une lecture
// disque n'y trouverait rien — la traduction serait morte chez tout le monde sauf
// nous. `FileMgr_LoadToMemory` est le lecteur du client lui-même : avec
// `disk_only == 0` il essaie le disque PUIS les archives, dans l'ordre que le
// patcheur impose (disque d'abord, cf. reference_grf_loading_patcher). Un
// override posé dans `data\` prime donc, exactement comme pour le client.
//
// ⚠ Le tampon vient de VirtualAlloc : il se rend par `FileMgr_FreeBuffer`, jamais
// par `free`/`delete[]` — ils corrompraient le processus.
constexpr uintptr_t kFileMgrAddr     = 0x0159d410;  // g_FileMgr (l'OBJET)
constexpr uintptr_t kLoadToMemoryAddr = 0x00a88ab0;
constexpr uintptr_t kFreeBufferAddr   = 0x00a892c0;

using LoadToMemoryFn = void*(__fastcall*)(void*, void*, const char*, DWORD*, char);
using FreeBufferFn   = int(__stdcall*)(void*);

// ⚠ Fonction SÉPARÉE, et sans le moindre objet C++ : MSVC refuse `__try` dans une
// fonction qui doit dérouler des destructeurs (C2712).
void* LoadClientFile(const char* path, DWORD* size) {
  __try {
    return reinterpret_cast<LoadToMemoryFn>(kLoadToMemoryAddr)(
        reinterpret_cast<void*>(kFileMgrAddr), nullptr, path, size, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// clé `MSI_*` -> id. Vide si le csv est introuvable.
std::unordered_map<std::string, int> ReadClientTable() {
  std::unordered_map<std::string, int> keys;

  DWORD size = 0;
  void* buffer = LoadClientFile("data\\msgstringtable.csv", &size);

  if (!buffer || size == 0) {
    LogDiag("[msgstring] data\\msgstringtable.csv illisible — "
            "aucune traduction du client possible");
    if (buffer) reinterpret_cast<FreeBufferFn>(kFreeBufferAddr)(buffer);
    return keys;
  }

  // Découpage sur les fins de ligne : l'index de LIGNE est l'id (ligne 1 = id 0),
  // et seule la première colonne (la clé, en base64) nous intéresse.
  const char* p = static_cast<const char*>(buffer);
  const char* end = p + size;
  int id = 0;
  while (p < end) {
    const char* eol = p;
    while (eol < end && *eol != '\n') ++eol;
    const char* stop = eol;
    if (stop > p && stop[-1] == '\r') --stop;
    const char* comma = p;
    while (comma < stop && *comma != ',') ++comma;
    if (comma > p) {
      std::string key = Base64Decode(p, static_cast<std::size_t>(comma - p));
      if (!key.empty()) keys.emplace(std::move(key), id);
    }
    ++id;
    p = (eol < end) ? eol + 1 : end;
  }

  reinterpret_cast<FreeBufferFn>(kFreeBufferAddr)(buffer);
  g_stats.table = keys.size();
  return keys;
}

// ── Lecture du catalogue Bourgeon ────────────────────────────────────────────
// Même format que en.yaml / es.yaml : `"clé": "valeur"`, une par ligne, les
// commentaires en `#`. Un mini-parseur suffit et évite de faire dépendre un
// module `ragnarok/` de yaml-cpp pour deux caractères de syntaxe.

std::string Unescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      switch (s[++i]) {
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        case '"':  out.push_back('"');  break;
        case '\\': out.push_back('\\'); break;
        default:   out.push_back(s[i]); break;
      }
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

bool ParseLine(const std::string& line, std::string* key, std::string* value) {
  if (line.empty() || line[0] != '"') return false;
  const std::size_t sep = line.find("\": \"");
  if (sep == std::string::npos) return false;
  const std::size_t end = line.rfind('"');
  if (end <= sep + 3) return false;
  *key = line.substr(1, sep - 1);
  *value = Unescape(line.substr(sep + 4, end - sep - 4));
  return !key->empty();
}

// Le catalogue est-il encore à lire ? Cf. `LoadCatalogNow` et `Reload`.
bool g_pending = false;
bool g_loading = false;

void LoadCatalogNow();

const char* __cdecl GetByIdHook(unsigned int id) {
  // 🔴 LE CHARGEMENT SE FAIT ICI, AU PREMIER MESSAGE DEMANDÉ — jamais à
  // l'initialisation de Bourgeon.
  //
  // Bug payé le 2026-08-14 : lire la table depuis `Bourgeon::Initialize` faisait
  // MOURIR le client au lancement, sans une ligne de journal après « Bourgeon
  // 0.1.0 ». La cause est un problème d'ordre, pas de code : notre DLL est
  // chargée en proxy de DirectDraw, donc `Initialize` tourne AVANT que le client
  // ait monté ses archives — et on y appelait son gestionnaire de fichiers.
  //
  // Ce point-ci est la garantie qu'on cherchait : si le client réclame un
  // message, c'est que sa propre table est chargée, donc que son VFS est prêt.
  // `g_loading` couvre la réentrance — la validation des formats rappelle le
  // trampoline, qui ne repasse pas par ce hook, mais un handler tiers le pourrait.
  if (g_pending && !g_loading) {
    g_loading = true;
    LoadCatalogNow();
    g_loading = false;
    g_pending = false;
  }
  const char* ours = Lookup(static_cast<int>(id));
  if (ours) return ours;
  return g_orig_get_by_id ? g_orig_get_by_id(id) : "";
}

void InstallHook() {
  if (g_orig_get_by_id) return;
  using namespace hooking;
  g_orig_get_by_id = reinterpret_cast<GetById_t>(
      HookManager::Instance().SetHook(
          HookType::kJmpHook, reinterpret_cast<uint8_t*>(msgstr::kGetAddr),
          reinterpret_cast<uint8_t*>(&GetByIdHook)));
  g_stats.hooked = (g_orig_get_by_id != nullptr);
}

// Le vrai travail : lire le csv du client et le catalogue de la langue, puis
// construire la table `id -> texte`. ⚠ N'est JAMAIS appelée depuis
// l'initialisation de Bourgeon — voir le commentaire de `GetByIdHook`.
void LoadCatalogNow() {
  g_by_id.clear();
  g_stats.entries = 0;
  g_stats.rejected = 0;
  g_stats.language = i18n::LanguageCode();

  // Le client parle déjà anglais : rien à surcharger si c'est la langue demandée.
  if (g_stats.language.empty() || g_stats.language == "en") return;

  const std::string path =
      paths::LangPath("msgstring." + g_stats.language);
  std::ifstream in(path, std::ios::binary);
  if (!in) return;  // pas de catalogue pour cette langue : le natif répond seul

  const std::unordered_map<std::string, int> keys = ReadClientTable();
  if (keys.empty()) return;

  std::string line, key, value;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!ParseLine(line, &key, &value) || value.empty()) continue;

    const auto found = keys.find(key);
    if (found == keys.end()) continue;  // clé inconnue du client : on ignore
    const int id = found->second;

    // 🔴 LE CONTRÔLE QUI ÉVITE LE CRASH : la traduction doit consommer les mêmes
    // arguments que l'original. On interroge le NATIF pour l'original — pas le
    // csv relu — parce que c'est lui que le client passera à `printf`.
    const char* original = g_orig_get_by_id
                               ? g_orig_get_by_id(static_cast<unsigned>(id))
                               : nullptr;
    if (original && *original) {
      if (FormatSequence(original) != FormatSequence(value)) {
        ++g_stats.rejected;
        LogDiag("[msgstring] {} ({}) refusée : formats différents — « {} » vs « {} »",
                key, id, original, value);
        continue;
      }
    }

    // Conversion UNE fois, vers la code-page que le natif sait dessiner.
    const char* local = ro::Utf8ToLocal(value.c_str());
    if (!local) continue;
    g_storage.emplace_back(local);
    g_by_id[id] = g_storage.back().c_str();
    ++g_stats.entries;
  }

  LogInfo("[msgstring] {} : {} traduction(s) active(s), {} refusée(s) sur {} entrées",
          g_stats.language, g_stats.entries, g_stats.rejected, g_stats.table);
}

}  // namespace

const char* Lookup(int id) {
  const auto it = g_by_id.find(id);
  return (it == g_by_id.end()) ? nullptr : it->second;
}

void Reload() {
  // 🔴 CE QUI SE PASSE ICI DOIT ÊTRE INOFFENSIF À TOUT MOMENT. `Reload` est
  // appelée depuis `Bourgeon::Initialize`, c'est-à-dire depuis le proxy
  // DirectDraw, AVANT que le client ait fini de se mettre en place. On se
  // contente donc de poser le détour et de lever un drapeau : la lecture du csv
  // et du catalogue attend le premier message demandé par le client
  // (cf. `GetByIdHook`), seul instant où son VFS est garanti prêt.
  //
  // Le détour est posé UNE fois. La langue peut changer en cours de partie, et
  // reposer un hook sur une fonction que nous détournons déjà enchaînerait deux
  // trampolines.
  InstallHook();

  // Rechargement demandé : on repart d'une table vide, et le prochain message
  // relira tout. Les chaînes déjà rendues au client, elles, ne sont pas libérées
  // (cf. `g_storage`) — il peut encore en tenir des pointeurs.
  g_by_id.clear();
  g_stats.entries = 0;
  g_stats.rejected = 0;
  g_pending = true;
}

Stats Current() { return g_stats; }

}  // namespace msgoverride
