#include "ragnarok/game_mode.h"

#include "bourgeon.h"
#include "imgui.h"
#include "ragnarok/mode_mgr.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// On some clients (20250716+) the "ProcessInput" address is actually
// CMode::SendMsg(int msg, int p1, int p2, int p3, int p4) — a __thiscall
// taking 5 stack args with callee cleanup (RET 0x14). Hooking it with the
// no-arg member hook below corrupts the caller's stack on early return (the
// caller's pushed args are left behind and its epilog pops them into
// EBX/ESI/EDI), crashing the game. For those clients the configuration sets
// ProcessInputArgs: 5 and we install this signature-correct hook instead.
// __fastcall with 5 stack params also emits RET 0x14, keeping the stack
// balanced on both the blocking and forwarding paths.
using ProcessInputMsg_t = int(__fastcall*)(void* ecx, void* edx, int msg,
                                           int p1, int p2, int p3, int p4);
static ProcessInputMsg_t g_orig_process_input_msg = nullptr;

static int __fastcall Hooked_ProcessInputMsg(void* ecx, void* edx, int msg,
                                             int p1, int p2, int p3, int p4) {
  ImGuiIO& io = ImGui::GetIO();
  // Suppress game input when ImGui needs the keyboard (text input active).
  if (io.WantCaptureKeyboard || io.WantTextInput) return 0;
  // Suppress game input during active mouse interactions on ImGui (click/drag).
  // Do NOT suppress on hover alone — that would swallow keyboard events (chat
  // Enter, etc.) whenever the mouse rests over the overlay.
  if (io.WantCaptureMouse && ImGui::IsAnyMouseDown()) return 0;
  return g_orig_process_input_msg(ecx, edx, msg, p1, p2, p3, p4);
}

GameMode::GameMode(const YAML::Node& game_mode_configuration) {
  using namespace hooking;

  // Hooks
  const auto onupdate_addr = game_mode_configuration["OnUpdate"];
  if (!onupdate_addr.IsDefined()) {
    throw std::exception("Missing required field 'OnUpdate' for CGameMode");
  }
  GameMode::OnUpdateRef = HookManager::Instance().SetHook(
      HookType::kJmpHook,
      reinterpret_cast<uint8_t*>(onupdate_addr.as<uint32_t>()),
      reinterpret_cast<uint8_t*>(void_cast(&GameMode::OnUpdateHook)));

  const auto processinput_addr = game_mode_configuration["ProcessInput"];
  if (!processinput_addr.IsDefined()) {
    throw std::exception("Missing required field 'ProcessInput' for CGameMode");
  }
  const auto processinput_args = game_mode_configuration["ProcessInputArgs"];
  if (processinput_args.IsDefined() && processinput_args.as<int>() == 5) {
    g_orig_process_input_msg = reinterpret_cast<ProcessInputMsg_t>(
        HookManager::Instance().SetHook(
            HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(processinput_addr.as<uint32_t>()),
            reinterpret_cast<uint8_t*>(Hooked_ProcessInputMsg)));
  } else {
    GameMode::ProcessInputRef = HookManager::Instance().SetHook(
        HookType::kJmpHook,
        reinterpret_cast<uint8_t*>(processinput_addr.as<uint32_t>()),
        reinterpret_cast<uint8_t*>(void_cast(&GameMode::ProcessInputHook)));
  }
}

void GameMode::OnUpdateHook() {
  ModeMgr::FireModeSwitch(ModeMgr::ModeType::kGame);
  Bourgeon::Instance().OnTick();
  return OnUpdateRef(this);
}

void GameMode::ProcessInputHook() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantCaptureKeyboard || io.WantTextInput) return;
  if (io.WantCaptureMouse && ImGui::IsAnyMouseDown()) return;
  return ProcessInputRef(this);
}

// References
MethodRef<GameMode, void (GameMode::*)()> GameMode::OnUpdateRef;
MethodRef<GameMode, void (GameMode::*)()> GameMode::ProcessInputRef;
