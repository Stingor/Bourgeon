#include "utils/game_paths.h"

#include <Windows.h>

namespace paths {

const std::string& GameDir() {
  // Initialisation d'une statique locale : garantie thread-safe et faite une
  // seule fois par le compilateur (C++11), donc pas de GetModuleFileNameA par
  // appel — il y en avait onze par lancement.
  static const std::string dir = [] {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    const auto sep = path.find_last_of("\\/");
    // Sans séparateur, on garde le nom tel quel : un chemin relatif se résout
    // alors depuis le dossier courant, ce qui vaut mieux qu'une chaîne vide.
    if (sep != std::string::npos) path.resize(sep + 1);
    return path;
  }();
  return dir;
}

std::string SettingsPath()       { return GameDir() + "bourgeon_settings.yaml"; }
std::string MoonlightUserPath()  { return GameDir() + "bourgeon_moonlight_user.txt"; }
std::string MoonlightPwPath()    { return GameDir() + "bourgeon_moonlight_pw.bin"; }
std::string LastCharsPath()      { return GameDir() + "bourgeon_last_chars.txt"; }
std::string RecipesPath() {
  // Le premier chemin QUI EXISTE, sinon SystemEN\ par défaut — ainsi le message
  // d'erreur du chargeur nomme l'emplacement attendu plutôt qu'un chemin au
  // hasard. Même ordre de préférence que LoadItemNames.
  static const char* kCandidates[] = {
      "SystemEN\\bourgeon_recipes.yaml",
      "System\\bourgeon_recipes.yaml",
  };
  for (const char* candidate : kCandidates) {
    const std::string path = GameDir() + candidate;
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
  }
  return GameDir() + kCandidates[0];
}

std::string InGameDir(const std::string& relative) { return GameDir() + relative; }

}  // namespace paths
