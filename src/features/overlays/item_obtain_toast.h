#pragma once

#include <cstdint>

#include "features/plugin.h"

// ── Bandeau « <objet> - N obtained. » ────────────────────────────────────────
// Remplacement ImGui de `UINotifyItemObtainWnd`, la fenêtre native 58 (0x3A) qui
// s'affiche en haut de l'écran à chaque ramassage. RE complet :
// docs/item_obtain_notify_re.md.
//
// Ce que le natif fait, et qu'on garde :
//   · le libellé EXACT du client — le nom composé par son propre name-builder,
//     suivi de MsgStringTable[696] (`MSI_EA_OBTAIN` = « ` - %d obtained.` ») ;
//   · l'icône de l'objet, le cadre clair « sysbox », la position en haut au
//     centre, la durée de 5 s.
//
// Ce que le natif NE fait pas, et qu'on comble (cf. le mémo « remplacer le natif
// = combler ses manques ») :
//   🔴 il n'EMPILE PAS. `MakeWindow(58)` est idempotent : un deuxième ramassage
//      écrase le premier au lieu de s'ajouter. En farm, on voit passer une
//      fraction de ce qu'on ramasse — le défaut le plus visible du natif, et la
//      raison d'être de ce module.
//   · il ne regroupe pas non plus : ramasser cinq fois la même herbe, c'est cinq
//     bandeaux successifs dont un seul survit. Ici, une ligne « x5 » qui monte.
//   · sa largeur est calculée sur un AUTRE texte que celui qu'il dessine
//     (MSI_GET_ITEM mesuré, MSI_EA_OBTAIN affiché), d'où le blanc à droite.
//
// ── Comment on capte les ramassages ──────────────────────────────────────────
// PAS en décodant les paquets. Sept opcodes `ZC_ITEM_PICKUP_ACK` mènent au même
// bandeau (0x00A0, 0x029A, 0x02D4, 0x0990, 0x0A0C, 0x0A37, 0x0B41), chacun avec
// son propre format ; les décoder tous serait sept parseurs à maintenir pour
// refaire un travail que le client fait déjà.
//
// On détourne donc l'`OnMsg` de la fenêtre 58 (slot vtable +0x94), et on lit le
// message **34**, celui qui porte un `ItemSkillInfo*` DÉJÀ REMPLI : nameid,
// quantité, refine, cartes, identifié, cassé. Un seul point d'interception,
// valable pour les sept opcodes, et par construction toujours d'accord avec ce
// que le client aurait affiché.
//
// La native, elle, n'est pas détruite : on la rend simplement invisible
// (`+0x28 = 0`) et elle se supprime toute seule au bout de ses 5 s, depuis son
// propre tick. C'est le cas rare où masquer est PLUS sûr que détruire — la
// fenêtre n'a ni bouton, ni saisie clavier (son vt+0x24 rend 0, donc pas de
// hit-test), et la détruire depuis son propre `OnMsg` libérerait le `this` en
// cours d'appel.
//
// ⚠ Le hook s'installe sur la vtable STATIQUE, donc une fois pour toutes : il
// n'y a rien à réarmer quand une instance naît ou meurt.

// Réglages persistés (scalaires nus, pour que moonlight_ui les sérialise sans
// rien connaître des internes — même contrat que QuestTrackerConfig).
struct ItemObtainToastConfig {
  bool enabled = true;  // interrupteur maître : masque le natif + dessine le nôtre

  // Empilement. Le natif est bloqué à 1 ; au-delà, chaque ligne a sa propre
  // durée de vie et la pile se referme quand la plus vieille expire.
  int  max_lines = 5;
  bool newest_on_top = true;

  // Regrouper les ramassages du MÊME objet (même id, même refine, même état)
  // tant que sa ligne est encore à l'écran : la quantité s'additionne et le
  // minuteur repart. Sans ça, un farm de consommables remplit la pile d'une
  // seule et même herbe.
  bool merge_same = true;

  // Durée d'une ligne. 5000 = la valeur en dur du natif.
  int  duration_ms = 5000;
  //
  // ⚠ Pas de fondu de sortie, et c'est un choix, pas un oubli : le cadre sysbox
  // du toolkit (`ro::DrawDescPanelFrame`) blitte des textures sans teinte, donc
  // sans alpha modulable. Faire fondre le texte et l'icône en laissant le cadre
  // opaque serait plus laid qu'une disparition nette — qui est d'ailleurs ce que
  // fait le natif. Le jour où le toolkit prendra un alpha, il n'y aura qu'un
  // paramètre à passer ici.

  // ── Ancrage ─────────────────────────────────────────────────────────────
  // `pos_x` négatif = centré horizontalement, ce qui reproduit le natif (il
  // place son cadre à 220 sur une largeur de référence de 640, puis recentre).
  // `pos_y` = 75, la valeur native.
  //
  // Ces deux champs ne se règlent PAS au curseur : ils sont pilotés par une
  // ancre qu'on attrape à la souris quand `locked` est faux — le geste direct
  // plutôt que deux nombres qu'il faut régler à l'aveugle, comme le fait déjà
  // le suivi de quête. Glisser l'ancre sort du centrage (`pos_x` devient
  // absolu), puisqu'on ne peut pas à la fois centrer et placer.
  bool locked = true;
  int  pos_x  = -1;
  int  pos_y  = 75;

  // Compacité : l'espace ENTRE deux lignes, et la marge verticale à l'intérieur
  // d'une ligne. La seconde n'agit que sans le cadre RO — avec, la hauteur est
  // imposée par la grille de tuiles (cf. le rendu).
  int  row_gap = 2;
  int  pad_v   = 3;

  bool show_icon  = true;
  bool show_frame = true;  // cadre clair « sysbox », comme le client
  // (Pas de réglage d'opacité du cadre RO : `ro::DrawDescPanelFrame` blitte ses
  // textures sans teinte. Un champ persisté qui ne ferait rien serait pire que
  // son absence — cf. la note sur le fondu ci-dessus.)

  // ── Fond libre, quand le cadre RO est masqué ────────────────────────────
  // Sans le cadre, le texte se retrouvait à nu sur la scène et devenait
  // illisible dès qu'un décor clair passait dessous. D'où un fond à soi :
  // couleur, opacité, arrondi, et une bordure facultative. Ces champs sont
  // INERTES tant que `show_frame` est vrai — le panneau les grise en
  // conséquence, ils ne se contredisent jamais à l'écran.
  bool bg_enabled       = true;
  int  bg_rgb           = 0x101014;
  int  bg_alpha         = 70;  // %
  int  bg_rounding      = 4;   // px ; 0 = angles droits
  bool border_enabled   = true;
  int  border_rgb       = 0x000000;
  int  border_alpha     = 90;  // %
  int  border_thickness = 1;   // px

  // Le natif écrit en noir sur son cadre clair ; `qty_rgb` colore le suffixe
  // « - N obtained. » pour que la quantité se détache du nom.
  int  text_rgb = 0x000000;
  int  qty_rgb  = 0x1E5AA0;

  int  font_scale = 100;  // %
};

// Le bandeau d'obtention d'objet, version Bourgeon.
class ItemObtainToast : public Plugin {
 public:
  ItemObtainToast();

  const char* name() const override { return "ItemObtainToast"; }

  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  ItemObtainToastConfig& config();

  // Panneau de réglages (sans fenêtre propre), hébergé par moonlight_ui.
  void DrawSettings();
};
