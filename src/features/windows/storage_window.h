#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "features/plugin.h"

// ── StorageWindow ───────────────────────────────────────────────────────────
//
// Viewer ImGui du storage (Kafra / guilde / premium). Quand imgui_enabled_
// est ON, la fenêtre NATIVE NE NAÎT PLUS : on prend la place de ses handlers de
// paquets (RegisterReplaceOpcode) au lieu de la masquer après coup — une native
// masquée reste vivante et garde le clavier.
//
// RE (cf. mémoire project_storage_window_re et docs/storage_window_re.md) :
//   - Fenêtre native = UIItemStoreWnd, id 0x21, vtable 0x0103ca40.
//   - Modèle des items = g_session+0x1718 (0x015fbad8), peuplé par les listes
//     0x0b09/0x0b39 INDÉPENDAMMENT de la fenêtre : leur ingesteur ne touche à
//     g_StorageWnd_ptr que sous un test de nullité. C'est ce qui rend le
//     remplacement possible — le modèle vit sans la fenêtre.
//   - Compteur used/max : NON lu dans la fenêtre, reçu en 0x00f2.
//
// 🔴 DEUX opcodes créent la fenêtre 0x21 sur ce serveur, pas un :
//   - 0x0b08 ZC_INVENTORY_START, mais seulement pour invType 2 (STORAGE) et 3
//     (GUILD_STORAGE) — les invType 0 et 1 ouvrent l'inventaire et le cart,
//     d'où le prédicat qui LIT le paquet ;
//   - 0x00f2 ZC_NOTIFY_STOREITEM_COUNTINFO, qui appelle MakeWindow(0x21) puis
//     déréférence le retour SANS test. Le laisser au natif ressusciterait la
//     fenêtre juste après l'ouverture.
// (Cinq autres cases du dispatcher la créent aussi — les listes storage des
// packetvers anciens. moonlight n'envoie que 0x0b09/0x0b39 ; la purge de OnTick
// est le filet si l'une d'elles arrivait quand même.)

class StorageWindow : public Plugin {
 public:

  // Contenu de sa section dans le panneau Moonlight. Rend true si un réglage
  // a changé — l'appelant décide de sauvegarder, une seule fois.
  bool DrawSettings();
  StorageWindow();  // enregistre l'opcode ZC_BOURGEON_STORAGE_PRICES

  const char* name() const override { return "StorageWindow"; }

  void OnTick() override;      // capture l'état du storage (polling read-only)
  void OnRenderUI() override;  // dessine le viewer ImGui si le storage est ouvert
  // Reçoit les prix de vente du storage (ZC_BOURGEON_STORAGE_PRICES 0x0F0F).
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  bool& show_panel() { return show_panel_; }

  // Une session storage est-elle ouverte ? À interroger par les AUTRES modules
  // (inventaire, cart) au lieu de chercher la fenêtre native : elle ne naît plus
  // en mode ImGui, et un `FindWindow(0x21)` nul y passerait pour « storage
  // fermé ». Or plusieurs de leurs règles en dépendent — le serveur refuse
  // inventaire <-> cart tant qu'un storage est ouvert (sd->state.storage_flag),
  // et ces modules n'arment le transfert que sur ce test.
  bool IsOpen() const { return open_; }

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, section « Storage » du
  // panneau Moonlight ; chargés/sauvés par MoonlightUi comme imgui_enabled_).
  // Description au SURVOL : ouvre la VRAIE fenêtre de description (celle du clic
  // droit) tant que la souris reste sur la ligne, et la ferme en sortant.
  bool& desc_tooltip()   { return show_desc_tooltip_; }
  bool& show_index_col() { return show_index_col_; }
  bool& show_id_col()    { return show_id_col_; }
  bool& show_slots_col() { return show_slots_col_; }
  bool& show_value_col() { return show_value_col_; }
  bool& show_total_value() { return show_total_value_; }
  bool& show_filter()    { return show_filter_; }
  // Filtres par TYPE d'item : les onglets de catégorie (Tout / Favoris / Consos
  // / Armes…) ET le combo « Sous-type » qui en dépend. Décoché, la fenêtre est
  // une liste unique — comme le champ de filtre, on ne laisse rien de masqué
  // derrière : la vue repasse à « Tout », sous-catégorie comprise.
  bool& show_type_tabs() { return show_type_tabs_; }
  // Onglets de STORAGE (principal / alternatifs) — OPT-IN. La liste vient du
  // serveur (ZC 0x0F1E, filtrée par ses droits) ; cocher n'ouvre aucun droit, ça
  // ne fait qu'afficher la rangée. Décoché, tout le reste de la fenêtre est
  // strictement identique à avant.
  bool& show_storage_tabs() { return show_storage_tabs_; }
  // Disposition des onglets de catégorie : false = horizontale (TabBar, défaut),
  // true = verticale à gauche (comme la fenêtre native).
  bool& tabs_vertical()  { return tabs_vertical_; }
  // Disposition VERTICALE uniquement : true = tuiles images du client (tab_*.bmp),
  // false = onglets texte au libellé tourné à 90°.
  bool& tab_images()     { return tab_images_; }
  int&  cur_tab()        { return cur_tab_; }

  // Setting PERSISTANT (bourgeon_settings.yaml "storage_imgui", géré par MoonlightUi) :
  // ON = viewer ImGui, la fenêtre native ne s'ouvre plus ; OFF = storage natif
  // seul, aucun viewer. Pas de cohabitation — et le réglage n'est jamais touché
  // seul : SetModernInterface l'écrit avec tout le groupe « Interface moderne ».
  // Public pour que MoonlightUi le charge/sauve (comme sb->enabled_).
  bool imgui_enabled_ = false;

  // Favoris 100 % CLIENT (aucun paquet, aucun flag serveur — le storage n'a pas de
  // flag favori par item, contrairement à l'inventaire). Set d'ids d'items marqués
  // favoris -> onglet « Favoris » + étoile sur l'icône. Keyé par id d'item (tous les
  // stacks/raffinements d'un même id sont favoris ensemble). Persisté par MoonlightUi
  // (bourgeon_settings.yaml "storage_favorites"). Public pour la persistance.
  std::unordered_set<uint32_t> favorites_;
  bool IsFavorite(uint32_t id) const { return favorites_.count(id) != 0; }
  void ToggleFavorite(uint32_t id) {
    if (id == 0) return;
    auto it = favorites_.find(id);
    if (it != favorites_.end()) favorites_.erase(it);
    else favorites_.insert(id);
  }

  // Personnalisation 100 % CLIENT des onglets de storage (aucun paquet, aucun
  // état serveur) : nom libre et icône d'item, par id de storage. Le serveur
  // envoie « Storage Alt 3 » ; le joueur, lui, sait que c'est son entrepôt à
  // consommables et veut le voir écrit — ou reconnu à une potion rouge.
  // Un nom vide = on retombe sur celui du serveur ; icône 0 = libellé texte.
  // Persisté par MoonlightUi (bourgeon_settings.yaml "storage_tab_custom"),
  // d'où le public, comme favorites_.
  struct TabCustom {
    char     name[25] = {0};
    uint32_t icon_id  = 0;
  };
  std::unordered_map<uint32_t, TabCustom> tab_custom_;

  // (Plus de HandleNativeDrop / OnMouseDown : ils accueillaient un drag NATIF venu
  // de l'inventaire ou du cart. « Interface moderne » étant un groupe tout-ou-rien,
  // ces deux fenêtres sont des viewers ImGui dès que celle-ci l'est, et leurs
  // natives masquées sont hors hit-test — plus aucun drag natif ne peut en partir.)

  // True si (mx,my) est au-dessus de la fenêtre du viewer storage (ImGui) ouverte.
  // Sert aux viewers INVENTAIRE et CART pour router un dépôt par glisser : c'est
  // la SEULE cible possible, la fenêtre native n'existant plus.
  bool PointOverViewer(int mx, int my) const {
    return open_ && imgui_enabled_ && win_valid_ && mx >= win_x_ && my >= win_y_ &&
           mx < win_x_ + win_w_ && my < win_y_ + win_h_;
  }

 private:
  // 🔴 Le décodage, sur le FIL PRINCIPAL. OnRecvPacket (fil réseau) ne fait que
  // copier : `prices_` et `meta_` sont des tables de hachage, et une insertion qui
  // REHASHE pendant que le rendu y cherche un prix est un crash.
  // Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Un item du storage, extrait en POD (sous SEH) pour rendre hors __try.
  struct Item {
    uint32_t id = 0;          // atoi(info+0x2c) — la SOURCE que le jeu utilise
    int      amount = 0;
    int      index = 0;       // info+4 (= node+0xc) : index storage pour le retrait
    int      type = 0;        // info+0 : type d'item (pour les onglets)
    uint8_t  identified = 0;  // info+0x5c (flag pour la résolution d'icône)
    uint8_t  damaged = 0;     // info+0x5d : équipement cassé (rendu rouge)
    int      refine = 0;      // info+0x60 : niveau de refine (préfixe « +N » de l'aperçu)
    char     name[64] = {0};
    // Données d'INSTANCE lues dans l'ItemSkillInfo (absentes de la DB) : elles
    // alimentent l'aperçu de description au survol.
    uint32_t cards[4] = {0};  // info+0x1c : 4 slots carte/enchant (0 = vide)
    int      opt_count = 0;   // info+0x98 : nombre de random options
    struct Opt { int16_t index; int16_t value; uint8_t param; };
    Opt      opts[5] = {};    // info+0x9c : entrées de 5 octets
  };
  static constexpr int kMaxItems = 700;  // MAX_STORAGE serveur (marge)

  // Remplit items_/item_count_ depuis le MODÈLE de session (g_session+0x1718),
  // qui existe que la fenêtre native soit là ou non. SEH (POD only).
  void Extract();

  // Ferme la session CÔTÉ CLIENT (viewer + état), sans rien envoyer : pour les
  // fermetures dont le serveur nous informe (0x00f8) et celles qu'il fait en
  // SILENCE. 🔴 Au warp, unit_remove_map_ appelle storage_storage_quit(), qui
  // sauvegarde et remet storage_flag à 0 SANS envoyer 0x00f8 : le serveur
  // suppose que le client ferme de lui-même. Sans le reset au changement de
  // map, le viewer resterait ouvert sur un storage qui n'existe plus.
  void CloseLocal();

  bool  show_panel_ = true;   // transitoire : détection du clic sur le X (ferme la session)
  bool  show_id_col_ = false; // setting : afficher une colonne avec l'id d'item
  bool  show_index_col_ = false;  // setting : afficher l'index storage (slot)
  bool  show_slots_col_ = false;  // setting : afficher une colonne nb de slots carte
  bool  show_desc_tooltip_ = false;  // setting : description native au survol
  // « Remettre l'ordre d'origine » (colonnes) demandé depuis le panneau de
  // réglages. PAS un setting : l'ordre lui-même vit dans imgui.ini, tenu par
  // ImGui. Le drapeau attend la prochaine table — le panneau s'ouvre aussi
  // storage fermé, et la demande doit tout de même être honorée.
  bool  reset_col_order_ = false;
  bool  show_value_col_ = true;    // setting : colonne prix de revente (NPC * qté)
  bool  show_total_value_ = true;  // setting : valeur estimée du storage (en-tête)
  bool  show_filter_ = true;  // setting : afficher le champ de filtre par nom
  bool  show_storage_tabs_ = false;  // setting (opt-in) : onglets de storage
  bool  show_type_tabs_ = true;      // setting : onglets de catégorie + sous-type
  bool  tabs_vertical_ = false;  // setting : onglets verticaux à gauche (natif)
  bool  tab_images_ = true;      // setting (vertical) : tuiles images vs texte 90°
  int   cur_tab_ = 0;         // onglet catégorie sélectionné (0 = Tout), persisté
  // Item survolé ce frame (0 = aucun) : alimente l'aperçu de description, un
  // panneau RO simplifié dessiné au curseur après la fenêtre du viewer. L'INDEX
  // sert à retrouver cartes/options (données d'instance) ; il n'est valable que
  // dans la frame courante, items_ étant reconstruit à chaque tick.
  uint32_t hover_desc_id_ = 0;
  int      hover_desc_idx_ = -1;
  // ── Verrou « description en vol » (anti-flicker) ────────────────────────────
  // Le menu contextuel masque l'aperçu au survol tant qu'il est ouvert. Au clic
  // sur « Description » le menu se ferme AVANT que la fenêtre de description
  // n'apparaisse : le curseur retombe sur la ligne et l'aperçu se rouvrait pour
  // quelques frames -> flicker. On le bloque donc dès la DEMANDE de description,
  // jusqu'à ce que le curseur bouge vraiment (le geste est fini), avec un
  // garde-fou de temps si la fenêtre n'arrive jamais.
  bool     desc_pending_ = false;
  float    desc_pending_x_ = 0.0f, desc_pending_y_ = 0.0f;
  uint32_t desc_pending_tick_ = 0;
  // Arme le verrou (à appeler juste avant OpenItemDesc).
  void MarkDescPending();
  // Met à jour le verrou et renvoie true si l'aperçu doit rester masqué.
  bool DescPendingBlocksHover();
  // Onglet persisté déjà ré-appliqué au TabBar ? (une seule fois par session)
  bool  tab_applied_ = false;

  // Rect écran du viewer (capturé au rendu) : sert aux AUTRES viewers pour tester
  // qu'un glisser est relâché dessus (PointOverViewer).
  float win_x_ = 0, win_y_ = 0, win_w_ = 0, win_h_ = 0;
  bool  win_valid_ = false;
  std::unordered_map<uint32_t, uint32_t> prices_;  // id -> prix de vente NPC (serveur)
  // Métadonnées item (serveur, statiques) pour les sous-catégories : subtype = type
  // d'arme (W_*) ou de munition (A_*) ; equip = masque de slot d'équipement.
  struct ItemMeta { uint8_t subtype = 0; uint32_t equip = 0; uint16_t slots = 0; };
  std::unordered_map<uint32_t, ItemMeta> meta_;    // id -> {subtype, equip}
  int cur_sub_ = -1;  // sous-catégorie sélectionnée (clé SubCat, -1 = Tout)
  // Sens d'un déplacement en attente — les deux sens SORTANTS, les seuls que
  // cette fenêtre émette ; `pend_index_` est donc toujours un index STORAGE. Ce
  // qui entre dans le storage part de la fenêtre d'origine (inventaire, cart),
  // qui envoie son propre paquet.
  enum PendAction { kPendStoToInv, kPendStoToCart };
  int   pend_id_ = 0;      // 0 = aucune action en attente
  int   pend_index_ = 0;   // index storage de la source
  int   pend_max_ = 0;     // quantité max (stack)
  bool  pend_open_prompt_ = false;  // ouvrir le prompt quantité au prochain rendu
  int   pend_action_ = kPendStoToInv;  // sens du déplacement en attente
  // Retrait par drag (viewer -> viewer inventaire) : suivi du drag en cours.
  bool  drag_active_ = false;
  int   drag_index_ = 0, drag_amount_ = 0;
  float drag_mx_ = 0, drag_my_ = 0;  // dernière pos souris pendant le drag
  // Session storage ouverte ? Posé par le paquet d'ouverture (0x0b08 pour un
  // invType de storage), levé par CloseLocal. Ne se déduit plus de la présence
  // d'une fenêtre native.
  bool  open_ = false;
  // Valeur d'imgui_enabled_ au tick précédent : sert à voir la BASCULE de mode,
  // qui doit être traitée pendant qu'une session est ouverte (cf. OnTick).
  bool  prev_imgui_enabled_ = false;
  bool  need_pos_ = false;    // poser la position par défaut à la 1re ouverture
  int   used_ = 0, max_ = 0;  // compteur du serveur (ZC_NOTIFY_STOREITEM_COUNTINFO)
  // Nom du storage ouvert, envoyé par le serveur dans ZC_INVENTORY_START (0x0b08,
  // invType STORAGE=2) : "Storage" / "Guild Storage" / nom premium. Sert de titre.
  char  storage_name_[32] = {0};
  int   item_count_ = 0;      // nb d'items valides dans items_
  Item  items_[kMaxItems];

  // ── Onglets de STORAGE (ZC 0x0F1E / CZ 0x0F1D) ──────────────────────────────
  // La liste vient du SERVEUR, filtrée par les droits du joueur (les mêmes que
  // les commandes @storage / @storagealtN). Rien n'est codé en dur ici : ni les
  // ids, ni les noms, ni leur nombre — ajouter un storage dans inter_server.yml
  // suffit à le faire apparaître.
  // (Pas d'abréviation stockée : elle se dérive du nom AFFICHÉ, lequel peut être
  // celui que le joueur s'est donné — deux sources de vérité pour rien.)
  struct StorageTab {
    uint8_t id = 0;
    char    name[25] = {0};  // NAME_LENGTH serveur (24) + NUL
  };
  static constexpr int kMaxStorageTabs = 16;
  StorageTab stg_tabs_[kMaxStorageTabs];
  int stg_tab_count_ = 0;
  int cur_storage_id_ = -1;  // id du storage ouvert (-1 = aucun / pas encore su)

  // 🔴 Bascule en cours. Entre la demande et la nouvelle liste, le serveur FERME
  // le storage courant (ZC 0x00f8) : c'est voulu — le handler natif de ce paquet
  // VIDE le modèle d'items de session, seul moyen d'être sûr que rien de
  // l'ancien storage ne se mélange au suivant. Mais CloseLocal, lui, fermerait
  // le viewer, qui renaîtrait à sa position par défaut à chaque changement
  // d'onglet. Ce drapeau distingue donc « le serveur a fermé » de « je bascule ».
  bool     switching_ = false;
  uint8_t  switch_target_ = 0;
  uint32_t switch_tick_ = 0;  // garde-fou : au-delà, la bascule est abandonnée
  // Envoie CZ_BOURGEON_OPEN_STORAGE et arme la bascule.
  void SendOpenStorage(uint8_t id);
  // Vide tout ce qui décrit le CONTENU du storage courant (items, compteur,
  // action en attente, survol, glisser) sans toucher à la fenêtre ni aux
  // réglages. Utilisée par la bascule ET par CloseLocal.
  void ClearStorageData();
  // Sous-catégorie à réinitialiser au prochain rendu : une sous-cat n'a de sens
  // que dans un onglet où elle existe encore, et les items changent à chaque
  // bascule. Posé par ClearStorageData (donc à chaque changement de storage).
  bool reset_sub_ = false;
  // Texte du filtre à vider au prochain rendu. Posé UNIQUEMENT à la fermeture :
  // le joueur qui cherche un item le cherche dans TOUS ses entrepôts, donc le
  // filtre survit aux bascules et se réapplique au contenu qui arrive. Le
  // filtre est un statique de OnRenderUI, d'où le drapeau plutôt qu'un appel.
  bool reset_filter_ = false;
};
