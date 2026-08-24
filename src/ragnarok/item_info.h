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

// Plafond du parcours : garde-fou anti-boucle, la liste s'arrêtant sur sa
// sentinelle. Une seule valeur pour les trois listes — la plus grosse, celle du
// storage premium, plafonne à 600 côté serveur.
constexpr int kWalkGuard = 4000;

}  // namespace rag::itemlist
