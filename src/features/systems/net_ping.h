#pragma once

#include <atomic>
#include <cstdint>

#include "features/plugin.h"

// ── NetPing — la latence serveur, mesurée sur le battement du CLIENT ─────────
//
// Le client demande périodiquement l'heure du serveur (`CZ_REQUEST_TIME`, un
// paquet de 6 octets : opcode + son propre tick) ; rAthena répond par
// `ZC_NOTIFY_TIME` (**0x007F**, `clif_notify_time`). L'aller-retour de cette
// paire EST le ping, et il ne coûte rien : ces paquets circulent déjà.
//
// 🔴 IL N'Y A PAS D'ÉCHO. La réponse porte le tick du SERVEUR, pas celui qu'on a
// envoyé : impossible d'apparier par le contenu. Il faut donc HORODATER L'ENVOI,
// d'où le point d'entrée `NoteSend`, appelé depuis `RagConnection::SendPacketHook`.
//
// 🔴 ET L'OPCODE DE LA DEMANDE N'EST PAS FIXE. Les clients officiels le
// permutent à chaque version — rAthena relie `clif_parse_TickSend` à une dizaine
// d'opcodes selon `PACKETVER` (0x007E, 0x0360, 0x0817, 0x0886/87, 0x035F, 0x0363,
// 0x0364…). Plutôt que d'en figer un, on part d'une liste de candidats à six
// octets, puis **on APPREND** : le premier qui se fait suivre d'un 0x007F dans un
// délai plausible devient le seul retenu. Un client dont l'opcode ne serait dans
// aucune liste n'affiche pas un chiffre faux — il n'affiche rien.
//
// ⚠ DEUX FILS. `NoteSend` vient du fil qui envoie, `OnRecvPacket` du fil RÉSEAU,
// et l'affichage lit depuis le fil principal. Tout l'état tient donc dans des
// atomiques scalaires — aucun conteneur, aucun verrou, rien à drainer
// (features/net_inbox.h ne concerne que ce qui se décode).
class NetPing : public Plugin {
 public:
  NetPing();

  const char* name() const override { return "NetPing"; }

  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Le dernier aller-retour mesuré, en millisecondes. **−1 tant qu'on n'a rien
  // mesuré** — c'est le cas au premier chargement (le client n'a pas encore
  // battu), hors du jeu, et sur un client dont on ne reconnaîtrait pas la
  // demande. L'appelant doit afficher cette absence, pas un zéro.
  static int LastMs();

  // Horodate la demande d'heure. Appelée par `RagConnection::SendPacketHook`
  // pour TOUT paquet sortant ; c'est elle qui trie.
  static void NoteSend(uint16_t opcode, int packet_len);

 private:
  // Horloge monotone en millisecondes (`GetTickCount64`), 0 = « rien en vol ».
  static std::atomic<uint64_t> s_sent_at_;
  // L'opcode retenu une fois qu'un aller-retour a réussi. 0 = on cherche encore.
  static std::atomic<uint16_t> s_learned_op_;
  static std::atomic<uint16_t> s_pending_op_;
  static std::atomic<int>      s_last_ms_;
};
