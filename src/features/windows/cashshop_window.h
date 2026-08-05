#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "features/plugin.h"

// ── CashShopWindow ───────────────────────────────────────────────────────────
//
// Redraw COMPLET et moderne du cash shop natif ("Simple Cash Shop" =
// UICashShopWnd, id 0x13e, vtable 0x0101ca18). En REMPLACEMENT du natif : dès
// qu'une fenêtre 0x13e est créée on la cache (win+0x28=0, hors rendu + input) et
// on dessine une UI ImGui à la place. Le natif reste vivant côté session (le
// serveur ne voit pas la différence) mais l'utilisateur n'interagit qu'avec le
// viewer.
//
// 🔴 La fenêtre native ne NAÎT PLUS : son unique paquet créateur est REMPLACÉ
// (RegisterReplaceOpcode, révocable par l'interrupteur). Elle était auparavant
// créée puis CACHÉE — or une native masquée garde le CLAVIER, et son bouton par
// défaut agit : ici, ce bouton ACHÈTE.
//
// RE 2026-08-01 (`Recv_ZC_SE_CASHSHOP_OPEN` 0x00D0BC80) : ZC 0x0B6E est le seul
// créateur vivant de la 0x13E. Les cases 0x0845 / 0x0A2B du dispatcher créent la
// même fenêtre mais sont les opcodes HÉRITÉS — moonlight choisit 0x0B6E dès
// PACKETVER_MAIN >= 20200129, il ne les envoie jamais.
//
// ⚠ TROIS choses que ce handler faisait et qu'on hérite :
//   1. il BASCULE — une 0x13E déjà ouverte est DÉTRUITE et le paquet ne rouvre
//      rien (SaveRectAndCloseWindow en tête). Même forme que la banque ;
//   2. il ne pose AUCUNE globale : les points ne vivaient que dans la fenêtre ;
//   3. il n'envoie rien — ni la list-request CZ 0x08C9 (qui partait de l'UI de la
//      fenêtre), ni la fermeture CZ 0x084A. Les deux nous incombent désormais, et
//      la seconde débloque le personnage (`npc_shopid`, cf. CloseShop).
//
// Contenu = 100 % SERVEUR-driven (cf. mémoire project_cashshop_re) :
//   - ZC_SE_CASHSHOP_OPEN 0x0b6e  : cashPoints, kafraPoints, onglet — REMPLACÉ.
//   - ZC_ACK_SCHEDULER_CASHITEM 0x08ca : {count, tabNum, items[]{id, price, ...}}
//     — un paquet par onglet ; c'est le VRAI peuplement sur ce packetver (le
//     couple 0x846/0x8c0 est du code serveur mort ici, cf. .cc).
//   - ZC_SE_PC_BUY_CASHITEM_RESULT 0x0849 : result + points mis à jour.
// Les deux dernières restent OBSERVÉES : le résultat d'achat 0x0849
// (`Recv_ZC_SE_PC_BUY_CASHITEM_RESULT` 0x00CD3B70) est INDÉPENDANT de la fenêtre —
// son FindWindow rend nul et il saute juste le rafraîchissement des points, mais
// il continue d'afficher les messages d'erreur EXACTS du serveur. Le remplacer
// nous obligerait à les réécrire ; l'observer les garde gratuitement.
//
// La liste est déclenchée par la list-request 0x08c9 (2 octets), que le natif
// n'envoie plus puisqu'elle partait de sa fenêtre : c'est CloseShop/l'ouverture qui
// s'en chargent. L'achat envoie CZ_SE_PC_BUY_CASHITEM_LIST 0x848.
//
// Noms/icônes d'item résolus par id (client itemdb) comme item_desc/storage.

class CashShopWindow : public Plugin {
 public:
  CashShopWindow();

  const char* name() const override { return "CashShopWindow"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // ── « Cet objet est-il vendu au vote shop ? » ──────────────────────────────
  //
  // Le catalogue reçu du serveur n'était consulté que par cette fenêtre. C'est
  // pourtant la réponse à une question qu'on se pose ailleurs — devant un lien
  // d'objet dans le chat, devant une table de drops : « est-ce que je peux
  // simplement l'acheter ? ». D'où ces deux entrées publiques.
  //
  // 🔴 Une absence ne prouve RIEN tant que le catalogue n'est pas arrivé. Il ne
  // l'est que sur demande (CZ 0x08C9), et le serveur ne répond qu'UNE fois par
  // session (`cashshop_sent`) : c'est `OnTick` qui la pose à l'entrée en jeu,
  // sans quoi rien ne serait connu avant la première ouverture de la boutique.
  // Les appelants doivent donc traiter `false` comme « on ne sait pas », et
  // n'afficher une disponibilité que quand elle est POSITIVE.
  //
  // `tab` et `price` sont facultatifs (nullptr accepté). Les onglets masqués
  // (kTabShown) sont ignorés : on ne renvoie pas une adresse où l'on ne peut
  // pas emmener le joueur.
  bool FindItem(uint32_t id, int* tab, int32_t* price) const;

  // Ouvre le vote shop sur l'objet : bon onglet, filtre posé sur son nom, et une
  // unité au panier. Ne fait rien (et renvoie false) si l'objet n'y est pas.
  //
  // ⚠ L'ouverture n'est PAS immédiate quand la boutique est fermée : c'est le
  // serveur qui ouvre (on lui envoie CZ 0x0B6D, il répond ZC 0x0B6E). L'objet
  // attend dans `pending_item_` jusque-là.
  bool OpenWithItem(uint32_t id);

  // Setting PERSISTANT (bourgeon_settings.yaml "cashshop_imgui", géré par
  // MoonlightUi) : ON = viewer ImGui + natif caché ; OFF = cash shop natif seul.
  // Basculé en GROUPE par SetModernInterface (moonlight_ui.h) — PLUS de case isolée
  // dans le panneau : ce qu'on achète atterrit dans l'inventaire, un panier moderne
  // au-dessus d'un inventaire natif (ou l'inverse) est le mixe qu'on a supprimé.
  bool imgui_enabled_ = false;

 private:
  // 🔴 Le décodage, sur le FIL PRINCIPAL. OnRecvPacket (fil réseau) ne fait que
  // copier : les neuf listes d'onglets étaient vidées et re-remplies pendant que le
  // rendu parcourait l'onglet courant. Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

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
  // Achat 1-clic : achète 1 unité de l'item immédiatement puis FERME le shop
  // (paquet 0x848 count=1 + fermeture CZ 0x084a + destruction fenêtre native).
  void BuyNow(uint32_t id, int tab, int32_t price);
  // Demande le catalogue complet au serveur (CZ 0x08C9, 2 octets). Le serveur
  // répond par une série de ZC 0x08CA, un paquet par onglet — et une seule fois
  // par session.
  void RequestCatalogue();
  // Amène la vue sur un objet déjà présent dans `tabs_` : onglet, filtre, panier.
  void RevealItem(uint32_t id, int tab, int32_t price);
  // Ferme la session d'achat côté SERVEUR (CZ 0x084A) puis chez nous. 🔴 Le paquet
  // n'est pas décoratif : sans lui le personnage reste bloqué (cf. le .cc).
  void CloseShop();

  bool open_ = false;       // cash shop ouvert ? (posé par ZC 0x0B6E, plus par la native)
  bool was_open_ = false;
  bool need_pos_ = false;
  bool show_panel_ = true;  // transitoire : détection du clic sur le X
  // Position de départ. Elle venait de la fenêtre native, qui ne naît plus : ImGui
  // retient la sienne dès la première ouverture (ImGuiCond_FirstUseEver).
  int  spawn_x_ = 120, spawn_y_ = 80;
  // Détection du basculement de l'interrupteur (les deux sens ferment le shop).
  bool prev_imgui_enabled_ = false;

  int      cur_tab_ = 0;
  // Onglet à SÉLECTIONNER de force à la prochaine frame (-1 = aucun). Écrire
  // `cur_tab_` ne suffit pas : la barre d'onglets ImGui tient sa propre sélection
  // et la réimposerait aussitôt. Il faut lui passer ImGuiTabItemFlags_SetSelected.
  int      force_tab_ = -1;
  // Objet à révéler dès que la boutique s'ouvre (0 = aucun). Cf. OpenWithItem :
  // c'est le serveur qui ouvre, on ne peut donc pas agir sur la vue tout de suite.
  uint32_t pending_item_ = 0;
  // Catalogue déjà demandé pour CETTE session ? Remis à zéro quand le monde de
  // jeu s'arrête (changement de personnage, reconnexion), parce que le serveur
  // remet `cashshop_sent` à zéro au même moment (pc.cpp).
  bool     catalogue_requested_ = false;
  bool     prev_game_active_ = false;
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
