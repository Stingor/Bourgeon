#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>

#include "plugins/plugin.h"
#include "ragnarok/ragnarok_client.h"

class DiscordRelay;
class DpsMeter;
class BasicInfoTweaks;
class MenuIconTweaks;
class StatusIconTweaks;
class QuestTrackerTweaks;
class SettingsTweaks;
class MoonlightUi;
class SkillBarTweaks;
class ChatTweaks;
class StorageTweaks;
class InventoryViewer;
class CartViewer;
class BankTweaks;
class CashShopTweaks;
class ShopTweaks;
class VendingTweaks;
class TradeTweaks;
class RodexTweaks;
class CharacterSheet;
class LoginParade;
class ItemDescTweaks;
class FpsViewTweaks;
class PlayerJumpTweaks;
class KeyboardMoveTweaks;
class DoomTweaks;
class RoggleTweaks;
class RojeweledTweaks;
class NpcDialogTweaks;
class BugReportTweaks;
class WeaponDualSprites;
class EntityNamesTweaks;

class Bourgeon {
 public:
  // Singleton stuff
  static Bourgeon& Instance() {
    static Bourgeon instance;
    return instance;
  }
  Bourgeon(Bourgeon const&) = delete;
  void operator=(Bourgeon const&) = delete;

  RagnarokClient& client();
  DiscordRelay* discord_relay();
  DpsMeter* dps_meter();
  BasicInfoTweaks* basic_info();
  MenuIconTweaks* menu_icons();
  StatusIconTweaks* status_icons();
  QuestTrackerTweaks* quest_tracker();
  SettingsTweaks* settings_tweaks();
  MoonlightUi* moonlight_ui();
  SkillBarTweaks* skill_bar();
  ChatTweaks* chat_tweaks();
  StorageTweaks* storage_tweaks();
  InventoryViewer* inventory_viewer();
  CartViewer* cart_viewer();
  BankTweaks* bank_tweaks();
  CashShopTweaks* cashshop_tweaks();
  ShopTweaks* shop_tweaks();
  VendingTweaks* vending_tweaks();
  TradeTweaks* trade_tweaks();
  RodexTweaks* rodex_tweaks();
  NpcDialogTweaks* npc_dialog_tweaks();
  BugReportTweaks* bug_report();
  CharacterSheet* character_sheet();
  LoginParade* login_parade();
  ItemDescTweaks* item_desc();
  FpsViewTweaks* fps_view();
  PlayerJumpTweaks* player_jump();
  KeyboardMoveTweaks* keyboard_move();
  DoomTweaks* doom();
  RoggleTweaks* roggle();
  RojeweledTweaks* rojeweled();
  WeaponDualSprites* weapon_dual_sprites();
  EntityNamesTweaks* entity_names();

  bool Initialize();
  void OnTick();
  void OnProcessInput();  // per-frame input-phase dispatch (NOT throttled)
  void AddLogLine(std::string log_line);
  void RenderUI();

  // Plugin event dispatch, called from the game hooks.
  void FireModeSwitch(ModeMgr::ModeType mode_type, const char* map_name);
  void FireTalkType(const char* chat_buffer);
  void FireChatMessage(const char* chat_buffer);
  void FireKeyDown(unsigned long vkey, int new_key, int accurate_key);
  void FireRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len);

  // Packet helpers for plugins.
  // SendPacket: raw send — caller builds the full packet including any header.
  bool SendPacket(const uint8_t* buf, size_t len);
  // RegisterRecvOpcode: installs a dispatch-table hook so the given server
  // opcode is forwarded to OnRecvPacket instead of being dropped as unknown.
  void RegisterRecvOpcode(uint16_t opcode);

  // RegisterObserveOpcode: forwards a *standard* client packet to OnRecvPacket
  // without replacing its dispatch handler.  `forward_len` bytes after the
  // 2-byte opcode are passed as `data`.  Use for reading fields off packets the
  // client already handles (e.g. mapname from 0x0091 ZC_NPCACK_MAPMOVE).
  void RegisterObserveOpcode(uint16_t opcode, uint16_t forward_len);

  // Map-loading gate. True from the ZC_NPCACK_MAPMOVE (0x0091) that begins a
  // warp/@load until the CZ_NOTIFY_ACTORINIT (0x007d) the client sends once the
  // new map is ready. During this window the native HUD is being torn down and
  // rebuilt (CGameMode::EnterWorld), so acting on it is unsafe — that race is
  // what freed a UIShortCutWnd while it was still in the native window-snap
  // manager and produced a use-after-free. While loading we stand down: hide the
  // plugin UI (which also stops SkillBarTweaks from MakeWindow'ing the shortcut
  // bar every frame) and swallow keyboard input.
  bool IsMapLoading() const;
  void SetMapLoading(bool loading);

  // Game-world gate. RenderUI() draws plugin ImGui windows only while the game
  // world is the actively-updating mode (CGameMode). At the login and
  // character-select screens CGameMode::OnUpdate does not run, so no plugin
  // window can linger there. NotifyGameUpdate() is the per-frame heartbeat fired
  // from GameMode::OnUpdateHook; IsGameActive() reports whether that heartbeat is
  // fresh. FireModeSwitch(non-kGame) also clears it for an instant hide — the
  // heartbeat staleness is the fallback for the char-change case, where the
  // client does not reliably re-fire a game->login mode switch (cf. the same
  // note in integrity_check.cc).
  void NotifyGameUpdate();
  bool IsGameActive() const;

 private:
  Bourgeon();

  void LoadPlugins();
  void ShowBourgeonWindow() const;

  std::vector<std::unique_ptr<Plugin>> plugins_;
  DiscordRelay* discord_relay_ = nullptr;  // non-owning, lifetime tied to plugins_
  DpsMeter*     dps_meter_     = nullptr;  // non-owning, lifetime tied to plugins_
  BasicInfoTweaks* basic_info_ = nullptr;  // non-owning, lifetime tied to plugins_
  MenuIconTweaks* menu_icons_  = nullptr;  // non-owning, lifetime tied to plugins_
  StatusIconTweaks* status_icons_ = nullptr;  // non-owning, lifetime tied to plugins_
  QuestTrackerTweaks* quest_tracker_ = nullptr;  // non-owning, lifetime tied to plugins_
  SettingsTweaks* settings_tweaks_ = nullptr; // non-owning, lifetime tied to plugins_
  MoonlightUi* moonlight_ui_ = nullptr;       // non-owning, lifetime tied to plugins_
  SkillBarTweaks* skill_bar_ = nullptr;       // non-owning, lifetime tied to plugins_
  ChatTweaks* chat_tweaks_ = nullptr;         // non-owning, lifetime tied to plugins_
  StorageTweaks* storage_tweaks_ = nullptr;   // non-owning, lifetime tied to plugins_
  InventoryViewer* inventory_viewer_ = nullptr;  // non-owning, lifetime tied to plugins_
  CartViewer* cart_viewer_ = nullptr;            // non-owning, lifetime tied to plugins_
  BankTweaks* bank_tweaks_ = nullptr;          // non-owning, lifetime tied to plugins_
  CashShopTweaks* cashshop_tweaks_ = nullptr;  // non-owning, lifetime tied to plugins_
  ShopTweaks* shop_tweaks_ = nullptr;          // non-owning, lifetime tied to plugins_
  VendingTweaks* vending_tweaks_ = nullptr;    // non-owning, lifetime tied to plugins_
  TradeTweaks* trade_tweaks_ = nullptr;        // non-owning, lifetime tied to plugins_
  RodexTweaks* rodex_tweaks_ = nullptr;        // non-owning, lifetime tied to plugins_
  NpcDialogTweaks* npc_dialog_tweaks_ = nullptr;  // non-owning, lifetime tied to plugins_
  BugReportTweaks* bug_report_ = nullptr;  // non-owning, lifetime tied to plugins_
  CharacterSheet* character_sheet_ = nullptr;  // non-owning, lifetime tied to plugins_
  LoginParade* login_parade_ = nullptr;        // non-owning, lifetime tied to plugins_
  FpsViewTweaks* fps_view_ = nullptr;         // non-owning, lifetime tied to plugins_
  PlayerJumpTweaks* player_jump_ = nullptr;   // non-owning, lifetime tied to plugins_
  KeyboardMoveTweaks* keyboard_move_ = nullptr;  // non-owning, lifetime tied to plugins_
  DoomTweaks* doom_ = nullptr;                // non-owning, lifetime tied to plugins_
  RoggleTweaks* roggle_ = nullptr;            // non-owning, lifetime tied to plugins_
  RojeweledTweaks* rojeweled_ = nullptr;      // non-owning, lifetime tied to plugins_
  ItemDescTweaks* item_desc_ = nullptr;       // non-owning, lifetime tied to plugins_
  WeaponDualSprites* weapon_dual_sprites_ = nullptr;  // non-owning, lifetime tied to plugins_
  EntityNamesTweaks* entity_names_ = nullptr;  // non-owning, lifetime tied to plugins_
  uint32_t last_tick_count_;
  std::atomic<bool> map_loading_{false};
  std::atomic<uint32_t> map_loading_since_ms_{0};  // GetTickCount at load start
  std::atomic<uint32_t> last_game_update_ms_{0};   // GetTickCount of last CGameMode update (0 = never)
  std::vector<std::string> log_lines_;
  RagnarokClient client_;
};
