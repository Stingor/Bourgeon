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
    // Prochain bit libre : 1u << 3.
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
