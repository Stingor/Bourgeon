#pragma once

#include <cstdint>

#include "features/plugin.h"

// ── CartViewer ───────────────────────────────────────────────────────────────
//
// Re-implémentation ImGui de la fenêtre CHARIOT native (UICartWnd, window id 40 =
// 0x28, vtable 0x0103d538, cherchée par ID au gestionnaire), calquée sur
// InventoryViewer — la fenêtre cart EST une sœur de l'inventaire dans le
// client : même framework de fenêtre, mêmes offsets de rect, même modèle d'item.
//
// Elle REMPLACE la native (masquée par le flag de visibilité wnd+0x28) quand
// imgui_enabled_, qui n'est PAS un réglage isolé : le cart fait partie du lot
// « Interface moderne » (SetModernInterface) avec l'inventaire, le storage, les
// barres d'action, l'échange et le courrier. Un cart moderne qui devrait
// échanger des items par glisser avec un inventaire natif n'aurait aucun sens.
//
// Lecture du modèle : liste SESSION du cart (g_session+0x1720 = 0x015fbae0,
// compteur +0x1724), nœud next@+0 / ItemSkillInfo@+8 — EXACTEMENT le layout de
// l'inventaire (RE Cart_CopyItemAt 0x00d5c000 / Cart_GetCount 0x00d5ce50). Donc
// le viewer marche fenêtre native cachée.
//
// ORDRE D'AFFICHAGE : celui de la liste, sans tri — c'est ce que fait la native
// (UICartWnd_OnMsg case 23 parcourt 0..count-1 sans trier), donc le tri serveur
// (@tri_cart / réglage « Tri Cart ») est reflété dès que le serveur renvoie la
// liste. Même leçon que l'inventaire, cf. project_inventory_viewer_wip.
//
// Transferts (dispatcher CMode, RE UICartWnd_OnRButtonDown 0x0094faa0 et
// UICartWnd_OnMsg case 38) : cart -> inventaire 0x4d, cart -> storage 0x4f,
// inventaire -> cart 0x4c, storage -> cart 0x4e.

class CartViewer : public Plugin {
 public:
  const char* name() const override { return "CartViewer"; }

  void OnTick() override;      // état du cart (polling read-only) + masquage natif
  void OnRenderUI() override;  // grille ImGui si le cart est ouvert

  // Contenu de sa section dans le panneau Moonlight. True si un réglage a changé.
  bool DrawSettings();

  // Setting PERSISTANT (bourgeon_settings.yaml "cart_imgui"), basculé en GROUPE par
  // SetModernInterface. Public pour le chargement/sauvegarde par MoonlightUi.
  bool imgui_enabled_ = false;  // OPT-IN : cart natif par défaut

  // ── Settings PERSISTANTS (section « Cart » du panneau Moonlight) ─────────
  bool& show_filter()   { return show_filter_; }
  bool& desc_tooltip()  { return show_desc_tooltip_; }
  bool& tabs_vertical() { return tabs_vertical_; }
  bool& lock_size()     { return lock_size_; }

  // Appelé par le hook MakeWindow de WindowPosTweaks à la création de la fenêtre
  // id 0x28 : masque la native AVANT son premier rendu (pas de flicker).
  void HideNativeAtCreation(void* win);

  // True si (mx,my) est au-dessus du viewer cart ouvert. Sert aux AUTRES viewers
  // (inventaire, storage) pour router un dépôt par glisser : quand le cart est
  // moderne, sa fenêtre native est cachée, donc leur test de rect natif échoue.
  bool PointOverViewer(int mx, int my) const {
    return open_ && imgui_enabled_ && win_valid_ && mx >= win_x_ && my >= win_y_ &&
           mx < win_x_ + win_w_ && my < win_y_ + win_h_;
  }

 private:
  // Un item du cart, extrait en POD (sous SEH) pour un rendu hors __try.
  struct Item {
    uint32_t id = 0;          // atoi(std::string @info+0x2c)
    int      amount = 0;      // node+0x18 (= info+0x10)
    int      index = 0;       // info+4 : index cart (arg des commandes de transfert)
    int      refine = 0;      // info+0x60
    int      type = 0;        // info+0 : type d'item (onglets)
    uint8_t  identified = 0;  // info+0x5c (résolution d'icône)
    char     name[64] = {0};
    // Données d'INSTANCE du stack, pour l'aperçu de description au survol.
    uint32_t cards[4] = {0};  // info+0x1c
    int      opt_count = 0;   // info+0x98
    struct Opt { int16_t index; int16_t value; uint8_t param; };
    Opt      opts[5] = {};    // info+0x9c
  };
  static constexpr int kMaxItems = 200;  // marge au-dessus de MAX_CART (100)

  // Remplit items_/item_count_ depuis le modèle session. SEH (POD only).
  void Extract();

  bool show_panel_ = true;          // transitoire : clic sur le X
  bool show_filter_ = true;         // setting : champ de filtre par nom
  bool show_desc_tooltip_ = false;  // setting : aperçu de description au survol
  bool tabs_vertical_ = true;       // setting : onglets verticaux (défaut) ou horizontaux
  bool lock_size_ = false;          // setting : taille de fenêtre verrouillée
  int  cur_tab_ = 0;                // onglet catégorie sélectionné

  // Rect écran du viewer (capturé au rendu), pour PointOverViewer.
  float win_x_ = 0, win_y_ = 0, win_w_ = 0, win_h_ = 0;
  bool  win_valid_ = false;

  // Action en attente (posée par un drag/menu, traitée au rendu, + prompt qté).
  enum PendAction { kPendToBody, kPendToStorage };
  int  pend_id_ = 0;      // 0 = aucune action en attente
  int  pend_index_ = 0;   // index cart de l'item concerné
  int  pend_max_ = 0;     // quantité max (stack) pour le prompt
  bool pend_open_prompt_ = false;
  int  pend_action_ = kPendToBody;

  // Drag d'un item du viewer (-> inventaire / storage selon la cible).
  bool  drag_active_ = false;
  int   drag_index_ = 0, drag_amount_ = 0;
  float drag_mx_ = 0, drag_my_ = 0;

  bool open_ = false;      // cart ouvert ce frame ?
  bool was_open_ = false;  // front montant (placement à la 1re ouverture)
  bool need_pos_ = false;  // repositionner sur la native à l'ouverture
  int  spawn_x_ = 0, spawn_y_ = 0;
  int  item_count_ = 0;
  // Case survolée ce frame pour l'aperçu de description (0 = aucune).
  uint32_t hover_desc_id_ = 0;
  int      hover_desc_idx_ = -1;

  Item items_[kMaxItems];
};
