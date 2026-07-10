#pragma once

#include <cstdint>
#include <unordered_map>

#include "plugins/plugin.h"

// ── StorageTweaks ───────────────────────────────────────────────────────────
//
// Viewer ImGui de l'entrepôt (Kafra / guilde / premium), en COEXISTENCE avec la
// fenêtre native : les deux affichent le même modèle, donc la synchro est
// automatique (l'ImGui relit la liste chaque tick ; déposer/retirer dans le
// natif met à jour le modèle -> reflété au frame suivant).
//
// RE (cf. mémoire project_storage_window_re) :
//   - Fenêtre native = UIItemStoreWnd, id 0x21, vtable 0x0103ca40.
//   - Slot manager (mgr+0x288 = 0x0131f770) : non-nul <=> fenêtre ouverte,
//     remis à 0 à la fermeture (signal FIABLE, = ce que FindWindow(0x21) rend).
//   - Liste AFFICHÉE = std::list à wnd+0xe8 (_Myhead sentinelle @+0xe8, taille
//     @+0xec). Nœud : value=node+8, id=*(node+0xc), quantité=*(node+0x18).
//   - Compteur used/max = wnd+0x188 / wnd+0x18c.
//
// v1 = LECTURE SEULE (aucun hook, aucun paquet) : on lit la liste de la fenêtre
// et on la rend en table ImGui recherchable/triable. Le natif reste 100 %
// fonctionnel pour déposer/retirer ; l'ImGui valide la synchro à côté.

class StorageTweaks : public Plugin {
 public:
  StorageTweaks();  // enregistre l'opcode ZC_BOURGEON_STORAGE_PRICES

  const char* name() const override { return "StorageTweaks"; }

  void OnTick() override;      // capture l'état de l'entrepôt (polling read-only)
  void OnRenderUI() override;  // dessine le viewer ImGui si l'entrepôt est ouvert
  // Reçoit les prix de vente du storage (ZC_BOURGEON_STORAGE_PRICES 0x0F0F).
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  bool& show_panel() { return show_panel_; }

  // Setting PERSISTANT (bourgeon_settings.yaml "storage_imgui", géré par MoonlightUi) :
  // ON = viewer ImGui + fenêtre native cachée ; OFF = entrepôt natif seul, aucun viewer.
  // Pas de cohabitation. Public pour que MoonlightUi le charge/sauve (comme sb->enabled_).
  bool imgui_enabled_ = true;

  // Appelé par le hook WndProc au WM_LBUTTONUP (PRÉ-input, comme skill_bar) : si un
  // drag NATIF d'un item d'inventaire est relâché au-dessus du viewer, capture un
  // dépôt en attente et annule le drag natif (charge vidée avant que le jeu voie le
  // up -> pas de drop au sol). Le dépôt réel part depuis OnRenderUI (SendPacket sûr,
  // + prompt quantité pour les piles). Renvoie true si le drop est consommé.
  bool HandleNativeDrop(int mx, int my);

  // Appelé par le hook WndProc au WM_LBUTTONDOWN : mémorise si le clic a démarré
  // au-dessus de la fenêtre CART. Sert à router un drag natif relâché sur le viewer :
  // clic parti du cart -> cart->storage (0x0129) ; sinon -> dépôt inventaire (0x08ac).
  // (Le payload de drag n'expose pas la source de façon fiable, cf. project mémoire.)
  void OnMouseDown(int mx, int my);

  // True si (mx,my) est au-dessus de la fenêtre du viewer storage (ImGui) ouverte. Sert
  // au viewer INVENTAIRE pour router un dépôt par glisser quand les DEUX sont des viewers
  // ImGui (le rect natif du storage est caché, donc MouseOverStorage échoue).
  bool PointOverViewer(int mx, int my) const {
    return open_ && imgui_enabled_ && win_valid_ && mx >= win_x_ && my >= win_y_ &&
           mx < win_x_ + win_w_ && my < win_y_ + win_h_;
  }

  // Appelé par le hook MakeWindow de WindowPosTweaks à la création d'une fenêtre id
  // 0x21 (entrepôt) : si imgui_enabled_, force le flag de visibilité (win+0x28) à 0
  // AVANT le 1er rendu -> pas de flicker (le OnTick seul laissait 1 frame visible).
  void HideNativeAtCreation(void* win);

 private:
  // Un item de l'entrepôt, extrait en POD (sous SEH) pour rendre hors __try.
  struct Item {
    uint32_t id = 0;          // atoi(info+0x2c) — la SOURCE que le jeu utilise
    int      amount = 0;
    int      index = 0;       // info+4 (= node+0xc) : index storage pour le retrait
    int      type = 0;        // info+0 : type d'item (pour les onglets)
    uint8_t  identified = 0;  // info+0x5c (flag pour la résolution d'icône)
    char     name[64] = {0};
  };
  static constexpr int kMaxItems = 700;  // MAX_STORAGE serveur (marge)

  // Remplit items_/item_count_ depuis la liste de la fenêtre. SEH (POD only).
  void Extract(uint8_t* wnd);

  bool  show_panel_ = true;   // transitoire : détection du clic sur le X (ferme la session)
  bool  show_id_col_ = false; // setting : afficher une colonne avec l'id d'item
  bool  show_index_col_ = false;  // setting : afficher l'index storage (slot)
  bool  show_slots_col_ = false;  // setting : afficher une colonne nb de slots carte
  int   cur_tab_ = 0;         // onglet catégorie sélectionné (0 = Tout)

  // Rect écran du viewer (capturé au rendu) pour tester le drop natif dessus.
  float win_x_ = 0, win_y_ = 0, win_w_ = 0, win_h_ = 0;
  bool  win_valid_ = false;
  // Le dernier WM_LBUTTONDOWN a-t-il démarré sur la fenêtre cart ? (routage du
  // drag natif relâché sur le viewer : cart->storage vs dépôt inventaire).
  bool  mousedown_over_cart_ = false;
  // Le dernier WM_LBUTTONDOWN a-t-il démarré SUR le viewer ? (un vrai drag natif
  // entrant démarre HORS du viewer -> sinon = simple clic, pas d'icône de drag).
  bool  mousedown_over_viewer_ = false;
  // Dépôt en attente (posé par HandleNativeDrop, traité en OnRenderUI).
  std::unordered_map<uint32_t, uint32_t> prices_;  // id -> prix de vente NPC (serveur)
  // Métadonnées item (serveur, statiques) pour les sous-catégories : subtype = type
  // d'arme (W_*) ou de munition (A_*) ; equip = masque de slot d'équipement.
  struct ItemMeta { uint8_t subtype = 0; uint32_t equip = 0; uint16_t slots = 0; };
  std::unordered_map<uint32_t, ItemMeta> meta_;    // id -> {subtype, equip}
  int cur_sub_ = -1;  // sous-catégorie sélectionnée (clé SubCat, -1 = Tout)
  // Sens d'un déplacement en attente. L'index de pend_index_ dépend du sens :
  // inventaire pour Deposit, storage pour Withdraw/StoToCart, cart pour CartToSto.
  enum PendAction { kPendDeposit, kPendWithdraw, kPendStoToCart, kPendCartToSto };
  int   pend_id_ = 0;      // 0 = aucune action en attente
  int   pend_index_ = 0;   // index (source, cf. PendAction)
  int   pend_max_ = 0;     // quantité max (stack)
  bool  pend_open_prompt_ = false;  // ouvrir le prompt quantité au prochain rendu
  int   pend_action_ = kPendDeposit;  // sens du déplacement en attente
  // Retrait par drag (viewer -> inventaire natif) : suivi du drag en cours.
  bool  drag_active_ = false;
  int   drag_index_ = 0, drag_amount_ = 0;
  float drag_mx_ = 0, drag_my_ = 0;  // dernière pos souris pendant le drag
  bool  open_ = false;        // entrepôt ouvert ce frame ?
  bool  was_open_ = false;    // pour le front montant (placement 1re ouverture)
  bool  need_pos_ = false;    // repositionner près du natif à l'ouverture
  int   spawn_x_ = 0, spawn_y_ = 0;
  int   used_ = 0, max_ = 0;  // compteur used/max (wnd+0x188/+0x18c)
  // Nom de l'entrepôt ouvert, envoyé par le serveur dans ZC_INVENTORY_START (0x0b08,
  // invType STORAGE=2) : "Storage" / "Guild Storage" / nom premium. Sert de titre.
  char  storage_name_[32] = {0};
  int   item_count_ = 0;      // nb d'items valides dans items_
  Item  items_[kMaxItems];
};
