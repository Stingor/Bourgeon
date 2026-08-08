#include "utils/i18n.h"

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <ostream>
#include <set>
#include <string_view>

#include "utils/game_paths.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

namespace i18n {
namespace {

// ── Le catalogue ─────────────────────────────────────────────────────────────
// `std::map` et non `unordered_map`, pour une raison précise : en C++17 (le
// standard du projet, cf. CMakeLists.txt) seul le conteneur ORDONNÉ sait chercher
// par `std::string_view` sans construire de `std::string` — c'est le comparateur
// transparent `std::less<>`. La recherche hétérogène n'est arrivée aux tables de
// hachage qu'en C++20. Passer par unordered_map ici allouerait à CHAQUE libellé
// de CHAQUE frame (nos libellés dépassent largement les 15 caractères du SSO) ;
// l'arbre coûte une douzaine de comparaisons et zéro allocation.
std::map<std::string, std::string, std::less<>> g_catalog;

// La langue SOURCE. Tant qu'elle est active, le catalogue reste vide.
constexpr const char* kSourceLanguage = "fr";

std::string g_code = kSourceLanguage;

// ── Les textes réclamés mais absents du catalogue ────────────────────────────
// Trié : le gabarit exporté est stable d'une exécution à l'autre, donc lisible en
// diff plutôt que réordonné au hasard.
std::set<std::string> g_missing;

// 🔴 DEUX BORNES, et elles ne sont pas décoratives. `Tr` est censé recevoir des
// littéraux, mais rien dans le langage ne l'impose : le jour où un appel passe
// une chaîne CONSTRUITE (un nom de joueur, un libellé formaté), cet ensemble
// grossirait d'une entrée par valeur possible — sans fin, à la vitesse des
// frames. Les bornes transforment cette fuite en simple perte d'information.
constexpr std::size_t kMissingCap = 4096;
constexpr std::size_t kMaxKeyLength = 1024;

// ── Les langues connues ──────────────────────────────────────────────────────
// En dur, et c'est voulu : le combo doit proposer des langues NOMMÉES, pas tout
// fichier .yaml qui traîne dans le dossier. La disponibilité, elle, se lit sur le
// disque — déposer `es.yaml` active l'espagnol sans recompiler.
struct KnownLanguage {
  const char* code;
  const char* label;
};
constexpr KnownLanguage kKnownLanguages[] = {
    {"fr", "Français"},
    {"en", "English"},
    {"es", "Español"},
};

bool FileExists(const std::string& path) {
  return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Échappement YAML minimal, écrit à la main plutôt qu'avec YAML::Emitter : on
// veut la CERTITUDE que les octets UTF-8 des accents ressortent tels quels dans
// le gabarit. Un émetteur qui déciderait de les échapper en \xNN produirait un
// fichier que le traducteur ne saurait plus lire — et surtout des clés qui ne
// correspondraient plus, octet pour octet, à celles du code.
std::string YamlQuote(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  out += '"';
  for (const char raw : text) {
    // 🔴 `char` est SIGNÉ sur MSVC : sans ce cast, tout octet UTF-8 (>= 0x80)
    // serait négatif et tomberait dans la branche « caractère de contrôle ».
    // Chaque accent sortirait échappé, et le catalogue ne matcherait plus rien.
    const unsigned char byte = static_cast<unsigned char>(raw);
    if (raw == '"' || raw == '\\') {
      out += '\\';
      out += raw;
    } else if (raw == '\n') {
      out += "\\n";
    } else if (raw == '\t') {
      out += "\\t";
    } else if (byte < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\x%02X", byte);
      out += escaped;
    } else {
      out += raw;  // ASCII imprimable ET octets UTF-8 : intacts
    }
  }
  out += '"';
  return out;
}

// 🔴 La spec YAML limite une clé IMPLICITE — la forme `clé: valeur` — à 1024
// caractères ; yaml-cpp l'applique à la lettre (simplekey.cpp, VerifySimpleKey).
// Au-delà, la clé est invalidée et l'analyseur bute sur le « : » qu'il ne sait
// plus rattacher : « illegal map value », et c'est le fichier ENTIER qui est
// perdu, pas la seule ligne fautive.
//
// La parade est la forme EXPLICITE, qui n'a aucune limite de longueur :
//     ? "la très longue clé"
//     : "sa traduction"
// Nos libellés d'aide (les pavés de raccourcis) dépassent le millier d'octets.
constexpr std::size_t kMaxImplicitKeyBytes = 1000;  // marge sous les 1024

// Écrit une entrée sous la forme qui convient à la longueur de sa clé.
void WriteYamlEntry(std::ostream& out, const std::string& source,
                    const std::string& translated) {
  const std::string key = YamlQuote(source);
  const std::string value = YamlQuote(translated);
  if (key.size() > kMaxImplicitKeyBytes) {
    out << "? " << key << "\n: " << value << "\n";
  } else {
    out << key << ": " << value << "\n";
  }
}

}  // namespace

const char* Tr(const char* fr) {
  // Le français ne paie rien : le catalogue est vide, on ressort immédiatement.
  // C'est aussi le chemin pris quand le fichier de langue manque ou est illisible
  // — l'interface reste alors en français plutôt que de se vider.
  if (g_catalog.empty() || fr == nullptr || *fr == '\0') return fr;

  const std::string_view key(fr);
  const auto found = g_catalog.find(key);
  // Une traduction VIDE compte comme absente : c'est ce qui rend un gabarit
  // `.missing.yaml` rempli à moitié utilisable tel quel — les lignes pas encore
  // traduites continuent de sortir en français au lieu d'effacer le libellé.
  if (found != g_catalog.end() && !found->second.empty()) {
    return found->second.c_str();
  }

  if (key.size() <= kMaxKeyLength && g_missing.size() < kMissingCap) {
    g_missing.emplace(key);
  }
  return fr;
}

const char* TrId(const char* fr, const char* stable_id) {
  // Un ANNEAU, et non un tampon unique : `Widget(TrId(a, "x"), TrId(b, "y"))`
  // évalue ses deux arguments avant l'appel, et un tampon unique rendrait deux
  // fois le même pointeur — donc deux fois le second libellé. Huit places
  // couvrent très largement les expressions qu'on écrit.
  //
  // Les `std::string` gardent leur capacité d'une frame à l'autre : après les
  // premières frames, `assign` n'alloue plus.
  constexpr std::size_t kSlots = 8;
  static std::string ring[kSlots];
  static std::size_t next_slot = 0;

  std::string& slot = ring[next_slot];
  next_slot = (next_slot + 1) % kSlots;

  slot.assign(Tr(fr));
  slot += "###";
  slot += stable_id;
  return slot.c_str();
}

const std::string& LanguageCode() { return g_code; }

std::string& MutableLanguageCode() { return g_code; }

void ReloadCatalog() {
  g_catalog.clear();
  g_missing.clear();

  if (g_code.empty()) g_code = kSourceLanguage;
  if (g_code == kSourceLanguage) return;  // rien à charger : le code EST la langue

  const std::string path = paths::LangPath(g_code);
  std::ifstream file(path);
  if (!file) {
    // Pas une erreur de programmation : le joueur peut avoir choisi une langue
    // dont le fichier n'a pas (encore) été livré. On le dit une fois et on reste
    // en français — ce que produit naturellement un catalogue vide.
    LogDiag("[i18n] langue « {} » demandée mais {} est introuvable — interface en français",
            g_code, path);
    return;
  }

  try {
    const YAML::Node root = YAML::Load(file);
    if (!root.IsMap()) {
      LogDiag("[i18n] {} n'est pas une table clé/valeur — ignoré", path);
      return;
    }
    for (const auto& entry : root) {
      // `as<std::string>("")` plutôt qu'un accès direct : une valeur nulle ou
      // d'un autre type ne doit pas faire perdre TOUT le reste du fichier.
      std::string source = entry.first.as<std::string>("");
      std::string translated = entry.second.as<std::string>("");
      if (source.empty()) continue;
      g_catalog.emplace(std::move(source), std::move(translated));
    }
    LogInfo("[i18n] langue « {} » : {} entrées chargées depuis {}", g_code,
            g_catalog.size(), path);
  } catch (const std::exception& error) {
    // Un fichier corrompu laisse le catalogue à moitié rempli : on le VIDE, pour
    // que l'interface soit intégralement française plutôt qu'un panachage des
    // deux langues, qui se lit bien plus mal.
    g_catalog.clear();
    LogDiag("[i18n] {} illisible ({}) — interface en français", path, error.what());
  }
}

bool SetLanguage(const std::string& code) {
  const std::string wanted = code.empty() ? kSourceLanguage : code;
  if (wanted == g_code) return false;
  g_code = wanted;
  ReloadCatalog();
  return true;
}

std::vector<Language> AvailableLanguages() {
  std::vector<Language> languages;
  languages.reserve(sizeof(kKnownLanguages) / sizeof(kKnownLanguages[0]));
  for (const KnownLanguage& known : kKnownLanguages) {
    const bool is_source = std::string_view(known.code) == kSourceLanguage;
    languages.push_back({known.code, known.label,
                         is_source || FileExists(paths::LangPath(known.code))});
  }
  return languages;
}

const char* LabelOf(const std::string& code) {
  for (const KnownLanguage& known : kKnownLanguages) {
    if (code == known.code) return known.label;
  }
  return code.c_str();
}

std::size_t MissingCount() { return g_missing.size(); }

bool ExportMissing(std::string* out_path) {
  const std::string path = paths::LangMissingPath(g_code);
  if (out_path) *out_path = path;

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    LogDiag("[i18n] impossible d'écrire {}", path);
    return false;
  }

  // En-tête en ANGLAIS volontairement : ce gabarit part chez un traducteur qui,
  // par définition, ne lit peut-être pas le français.
  file << "# Bourgeon - untranslated UI strings for language \"" << g_code << "\".\n"
       << "# Key = the French source string, value = your translation.\n"
       << "# Leave a value empty to keep the French text for now.\n"
       << "# Move finished lines into " << g_code << ".yaml.\n\n";
  for (const std::string& source : g_missing) {
    WriteYamlEntry(file, source, std::string());
  }

  // ⚠ Vérifier APRÈS écriture : un disque plein ou un fichier verrouillé ne se
  // manifeste qu'ici, l'ouverture ayant réussi. Sans ce test on annoncerait au
  // joueur un export qui n'a rien écrit.
  file.flush();
  if (!file) {
    LogDiag("[i18n] écriture de {} incomplète", path);
    return false;
  }
  LogInfo("[i18n] {} textes non traduits exportés vers {}", g_missing.size(), path);
  return true;
}

}  // namespace i18n
