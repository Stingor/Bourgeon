#pragma once

#include <cstdint>

#include "features/windows/item_viewer_base.h"

// ── CartViewer ───────────────────────────────────────────────────────────────
//
// Re-implémentation ImGui de la fenêtre CHARIOT native (UICartWnd, window id 40 =
// 0x28, vtable 0x0103d538, cherchée par ID au gestionnaire), calquée sur
// InventoryViewer — la fenêtre cart EST une sœur de l'inventaire dans le
// client : même framework de fenêtre, mêmes offsets de rect, même modèle d'item.
// C'est cette parenté que `ItemViewerBase` porte désormais : de ce fichier, il
// ne reste que ce qui est PROPRE au chariot.
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

class CartViewer : public ItemViewerBase {
 public:
  const char* name() const override { return "CartViewer"; }

  void OnTick() override;      // état du cart (polling read-only) + masquage natif
  void OnRenderUI() override;  // grille ImGui si le cart est ouvert

  // Contenu de sa section dans le panneau Moonlight. True si un réglage a changé.
  bool DrawSettings();

  // Setting PERSISTANT propre au cart ; les quatre communs (filtre, aperçu de
  // description, onglets verticaux, onglet courant) sont sur ItemViewerBase.
  bool& lock_size()     { return lock_size_; }

  // Appelé par le hook MakeWindow de WindowPosTweaks à la création de la fenêtre
  // id 0x28 : c'est la DEMANDE du joueur. Masque la native avant son premier
  // rendu (pas de flicker) et bascule le viewer ; OnTick la détruit ensuite.
  // 🔴 Détruite, pas masquée : toute bascule du client fait « ferme si elle
  // existe, sinon crée », donc une native vivante avalerait un appui sur deux.
  void HandleNativeCreation(void* win);

 private:
  // Un item du cart, extrait en POD (sous SEH) pour un rendu hors __try.
  struct Item {
    uint32_t id = 0;          // atoi(std::string @info+0x2c)
    int      amount = 0;      // node+0x18 (= info+0x10)
    int      index = 0;       // info+4 : index cart (arg des commandes de transfert)
    int      refine = 0;      // info+0x60
    int      type = 0;        // info+0 : type d'item (onglets)
    uint8_t  identified = 0;  // info+0x5c (résolution d'icône)
    uint8_t  damaged = 0;     // info+0x5d : équipement cassé (rendu rouge)
    char     name[64] = {0};
    // Données d'INSTANCE du stack, pour l'aperçu de description au survol.
    uint32_t cards[4] = {0};  // info+0x1c
    int      opt_count = 0;   // info+0x98
    int      total_slots = 0; // emplacements de carte (itemcell::SlotCount)
    struct Opt { int16_t index; int16_t value; uint8_t param; };
    Opt      opts[5] = {};    // info+0x9c
  };
  static constexpr int kMaxItems = 200;  // marge au-dessus de MAX_CART (100)

  // Remplit items_/item_count_ depuis le modèle session. SEH (POD only).
  void Extract();

  bool lock_size_ = false;          // setting : taille de fenêtre verrouillée

  // Les deux sens SORTANTS, les seuls que cette fenêtre émette ; le
  // `pend_action_` qui les transporte est sur ItemViewerBase.
  enum PendAction { kPendToBody, kPendToStorage };
  // Le défaut de `pend_action_` est posé à 0 dans la base pour les trois
  // viewers : ce n'est correct que tant que le premier énumérateur vaut 0.
  static_assert(kPendToBody == 0, "pend_action_ = 0 doit valoir kPendToBody");

  Item items_[kMaxItems];
};
