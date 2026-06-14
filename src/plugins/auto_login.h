#pragma once

#include <string>

#include "plugins/plugin.h"

// Auto-login: reads credentials supplied on the client command line
//
//     ragexe.exe --login:<user> --pass:<password> --server:<server name>
//
// (or, as a fallback, from the `auto_login` section of bourgeon_settings.yaml)
// and drives the login screens automatically so the client lands on character
// select without manual input.
//
// Flow handled (everything below happens inside the client's "login mode", so
// the stages are sequenced on a timer rather than on mode switches):
//
//   1. Server-select screen — shown only when clientinfo.xml has more than one
//      <connection>. We pick the entry matching --server: by its position in
//      clientinfo.xml (the same order the client lists them in).
//   2. Login screen — type the account id / password and submit.
//   3. Char-server select — confirm (single-server lists just need Enter).
//
// Input is delivered by posting WM_CHAR / WM_KEYDOWN messages to the game
// window — the same path a real keystroke takes — so this needs no knowledge
// of the client's UI object layout.
class AutoLogin : public Plugin {
 public:
  AutoLogin();

  const char* name() const override { return "Auto Login"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnTick() override;

 private:
  enum class Stage {
    kDisabled,      // no credentials supplied — plugin is inert
    kIdle,          // armed, waiting until we enter the login screen
    kSettle,        // login phase started, waiting for the first screen
    kSelectServer,  // pick the connection on the server-select list
    kWaitLogin,     // wait for the id/password window after selecting a server
    kCredentials,   // type id / password and submit
    kCharServer,    // confirm the char-server select screen
    kDone,          // finished — don't run again this session
  };

  bool ParseCommandLine();
  void LoadFromYaml();

  // Reads data\clientinfo.xml and resolves how the server-select list will look:
  //   server_count_  — number of <connection> entries (0 if file unreadable)
  //   server_index_  — 0-based position of `server_` in that list (0 if not
  //                    found, so we fall back to the first/default entry)
  void ResolveServerFromClientInfo();

  // Posts the characters of `text` as WM_CHAR messages to the game window.
  void TypeString(const std::string& text);
  // Posts a WM_KEYDOWN + WM_KEYUP for a virtual key to the game window.
  void PressKey(int vkey);

  Stage stage_ = Stage::kDisabled;
  int tick_counter_ = 0;

  std::string login_;
  std::string password_;
  std::string server_;

  // Which login field is focused when the window opens depends on the client's
  // "Save ID" checkbox: checked -> ID is pre-filled, so PASSWORD is focused;
  // unchecked -> the ID field is focused. We type in the matching order.
  // Override with --saveid:true|false or `auto_login: { save_id: }` in the yaml.
  bool save_id_ = true;

  int server_count_ = 0;  // <connection> entries in clientinfo.xml
  int server_index_ = 0;  // position of server_ in that list

  // Ticks (~100ms each) to wait at each stage. Generous defaults; tune against
  // the live server if a screen needs more time to appear.
  static constexpr int kSettleTicks = 1;
  static constexpr int kWaitLoginTicks = 2;
  static constexpr int kCharServerTicks = 5;
};
