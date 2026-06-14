#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "plugins/plugin.h"

// Two-way Discord <-> game chat relay.
//
// Inbound: the map server (groq_service.py + groq.npc) polls Discord once and
// broadcasts new messages to all Gonryun players via opcode 0x0C1F
// (ZC_BOURGEON_DISCORD_MSG).  This plugin registers that opcode and queues the
// message for display when the player's relay checkbox is ON; silently drops it
// when the checkbox is OFF.
//
// Outbound: chat lines typed by the player are posted to a Discord webhook.
//
// Configuration is read from ./discord.enc (encrypted YAML, never committed):
//
//   webhook_url: "https://discord.com/api/webhooks/..."
//   char_name: "Fallback"   # optional, used if session has no name yet
//   avatar_base: "..."       # optional, base URL for per-character avatars
//
// If discord.enc is missing or has no webhook_url, outbound webhook is disabled
// but inbound relay still works (the opcode is always registered).
class DiscordRelay : public Plugin {
 public:
  DiscordRelay();

  const char* name() const override { return "Discord relay"; }

  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnTalkType(const char* chat_buffer) override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Controlled by the UI checkbox — enables/disables both relay directions.
  void set_chat_active(bool active) { chat_active_.store(active); }
  bool chat_active() const { return chat_active_.load(); }

 private:
  // Queued by OnRecvPacket/background threads, drained on the game thread in OnTick().
  struct Event {
    enum class Type { kLog, kChat };
    Type type;
    std::string text;  // UTF-8
  };

  void PushEvent(Event::Type type, std::string text);

  // ZC_BOURGEON_DISCORD_MSG: inbound relay from server (registered here).
  static constexpr uint16_t kOpcodeDiscordMsg = 0x0C1F;
  // ZC_BOURGEON_SETTINGS: carries the player's char_id (registered by MoonlightUi;
  // all plugins receive it via FireRecvPacket).
  static constexpr uint16_t kOpcodeSettings   = 0x0BFE;

  // Configuration (outbound webhook only — inbound requires no config)
  bool enabled_ = false;
  std::string webhook_url_;
  std::string char_name_fallback_;
  std::string avatar_base_;

  // char_id received from ZC_BOURGEON_SETTINGS; used to build the avatar URL.
  uint32_t char_id_ = 0;

  // Runtime state
  std::atomic<bool> in_game_{false};
  std::atomic<bool> chat_active_{false};

  std::mutex events_mutex_;
  std::vector<Event> events_;
};
