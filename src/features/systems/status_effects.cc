#include "features/systems/status_effects.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "bourgeon.h"
#include "features/overlays/target_frame.h"  // la cible courante, second sujet
#include "features/systems/bourgeon_opcodes.h"
#include "ragnarok/game_scene.h"
#include "ragnarok/globals.h"
#include "ragnarok/social.h"  // la liste du groupe, pour le sondage

namespace {

// ── Les quatre paquets ──────────────────────────────────────────────────────
//
// DEUX familles. `clif_status_change` annonce un état qui COMMENCE ou qui
// FINIT — c'est la source utile, 599 statuts. `clif_efst_status_change_sub`
// rejoue ce qui est actif sur une entité qui ENTRE dans la vue, mais seulement
// ce que `sc_display` contient : 57 statuts sur 599, et aucun buff courant
// (cf. l'en-tête). On l'écoute quand même — ces 57-là sont gratuits — sans en
// attendre le rattrapage qu'elle semble promettre.
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

// Une entrée de NOTRE réponse : [efst:2][remain:4][total:4].
// ⚠ Doit suivre `BOURGEON_STATUS_ENTRY` du serveur, octet pour octet.
constexpr size_t kStatusEntrySize = 10;

// Le serveur envoie 9999 quand la durée est inconnue ou infinie (« this is
// indeed what official servers do », clif.cpp). Ce n'est PAS neuf secondes : le
// prendre au mot ferait disparaître les buffs permanents au bout de dix.
constexpr uint32_t kUnknownDurationMs = 9999;

// Au-delà, on cesse de croire la table : un état retenu pour une entité qui ne
// nous parle plus est un mensonge qui dure. La purge par acteur (OnTick) suffit
// dans la vie normale ; ceci ne couvre que le cas où l'acteur reste chargé alors
// que le serveur ne dit plus rien de lui.
constexpr size_t kMaxTrackedEntities = 256;

// ── Le paquet à nous : l'état COMPLET, celui que le protocole ne donne pas ──
//
// Sans lui, on ne connaît d'une entité que ce qui lui est arrivé PENDANT qu'on
// la regardait. Avec lui, on demande « où en est ce GID ? » et le serveur répond
// par la liste entière — y compris pour un membre du groupe qu'aucun sprite ne
// représente, ce que la diffusion AREA ne permettra jamais.
constexpr uint16_t kOpReqStatusList = bopcodes::kReqStatusList;  // CZ, on demande
constexpr uint16_t kOpStatusList    = bopcodes::kStatusList;     // ZC, il répond

// Un membre par tick. Sur un groupe de 24, tout interroger à chaque passage
// ferait des rafales pour une information qui bouge lentement.
constexpr unsigned kPollIntervalMs = 300;

// Passé ce délai sans réponse, un GID renseigné par le paquet redevient inconnu.
// 🔴 Ce n'est pas une optimisation : sans péremption, un membre dont la réponse
// cesse d'arriver garderait ses buffs à l'écran indéfiniment. Large, parce qu'un
// tour de rotation sur 24 membres prend déjà sept secondes.
constexpr unsigned kAnswerStaleMs = 20000;

// La CIBLE a sa propre cadence, plus rapide que la rotation du groupe : elle
// change à chaque clic, et c'est sur elle que se prend une décision immédiate.
constexpr unsigned kTargetPollMs = 700;

// Un GID que le serveur REFUSE (joueur hors de mon groupe) n'est pas redemandé
// avant ce délai. Sans ce silence, viser un adversaire ferait partir une requête
// toutes les 700 ms pour recevoir « non » à chaque fois.
constexpr unsigned kRefusedQuietMs = 15000;

// `GetEFSTImgFileName` du client : Lua d'abord, table en dur ensuite.
// __thiscall émulé en __fastcall avec un edx factice — la convention du projet.
using GetImg_t = const char* (__fastcall*)(void* session, void* edx, int id,
                                           int layer);
const auto GetImg = reinterpret_cast<GetImg_t>(0x00d87380);

uint32_t NowMs() { return ::timeGetTime(); }

// ── La duree TOTALE, celle qui manque au protocole ──────────────────────────
//
// 🔴 AUCUN paquet ne la donne de façon fiable. ZC 0x0983 met `total` égal à
// `remain` (« at this stage remain and total are the same value », clif.cpp), ce
// qui est JUSTE au démarrage d'un état — c'est alors la durée pleine — et faux
// partout ailleurs. Notre propre paquet ne fait pas mieux : le serveur lit un
// timer, il ne sait pas de quelle durée il est parti.
//
// D'où cette règle : on retient le PLUS GRAND restant jamais vu pour cet état.
//   · état capté à son DÉBUT (0x0983)   -> la durée pleine, exacte ;
//   · état découvert EN COURS (sondage) -> ce qu'il en restait à la découverte.
//
// Le second cas donne un grisage qui part de « plein » au lieu de partir du
// milieu. C'est faux dans l'absolu, mais jamais absurde : la jauge ne remonte
// pas, ne saute pas, et décroît au bon rythme. L'alternative — pas de grisage du
// tout tant qu'on n'a pas vu le début — aurait laissé la moitié des icônes sans
// aucune indication de temps.
//
// ⚠ Un état RELANCÉ repart de sa nouvelle durée. Sans ce test, un Blessing rendu
// à un niveau inférieur aurait gardé la durée de l'ancien, et son grisage serait
// parti d'à moitié écoulé.
uint32_t KeepLongest(uint32_t announced, uint32_t previous_total,
                     uint32_t previous_left) {
  if (announced == 0) return previous_total;  // permanent : pas de durée à tenir
  // Le restant a AUGMENTÉ : l'état vient d'être relancé, sa durée est celle-ci.
  if (announced > previous_left) return announced;
  return (previous_total > announced) ? previous_total : announced;
}

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
  // Celui-ci est À NOUS : au-dessus de l'opcode max du client, donc hors de sa
  // table de dispatch. C'est le reader-hook de RagConnection qui le livre.
  b.RegisterRecvOpcode(kOpStatusList);
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
      // entité qui entre dans la vue, jamais à en retirer un. Rattrapage très
      // partiel — voir l'en-tête.
      active = true;
      break;
    }
    // ── Notre paquet : la liste COMPLÈTE ──────────────────────────────────
    //
    // 🔴 Il REMPLACE ce qu'on savait de ce GID, il ne s'y ajoute pas. C'est un
    // ÉTAT : ce qui n'y figure pas n'est plus actif. Fusionner l'aurait rendu
    // inutile — les buffs disparus seraient restés.
    case kOpStatusList: {
      // `data` commence APRÈS [opcode:2][len:2] : convention de
      // RegisterRecvOpcode. [gid:4][status:1][count:1] puis les entrées.
      if (len < 6) return;
      std::memcpy(&gid, data, sizeof(gid));
      if (gid == 0) return;
      const uint8_t reply  = data[4];
      const uint8_t count  = data[5];

      // status != 0 : entité introuvable, ou refusée (un joueur qui n'est ni de
      // mon groupe ni de ma guilde). Dans les deux cas on OUBLIE — garder
      // l'ancienne liste afficherait un passé pour un présent.
      if (reply != 0) {
        by_gid_.erase(gid);
        answered_ms_.erase(gid);
        // Refus (statut 2) : ce n'est pas un incident, c'est une règle qui ne
        // changera pas tant que le groupe ne change pas. On se tait un moment
        // plutôt que de redemander en boucle.
        if (reply == 2) refused_ms_[gid] = NowMs();
        return;
      }

      std::vector<Entry> fresh;
      const uint32_t now = NowMs();
      for (uint8_t k = 0; k < count; ++k) {
        const size_t off = 6 + static_cast<size_t>(k) * kStatusEntrySize;
        if (off + kStatusEntrySize > len) break;  // paquet tronqué : on garde ce qu'on a lu
        uint16_t id = 0;
        uint32_t remain_ms = 0, total_ms = 0;
        std::memcpy(&id, data + off, sizeof(id));
        std::memcpy(&remain_ms, data + off + 2, sizeof(remain_ms));
        std::memcpy(&total_ms, data + off + 6, sizeof(total_ms));
        if (id == 0 || IconPath(id) == nullptr) continue;
        Entry e;
        e.efst = id;
        // 0 = pas d'échéance (état permanent) : surtout pas « expiré ».
        e.expires_ms = (remain_ms == 0) ? 0u : now + remain_ms;
        // La réponse ne porte pas la durée d'origine (le serveur lit un timer) :
        // on garde donc ce qu'on savait déjà de cet état, s'il était connu.
        uint32_t prev_total = 0, prev_left = 0;
        auto known = by_gid_.find(gid);
        if (known != by_gid_.end()) {
          for (const Entry& old : known->second) {
            if (old.efst != id) continue;
            prev_total = old.total_ms;
            prev_left = (old.expires_ms == 0) ? 0u : (old.expires_ms - now);
            break;
          }
        }
        e.total_ms = KeepLongest(std::max(remain_ms, total_ms), prev_total,
                                 prev_left);
        fresh.push_back(e);
      }

      answered_ms_[gid] = now;
      // Une liste VIDE est une réponse : « cette entité n'a aucun buff ». On
      // efface donc l'entrée plutôt que d'y laisser l'ancienne.
      if (fresh.empty()) by_gid_.erase(gid);
      else               by_gid_[gid] = std::move(fresh);
      return;
    }
    default:
      return;
  }

  if (gid == 0 || efst == 0) return;
  Apply(gid, efst, active, remain, total);
}

// ── Le sondage ──────────────────────────────────────────────────────────────
void StatusEffects::RequestFor(uint32_t gid) {
  if (gid == 0) return;
  uint8_t packet[8];  // [op:2][len:2][gid:4]
  *reinterpret_cast<uint16_t*>(packet + 0) = kOpReqStatusList;
  *reinterpret_cast<uint16_t*>(packet + 2) = static_cast<uint16_t>(sizeof(packet));
  *reinterpret_cast<uint32_t*>(packet + 4) = gid;
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
}

bool StatusEffects::Refused(uint32_t gid) const {
  auto it = refused_ms_.find(gid);
  if (it == refused_ms_.end()) return false;
  return (NowMs() - it->second) < kRefusedQuietMs;
}

// Deux sujets, et deux seulement : les membres du GROUPE, et l'entité que le
// joueur a en CIBLE. Le reste du monde ne nous regarde pas — sonder tout ce qui
// passe à l'écran serait du trafic pour des barres que personne n'affiche.
void StatusEffects::PollParty() {
  const unsigned now = GetTickCount();

  // ── La cible d'abord ─────────────────────────────────────────────────────
  // Sa cadence est plus rapide : elle change à chaque clic, et c'est sur elle
  // qu'on décide d'attaquer ou de fuir. Aucun filtre de type ici — le serveur
  // sait mieux que nous ce qu'il a le droit de dire, et son refus nous fait
  // taire (`Refused`). Une heuristique cliente « est-ce un monstre ? » aurait
  // dupliqué sa règle, avec le risque de diverger.
  const uint32_t target = TargetFrame::CurrentSelectionGid();
  if (target != 0 && target != rag::social::OwnAid() && !Refused(target) &&
      (last_target_poll_ms_ == 0 || (now - last_target_poll_ms_) >= kTargetPollMs)) {
    RequestFor(target);
    last_target_poll_ms_ = now;
    return;
  }

  if (last_poll_ms_ != 0 && (now - last_poll_ms_) < kPollIntervalMs) return;

  std::vector<rag::social::Entry> members;
  rag::social::ReadParty(members);
  if (members.size() <= 1) return;

  for (size_t tried = 0; tried < members.size(); ++tried) {
    if (poll_cursor_ >= members.size()) poll_cursor_ = 0;
    const rag::social::Entry& m = members[poll_cursor_++];
    // Un hors-ligne n'a pas d'entité : le serveur répondrait « introuvable ».
    // Moi non plus je ne m'interroge pas — mes propres états sont dans la barre
    // d'icônes du client, qui les tient déjà à jour.
    if (m.gid == 0 || m.offline || m.gid == rag::social::OwnAid()) continue;
    if (Refused(m.gid)) continue;
    // Hors de portée : ses états ne seraient pas affichés (cf. la purge), donc
    // la question ne se pose pas. Un paquet qu'on jetterait en arrivant.
    if (gamescene::FindActorByGid(m.gid) == nullptr) continue;
    RequestFor(m.gid);
    last_poll_ms_ = now;
    return;
  }
}

// 🔴🔴 QUI A LE DROIT D'ENTRER DANS LA TABLE
//
// ZC 0x0983 est diffusé en **AREA** : il arrive pour tout joueur à l'écran, y
// compris un ADVERSAIRE PVP. Le paquet custom, lui, est gaté côté serveur — mais
// la diffusion native ne l'est pas, et sans ce filtre elle remplirait la table
// d'états qu'on n'a pas le droit de lire. Il suffirait alors d'une surface qui
// affiche un GID quelconque pour voir les buffs d'un adversaire.
//
// La règle ne s'invente PAS ici : elle est déléguée au serveur.
//   · membre de mon groupe (ou moi)     -> accepté ;
//   · GID que le serveur vient de nous  -> accepté (il a appliqué SA gate en
//     RENSEIGNER (`answered_ms_`)          répondant : un monstre ciblé passe,
//                                          un joueur hors groupe est refusé) ;
//   · tout le reste                     -> ignoré.
//
// Écrire une heuristique cliente (« est-ce un monstre ? ») aurait dupliqué la
// règle du serveur, avec la certitude qu'elles divergent un jour.
bool StatusEffects::Allowed(uint32_t gid) const {
  if (gid == 0) return false;
  const uint32_t own = rag::OwnAccountIdSafe();
  if (own != 0 && gid == own) return true;
  // Le serveur nous a répondu pour ce GID, et récemment : il l'autorise.
  auto ans = answered_ms_.find(gid);
  if (ans != answered_ms_.end() &&
      static_cast<int32_t>(NowMs() - ans->second) <
          static_cast<int32_t>(kAnswerStaleMs))
    return true;
  return rag::social::FindPartyMember(gid, nullptr);
}

void StatusEffects::Apply(uint32_t gid, uint16_t efst, bool active,
                          uint32_t remain_ms, uint32_t total_ms) {
  // ⚠ Y COMPRIS pour retirer un état : accepter un « ce buff est fini » sur un
  // GID qu'on n'a pas le droit de suivre ne changerait rien à la table, mais
  // laisser le test au seul cas « actif » invite à l'oublier en le déplaçant.
  if (!Allowed(gid)) return;

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

  const uint32_t now = NowMs();
  Entry e;
  e.efst = efst;
  // 🔴 « 9999 » n'est pas une durée, c'est l'aveu qu'il n'y en a pas. Échéance
  // laissée à 0 : l'état reste jusqu'à ce que le serveur annonce sa fin.
  const bool timed = (remain_ms != 0 && remain_ms != kUnknownDurationMs);
  e.expires_ms = timed ? (now + remain_ms) : 0u;
  e.total_ms   = timed ? std::max(remain_ms, total_ms) : 0u;

  // Le même EFST qui revient RENOUVELLE le sien, il ne s'ajoute pas : un buff
  // relancé se voit à sa durée qui repart, pas à une deuxième icône.
  for (Entry& old : it->second) {
    if (old.efst != efst) continue;
    const uint32_t prev_left = (old.expires_ms == 0) ? 0u
                                                     : (old.expires_ms - now);
    e.total_ms = KeepLongest(e.total_ms, old.total_ms, prev_left);
    old = e;
    return;
  }
  it->second.push_back(e);
}

// ── Entretien ───────────────────────────────────────────────────────────────
void StatusEffects::OnTick() {
  if (Bourgeon::Instance().IsMapLoading()) return;
  // Demande VIVANTE : chaque surface la réarme à chaque frame où elle affiche
  // des buffs, donc la baisser ici suffit à couper le trafic dès qu'on ferme.
  const bool want = polling_wanted_;
  polling_wanted_ = false;
  if (want) PollParty();

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
    // 🔴 PAS D'ACTEUR, PAS D'ÉTATS — y compris pour un GID que le paquet vient
    // de renseigner. Le serveur sait répondre sur un membre hors de portée, mais
    // AFFICHER ses buffs à côté d'un « Hors de portée » et de PV inconnus se
    // contredit à l'écran : le lecteur voit une tuile qui prétend tout savoir de
    // quelqu'un dont elle avoue ignorer les points de vie.
    //
    // Le paquet garde tout son intérêt sans ça : il donne l'état COMPLET d'un
    // membre VISIBLE, là où la diffusion AREA n'annonce que les transitions et
    // où le rattrapage à l'entrée dans la vue ne couvre que 57 statuts sur 599.
    const bool present = (own != 0 && it->first == own) ||
                         gamescene::FindActorByGid(it->first) != nullptr;
    if (!present) {
      answered_ms_.erase(it->first);
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
  // vide — au retour en jeu, le sondage repeuplera.
  by_gid_.clear();
  answered_ms_.clear();
  refused_ms_.clear();
  last_poll_ms_        = 0;
  last_target_poll_ms_ = 0;
  poll_cursor_         = 0;
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
