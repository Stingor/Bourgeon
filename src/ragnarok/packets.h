#pragma once

#include <cstdint>

enum class PacketHeader { CZ_USE_ITEM = 0x439 };

#pragma pack(push, 1)

struct PACKET_CZ_USE_ITEM {
  int16_t header;
  uint16_t index;
  uint32_t aid;
};

#pragma pack(pop)

// ── Notifications de COUP (ZC, serveur → client) ─────────────────────────────
//
// Les trois paquets qui portent un dégât coup par coup — et les seuls à porter
// aussi l'amotion de l'attaquant et la dmotion imposée à la cible, frappe par
// frappe. Deux modules les lisent, pour deux usages sans rapport : le compteur
// de DPS (`features/overlays/dps_meter`) et la fiche technique du personnage
// (`features/windows/char_diagnostics`).
//
// 🔴 CHACUN EN TENAIT SA PROPRE COPIE, mot pour mot, dans son namespace anonyme,
// et sous DEUX jeux de noms d'opcodes concurrents (`kOpcodeNotifyAct` d'un côté,
// `kOpNotifyAct` de l'autre) — donc introuvables l'un depuis l'autre. Une
// disposition corrigée d'un côté après une RE aurait laissé l'autre
// silencieusement fausse, et un décalage d'un octet ici ne plante pas : il
// affiche des chiffres crédibles.
//
// S'inscrire des deux côtés sur le même opcode ne pose aucun problème :
// l'inscription retient le MAXIMUM des longueurs demandées.
//
// ⚠ Ces dispositions commencent APRÈS les deux octets d'opcode.
namespace rag::zc {

constexpr uint16_t kNotifyAct    = 0x08c8;  // ZC_NOTIFY_ACT   (PACKETVER >= 20131223)
constexpr uint16_t kNotifySkill  = 0x01de;  // ZC_NOTIFY_SKILL (PACKETVER >= 3)
constexpr uint16_t kNotifySkill2 = 0x0115;  // ZC_NOTIFY_SKILL, forme héritée

#pragma pack(push, 1)

// 32 octets. [0..3] src_id, [4..7] dst_id, [8..11] tick, [12..15] src_speed,
// [16..19] dst_speed, [20..23] damage, [24] is_sp_damage, [25..26] div,
// [27] type, [28..31] damage2.
struct NotifyActPayload {
  int32_t  src_id;
  int32_t  dst_id;
  int32_t  tick;
  int32_t  src_speed;     // amotion de l'attaquant
  int32_t  dst_speed;     // dmotion imposé à la cible
  int32_t  damage;
  int8_t   is_sp_damage;
  uint16_t div;
  uint8_t  type;          // 0 atk, 4 endure, 8 multi, 10 crit, 11 miss
  int32_t  damage2;
};

// 31 octets. [0..1] skill_id, [2..5] src_id, [6..9] dst_id, [10..13] start_time,
// [14..17] attack_mt, [18..21] attacked_mt, [22..25] damage, [26..27] level,
// [28..29] count, [30] action.
struct NotifySkillPayload {
  uint16_t skill_id;
  uint32_t src_id;
  uint32_t dst_id;
  uint32_t start_time;
  int32_t  attack_mt;     // amotion
  int32_t  attacked_mt;   // dmotion
  int32_t  damage;
  int16_t  level;
  int16_t  count;
  int8_t   action;
};

// 33 octets. [0..1] skill_id, [2..5] src_id, [6..9] dst_id, [10..13] tick,
// [14..17] sdelay, [18..21] ddelay, [22..23] dst_x, [24..25] dst_y,
// [26..27] damage (int16 ici, pas int32), [28..29] skill_lv, [30..31] div,
// [32] type.
struct NotifySkill2Payload {
  uint16_t skill_id;
  uint32_t src_id;
  uint32_t dst_id;
  uint32_t tick;
  uint32_t sdelay;
  uint32_t ddelay;
  uint16_t dst_x;
  uint16_t dst_y;
  int16_t  damage;
  uint16_t skill_lv;
  uint16_t div;
  uint8_t  type;
};

#pragma pack(pop)

// ── Les opcodes VANILLA que plusieurs modules observent ─────────────────────
//
// 🔴 POURQUOI ILS SONT ICI, et pas là où on les emploie. Les opcodes maison ont
// un domicile depuis longtemps (`bopcodes`, features/systems/bourgeon_opcodes.h).
// Les opcodes du CLIENT, eux, n'en avaient aucun : chaque module déclarait ceux
// dont il avait besoin, ce qui va très bien tant qu'un seul module les lit —
// mais CINQ d'entre eux étaient lus par deux ou trois modules, et chacun les
// avait donc nommés à sa façon.
//
// Trois conventions coexistaient dans le dépôt : `kOp*`, `kOpcode*`, et
// `kZc*`/`kCz*`. La valeur 0x0091 portait les TROIS à la fois —
// `Bourgeon::kOpMapChange`, `MoonlightUi::kOpcodeMapMove`,
// `npc_dialog_window::kZcMapChange` — donc introuvable depuis l'un quelconque
// des trois. C'est le même aveuglement que celui décrit en tête de
// `ragnarok/actor.h` : la même valeur sous des noms différents.
//
// La convention retenue est celle que ce fichier applique déjà : le NAMESPACE
// porte la direction (`zc` = serveur vers client, `cz` = client vers serveur) et
// le nom dit le rôle, sans préfixe redondant.
//
// ⚠ N'y monter QUE ce qui est réellement partagé. Un opcode lu par un seul
// module est mieux chez lui, à côté du code qui le décode — c'est le cas de la
// vingtaine de `kZc*` de `npc_dialog_window`, qui restent locaux.
//
// ⚠⚠ PIÈGE RELEVÉ EN CHEMIN : `grey_world.cc` déclare aussi un `kZcMapChange`,
// mais c'est une ADRESSE NATIVE (0x00ccea30, le handler du paquet), pas un
// opcode. Même nom, deux natures, deux fichiers. Un renommage global de ce nom
// aurait mordu dedans.

// ── Warp et changement de carte ──────────────────────────────────────────────
// Les deux paquets qui annoncent un changement de carte. Ils arrivent AVANT le
// chargement — c'est ce qui laisse à une boutique le temps d'émettre sa
// fermeture pendant qu'on est encore connecté.
//
// ⚠ NE PAS CONFONDRE AVEC `Bourgeon::MapLoadEpoch()`, qui s'incrémente PENDANT
// le chargement. Les deux signaux disent « la carte change » à des instants
// différents, et les échanger changerait le comportement.
//
// 0x0091 porte `mapname[16]` juste après l'opcode, puis x et y : c'est de là que
// MoonlightUi lit le nom de la carte courante, sans paquet maison ni
// modification serveur.
constexpr uint16_t kMapChange  = 0x0091;  // ZC_NPCACK_MAPMOVE    [name:16][x:2][y:2]
constexpr uint16_t kServerMove = 0x0092;  // ZC_NPCACK_SERVERMOVE
constexpr uint16_t kMapNameLen = 16;      // largeur du champ mapname de 0x0091

// Le nom ET le titre d'un PNJ, demandés par la fenêtre de dialogue comme par la
// boutique NPC — les deux en ont besoin pour titrer leur fenêtre.
constexpr uint16_t kNpcName = 0x0adf;  // ZC_ACK_REQNAMEALL_NPC
                                       // {gid:4, groupId:4, name[24], title[24]}

}  // namespace rag::zc

// ── CZ : ce que le client envoie ─────────────────────────────────────────────
// Même règle que pour `zc` : n'y monter que le partagé.
namespace rag::cz {

// Inviter en guilde PAR LE NOM (et non par l'AID) : la fiche de personnage
// l'émet depuis son onglet Guilde, la chatbox depuis sa commande.
constexpr uint16_t kJoinGuildByName = 0x0916;  // CZ_REQ_JOIN_GUILD2 {op, name[24]}

// Fermer la session NPC côté SERVEUR. La fenêtre de dialogue l'envoie sur sa
// croix, la boutique NPC en sortant de l'étal : les deux ferment la même chose.
constexpr uint16_t kCloseDialog = 0x0146;  // CZ_CLOSE_DIALOG {op, GID} 6 o

}  // namespace rag::cz
