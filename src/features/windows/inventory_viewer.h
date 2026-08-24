#pragma once

#include <cstdint>
#include <unordered_map>

#include "features/net_inbox.h"
#include "features/item_cell.h"  // itemcell::ItemRow / ExtractList
#include "features/windows/item_viewer_base.h"

// ── InventoryViewer ──────────────────────────────────────────────────────────
//
// Re-implémentation ImGui COMPLÈTE de la fenêtre inventaire native (UIItemWnd,
// window id 8), calquée sur StorageWindow. Elle REMPLACE le natif (caché via le
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
// drag-out + transfert ALT vers cart/entrepôt. (Les opcodes/commandes exacts
// sont finalisés par la RE en cours ; cf. project_inventory_viewer_wip.)
//
// Réutilise de storage_window.cc : cache d'icônes, BuildDisplayName, OpenItemDesc
// (MakeWindow 0xc + OnMsg 0x18), skin ro::, lecture du payload de drag natif.
//
// L'état que les trois viewers d'objets ont en commun — session ouverte, rect
// écran, glisser en cours, action en attente, onglets — est sur
// `ItemViewerBase` ; ici ne reste que ce qui est PROPRE à l'inventaire.

class InventoryViewer : public ItemViewerBase {
 public:

  // Contenu de sa section dans le panneau Moonlight. Rend true si un réglage
  // a changé — l'appelant décide de sauvegarder, une seule fois.
  bool DrawSettings();
  InventoryViewer();

  const char* name() const override { return "InventoryViewer"; }

  void OnTick() override;      // capture l'état de l'inventaire (polling read-only)
  void OnRenderUI() override;  // dessine la grille ImGui si l'inventaire est ouvert
  // Reçoit ZC_ITEMCOMPOSITION_LIST (0x017B, dont on a pris la place du handler
  // natif) et ZC_BOURGEON_COMPAT_CARDS (sertissage rapide). Fil RÉSEAU : on COPIE
  // et rien d'autre, le décodage repart à la frame (cf. features/net_inbox.h).
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // ── Settings PERSISTANTS propres à l'inventaire (section « Inventaire » du
  // panneau Moonlight ; chargés/sauvés par MoonlightUi). La clé de bascule
  // "inventory_imgui" et les quatre réglages communs (filtre, aperçu de
  // description au survol, onglets verticaux, onglet courant) sont sur
  // ItemViewerBase.
  //
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
  // id 8 : c'est la DEMANDE du joueur. Masque la native avant son premier rendu
  // (pas de flicker) et bascule le viewer ; OnTick la détruit ensuite.
  // 🔴 Détruite, pas masquée : toute bascule du client fait « ferme si elle
  // existe, sinon crée », donc une native vivante avalerait un appui sur deux.
  void HandleNativeCreation(void* win);

  // ── Sertissage de cartes (ex-popup natif UIItemCompositionWnd, id 0x4A) ─────
  // Double-cliquer une carte envoie CZ_REQ_ITEMCOMPOSITION_LIST (0x017A) ; le
  // SERVEUR répond (0x017B) avec la liste des équipements compatibles à slot
  // libre. Le filtrage est 100 % SERVEUR — le client n'évalue AUCUNE règle de
  // compatibilité, et nous non plus. Cf. docs/card_insert_re.md.
  //
  // 🔴 Le popup natif ne naît PLUS : on prend la place du handler de ZC 0x017B
  // (RegisterReplaceOpcode, révocable sur `imgui_enabled_`) et on tient nous-mêmes
  // la liste des candidats. Avant, on laissait le natif se créer pour LIRE sa
  // std::list, ce qui imposait de le masquer — donc de vivre avec une fenêtre
  // vivante qui garde le clavier (Entrée validait son bouton OK invisible) et avec
  // un offset de liste ambigu résolu à l'exécution. Les deux ont disparu avec elle.
  //
  // PAS de réglage séparé : le sertissage suit `imgui_enabled_`. Les deux fenêtres
  // forment un tout — proposer un inventaire ImGui qui ouvre un popup natif (ou
  // l'inverse) serait incohérent à l'usage.
  //
  // Filet de sécurité, appelé par le hook MakeWindow sur l'id 0x4A : si le popup
  // natif naissait quand même (bascule de mode en plein sertissage), on le masque
  // ici avant sa première frame et OnTick le détruit.
  void HandleCardInsertCreation(void* win);

  // (Plus de HandleNativeDrop / OnMouseDown : aucune fenêtre native ne peut plus
  // émettre un glisser vers ce viewer — équipement, cart et storage sont tous des
  // viewers ImGui, et leurs natives ne naissent plus.)

  // (`IsOpen()` et `PointOverViewer()` sont sur ItemViewerBase : les trois
  // viewers répondent aux deux mêmes questions, pour les deux autres.)

  // Équipe l'item d'inventaire ACTUELLEMENT GLISSÉ (index/type/loc SERVEUR stables du
  // drag en cours) — le serveur place/swappe automatiquement. No-op si aucun drag actif
  // ou item non équipable. Renvoie true si équipé. Utilisé par le drag-drop cross-plugin
  // de character_sheet (lâcher un item d'inventaire sur le doll) ; robuste à une
  // renumérotation d'items_ en cours de glisser (l'index utilisé est stable).
  bool EquipDraggedItem(bool left_hand);

  // Ajoute l'item ACTUELLEMENT GLISSÉ à l'échange en cours (cible de drop « Mon offre »
  // de trade_window). Une PILE ouvre le prompt de quantité, un item seul part direct.
  // No-op si aucun glisser en cours ou si aucun échange n'est ouvert.
  bool TradeDraggedItem();

  // Idem pour le COURRIER : joint l'item glissé au courrier en cours d'écriture
  // (cible de drop « Pièces jointes » de rodex_window). Même politique de quantité.
  // No-op si aucun glisser en cours ou si aucune écriture n'est ouverte.
  bool MailDraggedItem();

  // Nameid de l'item ACTUELLEMENT GLISSÉ (0 si aucun drag en cours). Utilisé par
  // skill_bar pour assigner l'item glissé à une case de la barre d'action
  // (drag-drop cross-plugin, comme EquipDraggedItem pour le doll de character_sheet).
  // Le nameid est la donnée stockée par un slot d'item de la barre (WriteSlotRecord).
  uint32_t DraggedItemNameId() const { return drag_active_ ? drag_id_ : 0; }

  // Insère le lien de l'item d'index inventaire `invIndex` dans l'input chat focalisé
  // (comme Maj+clic gauche dans l'inventaire). No-op si aucun input chat n'a le focus.
  // Marche même inventaire fermé (lit le modèle session). Utilisé par character_sheet
  // (Maj+clic gauche sur un slot équipé ou sur la munition).
  void LinkItemToChat(int invIndex);

 private:
  // Un item d'inventaire, extrait en POD (sous SEH) pour un rendu hors __try.
  // Le POD partage des trois viewers (features/item_cell.h).
  using Item = itemcell::ItemRow;
  static constexpr int kMaxItems = 500;  // marge au-dessus de la capacité serveur

  // Remplit items_/item_count_ depuis le modèle session. SEH (POD only).
  void Extract();

  // Décodage des paquets, rejoué sur le fil PRINCIPAL (le fil réseau ne fait que
  // copier — cf. features/net_inbox.h). Drainé par Bourgeon à chaque frame via la
  // file de `Plugin` : ce module en déclarait une SECONDE, du même nom, qui masquait
  // celle de la base — d'où un décodage qui n'avait lieu qu'au tick, bridé à 100 ms.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Dessine la fenêtre de sertissage si une session est en cours. Appelée AVANT
  // le early-return de OnRenderUI : elle vit sa propre vie et peut rester ouverte
  // alors que l'inventaire est refermé.
  void RenderCardInsert();
  // Termine la session de sertissage (oublie carte, candidats et sélection).
  void CloseCardInsert();

  // ── État du sertissage, porté par NOUS depuis la mort du popup natif ────────
  bool ci_open_ = false;   // une session est en cours (ZC 0x017B reçu, pas encore close)
  int  ci_card_ = 0;       // index inventaire de la carte source (lu dans CMode+0x45c)
  static constexpr int kCiMaxCands = 64;  // très au-delà de ce qu'un inventaire propose
  int  ci_cands_[kCiMaxCands] = {0};      // index inventaire des équipements candidats
  int  ci_cand_count_ = 0;                // tels que le SERVEUR les a listés

  // Candidat sélectionné dans la fenêtre de sertissage : index INVENTAIRE (pas un
  // rang de liste), donc stable si le serveur renvoie une liste différente.
  // -1 = aucune sélection. Remis à -1 à la fermeture de la session.
  int ci_sel_ = -1;
  // Front montant : sert à placer la fenêtre ET à la mettre au premier plan la
  // frame où elle apparaît (sinon elle s'ouvre derrière l'inventaire, qui a le
  // focus puisque c'est de là que part le double-clic sur la carte).
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

  bool lock_size_ = false;       // setting : taille de fenêtre verrouillée
  bool free_layout_ = false;     // setting : placement libre (exige lock_size_)
  bool sort_enabled_ = true;  // bouton Tri (footer, onglet Favoris) : trie la vue

  // Les sept actions que cette fenêtre peut mettre en attente ; le
  // `pend_action_` qui les transporte est sur ItemViewerBase.
  enum PendAction { kPendUse, kPendEquip, kPendDrop, kPendToCart, kPendToStorage,
                    kPendToTrade, kPendToMail };
  // Le défaut de `pend_action_` est posé à 0 dans la base pour les trois
  // viewers : ce n'est correct que tant que le premier énumérateur vaut 0.
  static_assert(kPendUse == 0, "pend_action_ = 0 doit valoir kPendUse");

  // Ce que l'inventaire glisse EN PLUS des quatre champs communs : le drop sur
  // le doll et sur la barre d'action a besoin du type, du nameid et du masque
  // d'emplacement, que les deux autres viewers n'ont pas à connaître.
  int      drag_type_ = 0;
  uint32_t drag_id_ = 0;   // nameid de l'item glissé (drag-drop vers la barre d'action)
  uint32_t drag_loc_ = 0;  // info+8 de l'item glissé (arg2 équip sur drop fenêtre Équip)

  Item items_[kMaxItems];
};
