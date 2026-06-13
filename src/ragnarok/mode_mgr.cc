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
static char s_current_map[64] = {};

void ModeMgr::FireModeSwitch(ModeType mode_type, const char* map_name) {
  const bool mode_changed = (s_current_mode != mode_type);
  const bool map_changed  = (strncmp(s_current_map, map_name, sizeof(s_current_map)) != 0);
  if (!mode_changed && !map_changed) return;

  s_current_mode = mode_type;
  strncpy_s(s_current_map, sizeof(s_current_map), map_name, _TRUNCATE);

  LogInfo("OnModeSwitch mode={} map={}", static_cast<int>(mode_type), map_name);
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
