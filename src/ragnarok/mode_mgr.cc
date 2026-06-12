#include "ragnarok/mode_mgr.h"

#include "bourgeon.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

ModeMgr::ModeMgr(const YAML::Node& modemgr_configuration) {
  using namespace hooking;

  // Hooks
  const auto switch_addr = modemgr_configuration["Switch"];
  if (!switch_addr.IsDefined()) {
    throw std::exception("Missing required field 'Switch' for ModeMgr");
  }
  ModeMgr::SwitchRef = HookManager::Instance().SetHook(
      HookType::kJmpHook,
      reinterpret_cast<uint8_t*>(switch_addr.as<uint32_t>()),
      reinterpret_cast<uint8_t*>(void_cast(&ModeMgr::SwitchHook)));
}

static ModeMgr::ModeType s_current_mode = static_cast<ModeMgr::ModeType>(-1);

void ModeMgr::FireModeSwitch(ModeType mode_type, const char* map_name) {
  if (s_current_mode == mode_type) return;
  s_current_mode = mode_type;

  LogInfo("OnModeSwitch mode={}", static_cast<int>(mode_type));
  Bourgeon::Instance().FireModeSwitch(mode_type, map_name);
}

void ModeMgr::SwitchHook(ModeType mode_type, char const* map_name) {
  FireModeSwitch(mode_type, map_name);
  return SwitchRef(this, mode_type, map_name);
}

// References
MethodRef<ModeMgr,
          void (ModeMgr::*)(ModeMgr::ModeType mode_type, char const* mode_name)>
    ModeMgr::SwitchRef;
