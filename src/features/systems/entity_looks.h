#pragma once

// ── L'APPARENCE des membres du groupe et des amis ───────────────────────────
//
// 🔴 POURQUOI CE REGISTRE EXISTE. La fenêtre des membres de guilde montre la
// tête de chacun parce que `ZC_MEMBERMGR_INFO` PORTE la coiffure, sa couleur et
// le sexe. Les paquets de groupe et d'amis ne portent rien de tel : sans ce
// registre, l'apparence ne peut venir que de l'ACTEUR, donc de la portée — un
// membre sur une autre carte n'a pas de tête, et un ami n'en a presque jamais.
//
// D'où un couple à nous, CZ 0x0F2E / ZC 0x0F2F, qui rend l'apparence de qui est
// EN LIGNE, où qu'il soit.
//
// ⚠ La gate est côté SERVEUR, et c'est le seul endroit où elle vaille : il ne
// renseigne que le groupe du demandeur et ses amis. Le champ `what` de la
// requête RESTREINT ce qu'on demande, il n'autorise rien — un client qui
// poserait tous les bits n'obtiendrait pas l'apparence d'un inconnu.
//
// ── Ce que ce registre ne fait PAS ──────────────────────────────────────────
//
// Il ne dit pas qui est en ligne : les listes natives le disent déjà, et un
// joueur hors ligne n'a simplement pas d'entrée ici. Il ne remplace pas non
// plus l'acteur — quand il est là, il est plus frais (un changement de coiffure
// s'y voit tout de suite). L'ordre de lecture appartient à l'appelant : acteur
// d'abord, ce registre en repli.

#include <cstdint>
#include <unordered_map>

#include "features/plugin.h"

class EntityLooks : public Plugin {
 public:
  struct Look {
    uint16_t job        = 0;
    uint16_t hair       = 0;
    uint16_t hair_color = 0;
    uint8_t  sex        = 1;  // 0 = femme
  };

  EntityLooks();

  const char* name() const override { return "EntityLooks"; }

  // 🔴 LES DEUX, et ce n'est pas facultatif. `HandlePacket` n'est appelé que
  // par `DrainNetInbox`, qui vide la file que `OnRecvPacket` remplit : sans ce
  // premier, le paquet arrive, personne ne le range, et le décodeur ne tourne
  // JAMAIS. Rien ne le signale — ni erreur, ni avertissement, juste une table
  // qui reste vide.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // ── La demande ────────────────────────────────────────────────────────────
  //
  // Une surface qui affiche des têtes le DEMANDE, à chaque frame où elle en
  // affiche. Sans demande vivante, aucun paquet ne part — c'est le patron de
  // `StatusEffects::RequestPolling`, et pour la même raison : une fenêtre qu'on
  // n'ouvre jamais ne doit rien coûter au serveur.
  //
  // ⚠ Les deux listes sont demandées SÉPARÉMENT : le réglage distingue « groupe »
  // d'« amis », et une demande commune ferait parcourir quarante amis pour un
  // onglet qui n'en montre aucun.
  void RequestParty()   { want_party_   = true; }
  void RequestFriends() { want_friends_ = true; }

  // L'apparence connue de ce GID. Faux quand on ne sait rien de lui — ce qui
  // veut dire « hors ligne, ou pas encore répondu », jamais « sans tête ».
  bool Of(uint32_t gid, Look* out) const;

 private:
  void Poll(uint8_t what);

  std::unordered_map<uint32_t, Look> by_gid_;

  bool     want_party_   = false;
  bool     want_friends_ = false;
  unsigned last_poll_ms_ = 0;
};
