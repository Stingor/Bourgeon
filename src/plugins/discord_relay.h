#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "plugins/plugin.h"

// Two-way Discord <-> game chat relay (C++ port of the former discord.py).
//
// Inbound: a background thread polls the configured Discord channels through
// the Bot API every 5 seconds and new messages are printed into the game
// chat. Outbound: chat lines typed by the player are posted to a Discord
// webhook.
//
// Configuration is read from ./plugins/config/discord.yaml (relative to the
// game directory, NEVER committed to git):
//
//   bot_token: "..."
//   guild_name: "My Server"
//   channels_to_watch: [general, gonryun]
//   webhook_url: "https://discord.com/api/webhooks/..."
//   char_name: "Fallback"   # optional, used if the session has no name yet
//
// If the file is missing or incomplete the plugin stays inert.
class DiscordRelay : public Plugin {
 public:
  DiscordRelay();

  const char* name() const override { return "Discord relay"; }

  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnTalkType(const char* chat_buffer) override;

  // Controlled by the UI checkbox — enables/disables both relay directions.
  void set_chat_active(bool active) { chat_active_.store(active); }
  bool chat_active() const { return chat_active_.load(); }

 private:
  struct Channel {
    std::string id;
    std::string name;
    std::string cursor;  // last seen message id, only used by the poll thread
  };

  // Queued by background threads, drained on the game thread in OnTick().
  struct Event {
    enum class Type { kLog, kChat };
    Type type;
    std::string text;  // UTF-8
  };

  void PushEvent(Event::Type type, std::string text);
  void InitThread();
  void PollThread();
  void PullMessages();

  // Configuration
  bool enabled_ = false;
  std::string bot_token_;
  std::string guild_name_;
  std::vector<std::string> channel_names_;
  std::string webhook_url_;
  std::string char_name_fallback_;

  // Runtime state
  uint32_t tick_count_ = 0;
  bool init_started_ = false;
  std::atomic<bool> init_done_{false};
  std::atomic<bool> in_game_{false};
  std::atomic<bool> chat_active_{false};
  std::vector<Channel> watched_channels_;  // filled before the poll thread starts

  std::mutex events_mutex_;
  std::vector<Event> events_;
};
