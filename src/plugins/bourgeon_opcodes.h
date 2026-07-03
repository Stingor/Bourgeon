#pragma once

#include <cstdint>

// =============================================================================
// Source unique de vérité des opcodes de paquets custom Bourgeon.
//
// ZONE SÛRE 0x0F00+ : au-dessus de l'opcode max connu du client (0x0C35), donc
// chaque opcode ici est vu par le client comme flag=-1 (variable, inconnu) dans
// sa table de longueurs. Conséquences :
//   - IMMUNISÉ contre les montées de version du client (aucun paquet vanilla ne
//     partage ces numéros, ni maintenant ni sur un Ragexe plus récent) ;
//   - IMMUNISÉ contre la classe de bug « collision de longueur fixe » (cf. le
//     gel historique de 0x0C22 quand il partageait le fixe-12 vanilla).
//
// Livraison côté client :
//   - CZ (client -> serveur) : envoi direct, aucune contrainte client.
//   - ZC (serveur -> client)  : > 0x0C35 donc HORS dispatch table -> livrés par
//     le reader-hook de RagConnection (s_reader_dispatch_opcodes_). Éprouvé.
//
// ⚠ DOIT rester synchronisé avec le serveur (moonlight : packets_struct.hpp +
// clif_packetdb.hpp + handlers). Tout changement ici = rupture de protocole
// coordonnée : déployer client ET serveur ensemble.
//
// Carte complète des opcodes du client : Bourgeon/docs/opcode_map.md
// =============================================================================

namespace bopcodes {

// --- CZ : client -> serveur -------------------------------------------------
constexpr uint16_t kIntegrity   = 0x0F02;  // ex-0x0BFB  CZ_BOURGEON_INTEGRITY
constexpr uint16_t kSetting     = 0x0F04;  // ex-0x0BFD  CZ_BOURGEON_SETTING
constexpr uint16_t kPresetCmd   = 0x0F06;  // ex-0x0C20  CZ_BOURGEON_PRESET_CMD
constexpr uint16_t kCheatReport = 0x0F0A;  // ex-0x0C23  CZ_BOURGEON_CHEAT_REPORT
constexpr uint16_t kReqTechData = 0x0F00;  // (déjà)      CZ_BOURGEON_REQ_TECHDATA

// --- ZC : serveur -> client (livrés par le reader-hook) ---------------------
constexpr uint16_t kKickNotice  = 0x0F03;  // ex-0x0BFA  ZC_BOURGEON_KICK_NOTICE
constexpr uint16_t kSettings    = 0x0F05;  // ex-0x0BFE  ZC_BOURGEON_SETTINGS
constexpr uint16_t kPresetList  = 0x0F07;  // ex-0x0C21  ZC_BOURGEON_PRESET_LIST
constexpr uint16_t kDiscordMsg  = 0x0F08;  // ex-0x0C1F  ZC_BOURGEON_DISCORD_MSG
constexpr uint16_t kSkillDmg    = 0x0F09;  // ex-0x0C22  ZC_BOURGEON_SKILL_DMG
constexpr uint16_t kTechData    = 0x0F01;  // (déjà)      ZC_BOURGEON_TECHDATA

// --- PROCHAIN OPCODE LIBRE : 0x0F0B -----------------------------------------
// Pour ajouter un opcode custom : prendre la valeur ci-dessous, l'incrémenter,
// déclarer la constante ici (préfixe CZ/ZC), puis la mirrorer côté serveur.
// Aucune vérification de collision nécessaire : toute la plage 0x0F00..0x0FFF
// est hors de la table du client (garantie flag=-1 = variable). Champ libre.
constexpr uint16_t kNextFree    = 0x0F0B;

}  // namespace bopcodes
