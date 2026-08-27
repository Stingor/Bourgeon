#pragma once

#include <cstdint>
#include <list>
#include <utility>
#include <vector>

#include "ragnarok/item_info.h"
#include "ragnarok/object_layouts/session/macro_utils.h"

// CSession sur le client 2026-07-07. `g_session` = 0x014B73B0.
//
// Relevé du 2026-08-27 — méthode et preuves dans docs/2026/session_layout_2026.md.
//
// 🔴 CE FICHIER NE DÉCLARE QUE CE QUI EST MESURÉ *ET* RÉELLEMENT LU.
//
// La macro SESSION_IMPLEMENTATION n'expose que sept accesseurs — aid_, hp_,
// max_hp_, sp_, max_sp_, char_name_ et item_list_. Tous les autres champs du
// layout 20250716 (mkcount_, head_, body_palette_, pos_x_, talk_type_table_…)
// ne sont lus par personne : les recopier ici reviendrait à inventer des
// offsets que rien ne vérifierait, et un offset faux dans une structure à
// padding se lit comme une valeur plausible. C'est ainsi qu'item_list_ (+0x16D8)
// a survécu des mois dans le layout 2025 alors qu'il était faux. Ils sont donc
// remplacés par du remplissage.
//
// Comment les offsets ont été obtenus : `g_session` est un global à adresse
// FIXE, donc chacun de ses champs apparaît en clair dans le code comme adresse
// absolue, et le portage 2025→2026 en a déjà apparié des milliers. On lit donc
// l'offset au lieu de le déduire.
//
// Ce qui rend ces valeurs sûres n'est pas la méthode mais la REDONDANCE :
//   - aid_ et les six statistiques tombent dans un palier de -56 appuyé sur
//     62 mesures, et les six stats sont contiguës par pas de 4 des deux côtés ;
//   - hp_/max_hp_/sp_/max_sp_ dans un palier de -1108 appuyé sur 17 mesures,
//     eux aussi contigus par pas de 4 ;
//   - char_name_ dans un palier de -2432 appuyé sur 4 mesures.
//
// Recoupement extérieur : aid_ tombe sur 0x14B895C, c'est-à-dire le global d'où
// le constructeur du CZ_ENTER tire l'account id (`mov eax, ArgList`, écrit en +2
// du paquet). Ce chemin-là a été établi indépendamment, en analysant le
// protocole — deux mesures sans rapport qui concordent.
//
// ⚠ item_list_ est le SEUL champ dont l'emplacement n'est pas mesuré : il est
// placé par report du palier -56, et c'est une extrapolation assumée. Elle est
// sans conséquence — le champ était DÉJÀ faux en 2025 (tête de liste lue à 0,
// crash au premier parcours) et son unique consommateur, GetItemInfoById, n'a
// aucun appelant. La vraie tête de l'inventaire est le global 0x015FBAB0 côté
// 2025, cf. features/windows/make_item_window.cc. La macro exige le champ, donc
// il est déclaré ; il ne doit pas être lu.
SESSION_IMPLEMENTATION(20260707, {
  /*+0x0000*/ int32_t cur_map_type_;
  /*+0x0004*/ uint8_t padding0[0x15A8];
  /*+0x15AC*/ uint32_t aid_;              // 2025 : +0x15E4  (palier -56)
  /*+0x15B0*/ uint8_t padding1[0x7C];
  /*+0x162C*/ int32_t str_base_;          // 2025 : +0x1664  (palier -56)
  /*+0x1630*/ int32_t agi_base_;
  /*+0x1634*/ int32_t vit_base_;
  /*+0x1638*/ int32_t int_base_;
  /*+0x163C*/ int32_t dex_base_;
  /*+0x1640*/ int32_t luk_base_;
  /*+0x1644*/ uint8_t padding2[0x5C];
  /*+0x16A0*/ std::list<ItemInfo> item_list_;  // NON MESURÉ — champ mort, cf. plus haut
  /*+0x16A8*/ uint8_t padding3[0x3A4C];
  /*+0x50F4*/ int32_t hp_;                // 2025 : +0x5548  (palier -1108)
  /*+0x50F8*/ int32_t max_hp_;
  /*+0x50FC*/ int32_t sp_;
  /*+0x5100*/ int32_t max_sp_;
  /*+0x5104*/ uint8_t padding4[0x2724];
  /*+0x7828*/ char char_name_[0x40];      // 2025 : +0x81A8  (palier -2432)
});
