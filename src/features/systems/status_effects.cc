#include "features/systems/status_effects.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "bourgeon.h"
#include "ragnarok/game_scene.h"
#include "ragnarok/globals.h"

namespace {

// ── Les quatre paquets ──────────────────────────────────────────────────────
//
// DEUX familles, et il faut les deux. `clif_status_change` annonce un état qui
// COMMENCE ou qui FINIT ; `clif_efst_status_change_sub` rejoue tout ce qui est
// déjà actif sur une entité qui ENTRE dans la vue. Sans la seconde, un membre
// qui apparaît à l'écran resterait vierge jusqu'à son prochain buff.
//
// Dans chaque famille, deux versions selon la conf du serveur : avec durée
// (`display_status_timers`) ou sans. Moonlight est sur « avec » — les variantes
// muettes sont écoutées quand même, parce qu'une conf se change et qu'un état
// sans échéance vaut mieux qu'un état perdu.
constexpr uint16_t kOpChangeTimed = 0x0983;  // ZC_MSG_STATE_CHANGE3 (avec durée)
constexpr uint16_t kOpChangePlain = 0x0196;  // ZC_MSG_STATE_CHANGE  (sans durée)
constexpr uint16_t kOpEnterTimed  = 0x0984;  // ZC_EFST_SET_ENTER2
constexpr uint16_t kOpEnterPlain  = 0x08FF;  // ZC_EFST_SET_ENTER

// Longueurs utiles, APRÈS l'en-tête de 2 octets que retire
// RegisterObserveOpcode — comme dans cast_bar.
constexpr uint16_t kLenChangeTimed = 27;  // paquet 29
constexpr uint16_t kLenChangePlain = 7;   // paquet 9
constexpr uint16_t kLenEnterTimed  = 26;  // paquet 28
constexpr uint16_t kLenEnterPlain  = 22;  // paquet 24

// ⚠ Les deux familles n'ordonnent PAS leurs champs pareil : l'une commence par
// le type, l'autre par le GID. Les confondre donnerait un GID lu dans un index
// d'EFST — une table qui se remplit d'entités qui n'existent pas.
//
// 0x0983 : type.W(0) id.L(2) flag.B(6) total.L(7) remain.L(11) val*3
constexpr int kCh_Efst   = 0x00;
constexpr int kCh_Gid    = 0x02;
constexpr int kCh_Flag   = 0x06;
constexpr int kCh_Total  = 0x07;
constexpr int kCh_Remain = 0x0b;
// 0x0984 : id.L(0) type.W(4) duration.L(6) duration2.L(10) val*3
constexpr int kEn_Gid      = 0x00;
constexpr int kEn_Efst     = 0x04;
constexpr int kEn_Duration = 0x06;

// Le serveur envoie 9999 quand la durée est inconnue ou infinie (« this is
// indeed what official servers do », clif.cpp). Ce n'est PAS neuf secondes : le
// prendre au mot ferait disparaître les buffs permanents au bout de dix.
constexpr uint32_t kUnknownDurationMs = 9999;

// Au-delà, on cesse de croire la table : un état retenu pour une entité qui ne
// nous parle plus est un mensonge qui dure. La purge par acteur (OnTick) suffit
// dans la vie normale ; ceci ne couvre que le cas où l'acteur reste chargé alors
// que le serveur ne dit plus rien de lui.
constexpr size_t kMaxTrackedEntities = 256;

// `GetEFSTImgFileName` du client : Lua d'abord, table en dur ensuite.
// __thiscall émulé en __fastcall avec un edx factice — la convention du projet.
using GetImg_t = const char* (__fastcall*)(void* session, void* edx, int id,
                                           int layer);
const auto GetImg = reinterpret_cast<GetImg_t>(0x00d87380);

uint32_t NowMs() { return ::timeGetTime(); }

// Interroge le client pour le fichier d'icône d'un EFST, et le COPIE.
//
// Isolée dans sa propre fonction à cause du `__try` : MSVC refuse un
// gestionnaire structuré dans une fonction qui doit dérouler des objets à
// destructeur (C2712), et l'appelant en manipule.
bool QueryIconPath(uint16_t efst, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  __try {
    void* session = reinterpret_cast<void*>(rag::kSessionAddr);
    const char* path = nullptr;
    // Les couches, dans l'ordre : une icône peut n'exister qu'à partir de la
    // deuxième. C'est ce que fait déjà `StatusIconBar` pour sa propre barre.
    for (int layer = 0; layer <= 5 && path == nullptr; ++layer)
      path = GetImg(session, nullptr, static_cast<int>(efst), layer);
    if (path == nullptr) return false;
    std::strncpy(out, path, cap - 1);
    out[cap - 1] = '\0';
    return out[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = '\0';
    return false;
  }
}

}  // namespace

StatusEffects::StatusEffects() {
  // Observation PURE : le client garde ses handlers — il en a besoin, ce sont
  // eux qui posent les effets visuels. On ne fait que lire au passage ce qu'il
  // ne conserve pas.
  auto& b = Bourgeon::Instance();
  b.RegisterObserveOpcode(kOpChangeTimed, kLenChangeTimed);
  b.RegisterObserveOpcode(kOpChangePlain, kLenChangePlain);
  b.RegisterObserveOpcode(kOpEnterTimed, kLenEnterTimed);
  b.RegisterObserveOpcode(kOpEnterPlain, kLenEnterPlain);
}

// ── Réseau ──────────────────────────────────────────────────────────────────
void StatusEffects::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                 uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

void StatusEffects::HandlePacket(uint16_t opcode, const uint8_t* data,
                                 uint16_t len) {
  if (data == nullptr) return;

  uint32_t gid = 0;
  uint16_t efst = 0;
  bool active = true;
  uint32_t remain = 0;
  uint32_t total = 0;

  switch (opcode) {
    case kOpChangeTimed: {
      if (len < kLenChangeTimed) return;
      std::memcpy(&efst, data + kCh_Efst, sizeof(efst));
      std::memcpy(&gid, data + kCh_Gid, sizeof(gid));
      active = data[kCh_Flag] != 0;
      std::memcpy(&total, data + kCh_Total, sizeof(total));
      std::memcpy(&remain, data + kCh_Remain, sizeof(remain));
      break;
    }
    case kOpChangePlain: {
      if (len < kLenChangePlain) return;
      std::memcpy(&efst, data + kCh_Efst, sizeof(efst));
      std::memcpy(&gid, data + kCh_Gid, sizeof(gid));
      active = data[kCh_Flag] != 0;
      break;  // pas de durée dans cette version : l'état vaut « jusqu'à sa fin »
    }
    case kOpEnterTimed:
    case kOpEnterPlain: {
      const uint16_t need =
          (opcode == kOpEnterTimed) ? kLenEnterTimed : kLenEnterPlain;
      if (len < need) return;
      std::memcpy(&gid, data + kEn_Gid, sizeof(gid));
      std::memcpy(&efst, data + kEn_Efst, sizeof(efst));
      std::memcpy(&remain, data + kEn_Duration, sizeof(remain));
      total = remain;
      // Cette famille n'annonce QUE des états actifs : elle sert à rattraper une
      // entité qui entre dans la vue, jamais à en retirer un.
      active = true;
      break;
    }
    default:
      return;
  }

  if (gid == 0 || efst == 0) return;
  Apply(gid, efst, active, remain, total);
}

void StatusEffects::Apply(uint32_t gid, uint16_t efst, bool active,
                          uint32_t remain_ms, uint32_t total_ms) {
  auto it = by_gid_.find(gid);

  if (!active) {
    if (it == by_gid_.end()) return;
    std::vector<Entry>& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [efst](const Entry& e) { return e.efst == efst; }),
               list.end());
    if (list.empty()) by_gid_.erase(it);
    return;
  }

  // Un état qu'on n'affichera jamais n'a pas à occuper la table : le client
  // lui-même n'a pas d'image pour lui, c'est SON arbitrage et on le suit.
  if (IconPath(efst) == nullptr) return;

  if (it == by_gid_.end()) {
    if (by_gid_.size() >= kMaxTrackedEntities) return;
    it = by_gid_.emplace(gid, std::vector<Entry>()).first;
  }

  Entry e;
  e.efst = efst;
  e.total_ms = total_ms;
  // 🔴 « 9999 » n'est pas une durée, c'est l'aveu qu'il n'y en a pas. Échéance
  // laissée à 0 : l'état reste jusqu'à ce que le serveur annonce sa fin.
  e.expires_ms = (remain_ms == 0 || remain_ms == kUnknownDurationMs)
                     ? 0u
                     : NowMs() + remain_ms;

  // Le même EFST qui revient RENOUVELLE le sien, il ne s'ajoute pas : un buff
  // relancé se voit à sa durée qui repart, pas à une deuxième icône.
  for (Entry& old : it->second) {
    if (old.efst != efst) continue;
    old = e;
    return;
  }
  it->second.push_back(e);
}

// ── Entretien ───────────────────────────────────────────────────────────────
void StatusEffects::OnTick() {
  if (by_gid_.empty()) return;
  const uint32_t now = NowMs();
  const uint32_t own = rag::OwnAccountIdSafe();

  for (auto it = by_gid_.begin(); it != by_gid_.end();) {
    // 🔴 Pas d'acteur, pas d'information. Une entité sortie de la vue cesse de
    // nous envoyer ses FINS de buff : garder sa liste, c'est afficher des états
    // qui ont peut-être expiré depuis longtemps. Mieux vaut ne rien montrer que
    // montrer un passé pour un présent.
    //
    // ⚠ MOI excepté : mon acteur n'est pas dans la liste que parcourt
    // `FindActorByGid` (le natif le range en `actorMgr+0x2C`).
    const bool present = (own != 0 && it->first == own) ||
                         gamescene::FindActorByGid(it->first) != nullptr;
    if (!present) {
      it = by_gid_.erase(it);
      continue;
    }

    std::vector<Entry>& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [now](const Entry& e) {
                                return e.expires_ms != 0 &&
                                       static_cast<int32_t>(now - e.expires_ms) >= 0;
                              }),
               list.end());
    if (list.empty()) it = by_gid_.erase(it);
    else              ++it;
  }
}

void StatusEffects::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  // Le monde repart : plus un seul de ces GID n'a de sens. On ne trie pas, on
  // vide — au retour en jeu, les paquets d'entrée dans la vue rempliront.
  by_gid_.clear();
  if (mode_type != ModeMgr::ModeType::kGame) return;
}

// ── Lecture ─────────────────────────────────────────────────────────────────
bool StatusEffects::Effects(uint32_t gid, std::vector<Entry>* out) const {
  if (out != nullptr) out->clear();
  if (gid == 0) return false;
  auto it = by_gid_.find(gid);
  if (it == by_gid_.end()) return false;
  if (out != nullptr) *out = it->second;
  return true;
}

const char* StatusEffects::IconPath(uint16_t efst) {
  // Mémorisation par EFST : `GetEFSTImgFileName` appelle du LUA, et le chemin
  // qu'il rend ne change jamais pour un id donné. L'ÉCHEC est mémorisé aussi —
  // c'est un résultat, pas une raison de réinterroger le Lua à chaque frame.
  //
  // 🔴 La chaîne est COPIÉE. Le client rend soit un littéral de sa table en dur,
  // soit une chaîne que le Lua vient de produire : le second pointeur n'a aucune
  // raison de survivre à l'appel. `StatusIconBar` s'en tire parce qu'il le
  // consomme dans la foulée ; nous le gardons, donc nous le copions.
  //
  // Une entrée vide vaut « cet EFST n'a pas d'icône » : le cache mémorise la
  // question posée, pas seulement les réponses utiles.
  static std::unordered_map<uint16_t, std::string> cache;
  auto known = cache.find(efst);
  if (known != cache.end())
    return known->second.empty() ? nullptr : known->second.c_str();

  char buf[128];
  QueryIconPath(efst, buf, sizeof(buf));

  auto& slot = cache[efst];
  slot.assign(buf);
  return slot.empty() ? nullptr : slot.c_str();
}
