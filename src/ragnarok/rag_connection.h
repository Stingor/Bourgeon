#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
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

  // ── RegisterReplaceOpcode : le handler natif ne tourne PAS ─────────────────
  //
  // Troisième régime, entre les deux précédents. `RegisterObserveOpcode` laisse
  // le handler natif faire son travail (et donc ouvrir sa fenêtre, qu'on doit
  // ensuite masquer puis détruire) ; `RegisterRecvOpcode` le remplace mais ne
  // vise que NOS opcodes custom, ceux dont aucun handler natif n'existe. Ici on
  // remplace un handler natif EXISTANT, de façon RÉVOCABLE.
  //
  // `claim` est interrogé À CHAQUE PAQUET, sur le fil réseau :
  //   - vrai  -> le paquet part vers Bourgeon::FireRecvPacket et le handler natif
  //              est purement et simplement sauté ;
  //   - faux  -> on saute (JMP, pas CALL : cf. RecvPacketHandler) vers le handler
  //              natif d'origine, qui se déroule exactement comme sans nous.
  //
  // 🔴 C'est ce prédicat, et lui seul, qui doit décider — pas un drapeau recopié
  // ailleurs. Le pire scénario n'est pas « notre fenêtre est cassée » mais
  // « ni la nôtre ni la native ne prend le paquet » : le joueur lance sa
  // compétence et rien n'apparaît.
  //
  // Les octets transmis commencent JUSTE APRÈS l'opcode (+2), exactement comme
  // RegisterObserveOpcode : un plugin passe d'un régime à l'autre sans toucher à
  // son parseur. Longueur fixe comme variable — cf. NativeFixedPacketLen.
  //
  // La surcharge qui reçoit (data, len) sert aux opcodes MULTIPLEXÉS, dont un
  // champ dit à quoi le paquet sert : ZC_INVENTORY_START (0x0b08) ouvre selon son
  // invType l'inventaire, le cart OU le storage. Revendiquer l'opcode entier y
  // tuerait deux fenêtres pour en remplacer une seule ; le prédicat doit donc
  // pouvoir lire le champ. Mêmes octets que ceux qui seront transmis ensuite.
  void RegisterReplaceOpcode(uint16_t opcode, std::function<bool()> claim);
  void RegisterReplaceOpcode(
      uint16_t opcode, std::function<bool(const uint8_t* data, uint16_t len)> claim);

  // Longueur TOTALE du paquet (opcode compris) telle que le CLIENT la calcule,
  // ou 0 pour un paquet à longueur variable — dont la longueur se lit alors dans
  // les deux octets qui suivent l'opcode.
  //
  // 🔴 C'est ce résolveur qui rend `RegisterReplaceOpcode` utilisable sur les
  // paquets à longueur FIXE. Sans lui, le reader-hook lisait `[+2]` comme une
  // longueur : sur un paquet fixe ces deux octets sont des DONNÉES, et la copie
  // partait sur une taille inventée. On interroge donc la table du client
  // (celle que consulte sa propre boucle recv), plutôt que de tenir une liste de
  // longueurs en dur qui dériverait au premier changement de client.
  static uint16_t NativeFixedPacketLen(uint16_t opcode);

  // Hooks
  void ConnectionHook();
  bool SendPacketHook(int packet_len, char *packet);

 protected:
  // Hook on FUN_00c144b0 — fires immediately after FUN_00c147d0 copies a
  // packet into the shared recv buffer, before FUN_00b1e920 can overwrite it.
  // Saves a copy of any registered packet so the dispatch handler can read it.
  uint16_t PacketBufReaderHook(uint8_t *packet_buf);

  // Installed in the dispatch table via RegisterRecvOpcode.  FUN_00c9df00
  // (20250716) reaches handlers via `JMP [table+idx*4]` — a tail call, not
  // a CALL — so no return address is pushed.  The function is naked and
  // performs FUN_00c9df00's epilogue itself before returning.
  static void RecvPacketHandler();
  // Appelée depuis le wrapper naked. Renvoie l'adresse du handler NATIF quand il
  // faut lui rendre la main (opcode enregistré en « replace » dont le prédicat dit
  // non), ou nullptr quand le paquet a été consommé par nous.
  static void* RecvPacketHandlerImpl();

  // Hook on RecvBuffer_ResetAll_OnUnknownOpcode (0x00c148b0). Les 2 boucles recv
  // (map + login/char) appellent cette fn pour TOUT opcode HORS-PLAGE (> 0x0C35) :
  // elle VIDE les 3 buffers de connexion (curseurs read/write -> 0), ce qui JETTE
  // les paquets encore en attente dans le buffer recv. Nos opcodes custom 0x0F00+
  // sont hors-plage -> sans ce hook, un 0x0F0x suivi d'un paquet critique dans le
  // même flush désynchronise (fantôme storage : delitem jeté 2026-07-04). Le hook
  // skippe le reset quand le dernier opcode lu est un reader-dispatch enregistré
  // (flag posé par PacketBufReaderHook) ; le reset légitime (vrai opcode inconnu)
  // reste actif. thiscall : this = objet connexion (param_1 natif).
  void BufferResetHook();

  static MethodRef<RagConnection, void (RagConnection::*)()> ConnectionRef;
  static MethodRef<RagConnection,
                   bool (RagConnection::*)(int packet_len, char *packet)>
      SendPacketRef;
  static MethodRef<RagConnection, uint16_t (RagConnection::*)(uint8_t *)>
      PacketBufReaderRef;
  static MethodRef<RagConnection, void (RagConnection::*)()> BufferResetRef;

  static std::atomic<RagConnection *> g_ragconnection_ptr;

  // Opcodes registered via RegisterRecvOpcode; checked in PacketBufReaderHook.
  static std::unordered_set<uint16_t> s_registered_opcodes_;

  // Opcodes observed via RegisterObserveOpcode (opcode -> bytes to forward after
  // the 2-byte opcode).  Checked in PacketBufReaderHook; dispatch is untouched.
  static std::unordered_map<uint16_t, uint16_t> s_observe_opcodes_;

  // Opcodes STANDARD dont on a pris la place dans la dispatch table
  // (RegisterReplaceOpcode) : prédicat de revendication, et adresse du handler
  // natif d'origine relevée AVANT d'écrire dans le slot — c'est elle qui rend le
  // régime révocable.
  static std::unordered_map<uint16_t,
                            std::function<bool(const uint8_t*, uint16_t)>>
      s_replace_opcodes_;
  static std::unordered_map<uint16_t, void*> s_native_handlers_;

  // Opcodes ABOVE the dispatch-table bound (can't patch the table without going
  // out of bounds) — dispatched directly from PacketBufReaderHook instead, which
  // sees every packet.  Guaranteed-free zone for custom ZC packets (the client's
  // length parser handles them as unknown/variable).
  static std::unordered_set<uint16_t> s_reader_dispatch_opcodes_;

  // Résolveur de longueur du client (PacketLenTable_Lookup + sa table). STATIQUES
  // comme les tables ci-dessus : les hooks recv sont des méthodes dont le `this`
  // est l'objet NATIF, pas notre RagConnection — un membre d'instance y serait
  // illisible.
  static uintptr_t s_packet_len_lookup_;
  static uintptr_t s_packet_len_table_;

  // Per-client addresses read from YAML config.
  void**   recv_dispatch_table_      = nullptr;
  uint16_t recv_opcode_base_         = 0;
  uint16_t recv_dispatch_table_size_ = 0;  // #entries (opcode > base+size-1 -> reader-hook)
};
