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
constexpr uint16_t kReqTechData = 0x0F0B;  // CZ_BOURGEON_REQ_TECHDATA (item/skill tech)
constexpr uint16_t kReqDamage   = 0x0F0D;  // CZ_BOURGEON_REQ_DAMAGE (estim. dégâts skill)
constexpr uint16_t kReqItemScript = 0x0F11;  // CZ_BOURGEON_REQ_ITEMSCRIPT (script brut d'un item)
constexpr uint16_t kBugReport   = 0x0F13;  // CZ_BOURGEON_BUG_REPORT (rapport de bug joueur)
constexpr uint16_t kCompanion   = 0x0F15;  // CZ_BOURGEON_COMPANION (invoquer/basculer cart/peco/falcon)
constexpr uint16_t kReqCompatCards = 0x0F18;  // CZ_BOURGEON_REQ_COMPAT_CARDS (cartes sertissables sur un équip)
constexpr uint16_t kJump        = 0x0F1A;  // CZ_BOURGEON_JUMP (« j'ai sauté » ; sans payload)
constexpr uint16_t kOpenStorage = 0x0F1D;  // CZ_BOURGEON_OPEN_STORAGE (ouvrir/basculer de storage)
constexpr uint16_t kReqMobInfo  = 0x0F1F;  // CZ_BOURGEON_REQ_MOBINFO (fiche détaillée d'un monstre)
constexpr uint16_t kReqEntityProps = 0x0F22;  // CZ_BOURGEON_REQ_ENTITY_PROPS (propriétés serveur d'une entité — STAFF)
constexpr uint16_t kUiCaps      = 0x0F24;  // CZ_BOURGEON_UI_CAPS (ce que l'interface moderne SAIT AFFICHER)
constexpr uint16_t kNpcAdmin    = 0x0F25;  // CZ_BOURGEON_NPC_ADMIN (recharger/décharger/déplacer un NPC — ADMIN)
constexpr uint16_t kStyle       = 0x0F26;  // CZ_BOURGEON_STYLE (style choisi : couleurs de corps, palette de cheveux, coiffure)
constexpr uint16_t kReqTargetInfo = 0x0F29;  // CZ_BOURGEON_TARGET_INFO (état de l'entité ciblée ; réémis tant que la fenêtre de cible est ouverte)
// Les bits du champ `known` de sa RÉPONSE : ils disent ce qui est RENSEIGNÉ.
// Un adversaire hors groupe ne reçoit que son type — d'où ce masque, seul moyen
// de distinguer « 0 SP » de « SP inconnu ». 🔴 `target_frame` les portait tous
// les quatre et `party_frames` en recopiait un : c'est le paquet qui les définit,
// pas ses lecteurs.
constexpr uint8_t kKnownHp    = 1;
constexpr uint8_t kKnownSp    = 2;
constexpr uint8_t kKnownLevel = 4;
constexpr uint8_t kKnownKind  = 8;
constexpr uint16_t kReqStatusList = 0x0F2C;  // CZ_BOURGEON_REQ_STATUS_LIST (etats actifs d'une entite ; le protocole n'annonce que les transitions)
constexpr uint16_t kPlayerAdmin = 0x0F2B;  // CZ_BOURGEON_PLAYER_ADMIN (outillage staff sur un joueur : venir, mute, jail, ban, points d'event)
constexpr uint16_t kReqLooks    = 0x0F2E;  // CZ_BOURGEON_REQ_LOOKS (apparence des membres du groupe / des amis EN LIGNE ; leurs paquets ne la portent pas, contrairement a ceux de la guilde)

// --- ZC : serveur -> client (livrés par le reader-hook) ---------------------
constexpr uint16_t kKickNotice  = 0x0F03;  // ex-0x0BFA  ZC_BOURGEON_KICK_NOTICE
constexpr uint16_t kSettings    = 0x0F05;  // ex-0x0BFE  ZC_BOURGEON_SETTINGS
constexpr uint16_t kPresetList  = 0x0F07;  // ex-0x0C21  ZC_BOURGEON_PRESET_LIST
constexpr uint16_t kDiscordMsg  = 0x0F08;  // ex-0x0C1F  ZC_BOURGEON_DISCORD_MSG
constexpr uint16_t kSkillDmg    = 0x0F09;  // ex-0x0C22  ZC_BOURGEON_SKILL_DMG
constexpr uint16_t kTechData    = 0x0F0C;  // ZC_BOURGEON_TECHDATA (réponse tech)
constexpr uint16_t kDamage      = 0x0F0E;  // ZC_BOURGEON_DAMAGE (réponse estim. dégâts)
constexpr uint16_t kStoragePrices = 0x0F0F;  // ZC_BOURGEON_STORAGE_PRICES (prix vente storage)
constexpr uint16_t kStatBonus     = 0x0F10;  // ZC_BOURGEON_STAT_BONUS (apport équip/cartes aux stats)
constexpr uint16_t kItemScript    = 0x0F12;  // ZC_BOURGEON_ITEMSCRIPT (script brut d'un item)
constexpr uint16_t kBugReportAck  = 0x0F14;  // ZC_BOURGEON_BUG_REPORT_ACK (accusé de réception)
constexpr uint16_t kCompanionState = 0x0F16;  // ZC_BOURGEON_COMPANION_STATE (niveaux skills + états cart/peco/falcon)
constexpr uint16_t kHatEffectMap   = 0x0F17;  // ZC_BOURGEON_HATEFFECT_MAP (itemId->ordinal hat effect ; preview costumes sans viewid)
constexpr uint16_t kCompatCards    = 0x0F19;  // ZC_BOURGEON_COMPAT_CARDS (liste des cartes sertissables sur un équip)
constexpr uint16_t kJumpNotify     = 0x0F1B;  // ZC_BOURGEON_JUMP (AREA sans self : le GID a sauté)
constexpr uint16_t kCookMastery    = 0x0F1C;  // ZC_BOURGEON_COOK_MASTERY (char reg COOK_MASTERY, [0,1999])
constexpr uint16_t kStorageList    = 0x0F1E;  // ZC_BOURGEON_STORAGE_LIST (storages accessibles + ouvert)
constexpr uint16_t kMobInfo        = 0x0F20;  // ZC_BOURGEON_MOBINFO (fiche monstre : stats, drops, spawns, skills)
constexpr uint16_t kChannelList    = 0x0F21;  // ZC_BOURGEON_CHANNEL_LIST (canaux de chat atteignables)
constexpr uint16_t kEntityProps    = 0x0F23;  // ZC_BOURGEON_ENTITY_PROPS (liste clé/valeur décrivant une entité — STAFF)
constexpr uint16_t kStyles         = 0x0F27;  // ZC_BOURGEON_STYLES (styles des joueurs en vue — LOT)
constexpr uint16_t kStyleOpen      = 0x0F28;  // ZC_BOURGEON_STYLE_OPEN (un NPC ouvre/ferme l'éditeur de style)
constexpr uint16_t kTargetInfo     = 0x0F2A;  // ZC_BOURGEON_TARGET_INFO (PV/SP/niveau/race/élément de la cible ; SP introuvable autrement)
constexpr uint16_t kStatusList     = 0x0F2D;  // ZC_BOURGEON_STATUS_LIST (liste COMPLETE des buffs/debuffs ; remplace ce qu'on savait du GID)
constexpr uint16_t kLooks          = 0x0F2F;  // ZC_BOURGEON_LOOKS (aid + job + coiffure + couleur + sexe, pour composer une tete hors de portee)

// --- PROCHAIN OPCODE LIBRE : la valeur de kNextFree ci-dessous ---------------
// Pour ajouter un opcode custom : prendre la valeur ci-dessous, l'incrémenter,
// déclarer la constante ici (préfixe CZ/ZC), puis la mirrorer côté serveur.
// Aucune vérification de collision nécessaire : toute la plage 0x0F00..0x0FFF
// est hors de la table du client (garantie flag=-1 = variable). Champ libre.
// (0x0F00/0x0F01 libérés — anciennes valeurs tech data avant regroupement.)
constexpr uint16_t kNextFree    = 0x0F30;
}  // namespace bopcodes
