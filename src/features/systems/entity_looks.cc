#include "features/systems/entity_looks.h"

#include <windows.h>

#include "bourgeon.h"
#include "features/systems/bourgeon_opcodes.h"

namespace {

constexpr uint16_t kOpReqLooks = bopcodes::kReqLooks;
constexpr uint16_t kOpLooks    = bopcodes::kLooks;

// Une entrée de la réponse : [aid:4][job:2][hair:2][hair_color:2][sex:1].
// ⚠ Doit suivre `BOURGEON_LOOK_ENTRY` du serveur, octet pour octet.
constexpr int kEntryBytes = 11;
constexpr int kEnt_Aid   = 0;
constexpr int kEnt_Job   = 4;
constexpr int kEnt_Hair  = 6;
constexpr int kEnt_Color = 8;
constexpr int kEnt_Sex   = 10;

// 🔴 L'apparence ne bouge PRESQUE JAMAIS — un changement de coiffure est un
// évènement rare, et quand il touche quelqu'un à l'écran c'est son ACTEUR qui
// le montre en premier. Un sondage lent suffit donc largement ; le mettre au
// rythme du SP ferait passer quarante amis toutes les trois secondes pour une
// information qui tient la journée.
constexpr unsigned kPollIntervalMs = 15000;

uint32_t NowMs() { return ::timeGetTime(); }

}  // namespace

EntityLooks::EntityLooks() {
  // Opcode À NOUS : au-dessus de l'opcode max du client, donc hors de sa table
  // de dispatch. C'est le reader-hook de RagConnection qui le livre.
  Bourgeon::Instance().RegisterRecvOpcode(kOpLooks);
}

// ⚠ FIL RÉSEAU. On ne fait que COPIER : le décodage attend le fil principal.
void EntityLooks::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

void EntityLooks::HandlePacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  if (opcode != kOpLooks) return;
  // `data` commence APRÈS [opcode:2][longueur:2] — régime RegisterRecvOpcode.
  if (data == nullptr || len < 2) return;

  const int announced = static_cast<int>(data[0]) |
                        (static_cast<int>(data[1]) << 8);
  int available = (len - 2) / kEntryBytes;
  if (available > announced) available = announced;

  // ⚠ Une réponse REMPLACE ce qu'on savait des GID qu'elle mentionne, mais ne
  // vide PAS la table : une demande « groupe seul » ne doit pas effacer les
  // amis répondus au tour précédent. Un joueur qui se déconnecte garde donc son
  // entrée — sans effet, personne ne dessine la tête d'un hors-ligne.
  const uint8_t* p = data + 2;
  for (int i = 0; i < available; ++i) {
    const uint8_t* e = p + i * kEntryBytes;
    const uint32_t aid = static_cast<uint32_t>(e[kEnt_Aid]) |
                         (static_cast<uint32_t>(e[kEnt_Aid + 1]) << 8) |
                         (static_cast<uint32_t>(e[kEnt_Aid + 2]) << 16) |
                         (static_cast<uint32_t>(e[kEnt_Aid + 3]) << 24);
    if (aid == 0) continue;
    Look l;
    l.job = static_cast<uint16_t>(e[kEnt_Job] |
                                  (static_cast<uint16_t>(e[kEnt_Job + 1]) << 8));
    l.hair = static_cast<uint16_t>(
        e[kEnt_Hair] | (static_cast<uint16_t>(e[kEnt_Hair + 1]) << 8));
    l.hair_color = static_cast<uint16_t>(
        e[kEnt_Color] | (static_cast<uint16_t>(e[kEnt_Color + 1]) << 8));
    l.sex = e[kEnt_Sex];
    by_gid_[aid] = l;
  }
}

void EntityLooks::OnTick() {
  // Demandes VIVANTES : elles se redemandent à chaque frame affichée, et se
  // taisent d'elles-mêmes dès que la surface se ferme.
  const uint8_t what = static_cast<uint8_t>((want_party_ ? 1 : 0) |
                                            (want_friends_ ? 2 : 0));
  want_party_   = false;
  want_friends_ = false;
  if (what == 0) return;
  if (Bourgeon::Instance().IsMapLoading()) return;

  const unsigned now = NowMs();
  if (last_poll_ms_ != 0 && (now - last_poll_ms_) < kPollIntervalMs) return;
  last_poll_ms_ = now;
  Poll(what);
}

void EntityLooks::Poll(uint8_t what) {
  uint8_t packet[5];  // [op:2][len:2][what:1]
  *reinterpret_cast<uint16_t*>(packet + 0) = kOpReqLooks;
  *reinterpret_cast<uint16_t*>(packet + 2) = static_cast<uint16_t>(sizeof(packet));
  packet[4] = what;
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
}

void EntityLooks::OnModeSwitch(ModeMgr::ModeType, const char*) {
  // 🔴 Un AID est réattribué d'un personnage à l'autre au char-select : garder
  // la table poserait la tête de l'ancien sur le nouveau. Même règle que pour
  // les états.
  by_gid_.clear();
  last_poll_ms_ = 0;
}

bool EntityLooks::Of(uint32_t gid, Look* out) const {
  auto it = by_gid_.find(gid);
  if (it == by_gid_.end()) return false;
  if (out != nullptr) *out = it->second;
  return true;
}
