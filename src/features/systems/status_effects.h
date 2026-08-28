#pragma once

// ── Les buffs et debuffs des AUTRES entités ─────────────────────────────────
//
// Le client REÇOIT ces états, mais ne les garde pas. Le handler de ZC 0x0983
// (`sub_CE9A60`, atteint depuis `RecvLoop_DispatchPackets` case 2435) les
// convertit en effets visuels — une aura, un halo — et la seule liste d'icônes
// qu'il tienne est un `std::vector` GLOBAL (0x0136e6c8) : la NÔTRE, celle que
// `StatusIconBar` dessine. Il n'y a donc rien à lire sur un autre acteur : ce
// module écoute le fil et tient sa propre table.
//
// ── Ce que le serveur envoie ────────────────────────────────────────────────
//
// `clif_status_change` (clif.cpp:11012) diffuse en **AREA** tout statut qui
// porte une icône, à son début comme à sa fin. Le filtre `StatusRelevantBLTypes`
// ne retire rien pour un joueur : `BL_SCEFFECT` contient `BL_PC`, donc les deux
// branches passent. C'est LA source, et elle est large : 599 statuts en
// pré-renewal.
//
// 🔴 AREA, et rien d'autre. Un membre du groupe sur une autre carte, ou à
// l'autre bout de celle-ci, n'émet RIEN. C'est le même mur que pour le SP d'un
// tiers, et il se franchit de la même façon : un paquet à nous.
//
// ── LE TROU : rien n'est rejoué à l'entrée dans la vue ──────────────────────
//
// 🔴🔴 On ne voit un état QUE s'il commence pendant qu'on regarde. Un joueur
// déjà bénit qui entre à l'écran arrive VIERGE, et le restera jusqu'à ce qu'on
// le rebénisse.
//
// Le chemin existe pourtant : `clif_insight` -> `clif_getareachar_unit`
// (clif.cpp:9503) -> `clif_efst_status_change_sub` (clif.cpp:11075). Mais cette
// dernière ne lit pas les status changes : elle lit `sd->sc_display`, que
// `status_change_start` ne remplit que pour les statuts portant le drapeau
// `DisplayPc` / `DisplayNpc` (status.cpp:13186).
//
// MESURÉ sur `db/pre-re/status.yml` : **57 sur 599**. Et pas les bons — ni
// Blessing, ni Agi Up, ni Endure, ni Magnificat, ni Angelus, ni Impositio, ni
// Kyrie, ni même Poison, Stone ou Freeze. Ce sont les statuts « de fond »
// (monture, clan…) qui y sont, précisément ceux qu'on ne regarde pas.
//
// Conclusion : le paquet à nous n'est pas seulement nécessaire pour les membres
// HORS de la vue, il l'est aussi pour l'état COMPLET d'un joueur à l'écran. Ce
// module-ci reste utile — il montre les buffs au moment où ils tombent, ce qui
// est le cas d'usage du soigneur — mais il ne prétend pas à l'exhaustivité, et
// aucune surface ne doit lui en prêter.
//
// ── La durée ────────────────────────────────────────────────────────────────
//
// `display_status_timers: yes` est posé dans `conf/import/battle_conf.txt`, donc
// le serveur émet 0x0983 (avec durée) et non 0x0196 (sans). Les deux sont écoutés
// quand même : la conf peut changer, et sans durée on garde l'état en le marquant
// « sans échéance » plutôt que de le perdre.
//
// ── Pourquoi une entrée peut MENTIR ─────────────────────────────────────────
//
// 🔴 Ces paquets disent « celui-ci commence » ou « celui-ci finit », jamais
// « voici l'état complet ». Quand une entité SORT de la vue, on cesse de recevoir
// ses fins de buff : la table resterait figée sur des états expirés sans que rien
// ne le signale. D'où la règle de fraîcheur ci-dessous, qui est la même que pour
// les PV : pas d'acteur, pas d'information.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "features/plugin.h"

class StatusEffects : public Plugin {
 public:
  // Un état actif sur une entité.
  struct Entry {
    uint16_t efst = 0;         // index EFST (celui du serveur, cf. db/status.yml)
    uint32_t expires_ms = 0;   // horloge `timeGetTime` ; 0 = pas d'échéance connue
    uint32_t total_ms = 0;     // durée annoncée, pour une jauge d'écoulement
  };

  StatusEffects();

  const char* name() const override { return "StatusEffects"; }

  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // ── Le sondage ────────────────────────────────────────────────────────────
  //
  // Une surface qui affiche des buffs le DEMANDE, à chaque frame où elle en
  // affiche. Sans demande vivante, aucun paquet ne part : une fonction qu'on
  // n'ouvre jamais ne doit rien coûter au serveur.
  //
  // C'est le patron de `PartyFrames::RequestSpPolling`, et pour la même raison :
  // deux surfaces montrent les mêmes membres, et la première qui s'éteint ne
  // doit pas assécher la seconde.
  // ⚠ DEUX sujets, et deux seulement : les membres du GROUPE, et l'entité que
  // le joueur a en CIBLE. Rien d'autre n'est interrogé — ni les passants, ni les
  // monstres alentour.
  void RequestPolling() { polling_wanted_ = true; }

  // Demande l'état complet d'UNE entité, tout de suite. Pour les surfaces qui
  // visent une entité précise plutôt que le groupe entier.
  void RequestFor(uint32_t gid);

  // Les états connus d'une entité, du plus récent au plus ancien.
  //
  // Rend FAUX quand on ne sait rien d'elle — ce qui ne veut pas dire « aucun
  // buff » mais « hors de portée de ce que le serveur nous raconte ». Les deux
  // se ressemblent à l'écran et ne doivent pas s'y confondre : l'appelant qui
  // affiche « aucun buff » sur un rien-du-tout ment à son lecteur.
  bool Effects(uint32_t gid, std::vector<Entry>* out) const;

  // Le chemin de l'icône d'un EFST, prêt pour `ro::CachedTextureFromGameFile`.
  //
  // Passe par `GetEFSTImgFileName`, la fonction du client (0x00d87380) : elle
  // interroge d'abord le Lua, puis une table en dur. Mémorisé par EFST — c'est
  // un appel LUA, et le redemander à chaque frame pour chaque icône de chaque
  // membre coûterait cher pour un résultat qui ne bouge jamais.
  //
  // Rend nullptr quand cet EFST n'a pas d'icône : il ne doit alors rien
  // afficher, c'est le choix du client lui-même.
  static const char* IconPath(uint16_t efst);

 private:
  void Apply(uint32_t gid, uint16_t efst, bool active, uint32_t remain_ms,
             uint32_t total_ms);
  // Un membre du groupe par tick, en rotation. Interroger les 24 à chaque fois
  // ferait des rafales pour une information qui bouge lentement.
  void PollParty();
  // Le serveur a-t-il refusé ce GID récemment ? Un refus est une règle, pas un
  // incident : on se tait au lieu de redemander en boucle.
  bool Refused(uint32_t gid) const;
  // 🔴 A-t-on le DROIT de retenir les états de ce GID ? ZC 0x0983 est diffusé en
  // AREA — il arrive pour tout joueur à l'écran, adversaire PVP compris — alors
  // que notre paquet, lui, est gaté côté serveur. Sans ce filtre, la diffusion
  // native remplirait la table de ce que la gate refuse. La règle est déléguée
  // au serveur : groupe, ou GID qu'il vient lui-même de renseigner.
  bool Allowed(uint32_t gid) const;

  // GID -> ses états. Purgée par `OnTick` : les entrées échues, et les entités
  // dont l'acteur a quitté la scène — un membre hors de portée n'affiche rien,
  // sa tuile avouant déjà ignorer ses PV.
  std::unordered_map<uint32_t, std::vector<Entry>> by_gid_;

  // 🔴 Les GID que le PAQUET a renseignés, avec l'instant de la réponse.
  //
  // Sert au DROIT D'ENTRÉE (`Allowed`), pas à la fraîcheur : un GID que le
  // serveur vient de nous décrire est un GID qu'il nous autorise à suivre, donc
  // ses transitions AREA sont recevables. La présence de l'acteur, elle, reste
  // seule juge de ce qu'on GARDE.
  std::unordered_map<uint32_t, uint32_t> answered_ms_;

  // GID -> instant du refus (statut 2 : pas de mon groupe).
  std::unordered_map<uint32_t, uint32_t> refused_ms_;

  bool     polling_wanted_      = false;
  unsigned last_poll_ms_        = 0;
  unsigned last_target_poll_ms_ = 0;
  size_t   poll_cursor_         = 0;
};
