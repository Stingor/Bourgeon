#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
#include "ragnarok/packets.h"  // rag::zc::kNpcName / rag::cz::kCloseDialog
#include "ragnarok/item_info.h"  // rag::itemlist : le layout du noeud
#include "features/windows/npc_shop_window.h"

// Icônes d'item : ro::ItemIcon (ui/icon_cache.h). Le chargement, le colorkey
// magenta et l'invalidation au reset de device y sont partagés — ce fichier en
// gardait sa propre copie, comme cinq autres plugins.
#include "ui/icon_cache.h"
#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "features/item_cell.h"  // itemcell::OpenDescById, itemcell::TypeIsStackable
#include "ragnarok/msgstring.h"  // msgstr::Cp949 (MSI_EQUIPITEM_OLNY_ONE)
#include "ragnarok/ui_window_mgr.h"  // UIM_PUSHINTOCHATHISTORY (refus au chat)
#include "imgui.h"
#include "ui/imgui_escape.h"
#include "ui/ro_imgui.h"          // BeginRoWindow (skin RO)
#include "ui/ro_widgets.h"        // mui::IsLastItemRightClicked
#include "utils/i18n.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ui/ui_palette.h"  // ro::pal : la palette de l'UI

// ── Constantes RE (client 20250716, base 0x400000 ; cf. project_npc_shop_re) ──
namespace {

// ── Les fenêtres natives de la boutique (cf. project_npc_shop_re) ────────────
// Les cinq identifiants, leurs vtables et la façon dont la carte a été PROUVÉE
// sont au foyer, sous « Comment la famille 22-25 / 50 a été PROUVÉE ».
//
// Ce qui gouverne le code ICI : `kUIItemShopWnd` est le CADRE, et le client
// échange le panneau intérieur — `kUIItemPurchaseWnd` en achat,
// `kUIItemSellWnd` en vente. D'où la purge des DEUX : celui qui n'est pas à
// l'écran reste vivant.

// Champ du chooser où atterrit le npcId (RE : son OnMsg case 0x1C, cf. ChooserNpcId).
constexpr int kChooserNpcId = 0xb4;

// (Plus de kSellListVTable/kSellListGlobal ni d'offsets de nœud d'affichage : la
// liste de vente ne se lit plus dans la fenêtre native — elle n'existe plus. Elle
// vient du paquet 0x00c7, croisé avec le modèle session ci-dessous.)

// Champs de l'ItemSkillInfo utilisés ici — la disposition est au foyer,
// `rag::itemlist` (ragnarok/item_info.h).
using rag::itemlist::kInfoIdStr;

// ── Verrou de vente des favoris ──────────────────────────────────────────────
// Bouton « Deal » du pied de l'inventaire (onglet Favoris) : 100 % CLIENT, aucun
// paquet, un simple octet. Le natif le consultait dans le constructeur de la liste
// de vente — NpcSell_BuildSellableList 0x00CD0F00, celui-là même dont on a pris la
// place : `if (trouve && (!favori || !g_inv_dealLock))`. En le remplaçant on a
// emporté le filtre avec lui : le verrou basculait toujours, mais plus personne ne
// le lisait, et les favoris réapparaissaient dans la liste — vendables.
bool DealLockOn() {
  __try { return *reinterpret_cast<uint8_t*>(rag::kFavoriteModeFlagAddr) != 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── Combien d'un même objet peut-on acheter d'un coup ? ──────────────────────
// Le marchand NPC a un stock illimité, mais deux plafonds s'appliquent quand
// même :
//
//   - EMPILABLE     : 30000, la taille de pile du client (le natif clampe là,
//                     UIItemPurchaseWnd_OnMsg case 38 : `if (v28 > 30000)` +
//                     MsgString 0x6A8) ;
//   - NON EMPILABLE : 1, et c'est le défaut corrigé ici. Rien ne bornait la
//                     quantité : cinq armes au panier partaient bien en CZ 0xc8
//                     avec amount=5, et le serveur les ramenait à UNE seule en
//                     journalisant « sent a hexed packet trying to buy 5 of
//                     nonstackable item 1247 » (npc_buylist, npc.cpp) — le
//                     joueur payait une arme et croyait en avoir commandé cinq.
//
// Le natif tient la même règle autrement : il autorise UNE ligne de panier par
// équipement et refuse le doublon (MSI_EQUIPITEM_OLNY_ONE). Ici la ligne porte sa
// quantité, donc la borne est la quantité elle-même — même résultat, sans
// interdire au joueur de corriger une ligne existante.
constexpr int kBuyStackMax = 30000;
int BuyStackMax(uint8_t type) {
  return itemcell::TypeIsStackable(type) ? kBuyStackMax : 1;
}

// « Please avoid buying 2 of the same items at one time. » — MSI_EQUIPITEM_OLNY_ONE,
// le message que le natif écrit dans le chat quand on tente d'ajouter un second
// équipement identique au panier. On reprend le libellé du client plutôt qu'une
// paraphrase (règle du projet, cf. ragnarok/msgstring.h).
constexpr int kMsgEquipOnlyOne = 0x77;
// ⚠ Appelée depuis le rendu (AddToCart). C'est bien une commande native, mais la
// seule qui soit sans danger là : UIM_PUSHINTOCHATHISTORY empile une ligne, il
// n'ouvre aucune modale — le DPS meter l'émet depuis son OnRenderUI depuis des
// mois. La règle « pas de commande native en pleine frame ImGui » vise les
// chemins qui peuvent atteindre UIWndMgr_ShowMessageBoxModal ; celui-ci ne le
// peut pas. Quand la chatbox ImGui est active, la ligne ne sort même pas de chez
// nous (Bourgeon::RouteChatLine la prend au passage).
void SayEquipOnlyOne() {
  const char* text = msgstr::Cp949(kMsgEquipOnlyOne);  // CP949 : ce qu'attend le chat
  if (text == nullptr || *text == '\0') return;
  UIWindowMgr::SendMsg(UIMessage::UIM_PUSHINTOCHATHISTORY,
                       reinterpret_cast<int>(text), 0, 0, 0);
}

// Icône d'item (image d'inventaire).

// GID du NPC d'une session shop ouverte AVANT nous, lu dans le chooser natif.
//
// 🔴 RE 2026-08-01 (UIChooseSellBuyWnd_OnMsg 0x008BE7B0) : son `case 28` — le
// message 0x1C que le handler de ZC_SELECT_DEALTYPE lui envoie juste après
// MakeWindow — fait `*((_DWORD *)this + 45) = p3`, soit **fenêtre+0xB4 = npcId**.
// C'est le même champ que relisent ses boutons Acheter (223) et Vendre (224).
//
// Sans cette lecture, fermer une boutique native ouverte avant l'allumage de
// l'interface moderne obligeait à passer par le CANCEL natif (cmd 0x28) faute de
// connaître le GID : on gardait donc du natif vivant pour se débarrasser du natif.
uint32_t ChooserNpcId() {
  __try {
    uint8_t* w = reinterpret_cast<uint8_t*>(
        uiwnd::SafeFindWindow(uiwnd::kUIChooseSellBuyWnd));
    if (!w) return 0;
    return *reinterpret_cast<uint32_t*>(w + kChooserNpcId);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Nom d'item par id : itemcell::NameById (DB de descriptions du client, cache
// partagé). La boutique NPC ne tient QUE des ids — pas d'ItemSkillInfo, donc pas
// de refine ni de cartes à composer.


// La description passe par itemcell::OpenDescById : la boutique n'a que des ids
// (les items ne sont pas encore à nous), donc pas de nœud d'inventaire à passer.
// À la VENTE on transmet view/location = 0 : le bouton « aperçu » reste
// verrouillé, ce qui est voulu, et la description elle-même reste correcte.

// Résout l'itemId d'un payload ItemSkillInfo (pour l'icône côté vente). Le
// payload porte l'id sous forme resname -> on relit son nom via la DB par id
// stocké ; ici on lit le nom std::string du nœud (déjà résolu par le natif) et on
// affiche une icône via l'itemId reconstruit par ctor+SetId serait coûteux : on
// se contente du nom natif pour la vente (icône best-effort par id si dispo).

// Opcodes shop (vanilla ; < 0x0C35 -> handler natif intact, on OBSERVE).
constexpr uint16_t kOpChoose   = 0x00c4;  // ZC_SELECT_DEALTYPE {npcId:4}
constexpr uint16_t kOpBuyList  = 0x0b77;  // ZC_PC_PURCHASE_ITEMLIST (var)
constexpr uint16_t kOpSellList = 0x00c7;  // ZC_PC_SELL_ITEMLIST (var)
constexpr uint16_t kOpBuyRes   = 0x00ca;  // ZC_PC_PURCHASE_RESULT {result:1}
constexpr uint16_t kOpSellRes  = 0x00cb;  // ZC_PC_SELL_RESULT {result:1}
// ZC_ACK_REQNAMEALL_NPC (0x0adf) est aussi lu par la fenetre de dialogue : il
// vit chez `rag::zc` (ragnarok/packets.h).
// Envois (CZ).
constexpr uint16_t kOpDealAck  = 0x00c5;  // CZ_ACK_SELECT_DEALTYPE {GID:4,type:1}
constexpr uint16_t kOpBuyReq   = 0x00c8;  // CZ_PC_PURCHASE_ITEMLIST {amount:2,itemId:4}*
constexpr uint16_t kOpSellReq  = 0x00c9;  // CZ_PC_SELL_ITEMLIST {index:2,amount:2}*
// CZ_CLOSE_DIALOG (0x0146) : ferme la session NPC serveur. Aussi emis par la
// fenetre de dialogue -> chez `rag::cz`.
// CZ_NPC_TRADE_QUIT {op:2} — 2 octets, l'opcode seul. 🔴 C'EST LUI qui débloque le
// personnage, et rien d'autre : côté serveur clif_parse_NPCShopClosed ne fait qu'une
// chose, `sd->npc_shopid = 0`, et `pc_cant_act2()` teste `|| sd->npc_shopid` —
// clif_parse_WalkToXY refuse donc tout déplacement tant qu'il est posé.
// CZ_CLOSE_DIALOG ne touche JAMAIS ce champ. Côté client c'est le sélecteur 291 de
// CMode::SendMsg (0x00C875E7) qui l'émet, à part de l'annulation du chooser.
constexpr uint16_t kOpShopQuit = 0x09d4;

// ShopCart_ResetAll(contexte de session) : vide le panier natif. Le chooser
// l'appelle lui-même en fin de CANCEL (son OnMsg case 6).
constexpr uintptr_t kShopCartResetAll = 0x00d55f80;

// CZ_CLOSE_DIALOG pour un GID donné (0 = rien à fermer). Fonction libre, et pas une
// lambda dans CloseNativeShop : celle-ci contient un __try, que MSVC refuse de voir
// cohabiter avec un objet local à déroulement.
void SendCloseDialog(uint32_t gid) {
  if (gid == 0) return;
  uint8_t pkt[6];
  *reinterpret_cast<uint16_t*>(pkt + 0) = rag::cz::kCloseDialog;
  *reinterpret_cast<uint32_t*>(pkt + 2) = gid;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

}  // namespace

NpcShopWindow::NpcShopWindow() {
  // ── Les trois paquets qui OUVRENT une fenêtre : on prend leur place ─────────
  //
  // 🔴 REMPLACEMENT. En observant, les quatre fenêtres natives (chooser 0x19,
  // achat 0x16, vente 0x17, comparateur 0x18) naissaient à chaque visite chez un
  // marchand et on les masquait ensuite ; or une native masquée garde le CLAVIER,
  // et son bouton par défaut valide une transaction. Aucune ne naît plus.
  //
  // Le prédicat est relu à chaque paquet : interrupteur éteint, le shop natif
  // reprend la main entièrement, chooser compris.
  //
  // ⚠ L'ancien commentaire disait ici que le remplacement était impossible parce
  // que « l'interception patche la table de dispatch en permanence, sans
  // dé-registration ». C'est ce que RegisterReplaceOpcode a résolu : le slot reste
  // patché, mais le prédicat rend la main au handler natif d'origine, paquet par
  // paquet.
  const auto claim = [this] { return imgui_enabled_; };
  Bourgeon::Instance().RegisterReplaceOpcode(kOpChoose, claim);   // {npcId:4}
  Bourgeon::Instance().RegisterReplaceOpcode(kOpBuyList, claim);  // liste achat (var)
  Bourgeon::Instance().RegisterReplaceOpcode(kOpSellList, claim); // liste vente (var)
  // Les deux RÉSULTATS restent OBSERVÉS : leur handler natif affiche les messages
  // d'échec du client (« pas assez de zeny »…), qu'on aurait sinon à réécrire, et
  // son rafraîchissement d'UI ne trouve aucune fenêtre — donc ne fait rien.
  Bourgeon::Instance().RegisterObserveOpcode(kOpBuyRes, 1);   // result
  Bourgeon::Instance().RegisterObserveOpcode(kOpSellRes, 1);  // result
  // Nom des NPC (pour le titre) : gid:4 + groupId:4 + name[24] = 32 octets utiles.
  Bourgeon::Instance().RegisterObserveOpcode(rag::zc::kNpcName, 32);
  // Changement de map / serveur (@load, warp) : le warp invalide la session shop
  // cote serveur (npc_shopid=0) -> on ferme le viewer pour ne pas laisser une
  // fenetre orpheline. On ne lit pas le payload, juste la reception.
  Bourgeon::Instance().ObserveWarpPackets();
}

// Fil RÉSEAU : on COPIE, rien de plus (cf. features/net_inbox.h). La liste d'achat
// est à longueur ANNONCÉE et son décodage lit tout le corps, bien au-delà des
// octets transmis : PushAnnounced.
void NpcShopWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                 uint16_t len) {
  if (!imgui_enabled_) return;
  if (opcode == kOpBuyList || opcode == kOpSellList)
    net_inbox_.PushAnnounced(opcode, data, len);
  else
    net_inbox_.Push(opcode, data, len);
}

// Fil PRINCIPAL : le décodage, rejoué à chaque frame, dans l'ordre d'arrivée.
void NpcShopWindow::HandlePacket(uint16_t opcode, const uint8_t* data,
                                 uint16_t len) {
  if (Bourgeon::IsWarpPacket(opcode)) {
    // Warp / changement de map -> la session shop est morte cote serveur. On ferme
    // le viewer au prochain OnTick (thread principal ; jamais depuis le thread recv).
    map_changed_ = true;
    return;
  }

  if (opcode == rag::zc::kNpcName) {
    // ZC_ACK_REQNAMEALL_NPC : gid@+0, name[24]@+8. Cache pour le titre.
    if (len < 32) return;
    const uint32_t gid = *reinterpret_cast<const uint32_t*>(data);
    char nm[25] = {0};
    std::memcpy(nm, data + 8, 24);
    nm[24] = '\0';
    // Tronque la partie interne cachée ("Marchand#gon" -> "Marchand").
    if (char* h = std::strchr(nm, '#')) *h = '\0';
    if (nm[0]) npc_names_[gid] = nm;
    return;
  }

  if (opcode == kOpChoose) {
    // ZC_SELECT_DEALTYPE : le shop s'ouvre. On mémorise le NPC, on part en mode
    // ACHAT, et on demande la liste d'achat tout de suite (skip du chooser natif,
    // qui sera caché à sa création).
    if (len < 4) return;
    npc_id_ = *reinterpret_cast<const uint32_t*>(data);
    // Ce que le handler remplacé écrivait en tête (`mov [edi+24Ch], 1`) : le client
    // doit se savoir en interaction NPC, sinon il laisse le joueur marcher et
    // attaquer pendant la boutique — et CloseNativeShop, qui débloque cet état,
    // n'aurait plus rien à débloquer.
    //
    // 🔴 GID = 0 : on NE TOUCHE PAS à CGameMode+0x2DC, et c'est tout l'enjeu. Le
    // handler natif (0x00CA0F02) pose le flag, crée le chooser et lui passe le
    // npcId par OnMsg 0x1C (-> fenêtre+0xB4) — il n'écrit JAMAIS +0x2DC. Vérifié à
    // l'échelle du dispatcher : sur les six sites qui posent +0x24C=1 dans
    // RecvLoop_DispatchPackets, seuls quatre écrivent aussi +0x2DC, et ce sont les
    // paquets de DIALOGUE.
    //
    // Ce champ porte le GID de la CONVERSATION en cours, pas celui de la boutique.
    // Une boutique EST une interaction NPC : elle s'ouvre depuis un script, et
    // c'est ce script que le serveur attend de voir fermé (sd->npc_id). En y
    // écrivant le GID de la boutique, on effaçait l'identité du script : la
    // fermeture partait au mauvais NPC, npc_scriptcont la rejetait, et le joueur
    // restait bloqué — au clic sur la croix comme au basculement à chaud.
    rag::SetNpcInteractionActive(0);
    open_ = true;
    cur_mode_ = kBuy;
    buy_items_.clear();
    sell_items_.clear();
    // Les deux moitiés de la liste de vente partent ensemble : garder les entrées
    // brutes d'une session précédente les ferait resurgir à la prochaine
    // résolution, avec les index d'un inventaire qui a changé depuis.
    sell_raw_.clear();
    sell_dirty_ = false;
    cart_.clear();
    have_buy_ = false;
    buy_requested_ = false;
    sell_requested_ = false;
    last_result_ = -1;
    sell_all_close_ = false;
    want_close_ = false;
    // La requête de liste (0xc5) part de OnTick (thread principal), pas d'ici.
    return;
  }

  if (opcode == kOpBuyList) {
    // OBSERVE : data = buffer recv live à [packetLength:2][sub...]. sub =
    // {itemId:4, price:4, discountPrice:4, itemType:1, viewSprite:2, location:4}
    // = 19 octets (PACKETVER >= 20210203).
    if (len < 2) return;
    const uint16_t plen = *reinterpret_cast<const uint16_t*>(data);
    const int body = static_cast<int>(plen) - 4;
    if (body <= 0) return;
    constexpr int kSub = 19;
    int count = body / kSub;
    if (count <= 0) return;
    if (count > 4096) count = 4096;
    buy_items_.clear();
    buy_items_.reserve(count);
    const uint8_t* p = data + 2;
    for (int i = 0; i < count; ++i) {
      BuyItem b;
      b.id       = *reinterpret_cast<const uint32_t*>(p + 0);
      b.price    = *reinterpret_cast<const int32_t*>(p + 4);
      b.discount = *reinterpret_cast<const int32_t*>(p + 8);
      b.type     = p[12];
      b.view     = *reinterpret_cast<const uint16_t*>(p + 13);
      b.location = *reinterpret_cast<const uint32_t*>(p + 15);
      buy_items_.push_back(b);
      p += kSub;
    }
    have_buy_ = true;
    return;
  }

  if (opcode == kOpBuyRes) {
    if (len < 1) return;
    last_result_ = data[0];
    last_result_sell_ = false;
    if (last_result_ == 0 && cur_mode_ == kBuy) cart_.clear();  // succès -> panier vidé
    return;
  }
  if (opcode == kOpSellRes) {
    if (len < 1) return;
    last_result_ = data[0];
    last_result_sell_ = true;
    if (last_result_ == 0 && cur_mode_ == kSell) {
      cart_.clear();
      // "Tout ajouter au panier" -> ferme le shop apres la vente reussie.
      if (sell_all_close_) want_close_ = true;
    }
    // La vente vide npc_shopid côté serveur : re-armer pour re-shopper.
    sell_requested_ = false;
    buy_requested_ = false;
    return;
  }
  if (opcode == kOpSellList) {
    // ZC_PC_SELL_ITEMLIST : data = [packetLength:2][ {index:2, price:4,
    // overcharge:4} * n ]. C'est le SERVEUR qui décide de ce qui est vendable —
    // Moonlight y ajoute ses propres filtres (ni cartes, ni objets sertis, ni
    // raffinés, groupes IG_SELLSTUFF/IG_SELLITEM, flèches selon la classe). La
    // liste ne peut donc PAS être déduite de l'inventaire : elle se lit ici, et
    // nulle part ailleurs.
    if (len < 2) return;
    const uint16_t plen = *reinterpret_cast<const uint16_t*>(data);
    const int body = static_cast<int>(plen) - 4;
    if (body <= 0) { sell_raw_.clear(); sell_dirty_ = true; return; }
    constexpr int kSub = 10;
    int count = body / kSub;
    if (count <= 0) { sell_raw_.clear(); sell_dirty_ = true; return; }
    if (count > 4096) count = 4096;
    sell_raw_.clear();
    sell_raw_.reserve(count);
    const uint8_t* p = data + 2;
    for (int i = 0; i < count; ++i) {
      SellRaw r;
      r.index      = *reinterpret_cast<const uint16_t*>(p + 0);
      r.price      = *reinterpret_cast<const int32_t*>(p + 2);
      r.overcharge = *reinterpret_cast<const int32_t*>(p + 6);
      sell_raw_.push_back(r);
      p += kSub;
    }
    // La RÉSOLUTION (nom composé, icône, quantité) attend le thread principal :
    // elle appelle le name-builder natif, et on ne fait rien de tel depuis le fil
    // réseau — c'est la règle de ce fichier depuis le début.
    sell_dirty_ = true;
    return;
  }
}

// Re-selectionne le deal (CZ_ACK_SELECT_DEALTYPE 0xc5) pour RE-ARMER sd->npc_shopid
// que le serveur efface apres chaque 0xc8/0xc9. A envoyer JUSTE AVANT chaque
// transaction (l'ordre TCP garantit : arme puis achete/vend). type 0=achat, 1=vente.
void NpcShopWindow::SendDealSelect(uint8_t type) {
  if (npc_id_ == 0) return;
  uint8_t pkt[7];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpDealAck;  // CZ_ACK_SELECT_DEALTYPE 0xc5
  *reinterpret_cast<uint32_t*>(pkt + 2) = npc_id_;
  pkt[6] = type;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

void NpcShopWindow::RequestList(Mode mode) {
  if (npc_id_ == 0) return;
  // Les DEUX modes tiennent désormais dans la même requête brute
  // CZ_ACK_SELECT_DEALTYPE : on parse nous-mêmes les deux listes (0x0b77 et
  // 0x00c7), donc aucune fenêtre native n'a besoin d'exister pour les recevoir.
  //
  // 🔴 La vente passait avant par un détour : la requête brute ne mettait pas le
  // client en « mode vente », donc il ne créait pas la fenêtre 0x17 — et c'est
  // d'ELLE qu'on lisait la liste résolue. On dispatchait donc cmd 0x25, le bouton
  // « Vendre » natif, uniquement pour faire naître cette fenêtre. Le détour tombe
  // avec elle : le serveur, lui, a toujours répondu au simple 0xc5(1).
  SendDealSelect(mode == kBuy ? 0 : 1);
  if (mode == kBuy) buy_requested_ = true;
  else              sell_requested_ = true;
}

void NpcShopWindow::AddToCart(uint32_t id, int index, int32_t price, int max, int qty) {
  if (max < 1) max = 1;
  if (qty < 1) qty = 1;
  for (auto& e : cart_) {
    if (e.id == id && e.index == index) {
      // Objet non empilable déjà au panier : le natif REFUSE le second exemplaire
      // et le dit au chat (case 38 : `if (ShopCart_FindBuyAmount(id)) ->
      // MsgString 0x77`). On garde le refus ET le message — sans lui, le bouton
      // « +1 » ne ferait rien et le joueur croirait à un clic manqué.
      if (e.max == 1 && cur_mode_ == kBuy && e.amount >= 1) {
        SayEquipOnlyOne();
        return;
      }
      e.amount += qty;
      if (e.amount > e.max) e.amount = e.max;  // borné à la quantité dispo
      return;
    }
  }
  cart_.push_back(CartEntry{id, index, qty > max ? max : qty, price, max});
}

// CZ_PC_PURCHASE_ITEMLIST 0xc8 : [op:2][len:2][ {amount:2, itemId:4} *count ]
//
// Le `SendDealSelect(0)` re-arme npc_shopid, que le serveur efface apres chaque
// achat. Il est ICI, pas chez les appelants : l'oublier ne donne pas une erreur,
// il donne un achat qui ne part pas.
void NpcShopWindow::SendBuyList(const CartEntry* items, int count) {
  if (items == nullptr || count < 1) return;
  SendDealSelect(0);
  const int plen = 4 + 6 * count;
  std::vector<uint8_t> pkt(plen);
  uint8_t* p = pkt.data();
  *reinterpret_cast<uint16_t*>(p + 0) = kOpBuyReq;
  *reinterpret_cast<uint16_t*>(p + 2) = static_cast<uint16_t>(plen);
  uint8_t* it = p + 4;
  for (int k = 0; k < count; ++k) {
    *reinterpret_cast<uint16_t*>(it + 0) = static_cast<uint16_t>(items[k].amount);
    *reinterpret_cast<uint32_t*>(it + 2) = items[k].id;
    it += 6;
  }
  Bourgeon::Instance().SendPacket(pkt.data(), pkt.size());
}

// CZ_PC_SELL_ITEMLIST 0xc9 : [op:2][len:2][ {index:2, amount:2} *count ]
void NpcShopWindow::SendSellList(const CartEntry* items, int count) {
  if (items == nullptr || count < 1) return;
  SendDealSelect(1);
  const int plen = 4 + 4 * count;
  std::vector<uint8_t> pkt(plen);
  uint8_t* p = pkt.data();
  *reinterpret_cast<uint16_t*>(p + 0) = kOpSellReq;
  *reinterpret_cast<uint16_t*>(p + 2) = static_cast<uint16_t>(plen);
  uint8_t* it = p + 4;
  for (int k = 0; k < count; ++k) {
    *reinterpret_cast<uint16_t*>(it + 0) = static_cast<uint16_t>(items[k].index);
    *reinterpret_cast<uint16_t*>(it + 2) = static_cast<uint16_t>(items[k].amount);
    it += 4;
  }
  Bourgeon::Instance().SendPacket(pkt.data(), pkt.size());
}

void NpcShopWindow::SendBuy() {
  if (cart_.empty()) return;
  SendBuyList(cart_.data(), static_cast<int>(cart_.size()));
}

void NpcShopWindow::SendSell() {
  if (cart_.empty()) return;
  SendSellList(cart_.data(), static_cast<int>(cart_.size()));
}

// Achat IMMEDIAT de `qty` unites de `id` (bypass panier) : le MEME paquet, a un
// seul item. Le serveur calcule le cout (discount inclus) et valide.
void NpcShopWindow::QuickBuy(uint32_t id, int qty) {
  if (npc_id_ == 0 || qty < 1) return;
  CartEntry e;
  e.id = id;
  e.amount = qty;
  SendBuyList(&e, 1);
}

// Vente IMMEDIATE de `qty` unites de l'item a l'index inventaire `index` (bypass
// panier) : le MEME paquet, a un seul item. Le serveur calcule le gain (overcharge).
void NpcShopWindow::QuickSell(int index, int qty) {
  if (npc_id_ == 0 || qty < 1) return;
  CartEntry e;
  e.index = index;
  e.amount = qty;
  SendSellList(&e, 1);
}

// Lit la liste de vente RÉSOLUE depuis le sous-window liste natif, pointé par le
// global g_ShopSellMirrorWnd_ptr (DAT_0131f738), vtable 0x0103cbf0, std::list @+0xe8.
// Nœud MSVC std::list : {next, prev, value@+8}. Offsets CONFIRMÉS live (jellopy) :
// +0x0c index inv, +0x18 qté, +0x1c/+0x20 prix (overcharge), +0x34 std::string =
// itemId EN TEXTE ("909"), +0x90 slots. SEH (POD only).
// Complète les entrées brutes du paquet (index + les deux prix) avec ce que le
// paquet ne porte PAS : l'objet lui-même. On croise donc chaque index avec le
// MODÈLE SESSION de l'inventaire — la même source que le natif consultait pour
// remplir sa fenêtre, et qui reste peuplée alors que cette fenêtre n'existe plus.
//
// Thread PRINCIPAL uniquement (appelée depuis OnTick) : le name-builder est natif.
void NpcShopWindow::ResolveSellItems() {
  sell_items_.clear();
  sell_items_.reserve(sell_raw_.size());
  // (Plus de « contexte » à fournir au name-builder : il résout lui-même le
  // gestionnaire de fenêtres, cf. itemcell::BuildDisplayName. On passait ici la
  // fenêtre d'inventaire — que l'interface moderne fait justement disparaître.)

  // Verrou « ne pas vendre les favoris » relu à chaque résolution : c'est un octet
  // que le joueur bascule quand il veut, y compris la boutique ouverte.
  const bool deal_lock = DealLockOn();

  for (const SellRaw& r : sell_raw_) {
    void* info = itemcell::FindInfoByIndex(rag::kInventoryListAddr, r.index);
    if (!info) continue;  // vendu entre-temps : l'entrée n'a plus d'objet
    SellItem s;
    s.index = r.index;
    // Le serveur envoie les DEUX prix : `price` est le tarif de base, `overcharge`
    // ce que le marchand paiera vraiment (compétence Overcharge). L'un peut valoir
    // l'autre. On garde les deux pour afficher « base -> final ».
    s.base_price = r.price;
    s.price      = (r.overcharge != 0) ? r.overcharge : r.price;
    uint8_t favorite = 0;
    __try {
      uint8_t* p = reinterpret_cast<uint8_t*>(info);
      const char* ids = rag::clientstr::Data(p + kInfoIdStr);
      s.id     = ids ? static_cast<uint32_t>(atoi(ids)) : 0;
      s.amount = *reinterpret_cast<int*>(p + rag::itemlist::kInfoAmount);
      favorite = *(p + rag::itemlist::kInfoFav);
      // Données d'instance de l'aperçu au survol (cf. SellItem dans le .h). Lues
      // ICI et pas au rendu : c'est le seul moment où l'on tient l'ItemSkillInfo,
      // et une lecture par frame dans une liste qui peut mourir sous nos pieds
      // (objet vendu, transféré) n'aurait rien apporté qu'un SEH de plus.
      s.refine  = *reinterpret_cast<int*>(p + rag::itemlist::kInfoRefine);
      s.damaged = *(p + rag::itemlist::kInfoDamaged);
      const uint32_t card0 = *reinterpret_cast<uint32_t*>(p + rag::itemlist::kInfoCards);
      s.forged = (card0 != 0 && card0 <= 500);  // forgeron, pas des cartes
      if (!s.forged) {
        for (int k = 0; k < 4; ++k)
          s.cards[k] = *reinterpret_cast<uint32_t*>(p + rag::itemlist::kInfoCards + k * 4);
      }
      int nopt = *reinterpret_cast<int*>(p + rag::itemlist::kInfoOptCount);
      if (nopt < 0) nopt = 0;
      if (nopt > 5) nopt = 5;
      s.opt_count = nopt;
      for (int k = 0; k < nopt; ++k) {
        const uint8_t* e = p + rag::itemlist::kInfoOpts + k * 5;
        s.opts[k].index = *reinterpret_cast<const int16_t*>(e);
        s.opts[k].value = *reinterpret_cast<const int16_t*>(e + 2);
        s.opts[k].param = e[4];
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
    // Le filtre du natif, à l'identique : favori + verrou posé = pas vendable. Le
    // serveur, lui, ne connaît pas ce verrou (c'est un garde-fou purement client) —
    // s'il n'est pas appliqué ICI, rien d'autre ne le fera.
    if (deal_lock && favorite != 0) continue;
    s.slots = itemcell::SlotCount(info);
    itemcell::BuildDisplayName(info, s.name, sizeof(s.name));
    if (s.amount > 0) sell_items_.push_back(s);
  }
  sell_dirty_ = false;
  PruneSellCart();
}

// Le panier de vente ne doit rien garder qui vient de sortir de la liste — cas
// concret : on remplit le panier de favoris, PUIS on pose le verrou. Sans ce ménage,
// le bouton « Vendre » les enverrait quand même, et le serveur, qui ne connaît pas
// ce verrou (purement client), obéirait.
//
// ⚠ Fonction à part, et pas la fin de ResolveSellItems : celle-ci contient un __try,
// que MSVC refuse de voir cohabiter avec un objet local à déroulement (C2712) — et
// la purge a besoin d'un vecteur temporaire.
void NpcShopWindow::PruneSellCart() {
  if (cur_mode_ != kSell || cart_.empty()) return;
  std::vector<CartEntry> kept;
  kept.reserve(cart_.size());
  for (const CartEntry& e : cart_) {
    for (const SellItem& s : sell_items_) {
      if (s.index == e.index) { kept.push_back(e); break; }
    }
  }
  if (kept.size() == cart_.size()) return;
  cart_.swap(kept);
  if (cart_.empty()) sell_all_close_ = false;  // plus rien à vendre -> désarme
}

// Ferme la boutique des DEUX côtés, sans rien piloter du natif.
//
// 🔴 Le CANCEL natif (cmd 0x28 sur CMode::SendMsg) a disparu d'ici. Il faisait
// trois choses — prévenir le serveur, éteindre l'état d'interaction du client,
// vider le panier — et on les fait désormais nous-mêmes, explicitement. C'était la
// dernière dépendance : se servir du natif pour se débarrasser du natif, alors
// même qu'on lui a retiré les fenêtres sur lesquelles ce chemin s'appuie.
//
// Ce qui l'a rendu possible, c'est de savoir lire le GID d'une session ouverte
// avant nous (ChooserNpcId, RE de UIChooseSellBuyWnd_OnMsg case 0x1C) : le
// paquet de fermeture demande ce GID, et il ne nous parvenait pas dans ce cas-là.
void NpcShopWindow::CloseNativeShop() {
  // ⚠ DEUX GID, et ils ne se valent pas.
  //
  //   - la CONVERSATION (CGameMode+0x2DC) : le NPC dont le script tourne, donc
  //     sd->npc_id côté serveur. C'est LUI que CZ_CLOSE_DIALOG doit nommer, sans
  //     quoi npc_scriptcont rejette la fermeture et le personnage reste figé.
  //   - la BOUTIQUE (ZC_SELECT_DEALTYPE, ou le chooser natif d'une session ouverte
  //     avant nous) : le marchand nommé par le script. Souvent un AUTRE NPC.
  //
  // On envoie donc les deux quand ils diffèrent, et non l'un « au choix » : selon
  // que la boutique vient d'un script ou d'un clic direct, ce n'est pas le même qui
  // débloque, et le second paquet ne coûte rien (sd->npc_id déjà nul -> le serveur
  // sort immédiatement). C'est exactement le partage que fait le natif, qui route
  // son annulation vers le sélecteur 0x59 — CZ_CLOSE_DIALOG — quand une fenêtre de
  // dialogue est encore vivante, et se contente de fermer ses fenêtres sinon.
  const uint32_t shop_gid = npc_id_ != 0 ? npc_id_ : ChooserNpcId();
  const uint32_t talk_gid = rag::NpcInteractionGid();

  // 1. SERVEUR, la boutique : CZ_NPC_TRADE_QUIT remet sd->npc_shopid à zéro. C'est
  //    LE paquet qui débloque le personnage — `pc_cant_act2()` teste ce champ, et
  //    clif_parse_WalkToXY refuse tout déplacement tant qu'il est posé. Il part
  //    d'ABORD, avant toute fermeture de dialogue.
  {
    uint8_t pkt[2];
    *reinterpret_cast<uint16_t*>(pkt) = kOpShopQuit;
    Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  }
  // 2. SERVEUR, le script : sans ça, sd->npc_id reste posé et le dialogue qui a mené
  //    à la boutique n'est jamais terminé.
  SendCloseDialog(talk_gid);
  if (shop_gid != talk_gid) SendCloseDialog(shop_gid);
  // 3. CLIENT : l'état d'interaction retombe. Il ne débloque QUE le client (le
  //    pipeline souris de CGameMode lit +0x24C) ; le serveur, lui, ne se libère que
  //    sur les paquets ci-dessus.
  rag::ClearNpcInteractionActive();
  // 4. Panier natif : ShopCart_ResetAll, que le natif appelle au même moment (son
  //    OnMsg case 6 / bouton 185). Fonction utilitaire sur la session, pas une
  //    fenêtre : la laisser au natif ne maintient rien en vie. ⚠ Elle prend le
  //    contexte de session, que le natif nomme g_UIWindowContextKey — c'est bien
  //    rag::kSessionAddr, même adresse (0x015FA3C0), vérifié au désassemblage.
  __try {
    reinterpret_cast<void(__fastcall*)(int)>(kShopCartResetAll)(
        static_cast<int>(rag::kSessionAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  // 5. Les fenêtres, s'il en reste une d'un basculement à chaud.
  PurgeNativeShopWindows();
  sell_all_close_ = false;  // desarme la fermeture auto
}

bool NpcShopWindow::AnyNativeShopWindow() const {
  return uiwnd::SafeFindWindow(uiwnd::kUIChooseSellBuyWnd) ||
         uiwnd::SafeFindWindow(uiwnd::kUIItemShopWnd) ||
         uiwnd::SafeFindWindow(uiwnd::kUIItemPurchaseWnd) ||
         uiwnd::SafeFindWindow(uiwnd::kUIItemSellWnd) ||
         uiwnd::SafeFindWindow(uiwnd::kUIItemParamChangeDisplayWnd);
}

void NpcShopWindow::PurgeNativeShopWindows() {
  uiwnd::SafeCloseWindow(uiwnd::kUIChooseSellBuyWnd);
  uiwnd::SafeCloseWindow(uiwnd::kUIItemShopWnd);
  uiwnd::SafeCloseWindow(uiwnd::kUIItemPurchaseWnd);  // panneau ACHAT
  uiwnd::SafeCloseWindow(uiwnd::kUIItemSellWnd);  // panneau VENTE — vivant même
                                                  // quand il n'est pas affiché
  // 🔴 Le comparateur ATK/DEF est DÉTRUIT, pas seulement masqué. Il ne l'était
  // dans aucune liste de fermeture : on se contentait de le rendre invisible, et
  // une native masquée garde le CLAVIER — la leçon du refine, celle qui a détruit
  // une arme. Son créateur (la fenêtre d'achat native) ne naît plus, donc ce
  // chemin ne devrait plus être emprunté ; c'est justement pour ça qu'il doit être
  // correct sans qu'on ait à y repenser.
  uiwnd::SafeCloseWindow(uiwnd::kUIItemParamChangeDisplayWnd);
}

void NpcShopWindow::HideDetailWindow(void* win) {
  if (!win || !imgui_enabled_ || !open_) return;
  __try {
    // Masquage SEUL, et à dessein : on est à l'intérieur de MakeWindow, dont
    // l'appelant déréférence le retour — détruire ici plante. Ça évite la frame de
    // flash ; la destruction, elle, revient à PurgeNativeShopWindows au tick.
    if (*reinterpret_cast<uintptr_t*>(win) == uiwnd::kUIItemParamChangeDisplayWndVTable)
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + uiwnd::kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void NpcShopWindow::OnTick() {
  // ── Basculement de l'interrupteur, les DEUX sens ────────────────────────────
  //
  // 🔴 Basculer FERME la boutique, quel que soit le sens. C'est tout l'objet du
  // remplacement : dès qu'ImGui prend la main, plus rien de natif ne doit être
  // vivant — une fenêtre native laissée à l'écran garde le CLAVIER, et son bouton
  // par défaut valide une transaction.
  //
  // ON -> OFF : lâcher `open_` ne suffit pas. Avant le remplacement des handlers
  // c'était sans conséquence — la native vivait derrière, masquée, et redevenait
  // visible. Elle n'existe plus, et c'est NOUS qui posons l'état d'interaction du
  // client : sans fermeture explicite, le joueur se retrouvait sans aucune fenêtre
  // ET bloqué (npc_shopid armé serveur, +0x24C posé client).
  //
  // OFF -> ON en pleine boutique NATIVE : on ferme la sienne. On ne la REPREND pas
  // — on ne rouvre jamais l'équivalent sur une bascule — et on ne la laisse pas
  // finir non plus, ce serait garder vivant ce qu'on prétend avoir remplacé.
  //
  // ⚠ Fermer une session ouverte AVANT nous demande son npc_id, qui ne nous est
  // jamais parvenu (le chooser le reçoit par OnMsg, il n'atterrit pas dans
  // CGameMode+0x2DC comme le ferait un dialogue). On va donc le LIRE dans le
  // chooser — cf. ChooserNpcId — au lieu de déléguer au CANCEL natif.
  if (imgui_enabled_ != prev_imgui_enabled_) {
    prev_imgui_enabled_ = imgui_enabled_;
    if (open_ || AnyNativeShopWindow()) CloseNativeShop();
    open_ = false;
    was_open_ = false;
    // Une demande de fermeture posée juste avant la bascule n'a plus d'objet — et
    // la laisser traîner ferait fermer, plus tard, une boutique qui n'est pas elle.
    want_close_ = false;
  }
  if (!imgui_enabled_) { open_ = false; was_open_ = false; return; }

  // Fermeture AUTO demandee (vente d'un "Tout ajouter au panier" reussie) : meme
  // chemin que le clic sur le X, depuis le thread principal.
  if (want_close_) {
    want_close_ = false;
    CloseNativeShop();
    open_ = false;
    was_open_ = false;
    show_panel_ = true;
    return;
  }

  // Changement de map (@load, warp...) recu : la session shop est invalidee cote
  // serveur (pc_setpos remet npc_shopid=0 / npc_id=0). Nettoyage CLIENT seulement,
  // donc — AUCUN paquet : le serveur a deja tout defait, et un CZ_CLOSE_DIALOG
  // arriverait apres la bataille.
  if (map_changed_) {
    map_changed_ = false;
    if (open_) {
      // Nettoyage CLIENT uniquement (pas de paquet : le warp a déjà tout remis à
      // zéro côté serveur), flag d'interaction compris — c'est nous qui l'avons
      // posé, personne d'autre ne l'éteindra.
      rag::ClearNpcInteractionActive();
      PurgeNativeShopWindows();
      open_ = false;
      was_open_ = false;
      show_panel_ = true;
      npc_id_ = 0;
      buy_items_.clear();
      sell_items_.clear();
      sell_raw_.clear();
      sell_dirty_ = false;
      cart_.clear();
      buy_requested_ = false;
      sell_requested_ = false;
      have_buy_ = false;
      sell_all_close_ = false;
      want_close_ = false;
    }
    return;
  }

  // 🔴 La session n'est plus déduite de la présence d'une fenêtre native : c'est
  // le paquet 0x00c4 qui l'ouvre et notre fermeture qui l'éteint. L'ancienne
  // détection était même devenue trompeuse — les fenêtres ne naissant plus, elle
  // n'aurait jamais rien vu.
  if (open_) {
    // Filet du basculement d'interrupteur en pleine session : on DÉTRUIT ce qui
    // traîne au lieu de le masquer (masquée, une native garde le clavier).
    PurgeNativeShopWindows();
    if (!was_open_) need_pos_ = true;
    // Demande la liste du mode courant si pas encore faite (envoi thread principal).
    if (cur_mode_ == kBuy && !buy_requested_) RequestList(kBuy);
    if (cur_mode_ == kSell && !sell_requested_) RequestList(kSell);
    // Verrou de vente des favoris basculé pendant que la boutique est ouverte (le
    // pied de l'inventaire moderne est à portée de clic) : la liste doit suivre,
    // dans les deux sens — masquer les favoris, ou les faire réapparaître.
    const bool deal_lock = DealLockOn();
    if (deal_lock != prev_deal_lock_) {
      prev_deal_lock_ = deal_lock;
      sell_dirty_ = true;
    }
    // Résolution de la liste de vente reçue au réseau : ici, parce qu'elle appelle
    // le name-builder natif. Une seule fois par salve, pas à chaque tick — l'ancien
    // code relisait la fenêtre native en boucle faute d'événement.
    if (sell_dirty_) ResolveSellItems();
  } else {
    npc_id_ = 0;
  }
  was_open_ = open_;
}

void NpcShopWindow::OnRenderUI() {
  if (!open_ || !imgui_enabled_) return;

  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(spawn_x_),
                                   static_cast<float>(spawn_y_)),
                            ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_FirstUseEver);
  // Meme habillage que le cashshop (skin RO).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

  // Titre = nom du NPC (observé via 0x0adf) si connu, sinon "Shop".
  char title[64];
  auto nit = npc_names_.find(npc_id_);
  std::snprintf(title, sizeof(title), i18n::Tr("%s###bourgeon_shop"),
                (nit != npc_names_.end() && !nit->second.empty())
                    ? nit->second.c_str()
                    : "Shop");
  const bool begun =
      ro::BeginRoWindow(title, &show_panel_, ImGuiWindowFlags_NoCollapse);
  bourgeon::CloseWindowOnEscape(show_panel_);
  if (!show_panel_) {
    // Fermeture réelle : détruit les fenêtres natives -> plus de fenêtre shop ->
    // open_ repasse à false au prochain tick.
    CloseNativeShop();
    open_ = false;
    show_panel_ = true;
    ro::EndRoWindow();
    ImGui::PopStyleVar(5);
    return;
  }
  if (!begun) { ro::EndRoWindow(); ImGui::PopStyleVar(5); return; }

  // ── Onglets Achat / Vente ──
  int prev_mode = cur_mode_;
  if (ro::RoBeginTabBar("shop_tabs")) {
    if (ImGui::BeginTabItem(i18n::Tr("Acheter"))) { cur_mode_ = kBuy; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(i18n::Tr("Vendre")))  { cur_mode_ = kSell; ImGui::EndTabItem(); }
    ro::RoEndTabBar();
  }
  if (cur_mode_ != prev_mode) {
    cart_.clear();       // panier propre à chaque onglet
    last_result_ = -1;
    // Bascule vers Vendre : RE-demander la liste (l'inventaire a pu changer, ex.
    // après un achat) -> la liste native est un snapshot, sinon elle reste périmée.
    if (cur_mode_ == kSell) sell_requested_ = false;
    sell_all_close_ = false;  // changement d'onglet -> desarme la fermeture auto
  }

  // Bandeau : zeny du joueur + résultat de la dernière transaction.
  const uint32_t zeny = static_cast<uint32_t>(rag::Zeny());
  ImGui::TextColored(ro::pal::kBlack, i18n::Tr("Zeny: %uz"), zeny);
  if (last_result_ == 0) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), i18n::Tr("  %s OK"),
                       last_result_sell_ ? "Vente" : "Achat");
  } else if (last_result_ > 0) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), i18n::Tr("  Échec (%d)"), last_result_);
  }

  // Rend une cellule prix facon natif : "base -> final" (base grise, final en
  // couleur) quand la remise Discount (achat) ou la majoration Overcharge (vente)
  // change le prix marchand ; sinon juste le prix final. base<=0 ou == final =>
  // affichage simple.
  auto draw_price = [&](int32_t base, int32_t final_price, const ImVec4& col) {
    if (base > 0 && base != final_price) {
      ImGui::TextDisabled("%d", base);
      ImGui::SameLine(0.0f, 3.0f);
      ImGui::TextDisabled("->");
      ImGui::SameLine(0.0f, 3.0f);
      ImGui::TextColored(col, "%dz", final_price);
    } else {
      ImGui::TextColored(col, "%dz", final_price);
    }
  };

  // ── Objet survolé cette frame ───────────────────────────────────────────────
  // On MÉMORISE, on ne peint rien dans la boucle : `itemcell::DrawTooltip` crée son
  // PROPRE popup et doit sortir hors de toute fenêtre ImGui (cf. features/item_cell.h)
  // — né dans le tableau, il serait clippé dedans. Le rendu a lieu après EndRoWindow.
  // Deux natures d'aperçu, parce que les deux moitiés ne savent pas la même chose :
  // à l'ACHAT on ne tient qu'un id (l'objet n'est l'exemplaire de personne), à la
  // VENTE on tient l'ItemSkillInfo du sac, cartes et raffinage compris.
  const SellItem* hover_sell = nullptr;
  uint32_t        hover_buy  = 0;

  // Clic-droit sur le DERNIER item dessine (icone ou nom) -> description native.
  // DIFFÉRÉE au relâchement (itemcell::FlushDeferredDesc) : ouverte dès le clic,
  // un appui PROLONGÉ faisait passer la description DERRIÈRE nous.
  auto rclick_desc = [&](uint32_t id, uint16_t view, uint32_t loc) {
    if (mui::IsLastItemRightClicked()) {
      const ImVec2 mp = ImGui::GetMousePos();
      itemcell::DeferDescById(id, view, loc, static_cast<int>(mp.x),
                              static_cast<int>(mp.y));
    }
  };

  // Boutons quantite +1/+10/+100/+1k d'une ligne. Clic = ajout au panier ;
  // Ctrl+clic = transaction immediate (bypass panier). Le grisage n'a lieu QU'EN
  // mode immediat (Ctrl) : sans Ctrl on empile librement (le bouton Acheter/Vendre
  // gere la solvabilite du total). Immediat : achat grise si qty*prix > zeny ;
  // vente grisee si qty > quantite possedee.
  static const int kQty[4] = {1, 10, 100, 1000};
  // Palier de quantites d'une ligne. A l'ACHAT le stock du marchand est illimite en
  // pratique -> l'echelle fixe 1/10/100/1k. A la VENTE elle est absurde : proposer
  // « +1k » sur un objet possede en 3 exemplaires n'offre que des boutons morts. On
  // taille donc l'echelle sur ce qu'on POSSEDE — on garde les paliers strictement
  // inferieurs au stack, et le dernier bouton est le stack ENTIER, etiquete de sa
  // vraie valeur : 9 -> « +1 +9 » ; 650 -> « +1 +10 +100 +650 » ; 1 -> « +1 » seul.
  // Plafonne a 4 boutons (largeur de colonne), le dernier restant toujours le total.
  auto qty_steps = [](int max_avail, bool is_buy, int* out) -> int {
    // Un objet non empilable s'achete a l'unite (max_avail = 1) : proposer
    // « +10 / +100 / +1k » sur une arme, c'est promettre une commande que le
    // serveur ramenera a UN exemplaire sans le dire au joueur.
    if (max_avail <= 1) { out[0] = 1; return 1; }
    if (is_buy) { for (int k = 0; k < 4; ++k) out[k] = kQty[k]; return 4; }
    if (max_avail < 1) max_avail = 1;
    int n = 0;
    for (int k = 0; k < 4 && n < 3; ++k) {
      if (kQty[k] >= max_avail) break;
      out[n++] = kQty[k];
    }
    if (n == 0 || out[n - 1] != max_avail) out[n++] = max_avail;  // le stack entier
    return n;
  };
  auto qty_buttons = [&](uint32_t id, int index, int32_t unit_price, int max_avail,
                         bool is_buy) {
    const bool ctrl = ImGui::GetIO().KeyCtrl;
    int steps[4] = {0, 0, 0, 0};
    const int count = qty_steps(max_avail, is_buy, steps);
    const float pad2 = ImGui::GetStyle().FramePadding.x * 2.0f;
    for (int k = 0; k < count; ++k) {
      if (k) ImGui::SameLine(0.0f, 2.0f);
      const int q = steps[k];
      // Libelle : « +1k » reste l'abrege consacre a l'achat ; a la vente on ecrit la
      // valeur exacte, c'est tout l'interet (savoir qu'on a 650 sans compter).
      char lbl[16];
      if (is_buy && k == 3) std::snprintf(lbl, sizeof(lbl), "+1k");
      else                  std::snprintf(lbl, sizeof(lbl), "+%d", q);
      // Sans Ctrl (ajout panier) : jamais grise. Avec Ctrl (transaction immediate) :
      // grise si non abordable (achat). A la vente le palier ne depasse jamais le
      // stack par construction, il n'y a donc plus rien a griser.
      const bool ok = !ctrl || !is_buy ||
                      static_cast<long long>(unit_price) * q <=
                          static_cast<long long>(zeny);
      if (!ok) ImGui::BeginDisabled();
      // Largeur au contenu, plancher a 34 : « +30000 » ne doit pas etre tronque.
      const float bw = std::max(34.0f, ImGui::CalcTextSize(lbl).x + pad2);
      if (ro::RoButton(lbl, bw, 0.0f)) {
        if (ctrl) { if (is_buy) QuickBuy(id, q); else QuickSell(index, q); }
        else      AddToCart(id, index, unit_price, max_avail, q);
      }
      if (!ok) ImGui::EndDisabled();
    }
  };

  static ImGuiTextFilter filter;
  ImGui::SetNextItemWidth(-1.0f);
  // Placeholder grise "Filtrer..." quand le champ est vide (pilote InputBuf/Build).
  if (ImGui::InputTextWithHint("##shop_filter", i18n::Tr("Filtrer..."), filter.InputBuf,
                               IM_ARRAYSIZE(filter.InputBuf)))
    filter.Build();
  ImGui::TextDisabled("%s", i18n::Tr("Clic = panier   -   Ctrl+clic = achat/vente immédiat"));
  ImGui::Separator();

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  // Panier auto-masque quand il est vide -> la liste prend toute la largeur. (Avec
  // l'achat/vente immediat en Ctrl+clic, le panier ne sert qu'a l'achat groupe ; il
  // reapparait des qu'on y ajoute un item.)
  const bool show_cart = !cart_.empty();
  const float cart_w = 200.0f;
  const float list_w =
      show_cart ? std::max(160.0f, avail.x - cart_w - 8.0f) : 0.0f;
  // Hauteur réservée au pied de fenêtre (bouton « Fermer »), retirée aux deux
  // colonnes : hauteur négative = « laisse ça libre en bas ».
  const float footer_h =
      ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;

  // ── Liste (gauche) ──
  ImGui::BeginChild("shop_list", ImVec2(list_w, -footer_h), true);
  if (cur_mode_ == kBuy) {
    if (ImGui::BeginTable("buytbl", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn(i18n::Tr("Objet"));
      ImGui::TableSetupColumn(i18n::Tr("Prix"), ImGuiTableColumnFlags_WidthFixed, ro::Px(110.0f));
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ro::Px(150.0f));
      ImGui::TableHeadersRow();
      for (const auto& b : buy_items_) {
        const char* nm = itemcell::NameById(b.id);
        if (!filter.PassFilter(nm)) continue;
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(b.id));
        ImGui::TableNextColumn();
        ro::IconTex ic = ro::ItemIcon(b.id);
        if (ic.tex) {
          ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(20, 20));
          if (ImGui::IsItemHovered()) hover_buy = b.id;  // survol icone -> apercu
          rclick_desc(b.id, b.view, b.location);  // clic-droit icone -> desc
          ImGui::SameLine();
        }
        ImGui::TextUnformatted(nm);
        if (ImGui::IsItemHovered()) hover_buy = b.id;  // survol nom -> apercu
        rclick_desc(b.id, b.view, b.location);  // clic-droit nom -> desc
        ImGui::TableNextColumn();
        const bool afford = static_cast<uint32_t>(b.discount) <= zeny;
        // Prix noir si abordable, rouge sombre si trop cher ; "base -> remise" si Discount.
        draw_price(b.price, b.discount,
                   afford ? ro::pal::kBlack : ImVec4(0.75f, 0.15f, 0.15f, 1.0f));
        ImGui::TableNextColumn();
        // Plafond : la pile du client pour un consommable, UN SEUL exemplaire
        // pour un équipement (cf. BuyStackMax) — il porte la borne du panier et
        // taille les boutons de quantité.
        qty_buttons(b.id, -1, b.discount, BuyStackMax(b.type), true);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (buy_items_.empty())
      ImGui::TextDisabled("%s", i18n::Tr("(liste d'achat en attente du serveur...)"));
  } else {  // kSell
    // "Tout vendre" : remplit le panier avec TOUS les items vendables au stack
    // complet ; l'utilisateur confirme ensuite via le bouton "Vendre".
    if (!sell_items_.empty() &&
        ro::RoButton(i18n::Tr("Tout ajouter au panier"))) {
      cart_.clear();
      for (const auto& s : sell_items_)
        cart_.push_back(CartEntry{s.id, s.index, s.amount, s.price, s.amount});
      sell_all_close_ = true;  // arme la fermeture auto du shop apres la vente
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", i18n::Tr("Ajoute tout l'inventaire vendable ; le shop se fermera "
                              "automatiquement après la vente."));
    if (ImGui::BeginTable("selltbl", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn(i18n::Tr("Objet"));
      ImGui::TableSetupColumn(i18n::Tr("Vente"), ImGuiTableColumnFlags_WidthFixed, ro::Px(110.0f));
      // Plus large qu'a l'achat : le dernier bouton porte le stack en clair
      // (« +30000 » au pire), pas l'abrege « +1k ».
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ro::Px(172.0f));
      ImGui::TableHeadersRow();
      for (const auto& s : sell_items_) {
        // Nom COMPOSÉ (+refine, préfixes/suffixes de cartes) quand on a pu le
        // bâtir, sinon le nom nu de la DB. Le champ existait depuis toujours dans
        // SellItem mais restait vide : la liste venait de la fenêtre native, où
        // le nom stocké était l'itemId en texte. Il compte surtout chez un NPC en
        // `sellstuff`, le seul qui accepte le raffiné et le serti — voir « +7 »
        // avant de cliquer évite de vendre la mauvaise pièce.
        char nm_buf[96];
        const char* nm = itemcell::Label(
            nm_buf, sizeof(nm_buf),
            s.name[0] ? s.name : itemcell::NameById(s.id), s.slots);
        if (!filter.PassFilter(nm)) continue;
        ImGui::TableNextRow();
        ImGui::PushID(s.index);
        ImGui::TableNextColumn();
        ro::IconTex ic = ro::ItemIcon(s.id);
        // view/loc inconnus en vente -> 0 : pas d'apercu, la desc reste correcte.
        if (ic.tex) {
          ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(20, 20));
          if (ImGui::IsItemHovered()) hover_sell = &s;  // survol icone -> apercu
          rclick_desc(s.id, 0, 0);  // clic-droit icone -> desc
          ImGui::SameLine();
        }
        // `itemcell::NameText` et non `ImGui::Text` : un équipement CASSÉ porte
        // l'ombre rouge du natif, ici comme dans l'inventaire et l'échange. On lit
        // désormais le flag pour l'aperçu — la liste n'a pas de raison de l'ignorer,
        // et c'est justement au moment de vendre qu'on veut le voir.
        char row[128];
        std::snprintf(row, sizeof(row), "%s x%d", nm, s.amount);
        itemcell::NameText(row, s.damaged != 0);
        if (ImGui::IsItemHovered()) hover_sell = &s;  // survol nom -> apercu
        rclick_desc(s.id, 0, 0);  // clic-droit nom -> desc
        ImGui::TableNextColumn();
        // "base -> majore" si Overcharge, sinon juste le prix (lu du noeud natif).
        draw_price(s.base_price, s.price, ro::pal::kBlack);
        ImGui::TableNextColumn();
        qty_buttons(s.id, s.index, s.price, s.amount, false);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (sell_items_.empty())
      ImGui::TextDisabled("%s", i18n::Tr("(rien à vendre / liste en attente...)"));
  }
  ImGui::EndChild();

  // ── Panier (droite) — masque automatiquement quand vide (cf. show_cart) ──
  if (show_cart) {
  ImGui::SameLine();
  ImGui::BeginChild("shop_cart", ImVec2(cart_w, -footer_h), true);
  ImGui::TextUnformatted(cur_mode_ == kBuy ? i18n::Tr("Panier d'achat") : i18n::Tr("Panier de vente"));
  ImGui::SameLine();
  if (!cart_.empty() && ro::RoButton(i18n::Tr("Vider"))) { cart_.clear(); sell_all_close_ = false; }
  ImGui::Separator();
  long long total = 0;
  int remove = -1;
  ImGui::BeginChild("shop_cart_items", ImVec2(0, -56), false);
  for (int i = 0; i < static_cast<int>(cart_.size()); ++i) {
    CartEntry& e = cart_[i];
    total += static_cast<long long>(e.price) * e.amount;
    ImGui::PushID(2000 + i);
    ImGui::TextWrapped("%s", itemcell::NameById(e.id));
    // Controle quantite : [-] [champ] [+], petits boutons RO carres (comme le
    // cashshop). InputInt en step=0 -> pas de +/- natifs non skinnes.
    const float step = ImGui::GetFrameHeight();
    if (ro::RoButton("-", step, step) && e.amount > 1) --e.amount;
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(ro::Px(42.0f));
    if (ImGui::InputInt("##qty", &e.amount, 0, 0)) {
      if (e.amount < 1) e.amount = 1;
      if (e.amount > e.max) e.amount = e.max;  // vente = qté possédée ; achat = stack
    }
    ImGui::SameLine(0.0f, 2.0f);
    // Le « + » d'une ligne au plafond ne fait rien — sauf sur un équipement, où
    // le plafond EST la règle du serveur : là on le dit, comme le natif, plutôt
    // que de laisser croire à un clic manqué.
    if (ro::RoButton("+", step, step)) {
      if (e.amount < e.max)                        ++e.amount;
      else if (e.max == 1 && cur_mode_ == kBuy)    SayEquipOnlyOne();
    }
    // Sous-total (noir) centre verticalement sur la ligne des champs.
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ro::pal::kBlack, "%lldz", static_cast<long long>(e.price) * e.amount);
    // Bouton supprimer : petit bouton RO carre, aligne a droite.
    ImGui::SameLine();
    const float xr = ImGui::GetContentRegionMax().x - step;
    if (xr > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(xr);
    if (ro::RoButton("x", step, step)) remove = i;
    ImGui::Separator();
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (remove >= 0) cart_.erase(cart_.begin() + remove);
  ImGui::Separator();
  ImGui::Text(i18n::Tr("Total: %lldz"), total);

  if (cur_mode_ == kBuy) {
    const bool afford = total <= static_cast<long long>(zeny);
    if (cart_.empty()) ImGui::BeginDisabled();
    if (ro::RoButton(afford ? "Acheter" : i18n::Tr("Zeny insuffisant"),
                     ImGui::GetContentRegionAvail().x, 0))
      SendBuy();
    if (cart_.empty()) ImGui::EndDisabled();
  } else {
    if (cart_.empty()) ImGui::BeginDisabled();
    if (ro::RoButton(i18n::Tr("Vendre"), ImGui::GetContentRegionAvail().x, 0)) SendSell();
    if (cart_.empty()) ImGui::EndDisabled();
  }
  ImGui::EndChild();
  }  // if (show_cart) : panier masque quand vide

  // ── Pied de fenêtre : quitter la boutique ───────────────────────────────────
  //
  // Présent dans les DEUX onglets, et volontairement pas seulement dans le panier
  // (qui disparaît quand il est vide). La croix de la barre de titre fait la même
  // chose, mais rien ne le dit : chez le natif, « Annuler » était un bouton bien
  // visible, et fermer une boutique est l'action qu'un joueur cherche le plus
  // souvent après avoir acheté.
  //
  // Passe par want_close_ plutôt que d'appeler CloseNativeShop ici : la fermeture
  // touche des fonctions natives et se fait donc au tick suivant, hors de la frame
  // ImGui — le chemin qu'emprunte déjà la fermeture automatique de « Tout ajouter
  // au panier ».
  ImGui::Separator();
  if (ro::RoButton(i18n::Tr("Fermer"), 90.0f, 0.0f)) want_close_ = true;
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", i18n::Tr("Survol : aperçu   -   Clic droit : description"));

  ro::EndRoWindow();
  ImGui::PopStyleVar(5);

  // ── Aperçu de l'objet survolé ───────────────────────────────────────────────
  // APRÈS la fenêtre et après ses styles : un tooltip crée son propre popup, et
  // hériter des arrondis/bordures de la boutique lui donnerait un cadre qui n'est
  // pas le sien (celui de la description RO est peint par DrawTooltip).
  //
  // VENTE : l'objet est déjà à nous, on montre l'EXEMPLAIRE — cartes, raffinage,
  // options, « cassé ». C'est ce qui distingue deux pièces au même nom, et la
  // vente ne se rattrape pas. ACHAT : rien de tout ça n'existe encore, la
  // description de base suffit et le nom vient de la DB (nullptr = repli).
  if (hover_sell) {
    itemcell::DrawTooltip(hover_sell->id,
                          hover_sell->forged ? nullptr : hover_sell->cards,
                          hover_sell->forged ? 0 : 4, hover_sell->opts,
                          hover_sell->opt_count, hover_sell->refine,
                          hover_sell->name[0] ? hover_sell->name : nullptr,
                          hover_sell->damaged != 0);
  } else if (hover_buy != 0) {
    itemcell::DrawTooltip(hover_buy, nullptr, 0, nullptr, 0, 0, nullptr);
  }
}
