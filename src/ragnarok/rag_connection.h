#pragma once

#include <atomic>
#include <memory>

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

  // Hooks
  void ConnectionHook();
  bool SendPacketHook(int packet_len, char *packet);

 protected:
  // Called by the dispatch table with no args; reads the assembled packet from
  // the global recv buffer and fires it to plugins.
  static void RecvPacketHandler();

  static MethodRef<RagConnection, void (RagConnection::*)()> ConnectionRef;
  static MethodRef<RagConnection,
                   bool (RagConnection::*)(int packet_len, char *packet)>
      SendPacketRef;

  static std::atomic<RagConnection *> g_ragconnection_ptr;

  // Per-client addresses read from YAML config.
  void**   recv_dispatch_table_ = nullptr;
  uint16_t recv_opcode_base_    = 0;
  uint8_t* recv_packet_buf_     = nullptr;
};
