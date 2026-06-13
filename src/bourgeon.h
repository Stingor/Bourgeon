#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>

#include "plugins/plugin.h"
#include "ragnarok/ragnarok_client.h"

class DiscordRelay;

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

  bool Initialize();
  void OnTick();
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

 private:
  Bourgeon();

  void LoadPlugins();
  void ShowBourgeonWindow() const;

  std::vector<std::unique_ptr<Plugin>> plugins_;
  DiscordRelay* discord_relay_ = nullptr;  // non-owning, lifetime tied to plugins_
  uint32_t last_tick_count_;
  std::vector<std::string> log_lines_;
  RagnarokClient client_;
};
