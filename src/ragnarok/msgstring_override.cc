#include "ragnarok/msgstring_override.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ragnarok/file_mgr.h"
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

// 🔴 INDEXÉ PAR LE TEXTE ANGLAIS, PAS PAR L'ID — et ce n'est pas un détail de
// mise en œuvre, c'est ce qui rend la traduction JUSTE.
//
// Première version : `id -> traduction`, l'id étant le numéro de ligne du csv.
// Faux. Constaté en jeu le 2026-08-14 : le titre du panneau, demandé à l'id 4241
// où le csv place `MSI_OPTION_ESC`, sortait « Indoor teleport is not supported. »
// — et les ids 4216/4217 rendaient « NO MSG » quand le natif, lui, affichait le
// bon libellé. Le client résout ses ids autrement que par la position dans le
// fichier, et bâtir la table sur cette hypothèse revenait à distribuer les
// traductions au hasard passé un certain rang.
//
// On ne résout donc plus rien : le hook demande au CLIENT son texte anglais
// (par le trampoline) et cherche CE TEXTE. La correspondance
// « clé MSI_* -> texte anglais » vient du csv, où elle est sur la même ligne,
// donc à l'abri de tout décalage d'indexation.
//
// ⚠ Deux entrées de même texte anglais partagent leur traduction. C'est voulu :
// ce sont les mêmes mots, ils doivent se dire pareil.
std::unordered_map<std::string, const char*> g_by_text;

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

// ── Espaces de bord ──────────────────────────────────────────────────────────
// 🔴 UN ESPACE DE BORD EST UN CONTRAT, AU MÊME TITRE QU'UN `%s`.
//
// Le client CONCATÈNE une partie de ces textes au lieu de les afficher seuls :
// « Beloved » (MSI_NAMED_PET) est PRÉFIXÉ au nom d'un œuf de familier, « 's Fire »
// s'insère entre le nom d'un forgeron et celui de sa lame, « Price: » précède un
// montant. L'espace final n'est pas de la mise en forme, c'est le SÉPARATEUR.
// D'autres entrées ouvrent sur des espaces d'ALIGNEMENT dans un widget natif
// (« ....BGM »), qui jouent le même rôle positionnel.
//
// Et un traducteur le perd forcément, parce qu'un espace en fin de chaîne NE SE
// VOIT PAS. Le relevé sur nos deux catalogues en a dénombré 100 par langue —
// dont le « Bien-aimé[carré]ring Egg » qui a mené ici. Compter sur la vigilance
// humaine pour un caractère invisible, c'est reconduire le bug à chaque langue
// ajoutée et à chaque ligne retouchée.
//
// On RESTAURE plutôt qu'on ne refuse, contrairement aux formats. Refuser
// jetterait une traduction juste pour un caractère qu'on ne peut pas voir, alors
// que recopier les bords de l'original ne peut jamais faire pire que l'original
// lui-même — c'est lui le contrat. Corollaire assumé : une traduction ne choisit
// PAS ses propres espaces de bord.
std::string WithOriginalEdges(const std::string& english, const std::string& value) {
  const std::size_t lead = english.find_first_not_of(' ');
  if (lead == std::string::npos) return value;  // original tout blanc : rien à copier
  const std::size_t trail = english.size() - 1 - english.find_last_not_of(' ');

  const std::size_t begin = value.find_first_not_of(' ');
  if (begin == std::string::npos) return value;  // traduction toute blanche
  const std::size_t end = value.find_last_not_of(' ');

  return std::string(lead, ' ') + value.substr(begin, end - begin + 1) +
         std::string(trail, ' ');
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

using LoadToMemoryFn = void*(__fastcall*)(void*, void*, const char*, DWORD*, char);
using FreeBufferFn   = int(__stdcall*)(void*);

// Défini plus bas : le dés-échappement, partagé par le csv du client et par
// notre catalogue — les deux emploient les mêmes séquences.
std::string Unescape(const std::string& s);

// ⚠ Fonction SÉPARÉE, et sans le moindre objet C++ : MSVC refuse `__try` dans une
// fonction qui doit dérouler des destructeurs (C2712).
void* LoadClientFile(const char* path, DWORD* size) {
  __try {
    return reinterpret_cast<LoadToMemoryFn>(filemgr::kLoadToMemoryAddr)(
        reinterpret_cast<void*>(filemgr::kFileMgrAddr), nullptr, path, size, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// clé `MSI_*` -> texte ANGLAIS, tel que le client l'affichera. Vide si le csv est
// introuvable.
//
// ⚠ Le texte est DÉS-ÉCHAPPÉ ici, comme le fait le parseur du client
// (`sub_A9E800`) au chargement : c'est la forme dés-échappée que rendra
// `GetById`, donc celle qu'il faut indexer pour la retrouver.
std::unordered_map<std::string, std::string> ReadClientTable() {
  std::unordered_map<std::string, std::string> keys;

  DWORD size = 0;
  void* buffer = LoadClientFile("data\\msgstringtable.csv", &size);

  if (!buffer || size == 0) {
    LogDiag("[msgstring] data\\msgstringtable.csv illisible — "
            "aucune traduction du client possible");
    if (buffer) reinterpret_cast<FreeBufferFn>(filemgr::kFreeBufferAddr)(buffer);
    return keys;
  }

  // Deux colonnes en base64 sur la même ligne : la clé, puis le texte. C'est
  // cette CO-LOCALISATION qui fait la valeur du fichier — la paire est juste
  // quelle que soit la façon dont le client numérote ses messages.
  //
  // ⚠ Une ligne du fichier porte TROIS colonnes (un texte à deux valeurs) : on
  // prend la deuxième, comme le client.
  const char* p = static_cast<const char*>(buffer);
  const char* end = p + size;
  while (p < end) {
    const char* eol = p;
    while (eol < end && *eol != '\n') ++eol;
    const char* stop = eol;
    if (stop > p && stop[-1] == '\r') --stop;

    const char* comma = p;
    while (comma < stop && *comma != ',') ++comma;
    if (comma > p && comma < stop) {
      const char* second = comma + 1;
      const char* comma2 = second;
      while (comma2 < stop && *comma2 != ',') ++comma2;
      std::string key = Base64Decode(p, static_cast<std::size_t>(comma - p));
      std::string text =
          Base64Decode(second, static_cast<std::size_t>(comma2 - second));
      if (!key.empty() && !text.empty())
        keys.emplace(std::move(key), Unescape(text));
    }
    p = (eol < end) ? eol + 1 : end;
  }

  reinterpret_cast<FreeBufferFn>(filemgr::kFreeBufferAddr)(buffer);
  g_stats.table = keys.size();
  return keys;
}

// ── Lecture du catalogue Bourgeon ────────────────────────────────────────────
// Même format que en.yaml / es.yaml : `"clé": "valeur"`, une par ligne, les
// commentaires en `#`. Un mini-parseur suffit et évite de faire dépendre un
// module `ragnarok/` de yaml-cpp pour deux caractères de syntaxe.

// ⚠ LES MÊMES SÉQUENCES QUE LE PARSEUR DU CLIENT, ni plus ni moins
// (`sub_A9E800` : `\r \n \t \' \" \\`). Le `\'` avait été oublié à la première
// écriture — un apostrophe échappé serait ressorti avec son antislash.
//
// 🔴 Une séquence INCONNUE garde son antislash. Le client fait pareil, et les
// chemins de ressources en dépendent : « data\aura » perdrait son séparateur si
// l'on avalait le `\a`.
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
        case '\'': out.push_back('\''); break;
        case '\\': out.push_back('\\'); break;
        default:   out.push_back('\\'); out.push_back(s[i]); break;
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

  // On demande TOUJOURS son texte au client d'abord : c'est lui la clé de
  // recherche, et c'est aussi le repli quand la traduction manque.
  const char* original = g_orig_get_by_id ? g_orig_get_by_id(id) : "";
  if (!original || !*original || g_by_text.empty()) return original ? original : "";

  const auto it = g_by_text.find(original);
  return (it == g_by_text.end()) ? original : it->second;
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
  g_by_text.clear();
  g_stats.entries = 0;
  g_stats.rejected = 0;
  g_stats.restored = 0;
  g_stats.language = i18n::LanguageCode();

  // Le client parle déjà anglais : rien à surcharger si c'est la langue demandée.
  if (g_stats.language.empty() || g_stats.language == "en") return;

  const std::string path =
      paths::LangPath("msgstring." + g_stats.language);
  std::ifstream in(path, std::ios::binary);
  if (!in) return;  // pas de catalogue pour cette langue : le natif répond seul

  const std::unordered_map<std::string, std::string> keys = ReadClientTable();
  if (keys.empty()) return;

  std::string line, key, value;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!ParseLine(line, &key, &value) || value.empty()) continue;

    const auto found = keys.find(key);
    if (found == keys.end()) continue;  // clé inconnue du client : on ignore
    const std::string& english = found->second;
    if (english.empty()) continue;

    // 🔴 LE CONTRÔLE QUI ÉVITE LE CRASH : la traduction doit consommer les mêmes
    // arguments que l'original, sinon le client passe ses arguments à un format
    // qui ne les attend pas.
    if (FormatSequence(english) != FormatSequence(value)) {
      ++g_stats.rejected;
      LogDiag("[msgstring] {} refusée : formats différents — « {} » vs « {} »",
              key, english, value);
      continue;
    }

    // 🔴 Les espaces de bord de l'original sont RESTAURÉS : ils font partie du
    // contrat au même titre que les formats, et un traducteur ne peut pas les
    // voir (cf. `WithOriginalEdges`).
    const std::string fixed = WithOriginalEdges(english, value);
    if (fixed != value) ++g_stats.restored;

    // Conversion UNE fois, vers la code-page que le natif sait dessiner.
    const char* local = ro::Utf8ToLocal(fixed.c_str());
    if (!local) continue;
    g_storage.emplace_back(local);
    // ⚠ `emplace` et non `operator[]` : deux clés peuvent porter le MÊME texte
    // anglais, et la première traduction gagne. Écraser reviendrait à laisser le
    // dernier passage décider, sans que rien ne le signale.
    g_by_text.emplace(english, g_storage.back().c_str());
    ++g_stats.entries;
  }

  LogInfo("[msgstring] {} : {} traduction(s) active(s), {} refusée(s), "
          "{} bord(s) restauré(s) sur {} entrées",
          g_stats.language, g_stats.entries, g_stats.rejected, g_stats.restored,
          g_stats.table);
}

}  // namespace

const char* Lookup(int id) {
  if (!g_orig_get_by_id || g_by_text.empty()) return nullptr;
  const char* original = g_orig_get_by_id(static_cast<unsigned>(id));
  if (!original || !*original) return nullptr;
  const auto it = g_by_text.find(original);
  return (it == g_by_text.end()) ? nullptr : it->second;
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
  g_by_text.clear();
  g_stats.entries = 0;
  g_stats.rejected = 0;
  g_stats.restored = 0;
  g_pending = true;
}

Stats Current() { return g_stats; }

}  // namespace msgoverride
