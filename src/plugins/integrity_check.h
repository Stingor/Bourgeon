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

 private:
  // CZ_BOURGEON_INTEGRITY: [opcode:2][total_len:2][sha256:32]  (total_len = 36)
  void SendChecksum();

  // Reads --integrity:true|false from the command line (default true). Use
  // --integrity:false on shortcuts that connect to servers which don't yet
  // understand the integrity opcode, so the client doesn't get dropped for
  // sending an unknown packet.
  static bool ParseEnabled();

  static constexpr uint16_t kOpcodeToServer = 0x0BFB;  // server packet_db must define it
  static constexpr int kHashLen = 32;                  // SHA-256

  bool enabled_ = true;     // sending of the integrity packet is enabled
  uint8_t hash_[kHashLen] = {};
  bool have_hash_ = false;  // self-hash computed successfully in the ctor
  bool sent_ = false;       // already sent for the current game session
};
