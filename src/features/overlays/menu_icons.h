#pragma once

#include <map>
#include <string>
#include <vector>

#include "features/plugin.h"

// Recreates the Basic-Info menu icons as ImGui widgets (Approach B). The native
// grid (UIMenuIconWnd) is hidden (its DrawContent vtable slot is swapped to a
// no-op) and each FUNCTIONAL icon (shown in the native grid per the WARP
// visibility table @0x814064) is redrawn in ImGui from the game's own icon
// bitmap, clickable (routes the command to the game's UI dispatcher) and with
// the native tooltip text. Phase 2a: load + draw + click + tooltip + hide grid.
// Phase 2b will add per-icon move/snap/show-hide/persistence (edit mode).
class MenuIcons : public Plugin {
 public:

  // Contenu de sa section dans le panneau Moonlight. Rend true si un réglage
  // a changé — l'appelant décide de sauvegarder, une seule fois.
  bool DrawSettings();
  const char* name() const override { return "Menu Icons"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRenderUI() override;
  void OnTick() override;
  void FlushPending();  // dispatch the queued click (input phase + OnTick fallback)

  // Replace native menu icons with ImGui ones (driven from MoonlightUi).
  bool enabled_   = false;
  bool edit_mode_ = false;  // drag icons to reposition (driven from MoonlightUi)
  // Set after a drag-end or show/hide change; MoonlightUi drains it -> saves YAML.
  bool geometry_dirty_ = false;

  // Un bitmap d'icône et sa taille MESURÉE. Les quatre états d'une icône
  // (normal / enfoncé / signalé / signalé-enfoncé) sont QUATRE fichiers .bmp
  // distincts chez le natif, pas un atlas ni une teinte : ils ont donc chacun
  // leur taille propre — le bitmap « _new » déborde vers le haut — et chacun son
  // compteur d'échecs, parce que la plupart des icônes n'ont ni « _new » ni,
  // pour les boutons hors grille, de « _press ».
  struct Bitmap {
    void* tex = nullptr;  // IDirect3DTexture9* (chargé à la demande)
    int   w = 0, h = 0;
    int   fail = 0;       // échecs de chargement : 3 et on renonce
  };

  struct Icon {
    const char* name;   // bitmap base name -> \<dir><name>.bmp
    // Dossier + préfixe du bitmap sous 유저인터페이스\. Les 25 icônes de la
    // grille sont toutes en « menu_icon\bt_ », mais le bouton du cash shop —
    // qui rejoint la liste pour hériter du mode édition, de l'aimantage et de
    // la persistance — est une fenêtre native à lui et son art vit ailleurs.
    const char* dir = nullptr;
    // Fenêtre native à qui adresser le clic : la grille (0x133) pour les 25
    // icônes, sa propre fenêtre (190) pour le bouton du cash shop. C'est ce
    // champ, et non `cmd_id`, qui sépare les deux familles partout ailleurs
    // (signalement « nouveau », liste de réglages, position par défaut) : les
    // deux commandes 0xC0 du client, « status » et « cash shop », se
    // confondraient sinon.
    int         wnd_id = 0;
    int         cmd_id; // UI command id (routed on click)
    int         msg_id; // tooltip message id
    // Action du catalogue de Bourgeon (`hotkey_actions`) que ce bouton déclenche,
    // pour les icônes qui n'ont AUCUNE contrepartie dans le client — l'Atlas des
    // recettes, par exemple. nullptr = icône du jeu, le clic part au natif.
    // C'est l'identifiant du catalogue et pas un pointeur de fenêtre : le bouton
    // hérite ainsi du même point d'entrée que le raccourci clavier, et son
    // infobulle sait retrouver la touche qui lui est liée.
    const char* action_id = nullptr;
    // Libellé (clé i18n française) des icônes de Bourgeon, qui n'ont pas de
    // ligne dans la table de messages du client. Ignoré quand `msg_id` != 0.
    const char* label_fr = nullptr;
    // Index de commande de la catégorie Interface (`userhotkey::kInterface`)
    // dont la touche ouvre CETTE fenêtre, -1 si le client n'en propose pas.
    // C'est ce qui met le raccourci dans l'infobulle, sans le recopier : le nom
    // de touche est relu chez le jeu à chaque survol, donc layout-aware et à
    // jour du remappage.
    int         hk_cmd = -1;
    int         x = 0, y = 0;   // screen position
    bool        hidden = false; // user-hidden via the MoonlightUi list
    // Cette icône a-t-elle un bitmap d'état enfoncé dans le jeu ? Faux pour le
    // bouton du cash shop, dont l'art unique sert les trois états. Le demander
    // quand même coûterait une recherche GRF infructueuse ET une ligne d'erreur
    // « Resource File Loading fail » du natif à chaque tentative.
    bool        has_press = true;
    // Les quatre bitmaps. `normal` porte AUSSI la taille de référence de l'icône
    // (position, aimantage, clamp) : les trois autres ne sont que des états.
    Bitmap      normal;
    // État enfoncé : \menu_icon\bt_<name>_press.bmp, ce que le natif montre tant
    // que le bouton de la souris reste appuyé sur l'icône. Sans lui, un clic ne
    // renvoyait AUCUN retour visuel — c'est ce qui manquait à cette copie ImGui.
    Bitmap      pressed;
    // Signalement « nouveau » (courrier non lu, succès débloqué…) : le natif ne
    // pose pas de pastille par-dessus, il affiche une SECONDE icône complète
    // \menu_icon\bt_<name>_new.bmp à la place de la normale. Elle est plus haute
    // (le badge déborde vers le haut), d'où sa taille propre — et elle a elle
    // aussi son état enfoncé, `bt_<name>_new_press.bmp`.
    bool        badge = false;      // le natif signale cette commande
    Bitmap      badge_normal;
    Bitmap      badge_pressed;
  };

  // Persisted per-icon position/visibility, keyed by icon name. MoonlightUi loads
  // it before the icons exist; BuildIconList applies it; every edit updates it
  // (the canonical source MoonlightUi writes back to the YAML).
  struct IconSave { int x = -1, y = -1; bool hidden = false; bool valid = false; };
  std::map<std::string, IconSave> saved_;

  // Live icon list (built in-game) — MoonlightUi iterates it for the show/hide UI.
  std::vector<Icon>& icons() { return icons_; }

 private:
  bool in_game_     = false;
  bool icons_built_ = false;
  bool grid_hidden_ = false;
  bool pending_refresh_ = false;  // request a server clif_refresh (drains in OnTick)
  // Clic mis en file pendant le rendu, rejoué en phase d'input. On garde l'ID de
  // la fenêtre destinataire, JAMAIS son pointeur : le client détruit ses
  // fenêtres, et une frame plus tard le pointeur serait libéré.
  int  pending_wnd_ = 0;
  int  pending_cmd_ = 0;
  // Action de catalogue mise en file à la place de la commande native, pour les
  // boutons de Bourgeon. Pointeur vers un littéral du catalogue : rien à libérer,
  // et rien qui puisse mourir sous nous.
  const char* pending_action_ = nullptr;
  int  dragging_    = -1;  // index of the icon being dragged in edit mode, else -1
  float drag_off_x_ = 0.0f, drag_off_y_ = 0.0f;  // mouse-to-origin offset at grab
  std::vector<Icon> icons_;

  void BuildIconList();         // populate icons_ with the functional icons
  void RefreshBadges();         // relève les commandes signalées par le natif
  void HideNativeGrid(bool hide);
  // Accorde le bouton natif du cash shop et notre copie ImGui : un seul endroit
  // décide qui des deux se voit (cf. le pavé de commentaire dans le .cc).
  void SyncCashShopButton();
  // `action_id` non nul court-circuite le couple (fenêtre, commande) : le clic
  // part au catalogue de raccourcis de Bourgeon au lieu du client.
  void DispatchCommand(int wnd_id, int cmd_id, const char* action_id);
  // Magnetic snap of value v (extent ext) on one axis to other icons' edges.
  float SnapIcon(float v, float ext, int self, bool y_axis) const;
};
