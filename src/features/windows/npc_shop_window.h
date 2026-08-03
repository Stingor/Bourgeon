#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "features/plugin.h"

// ── NpcShopWindow ───────────────────────────────────────────────────────────────
//
// Remplacement ImGui COMPLET de l'interaction shop NPC (achat / vente). On SAUTE
// la fenêtre "Acheter / Vendre / Annuler" et on atterrit directement sur une
// fenêtre unifiée à deux onglets (Achat | Vente).
//
// 🔴 Les fenêtres natives (chooser 0x19, achat 0x16, vente 0x17, comparateur 0x18)
// ne NAISSENT PLUS : les trois paquets qui les ouvrent sont REMPLACÉS
// (RegisterReplaceOpcode, révocable par l'interrupteur). Elles étaient auparavant
// créées puis masquées — or une native masquée garde le CLAVIER, et son bouton par
// défaut valide une transaction.
//
// Ce que le handler remplacé de 0x00c4 faisait et qu'on reprend : `GameMode+0x24C
// = 1`, l'état « interaction NPC en cours » du client (rag::SetNpcInteractionActive,
// cf. ragnarok/globals.h). Sans lui le joueur pourrait marcher pendant la boutique,
// et CloseNativeShop n'aurait plus rien à débloquer.
//
// 🔴 Ce qu'il ne faisait PAS, et qu'il ne faut donc pas faire : écrire
// `GameMode+0x2DC`. Ce champ porte le GID de la CONVERSATION — le NPC dont le
// script tourne, donc sd->npc_id côté serveur — et une boutique EST une interaction
// NPC : elle s'ouvre depuis un script, souvent au nom d'un AUTRE NPC que le
// marchand annoncé par ZC_SELECT_DEALTYPE. Y ranger le GID de la boutique effaçait
// l'identité du script, et la fermeture partait alors au mauvais NPC : le serveur la
// rejetait (npc_scriptcont) et le joueur restait bloqué, aussi bien au clic sur la
// croix qu'au basculement de l'interrupteur. CloseNativeShop ferme donc les DEUX
// GID quand ils diffèrent.
//
// 🔴 Plus AUCUN pilotage du natif, dans les deux sens de l'interrupteur :
//   - la vente ne fait plus naître la fenêtre 0x17 pour lire sa liste (cmd 0x25),
//     elle demande la liste au serveur et la parse ;
//   - la fermeture ne délègue plus au CANCEL natif (cmd 0x28) : elle envoie
//     CZ_NPC_TRADE_QUIT puis CZ_CLOSE_DIALOG, éteint l'état client et vide le
//     panier, chacun explicitement.
//
// 🔴 Fermer une boutique demande DEUX paquets, pas un. `CZ_NPC_TRADE_QUIT 0x09D4`
// (2 octets, l'opcode seul) est celui qui DÉBLOQUE le personnage : côté serveur il
// remet `sd->npc_shopid` à zéro, et `pc_cant_act2()` teste ce champ — tant qu'il est
// posé, clif_parse_WalkToXY refuse tout déplacement. `CZ_CLOSE_DIALOG` n'y touche
// jamais : il termine le SCRIPT (sd->npc_id). Le natif les émet par deux sélecteurs
// distincts de CMode::SendMsg (291 -> 0x09D4, 0x59 -> 0x0146), ce qui explique qu'on
// ait pu croire longtemps que l'annulation du chooser suffisait.
// Ce dernier point demandait de savoir lire le GID d'une boutique ouverte AVANT
// l'allumage de l'interface moderne — RE de UIChooseSellBuyWnd_OnMsg (0x008BE7B0),
// dont le `case 0x1C` range le npcId à **fenêtre+0xB4**. Sans lui, on gardait du
// natif vivant dans le seul but de se débarrasser du natif.
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
// Sources d'affichage — les DEUX viennent des paquets, plus aucune fenêtre native :
//   - ACHAT  : 0x0b77 (itemId + prix) ; nom/icône résolus par id (itemdb client).
//   - VENTE  : 0x00c7, qui ne porte QUE {index, prix, overcharge}. C'est le
//     SERVEUR qui décide du vendable — Moonlight y ajoute ses filtres (ni cartes,
//     ni objets sertis, ni raffinés, groupes IG_SELLSTUFF/IG_SELLITEM, flèches
//     selon la classe) — donc la liste ne se déduit PAS de l'inventaire. L'objet
//     lui-même (id, quantité, emplacements, nom composé) est retrouvé par index
//     dans le modèle session de l'inventaire, la source que le natif consultait
//     lui aussi pour remplir sa fenêtre.
//
// ⚠ Le natif ne se contentait PAS de recopier la liste du serveur : son
// constructeur (NpcSell_BuildSellableList 0x00CD0F00) écartait les FAVORIS quand le
// verrou de vente est posé — `if (trouvé && (!favori || !g_inv_dealLock))`. Ce
// verrou (bouton « Deal » du pied de l'inventaire, octet client 0x01600553) est
// purement client : le serveur l'ignore. En prenant la place du handler, on doit
// donc reprendre ce filtre nous-mêmes, sinon plus personne ne le lit et les favoris
// redeviennent vendables alors que le joueur les croit protégés.

class NpcShopWindow : public Plugin {
 public:
  NpcShopWindow();  // enregistre les opcodes observés (0xc4/0x0b77/0xc7/0xca/0xcb)

  const char* name() const override { return "NpcShopWindow"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Masque le comparateur de stats ATK/DEF (UIItemParamChangeDisplayWnd, id 0x32),
  // que la fenêtre d'ACHAT native ouvre quand on sélectionne un équipement. Détecté
  // par VTABLE parce qu'on le voit passer dans le hook MakeWindow, avant qu'il ait
  // un id dans la map. Appelé pour toute fenêtre créée pendant une session shop.
  // ⚠ Ne fait que MASQUER (destruction impossible depuis MakeWindow) : c'est
  // PurgeNativeShopWindows qui le détruit au tick.
  void HideDetailWindow(void* win);

  // Setting PERSISTANT (bourgeon_settings.yaml "shop_imgui", géré par MoonlightUi) :
  // ON = viewer ImGui + natif caché ; OFF = shop natif d'origine (chooser inclus).
  // Défaut OFF : opt-in — on n'impose pas un changement de gameplay aux joueurs.
  // Basculé en GROUPE par SetModernInterface (moonlight_ui.h) — PLUS de case isolée
  // dans le panneau : l'onglet Vendre travaille DEPUIS l'inventaire, moderne d'un
  // côté et natif de l'autre les objets ne se glissent plus.
  bool imgui_enabled_ = false;

 private:
  // 🔴 Le décodage des paquets, sur le FIL PRINCIPAL. OnRecvPacket (fil réseau) ne
  // fait que copier les octets : la liste d'achat, celle de vente et le panier
  // étaient reconstruits pendant que le rendu les parcourait.
  // Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  enum Mode { kBuy = 0, kSell = 1 };

  struct BuyItem {
    uint32_t id = 0;
    int32_t  price = 0;         // prix plein
    int32_t  discount = 0;      // prix remisé (Discount) — ce qu'on paie
    uint8_t  type = 0;
    uint16_t view = 0;          // viewSprite (aperçu)
    uint32_t location = 0;      // masque d'équipement (aperçu/filtre)
  };
  // Une entrée de ZC_PC_SELL_ITEMLIST, brute. Le paquet ne dit QUE l'index et les
  // deux prix : ni l'objet, ni la quantité, ni les emplacements — c'est
  // l'inventaire qui les porte (cf. ResolveSellItems).
  struct SellRaw {
    int     index = 0;
    int32_t price = 0;       // tarif de base
    int32_t overcharge = 0;  // ce que le marchand paie vraiment (compétence)
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
  // Ferme réellement le shop : débloque l'état dialogue client + prévient le serveur.
  void CloseNativeShop();
  // L'une des quatre fenêtres natives est-elle à l'écran ? Sert à reconnaître une
  // boutique NATIVE en cours au moment où l'on allume l'interface moderne — la
  // seule façon de le savoir, puisque aucun paquet de cette session n'est passé
  // par nous.
  bool AnyNativeShopWindow() const;
  // Détruit les quatre fenêtres natives si l'une traîne. Elles ne naissent plus
  // (leurs paquets d'ouverture sont remplacés) : il n'en reste qu'après un
  // basculement d'interrupteur en pleine session. On les DÉTRUIT plutôt que de les
  // masquer — masquée, une native garde le clavier.
  void PurgeNativeShopWindows();
  // Complète les entrées brutes de 0x00c7 avec l'objet correspondant, lu dans le
  // modèle session de l'inventaire. Thread PRINCIPAL (name-builder natif).
  void ResolveSellItems();
  // Retire du panier de vente ce qui n'est plus dans la liste (verrou favoris posé
  // après coup, objet consommé…). Séparée de ResolveSellItems, dont le __try
  // interdit tout objet local à déroulement (C2712).
  void PruneSellCart();
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
  // Détection du basculement de l'interrupteur. Éteindre en pleine boutique doit
  // FERMER la session (serveur + client), pas seulement cesser d'afficher : la
  // fenêtre native qui reprenait la main autrefois ne naît plus.
  bool     prev_imgui_enabled_ = false;

  // Tri / filtre partagés.
  int  cur_sort_ = 0;   // 0 = Nom, 1 = ID, 2 = Prix
  bool sort_asc_ = true;

  std::vector<BuyItem>   buy_items_;
  std::vector<SellItem>  sell_items_;
  // Entrées de vente TELLES QUE LE SERVEUR LES ENVOIE (0x00c7), avant croisement
  // avec l'inventaire. Séparées parce que les deux moitiés n'arrivent pas au même
  // endroit : le paquet est lu sur le fil réseau, la résolution appelle le
  // name-builder natif et attend donc le thread principal.
  std::vector<SellRaw>   sell_raw_;
  bool sell_dirty_ = false;   // une salve reçue attend sa résolution
  // Dernier état lu du verrou « ne pas vendre les favoris » (octet client
  // 0x01600553, bouton Deal du pied de l'inventaire). Le joueur peut le basculer
  // boutique ouverte : on redemande alors une résolution de la liste.
  bool prev_deal_lock_ = false;
  std::vector<CartEntry> cart_;   // panier de l'onglet courant (vidé au switch)
  // Nom des NPC par GID (observé via ZC_ACK_REQNAMEALL_NPC 0x0adf) -> titre.
  std::unordered_map<uint32_t, std::string> npc_names_;
};
