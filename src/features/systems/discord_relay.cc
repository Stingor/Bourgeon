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
    // ⚠ UIM_PUSHINTOCHATHISTORY s'adresse à la chatbox NATIVE, et cette voie ne
    // passe PAS par `ChatAction` (mesuré en jeu). Quand la chatbox ImGui a détruit
    // la native, c'est `UIWindowMgr::SendMsg` qui réaiguille — surtout pas le hook
    // `SendMsgHook`, que cet appel-ci contourne par le trampoline.
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
  // 🔴 TRACER LE REJET, pas la réussite. Un message écarté ici ne laissait AUCUNE
  // trace nulle part : côté serveur la ligne partait, côté joueur rien n'arrivait,
  // et rien ne disait lequel des deux verrous avait joué. Une ligne affichée, elle,
  // se voit — elle n'a pas besoin d'être racontée aussi dans le journal.
  const bool active = chat_active_.load();
  if (!active) {
    LogDiag("[DiscordRelay] message JETE ({} octets) : relais muet "
            "(reglage eteint ou hors Gonryun)", len);
    return;
  }
  if (len == 0) return;
  const char* p = reinterpret_cast<const char*>(data);
  size_t str_len = 0;
  while (str_len < len && p[str_len] != '\0') ++str_len;
  if (str_len > 0)
    PushMessage(std::string(p, str_len));
}
