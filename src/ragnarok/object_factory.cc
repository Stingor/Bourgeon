#include "ragnarok/object_factory.h"

#include "ragnarok/object_layouts/session/layouts.h"
#include "utils/log_console.h"

Session::Pointer ObjectFactory::CreateSession(
    const YAML::Node& session_configuration) {
  Session::Pointer result;

  const auto session_layout = session_configuration["layout"];
  if (!session_layout.IsDefined()) {
    LogError("Missing required field 'layout' for Session");
    return nullptr;
  }

  // Une seule disposition vit encore ici — cf. object_layouts/session/layouts.h
  // pour la raison. Le `switch` est resté sous forme de test : c'est le point
  // d'accroche s'il faut un jour en rouvrir une seconde.
  try {
    const auto layout = session_layout.as<uint32_t>();
    if (layout == 20250716) {
      result = std::make_unique<Session_20250716>(session_configuration);
    } else {
      LogError("Unknown CSession layout {} (only 20250716 is implemented)",
               layout);
      result = nullptr;
    }

    return result;
  } catch (std::exception& ex) {
    LogError("CSession configuration is invalid: {}", ex.what());
    return nullptr;
  }
}

RagConnection::Pointer ObjectFactory::CreateRagConnection(
    const YAML::Node& ragconnection_configuration) noexcept {
  try {
    return std::make_unique<RagConnection>(ragconnection_configuration);
  } catch (std::exception& ex) {
    LogError("CRagConnection configuration is invalid: {}", ex.what());
    return nullptr;
  }
}

UIWindowMgr::Pointer ObjectFactory::CreateUIWindowMgr(
    const YAML::Node& uiwindowmgr_configuration) noexcept {
  try {
    return std::make_unique<UIWindowMgr>(uiwindowmgr_configuration);
  } catch (std::exception& ex) {
    // Message de LOG : il s adresse au developpeur, jamais au joueur.
    LogError(std::string("UIWindowMgr configuration is invalid") + ex.what());
    return nullptr;
  }
}

ModeMgr::Pointer ObjectFactory::CreateModeMgr(
    const YAML::Node& modemgr_configuration) noexcept {
  try {
    return std::make_unique<ModeMgr>(modemgr_configuration);
  } catch (std::exception& ex) {
    LogError("CModeMgr configuration is invalid: {}", ex.what());
    return nullptr;
  }
}

LoginMode::Pointer ObjectFactory::CreateLoginMode(
    const YAML::Node& login_mode_configuration) noexcept {
  try {
    return std::make_unique<LoginMode>(login_mode_configuration);
  } catch (std::exception& ex) {
    LogError("CLoginMode configuration is invalid: {}", +ex.what());
    return nullptr;
  }
}

GameMode::Pointer ObjectFactory::CreateGameMode(
    const YAML::Node& game_mode_configuration) noexcept {
  try {
    return std::make_unique<GameMode>(game_mode_configuration);
  } catch (std::exception& ex) {
    LogError("CGameMode configuration is invalid: {}", ex.what());
    return nullptr;
  }
}