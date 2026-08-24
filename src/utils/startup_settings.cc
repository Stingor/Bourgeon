#include "utils/startup_settings.h"

#include <fstream>
#include <string>

#include "utils/game_paths.h"
#include "utils/log_console.h"

namespace startup {
namespace {

// Les clés, sous la racine du fichier de démarrage.
constexpr const char* kUiFontFamilyKey = "ui_font_family";
constexpr const char* kUiScaleKey = "ui_scale_percent";
// L'ancien booléen « police Malgun » : faux = police intégrée d'ImGui. Il n'a
// jamais vécu qu'au vieil emplacement, d'où sa lecture en repli seulement.
constexpr const char* kLegacyMalgunKey = "malgun_font";
// La section qui portait les deux, dans bourgeon_settings.yaml.
constexpr const char* kLegacySection = "moonlight_ui";

// Charge un document, ou un nœud invalide si le fichier manque ou ne s'analyse
// pas. Pas de journalisation ici : l'absence est le cas NORMAL au premier
// lancement, et un fichier illisible se signalera de lui-même chez l'appelant,
// qui retombera sur ses valeurs usine.
YAML::Node LoadDocument(const std::string& path) {
  try {
    return YAML::LoadFile(path);
  } catch (const std::exception&) {
    return YAML::Node();
  }
}

// Pose UNE clé entière à la racine du fichier de démarrage, en préservant tout
// le reste.
//
// 🔴 On RELIT puis on remplace la seule clé : ce document porte aussi la langue
// et les sections `auto_login`, `char_select` et `moonlight_auth`. L'écrire à
// plat le tronquerait — changer de police effacerait les identifiants
// d'auto-login.
//
// `what` nomme le réglage dans le journal : « impossible d'écrire … » sans dire
// QUOI n'a pas été retenu n'aide personne à comprendre ce qu'il vient de perdre.
// Un nom NU (« police de l'interface »), sans article : la phrase l'encadre de
// guillemets, ce qui lui évite d'avoir à s'accorder avec le réglage du jour.
template <typename T>
void SaveRootKeyImpl(const char* key, const T& value, const char* what) {
  const std::string path = paths::StartupSettingsPath();
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const std::exception&) {
    // Absent au premier lancement, ou illisible : on repart d'un document vide
    // plutôt que de renoncer à enregistrer le choix du joueur.
  }
  if (!root.IsMap()) root = YAML::Node(YAML::NodeType::Map);
  root[key] = value;

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    LogDiag("[startup] impossible d'écrire {} — réglage « {} » non retenu", path,
            what);
    return;
  }
  file << YAML::Dump(root) << "\n";
  file.flush();
  // ⚠ Vérifier APRÈS écriture : un disque plein ou un fichier verrouillé ne se
  // manifeste qu'ici, l'ouverture ayant réussi.
  if (!file) LogDiag("[startup] écriture de {} incomplète", path);
}

}  // namespace

void SaveRootKey(const char* key, int value, const char* what) {
  SaveRootKeyImpl(key, value, what);
}
void SaveRootKey(const char* key, const std::string& value, const char* what) {
  SaveRootKeyImpl(key, value, what);
}

YAML::Node Section(const char* name) {
  const YAML::Node startup_root = LoadDocument(paths::StartupSettingsPath());
  if (startup_root.IsMap()) {
    if (const YAML::Node section = startup_root[name]) return section;
  }

  // Pas dans le nouveau fichier : l'ancien fait foi tant que le joueur n'a pas
  // déménagé sa configuration. On ne recopie pas — cf. l'en-tête.
  const YAML::Node legacy_root = LoadDocument(paths::SettingsPath());
  if (legacy_root.IsMap()) {
    if (const YAML::Node section = legacy_root[name]) return section;
  }
  return YAML::Node();
}

int UiFontFamily(int fallback) {
  const YAML::Node startup_root = LoadDocument(paths::StartupSettingsPath());
  if (startup_root.IsMap()) {
    if (const YAML::Node family = startup_root[kUiFontFamilyKey])
      return family.as<int>(fallback);
  }

  const YAML::Node legacy_root = LoadDocument(paths::SettingsPath());
  if (!legacy_root.IsMap()) return fallback;
  const YAML::Node ui = legacy_root[kLegacySection];
  if (!ui || !ui.IsMap()) return fallback;

  // 🔴 DANS CET ORDRE, comme le faisait la lecture d'origine : le booléen est
  // l'ancêtre de la famille, donc c'est la famille qui tranche. Un yaml
  // antérieur à celle-ci n'a que le booléen, et on retombe alors exactement sur
  // le comportement qu'avait ce client-là.
  int family = fallback;
  if (const YAML::Node malgun = ui[kLegacyMalgunKey])
    if (!malgun.as<bool>(true)) family = -1;
  if (const YAML::Node saved = ui[kUiFontFamilyKey])
    family = saved.as<int>(family);
  return family;
}

void SaveUiFontFamily(int family) {
  SaveRootKey(kUiFontFamilyKey, family, "police de l'interface");
}

// Pas de repli sur l'ancien fichier, contrairement à la police : ce réglage
// n'a jamais vécu ailleurs. Et aucune borne ici — c'est ro::SetUiScalePercent
// qui sait ce qu'une échelle valide veut dire, et lui seul.
int UiScalePercent(int fallback) {
  const YAML::Node root = LoadDocument(paths::StartupSettingsPath());
  if (!root.IsMap()) return fallback;
  const YAML::Node scale = root[kUiScaleKey];
  if (!scale) return fallback;
  return scale.as<int>(fallback);
}

void SaveUiScalePercent(int percent) {
  SaveRootKey(kUiScaleKey, percent, "échelle de l'interface");
}

}  // namespace startup
