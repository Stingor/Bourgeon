#pragma once

#include <cstdint>

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
  InventoryViewer();

  const char* name() const override { return "InventoryViewer"; }

  void OnTick() override;      // capture l'état de l'inventaire (polling read-only)
  void OnRenderUI() override;  // dessine la grille ImGui si l'inventaire est ouvert

  // Setting PERSISTANT (bourgeon_settings.yaml "inventory_imgui", géré par
  // MoonlightUi comme storage_imgui) : ON = viewer ImGui + natif caché ; OFF =
  // inventaire natif seul, aucun viewer. Public pour le chargement/sauvegarde.
  bool imgui_enabled_ = false;  // OPT-IN : inventaire natif par defaut

  bool& show_panel() { return show_panel_; }

  // Appelé par le hook MakeWindow de WindowPosTweaks à la création de la fenêtre
  // id 8 : si imgui_enabled_, force wnd+0x28 = 0 AVANT le 1er rendu -> pas de
  // flicker (le OnTick seul laisserait passer une frame native visible).
  void HideNativeAtCreation(void* win);

  // Hooks WndProc (pré-input, comme storage/skill_bar) : router un drag NATIF
  // relâché sur le viewer, et mémoriser où le clic a démarré.
  bool HandleNativeDrop(int mx, int my);
  void OnMouseDown(int mx, int my);

 private:
  // Un item d'inventaire, extrait en POD (sous SEH) pour un rendu hors __try.
  struct Item {
    uint32_t id = 0;          // atoi(std::string @info+0x2c) — la SOURCE du jeu
    int      amount = 0;      // node+0x18
    int      index = 0;       // info+4 : index inventaire (arg use/equip/drop)
    int      type = 0;        // info+0 : type d'item (onglets)
    uint8_t  identified = 0;  // info+0x5c (résolution d'icône)
    uint8_t  favorite = 0;    // node+0x90 (onglet favoris)
    char     name[64] = {0};
  };
  static constexpr int kMaxItems = 500;  // marge au-dessus de la capacité serveur

  // Remplit items_/item_count_ depuis le modèle session. SEH (POD only).
  void Extract();

  bool show_panel_ = true;    // transitoire : clic sur le X (ferme la session)
  int  cur_tab_ = 0;          // onglet catégorie sélectionné
  bool sort_enabled_ = true;  // bouton Tri (footer, onglet Favoris) : trie la vue

  // Rect écran du viewer (capturé au rendu) pour tester un drop natif dessus.
  float win_x_ = 0, win_y_ = 0, win_w_ = 0, win_h_ = 0;
  bool  win_valid_ = false;
  bool  mousedown_over_viewer_ = false;

  // Action en attente (posée par un drag/clic, traitée au rendu, + prompt qté).
  enum PendAction { kPendUse, kPendEquip, kPendDrop, kPendToCart, kPendToStorage };
  int  pend_id_ = 0;      // 0 = aucune action en attente
  int  pend_index_ = 0;   // index inventaire de l'item concerné
  int  pend_max_ = 0;     // quantité max (stack) pour le prompt
  bool pend_open_prompt_ = false;
  int  pend_action_ = kPendUse;

  // Drag d'un item du viewer (-> équip/sol/chariot selon la cible).
  bool  drag_active_ = false;
  int   drag_index_ = 0, drag_amount_ = 0, drag_type_ = 0;
  float drag_mx_ = 0, drag_my_ = 0;

  bool open_ = false;         // inventaire ouvert ce frame ?
  bool was_open_ = false;     // front montant (placement 1re ouverture)
  bool need_pos_ = false;     // repositionner près du natif à l'ouverture
  int  spawn_x_ = 0, spawn_y_ = 0;
  int  item_count_ = 0;       // nb d'items valides dans items_
  Item items_[kMaxItems];
};
