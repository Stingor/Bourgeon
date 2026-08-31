#include "features/systems/ui_caps.h"

#include <cstdint>
#include <cstring>

#include "bourgeon.h"
#include "features/windows/chat_window.h"
#include "features/systems/mvp_tracker.h"
#include "features/windows/npc_dialog_window.h"
#include "utils/log_console.h"

namespace {

// ZC_ACCEPT_ENTER — le `clif_authok` du serveur, émis exactement une fois par
// session de zone (login initial ET changement de personnage). Même repère que
// dans integrity_check.cc, et pour la même raison : la reconnaissance Bourgeon
// est refaite par session, notre annonce doit l'être aussi.
constexpr uint16_t kOpcodeAcceptEnter = 0x02eb;

}  // namespace

UiCaps::UiCaps() {
  auto& b = Bourgeon::Instance();
  // Le signal « le serveur t'a reconnu ». Déjà enregistré par MoonlightUi, qui en
  // lit le contenu ; l'enregistrement est un ensemble, donc le redemander ne
  // dédouble rien — et ne pas dépendre d'un autre plugin pour exister.
  b.RegisterRecvOpcode(kOpcodeSettings);
  b.RegisterObserveOpcode(kOpcodeAcceptEnter, 0);
}

// ⚠ Le masque se lit sur les plugins EUX-MÊMES, jamais sur une copie tenue à
// jour à côté : un réglage a exactement un domicile, et un miroir finit toujours
// par diverger d'une bascule.
uint32_t UiCaps::Current() {
  uint32_t caps = 0;
  auto& b = Bourgeon::Instance();
  if (NpcDialogWindow* nd = b.npc_dialog_window()) {
    if (nd->imgui_enabled_) caps |= kNpcDialog;
  }
  if (ChatWindow* cw = b.chat_window()) {
    if (cw->imgui_enabled_) caps |= kChat;
  }
  // L'interrupteur MAÎTRE du tracker, pas la visibilité de sa fenêtre : une
  // alerte doit pouvoir sonner fenêtre fermée, et un delta reçu fenêtre fermée
  // est ce qui la rendra juste à sa réouverture.
  if (MvpTracker* mt = b.mvp_tracker()) {
    if (mt->config().enabled) caps |= kMvpTracker;
  }
  return caps;
}

// Fil RÉSEAU : on ne fait que copier (cf. features/net_inbox.h). Le contenu ne
// nous intéresse pas — seule compte l'ARRIVÉE de ces paquets.
void UiCaps::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

void UiCaps::HandlePacket(uint16_t opcode, const uint8_t* /*data*/,
                          uint16_t /*len*/) {
  if (opcode == kOpcodeAcceptEnter) {
    // Nouvelle session de zone : la reconnaissance Bourgeon n'a pas encore eu
    // lieu et notre annonce précédente appartenait à la session d'avant.
    known_ = false;
    sent_  = false;
    return;
  }
  if (opcode == kOpcodeSettings) {
    // Le serveur pousse les réglages : `has_bourgeon` est posé, il acceptera
    // désormais nos CZ custom. Envoyé au prochain tick (jamais depuis ce fil).
    known_ = true;
  }
}

void UiCaps::OnTick() {
  if (!known_) return;
  const uint32_t caps = Current();
  if (sent_ && caps == last_) return;  // rien de neuf à dire
  Send(caps);
}

void UiCaps::Send(uint32_t caps) {
  uint8_t pkt[8];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpcodeToServer;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(sizeof(pkt));
  *reinterpret_cast<uint32_t*>(pkt + 4) = caps;
  if (!Bourgeon::Instance().SendPacket(pkt, sizeof(pkt))) {
    // Socket pas prête : `sent_` reste faux, le prochain tick réessaie.
    return;
  }
  last_ = caps;
  sent_ = true;
  LogInfo("[UiCaps] annoncé au serveur : 0x{:08x}", caps);
}
