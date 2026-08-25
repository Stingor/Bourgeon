#pragma once

#include <cstdint>
#include <cstdlib>  // atoi
#include <excpt.h>  // __try / __except — jamais <Windows.h> dans un en-tête
#include <string>

#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client

// 🔴 CE LAYOUT EST FAUX EN PARTIE sur le client 20250716 — vérifié en jeu
// (2026-07-29). Confirmés : `num_` (+0x10) et `item_name_` (+0x2C). En revanche
// l'INDEX D'INVENTAIRE est à **+0x04**, là où `location_` est déclaré : les deux
// champs sont intervertis (lire +0x08 donnait 0 sur des lignes pourtant valides,
// et +0x04 a rendu l'index 20 attendu pour le Mini Furnace).
// Les champs non cités n'ont JAMAIS été vérifiés : ne rien en déduire.
// Parcours d'inventaire vérifié : global 0x015FBAB0 (liste circulaire), nœud
// { next+0x00, ItemInfo+0x08, amount+0x18 } — cf. features/windows/make_item_window.cc.
struct ItemInfo {
  int item_type_;
  int location_;  // ⚠ c'est ICI (+0x04) que vit l'index d'inventaire
  unsigned long item_index_;  // ⚠ NON : +0x08 lit autre chose (0 en pratique)
  int wear_location_;
  int num_;
  int price_;
  int real_price_;
  int slot_[0x4];
  std::string item_name_;
  unsigned char is_identified_;
  unsigned char is_damaged_;
  int refining_level_;
  unsigned short is_yours_;
  long delete_time_;
};

// ── Le parcours d'une liste d'objets, en OFFSETS VÉRIFIÉS ────────────────────
//
// La `struct` ci-dessus ne sert plus à grand-chose : son layout est faux en
// partie, et personne ne la déréférence. Ce qui est réellement employé, ce sont
// ces cinq offsets — et ils étaient déclarés dans DOUZE fichiers.
//
// 🔴 C'est le même défaut que `kOffOwnActor` (soldé le 2026-08-24), en plus
// gros : des offsets de cette taille n'apparaissent dans AUCUN relevé. Trop
// petits pour le balayage d'adresses, qui commence à 0x400000 ; trop courts pour
// la comparaison de corps de fonction ; et l'expression qui les emploie diffère
// juste assez d'un fichier à l'autre pour échapper au relevé d'expressions.
// Douze déclarations d'un même layout, qu'aucun outil ne pouvait rapprocher.
//
// ⚠ N'y mettre que du VÉRIFIÉ. Les trois champs d'`ItemSkillInfo` ci-dessous ont
// été relus en jeu ; le reste de la structure ne l'a jamais été, et un offset
// « déduit du layout » y serait un piège de plus.
namespace rag::itemlist {

// Nœud de `std::list` MSVC : next@+0, prev@+4, value@+8 — `value` EST
// l'ItemSkillInfo. La liste est CIRCULAIRE et sa sentinelle est ce que le global
// CONTIENT, pas le global lui-même (cf. `itemcell::CountById`, qui porte le
// piège en commentaire : comparer au global fait boucler jusqu'au garde-fou).
constexpr int kNodeNext = 0x00;
constexpr int kNodeInfo = 0x08;

// Champs d'`ItemSkillInfo`, comptés DEPUIS L'INFO.
constexpr int kInfoIndex  = 0x04;  // int : index client (l'argument des commandes)
constexpr int kInfoAmount = 0x10;  // int : quantité de la pile (`num_`)
constexpr int kInfoIdStr  = 0x2c;  // std::string : l'itemId EN TEXTE (le jeu fait atoi)

// Les mêmes vus depuis le NŒUD. Huit fichiers écrivaient `0x18` en littéral avec
// le commentaire « (= info+0x10) » — ici c'est DÉRIVÉ, donc les deux ne peuvent
// plus diverger.
constexpr int kNodeAmount = kNodeInfo + kInfoAmount;
constexpr int kNodeIndex  = kNodeInfo + kInfoIndex;

// L'itemId d'un `ItemSkillInfo`. Le client le range EN TEXTE dans une
// `std::string` et fait `atoi` dessus — ce n'est pas un entier à lire.
//
// Trois fichiers portaient cette lecture sous deux noms (`InfoItemId`, `InfoId`)
// et deux conversions différentes (`atoi`, `strtoul`). Les deux répondent pareil
// sur un id d'objet, mais rien ne le disait.
inline uint32_t ItemId(const void* info) {
  __try {
    const char* s = clientstr::Data(
        reinterpret_cast<const uint8_t*>(info) + kInfoIdStr);
    return (s && *s) ? static_cast<uint32_t>(std::atoi(s)) : 0u;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
}

// ── La DISPOSITION d'un ItemSkillInfo ────────────────────────────────────────
//
// 🔴 CES CHAMPS ÉTAIENT ÉCRITS DANS SEPT FICHIERS. `kInfoDamaged` et
// `kInfoRefine` en SEPT exemplaires chacun, `kInfoIdent` en six — et le même
// offset sous des noms différents (`kInfoNum`/`kInfoAmount`, `kInfoCard0`/
// `kInfoCards`, `kInfoOptCnt`/`kInfoOptCount`/`kInfoOptNum`), ce qu'aucune
// recherche par nom ne rapproche jamais.
//
// ⚠ L'AVERTISSEMENT DU HAUT TIENT TOUJOURS : on n'écrit ici que du VÉRIFIÉ. Il
// visait les offsets DÉDUITS de la `struct` plus haut, dont le layout est faux
// en partie. Ceux-ci ne sont pas déduits — chacun porte sa preuve, et les trois
// qui n'en avaient pas ont été MESURÉS au désassembleur le 2026-08-25 :
//
//   · ctor `0x006a1b20` : il n'initialise que DEUX `std::string`, en +0x2c et
//     +0x44. C'est pour ça que le champ suivant tombe pile à +0x5c.
//   · `ItemSkillInfo_Reset` (`0x6a5ff0`) finit par une boucle de 25 octets à
//     partir de +0x9c — soit CINQ entrées de cinq octets. `kInfoOpts` et
//     `kMaxOpts` ne sont donc plus des relevés concordants : ce sont des faits.
//   · dans la pile de `sub_5A75A0`, la locale qui suit la seconde `std::string`
//     est à +0xB4 d'elle : `0x44 + 0xB4` = **0xF8**, le vrai `sizeof`.
constexpr int kInfoType     = 0x00;  // int : type d'item (ce qui classe les onglets)
constexpr int kInfoLoc      = 0x08;  // u32 : masque d'emplacement d'équipement
                                     //   (l'arg2 des msg 0x13/0x57 ; RE du double-clic natif)
constexpr int kInfoCards    = 0x1c;  // 4 × u32 : cartes / enchantements, 0 = vide
constexpr int kInfoIdent    = 0x5c;  // octet : IDENTIFIÉ ?
constexpr int kInfoDamaged  = 0x5d;  // octet : équipement CASSÉ (rendu rouge)
constexpr int kInfoRefine   = 0x60;  // int : niveau d'affinage
constexpr int kInfoFav      = 0x74;  // octet : favori (RE FUN_0095af80, local_98 = info+0x74)
constexpr int kInfoOptCount = 0x98;  // int : nombre d'options aléatoires
constexpr int kInfoOpts     = 0x9c;  // 5 entrées de 5 octets {index:2, value:2, param:1}

constexpr int kMaxCards = 4;
constexpr int kMaxOpts  = 5;

// 🔴 `kInfoIdent` PORTAIT DEUX NOMS, ET LE SECOND CACHAIT CE QU'IL FAISAIT.
// `item_desc_window` et `npc_dialog_window` l'appelaient `kInfoFlag` et le
// posaient à 1 « pour que GetDescLines lise rec+0x0c ». C'est le MÊME octet :
// poser ce drapeau, c'est DÉCLARER L'OBJET IDENTIFIÉ. Sur un `info` fabriqué sur
// la pile pour interroger la DB, c'est exactement ce qu'on veut — mais il fallait
// que le nom le dise, sans quoi le geste passe pour un réglage d'affichage.
// (Cf. la règle du projet : quand l'identification est inconnue, passer 1.)

// La TAILLE. 🔴 Deux valeurs circulaient sous le même nom : 0xf8 chez
// `make_item_window`, 0x100 ailleurs. Les deux sont justes, mais pas
// interchangeables :
//   · `kInfoSize` (0xf8) est le VRAI `sizeof`, mesuré. C'est le seul PAS valide
//     pour un TABLEAU — `SendProduceCmd` en construit trois d'affilée et le natif
//     les indexe avec le sien. Un pas de 0x100 y décalerait les deux derniers.
//   · `kInfoBuf` (0x100) est l'arrondi confortable pour UNE structure sur la
//     pile. Sur-allouer huit octets ne coûte rien ; c'est ce que font les huit
//     sites qui fabriquent un `info` isolé.
constexpr int kInfoSize = 0xf8;
constexpr int kInfoBuf  = 0x100;

// ⚠ CTOR ET DTOR VONT PAR PAIRES, ET IL Y A DEUX PAIRES QUE L'IDB NOMME PAREIL.
// Cette structure-ci se construit avec `itemdb::kInfoCtorAddr` (0x006a1b20) et se
// détruit avec `kInfoDtorAddr` CI-DESSOUS (0x005a4300) — celui qui libère les deux
// `std::string`. L'AUTRE `ItemSkillInfo_Dtor` (0x00739cd0) appartient à la
// structure que rend `itemdb::kFillInfoByIdAddr` : elle porte une vtable
// `CSkillInfo` en +0 et n'a rien à voir. Les intervertir écrirait un pointeur de
// vtable par-dessus `kInfoType`.
//
// ⚠ Huit sites construisent sans détruire, et c'est SANS FUITE — mais par
// chance, pas par principe : `SetId` n'écrit que dans la chaîne de +0x2c, et un
// id en décimal tient toujours dans le SSO. Le jour où l'on fait écrire +0x44 par
// le client, il FAUDRA détruire.
constexpr uintptr_t kInfoDtorAddr = 0x005a4300;

// Plafond du parcours : garde-fou anti-boucle, la liste s'arrêtant sur sa
// sentinelle. Une seule valeur pour les trois listes — la plus grosse, celle du
// storage premium, plafonne à 600 côté serveur.
constexpr int kWalkGuard = 4000;

}  // namespace rag::itemlist
