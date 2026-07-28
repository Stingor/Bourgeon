#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "features/systems/bourgeon_opcodes.h"
#include "features/plugin.h"

// Inbound Discord -> in-game relay.
//
// The map server (groq_service.py + groq.npc) polls Discord and broadcasts
// new messages to all Gonryun players via ZC_BOURGEON_DISCORD_MSG (0x0F08).
// This plugin registers that opcode and queues messages for display when the
// player's relay checkbox is ON; silently drops them when it is OFF.
class DiscordRelay : public Plugin {
 public:
  DiscordRelay();

  const char* name() const override { return "Discord relay"; }

  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Controlled by the MoonlightUI checkbox.
  void set_chat_active(bool active) { chat_active_.store(active); }
  bool chat_active() const { return chat_active_.load(); }

 private:
  void PushMessage(std::string text);

  static constexpr uint16_t kOpcodeDiscordMsg = bopcodes::kDiscordMsg;

  std::atomic<bool> chat_active_{false};

  std::mutex messages_mutex_;
  std::vector<std::string> messages_;
};
