#include "ragnarok/rag_connection.h"

#include <Windows.h>

#include <cstring>

#include "bourgeon.h"
#include "features/windows/npc_dialog_window.h"
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

// Opcodes STANDARD dont on a pris la place (RegisterReplaceOpcode).
std::unordered_map<uint16_t, std::function<bool()>> RagConnection::s_replace_opcodes_;
std::unordered_map<uint16_t, void*> RagConnection::s_native_handlers_;

// Résolveur de longueur du client (renseigné depuis la config).
uintptr_t RagConnection::s_packet_len_lookup_ = 0;
uintptr_t RagConnection::s_packet_len_table_  = 0;

// Cible du renvoi vers le handler natif, posée par RecvPacketHandlerImpl et lue
// par le stub naked APRÈS son `popad` — qui écrase EAX, d'où le passage par une
// globale plutôt que par la valeur de retour.
//
// Pas de course : tout le chemin recv est séquentiel sur le fil réseau (même
// raisonnement que g_suppress_buffer_reset), et la valeur est RÉÉCRITE à chaque
// dispatch avant d'être lue — un reliquat ne peut donc pas être pris pour une
// décision.
static void* g_forward_native_handler = nullptr;

// Packet saved by PacketBufReaderHook: captured right after FUN_00c147d0
// fills the shared buffer, before anything downstream overwrites it.
// The dispatch handler (RecvPacketHandlerImpl) reads from here.
static uint8_t  g_saved_packet[65536];
static uint32_t g_saved_packet_len = 0;

// Régime du paquet sauvé : vrai quand sa longueur vient de la table du client
// (paquet à longueur FIXE) et non de ses octets [+2]. Lu par
// RecvPacketHandlerImpl, qui n'a que le buffer sous les yeux et ne peut donc pas
// redécouvrir seul qu'un paquet de 6 octets n'annonce pas sa taille. Séquentiel
// sur le fil réseau, comme les deux globales voisines.
static bool g_saved_packet_fixed = false;

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

    // Résolveur de longueur du client : optionnel, mais sans lui les paquets à
    // longueur FIXE ne peuvent pas être remplacés (cf. NativeFixedPacketLen).
    const auto lenlookup_addr = ragconnection_configuration["PacketLenLookup"];
    const auto lentable_addr  = ragconnection_configuration["PacketLenTable"];
    if (lenlookup_addr.IsDefined() && lentable_addr.IsDefined()) {
      s_packet_len_lookup_ = lenlookup_addr.as<uint32_t>();
      s_packet_len_table_  = lentable_addr.as<uint32_t>();
    }

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

// Calqué sur RecvBuffer_ReadPacket (0x00c147d0), qui décide de la même façon
// combien d'octets défiler du buffer : `out[0] == 1` -> longueur fixe `out[1]`
// (plancher 2), tout le reste -> variable. Les deux opcodes 703/704 sont les
// exceptions en dur du natif ; on les reproduit pour ne pas diverger de lui.
uint16_t RagConnection::NativeFixedPacketLen(uint16_t opcode) {
  if (s_packet_len_lookup_ == 0 || s_packet_len_table_ == 0) return 0;
  using PacketLenLookup_t = int(__thiscall*)(void*, int*, int);
  int out[2] = {0, 0};
  __try {
    reinterpret_cast<PacketLenLookup_t>(s_packet_len_lookup_)(
        reinterpret_cast<void*>(s_packet_len_table_), out,
        static_cast<int>(opcode));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;  // table illisible : on retombe sur le régime variable
  }
  if (out[0] != 1) return 0;  // variable : la longueur est dans les octets [+2]
  if (opcode == 703) return 12;
  if (opcode == 704) return 4;
  return out[1] < 2 ? 2 : static_cast<uint16_t>(out[1]);
}

void RagConnection::RegisterObserveOpcode(uint16_t opcode, uint16_t forward_len) {
  s_observe_opcodes_[opcode] = forward_len;
  // LogInfo("RagConnection: observe opcode 0x{:04x} (forward {} bytes)", opcode, forward_len);
}

void RagConnection::RegisterReplaceOpcode(uint16_t opcode,
                                          std::function<bool()> claim) {
  if (!recv_dispatch_table_) {
    LogError("RagConnection: RegisterReplaceOpcode(0x{:04x}) sans dispatch table",
             opcode);
    return;
  }
  if (!claim) {
    LogError("RagConnection: RegisterReplaceOpcode(0x{:04x}) sans predicat", opcode);
    return;
  }
  const int idx = static_cast<int>(opcode) - static_cast<int>(recv_opcode_base_);
  // 🔴 Hors de la table, il n'y a AUCUN handler natif à remplacer ni à qui rendre
  // la main : ce régime n'a pas de sens là, et patcher écrirait hors bornes.
  if (idx < 0 || (recv_dispatch_table_size_ != 0 &&
                  idx >= static_cast<int>(recv_dispatch_table_size_))) {
    LogError("RagConnection: opcode 0x{:04x} hors dispatch table (idx {}) — "
             "replace impossible", opcode, idx);
    return;
  }

  s_replace_opcodes_[opcode] = std::move(claim);
  // Le paquet doit être RECOPIÉ par le reader-hook pour que le handler puisse le
  // lire : c'est ce set qui commande la copie.
  s_registered_opcodes_.insert(opcode);

  void** slot = &recv_dispatch_table_[idx];
  // Idempotent : deux appels pour le même opcode ne doivent pas enregistrer NOTRE
  // stub comme « handler natif d'origine » — ce serait une boucle infinie au
  // premier renvoi.
  if (s_native_handlers_.find(opcode) != s_native_handlers_.end()) return;

  DWORD old;
  VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
  s_native_handlers_[opcode] = *slot;   // relevé AVANT l'écrasement
  *slot = reinterpret_cast<void*>(&RagConnection::RecvPacketHandler);
  VirtualProtect(slot, sizeof(void*), old, &old);
  LogInfo("RagConnection: opcode 0x{:04x} remplace (handler natif 0x{:x} garde "
          "en renvoi)", opcode,
          reinterpret_cast<uintptr_t>(s_native_handlers_[opcode]));
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
    // Longueur FIXE d'abord : sur ces paquets, `[+2]` n'est pas une taille mais
    // deux octets de données (un GID, un résultat…), et les lire comme une taille
    // donnait une copie de longueur arbitraire.
    const uint16_t fixed_len = NativeFixedPacketLen(opcode);
    const uint16_t total_len =
        fixed_len ? fixed_len : *reinterpret_cast<const uint16_t*>(packet_buf + 2);
    // Un paquet fixe peut ne faire que 2 octets (opcode seul) ; un variable porte
    // au moins son en-tête de 4.
    const uint16_t min_len = fixed_len ? 2 : 4;
    if (total_len >= min_len && total_len <= sizeof(g_saved_packet)) {
      std::memcpy(g_saved_packet, packet_buf, total_len);
      g_saved_packet_len = total_len;
      g_saved_packet_fixed = fixed_len != 0;
      // Opcodes au-dessus de la dispatch table : leur handler natif n'est PAS
      // appelé (hors bornes) -> on déclenche OnRecvPacket ICI (comme
      // RecvPacketHandlerImpl le fait pour les opcodes de la table).
      if (s_reader_dispatch_opcodes_.count(opcode)) {
        const uint16_t data_len = static_cast<uint16_t>(g_saved_packet_len) - 4;
        g_saved_packet_len   = 0;
        g_saved_packet_fixed = false;
        Bourgeon::Instance().FireRecvPacket(opcode, g_saved_packet + 4, data_len);
      }
    } else {
      // Rien de sauvé : on efface AUSSI le régime, sinon le « fixe » d'un paquet
      // précédent survivrait et RecvPacketHandlerImpl prendrait le reliquat pour
      // un paquet à lui.
      g_saved_packet_len   = 0;
      g_saved_packet_fixed = false;
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

void* RagConnection::RecvPacketHandlerImpl() {
  // Un paquet FIXE peut légitimement tenir en 2 octets : lui appliquer le plancher
  // des variables (4) le ferait passer pour « rien de sauvé », et le paquet serait
  // rendu au natif alors qu'il nous revient.
  const bool     saved_fixed = g_saved_packet_fixed;
  const uint32_t min_saved   = saved_fixed ? 2u : 4u;
  if (g_saved_packet_len < min_saved) {
    // LogInfo("RecvPacketHandlerImpl: no saved packet (dispatch_opcode=0x{:04x})", g_dispatch_opcode);
    // 🔴 Rien de sauvé alors qu'un slot NOUS a été confié : si cet opcode est un
    // « replace », rendre la main au natif est la seule issue sûre — le sauter ici
    // ferait disparaître le paquet pour tout le monde.
    const auto native = s_native_handlers_.find(g_dispatch_opcode);
    if (native != s_native_handlers_.end()) {
      LogError("RecvPacketHandlerImpl: 0x{:04x} sans paquet sauve -> renvoi natif",
               g_dispatch_opcode);
      return native->second;
    }
    return nullptr;
  }
  const uint16_t opcode = *reinterpret_cast<const uint16_t*>(g_saved_packet);
  // Ce que voit un plugin en régime « replace » : tout ce qui suit l'opcode. La
  // formule vaut pour les deux régimes (un variable compte son en-tête de
  // longueur dedans, exactement comme RegisterObserveOpcode le lui donnerait).
  const uint16_t after_opcode = static_cast<uint16_t>(g_saved_packet_len) - 2;
  g_saved_packet_len   = 0;
  g_saved_packet_fixed = false;

  // ── Opcode STANDARD remplacé : le prédicat décide, paquet par paquet ────────
  const auto replaced = s_replace_opcodes_.find(opcode);
  if (replaced != s_replace_opcodes_.end()) {
    bool claimed = false;
    try {
      claimed = replaced->second();
    } catch (...) {
      // Un prédicat qui lève ne doit PAS faire disparaître le paquet : on rend la
      // main au natif, qui est exactement le comportement « plugin absent ».
      claimed = false;
    }
    // Trace de VALIDATION : ces paquets n'arrivent qu'au lancement d'une
    // compétence (quelques-uns par session), donc aucun coût en volume — et c'est
    // la seule façon de voir, en jeu, quel régime a pris le paquet.
    LogDiag("[recv] 0x{:04x} {} (len {})", opcode,
            claimed ? "revendique -> ImGui, natif saute" : "rendu au natif",
            after_opcode);
    if (!claimed) {
      const auto native = s_native_handlers_.find(opcode);
      return (native != s_native_handlers_.end()) ? native->second : nullptr;
    }
    // Revendiqué : mêmes octets que RegisterObserveOpcode, c'est-à-dire à partir de
    // l'octet qui SUIT l'opcode (+2) et non des données d'un variable (+4). Sur un
    // paquet variable ces deux octets sont sa longueur, que nos parseurs relisent
    // eux-mêmes ; la faire sauter ici obligerait chaque plugin à deux lectures
    // différentes selon le régime — et le passage observe -> replace, qui doit
    // rester un changement d'une ligne, deviendrait une réécriture de parseur.
    Bourgeon::Instance().FireRecvPacket(opcode, g_saved_packet + 2, after_opcode);
    return nullptr;
  }

  // Opcodes CUSTOM (RegisterRecvOpcode) : toujours à longueur variable, donc les
  // données commencent après l'en-tête complet.
  if (after_opcode >= 2)
    Bourgeon::Instance().FireRecvPacket(opcode, g_saved_packet + 4,
                                        static_cast<uint16_t>(after_opcode - 2));
  return nullptr;
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
// ── Le RENVOI vers le handler natif (RegisterReplaceOpcode) ──────────────────
//
// Quand l'impl rend une adresse, on y SAUTE au lieu de faire l'épilogue. Trois
// raisons de sauter plutôt que d'appeler :
//   - le handler natif est lui-même une cible de tail-call : il fait l'épilogue de
//     FUN_00c9df00 et rend la main à SON appelant, pas à nous ;
//   - il lit son paquet dans la frame de FUN_00c9df00 par des offsets EBP-relatifs
//     — la frame doit donc être intacte, ce que `jmp` garantit ;
//   - `pushad`/`popad` encadrent le seul moment où l'on touche aux registres, si
//     bien qu'au `jmp` l'état est bit pour bit celui du dispatch natif.
#pragma warning(push)
#pragma warning(disable: 4733)  // intentional: restoring FUN_00c9df00's SEH chain in its own epilogue
__declspec(naked) void RagConnection::RecvPacketHandler() {
  __asm {
    movzx eax, word ptr [ebp - 0x44c0]
    mov word ptr [g_dispatch_opcode], ax
    pushad
    call RagConnection::RecvPacketHandlerImpl
    mov dword ptr [g_forward_native_handler], eax
    popad
    cmp dword ptr [g_forward_native_handler], 0
    jne forward_to_native
    mov ecx, [ebp - 0x0c]  ; restore SEH chain
    mov fs:[0], ecx
    pop ecx                  ; XOR'd cookie (discarded)
    pop edi                  ; restore caller's EDI
    pop esi                  ; restore caller's ESI
    mov esp, ebp
    pop ebp
    ret
  forward_to_native:
    ; Frame, SEH et registres inchangés : le handler natif ne peut pas voir la
    ; différence avec un dispatch direct depuis la table.
    jmp dword ptr [g_forward_native_handler]
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

  // [fabrication] Observation PURE des usages d'objet. CZ_USE_ITEM2 (0x0439,
  // 8 octets : <index>.W <aid>.L) est le SEUL point commun à tous les chemins
  // d'usage — double-clic dans l'inventaire, barre de raccourcis, touche — là où
  // hooker une seule fenêtre en raterait deux. Les listes de fabrication en ont
  // besoin : un Mini Furnace ou un marteau ouvre la liste par un script d'objet
  // (`produce N;`), et RIEN dans le paquet de liste ne dit lequel.
  // L'opcode est en clair ici (le XOR natif n'agit qu'APRÈS nous), et nos propres
  // envois passent par SendPacketRef en contournant ce hook : ce qu'on voit ici
  // est donc forcément un geste du JOUEUR.
  if (packet_len == 8 && packet != nullptr &&
      *reinterpret_cast<uint16_t*>(packet) == 0x0439) {
    Bourgeon::Instance().NotifyItemUse(*reinterpret_cast<uint16_t*>(packet + 2));
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
    if (auto* nd = Bourgeon::Instance().npc_dialog_window();
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
