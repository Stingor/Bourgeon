#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugins/plugin.h"

// ── ShopTweaks ───────────────────────────────────────────────────────────────
//
// Remplacement ImGui COMPLET de l'interaction shop NPC (achat / vente). En
// REMPLACEMENT du natif : on SAUTE la fenêtre "Acheter / Vendre / Annuler" et on
// atterrit directement sur une fenêtre unifiée à deux onglets (Achat | Vente),
// avec un bouton pour basculer. Les fenêtres natives (UIChooseSellBuyWnd 0x19,
// UIItemPurchaseWnd 0x16, UIItemSellWnd 0x17) sont cachées dès leur création.
//
// RE complète : cf. mémoire project_npc_shop_re. Résumé du pipeline paquet
// (PACKETVER client == serveur == 20250716, vérifié) :
//   Serveur ZC_SELECT_DEALTYPE 0xc4 {npcId:4}         -> ouvre le shop.
//   Client  CZ_ACK_SELECT_DEALTYPE 0xc5 {GID:4,type:1} -> demande la liste
//           (type 0 = achat, 1 = vente). npc_shopid reste valide tant qu'on est
//           près du NPC -> on peut re-demander achat/vente librement (serveur
//           npc_buysellsel, aucun verrou ; la vente vide npc_shopid côté serveur,
//           on re-arme au besoin).
//   Serveur ZC_PC_PURCHASE_ITEMLIST 0x0b77 (var) sub {itemId:4,price:4,
//           discountPrice:4,itemType:1,viewSprite:2,location:4}  -> liste achat.
//   Serveur ZC_PC_SELL_ITEMLIST 0xc7 (var) sub {index:2,price:4,overcharge:4}.
//   Client  CZ_PC_PURCHASE_ITEMLIST 0xc8 (var) sub {amount:2,itemId:4} -> achat.
//   Client  CZ_PC_SELL_ITEMLIST 0xc9 (var) sub {index:2,amount:2}      -> vente.
//   Serveur ZC_PC_PURCHASE_RESULT 0xca / ZC_PC_SELL_RESULT 0xcb {result:1}.
//
// Le serveur valide TOUTE transaction (zeny/poids/stock) -> aucun risque d'exploit ;
// une requête invalide renvoie juste un code d'échec.
//
// Sources d'affichage :
//   - ACHAT  : on parse directement 0x0b77 (itemId+prix) ; nom/icône résolus par
//     id (client itemdb, comme cashshop/item_desc).
//   - VENTE  : on lit la liste RÉSOLUE de la fenêtre native de vente cachée
//     (UIItemSellWnd id 0x17, std::list @+0xe8 : nom/qté/prix/index déjà calculés
//     par le handler natif) — pattern StorageTweaks, réutilise la résolution native.

class ShopTweaks : public Plugin {
 public:
  ShopTweaks();  // enregistre les opcodes observés (0xc4/0x0b77/0xc7/0xca/0xcb)

  const char* name() const override { return "ShopTweaks"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Cache la fenêtre native (hook MakeWindow de WindowPosTweaks) : ids 0x16
  // (achat), 0x17 (vente), 0x18, 0x19 (chooser). No-op si le viewer est désactivé.
  void HideNativeAtCreation(void* win);

  // Cache le comparateur de stats ATK/DEF (UIItemParamChangeDisplayWnd), créé par
  // le handler d'achat natif avec un id variable -> détecté par VTABLE. Appelé pour
  // toute fenêtre créée pendant une session shop (hook MakeWindow).
  void HideDetailWindow(void* win);

  // Setting PERSISTANT (bourgeon_settings.yaml "shop_imgui", géré par MoonlightUi) :
  // ON = viewer ImGui + natif caché ; OFF = shop natif d'origine (chooser inclus).
  // Défaut OFF : opt-in — on n'impose pas un changement de gameplay aux joueurs.
  // Basculé en GROUPE par SetModernInterface (moonlight_ui.h) — PLUS de case isolée
  // dans le panneau : l'onglet Vendre travaille DEPUIS l'inventaire, moderne d'un
  // côté et natif de l'autre les objets ne se glissent plus.
  bool imgui_enabled_ = false;

 private:
  enum Mode { kBuy = 0, kSell = 1 };

  struct BuyItem {
    uint32_t id = 0;
    int32_t  price = 0;         // prix plein
    int32_t  discount = 0;      // prix remisé (Discount) — ce qu'on paie
    uint8_t  type = 0;
    uint16_t view = 0;          // viewSprite (aperçu)
    uint32_t location = 0;      // masque d'équipement (aperçu/filtre)
  };
  struct SellItem {
    int      index = 0;         // index inventaire (pour CZ_PC_SELL_ITEMLIST)
    uint32_t id = 0;            // itemId résolu (icône/nom)
    int      amount = 0;        // quantité possédée
    int32_t  price = 0;         // prix de vente unitaire (overcharge inclus) = final
    int32_t  base_price = 0;    // prix de vente de base (avant Overcharge) — affichage base->final
    int      slots = 0;
    char     name[64] = {0};
  };
  // Une ligne de panier (achat ou vente selon l'onglet actif).
  struct CartEntry {
    uint32_t id = 0;      // itemId (achat) ou itemId résolu (vente, info)
    int      index = 0;   // index inventaire (vente uniquement)
    int      amount = 1;
    int32_t  price = 0;   // unitaire (achat = discount, vente = price)
    int      max = 30000; // borne quantité (vente = qté possédée ; achat = stack)
  };

  // Envoi CZ_ACK_SELECT_DEALTYPE 0xc5 (demande la liste achat/vente).
  void RequestList(Mode mode);
  // Re-arme la selection de deal (CZ 0xc5 ; type 0 = achat, 1 = vente). Le serveur
  // EFFACE sd->npc_shopid apres CHAQUE 0xc8/0xc9 (clif_parse_NpcBuy/SellListSend :
  // "npc_shopid = 0" ; le shop natif se ferme apres une transaction). Notre viewer
  // reste ouvert -> il FAUT re-selectionner juste avant chaque transaction, sinon la
  // 2e (et les suivantes) echouent avec PURCHASE_FAIL_MONEY (trompeur "pas de zeny").
  void SendDealSelect(uint8_t type);
  // Envoi de l'achat (CZ_PC_PURCHASE_ITEMLIST 0xc8) / vente (CZ 0xc9) du panier.
  void SendBuy();
  void SendSell();
  // Ferme réellement le shop : détruit les fenêtres natives 0x16/0x17/0x19.
  void CloseNativeShop();
  // Recharge sell_items_ depuis la fenêtre native de vente cachée (SEH, POD).
  void RefreshSellFromNative();
  void AddToCart(uint32_t id, int index, int32_t price, int max, int qty = 1);
  // Transaction IMMEDIATE (bypass panier) : achat CZ 0xc8 / vente CZ 0xc9 a 1 item
  // de `qty` unites. Declenchee par Ctrl+clic sur les boutons quantite.
  void QuickBuy(uint32_t id, int qty);
  void QuickSell(int index, int qty);

  bool     open_ = false;        // shop ouvert ce frame ?
  bool     was_open_ = false;
  bool     need_pos_ = false;
  bool     show_panel_ = true;   // détection du clic sur le X
  int      spawn_x_ = 100, spawn_y_ = 100;
  uint32_t npc_id_ = 0;          // GID du NPC courant (pour 0xc5)
  int      cur_mode_ = kBuy;     // onglet actif
  bool     buy_requested_ = false;   // 0xc5(0) déjà envoyé cette session ?
  bool     sell_requested_ = false;  // 0xc5(1) déjà envoyé cette session ?
  bool     have_buy_ = false;        // liste achat reçue ?
  int      last_result_ = -1;        // dernier 0xca/0xcb (-1 = aucun)
  bool     last_result_sell_ = false;
  // "Tout ajouter au panier" (vente) arme la fermeture AUTO du shop une fois la
  // liste vendue (fonctionnalite dump-tout-et-quitte).
  bool     sell_all_close_ = false;  // arme par "Tout ajouter au panier"
  bool     want_close_ = false;      // fermeture differee (0xcb thread recv -> OnTick)
  bool     map_changed_ = false;     // 0x0091/0x0092 recu (warp) -> fermer le viewer (OnTick)

  // Tri / filtre partagés.
  int  cur_sort_ = 0;   // 0 = Nom, 1 = ID, 2 = Prix
  bool sort_asc_ = true;

  std::vector<BuyItem>   buy_items_;
  std::vector<SellItem>  sell_items_;
  std::vector<CartEntry> cart_;   // panier de l'onglet courant (vidé au switch)
  // Nom des NPC par GID (observé via ZC_ACK_REQNAMEALL_NPC 0x0adf) -> titre.
  std::unordered_map<uint32_t, std::string> npc_names_;
};
