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

// CMode::SendMsg (the ProcessInput target @0x00c86740) is the game's GENERAL,
// RE-ENTRANT message dispatch — used for real input AND internal data queries
// (e.g. SendMsg(9) = current map name, during world-map init). We do NOT suppress
// input here: ImGui input blocking is handled at the WndProc level (WindowProcHook
// in ragnarok_client.cc drops WM_* mouse/keyboard when the cursor is over ImGui).
// Suppressing SendMsg here returned NULL to those internal queries and crashed
// the world-map open on strlen(NULL). The depth counter only lets the queued-icon
// dispatch run once, at the outermost (top-level) call.
static int g_send_msg_depth = 0;
struct SendMsgDepthGuard {
  SendMsgDepthGuard() { ++g_send_msg_depth; }
  ~SendMsgDepthGuard() { --g_send_msg_depth; }
};

static int __fastcall Hooked_ProcessInputMsg(void* ecx, void* edx, int msg,
                                             int p1, int p2, int p3, int p4) {
  SendMsgDepthGuard depth;
  if (g_send_msg_depth == 1)  // top-level call only
    Bourgeon::Instance().OnProcessInput();  // dispatch queued icon clicks
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
  // Heartbeat: this hook runs only while CGameMode is the actively-updating mode
  // (the in-world game loop), so it is Bourgeon's authoritative "in game" signal
  // — more reliable than the mode-switch event, which the char-change path does
  // not always re-fire. RenderUI() gates plugin UI on its freshness.
  Bourgeon::Instance().NotifyGameUpdate();
  Bourgeon::Instance().OnTick();
  return OnUpdateRef(this);
}

void GameMode::ProcessInputHook() {
  SendMsgDepthGuard depth;
  if (g_send_msg_depth == 1)  // top-level call only
    Bourgeon::Instance().OnProcessInput();  // dispatch queued icon clicks
  return ProcessInputRef(this);
}

// References
MethodRef<GameMode, void (GameMode::*)()> GameMode::OnUpdateRef;
MethodRef<GameMode, void (GameMode::*)()> GameMode::ProcessInputRef;
