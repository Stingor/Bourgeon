#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "plugins/plugin.h"

// ── CashShopTweaks ───────────────────────────────────────────────────────────
//
// Redraw COMPLET et moderne du cash shop natif ("Simple Cash Shop" =
// UICashShopWnd, id 0x13e, vtable 0x0101ca18). En REMPLACEMENT du natif : dès
// qu'une fenêtre 0x13e est créée on la cache (win+0x28=0, hors rendu + input) et
// on dessine une UI ImGui à la place. Le natif reste vivant côté session (le
// serveur ne voit pas la différence) mais l'utilisateur n'interagit qu'avec le
// viewer.
//
// Contenu = 100 % SERVEUR-driven (cf. mémoire project_cashshop_re). On OBSERVE
// les paquets (le handler natif tourne toujours dessous) pour bâtir notre modèle :
//   - ZC_SE_CASHSHOP_OPEN 0x0b6e  : cashPoints, kafraPoints (points du compte).
//   - ZC_ACK_SCHEDULER_CASHITEM 0x08ca : {count, tabNum, items[]{id, price, ...}}
//     — un paquet par onglet ; c'est le VRAI peuplement sur ce packetver (le
//     couple 0x846/0x8c0 est du code serveur mort ici, cf. .cc).
//   - ZC_SE_PC_BUY_CASHITEM_RESULT 0x0849 : result + points mis à jour.
// La liste est déclenchée par la list-request 0x08c9 (2 octets) que le natif
// envoie déjà à l'ouverture ; on l'émet aussi par sécurité. L'achat envoie
// CZ_SE_PC_BUY_CASHITEM_LIST 0x848.
//
// Noms/icônes d'item résolus par id (client itemdb) comme item_desc/storage.

class CashShopTweaks : public Plugin {
 public:
  CashShopTweaks();

  const char* name() const override { return "CashShopTweaks"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Cache la fenêtre native dès sa création (hook MakeWindow de WindowPosTweaks,
  // id 0x13e) -> zéro flicker. No-op si le viewer est désactivé.
  void HideNativeAtCreation(void* win);

  // Setting PERSISTANT (bourgeon_settings.yaml "cashshop_imgui", géré par
  // MoonlightUi) : ON = viewer ImGui + natif caché ; OFF = cash shop natif seul.
  bool imgui_enabled_ = true;

 private:
  static constexpr int kNumTabs = 9;  // e_cash_shop_tab (NEW..SALE)

  struct CashItem {
    uint32_t id = 0;
    int32_t  price = 0;
    uint16_t view = 0;      // viewSprite (viewID), présent si ENABLE_CASHSHOP_PREVIEW_PATCH
    uint32_t location = 0;  // equip location (pc_equippoint) — idem preview patch
  };
  // Une entrée du panier (item choisi en attente d'achat).
  struct CartEntry {
    uint32_t id = 0;
    int      tab = 0;
    int      amount = 1;
    int32_t  price = 0;
  };

  // Ajoute/incrémente un item au panier.
  void AddToCart(uint32_t id, int tab, int32_t price);
  // Envoie l'achat de tout le panier (CZ_SE_PC_BUY_CASHITEM_LIST 0x848).
  void SendBuy();
  // Demande la liste d'items d'un onglet au serveur (CZ_REQ_SE_CASH_TAB_CODE 0x846).
  void RequestTab(int tab);

  bool open_ = false;       // cash shop ouvert ce frame ?
  bool was_open_ = false;   // front montant (placement + requêtes 1re ouverture)
  bool need_pos_ = false;
  bool show_panel_ = true;  // transitoire : détection du clic sur le X
  int  spawn_x_ = 0, spawn_y_ = 0;

  int      cur_tab_ = 0;
  int      cur_slot_ = -1;  // filtre emplacement d'équipement (clé, -1 = tous)
  int      cur_sort_ = 0;   // tri : 0 = Nom, 1 = ID, 2 = Coût
  bool     sort_asc_ = true;
  // Survol d'une image previewable la frame precedente : gele le scroll-molette de
  // la grille pour laisser la molette tourner le perso (basic_info consomme le wheel,
  // mais ImGui scrolle la fenetre AVANT -> il faut couper le scroll en amont).
  bool     preview_active_ = false;
  uint32_t cash_points_ = 0;
  uint32_t kafra_points_ = 0;
  bool     use_kafra_ = false;   // dépenser d'abord les points Kafra
  int      last_result_ = -1;    // dernier ZC_SE_PC_BUY_CASHITEM_RESULT (-1 = aucun)

  // Dernier onglet reçu en 0x08ca : un onglet volumineux (>~2340 items) est
  // DÉCOUPÉ par le serveur en plusieurs paquets 0x08ca consécutifs de même
  // tabNum -> on ACCUMULE tant que tabNum ne change pas, et on ne vide l'onglet
  // qu'au 1er paquet le concernant (tabNum différent du précédent). -1 = aucun.
  int last_recv_tab_ = -1;

  std::vector<CashItem>  tabs_[kNumTabs];   // items par onglet (serveur)
  std::vector<CartEntry> cart_;
};
