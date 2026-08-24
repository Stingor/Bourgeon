#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "features/item_cell.h"  // itemcell::ItemRow / ExtractList
#include "features/windows/item_viewer_base.h"

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

class StorageWindow : public ItemViewerBase {
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

  // (`IsOpen()`, `PointOverViewer()`, `show_panel()`, `desc_tooltip()`,
  // `show_filter()`, `tabs_vertical()`, `cur_tab()` et la bascule
  // `imgui_enabled_` sont sur ItemViewerBase — les trois viewers d'objets les
  // portaient à l'identique. Ce qui suit est PROPRE à l'entrepôt.
  //
  // Une raison de plus d'interroger `IsOpen()` plutôt que la fenêtre native,
  // celle-ci : le serveur refuse inventaire <-> cart tant qu'un storage est
  // ouvert (sd->state.storage_flag), et les deux autres viewers n'arment leur
  // transfert que sur ce test.)

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, section « Storage » du
  // panneau Moonlight ; chargés/sauvés par MoonlightUi comme imgui_enabled_).
  bool& show_index_col() { return show_index_col_; }
  bool& show_id_col()    { return show_id_col_; }
  bool& show_slots_col() { return show_slots_col_; }
  bool& show_value_col() { return show_value_col_; }
  bool& show_total_value() { return show_total_value_; }
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
  // Disposition VERTICALE uniquement : true = tuiles images du client (tab_*.bmp),
  // false = onglets texte au libellé tourné à 90°.
  // (`tabs_vertical()` lui-même est sur ItemViewerBase ; ⚠ c'est la seule des
  // trois fenêtres à le vouloir FAUX par défaut — son constructeur le pose.)
  bool& tab_images()     { return tab_images_; }

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

 private:
  // 🔴 Le décodage, sur le FIL PRINCIPAL. OnRecvPacket (fil réseau) ne fait que
  // copier : `prices_` et `meta_` sont des tables de hachage, et une insertion qui
  // REHASHE pendant que le rendu y cherche un prix est un crash.
  // Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Un item du storage, extrait en POD (sous SEH) pour rendre hors __try.
  // Le POD partage des trois viewers (features/item_cell.h). L'entrepot y gagne
  // `total_slots`, qu'il ne lisait pas.
  using Item = itemcell::ItemRow;
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

  bool  show_id_col_ = false; // setting : afficher une colonne avec l'id d'item
  bool  show_index_col_ = false;  // setting : afficher l'index storage (slot)
  bool  show_slots_col_ = false;  // setting : afficher une colonne nb de slots carte
  // « Remettre l'ordre d'origine » (colonnes) demandé depuis le panneau de
  // réglages. PAS un setting : l'ordre lui-même vit dans imgui.ini, tenu par
  // ImGui. Le drapeau attend la prochaine table — le panneau s'ouvre aussi
  // storage fermé, et la demande doit tout de même être honorée.
  bool  reset_col_order_ = false;
  bool  show_value_col_ = true;    // setting : colonne prix de revente (NPC * qté)
  bool  show_total_value_ = true;  // setting : valeur estimée du storage (en-tête)
  bool  show_storage_tabs_ = false;  // setting (opt-in) : onglets de storage
  bool  show_type_tabs_ = true;      // setting : onglets de catégorie + sous-type
  bool  tab_images_ = true;      // setting (vertical) : tuiles images vs texte 90°
  // Onglet persisté déjà ré-appliqué au TabBar ? (une seule fois par session)
  bool  tab_applied_ = false;

  std::unordered_map<uint32_t, uint32_t> prices_;  // id -> prix de vente NPC (serveur)
  // Métadonnées item (serveur, statiques) pour les sous-catégories : subtype = type
  // d'arme (W_*) ou de munition (A_*) ; equip = masque de slot d'équipement.
  struct ItemMeta { uint8_t subtype = 0; uint32_t equip = 0; uint16_t slots = 0; };
  std::unordered_map<uint32_t, ItemMeta> meta_;    // id -> {subtype, equip}
  int cur_sub_ = -1;  // sous-catégorie sélectionnée (clé SubCat, -1 = Tout)
  // Sens d'un déplacement en attente — les deux sens SORTANTS, les seuls que
  // cette fenêtre émette ; `pend_index_` (sur ItemViewerBase) est donc toujours
  // un index STORAGE. Ce qui entre dans le storage part de la fenêtre d'origine
  // (inventaire, cart), qui envoie son propre paquet.
  enum PendAction { kPendStoToInv, kPendStoToCart };
  // Le défaut de `pend_action_` est posé à 0 dans la base pour les trois
  // viewers : ce n'est correct que tant que le premier énumérateur vaut 0.
  static_assert(kPendStoToInv == 0, "pend_action_ = 0 doit valoir kPendStoToInv");

  int   used_ = 0, max_ = 0;  // compteur du serveur (ZC_NOTIFY_STOREITEM_COUNTINFO)
  // Nom du storage ouvert, envoyé par le serveur dans ZC_INVENTORY_START (0x0b08,
  // invType STORAGE=2) : "Storage" / "Guild Storage" / nom premium. Sert de titre.
  char  storage_name_[32] = {0};
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
