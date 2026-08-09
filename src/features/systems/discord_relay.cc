#include "features/systems/discord_relay.h"

#include <cstdio>
#include <mutex>
#include <string>

#include "bourgeon.h"
#include "ui/ro_imgui.h"  // ro::IsUtf8 (verdict d'encodage, pour le diagnostic)
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
  if (str_len == 0) return;
  const std::string text(p, str_len);

  // ── La PREUVE de l'encodage, sans debugger ─────────────────────────────────
  // Les octets bruts tels qu'ils sortent du fil, avant toute interprétation, et
  // le verdict qui leur sera appliqué. C'est ce qui répond à « le serveur
  // envoie-t-il vraiment de l'UTF-8, et le client le voit-il ? » :
  //   · un emoji intact se lit F0 9F .. et le verdict dit UTF-8 ;
  //   · un « ? » (3F) à sa place = il a été perdu AVANT nous, dans la base — la
  //     connexion MySQL de rAthena est en latin1, où aucun emoji n'existe, d'où
  //     la traduction faite par le bot (_to_wire, tools/groq_service.py) ;
  //   · verdict CP1252 alors que des octets >= 0x80 défilent = la séquence est
  //     invalide, donc coupée en route.
  //
  // 🔴 UNE FOIS par session, plus les anomalies. Tracer chaque message serait du
  // bruit permanent dans le journal de TOUS les joueurs, au niveau warn de
  // surcroît — et un diagnostic qu'on ne peut pas éteindre finit par masquer ce
  // qu'il devait montrer. Le premier message non-ASCII suffit à prouver que la
  // chaîne tient ; après quoi seul l'anormal parle.
  {
    bool high = false;
    for (char c : text)
      if (static_cast<unsigned char>(c) >= 0x80) { high = true; break; }
    if (high) {
      const bool utf8 = ro::IsUtf8(text.c_str());
      static bool proof_logged = false;
      if (!proof_logged || !utf8) {
        proof_logged = true;
        std::string hex;
        const size_t shown = (str_len < 48) ? str_len : 48;
        hex.reserve(shown * 3);
        for (size_t i = 0; i < shown; ++i) {
          char cell[4];
          std::snprintf(cell, sizeof(cell), "%02X ",
                        static_cast<unsigned char>(text[i]));
          hex += cell;
        }
        LogDiag("[DiscordRelay] {} octets, verdict {} | {}{}", str_len,
                utf8 ? "UTF-8" : "CP1252", hex, (shown < str_len) ? "..." : "");
      }
    }
  }

  PushMessage(text);
}
