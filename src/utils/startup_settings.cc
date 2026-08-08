#include "utils/startup_settings.h"

#include <string>

#include "utils/game_paths.h"

namespace startup {
namespace {

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

}  // namespace startup
