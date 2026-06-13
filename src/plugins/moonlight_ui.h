#pragma once

#include <cstdint>
#include "plugins/plugin.h"

// Moonlight-Destiny settings panel — manages client/server settings sync.
class MoonlightUi : public Plugin {
 public:
  MoonlightUi();

  const char* name() const override { return "Moonlight-Destiny UI"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnRenderUI() override;

 private:
  // Sends a single setting change to the server.
  // CZ: [opcode:2][total_len:2][id:2][value:2]
  void SendSetting(uint16_t id, uint16_t value);

  static constexpr uint16_t kOpcodeFromServer = 0x0BFE;  // ZC_BOURGEON_SETTINGS
  static constexpr uint16_t kOpcodeToServer   = 0x0BFD;  // CZ_BOURGEON_SETTING

  // Setting IDs (must match server-side switch in clif_parse_bourgeon_setting)
  static constexpr uint16_t kSettingDiscord = 0;

  // Updates both directions of the relay based on current state.
  void UpdateRelay();

  static constexpr const char* kDiscordMap = "gonryun";

  bool in_game_    = false;
  bool in_gonryun_ = false;
  bool discord_chat_ = false;

  // ── Chat window background color ─────────────────────────────────────────
  // The chat window init stores an ARGB color (default 0x66000000 = 40% alpha
  // black) at object+0xD8.  We patch the MOV immediate in .text at startup so
  // new windows use our color, and write object+0xD8 on existing instances via
  // a heap walk triggered when the user releases the color-picker drag.
  //
  // Pattern searched in .text: C7 ?? D8 00 00 00 ?? ?? ?? ??
  //   C7 ??            = MOV dword ptr [reg+disp32], imm32  (any register)
  //   D8 00 00 00      = displacement 0xD8
  //   ?? ?? ?? ??      = the 4-byte ARGB immediate  ← chat_bg_instr_ points here

  // Chat-window vtable address (20250716 client).
  static constexpr uint32_t kChatWinVtable  = 0x01037F80;
  // Byte offset of the ARGB color field inside the chat window object.
  static constexpr uint32_t kChatBgColorOff = 0xD8;

  // Scans .text for the init instruction and makes its immediate writable.
  // Called once in the constructor (before the chat window is created).
  void FindChatBgInstruction();

  // Writes argb to the instruction immediate (cheap: 4-byte write + icache flush).
  void PatchInstruction(uint32_t argb);

  // Walks the process heap and writes argb to every chat window object's +0xD8.
  // Relatively expensive; call only when the user releases the color picker.
  void PatchExistingObjects(uint32_t argb);

  // Converts the current ImGui float[4] RGBA picker state to a packed ARGB uint32.
  uint32_t PickerToArgb() const;

  // Pointer into the writable .text section at the 4-byte ARGB immediate.
  // Null if the pattern was not found.
  uint32_t* chat_bg_instr_ = nullptr;

  // ImGui color picker state: RGBA, each channel in [0.0, 1.0].
  float chat_bg_color_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};
