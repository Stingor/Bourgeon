#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "plugins/plugin.h"
#include "ragnarok/ragnarok_client.h"

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

  bool Initialize();
  void OnTick();
  void AddLogLine(std::string log_line);
  void RenderUI();

  // Plugin event dispatch, called from the game hooks.
  void FireModeSwitch(ModeMgr::ModeType mode_type, const char* map_name);
  void FireTalkType(const char* chat_buffer);
  void FireChatMessage(const char* chat_buffer);
  void FireKeyDown(unsigned long vkey, int new_key, int accurate_key);

 private:
  Bourgeon();

  void LoadPlugins();
  void ShowBourgeonWindow() const;

  std::vector<std::unique_ptr<Plugin>> plugins_;
  uint32_t last_tick_count_;
  std::vector<std::string> log_lines_;
  RagnarokClient client_;
};
