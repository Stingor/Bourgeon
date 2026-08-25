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
std::string CharSelectLayoutPath() {
  return GameDir() + "bourgeon_charselect_layout.yaml";
}
std::string ChatLayoutPath() {
  // Dans SaveData\, auprès du ChatWndInfo_U.lua du client : c'est la même nature
  // de donnée — la disposition du chat de CE joueur. Le dossier est créé s'il
  // manque ; `_mkdir` n'étant pas récursif côté client, c'est exactement ce qui
  // rend son /savechat muet quand SaveData\ n'existe pas encore.
  const std::string dir = GameDir() + "SaveData";
  CreateDirectoryA(dir.c_str(), nullptr);  // ERROR_ALREADY_EXISTS : sans importance
  return dir + "\\bourgeon_chat.yaml";
}

std::string ChatHistoryPath() {
  const std::string dir = GameDir() + "SaveData";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir + "\\bourgeon_chat_history.yaml";
}

std::string StartupSettingsPath() {
  const std::string dir = GameDir() + "SaveData";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir + "\\bourgeon_startup.yaml";
}

std::string PaletteCachePath() {
  const std::string dir = GameDir() + "SaveData";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir + "\\bourgeon_palettes.yaml";
}

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

namespace {

// `SaveData\lang\`, créé au besoin. Les deux niveaux sont créés séparément :
// CreateDirectoryA n'est pas récursif, et SaveData\ peut très bien ne pas
// exister encore — c'est exactement ce qui rend le /savechat du client muet au
// premier lancement (cf. ChatLayoutPath).
std::string LangDir() {
  const std::string save_data = GameDir() + "SaveData";
  CreateDirectoryA(save_data.c_str(), nullptr);
  const std::string dir = save_data + "\\lang";
  CreateDirectoryA(dir.c_str(), nullptr);  // ERROR_ALREADY_EXISTS : sans importance
  return dir;
}

}  // namespace

std::string LangPath(const std::string& code) {
  return LangDir() + "\\" + code + ".yaml";
}

std::string LangMissingPath(const std::string& code) {
  return LangDir() + "\\" + code + ".missing.yaml";
}

bool GameDirW(std::wstring& out) {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  const std::wstring path(buf, n);
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) return false;
  out = path.substr(0, slash + 1);
  return true;
}

std::string InGameDir(const std::string& relative) { return GameDir() + relative; }

std::string ClientInfoPath() { return InGameDir("data\\clientinfo.xml"); }

}  // namespace paths
