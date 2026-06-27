#pragma once

#include <cstdint>

#include "plugins/plugin.h"

// Anti-tamper: computes a SHA-256 of this DLL's on-disk file and sends it to the
// map-server when the player enters the game, so the server can verify the
// client is running an approved Bourgeon build.
//
// The same packet also carries the Windows MachineGuid (registry) so the server
// can detect multi-account abuse across different accounts on the same machine.
//
// Packet CZ_BOURGEON_INTEGRITY (0x0BFB):
//   [opcode:2][total_len:2][sha256:32][guid:36]   (total_len = 72)
//
// IMPORTANT: enforcement (kick + admin report) and any "development" bypass live
// entirely on the SERVER. A client-side bypass flag would be trivially spoofed,
// and the check itself only raises the bar — a determined attacker controlling
// the machine can still replay a valid checksum. Treat this as defense in depth.
class IntegrityCheck : public Plugin {
 public:
  IntegrityCheck();

  const char* name() const override { return "Integrity Check"; }

  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Receives ZC_BOURGEON_KICK_NOTICE (0x0BFA): server signals the client is
  // outdated before kicking. We queue an ImGui popup so the player understands
  // why they are being disconnected.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Draws the "update required" modal popup when a kick-notice was received.
  void OnRenderUI() override;

 private:
  // CZ_BOURGEON_INTEGRITY: [opcode:2][total_len:2][sha256:32][guid:36]  (total_len = 72)
  // Returns false if SendPacket failed (socket not ready) — caller should retry.
  bool SendChecksum();

  static constexpr uint16_t kOpcodeToServer   = 0x0BFB;  // CZ: SHA-256 + MachineGuid
  static constexpr uint16_t kOpcodeKickNotice = 0x0BFA;  // ZC: outdated-client notice

  // ZC_ACCEPT_ENTER — the server's clif_authok, sent exactly once per zone-server
  // session (initial login AND after a character change). We observe it to re-arm
  // the handshake on every new session, because the client otherwise sends the
  // integrity packet only once per process and may not re-fire a login->game mode
  // switch on a character change. 0x02eb for PACKETVER >= 20160330 (incl.
  // 20250716); older 2014-2016 clients used 0x0a18.
  static constexpr uint16_t kOpcodeAcceptEnter = 0x02eb;
  static constexpr int kHashLen  = 32;                   // SHA-256
  static constexpr int kGuidLen  = 36;                   // MachineGuid (no null)

  // Delay must match the server-side add_timer value in clif_parse_bourgeon_integrity.
  static constexpr uint32_t kKickDelayMs = 5000;

  bool TryComputeHash();        // computes hash_, returns true on success
  static bool ReadMachineGuid(char out[37]);

  uint8_t hash_[kHashLen] = {};
  bool have_hash_         = false;  // self-hash computed successfully
  char guid_[37]          = {};     // MachineGuid (36 chars + null)
  bool have_guid_         = false;
  bool sent_              = false;  // already sent for the current game session
  bool in_game_           = false;
  bool popup_pending_     = false;
  uint32_t kick_notice_tick_ = 0;
};
