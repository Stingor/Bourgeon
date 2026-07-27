#include "ragnarok/rag_connection.h"

#include <Windows.h>

#include <cstring>

#include "bourgeon.h"
#include "plugins/npc_dialog_tweaks.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// Pointer to the game's RagConnection singleton instance
std::atomic<RagConnection*> RagConnection::g_ragconnection_ptr(nullptr);

// Opcodes registered via RegisterRecvOpcode.
std::unordered_set<uint16_t> RagConnection::s_registered_opcodes_;

// Opcodes observed via RegisterObserveOpcode (opcode -> forward byte count).
std::unordered_map<uint16_t, uint16_t> RagConnection::s_observe_opcodes_;

// Opcodes au-dessus de la dispatch table (dispatchés depuis le reader-hook).
std::unordered_set<uint16_t> RagConnection::s_reader_dispatch_opcodes_;

// Packet saved by PacketBufReaderHook: captured right after FUN_00c147d0
// fills the shared buffer, before anything downstream overwrites it.
// The dispatch handler (RecvPacketHandlerImpl) reads from here.
static uint8_t  g_saved_packet[65536];
static uint32_t g_saved_packet_len = 0;

// Opcode captured from FUN_00c9df00's stack frame at JMP time; used only to
// verify the right packet was saved.
static uint16_t g_dispatch_opcode = 0;

// Posé par PacketBufReaderHook quand l'opcode courant est un reader-dispatch
// enregistré (custom 0x0F00+, HORS-PLAGE). Consommé par BufferResetHook : la
// boucle recv appelle RecvBuffer_ResetAll_OnUnknownOpcode juste après avoir lu un
// opcode hors-plage ; ce reset VIDE le buffer et jetterait les paquets suivants
// (delitem, etc.). Quand ce flag est vrai, on skippe le reset -> le flux reste
// intact. Séquentiel sur le thread réseau (pas de course).
static bool g_suppress_buffer_reset = false;

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
    recv_dispatch_table_size_ =
        ragconnection_configuration["RecvDispatchTableSize"].as<uint16_t>(0xBC3);
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
      // LogInfo("RagConnection: recv dispatch table at {:x}, opcode base 0x{:x}, reader hook at {:x}",
              // table_addr.as<uint32_t>(), recv_opcode_base_, reader_addr.as<uint32_t>());
    } else {
      // LogInfo("RagConnection: recv dispatch table at {:x}, opcode base 0x{:x}",
              // table_addr.as<uint32_t>(), recv_opcode_base_);
    }

    // Hook du reset de buffer déclenché par les opcodes hors-plage : sans lui, nos
    // opcodes custom 0x0F00+ vident le buffer recv et jettent le paquet suivant
    // (désync). Le hook skippe le reset pour nos opcodes enregistrés uniquement.
    const auto bufreset_addr = ragconnection_configuration["RecvBufferReset"];
    if (bufreset_addr.IsDefined()) {
      RagConnection::BufferResetRef = HookManager::Instance().SetHook(
          HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(bufreset_addr.as<uint32_t>()),
          reinterpret_cast<uint8_t*>(void_cast(&RagConnection::BufferResetHook)));
      // LogInfo("RagConnection: recv buffer-reset hook at {:x}", bufreset_addr.as<uint32_t>());
    }
  }
}

bool RagConnection::SendPacket(int packet_len, char* packet) {
  RagConnection* connection = g_ragconnection_ptr.load();
  // Le pointeur n'est capturé qu'au PREMIER envoi natif : tant qu'aucun paquet du
  // client n'est passé par le hook (juste après l'entrée en jeu, par exemple), nos
  // propres envois partiraient sur un `this` nul et seraient perdus en silence.
  if (connection == nullptr) return false;
  // ⚠ CRagConnection::SendPacket (0x00c14920) renvoie TOUJOURS 1, même quand il ne met
  // rien en file : son « ok » ne prouve rien. Trois champs décident du sort réel du
  // paquet, à relire si un envoi disparaît sans trace — +0x6C connexion close (sortie
  // immédiate), +0x04 socket (-1 = paquet jeté), +0x18 bascule vers le buffer d'attente.
  return SendPacketRef(connection, packet_len, packet);
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
  // Au-delà de la dispatch table : patcher la table écrirait HORS BORNES
  // (corruption). Ces opcodes (zone custom sûre > 0x0C35) sont dispatchés depuis
  // PacketBufReaderHook, qui voit tous les paquets ; le parser de longueur du
  // client les traite en variable (flag=-1).
  if (recv_dispatch_table_size_ != 0 &&
      idx >= static_cast<int>(recv_dispatch_table_size_)) {
    s_reader_dispatch_opcodes_.insert(opcode);
    s_registered_opcodes_.insert(opcode);
    // LogInfo("RagConnection: recv opcode 0x{:04x} -> reader-hook (au-dessus dispatch table, idx {})",
            // opcode, idx);
    return;
  }
  void** slot = &recv_dispatch_table_[idx];
  DWORD old;
  VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
  *slot = reinterpret_cast<void*>(&RagConnection::RecvPacketHandler);
  VirtualProtect(slot, sizeof(void*), old, &old);
  s_registered_opcodes_.insert(opcode);
  // LogInfo("RagConnection: recv opcode 0x{:04x} → dispatch table slot [{}]", opcode, idx);
}

void RagConnection::RegisterObserveOpcode(uint16_t opcode, uint16_t forward_len) {
  s_observe_opcodes_[opcode] = forward_len;
  // LogInfo("RagConnection: observe opcode 0x{:04x} (forward {} bytes)", opcode, forward_len);
}

// Called by the game's packet-read loop (FUN_00c9df00) right after
// FUN_00c147d0 copies the incoming packet into the shared buffer.  At this
// point the buffer has not yet been processed by FUN_00b1e920, so the data is
// still the original packet bytes.  We save a copy for registered opcodes so
// the dispatch handler can read it without racing against later writes.
uint16_t RagConnection::PacketBufReaderHook(uint8_t* packet_buf) {
  const uint16_t opcode = *reinterpret_cast<const uint16_t*>(packet_buf);
  // Map-load start: ZC_NPCACK_MAPMOVE (0x0091, same-server warp/@load) or
  // ZC_NPCACK_SERVERMOVE (0x0092, cross-server) begins a map transition. Hold the
  // loading gate until the client reports ready (CZ_NOTIFY_ACTORINIT 0x007d, see
  // SendPacketHook) so Bourgeon UI/input stand down while the HUD is rebuilt.
  if (opcode == 0x0091 || opcode == 0x0092)
    Bourgeon::Instance().SetMapLoading(true);
  // Call the original (just returns opcode = *(uint16_t*)param_1).
  const uint16_t result = PacketBufReaderRef(this, packet_buf);

  // Si c'est un de NOS opcodes hors-plage (reader-dispatch), la boucle recv va
  // appeler RecvBuffer_ResetAll juste après -> on arme le skip pour éviter que le
  // reset vide le buffer et jette le paquet suivant (cf. BufferResetHook). Toujours
  // rafraîchi (faux pour tout autre opcode) : un vrai opcode inconnu garde le reset.
  g_suppress_buffer_reset = s_reader_dispatch_opcodes_.count(opcode) != 0;

  if (s_registered_opcodes_.count(opcode)) {
    const uint16_t total_len = *reinterpret_cast<const uint16_t*>(packet_buf + 2);
    if (total_len >= 4 && total_len <= sizeof(g_saved_packet)) {
      std::memcpy(g_saved_packet, packet_buf, total_len);
      g_saved_packet_len = total_len;
      // Opcodes au-dessus de la dispatch table : leur handler natif n'est PAS
      // appelé (hors bornes) -> on déclenche OnRecvPacket ICI (comme
      // RecvPacketHandlerImpl le fait pour les opcodes de la table).
      if (s_reader_dispatch_opcodes_.count(opcode)) {
        const uint16_t data_len = static_cast<uint16_t>(g_saved_packet_len) - 4;
        g_saved_packet_len = 0;
        Bourgeon::Instance().FireRecvPacket(opcode, g_saved_packet + 4, data_len);
      }
    } else {
      LogError("PacketBufReaderHook: opcode=0x{:04x} bad total_len={} (ignored)", opcode, total_len);
    }
  }

  // Passive observation of standard packets: fire the plugin callback with the
  // bytes right after the opcode.  We do NOT touch the dispatch table, so the
  // game's own handler still runs — we only peek (e.g. mapname from 0x0091).
  const auto obs = s_observe_opcodes_.find(opcode);
  if (obs != s_observe_opcodes_.end()) {
    Bourgeon::Instance().FireRecvPacket(opcode, packet_buf + 2, obs->second);
  }
  return result;
}

void RagConnection::RecvPacketHandlerImpl() {
  if (g_saved_packet_len < 4) {
    // LogInfo("RecvPacketHandlerImpl: no saved packet (dispatch_opcode=0x{:04x})", g_dispatch_opcode);
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
#pragma warning(push)
#pragma warning(disable: 4733)  // intentional: restoring FUN_00c9df00's SEH chain in its own epilogue
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
#pragma warning(pop)

void RagConnection::ConnectionHook() {
  LogDebug("RagConnection: 0x{:x}", reinterpret_cast<uintptr_t>(this));
  g_ragconnection_ptr.store(this);
  ConnectionRef(this);
}

bool RagConnection::SendPacketHook(int packet_len, char* packet) {
  // Map-load end: the client sends CZ_NOTIFY_ACTORINIT (0x007d, a 2-byte packet)
  // once the new map has finished loading and it is ready — clear the loading
  // gate. Opcode is plaintext here (the native XOR runs only after us).
  if (packet_len == 2 && packet != nullptr &&
      *reinterpret_cast<uint16_t*>(packet) == 0x007d) {
    Bourgeon::Instance().SetMapLoading(false);
  }

  // [NPC dialog ImGui] Quand l'overlay NPC est actif, on JETTE les CZ de dialogue
  // émis par les fenêtres NATIVES résiduelles (cachées mais vivantes) : au clic
  // « Fermer », cmd 0x28 ré-active la fenêtre menu native qui envoie un
  // CZ_CHOOSE_MENU parasite (« Invalid menu selection ... got 1, valid [1..0] »).
  // Nos propres CZ passent par RagConnection::SendPacket -> SendPacketRef et
  // contournent CE hook, donc tout CZ de dialogue vu ici est forcément natif.
  // L'opcode est en clair ici (le XOR natif n'agit qu'APRÈS nous, sur le 1er mot).
  if (packet_len >= 2 && packet != nullptr) {
    const uint16_t op = *reinterpret_cast<uint16_t*>(packet);
    if (auto* nd = Bourgeon::Instance().npc_dialog_tweaks();
        nd && nd->ShouldSuppressNativeDialogSend(op)) {
      return true;  // envoi natif supprimé (on simule le succès)
    }
  }

  // [dual-wield CTRL = equip LEFT] A normal inventory double-click on a dual-wield
  // weapon sends CZ_REQ_WEAR_EQUIP_V5 (0x0998) with position = EQP_ARMS (0x22 =
  // both hand bits) and lets the (pre-renewal) server pick the hand — which, once
  // the right hand is occupied, defaults to the LEFT. Holding CTRL while double-
  // clicking lets the player FORCE the left hand: we narrow the position
  // EQP_ARMS -> EQP_HAND_L (0x20) so the server equips left without re-picking.
  //   - Why CTRL (not SHIFT): SHIFT+click inserts an item link into the chat when
  //     the chat input is open. The native dblclick handler already lets
  //     CTRL+dblclick reach the equip send (it only diverts to a chat link when
  //     the chat input is open, DAT_01602278 != 0), so NO client exe patch is
  //     needed — this send-hook does the whole feature.
  //   - Gated on position == EQP_ARMS so a manual drag onto a specific hand slot
  //     (single-bit position) is never touched, and on GetAsyncKeyState(CONTROL)
  //     so a plain double-click keeps the server default.
  //   - Safe rewrite: the opcode is still plaintext here (the original XORs only
  //     the first word AFTER us) and the position field (bytes 4..7) is never
  //     XORed. See memory project_weapon_dualwield_hand_bug.
  if (packet_len >= 8 && packet != nullptr &&
      *reinterpret_cast<uint16_t*>(packet) == 0x0998 &&
      *reinterpret_cast<uint32_t*>(packet + 4) == 0x22 &&  // EQP_ARMS
      (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
    *reinterpret_cast<uint32_t*>(packet + 4) = 0x20;  // EQP_HAND_L
    // LogInfo("[Equip] CTRL held -> weapon forced to LEFT hand (0x0998 pos 0x22->0x20)");
  }
  return SendPacketRef(this, packet_len, packet);
}

// Hook sur RecvBuffer_ResetAll_OnUnknownOpcode (0x00c148b0). La boucle recv l'appelle
// pour TOUT opcode hors-plage (> 0x0C35). Ce reset vide les buffers de connexion et
// jetterait les paquets suivants. Si le dernier opcode lu est un de nos opcodes custom
// enregistrés (flag posé par PacketBufReaderHook), on SKIPPE le reset -> le flux reste
// intact (fix fantôme storage + tout futur ZC custom en interleave). Sinon (vrai opcode
// inconnu / corruption), on laisse le reset natif faire sa récupération d'erreur.
void RagConnection::BufferResetHook() {
  if (g_suppress_buffer_reset) {
    g_suppress_buffer_reset = false;
    return;  // notre paquet custom : ne PAS vider le buffer
  }
  BufferResetRef(this);
}

// References
MethodRef<RagConnection, void (RagConnection::*)()>
    RagConnection::ConnectionRef;
MethodRef<RagConnection, bool (RagConnection::*)(int packet_len, char* packet)>
    RagConnection::SendPacketRef;
MethodRef<RagConnection, uint16_t (RagConnection::*)(uint8_t*)>
    RagConnection::PacketBufReaderRef;
MethodRef<RagConnection, void (RagConnection::*)()>
    RagConnection::BufferResetRef;
