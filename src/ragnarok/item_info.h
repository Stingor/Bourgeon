#pragma once

#include <string>

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
