#pragma once

// ── Cooldowns de compétences (ZC_SKILL_POSTDELAY 0x043D) ─────────────────────
//
// POURQUOI CE MODULE. Le client garde ses cooldowns dans une std::list globale,
// g_ShortCutCooldownList (0x015ff7e0) : c'est elle que la barre de raccourcis
// native balaie pour assombrir une case. Mais elle n'est PAS la table des
// cooldowns du personnage — c'est un sous-produit du DESSIN de la barre native.
//
// RE de Recv_SkillPostDelay (0x00cd60b0, case 1085 = ZC 0x043D) : le handler
// parcourt les listes d'icônes DESSINÉES par les deux barres de raccourcis
// (0x015fa434 et 0x015fa43c, vidées puis reconstruites à chaque OnDraw, cf.
// UIShortCutWnd_OnDraw 0x008f5800) et n'insère un nœud de cooldown QUE si le
// skill s'y trouve déjà. Conséquences :
//   • barre native cachée (c'est le cas dès que la barre ImGui moderne prend la
//     main) -> les listes d'icônes restent vides -> AUCUN cooldown n'est jamais
//     enregistré, quel que soit le skill ;
//   • même barre native visible, seules les cases de l'ONGLET AFFICHÉ comptent —
//     les barres 2 et 3 de la multibar moderne n'auraient rien.
// Lire la liste native était donc une impasse : c'est le paquet serveur qui fait
// autorité, et lui arrive toujours.
//
// Le serveur (rAthena) envoie un 0x043D par compétence bloquée, avec une DURÉE en
// ms — jamais une échéance. ⚠ En pré-renewal, lancer une compétence de guilde les
// bloque toutes les quatre (guild.cpp guild_block_skill) : quatre paquets. Une
// durée de 0 signifie « prête » (skill_blockpc_clear).
//
// Alimenté en un seul point — Bourgeon::FireRecvPacket — pour que la table vive
// même si aucun plugin consommateur n'est chargé, et qu'un paquet ne soit jamais
// compté deux fois.

#include <cstdint>

namespace ro {

// Enregistre l'observation des paquets de cooldown. À appeler une fois au
// démarrage, avant tout plugin.
void InstallSkillCooldowns();

// Ingestion d'un paquet reçu ; ignore silencieusement les opcodes étrangers.
void FeedSkillCooldownPacket(uint16_t opcode, const uint8_t* data, uint16_t len);

// Vide la table (retour au login / changement de personnage : les cooldowns du
// perso précédent ne concernent pas le suivant, et le serveur renvoie ceux qui
// restent à l'entrée en jeu).
void ClearSkillCooldowns();

// Temps restant en ms, 0 = compétence prête.
unsigned long SkillCooldownRemainingMs(uint16_t skill_id);

// Fraction de cooldown restante dans [0,1] (0 = prête), pour un voile de case.
// Repli sur la liste native quand le skill n'est pas dans notre table : elle
// couvre les rares cooldowns posés par le client lui-même.
float SkillCooldownFraction(uint16_t skill_id);

}  // namespace ro
