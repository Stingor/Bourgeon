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
// branches passent. Et `clif_efst_status_change_sub` (clif.cpp:11075) REJOUE
// tous les statuts actifs d'une entité quand elle entre dans la vue — on n'a
// donc pas à attendre le prochain buff pour connaître qui vient d'apparaître.
//
// 🔴 AREA, et rien d'autre. Un membre du groupe sur une autre carte, ou à
// l'autre bout de celle-ci, n'émet RIEN. C'est le même mur que pour le SP d'un
// tiers, et il se franchit de la même façon : un paquet à nous. Ce module-ci ne
// couvre que ce qui est à l'écran.
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

  // GID -> ses états. Purgée par `OnTick` : les entrées échues, et les entités
  // dont l'acteur a disparu de la scène.
  std::unordered_map<uint32_t, std::vector<Entry>> by_gid_;
};
