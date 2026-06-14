#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "utils/hooking/proxy.h"
#include "yaml-cpp/yaml.h"

class RagConnection {
 public:
  using Pointer = std::unique_ptr<RagConnection>;

  RagConnection(const YAML::Node &ragconnection_configuration);

  virtual ~RagConnection() = default;

  bool SendPacket(int packet_len, char *packet);

  // Patches the client's recv dispatch table so `opcode` is forwarded to
  // plugins via Bourgeon::FireRecvPacket instead of being dropped as unknown.
  // Custom packets must use variable-length format: [opcode:2][total_len:2][data...].
  void RegisterRecvOpcode(uint16_t opcode);

  // Passively observes a *standard* client packet without touching its dispatch
  // handler.  Unlike RegisterRecvOpcode (which hijacks the dispatch slot for our
  // own custom opcodes), this fires Bourgeon::FireRecvPacket whenever the client
  // receives `opcode`, while the game's real handler still runs as normal.
  // `forward_len` bytes starting right after the 2-byte opcode are forwarded to
  // plugins as `data` — use the packet's known fixed field layout (e.g. for
  // 0x0091 ZC_NPCACK_MAPMOVE, forward_len = 16 to cover mapname[16]).
  void RegisterObserveOpcode(uint16_t opcode, uint16_t forward_len);

  // Hooks
  void ConnectionHook();
  bool SendPacketHook(int packet_len, char *packet);

 protected:
  // Hook on FUN_00c144b0 — fires immediately after FUN_00c147d0 copies a
  // packet into the shared recv buffer, before FUN_00b1e920 can overwrite it.
  // Saves a copy of any registered packet so the dispatch handler can read it.
  uint16_t PacketBufReaderHook(uint8_t *param_1);

  // Installed in the dispatch table via RegisterRecvOpcode.  FUN_00c9df00
  // (20250716) reaches handlers via `JMP [table+idx*4]` — a tail call, not
  // a CALL — so no return address is pushed.  The function is naked and
  // performs FUN_00c9df00's epilogue itself before returning.
  static void RecvPacketHandler();
  static void RecvPacketHandlerImpl();  // called from the naked wrapper

  static MethodRef<RagConnection, void (RagConnection::*)()> ConnectionRef;
  static MethodRef<RagConnection,
                   bool (RagConnection::*)(int packet_len, char *packet)>
      SendPacketRef;
  static MethodRef<RagConnection, uint16_t (RagConnection::*)(uint8_t *)>
      PacketBufReaderRef;

  static std::atomic<RagConnection *> g_ragconnection_ptr;

  // Opcodes registered via RegisterRecvOpcode; checked in PacketBufReaderHook.
  static std::unordered_set<uint16_t> s_registered_opcodes_;

  // Opcodes observed via RegisterObserveOpcode (opcode -> bytes to forward after
  // the 2-byte opcode).  Checked in PacketBufReaderHook; dispatch is untouched.
  static std::unordered_map<uint16_t, uint16_t> s_observe_opcodes_;

  // Per-client addresses read from YAML config.
  void**   recv_dispatch_table_ = nullptr;
  uint16_t recv_opcode_base_    = 0;
};
