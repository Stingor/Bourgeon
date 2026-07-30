#pragma once

#include <cstdint>
#include <list>
#include <utility>
#include <vector>

#include "ragnarok/item_info.h"
#include "ragnarok/object_layouts/session/macro_utils.h"

// Verification status (all against g_session=0x015fa3c0):
//   CONFIRMED: char_name_ (+0x81A8, mem_scan found "Stingor" string)
//   CONFIRMED: hp_/max_hp_/sp_/max_sp_ (+0x5548-5554, confirmed at full+depleted HP)
//   CONFIRMED: stats block STR/AGI/VIT/INT/DEX/LUK (+0x1664-0x1678)
//   CONFIRMED: aid_ (+0x15E4, DAT_015fb9a4 used as AID in network calls)
//   CONFIRMED: talk_type_table_ (+0x51F8)
//   🔴 WRONG:  item_list_ (+0x16D8) — était noté « LIKELY, xref pattern matches
//              std::list usage ». C'EST FAUX, prouvé en jeu (2026-07-29) : la
//              tête de liste lue vaut 0 et le premier parcours réel a planté le
//              client (`mov eax,[esi]`, esi = 0). L'erreur est restée invisible
//              parce que le seul consommateur (Session::GetItemInfoById) n'était
//              appelé que par RagnarokClient::UseItemById, qui n'avait aucun
//              appelant : du code mort de bout en bout.
//              La VRAIE tête de l'inventaire est le global 0x015FBAB0 (liste
//              circulaire, nœud : next+0x00, ItemInfo+0x08, amount+0x18) — cf.
//              features/windows/make_item_window.cc, vérifié en jeu.
//              ⚠ Le layout d'ItemInfo est lui aussi PARTIELLEMENT faux : num_
//              (+0x10) et item_name_ (+0x2C) sont bons, mais l'index d'inventaire
//              est à **+0x04** et non +0x08 — les deux premiers champs sont
//              intervertis (vérifié en jeu 2026-07-29 : node+0x0C, soit info+0x04,
//              a rendu l'index 20 attendu ; +0x08 lisait 0).
//              L'emplacement de la liste DANS la session reste à retrouver.
//   ESTIMATE:  mkcount_ block (+0xAFC, stable across versions)
//
// NOTE: Fields are listed in ascending offset order (correct memory layout).
//       char_name_ is at +0x81A8 which is above aid_ at +0x15E4 — char_name_
//       must come last in the struct definition.
SESSION_IMPLEMENTATION(20250716, {
  /*+0x000*/ int32_t cur_map_type_;
  /*+0x004*/ uint8_t padding0[0xAF8];
  /*+0xAFC*/ int32_t mkcount_;
  /*+0xB00*/ int32_t haircolor_;
  /*+0xB04*/ int32_t deadcount_;
  /*+0xB08*/ int32_t head_;
  /*+0xB0C*/ int32_t weapon_;
  /*+0xB10*/ int32_t shield_;
  /*+0xB14*/ int32_t body_palette_;
  /*+0xB18*/ int32_t head_palette_;
  /*+0xB1C*/ int32_t accessory_;
  /*+0xB20*/ int32_t accessory2_;
  /*+0xB24*/ int32_t accessory3_;
  /*+0xB28*/ int32_t body_state_;
  /*+0xB2C*/ int32_t health_state_;
  /*+0xB30*/ int32_t effect_state_;
  /*+0xB34*/ int32_t pos_x_;
  /*+0xB38*/ int32_t pos_y_;
  /*+0xB3C*/ uint8_t padding2[0xAA8];
  /*+0x15E4*/ uint32_t aid_;
  /*+0x15E8*/ uint8_t padding3a[0x7C];
  /*+0x1664*/ int32_t str_base_;
  /*+0x1668*/ int32_t agi_base_;
  /*+0x166C*/ int32_t vit_base_;
  /*+0x1670*/ int32_t int_base_;
  /*+0x1674*/ int32_t dex_base_;
  /*+0x1678*/ int32_t luk_base_;
  /*+0x167C*/ uint8_t padding3b[0x5C];
  /*+0x16D8*/ std::list<ItemInfo> item_list_;
  /*+0x16E0*/ uint8_t padding4[0x3B18];
  /*+0x51F8*/ TalkTypeTable talk_type_table_;
  /*+0x5204*/ uint8_t padding5[0x344];
  /*+0x5548*/ int32_t hp_;
  /*+0x554C*/ int32_t max_hp_;
  /*+0x5550*/ int32_t sp_;
  /*+0x5554*/ int32_t max_sp_;
  /*+0x5558*/ uint8_t padding6[0x2C50];
  /*+0x81A8*/ char char_name_[0x40];
});
