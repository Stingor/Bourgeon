#pragma once

#include <cstdint>

#include "plugins/plugin.h"

// Anti-tamper: computes a SHA-256 of this DLL's on-disk file and sends it to the
// map-server when the player enters the game, so the server can verify the
// client is running an approved Bourgeon build.
//
// IMPORTANT: enforcement (kick + admin report) and any "development" bypass live
// entirely on the SERVER. A client-side bypass flag would be trivially spoofed,
// and the check itself only raises the bar — a determined attacker controlling
// the machine can still replay a valid checksum. Treat this as defense in depth.
class IntegrityCheck : public Plugin {
 public:
  IntegrityCheck();

  const char* name() const override { return "Integrity Check"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Receives ZC_BOURGEON_KICK_NOTICE (0x0BFA): server signals the client is
  // outdated before kicking. We queue an ImGui popup so the player understands
  // why they are being disconnected.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Draws the "update required" modal popup when a kick-notice was received.
  void OnRenderUI() override;

 private:
  // CZ_BOURGEON_INTEGRITY: [opcode:2][total_len:2][sha256:32]  (total_len = 36)
  void SendChecksum();

  // Reads --integrity:true|false from the command line (default true). Use
  // --integrity:false on shortcuts that connect to servers which don't yet
  // understand the integrity opcode, so the client doesn't get dropped for
  // sending an unknown packet.
  static bool ParseEnabled();

  static constexpr uint16_t kOpcodeToServer  = 0x0BFB;  // CZ: SHA-256 report
  static constexpr uint16_t kOpcodeKickNotice = 0x0BFA;  // ZC: outdated-client notice
  static constexpr int kHashLen = 32;                    // SHA-256

  // Delay must match the server-side add_timer value in clif_parse_bourgeon_integrity.
  static constexpr uint32_t kKickDelayMs = 5000;

  bool enabled_ = true;        // sending of the integrity packet is enabled
  uint8_t hash_[kHashLen] = {};
  bool have_hash_ = false;     // self-hash computed successfully in the ctor
  bool sent_ = false;          // already sent for the current game session
  bool popup_pending_ = false; // set by OnRecvPacket, consumed by OnRenderUI
  uint32_t kick_notice_tick_ = 0; // GetTickCount() when kick-notice arrived (0 = not arrived)
};
