#pragma once

#include <cstdint>

#include "features/plugin.h"
#include "features/systems/bourgeon_opcodes.h"

// ── UiCaps — DIRE AU SERVEUR CE QUE CE CLIENT SAIT AFFICHER ──────────────────
//
// Le serveur sait déjà qu'il parle à un client Bourgeon : c'est le rôle du
// handshake d'intégrité (CZ 0x0F02 -> `sd->state.has_bourgeon`). Ce qu'il ne sait
// pas, c'est laquelle des interfaces modernes est ALLUMÉE — elles sont toutes
// opt-in, et le joueur peut en éteindre une au milieu d'une conversation.
//
// 🔴 LA DIFFÉRENCE COMPTE, et c'est tout l'objet de ce module. Un script NPC qui
// écrit `<MOBL>1002:0:Poring</MOBL>` produit un lien cliquable chez qui a le
// dialogue moderne, et une ligne de charabia à chevrons chez qui a gardé le
// dialogue natif — lequel efface les balises qu'il connaît et laisse passer les
// autres. Sans ce paquet, le serveur ne peut pas choisir, donc personne ne peut
// se servir des balises maison en dehors du chat.
//
// ── Ce que le masque décrit ──────────────────────────────────────────────────
// Une CAPACITÉ D'AFFICHAGE, jamais une préférence de jeu. Un bit répond à « si
// j'envoie du markup Bourgeon sur cette surface, sera-t-il rendu ? » — pas à
// « ce joueur aime-t-il les images ». Les préférences, elles, ont déjà leur
// chemin (CZ_BOURGEON_SETTING) et sont PERSISTÉES côté serveur ; ceci ne l'est
// pas et ne doit pas l'être : c'est un état de SESSION, refait à chaque login.
//
// ── Sur le fil ───────────────────────────────────────────────────────────────
//   CZ_BOURGEON_UI_CAPS 0x0F24 : [opcode:2][len:2][caps:4]  (len = 8)
// Variable, comme les autres customs, pour qu'un champ puisse s'ajouter sans
// désaccorder les deux côtés.
//
// ⚠ ORDRE D'ENVOI. Le serveur ignore tout ZC/CZ Bourgeon tant que
// `has_bourgeon` n'est pas posé. On n'envoie donc pas « à l'entrée en jeu » mais
// APRÈS le premier paquet qui prouve que le serveur nous a reconnus
// (ZC_BOURGEON_SETTINGS, poussé par `clif_bourgeon_grant_verified`). C'est le
// même signal qui ré-arme l'envoi après un changement de personnage — la
// vérification est refaite par session de zone.
class UiCaps : public Plugin {
 public:
  UiCaps();

  const char* name() const override { return "UiCaps"; }

  // Les bits. ⚠ Miroir EXACT de `e_bourgeon_ui_cap` côté moonlight : ajouter un
  // bit ici sans l'ajouter là-bas donne une capacité que personne ne teste, et
  // l'inverse une capacité que personne n'annonce.
  enum Cap : uint32_t {
    // Le dialogue NPC est rendu par l'overlay moderne : il connaît `<MOBL>`,
    // `<ITMR>`, `<CRAF>`, `<SETL>`, `<STAL>`, `<IMG>`, `<MOBS>` et `<MOBP>`.
    kNpcDialog = 1u << 0,
    // La chatbox est la nôtre : mêmes balises de LIEN, `<STAL>` compris (pas
    // les médias, qui n'auraient pas de place dans une ligne de log).
    kChat      = 1u << 1,
    // Le carnet de chasse MVP est allumé : les deltas d'observation seront
    // montrés. Éteint, le serveur cesse de nous les diffuser — mais nos kills
    // continuent d'ALIMENTER le groupe. Ce n'est donc pas un cas dégradé.
    kMvpTracker = 1u << 2,
    // L'album de cartes est disponible dans cette interface. Sans ce bit le
    // serveur REFUSE toute commande d'album — et c'est voulu : le sacrifice
    // d'une carte est irréversible, et un client natif n'a aucune surface pour
    // montrer au joueur ce qu'il vient de payer.
    kCardAlbum = 1u << 3,
    // Cette build sait lire l'octet de NATURE du catalogue d'album (0 ordinaire,
    // 1 mini-boss, 2 MVP), qui donne au liséré de la pochette sa couleur.
    //
    // 🔴 Une CAPACITÉ DE LECTURE, pas une préférence : le joueur peut éteindre le
    // liséré, le bit reste. Il dit ce que cette build SAIT LIRE sur le fil, et
    // s'éteindre ferait envoyer par le serveur des entrées plus courtes que ce
    // que le parseur attend déjà — une préférence n'a rien à faire là.
    kCardAlbumBoss = 1u << 4,
    // RÉSERVÉS côté serveur pour le client moonclient, et JAMAIS annoncés par
    // cette DLL — elle n'enregistre pas leurs opcodes, et un opcode inconnu
    // viderait le tampon de réception du client :
    //   1u << 5 (0x20) : ZC_BOURGEON_FLAG_GRAFFITI 0x0F36 ;
    //   1u << 6 (0x40) : ZC_BOURGEON_UNIT_MASTER 0x0F37, le maître d'un monstre
    //                    invoqué (voir bopcodes::kUnitMaster) ;
    //   1u << 7 (0x80) : ZC_BOURGEON_SERVER_RULES 0x0F38, les réglages du serveur
    //                    utiles au client (voir bopcodes::kServerRules) ;
    //   1u << 8 (0x100) : BOURGEON_UI_MVP_TRACKER_EXT, le carnet MVP étendu. Pas
    //                    d'opcode neuf : le serveur ajoute des QUEUES à
    //                    ZC 0x0F32 (notre user_id derrière le groupe, l'origine
    //                    derrière l'invitation, l'identifiant de commande derrière
    //                    le résultat), pousse la présence des membres, et rend
    //                    les codes de résultat 14 à 17, que `ResultText` ne
    //                    connaît pas. Sans ce bit, cette DLL reçoit les trames
    //                    d'avant, octet pour octet.
    //   1u << 9 (0x200) : ZC_BOURGEON_CHAT_AUTHOR 0x0F3A, l'auteur de la ligne
    //                    de parole qui suit (voir bopcodes::kChatAuthor) ;
    //   1u << 10 (0x400) : ZC_BOURGEON_QUEST_END 0x0F3B, pourquoi une quête
    //                    quitte le journal (voir bopcodes::kQuestEnd) ;
    //   1u << 11 (0x800) : ZC_BOURGEON_DISCORD_RICH 0x0F3C, le relais Discord
    //                    riche, à la place de 0x0F08 (voir bopcodes::kDiscordRich) ;
    //   1u << 12 (0x1000) : le maître des COMPAGNONS dans ZC_BOURGEON_UNIT_MASTER
    //                    0x0F37 — homoncule, mercenaire, familier, élémentaire.
    //                    Sans ce bit, 0x0F37 ne décrit que des monstres.
    // Prochain bit libre : 1u << 13.
  };

  void OnTick() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Le masque tel qu'il vaut MAINTENANT, relu depuis les plugins concernés. Un
  // plugin absent (non enregistré dans cette build) vaut capacité absente : on
  // n'annonce que ce qu'on peut tenir.
  static uint32_t Current();

 private:
  void Send(uint32_t caps);

  static constexpr uint16_t kOpcodeToServer = bopcodes::kUiCaps;   // CZ
  static constexpr uint16_t kOpcodeSettings = bopcodes::kSettings;  // ZC (signal de reconnaissance)

  bool     known_ = false;       // le serveur nous a reconnus dans CETTE session
  bool     sent_  = false;       // `last_` a bien été accepté par la socket
  uint32_t last_  = 0;           // dernier masque envoyé (anti-répétition)
};
