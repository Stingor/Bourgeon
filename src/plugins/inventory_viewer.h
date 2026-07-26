#pragma once

#include <cstdint>
#include <unordered_map>

#include "plugins/plugin.h"

// ── InventoryViewer ──────────────────────────────────────────────────────────
//
// Re-implémentation ImGui COMPLÈTE de la fenêtre inventaire native (UIItemWnd,
// window id 8), calquée sur StorageTweaks. Elle REMPLACE le natif (caché via le
// flag de visibilité wnd+0x28) quand imgui_enabled_ ; sinon on laisse la fenêtre
// native intacte (avec les patches de inventory_tweaks.cc : poids, onglet Cards,
// resize). Pas de cohabitation — un master switch, comme le storage.
//
// Lecture du modèle : on lit le MODÈLE SESSION de l'inventaire (g_session
// 0x015fa3c0 : liste @0x015fbab0, compteur @0x015fbab4 ; nœud value @+8), donc le
// viewer fonctionne même fenêtre native cachée (RE confirmée 2026-07-08, via
// FUN_00d5ce30 count / FUN_00d5acb0 fetch). Mêmes offsets d'ItemSkillInfo que le
// storage (id = atoi(std::string @info+0x2c), type info+0, index info+4,
// amount node+0x18, identified info+0x5c).
//
// Layout : GRILLE d'icônes (proche du natif), onglets Use/Eqp/Etc/Card/Fav par
// type d'item, recherche, poids/zeny/compteur en bas, clic-droit = description.
// Interactif : utiliser (conso) / équiper / jeter / (dés)favori / trier +
// drag-out + transfert ALT vers chariot/entrepôt. (Les opcodes/commandes exacts
// sont finalisés par la RE en cours ; cf. project_inventory_viewer_wip.)
//
// Réutilise de storage_tweaks.cc : cache d'icônes, BuildDisplayName, OpenItemDesc
// (MakeWindow 0xc + OnMsg 0x18), skin ro::, lecture du payload de drag natif.

class InventoryViewer : public Plugin {
 public:

  // Contenu de sa section dans le panneau Moonlight. Rend true si un réglage
  // a changé — l'appelant décide de sauvegarder, une seule fois.
  bool DrawSettings();
  InventoryViewer();

  const char* name() const override { return "InventoryViewer"; }

  void OnTick() override;      // capture l'état de l'inventaire (polling read-only)
  void OnRenderUI() override;  // dessine la grille ImGui si l'inventaire est ouvert
  // Reçoit ZC_BOURGEON_COMPAT_CARDS (sertissage rapide) : liste des index de cartes
  // sertissables sur l'équipement demandé.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Setting PERSISTANT (bourgeon_settings.yaml "inventory_imgui", géré par
  // MoonlightUi comme storage_imgui) : ON = viewer ImGui + natif caché ; OFF =
  // inventaire natif seul, aucun viewer. Public pour le chargement/sauvegarde.
  bool imgui_enabled_ = false;  // OPT-IN : inventaire natif par defaut

  bool& show_panel() { return show_panel_; }

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, section « Inventaire » du
  // panneau Moonlight ; chargés/sauvés par MoonlightUi comme imgui_enabled_).
  bool& show_filter()   { return show_filter_; }
  // Aperçu de description RO au survol d'une case (comme le storage) ; OFF = simple
  // tooltip texte (nom + quantité).
  bool& desc_tooltip()  { return show_desc_tooltip_; }
  // Disposition des onglets de catégorie : true = verticale à gauche (défaut,
  // comme le natif), false = rangée horizontale au-dessus de la grille.
  bool& tabs_vertical() { return tabs_vertical_; }
  // Taille de la fenêtre verrouillée (plus de redimensionnement).
  bool& lock_size()     { return lock_size_; }
  // Placement LIBRE des items sur les cases (au lieu du remplissage automatique).
  // ⚠ TRIBUTAIRE de lock_size_ : une case est un index absolu (ligne × colonnes +
  // colonne), donc redimensionner la fenêtre change le nombre de colonnes et
  // mélangerait toutes les positions. Ignoré tant que la taille n'est pas verrouillée.
  bool& free_layout()   { return free_layout_; }
  // Placement choisi par le joueur : nameid -> index de case. 100 % CLIENT (aucun
  // paquet ; le serveur ne connaît pas de position d'item). Public pour que
  // MoonlightUi le persiste, comme les favoris du storage.
  std::unordered_map<uint32_t, int> layout_;

  // Appelé par le hook MakeWindow de WindowPosTweaks à la création de la fenêtre
  // id 8 : si imgui_enabled_, force wnd+0x28 = 0 AVANT le 1er rendu -> pas de
  // flicker (le OnTick seul laisserait passer une frame native visible).
  void HideNativeAtCreation(void* win);

  // ── Sertissage de cartes (popup natif UIItemCompositionWnd, id 0x4A) ────────
  // Double-cliquer une carte envoie CZ_REQ_ITEMCOMPOSITION_LIST (0x017A) ; le
  // SERVEUR répond (0x017B) avec la liste des équipements compatibles à slot
  // libre, et le client ouvre un popup pour choisir la cible.
  //
  // Notre version ImGui ne rejoue PAS ce protocole : elle LIT le popup natif
  // (slot 0x0131f6f0, vtable 0x01034684) qui contient déjà tout l'état — liste
  // des candidats en +0xd0, carte source en +0xf8. Aucun état dupliqué, donc
  // aucun risque de désynchro, et le filtrage reste 100 % serveur (le client
  // n'évalue AUCUNE règle de compatibilité). Cf. docs/card_insert_re.md.
  //
  // PAS de réglage séparé : le sertissage suit `imgui_enabled_`. Les deux fenêtres
  // forment un tout — proposer un inventaire ImGui qui ouvre un popup natif (ou
  // l'inverse) serait incohérent à l'usage.
  //
  // Pendant du HideNativeAtCreation ci-dessus, pour la fenêtre id 0x4A : évite la
  // frame de flicker entre la création du popup natif et le premier OnTick.
  void HideCardInsertAtCreation(void* win);

  // Hooks WndProc (pré-input, comme storage/skill_bar) : router un drag NATIF
  // relâché sur le viewer, et mémoriser où le clic a démarré.
  bool HandleNativeDrop(int mx, int my);
  void OnMouseDown(int mx, int my);

  // True si (mx,my) est au-dessus de la fenêtre du viewer inventaire (ImGui) ouverte.
  // Sert au viewer STORAGE pour router un retrait par glisser quand les deux sont des
  // viewers ImGui (le rect natif de l'inventaire est caché -> MouseOverInventory échoue).
  bool PointOverViewer(int mx, int my) const {
    return open_ && imgui_enabled_ && win_valid_ && mx >= win_x_ && my >= win_y_ &&
           mx < win_x_ + win_w_ && my < win_y_ + win_h_;
  }

  // Équipe l'item d'inventaire ACTUELLEMENT GLISSÉ (index/type/loc SERVEUR stables du
  // drag en cours) — le serveur place/swappe automatiquement. No-op si aucun drag actif
  // ou item non équipable. Renvoie true si équipé. Utilisé par le drag-drop cross-plugin
  // de character_sheet (lâcher un item d'inventaire sur le doll) ; robuste à une
  // renumérotation d'items_ en cours de glisser (l'index utilisé est stable).
  bool EquipDraggedItem(bool leftHand);

  // Ajoute l'item ACTUELLEMENT GLISSÉ à l'échange en cours (cible de drop « Mon offre »
  // de trade_tweaks). Une PILE ouvre le prompt de quantité, un item seul part direct.
  // No-op si aucun glisser en cours ou si aucun échange n'est ouvert.
  bool TradeDraggedItem();

  // Nameid de l'item ACTUELLEMENT GLISSÉ (0 si aucun drag en cours). Utilisé par
  // skill_bar_tweaks pour assigner l'item glissé à une case de la barre d'action
  // (drag-drop cross-plugin, comme EquipDraggedItem pour le doll de character_sheet).
  // Le nameid est la donnée stockée par un slot d'item de la barre (WriteSlotRecord).
  uint32_t DraggedItemNameId() const { return drag_active_ ? drag_id_ : 0; }

  // Insère le lien de l'item d'index inventaire `invIndex` dans l'input chat focalisé
  // (comme Maj+clic gauche dans l'inventaire). No-op si aucun input chat n'a le focus.
  // Marche même inventaire fermé (lit le modèle session). Utilisé par character_sheet
  // (Maj+clic droit sur un slot équipé).
  void LinkItemToChat(int invIndex);

 private:
  // Un item d'inventaire, extrait en POD (sous SEH) pour un rendu hors __try.
  struct Item {
    uint32_t id = 0;          // atoi(std::string @info+0x2c) — la SOURCE du jeu
    int      amount = 0;      // node+0x18
    int      index = 0;       // info+4 : index inventaire (arg use/equip/drop)
    uint32_t loc = 0;         // info+8 : masque d'emplacement d'équip (arg2 équip/munition)
    int      refine = 0;      // info+0x60 : niveau de refine (affiché "+N" à la place de la qté)
    int      type = 0;        // info+0 : type d'item (onglets)
    uint8_t  identified = 0;  // info+0x5c (résolution d'icône)
    uint8_t  favorite = 0;    // node+0x90 (onglet favoris)
    char     name[64] = {0};
    // Données d'INSTANCE du stack (pas de la DB) : lues pour l'aperçu de description
    // au survol, mêmes offsets que la fenêtre de description native (cf. storage).
    uint32_t cards[4] = {0};  // info+0x1c : 4 slots carte/enchant (0 = vide)
    int      opt_count = 0;   // info+0x98 : nb de random options
    struct Opt { int16_t index; int16_t value; uint8_t param; };
    Opt      opts[5] = {};    // info+0x9c : entrées de 5 octets
  };
  static constexpr int kMaxItems = 500;  // marge au-dessus de la capacité serveur

  // Remplit items_/item_count_ depuis le modèle session. SEH (POD only).
  void Extract();

  // Dessine la fenêtre de sertissage si le popup natif 0x4A est ouvert. Appelée
  // AVANT le early-return de OnRenderUI : le popup vit sa propre vie et peut
  // rester ouvert alors que l'inventaire est refermé.
  void RenderCardInsert();

  // Candidat sélectionné dans la fenêtre de sertissage : index INVENTAIRE (pas un
  // rang de liste), donc stable si le serveur renvoie une liste différente.
  // -1 = aucune sélection. Remis à -1 dès que le popup natif disparaît.
  int ci_sel_ = -1;
  // Front montant du popup : sert à le placer ET à le mettre au premier plan la
  // frame où il apparaît (sinon il s'ouvre derrière l'inventaire, qui a le focus
  // puisque c'est de là que part le double-clic sur la carte).
  bool ci_was_open_ = false;

  // ── Sertissage rapide (sous-menu du menu contextuel d'un équipement) ────────
  // Le serveur (ZC_BOURGEON_COMPAT_CARDS) calcule les cartes de l'inventaire
  // compatibles avec un équipement via le prédicat EXACT du sertissage
  // (pc_can_insert_card) -> aucun faux positif. Requête ASYNC : on la lance à
  // l'ouverture du sous-menu, le résultat s'affiche à la frame où il arrive.
  int  qs_equip_index_ = -1;   // équipement demandé (index inventaire CLIENT) ; -1 = aucun
  static constexpr int kQsMaxCards = 128;
  int  qs_cards_[kQsMaxCards] = {0};  // index inventaire des cartes compatibles reçues
  int  qs_card_count_ = 0;
  // Envoie CZ_BOURGEON_REQ_COMPAT_CARDS pour cet équipement si ce n'est pas déjà
  // celui en cours (évite de re-demander à chaque frame d'ouverture du sous-menu).
  void RequestCompatCards(int equipInvIndex);

  bool show_panel_ = true;    // transitoire : clic sur le X (ferme la session)
  bool show_filter_ = true;      // setting : champ de filtre par nom
  bool show_desc_tooltip_ = false;  // setting : aperçu de description au survol
  bool tabs_vertical_ = true;    // setting : onglets verticaux (défaut) ou horizontaux
  bool lock_size_ = false;       // setting : taille de fenêtre verrouillée
  bool free_layout_ = false;     // setting : placement libre (exige lock_size_)
  int  cur_tab_ = 0;          // onglet catégorie sélectionné
  bool sort_enabled_ = true;  // bouton Tri (footer, onglet Favoris) : trie la vue

  // Rect écran du viewer (capturé au rendu) pour tester un drop natif dessus.
  float win_x_ = 0, win_y_ = 0, win_w_ = 0, win_h_ = 0;
  bool  win_valid_ = false;
  bool  mousedown_over_viewer_ = false;
  bool  mousedown_over_equip_ = false;  // le drag natif a démarré sur la fenêtre Équipement
  bool  mousedown_over_cart_ = false;   // le drag natif a démarré sur la fenêtre Chariot

  // Action en attente (posée par un drag/clic, traitée au rendu, + prompt qté).
  enum PendAction { kPendUse, kPendEquip, kPendDrop, kPendToCart, kPendToStorage,
                    kPendToTrade };
  int  pend_id_ = 0;      // 0 = aucune action en attente
  int  pend_index_ = 0;   // index inventaire de l'item concerné
  int  pend_max_ = 0;     // quantité max (stack) pour le prompt
  bool pend_open_prompt_ = false;
  int  pend_action_ = kPendUse;

  // Drag d'un item du viewer (-> équip/sol/chariot selon la cible).
  bool  drag_active_ = false;
  int   drag_index_ = 0, drag_amount_ = 0, drag_type_ = 0;
  uint32_t drag_id_ = 0;   // nameid de l'item glissé (drag-drop vers la barre d'action)
  uint32_t drag_loc_ = 0;  // info+8 de l'item glissé (arg2 équip sur drop fenêtre Équip)
  float drag_mx_ = 0, drag_my_ = 0;

  bool open_ = false;         // inventaire ouvert ce frame ?
  bool was_open_ = false;     // front montant (placement 1re ouverture)
  bool need_pos_ = false;     // repositionner près du natif à l'ouverture
  int  spawn_x_ = 0, spawn_y_ = 0;
  int  item_count_ = 0;       // nb d'items valides dans items_
  // Case survolée ce frame pour l'aperçu de description (0 = aucune) ; l'aperçu est
  // dessiné APRÈS la fenêtre, en tooltip (cf. storage_tweaks).
  uint32_t hover_desc_id_ = 0;
  int      hover_desc_idx_ = -1;

  // ── Verrou « description en vol » (anti-flicker) ────────────────────────────
  // Le menu contextuel masque l'aperçu au survol tant qu'il est ouvert. Au clic
  // sur « Description » le menu se ferme AVANT que la fenêtre de description
  // n'apparaisse : le curseur retombe sur la case et l'aperçu se rouvrait pour
  // quelques frames -> flicker. On le bloque donc dès la DEMANDE de description,
  // jusqu'à ce que le curseur bouge vraiment (le geste est fini), avec un
  // garde-fou de temps si la fenêtre n'arrive jamais.
  bool     desc_pending_ = false;
  float    desc_pending_x_ = 0.0f, desc_pending_y_ = 0.0f;
  uint32_t desc_pending_tick_ = 0;
  // Arme le verrou (à appeler juste avant/après OpenItemDesc).
  void MarkDescPending();
  // Met à jour le verrou et renvoie true si l'aperçu doit rester masqué.
  bool DescPendingBlocksHover();
  Item items_[kMaxItems];
};
