#pragma once

// ── Le socle commun des trois VIEWERS D'OBJETS ───────────────────────────────
//
// L'inventaire, le chariot et l'entrepôt sont trois vues d'une même chose : une
// liste d'`ItemSkillInfo` lue dans la session, rendue en ImGui à la place d'une
// fenêtre native qui ne naît plus, avec un glisser qui sort vers les deux
// autres. Le client lui-même les traite en sœurs — même framework de fenêtre,
// mêmes offsets de rect, même modèle d'item.
//
// Écrites l'une après l'autre en se copiant, elles avaient fini par porter
// VINGT ET UN MEMBRES IDENTIQUES chacune : sur les vingt-deux de `CartViewer`,
// vingt et un étaient ici. Ce n'était plus une ressemblance, c'était une classe
// de base qui n'avait jamais été écrite. Les défauts ont été comparés un à un
// avant la fusion — voir les deux ⚠ plus bas, seuls écarts trouvés.
//
// ── Ce qui reste chez chacune, et pourquoi ───────────────────────────────────
//   · `Item`, `kMaxItems`, `items_`, `Extract()` — le POD extrait diffère par
//     fenêtre (le chariot ignore `drag_type_`, l'entrepôt porte prix et méta).
//   · `enum PendAction` — les trois vocabulaires d'actions n'ont RIEN en commun
//     (utiliser/équiper/jeter d'un côté, deux sens de transfert de l'autre).
//     Seul le `pend_action_` qui les transporte est ici, en `int`, comme il
//     l'était déjà dans les trois.
//   · Tout le particulier : sertissage et placement libre pour l'inventaire,
//     colonnes/prix/onglets de coffre pour l'entrepôt.
//
// ⚠ `tabs_vertical_` vaut `true` ici parce que deux des trois le veulent ;
// `StorageWindow` le remet à `false` dans son constructeur, avec la raison.
// C'est le SEUL défaut qui divergeait vraiment.
//
// ⚠ `pend_action_ = 0` convient aux trois parce que leur premier énumérateur
// vaut 0 dans les trois (`kPendUse`, `kPendToBody`, `kPendStoToInv`). Ce n'est
// pas une coïncidence sur laquelle se reposer en silence : chaque classe pose un
// `static_assert` sous son enum pour que la fusion casse à la compilation si
// quelqu'un réordonne.

#include <cstdint>

#include "features/plugin.h"
#include "ui/desc_pending_lock.h"  // ro::DescPendingLock : l'anti-flicker de l'aperçu
#include "ui/viewer_rect.h"        // ro::ViewerRect : le rect écran, capturé au rendu

class ItemViewerBase : public Plugin {
 public:
  // Setting PERSISTANT, jamais touché seul : les trois clés
  // (`inventory_imgui`, `cart_imgui`, `storage_imgui`) sont basculées en GROUPE
  // par SetModernInterface, avec les barres d'action, l'échange et le courrier.
  // Un viewer moderne qui devrait échanger des items par glisser avec une
  // fenêtre native n'aurait aucun sens. Public pour que MoonlightUi le
  // charge/sauve. OPT-IN : le natif reste le défaut.
  bool imgui_enabled_ = false;

  // La session est-elle ouverte ? À interroger par les AUTRES modules au lieu de
  // chercher la fenêtre native : elle ne naît plus en mode ImGui, et un
  // `FindWindow` nul y passerait pour « fermé ».
  bool IsOpen() const { return open_; }

  // Le point est-il au-dessus de CE viewer ? Sert aux deux autres à router un
  // dépôt par glisser. Les trois tests sont nécessaires — le pourquoi du
  // `open_` malgré `valid()` est expliqué dans ui/viewer_rect.h.
  bool PointOverViewer(int mx, int my) const {
    return open_ && imgui_enabled_ &&
           win_rect_.Contains(static_cast<float>(mx), static_cast<float>(my));
  }

  // ── Settings PERSISTANTS communs (section du panneau Moonlight) ────────────
  bool& show_panel()     { return show_panel_; }
  bool& show_filter()    { return show_filter_; }
  bool& desc_tooltip()   { return show_desc_tooltip_; }
  bool& tabs_vertical()  { return tabs_vertical_; }
  int&  cur_tab()        { return cur_tab_; }

 protected:
  // ── Cycle de vie ──────────────────────────────────────────────────────────
  bool open_ = false;   // session ouverte ce frame ?
  // Valeur d'`imgui_enabled_` au tick précédent : détecte la BASCULE de mode,
  // qui doit ADOPTER une fenêtre déjà ouverte au lieu de la faire disparaître.
  bool prev_imgui_enabled_ = false;
  bool need_pos_ = false;    // poser la position par défaut à la 1re ouverture
  bool show_panel_ = true;   // transitoire : détecte le clic sur le X (ferme la session)

  // Rect écran, capturé au rendu pour que les AUTRES viewers puissent le tester
  // hors de leur propre rendu. Cf. ui/viewer_rect.h.
  ro::ViewerRect win_rect_;

  int item_count_ = 0;   // nb d'items valides dans le `items_` de la dérivée

  // ── Vue ───────────────────────────────────────────────────────────────────
  int  cur_tab_ = 0;                // onglet catégorie sélectionné (0 = Tout)
  bool show_filter_ = true;         // setting : champ de filtre par nom
  // Description au SURVOL : ouvre la VRAIE fenêtre de description (celle du clic
  // droit) tant que la souris reste sur la case, et la ferme en sortant. OFF =
  // simple tooltip texte (nom + quantité).
  bool show_desc_tooltip_ = false;
  bool tabs_vertical_ = true;       // setting : onglets verticaux (cf. le ⚠ en tête)

  // ── Survol et description ─────────────────────────────────────────────────
  // Case survolée ce frame (0 = aucune) : alimente l'aperçu, dessiné APRÈS la
  // fenêtre, en tooltip. L'INDEX sert à retrouver cartes et options (données
  // d'instance) ; il n'est valable que dans la frame courante, `items_` étant
  // reconstruit à chaque tick.
  uint32_t hover_desc_id_ = 0;
  int      hover_desc_idx_ = -1;
  // Verrou anti-flicker de l'aperçu, armé à la DEMANDE de description.
  ro::DescPendingLock desc_lock_;

  // ── Glisser d'un item hors de cette fenêtre ───────────────────────────────
  bool  drag_active_ = false;
  int   drag_index_ = 0, drag_amount_ = 0;
  float drag_mx_ = 0, drag_my_ = 0;  // dernière position souris pendant le glisser

  // ── Action en attente (posée par un glisser ou un menu, traitée au rendu) ──
  int  pend_id_ = 0;      // 0 = aucune action en attente
  int  pend_index_ = 0;   // index (inventaire / cart / storage) de la source
  int  pend_max_ = 0;     // quantité max (stack) pour le prompt
  bool pend_open_prompt_ = false;  // ouvrir le prompt quantité au prochain rendu
  int  pend_action_ = 0;  // valeur du `PendAction` de la dérivée (cf. le ⚠ en tête)
};
