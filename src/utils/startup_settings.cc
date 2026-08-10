#include "utils/startup_settings.h"

#include <fstream>
#include <string>

#include "utils/game_paths.h"
#include "utils/log_console.h"

namespace startup {
namespace {

// Les clés, sous la racine du fichier de démarrage.
constexpr const char* kUiFontFamilyKey = "ui_font_family";
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

}  // namespace

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
  const std::string path = paths::StartupSettingsPath();
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const std::exception&) {
    // Absent au premier lancement, ou illisible : on repart d'un document vide
    // plutôt que de renoncer à enregistrer le choix du joueur.
  }
  if (!root.IsMap()) root = YAML::Node(YAML::NodeType::Map);
  root[kUiFontFamilyKey] = family;

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    LogDiag("[startup] impossible d'écrire {} — la police ne sera pas retenue",
            path);
    return;
  }
  file << YAML::Dump(root) << "\n";
  file.flush();
  // ⚠ Vérifier APRÈS écriture : un disque plein ou un fichier verrouillé ne se
  // manifeste qu'ici, l'ouverture ayant réussi.
  if (!file) LogDiag("[startup] écriture de {} incomplète", path);
}

}  // namespace startup
