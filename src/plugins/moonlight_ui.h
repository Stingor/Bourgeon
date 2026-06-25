#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
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
  void SendSetting(uint16_t id, uint32_t value);

  static constexpr uint16_t kOpcodeFromServer    = 0x0BFE;  // ZC_BOURGEON_SETTINGS
  static constexpr uint16_t kOpcodeToServer      = 0x0BFD;  // CZ_BOURGEON_SETTING
  static constexpr uint16_t kOpcodePresetList    = 0x0C21;  // ZC_BOURGEON_PRESET_LIST
  static constexpr uint16_t kOpcodePresetCmd     = 0x0C20;  // CZ_BOURGEON_PRESET_CMD
  // We read the current map name from the STANDARD client packet 0x0091
  // (ZC_NPCACK_MAPMOVE), which arrives on login and every warp/map change and
  // carries mapname[16] right after the opcode.  This needs no custom packet
  // and no server changes, so there is no opcode to collide with the client's
  // packet-length table (0x0BFC and 0x0BFF both turned out to be reserved).
  static constexpr uint16_t kOpcodeMapMove    = 0x0091;  // [opcode:2][mapname:16][x:2][y:2]
  static constexpr uint16_t kMapNameLen       = 16;      // mapname field width in 0x0091

  // Setting IDs sent via CZ 0x0BFD (must match server-side clif_parse_bourgeon_setting).
  static constexpr uint16_t kSettingShowExp     = 0;
  static constexpr uint16_t kSettingShowZeny    = 1;
  static constexpr uint16_t kSettingShowMobInfo = 2;
  static constexpr uint16_t kSettingSeparate    = 3;
  static constexpr uint16_t kSettingBlockExp    = 4;
  static constexpr uint16_t kSettingAlootRare   = 5;
  static constexpr uint16_t kSettingAlootRate   = 6;  // autoloot rate 0-100
  static constexpr uint16_t kSettingAlootPognon = 7;  // zeny threshold / 100 on wire
  static constexpr uint16_t kSettingAlootType   = 8;  // autoloottype toggle
  static constexpr uint16_t kSettingDiscordChat  = 9;  // discord relay opt-in
  static constexpr uint16_t kSettingShowDelay   = 10;
  static constexpr uint16_t kSettingShowSpeed   = 11;
  static constexpr uint16_t kSettingSellStuff   = 12;
  static constexpr uint16_t kSettingSellItem    = 13;
  static constexpr uint16_t kSettingNoAsk       = 14;
  static constexpr uint16_t kSettingNoks        = 15;  // 0=off 1=self 2=party 3=guild
  static constexpr uint16_t kSettingWings       = 16;
  static constexpr uint16_t kSettingAlootMvp      = 17;
  static constexpr uint16_t kSettingAlootMvpRwd   = 18;
  static constexpr uint16_t kSettingTriInv        = 19;
  static constexpr uint16_t kSettingTriCart       = 20;
  static constexpr uint16_t kSettingTriStorage    = 21;
  static constexpr uint16_t kSettingTriGstorage   = 22;
  static constexpr uint16_t kSettingAlootId       = 23;  // add item ID to autolootid list (0=clear)
  static constexpr uint16_t kSettingAlootIdRemove = 24;  // remove item ID from list

  // Updates both directions of the relay based on current state.
  void UpdateRelay();

  static constexpr char kDiscordMap[] = "gonryun";  // sizeof - 1 = 7, correct prefix length

  bool in_game_    = false;
  bool in_gonryun_ = false;

  // Server-synced (ID 9); restored from globalreg on login.
  bool discord_chat_ = false;

  // Server-synced toggles (IDs above).
  bool show_exp_      = false;
  bool show_zeny_     = false;
  bool show_mob_info_ = false;
  bool separate_      = false;
  bool block_exp_     = false;
  bool aloot_rare_    = false;
  int  aloot_rate_    = 0;    // 0-100 (%)
  int  aloot_pognon_  = 0;    // 0-1,000,000 (z), stored locally; wire = /100
  int  aloot_type_mask_ = 0;  // bitmask uint16 : bit i = (1 << item_type i)

  bool show_delay_  = false;
  bool show_speed_  = false;
  bool sell_stuff_  = false;
  bool sell_item_   = false;
  bool no_ask_      = false;
  int  noks_        = 0;  // 0=off 1=self 2=party 3=guild
  bool wings_           = false;
  bool aloot_mvp_       = false;
  bool aloot_mvp_rwd_   = false;
  int  tri_inv_         = 0;  // e_sort_mode 0-6
  int  tri_cart_        = 0;
  int  tri_storage_     = 0;
  int  tri_gstorage_    = 0;

  std::vector<uint32_t> aloot_ids_;        // client-tracked autolootid list (max 50)
  std::vector<uint32_t> alootid_saved_ids_; // snapshot at last preset load/save
  int                   aloot_id_input_ = 0;

  struct AlootPreset { uint8_t no; std::string name; bool autoload; };
  std::vector<AlootPreset> alootid_presets_;   // received from server (ZC 0x0C21)
  uint8_t alootid_active_preset_   = 0;        // currently loaded preset no (0=none)
  uint8_t alootid_selected_preset_ = 0;        // selected in combo (may differ from active)
  char    alootid_preset_input_[64] = {};       // name field for save
  char    alootid_rename_input_[64] = {};       // name field for rename (cmd 6)
  bool    alootid_rename_open_      = false;    // checkbox: show rename field

  // Sends a preset management command to the server (CZ 0x0C20).
  void SendPresetCmd(uint8_t cmd, uint8_t no = 0, const char* name = nullptr);
  bool                  show_alootid_overlay_ = false;  // floating Add/Remove overlay on right-click

  // Address of the item description window message handler (FUN_008c18b0,
  // 20250716 client).  We hook it to capture the nameid of right-clicked items.
  static constexpr uintptr_t kItemDescWndAddr = 0x008c18b0;
  // [edi+0x218] in the game's UI manager object (edi=0x0131F4E8): pointer to
  // the active item description window, 0 when no tooltip is open.  Written by
  // the game independently of our hook, so polling it catches silent closes
  // (e.g. comparison→non-comparison transition).
  static constexpr uintptr_t kItemDescWndGlobalPtr = 0x0131F700;

  std::unordered_map<uint32_t, std::string> item_names_;  // ID → Name from itemInfoMerged.lua
  void LoadItemNames();

  // ── Chat window background colours ───────────────────────────────────────
  // Three independent, in-game-customisable chat backgrounds. Each is backed by
  // one or more ARGB immediates in .text (default 0x66000000 = 40% alpha black),
  // patched live so future windows and the per-frame draws use the new colour.
  // The ctor-based sites also store the colour into the object, so a heap walk
  // recolours already-open windows.
  //
  //   Main     : UINewChatWnd ctor (obj+0xD8) + selected-tab colour (Draw).
  //   Detached : detached outer border (Draw) + UISubChatHisWnd ctor (obj+0xD4).
  //   Whisper  : 1:1 whisper window background (Draw).

  // Vtable addresses (20250716 client) used to recognise live window objects.
  static constexpr uint32_t kVtUINewChatWnd    = 0x01037F80;  // main chat window
  static constexpr uint32_t kVtUISubChatHisWnd = 0x01037EA8;  // detached chat history

  enum ChatBgGroupId { kChatBgMain = 0, kChatBgDetached, kChatBgWhisper, kChatBgCount };

  // A heap-stored colour field to recolour on existing objects.
  struct ChatBgHeapTarget { uint32_t vtable; uint32_t field_off; };

  // One user-facing chat-background setting.
  struct ChatBgGroup {
    const char* label    = "";                  // UI label
    const char* yaml_key = "";                  // persistence key
    std::vector<uint32_t*>        instrs;       // resolved writable .text ARGB immediates
    std::vector<ChatBgHeapTarget> heap;         // existing-object recolour targets
    float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // ImGui RGBA picker state
    bool  editing  = false;                     // picker drag in progress
  };

  ChatBgGroup chat_bg_[kChatBgCount];
  bool        chat_bg_found_ = false;           // at least one site resolved

  // Custom main-chat width — the stock chat resizes height only.  The apply
  // mechanism (engine hooks) lives in the ChatTweaks plugin; this owns the
  // setting + UI + persistence and drives it via chat::SetCustomWidth().
  bool chat_width_enabled_ = false;
  int  chat_width_px_      = 800;
  bool chat_timestamps_    = false;  // [HH:MM:SS] prefix on chat lines
  bool chat_item_icons_    = true;   // native item icons on <ITEML> chat links

  // Scans .text once (constructor) and resolves every group's immediates + heap
  // targets, seeding each picker from the colour currently in the binary.
  void FindChatBgSites();

  // Writes argb to all of a group's .text immediates (+ icache flush). When
  // walk_heap is set and the group has heap targets, also recolours live objects.
  void ApplyChatBg(ChatBgGroup& g, uint32_t argb, bool walk_heap);

  // Single heap walk: recolours every live object matching one of g's heap targets.
  void PatchChatBgObjects(const ChatBgGroup& g, uint32_t argb);

  static uint32_t ArgbFromPicker(const float c[4]);
  static void     PickerFromArgb(float c[4], uint32_t argb);

  // Persists / restores all chat-bg groups from bourgeon_settings.yaml in the
  // game directory.  Load is called after FindChatBgSites so it can immediately
  // apply the saved colours.  Save is called on picker release / preset change.
  void LoadSettings();
  void SaveSettings();

  // Shared colour presets, applicable to any group's picker.
  struct ChatBgPreset { std::string name; uint32_t argb; };
  std::vector<ChatBgPreset> chat_bg_presets_;
  char preset_name_buf_[64] = {};

  // When true, shows a small draggable window listing the colour presets, so the
  // user can quickly switch the MAIN chat colour without opening the picker.
  bool mainchat_preset_bar_ = false;

  // Log verbosity threshold (rAthena-style), persisted so it's discoverable in
  // bourgeon_settings.yaml. Applied via LogConsole::SetLevel on load; also read
  // directly by LogConsole at startup so it takes effect before login.
  // One of: trace, debug, info, warn, error, off.
  std::string log_level_ = "info";

  // Persisted collapse state of the Moonlight-Destiny window.
  // Restored once per login via SetNextWindowCollapsed; saved on every change.
  bool ui_collapsed_     = false;
  bool apply_collapse_   = false;  // set by LoadSettings, consumed on first render
};
