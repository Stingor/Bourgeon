#pragma once

#include <cstdint>

#include "features/net_inbox.h"  // bourgeon::PacketInbox
#include "ragnarok/mode_mgr.h"

// Base class for Bourgeon plugins. Plugins are written in C++ and compiled
// into the DLL; there is intentionally no runtime scripting or external
// plugin loading. To add a plugin, subclass Plugin, override the events you
// need and register an instance in Bourgeon::LoadPlugins().
//
// 🔴 TOUS les événements sont émis sur le fil PRINCIPAL — SAUF UN. `OnRecvPacket`
// tourne sur le fil RÉSEAU, et la ligne qui prétendait le contraire ici a coûté
// une famille entière de crashes irreproductibles : un module qui remplit ses
// listes depuis ce fil-là écrit pendant que le rendu les parcourt. La réponse est
// `HandlePacket` juste en dessous, et le raisonnement complet est dans
// features/net_inbox.h.
class Plugin {
 public:
  virtual ~Plugin() = default;

  virtual const char* name() const = 0;

  // Fired roughly every 100ms while the client runs its update loop.
  virtual void OnTick() {}

  // Fired when the client switches modes (login <-> game).
  virtual void OnModeSwitch(ModeMgr::ModeType mode_type,
                            const char* map_name) {}

  // Fired when the user submits text in the chat box.
  virtual void OnTalkType(const char* chat_buffer) {}

  // Fired when a message is pushed into the chat history.
  virtual void OnChatMessage(const char* chat_buffer) {}

  // Fired when the game processes WM_KEYDOWN/WM_SYSKEYDOWN.
  virtual void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) {}

  // Fired every frame between ImGui::NewFrame() and ImGui::Render();
  // draw plugin windows here using the ImGui API directly.
  // NOTE: only dispatched while the game world is active — the login and
  // character-select screens are intentionally skipped (see Bourgeon::RenderUI).
  virtual void OnRenderUI() {}

  // Like OnRenderUI, but dispatched ONLY on the login / character-select screens
  // (when the game world is NOT active). Opt-in: override this for cosmetics that
  // belong on those screens (e.g. the Poring parade). Still inside a live ImGui
  // frame; still skipped while a map is loading.
  virtual void OnRenderLoginUI() {}

  // Fired when a registered server packet is received. Where `data` starts
  // depends on HOW the opcode was registered — the three regimes do not agree,
  // and reading the wrong one shifts every field:
  //   · RegisterRecvOpcode (our custom opcodes) -> after the full
  //     [opcode:2][total_len:2] header;
  //   · RegisterObserveOpcode / RegisterReplaceOpcode (standard packets) ->
  //     right after the 2-byte opcode, so a variable-length packet still begins
  //     with its own length field. Those two match on purpose: switching a
  //     plugin from observing to replacing must not rewrite its parser.
  //
  // 🔴 FIL RÉSEAU. Le seul événement qui ne soit pas sur le fil principal. La
  // forme normale tient en une ligne — copier, et laisser HandlePacket décoder :
  //     void X::OnRecvPacket(uint16_t op, const uint8_t* d, uint16_t n) {
  //       net_inbox_.Push(op, d, n);        // ou PushAnnounced (longueur annoncée)
  //     }
  virtual void OnRecvPacket(uint16_t opcode, const uint8_t* data,
                            uint16_t len) {}

  // Le décodage du paquet, sur le FIL PRINCIPAL, une fois par frame dans la phase
  // d'entrée (`Bourgeon::OnProcessInput`, non bridée — contrairement à OnTick et
  // ses ~100 ms). C'est là que va le corps qu'on écrivait autrefois dans
  // OnRecvPacket, INCHANGÉ : mêmes offsets, même parsing, mêmes membres.
  //
  // `data` pointe dans un tampon qui ne vit que le temps de l'appel — comme celui
  // du client. On décode, on n'en garde rien.
  virtual void HandlePacket(uint16_t opcode, const uint8_t* data,
                            uint16_t len) {}

  // Rejoue les paquets copiés depuis le dernier passage. Appelé par Bourgeon pour
  // TOUS les modules, chaque frame : un module qui n'a rien empilé ne paie qu'un
  // verrou non contesté.
  void DrainNetInbox() {
    net_inbox_.Drain([this](uint16_t opcode, const uint8_t* data, uint16_t len) {
      HandlePacket(opcode, data, len);
    });
  }

 protected:
  // La file du module. Écrite par OnRecvPacket (fil réseau), vidée par
  // DrainNetInbox (fil principal). Un module qui ne s'en sert pas ne paie rien.
  bourgeon::PacketInbox net_inbox_;
};
