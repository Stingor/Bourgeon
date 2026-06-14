#include "ragnarok/rag_connection.h"

#include <Windows.h>

#include <cstring>

#include "bourgeon.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// Pointer to the game's RagConnection singleton instance
std::atomic<RagConnection*> RagConnection::g_ragconnection_ptr(nullptr);

// Opcodes registered via RegisterRecvOpcode.
std::unordered_set<uint16_t> RagConnection::s_registered_opcodes_;

// Opcodes observed via RegisterObserveOpcode (opcode -> forward byte count).
std::unordered_map<uint16_t, uint16_t> RagConnection::s_observe_opcodes_;

// Packet saved by PacketBufReaderHook: captured right after FUN_00c147d0
// fills the shared buffer, before anything downstream overwrites it.
// The dispatch handler (RecvPacketHandlerImpl) reads from here.
static uint8_t  g_saved_packet[65536];
static uint32_t g_saved_packet_len = 0;

// Opcode captured from FUN_00c9df00's stack frame at JMP time; used only to
// verify the right packet was saved.
static uint16_t g_dispatch_opcode = 0;

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

    const auto reader_addr = ragconnection_configuration["RecvOpcodeReader"];
    if (reader_addr.IsDefined()) {
      RagConnection::PacketBufReaderRef = HookManager::Instance().SetHook(
          HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(reader_addr.as<uint32_t>()),
          reinterpret_cast<uint8_t*>(void_cast(&RagConnection::PacketBufReaderHook)));
      LogInfo("RagConnection: recv dispatch table at {:x}, opcode base 0x{:x}, reader hook at {:x}",
              table_addr.as<uint32_t>(), recv_opcode_base_, reader_addr.as<uint32_t>());
    } else {
      LogInfo("RagConnection: recv dispatch table at {:x}, opcode base 0x{:x}",
              table_addr.as<uint32_t>(), recv_opcode_base_);
    }
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
  s_registered_opcodes_.insert(opcode);
  LogInfo("RagConnection: recv opcode 0x{:04x} → dispatch table slot [{}]", opcode, idx);
}

void RagConnection::RegisterObserveOpcode(uint16_t opcode, uint16_t forward_len) {
  s_observe_opcodes_[opcode] = forward_len;
  LogInfo("RagConnection: observe opcode 0x{:04x} (forward {} bytes)", opcode, forward_len);
}

// Called by the game's packet-read loop (FUN_00c9df00) right after
// FUN_00c147d0 copies the incoming packet into the shared buffer.  At this
// point the buffer has not yet been processed by FUN_00b1e920, so the data is
// still the original packet bytes.  We save a copy for registered opcodes so
// the dispatch handler can read it without racing against later writes.
uint16_t RagConnection::PacketBufReaderHook(uint8_t* param_1) {
  const uint16_t opcode = *reinterpret_cast<const uint16_t*>(param_1);
  // Call the original (just returns opcode = *(uint16_t*)param_1).
  const uint16_t result = PacketBufReaderRef(this, param_1);

  if (s_registered_opcodes_.count(opcode)) {
    const uint16_t total_len = *reinterpret_cast<const uint16_t*>(param_1 + 2);
    if (total_len >= 4 && total_len <= sizeof(g_saved_packet)) {
      std::memcpy(g_saved_packet, param_1, total_len);
      g_saved_packet_len = total_len;
    } else {
      LogError("PacketBufReaderHook: opcode=0x{:04x} bad total_len={} (ignored)", opcode, total_len);
    }
  }

  // Passive observation of standard packets: fire the plugin callback with the
  // bytes right after the opcode.  We do NOT touch the dispatch table, so the
  // game's own handler still runs — we only peek (e.g. mapname from 0x0091).
  const auto obs = s_observe_opcodes_.find(opcode);
  if (obs != s_observe_opcodes_.end()) {
    Bourgeon::Instance().FireRecvPacket(opcode, param_1 + 2, obs->second);
  }
  return result;
}

void RagConnection::RecvPacketHandlerImpl() {
  if (g_saved_packet_len < 4) {
    LogInfo("RecvPacketHandlerImpl: no saved packet (dispatch_opcode=0x{:04x})", g_dispatch_opcode);
    return;
  }
  const uint16_t opcode   = *reinterpret_cast<const uint16_t*>(g_saved_packet);
  const uint16_t data_len = static_cast<uint16_t>(g_saved_packet_len) - 4;
  g_saved_packet_len = 0;
  Bourgeon::Instance().FireRecvPacket(opcode, g_saved_packet + 4, data_len);
}

// FUN_00c9df00 (20250716) dispatches via `JMP [table+idx*4]` — a tail call
// that does NOT push a return address.  A normal C++ function would corrupt
// the stack because its RET would consume one of FUN_00c9df00's local
// variables instead of the real return address.
//
// This naked function calls our C++ impl normally (CALL pushes a return
// address for the impl, which RETs back here), then performs FUN_00c9df00's
// own epilogue so the stack and SEH chain are correctly restored:
//
//   FUN_00c9df00 prologue leaves (low→high addr) at JMP time:
//     [ESP+0]  XOR'd security cookie  (PUSH EAX after alloca)
//     [ESP+4]  saved EDI              (PUSH EDI)
//     [ESP+8]  saved ESI              (PUSH ESI)
//     ... alloca space (0x47D8 bytes) ...
//     [EBP-12] old FS:[0]
//     [EBP]    old caller's EBP
//     [EBP+4]  return address
//
// auStack_44c4[0] (the dispatch opcode) lives at [EBP-0x44C0] in
// FUN_00c9df00's frame; we snapshot it for diagnostic use.
__declspec(naked) void RagConnection::RecvPacketHandler() {
  __asm {
    movzx eax, word ptr [ebp - 0x44c0]
    mov word ptr [g_dispatch_opcode], ax
    pushad
    call RagConnection::RecvPacketHandlerImpl
    popad
    mov ecx, [ebp - 0x0c]  ; restore SEH chain
    mov fs:[0], ecx
    pop ecx                  ; XOR'd cookie (discarded)
    pop edi                  ; restore caller's EDI
    pop esi                  ; restore caller's ESI
    mov esp, ebp
    pop ebp
    ret
  }
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
MethodRef<RagConnection, uint16_t (RagConnection::*)(uint8_t*)>
    RagConnection::PacketBufReaderRef;
