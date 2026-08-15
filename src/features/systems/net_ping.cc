#include "features/systems/net_ping.h"

#include <windows.h>

#include "bourgeon.h"
#include "utils/log_console.h"

namespace {

// `ZC_NOTIFY_TIME` — la réponse du serveur. Elle porte 4 octets (le tick
// serveur), dont on ne fait rien : c'est son ARRIVÉE qui nous intéresse.
constexpr uint16_t kOpNotifyTime = 0x007f;
constexpr uint16_t kNotifyTimeLen = 4;

// Les opcodes que rAthena relie à `clif_parse_TickSend` **avec une longueur de
// 6** (opcode + tick), toutes versions confondues.
//
// ⚠ Les variantes plus longues (0x0089/0x0116/0x00F7 en 9 à 14 octets) sont
// VOLONTAIREMENT absentes : ces opcodes-là servent aussi à tout autre chose
// selon la version — 0x0089 est une demande d'action sur d'autres clients — et
// le filtre de longueur ne suffirait pas à les distinguer. Mieux vaut ne pas
// mesurer que mesurer le mauvais paquet.
constexpr uint16_t kTickCandidates[] = {
    0x0363, 0x0364, 0x035f, 0x0360, 0x0886, 0x0887, 0x0817, 0x007e,
};
constexpr int kTickRequestLen = 6;

// Au-delà, on considère que la réponse ne vient pas de l'envoi qu'on a retenu :
// le client bat toutes les quelques secondes, et un aller-retour de plus d'une
// seconde et demie serait de toute façon inexploitable comme mesure.
constexpr int kMaxRoundTripUs = 1500 * 1000;

bool IsCandidate(uint16_t opcode) {
  for (uint16_t candidate : kTickCandidates)
    if (candidate == opcode) return true;
  return false;
}

// 🔴 `QueryPerformanceCounter`, PAS `GetTickCount64`. Ce dernier avance par pas
// du tick du planificateur — ~15,6 ms — et sur un serveur local l'aller-retour
// est plus court que ça : la mesure sautait entre 0 et 15 ms, ce qui ressemble à
// une latence variable alors que c'est la granularité de l'horloge. Le compteur
// de performance, lui, est monotone et cadencé par le matériel.
uint64_t NowTicks() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<uint64_t>(now.QuadPart);
}

// La fréquence est FIXE depuis Windows XP : on la lit une fois.
uint64_t TicksPerSecond() {
  static const uint64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (f.QuadPart > 0) ? static_cast<uint64_t>(f.QuadPart) : 1u;
  }();
  return freq;
}

}  // namespace

std::atomic<uint64_t> NetPing::s_sent_at_{0};
std::atomic<uint16_t> NetPing::s_learned_op_{0};
std::atomic<uint16_t> NetPing::s_pending_op_{0};
std::atomic<int>      NetPing::s_last_us_{-1};

NetPing::NetPing() {
  // Observation PURE : le client garde son handler d'heure, on ne fait que noter
  // l'instant où le paquet arrive.
  Bourgeon::Instance().RegisterObserveOpcode(kOpNotifyTime, kNotifyTimeLen);
}

void NetPing::NoteSend(uint16_t opcode, int packet_len) {
  if (packet_len != kTickRequestLen) return;
  const uint16_t learned = s_learned_op_.load(std::memory_order_relaxed);
  // Une fois l'opcode appris, lui seul arme le chronomètre : la liste de
  // candidats n'a servi qu'à le trouver.
  if (learned != 0) {
    if (opcode != learned) return;
  } else if (!IsCandidate(opcode)) {
    return;
  }
  s_pending_op_.store(opcode, std::memory_order_relaxed);
  s_sent_at_.store(NowTicks(), std::memory_order_release);
}

void NetPing::OnRecvPacket(uint16_t opcode, const uint8_t*, uint16_t) {
  if (opcode != kOpNotifyTime) return;
  const uint64_t sent = s_sent_at_.exchange(0, std::memory_order_acquire);
  if (sent == 0) return;  // réponse sans demande retenue : on ne devine pas

  const uint64_t now = NowTicks();
  const uint64_t ticks = (now > sent) ? (now - sent) : 0;
  // Multiplier AVANT de diviser : à l'inverse, un aller-retour d'une fraction de
  // milliseconde donnerait zéro et l'on aurait remplacé un pas de 15 ms par un
  // plancher à 0. La fréquence du compteur tient largement dans 64 bits pour un
  // écart borné à une seconde et demie.
  const uint64_t rtt_us = ticks * 1000000ull / TicksPerSecond();
  if (rtt_us > static_cast<uint64_t>(kMaxRoundTripUs)) return;

  const uint16_t op = s_pending_op_.load(std::memory_order_relaxed);
  if (s_learned_op_.load(std::memory_order_relaxed) == 0 && op != 0) {
    s_learned_op_.store(op, std::memory_order_relaxed);
    // Une ligne, une seule fois : elle dit quel opcode ce client emploie, ce qui
    // est exactement ce qu'il faudrait savoir si la mesure se mettait à mentir.
    LogDiag("[NetPing] demande d'heure reconnue : opcode 0x{:04X}", op);
  }
  s_last_us_.store(static_cast<int>(rtt_us), std::memory_order_relaxed);
}

void NetPing::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  if (mode_type == ModeMgr::ModeType::kGame) return;
  // Hors du jeu, la dernière mesure ne veut plus rien dire : la garder ferait
  // afficher un ping au char-select, et le même chiffre figé au retour en jeu
  // jusqu'au premier battement. L'opcode APPRIS, lui, reste — c'est une
  // propriété du client, pas de la session.
  s_sent_at_.store(0, std::memory_order_relaxed);
  s_last_us_.store(-1, std::memory_order_relaxed);
}

int NetPing::LastUs() { return s_last_us_.load(std::memory_order_relaxed); }
