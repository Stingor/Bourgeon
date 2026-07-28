#include "features/systems/discord_relay.h"

#include <mutex>

#include "bourgeon.h"
#include "utils/log_console.h"

namespace {
constexpr unsigned int kDiscordColor = 0x7289DA;
}

DiscordRelay::DiscordRelay() {
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeDiscordMsg);
}

void DiscordRelay::PushMessage(std::string text) {
  std::lock_guard<std::mutex> lock(messages_mutex_);
  messages_.push_back(std::move(text));
}

void DiscordRelay::OnTick() {
  std::vector<std::string> pending;
  {
    std::lock_guard<std::mutex> lock(messages_mutex_);
    pending.swap(messages_);
  }
  for (const auto& msg : pending) {
    Bourgeon::Instance().client().window_mgr().SendMsg(
        UIMessage::UIM_PUSHINTOCHATHISTORY,
        reinterpret_cast<int>(msg.c_str()), kDiscordColor, 0, 0);
  }
}

void DiscordRelay::OnModeSwitch(ModeMgr::ModeType /*mode_type*/,
                                const char* /*map_name*/) {}

void DiscordRelay::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                uint16_t len) {
  if (opcode != kOpcodeDiscordMsg) return;
  if (!chat_active_.load() || len == 0) return;
  const char* p = reinterpret_cast<const char*>(data);
  size_t str_len = 0;
  while (str_len < len && p[str_len] != '\0') ++str_len;
  if (str_len > 0)
    PushMessage(std::string(p, str_len));
}
