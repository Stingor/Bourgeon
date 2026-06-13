#include "ragnarok/rag_connection.h"

#include <Windows.h>

#include "bourgeon.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// Pointer to the game's RagConnection singleton instance
std::atomic<RagConnection*> RagConnection::g_ragconnection_ptr(nullptr);

RagConnection::RagConnection(const YAML::Node& ragconnection_configuration) {
  using namespace hooking;

  // Hooks
  const auto connection_addr = ragconnection_configuration["CConnection"];
  if (!connection_addr.IsDefined()) {
    throw std::exception(
        "Missing required field 'CConnection' for RagConnection");
  }
  RagConnection::ConnectionRef = HookManager::Instance().SetHook(
      HookType::kJmpHook,
      reinterpret_cast<uint8_t*>(connection_addr.as<uint32_t>()),
      reinterpret_cast<uint8_t*>(void_cast(&RagConnection::ConnectionHook)));

  const auto sendpacket_addr = ragconnection_configuration["SendPacket"];
  if (!sendpacket_addr.IsDefined()) {
    throw std::exception(
        "Missing required field 'SendPacket' for RagConnection");
  }
  RagConnection::SendPacketRef = HookManager::Instance().SetHook(
      HookType::kJmpHook,
      reinterpret_cast<uint8_t*>(sendpacket_addr.as<uint32_t>()),
      reinterpret_cast<uint8_t*>(void_cast(&RagConnection::SendPacketHook)));

  // Optional recv dispatch table — only present for clients where we've
  // confirmed the layout.
  const auto table_addr = ragconnection_configuration["RecvDispatchTable"];
  if (table_addr.IsDefined()) {
    recv_dispatch_table_ =
        reinterpret_cast<void**>(table_addr.as<uint32_t>());
    recv_opcode_base_ =
        ragconnection_configuration["RecvOpcodeBase"].as<uint16_t>(0x73);
    recv_packet_buf_ = reinterpret_cast<uint8_t*>(
        ragconnection_configuration["RecvPacketBuf"].as<uint32_t>());
    LogInfo("RagConnection: recv dispatch table at {:x}, opcode base 0x{:x}",
            table_addr.as<uint32_t>(), recv_opcode_base_);
  }
}

bool RagConnection::SendPacket(int packet_len, char* packet) {
  return SendPacketRef(g_ragconnection_ptr.load(), packet_len, packet);
}

void RagConnection::RegisterRecvOpcode(uint16_t opcode) {
  if (!recv_dispatch_table_) {
    LogError("RagConnection: RegisterRecvOpcode called but no dispatch table configured");
    return;
  }
  const int idx = static_cast<int>(opcode) - static_cast<int>(recv_opcode_base_);
  if (idx < 0) {
    LogError("RagConnection: opcode 0x{:04x} is below base 0x{:x}", opcode, recv_opcode_base_);
    return;
  }
  void** slot = &recv_dispatch_table_[idx];
  DWORD old;
  VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
  *slot = reinterpret_cast<void*>(&RagConnection::RecvPacketHandler);
  VirtualProtect(slot, sizeof(void*), old, &old);
  LogInfo("RagConnection: recv opcode 0x{:04x} → dispatch table slot [{}]", opcode, idx);
}

void RagConnection::RecvPacketHandler() {
  RagConnection* conn = g_ragconnection_ptr.load();
  if (!conn || !conn->recv_packet_buf_) return;

  const uint8_t* buf = conn->recv_packet_buf_;
  const uint16_t opcode    = *reinterpret_cast<const uint16_t*>(buf);
  const uint16_t total_len = *reinterpret_cast<const uint16_t*>(buf + 2);
  if (total_len < 4) return;
  const uint16_t data_len = total_len - 4;
  Bourgeon::Instance().FireRecvPacket(opcode, buf + 4, data_len);
}

void RagConnection::ConnectionHook() {
  LogDebug("RagConnection: 0x{:x}", reinterpret_cast<uintptr_t>(this));
  g_ragconnection_ptr.store(this);
  ConnectionRef(this);
}

bool RagConnection::SendPacketHook(int packet_len, char* packet) {
  return SendPacketRef(this, packet_len, packet);
}

// References
MethodRef<RagConnection, void (RagConnection::*)()>
    RagConnection::ConnectionRef;
MethodRef<RagConnection, bool (RagConnection::*)(int packet_len, char* packet)>
    RagConnection::SendPacketRef;
