#include "ragnarok/item_db.h"
#include "features/windows/cashshop_window.h"

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
#include "imgui.h"
#include "features/item_cell.h"             // itemcell::OpenDescById (description au clic droit)
#include "features/overlays/basic_info.h"   // aperçu porté (RenderItemPreviewTooltip / CanPreview)
#include "ragnarok/msgstring.h"            // msgstr::Utf8 (messages EXACTS du client)
#include "ui/imgui_escape.h"
#include "ui/ro_imgui.h"          // BeginRoWindow (skin RO)
#include "ui/ro_widgets.h"        // mui::IsLastItemRightClicked
#include "utils/i18n.h"

//  Constantes RE (client 20250716, base 0x400000 ; cf. project_cashshop_re) 
namespace {

// UICashShopWnd : id 0x13e (318), vtable 0x0101ca18. Trouvée par FindWindow.
constexpr int       kWinCashShop  = 0x13e;
constexpr uintptr_t kCashVTable   = 0x0101ca18;

// Offsets UIWindow.

// Description d'item (clic-droit) : MakeWindow(0xc) + OnMsg 0x18 

// Icône d'item : le cash shop affiche l'image de COLLECTION (art de preview,
// bien plus grande que l'icône d'inventaire). Sa résolution vit dans le cache
// partagé, ro::ItemCollectionIcon (ui/icon_cache.h), avec le repli sur la petite
// icône quand l'art n'existe pas.

// Détruit la fenêtre native du cash shop (id 0x13e). SEH-gardé (POD only).
void CloseNativeCashShop() {
  __try {
    uiwnd::CloseWindow(kWinCashShop);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Lit le pointeur de fenêtre valide (vtable vérifiée). SEH-gardé.
void* FindCashWnd() {
  __try {
    void* w = uiwnd::FindWindow(kWinCashShop);
    if (!w) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(w) != kCashVTable) return nullptr;
    return w;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ── Snap de resize : la fenêtre ne prend QUE des tailles tenant un nombre entier
// de colonnes/lignes de cartes (pas d'espace vide partiel). Le callback arrondit
// la taille demandée ; `chrome` (= fenêtre − zone cartes : panier + en-tête +
// bordures) est mesuré chaque frame pour convertir taille fenêtre <-> grille.
struct SnapState {
  float cardw = 172.0f, cardh = 100.0f, gap = 4.0f;
  float chromew = 0.0f, chromeh = 0.0f;
  bool  valid = false;
};
SnapState g_snap;
void SnapWindowSize(ImGuiSizeCallbackData* d) {
  const SnapState& s = g_snap;
  const float sx = s.cardw + s.gap, sy = s.cardh + s.gap;
  const float gw = d->DesiredSize.x - s.chromew;
  const float gh = d->DesiredSize.y - s.chromeh;
  int cols = static_cast<int>((gw + s.gap) / sx + 0.5f);  // arrondi
  int rows = static_cast<int>((gh + s.gap) / sy + 0.5f);
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  d->DesiredSize.x = s.chromew + cols * sx - s.gap;
  d->DesiredSize.y = s.chromeh + rows * sy - s.gap;
}

// Nom d'affichage du cash shop : le nom de la DB (itemcell::NameById, cache
// partagé) AMPUTÉ de son préfixe « Costume », redondant ici — l'onglet et la
// vignette disent déjà qu'on est dans les costumes.
//
// Ce raccourcissement reste LOCAL, et c'est délibéré : c'est une décision de
// présentation propre à cette fenêtre, pas une propriété du nom. Le mettre dans
// itemcell::NameById le servirait aux cinq autres fenêtres, qui, elles, veulent
// le nom exact. Simple arithmétique de pointeur sur une chaîne déjà en cache :
// rien à mémoriser en plus.
const char* ShortName(uint32_t id) {
  const char* nm = itemcell::NameById(id);
  if (std::strncmp(nm, "Costume ", 8) == 0) nm += 8;
  return nm;
}

// Filtre de la grille. Il était `static` DANS OnRenderUI ; il est remonté ici
// parce qu'un second appelant l'écrit désormais : `RevealItem` y pose le nom de
// l'objet qu'on vient de demander depuis un lien de chat. Sans ça, on ouvrirait
// la boutique sur un onglet de 2 500 cartes en laissant le joueur chercher celle
// dont il vient justement de dire qu'elle l'intéressait.
ImGuiTextFilter g_filter;



// La description passe par itemcell::OpenDescById : les items du cash shop ne
// sont pas encore à nous, il n'y a donc pas de nœud d'inventaire à passer.
// `view` (viewID) et `location` (equip point) sont ce qui déverrouille le bouton
// APERÇU de la fenêtre — ici on les a, et ils comptent : c'est tout l'intérêt de
// pouvoir se voir porter un costume avant de l'acheter.
// Le chemin collection (wnd+0x1c4) est bâti par GetResName(info) : avec le flag
// « identifié » que pose OpenDescById, il pointe sur
// 유저인터페이스\collection\<resname>.bmp.

// ── Le message d'un résultat d'achat, EXACTEMENT celui du client ─────────────
//
// La table result -> identifiant de message est relevée sur le handler natif
// `Recv_ZC_SE_PC_BUY_CASHITEM_RESULT` (0x00CD3B70), qu'on continue d'OBSERVER : il
// écrit déjà ces chaînes dans le chat (et une modale au-delà de 8). On les affiche
// AUSSI dans la fenêtre, parce que c'est là que le joueur vient de cliquer — le
// chat, il ne le regarde pas à ce moment-là.
//
// 🔴 Ce qu'il y avait avant : « Achat refuse (12) ». Un générique avec un code, à
// côté du message exact que le client, lui, savait dire. C'est précisément ce que
// la règle « message serveur EXACT » interdit.
//
// Rien en dur : msgstr lit la table du client (data\msgstringtable.txt), donc la
// langue suit celle du client, comme partout ailleurs dans Bourgeon.
// Les MSI_* du client (data\msgstringtable.txt ; les textes cités sont ceux de
// moonlight, qui a sa propre table — d'où « Vote » plutôt que « Cash »).
constexpr int kMsiCash                 = 0x51b;  // MSI_CASH               "Vote"
constexpr int kMsiCashShop             = 0xc41;  // MSI_CASHSHOP           "Vote Shop"
constexpr int kMsiCashShopFreePoint    = 0xce7;  // ..._FREE_POINT         "Event points"
constexpr int kMsiCashShopFreePointToUse = 0xce8;  // ..._FREE_POINT_TO_USE "Use Event pts"
constexpr int kMsiDealFail             = 0x039;  // "The deal has failed."
constexpr int kMsiCashFailedBuySome    = 0x716;  // "Some items could not be purchased."
constexpr int kMsiCashFailedRuneOver   = 0x798;  // "Failed purchase of runes, items exceed…"
constexpr int kMsiCashFailedItemOver   = 0x799;  // "Exceeded the number of individual items…"
constexpr int kMsiCashFailedUnknown    = 0x79a;  // "Purchase failed due to an unknown error."
constexpr int kMsiCashFailedBusy       = 0x79b;  // "Please try again later."
constexpr int kMsiResultErrorVtc303    = 0xf64;  // "Your billing session has expired…"
constexpr int kMsiResultErrorVtc304    = 0xf65;  // "An error has occurred. please try again"

int BuyResultMsgId(int result) {
  // Les codes sont l'enum CASHSHOP_BUY_RESULT du serveur (cashshop.hpp) ; la
  // correspondance vers les MSI_* est celle du natif, relevée sur son handler.
  switch (result) {
    case 0x0: return 0;  // SUCCESS — le natif ne dit rien, nous non plus
    case 0x8: return kMsiCashFailedBuySome;   // ERROR_SOME_BUY_FAILURE
    case 0x9: return kMsiCashFailedRuneOver;  // ERROR_RUNE_OVERCOUNT
    case 0xa: return kMsiCashFailedItemOver;  // ERROR_EACHITEM_OVERCOUNT
    case 0xb: return kMsiCashFailedUnknown;   // ERROR_UNKNOWN
    case 0xc: return kMsiCashFailedBusy;      // ERROR_BUSY
    // Erreurs de facturation. Le natif préfixe le nom de l'objet ; le nôtre est
    // déjà à l'écran, juste au-dessus de la ligne d'état.
    case 303: return kMsiResultErrorVtc303;
    case 304: return kMsiResultErrorVtc304;
    // ⚠ 1 à 7 tombent TOUS sur « The deal has failed. », et cette imprécision est
    // celle du CLIENT, pas la nôtre : son handler n'a pas d'autre message pour
    // eux. Ce sont pourtant les cas les plus courants —
    //   1 ERROR_SYSTEM · 2 ERROR_SHORTTAGE_CASH (pas assez de points) ·
    //   3 ERROR_UNKONWN_ITEM · 4 ERROR_INVENTORY_WEIGHT (surpoids) ·
    //   5 ERROR_INVENTORY_ITEMCNT (sac plein) · 6 ERROR_PC_STATE ·
    //   7 ERROR_OVER_PRODUCT_TOTAL_CNT
    // On garde quand même SON message : la table de messages n'a rien de plus
    // précis pour ces codes (vérifié sur toute la famille MSI_CASH*), et notre
    // fenêtre dirait alors autre chose que la ligne que ce même handler écrit
    // dans le chat. Le jour où on voudra mieux, ça se règle en AJOUTANT des
    // entrées à msgstringtable, pas en écrivant du texte ici.
    default:  return kMsiDealFail;
  }
}

// Labels des catégories (e_cash_shop_tab, serveur cashshop.hpp).
const char* kTabLabels[] = {"Nouveautés", "Populaire", "Limité", "Location",
                            "Permanent", "Parchemins", "Consommables",
                            "Divers",     "Soldes"};
// Onglets AFFICHÉS (l'index serveur reste 0..8 pour la réception 0x08ca / l'achat
// 0x848 ; on masque juste ceux toujours vides). Cachés : Limite/Location/Permanent/
// Parchemins/Soldes.
const bool kTabShown[] = {true, true, false, false, false, false, true, true, false};

// Emplacement d'équipement d'un item depuis son masque `location` (EQP_*, =
// pc_equippoint). Renvoie {clé d'ordre stable, label} ; {99,"Autre"} = non
// équipable (consommable...) ou slot inconnu. Costumes prioritaires (les items
// cash sont majoritairement des costumes/coiffes).
// Clé de filtre virtuelle : "Costumes hat-effect" = costumes sans viewID rendus
// via effet .str (ItemToHatOrdinal != 0). N'existe pas dans SlotOf (dérivé de
// l'emplacement) -> clé dédiée hors de la plage des slots réels (0..13, 99).
constexpr int kSlotHatEffect = 100;

struct Slot { int key; const char* label; };
Slot SlotOf(uint32_t e) {
  if (e & 0x2000)            return {13, i18n::Tr("Costume garment")};    // COSTUME_GARMENT
  if (e & (0x0400 | 0x0800 | 0x1000))
                            return {10, i18n::Tr("Costume head")};        // COSTUME_HEAD_*
  if (e & 0x0100)            return {0,  i18n::Tr("Head top")};           // HEAD_TOP
  if (e & 0x0200)            return {1,  i18n::Tr("Head mid")};           // HEAD_MID
  if (e & 0x0001)            return {2,  i18n::Tr("Head bot")};           // HEAD_LOW
  if (e & 0x0010)            return {3,  "Armor"};              // ARMOR
  if (e & 0x0004)            return {4,  "Garment"};            // GARMENT
  if (e & 0x0040)            return {5,  "Shoes"};              // SHOES
  if (e & (0x0008 | 0x0080)) return {6,  "Accessory"};          // ACC L/R
  if (e & 0x0020)            return {7,  "Shield"};             // HAND_L
  if (e & 0x0002)            return {8,  "Weapon"};             // HAND_R
  if (e & 0x8000)            return {9,  "Ammunition"};         // AMMO
  return {99, "Other"};
}

}  // namespace

//  Opcodes cash shop (vanilla, < 0x0C35 -> handler natif intact, on OBSERVE) 
// NOTE (RE 2026-07-05) : sur ce packetver (20250716) le peuplement se fait par
// **ZC_ACK_SCHEDULER_CASHITEM 0x08ca** (un paquet par onglet), déclenché par la
// list-request **0x08c9** (2 octets) que le client natif envoie à l'ouverture.
// Le couple CZ_REQ_SE_CASH_TAB_CODE 0x846 / ZC_ACK_SE_CASH_ITEM_LIST2 0x8c0 est
// du code serveur mort ici (#if PACKETVER 2011 uniquement) -> ne rien en attendre.
constexpr uint16_t kOpOpen     = 0x0b6e;  // ZC_SE_CASHSHOP_OPEN (points)
constexpr uint16_t kOpItemList = 0x08ca;  // ZC_ACK_SCHEDULER_CASHITEM (items/onglet)
constexpr uint16_t kOpResult   = 0x0849;  // ZC_SE_PC_BUY_CASHITEM_RESULT
// Envois (CZ).
constexpr uint16_t kOpListReq  = 0x08c9;  // list request -> déclenche les 0x08ca
// CZ_SE_CASHSHOP_OPEN2 [op:2][tab:4] : la demande d'OUVERTURE, celle que le bouton
// du menu envoie. C'est le serveur qui ouvre — il répond ZC 0x0B6E, que l'on
// remplace. 🔴 Il pose aussi `sd->npc_shopid = -1` : toute ouverture engage donc
// la fermeture CZ 0x084A, sans quoi le personnage reste bloqué (cf. CloseShop).
constexpr uint16_t kOpOpenReq  = 0x0b6d;  // CZ_SE_CASHSHOP_OPEN2
constexpr uint16_t kOpBuy      = 0x0848;  // CZ_SE_PC_BUY_CASHITEM_LIST
constexpr uint16_t kOpClose    = 0x084a;  // CZ cashshop close (2 octets)
// Changement de map / de serveur. 🔴 Ce n'était pas notre affaire tant que le
// viewer suivait la fenêtre NATIVE : le client la détruisait au warp, et notre
// `open_` retombait tout seul. La native ne naissant plus, plus personne ne nous
// dit que la session est morte — un warp en pleine boutique (téléporteur, @load,
// carte de warp) laisserait le viewer à l'écran, à acheter dans le vide.
constexpr uint16_t kOpMapChange  = 0x0091;  // ZC_NPCACK_MAPMOVE
constexpr uint16_t kOpServerMove = 0x0092;  // ZC_NPCACK_SERVERMOVE

CashShopWindow::CashShopWindow() {
  // ── ZC_SE_CASHSHOP_OPEN : on prend sa place ────────────────────────────────
  //
  // 🔴 REMPLACEMENT. Ce paquet est le SEUL créateur vivant de la fenêtre native
  // (RE 2026-08-01 : `Recv_ZC_SE_CASHSHOP_OPEN` 0x00D0BC80 — MakeWindow(0x13E)
  // puis OnMsg 0xA4 = onglet et OnMsg 0x78 = points). Les deux autres créateurs
  // du dispatcher, cases 0x0845 et 0x0A2B @0x00CA4461, sont les opcodes HÉRITÉS :
  // moonlight choisit 0x0B6E dès PACKETVER_MAIN >= 20200129 (packets_struct.hpp),
  // ils ne partent jamais — les remplacer serait du code mort.
  //
  // Empêcher la fenêtre de NAÎTRE est le seul état sûr : masquée, une native garde
  // le CLAVIER et son bouton par défaut agit — ici, ce bouton achète.
  //
  // Paquet de longueur FIXE (14 o) : le remplacement n'est possible que grâce au
  // résolveur de longueur du client (cf. reference_native_packet_len_resolver).
  Bourgeon::Instance().RegisterReplaceOpcode(
      kOpOpen, [this] { return imgui_enabled_; });
  // Warp / changement de serveur : la session d'achat est morte côté serveur.
  Bourgeon::Instance().RegisterObserveOpcode(kOpMapChange, 4);
  Bourgeon::Instance().RegisterObserveOpcode(kOpServerMove, 4);
  // ZC_ACK_SCHEDULER_CASHITEM (var) : [len:2][count:2][tabNum:2] = 6 octets pour
  // atteindre l'en-tête ; les items sont lus directement dans le buffer live.
  Bourgeon::Instance().RegisterObserveOpcode(kOpItemList, 6);
  // ZC_SE_PC_BUY_CASHITEM_RESULT : [itemId:4][result:2][cash:4][kafra:4] = 14 o.
  Bourgeon::Instance().RegisterObserveOpcode(kOpResult, 14);
}

// Fil RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h). La liste d'onglet
// est à longueur ANNONCÉE et son décodage lit tout le corps : PushAnnounced.
void CashShopWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                  uint16_t len) {
  if (opcode == kOpItemList) net_inbox_.PushAnnounced(opcode, data, len);
  else                       net_inbox_.Push(opcode, data, len);
}

// Fil PRINCIPAL : le décodage, rejoué à chaque frame, dans l'ordre d'arrivée.
// Il compte ici : un onglet découpé en plusieurs paquets s'APPEND, un paquet rejoué
// dans le désordre mélangerait deux onglets.
void CashShopWindow::HandlePacket(uint16_t opcode, const uint8_t* data,
                                  uint16_t len) {
  if (opcode == kOpMapChange || opcode == kOpServerMove) {
    if (!open_) return;
    // Warp : le BLOCAGE, lui, est déjà levé — `unit_remove_map_` remet
    // `sd->npc_shopid` à zéro en quittant la carte (unit.cpp). Le personnage n'est
    // donc pas coincé, et c'est le point important.
    //
    // On envoie quand même la fermeture sur un changement de CARTE, et c'est un
    // écart ASSUMÉ avec le natif : `sd->state.cashshop_open`, lui, n'est remis à
    // zéro QUE par `clif_parse_cashshop_close`. Le client officiel détruit sa
    // fenêtre au warp sans rien envoyer et laisse donc ce drapeau en l'air — après
    // quoi @cash et @points répondent « Please close the cashshop before using this
    // command » jusqu'à la prochaine ouverture-fermeture. Deux octets le règlent.
    //
    // Pas sur un changement de SERVEUR (0x0092) : la socket est en train d'être
    // défaite, et la session repart de zéro de l'autre côté.
    if (opcode == kOpMapChange) {
      uint16_t op = kOpClose;
      Bourgeon::Instance().SendPacket(reinterpret_cast<uint8_t*>(&op), sizeof(op));
    }
    open_ = false;
    was_open_ = false;
    show_panel_ = true;
    return;
  }
  if (opcode == kOpOpen) {
    if (len < 8) return;
    // 🔴 CE PAQUET EST UNE BASCULE, et c'est le piège de ce handler. Le natif
    // (`Recv_ZC_SE_CASHSHOP_OPEN` 0x00D0BC80) commence par SaveRectAndCloseWindow
    // (0x13E) : si une fenêtre existe, il la DÉTRUIT et SORT — le paquet ne rouvre
    // rien. Même forme que la banque (0x09A6), et le bouton du menu, qui envoie
    // toujours son CZ 0x0B6D, compte là-dessus pour refermer.
    if (open_) { CloseShop(); return; }
    cash_points_  = *reinterpret_cast<const uint32_t*>(data);
    kafra_points_ = *reinterpret_cast<const uint32_t*>(data + 4);
    // Onglet demandé (le natif le pose par OnMsg 0xA4). On ne le suit que s'il est
    // AFFICHÉ : le serveur peut nommer un onglet qu'on masque, et atterrir sur une
    // page vide sans un mot serait pire que d'ignorer sa préférence.
    if (len >= 12) {
      const uint32_t tab = *reinterpret_cast<const uint32_t*>(data + 8);
      if (tab < static_cast<uint32_t>(kNumTabs) && kTabShown[tab])
        cur_tab_ = static_cast<int>(tab);
    }
    open_ = true;
    need_pos_ = true;
    show_panel_ = true;
    last_result_ = -1;
    last_recv_tab_ = -1;  // nouvelle salve de listes -> le 1er paquet videra son onglet
    // 🔴 DEVOIR HÉRITÉ. Le handler natif n'envoie PAS la list-request : elle part de
    // l'UI de sa fenêtre, qui ne naît plus. Sans elle, les onglets restent vides à
    // la toute première ouverture de la session (le serveur ne l'envoie qu'une fois,
    // `cashshop_sent`). C'est donc à nous, maintenant.
    RequestCatalogue();
    // Ouverture demandée depuis un lien d'objet : on l'amène sous les yeux du
    // joueur. On écrase l'onglet que le serveur a nommé, et c'est voulu — le
    // joueur a désigné un OBJET, pas une catégorie.
    if (pending_item_ != 0) {
      const uint32_t id = pending_item_;
      pending_item_ = 0;
      int tab = 0;
      int32_t price = 0;
      if (FindItem(id, &tab, &price)) RevealItem(id, tab, price);
    }
    return;
  }
  if (opcode == kOpItemList) {
    // data = [packetLength:2][count:2][tabNum:2][items...] (ZC_ACK_SCHEDULER_CASHITEM,
    // un paquet par onglet). packetLength inclut l'en-tête [opcode:2] -> le pas d'un
    // item = (packetLength - 8) / count : robuste au ENABLE_CASHSHOP_PREVIEW_PATCH
    // (item = itemId:4 + price:4 [+ viewSprite:2 + location:4]). data pointe dans le
    // buffer recv live (paquet complet présent) -> on lit au-delà des `len` forwardés.
    if (len < 6) return;
    const uint16_t plen  = *reinterpret_cast<const uint16_t*>(data);
    int32_t        count = *reinterpret_cast<const int16_t*>(data + 2);
    const uint16_t tab   = *reinterpret_cast<const uint16_t*>(data + 4);
    if (tab >= static_cast<uint16_t>(kNumTabs) || count <= 0) return;
    const int32_t body = static_cast<int32_t>(plen) - 8;  // octets d'items
    if (body <= 0) return;
    const int32_t stride = body / count;
    if (stride < 8) return;  // au moins itemId(4)+price(4)
    if (count > 8192) count = 8192;
    // 1er paquet de CET onglet (tabNum != précédent) -> on repart de zéro ;
    // sinon = continuation d'un onglet découpé -> on APPEND (ne pas écraser).
    if (last_recv_tab_ != static_cast<int>(tab)) {
      tabs_[tab].clear();
      last_recv_tab_ = static_cast<int>(tab);
    }
    tabs_[tab].reserve(tabs_[tab].size() + count);
    const uint8_t* p = data + 6;  // 1er item
    for (int32_t i = 0; i < count; ++i) {
      CashItem ci;
      ci.id    = *reinterpret_cast<const uint32_t*>(p);
      ci.price = *reinterpret_cast<const int32_t*>(p + 4);
      // Preview patch : itemId:4 + price:4 + viewSprite:2 + location:4 -> stride>=14.
      if (stride >= 10) ci.view = *reinterpret_cast<const uint16_t*>(p + 8);
      if (stride >= 14) ci.location = *reinterpret_cast<const uint32_t*>(p + 10);
      tabs_[tab].push_back(ci);
      p += stride;
    }
    return;
  }
  if (opcode == kOpResult) {
    // data = [itemId:4][result:2][cash:4][kafra:4]. result @ +4.
    if (len < 6) return;
    last_result_  = *reinterpret_cast<const uint16_t*>(data + 4);
    if (len >= 14) {
      cash_points_  = *reinterpret_cast<const uint32_t*>(data + 6);
      kafra_points_ = *reinterpret_cast<const uint32_t*>(data + 10);
    }
    if (last_result_ == 0) cart_.clear();  // succès -> panier vidé
    return;
  }
}

// Demande la liste des items du cash shop (2 octets = juste l'opcode). Le serveur
// répond par une série de ZC_ACK_SCHEDULER_CASHITEM 0x08ca (un par onglet rempli),
// mais UNE SEULE FOIS par session (flag `cashshop_sent`, remis à zéro à chaque
// authentification — pc.cpp).
void CashShopWindow::RequestCatalogue() {
  uint16_t op = kOpListReq;
  Bourgeon::Instance().SendPacket(reinterpret_cast<uint8_t*>(&op), sizeof(op));
}

// ── Le catalogue vu de l'extérieur ───────────────────────────────────────────
//
// Un balayage linéaire des onglets AFFICHÉS. Quelques milliers d'entrées au pire,
// et l'appel vient d'un menu contextuel qu'on ouvre à la main : pas de quoi
// entretenir un index qu'il faudrait ensuite tenir à jour à chaque 0x08CA.
bool CashShopWindow::FindItem(uint32_t id, int* tab, int32_t* price) const {
  if (id == 0) return false;
  for (int t = 0; t < kNumTabs; ++t) {
    if (!kTabShown[t]) continue;  // onglet masqué : on ne peut pas y emmener
    for (const auto& ci : tabs_[t]) {
      if (ci.id != id) continue;
      if (tab)   *tab = t;
      if (price) *price = ci.price;
      return true;
    }
  }
  return false;
}

// Amène la vue sur l'objet : onglet, filtres, panier. Suppose `tabs_` déjà peuplé
// et la boutique ouverte (c'est l'appelant qui s'en assure).
void CashShopWindow::RevealItem(uint32_t id, int tab, int32_t price) {
  force_tab_ = tab;   // la barre d'onglets tient sa propre sélection : cf. le .h
  cur_tab_   = tab;
  cur_slot_  = -1;    // un filtre d'emplacement resté d'avant le cacherait
  // Filtre posé sur son nom : l'onglet peut compter des milliers de cartes, et
  // « ajouté au panier » sans le montrer laisserait le joueur devant une grille
  // où rien n'a l'air d'avoir bougé.
  std::snprintf(g_filter.InputBuf, IM_ARRAYSIZE(g_filter.InputBuf), "%s",
                ShortName(id));
  g_filter.Build();
  AddToCart(id, tab, price);
}

// Ouvre la boutique sur un objet précis. Depuis un lien de chat, une table de
// drops — partout où l'on parle d'un objet sans être dans la boutique.
bool CashShopWindow::OpenWithItem(uint32_t id) {
  if (!imgui_enabled_) return false;  // sans notre fenêtre, rien à piloter
  int tab = 0;
  int32_t price = 0;
  if (!FindItem(id, &tab, &price)) return false;
  if (open_) { RevealItem(id, tab, price); return true; }
  // Fermée : on ne peut pas l'ouvrir nous-mêmes. C'est le SERVEUR qui ouvre (il
  // répond ZC 0x0B6E, notre handler), et il faut passer par lui — c'est là qu'il
  // ouvre la session d'achat côté personnage. L'objet attend jusque-là.
  pending_item_ = id;
  uint8_t pkt[6];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpOpenReq;
  *reinterpret_cast<uint32_t*>(pkt + 2) = static_cast<uint32_t>(tab);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  return true;
}

void CashShopWindow::AddToCart(uint32_t id, int tab, int32_t price) {
  for (auto& e : cart_) {
    if (e.id == id && e.tab == tab) { ++e.amount; return; }
  }
  cart_.push_back(CartEntry{id, tab, 1, price});
}

// CZ_SE_PC_BUY_CASHITEM_LIST 0x848 :
//   [type:2][packetLength:2][count:2][kafraPoints:4][ {id:4,amount:4,tab:2} *count ]
void CashShopWindow::SendBuy() {
  if (cart_.empty()) return;
  const int count = static_cast<int>(cart_.size());
  const int plen = 10 + 10 * count;
  // kafraPoints = montant EXACT de points d'Event à dépenser (le serveur paie le
  // reste `price - kafraPoints` en points de Vote : cash = price - points, SANS
  // borner points au prix). Donc envoyer tout le solde draine tout + fausse le
  // Vote. On dépense au plus le total du panier (min(total, solde)).
  long long total = 0;
  for (const auto& e : cart_)
    total += static_cast<long long>(e.price) * e.amount;
  uint32_t kafra_to_spend = 0;
  if (use_kafra_) {
    const long long cap = std::min<long long>(total, kafra_points_);
    kafra_to_spend = static_cast<uint32_t>(cap < 0 ? 0 : cap);
  }
  std::vector<uint8_t> pkt(plen);
  uint8_t* p = pkt.data();
  *reinterpret_cast<uint16_t*>(p + 0) = kOpBuy;
  *reinterpret_cast<uint16_t*>(p + 2) = static_cast<uint16_t>(plen);
  *reinterpret_cast<uint16_t*>(p + 4) = static_cast<uint16_t>(count);
  *reinterpret_cast<uint32_t*>(p + 6) = kafra_to_spend;
  uint8_t* it = p + 10;
  for (const auto& e : cart_) {
    *reinterpret_cast<uint32_t*>(it + 0) = e.id;
    *reinterpret_cast<uint32_t*>(it + 4) = static_cast<uint32_t>(e.amount);
    *reinterpret_cast<uint16_t*>(it + 8) = static_cast<uint16_t>(e.tab);
    it += 10;
  }
  Bourgeon::Instance().SendPacket(pkt.data(), pkt.size());
}

// Achat 1-clic : 1 unité de `id` (tab `tab`), puis fermeture du shop. Paquet 0x848
// à 1 item (indépendant du panier) + fermeture (CZ 0x084a + destruction native),
// comme le bouton X. kafraPoints = min(prix, solde Event) si l'option est cochée.
void CashShopWindow::BuyNow(uint32_t id, int tab, int32_t price) {
  uint8_t pkt[20];
  const uint16_t plen = 20;  // 10 (en-tête) + 10 (1 item)
  uint32_t kafra_to_spend = 0;
  if (use_kafra_) {
    const long long cap = std::min<long long>(price, kafra_points_);
    kafra_to_spend = static_cast<uint32_t>(cap < 0 ? 0 : cap);
  }
  *reinterpret_cast<uint16_t*>(pkt + 0)  = kOpBuy;
  *reinterpret_cast<uint16_t*>(pkt + 2)  = plen;
  *reinterpret_cast<uint16_t*>(pkt + 4)  = 1;  // count
  *reinterpret_cast<uint32_t*>(pkt + 6)  = kafra_to_spend;
  *reinterpret_cast<uint32_t*>(pkt + 10) = id;
  *reinterpret_cast<uint32_t*>(pkt + 14) = 1;  // amount
  *reinterpret_cast<uint16_t*>(pkt + 18) = static_cast<uint16_t>(tab);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  // Fermeture (idem bouton X) : le serveur clôt la session d'achat et débloque le
  // personnage. L'ordre TCP garantit que l'achat part AVANT la fermeture.
  CloseShop();
}

// Ferme le cash shop, côté serveur d'abord.
//
// 🔴 CZ_SE_CASHSHOP_CLOSE 0x084A n'est PAS décoratif : à l'ouverture, le serveur
// pose `sd->npc_shopid = -1` (clif_parse_cashshop_open_request) — le même champ que
// la boutique NPC, celui que teste `pc_cant_act2()`. Sans ce paquet, le joueur
// reste BLOQUÉ après la fermeture : il ne peut plus ni marcher ni attaquer. Seul
// `clif_parse_cashshop_close` le remet à zéro.
void CashShopWindow::CloseShop() {
  uint16_t op = kOpClose;
  Bourgeon::Instance().SendPacket(reinterpret_cast<uint8_t*>(&op), sizeof(op));
  open_ = false;
  was_open_ = false;
  show_panel_ = true;
  // 🔴 L'attente meurt avec la fermeture. ZC 0x0B6E est une BASCULE : si le
  // joueur ferme la boutique entre notre demande et la réponse, ce même paquet
  // passe ici — et l'objet en attente serait révélé à la PROCHAINE ouverture,
  // celle-là voulue pour tout autre chose.
  pending_item_ = 0;
  // Filet : une native peut traîner si l'interrupteur vient d'être allumé sur un
  // shop natif déjà ouvert. On la DÉTRUIT — masquée, elle garderait le clavier.
  CloseNativeCashShop();
}

void CashShopWindow::OnTick() {
  // ── Le catalogue, avant qu'on en ait besoin ─────────────────────────────────
  //
  // Le serveur n'envoie la liste que sur demande (CZ 0x08C9) et ne la renvoie
  // jamais (`cashshop_sent`). Tant que la demande partait à la première ouverture
  // de la boutique, on ne pouvait rien dire d'un objet avant que le joueur ne
  // l'ait ouverte — soit, en pratique, jamais. On la pose donc à l'entrée en jeu.
  //
  // ⚠ Cela CONSOMME la réponse unique de la session. Sans effet sur la boutique
  // native, qui ne naît plus quand l'interface moderne est active (son paquet
  // créateur est remplacé) — et c'est pourquoi la demande reste sous cet
  // interrupteur : allumé, la native est hors jeu ; éteint, on n'envoie rien et
  // elle fait sa propre demande comme avant.
  const bool game_active = Bourgeon::Instance().IsGameActive();
  if (!game_active && prev_game_active_) {
    // Retour au choix de personnage / déconnexion : la prochaine authentification
    // remettra `cashshop_sent` à zéro côté serveur (pc.cpp), donc nous aussi.
    catalogue_requested_ = false;
  }
  prev_game_active_ = game_active;

  // ── Basculement de l'interrupteur, les DEUX sens ────────────────────────────
  // Basculer FERME le shop. On ne reprend pas une session qu'on n'a pas vue naître
  // (OFF -> ON : le paquet d'ouverture est passé au natif, on ne sait rien de ses
  // points ni de son onglet) et on ne laisse pas non plus le natif finir. Fermer
  // est ici l'issue neutre — et surtout, elle débloque le personnage.
  if (imgui_enabled_ != prev_imgui_enabled_) {
    prev_imgui_enabled_ = imgui_enabled_;
    if (open_ || FindCashWnd()) CloseShop();
    net_inbox_.Clear();  // ce qui reste appartient à la session abandonnée
    open_ = false;
    was_open_ = false;
  }
  if (!imgui_enabled_) { open_ = false; was_open_ = false; return; }

  // La demande, une fois par session, une fois le monde en place. On attend la
  // fin du chargement de carte : pendant, le HUD natif se démonte et se remonte,
  // et rien de ce qu'on envoie là n'a de raison de partir en premier.
  if (game_active && !catalogue_requested_ &&
      !Bourgeon::Instance().IsMapLoading()) {
    RequestCatalogue();
    catalogue_requested_ = true;
  }

  // Filet du basculement en pleine session : on DÉTRUIT ce qui traîne au lieu de
  // le masquer. Idempotent, et sans effet dans le cas normal (rien ne naît plus).
  if (open_ && FindCashWnd()) CloseNativeCashShop();

  was_open_ = open_;
}

void CashShopWindow::OnRenderUI() {
  if (!open_ || !imgui_enabled_) return;

  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(spawn_x_),
                                   static_cast<float>(spawn_y_)),
                            ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(680, 500), ImGuiCond_FirstUseEver);
  // Resize par PALIERS : la taille saute d'une colonne/ligne de cartes à la fois
  // (aucun espace vide partiel). Le chrome est mesuré la frame précédente.
  if (g_snap.valid) {
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(g_snap.chromew + g_snap.cardw, g_snap.chromeh + g_snap.cardh),
        ImVec2(10000.0f, 10000.0f), SnapWindowSize);
  }

  // Même style de fenêtre que MoonlightUi (cadre arrondi / roundframe).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);  // grille/panier/cartes arrondis
  // Titre = MSI_CASHSHOP, lu dans la table du client. « Vote Shop » y était recopié
  // à la main : moonlight a rebaptisé toute la famille (Cash -> Vote) dans SA table,
  // et une prochaine retouche là-bas doit se voir ici sans recompiler.
  char title[96];
  const char* shop_name = msgstr::Utf8(kMsiCashShop);
  std::snprintf(title, sizeof(title), "%s###bourgeon_cashshop",
                (shop_name && shop_name[0]) ? shop_name : i18n::Tr("Vote Shop"));
  const bool begun =
      ro::BeginRoWindow(title, &show_panel_, ImGuiWindowFlags_NoCollapse);
  if (!show_panel_) {
    // X (ou Échap) -> on FERME réellement le cash shop : CZ 0x084A, qui remet
    // `npc_shopid` à zéro côté serveur et débloque le personnage. CloseShop remet
    // show_panel_ à true pour la prochaine ouverture.
    CloseShop();
    ro::EndRoWindow();
    ImGui::PopStyleVar(5);
    return;
  }
  if (!begun) { ro::EndRoWindow(); ImGui::PopStyleVar(5); return; }

  //  En-tête : points du compte
  // Les trois libellés sortent de la table du client (MSI_CASH,
  // MSI_CASHSHOP_FREE_POINT, MSI_CASHSHOP_FREE_POINT_TO_USE) : ce sont les noms que
  // MOONLIGHT donne à ses deux monnaies, pas des termes génériques. Les recopier ici
  // aurait figé « Vote » et « Event points » dans le binaire.
  const ImVec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);  // texte noir (skin RO clair)
  const char* lbl_cash  = msgstr::Utf8(kMsiCash);
  const char* lbl_free  = msgstr::Utf8(kMsiCashShopFreePoint);
  const char* lbl_usef  = msgstr::Utf8(kMsiCashShopFreePointToUse);
  ImGui::TextColored(kBlack, "%s: %u",
                     (lbl_cash && lbl_cash[0]) ? lbl_cash : "Vote", cash_points_);
  ImGui::SameLine();
  ImGui::TextColored(kBlack, " | %s: %u",
                     (lbl_free && lbl_free[0]) ? lbl_free : i18n::Tr("Points d'Event"),
                     kafra_points_);
  ImGui::SameLine();
  ro::RoCheckbox((lbl_usef && lbl_usef[0]) ? lbl_usef : i18n::Tr("Points d'Event d'abord"),
                 &use_kafra_);
  if (last_result_ == 0) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), i18n::Tr(" | Achat OK"));
  } else if (last_result_ > 0) {
    // Le message EXACT du client, pas un code (cf. BuyResultMsgId). Repli sur le
    // code seulement si la table de messages ne rend rien — mieux vaut un numéro
    // que rien du tout quand on doit diagnostiquer.
    const char* why = msgstr::Utf8(BuyResultMsgId(last_result_));
    ImGui::SameLine();
    if (why && why[0])
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), " | %s", why);
    else
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), i18n::Tr(" | Achat refusé (%d)"),
                         last_result_);
  }
  ImGui::Separator();

  //  Onglets de catégorie 
  if (ImGui::BeginTabBar("cashshop_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
    for (int t = 0; t < kNumTabs; ++t) {
      if (!kTabShown[t]) continue;  // onglet toujours vide -> masqué
      char lbl[48];
      std::snprintf(lbl, sizeof(lbl), "%s (%d)###cstab%d", i18n::Tr(kTabLabels[t]),
                    static_cast<int>(tabs_[t].size()), t);
      // Onglet imposé de l'extérieur (arrivée par un lien d'objet) : la barre
      // tient sa propre sélection et écraserait `cur_tab_` — il faut le lui dire.
      const ImGuiTabItemFlags tab_flags =
          (force_tab_ == t) ? ImGuiTabItemFlags_SetSelected : 0;
      if (ImGui::BeginTabItem(lbl, nullptr, tab_flags)) {
        if (cur_tab_ != t) cur_slot_ = -1;  // changer d'onglet -> filtre slot remis à Tous
        cur_tab_ = t;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  force_tab_ = -1;  // une seule frame : après quoi le joueur reprend la main

  ImGui::SetNextItemWidth(-1.0f);
  // Champ filtre avec texte d'aide grise (placeholder) quand il est vide. On pilote
  // directement le buffer du ImGuiTextFilter (InputBuf/Build) pour garder le filtrage
  // natif tout en ayant le hint (g_filter.Draw() n'expose pas de hint).
  if (ImGui::InputTextWithHint("##cs_filter", "Filtrer...", g_filter.InputBuf,
                               IM_ARRAYSIZE(g_filter.InputBuf)))
    g_filter.Build();

  //  Filtre par emplacement d'équipement (slots présents dans l'onglet)
  // Prédicat "costume hat-effect" : rendu par effet .str (ItemToHatOrdinal != 0),
  // typiquement un costume SANS viewID propre. O(1) (lookup map dans basic_info).
  auto* bi = Bourgeon::Instance().basic_info();
  auto IsHatEffect = [bi](const CashItem& ci) {
    return bi && bi->ItemToHatOrdinal(static_cast<int>(ci.id)) != 0;
  };
  std::vector<Slot> slots;
  bool has_hateffect = false;
  for (const auto& ci : tabs_[cur_tab_]) {
    if (IsHatEffect(ci)) has_hateffect = true;
    const Slot s = SlotOf(ci.location);
    bool seen = false;
    for (const auto& x : slots) if (x.key == s.key) { seen = true; break; }
    if (!seen) {
      size_t p = slots.size();  // insertion triée par clé
      while (p > 0 && slots[p - 1].key > s.key) --p;
      slots.insert(slots.begin() + p, s);
    }
  }
  // Entrée virtuelle "Costumes hat-effect" (clé 100 > toutes les clés réelles ->
  // en fin de liste), présente seulement si l'onglet en contient.
  if (has_hateffect) slots.push_back({kSlotHatEffect, i18n::Tr("Costumes hat-effect")});
  // Un seul slot "Autre" (99) => onglet non-équipable : pas de filtre utile.
  const bool slot_filter_useful = slots.size() > 1;
  if (slot_filter_useful) {
    const char* cur_label = "Emplacement: tous";
    if (cur_slot_ != -1) {
      bool found = false;
      for (const auto& s : slots)
        if (s.key == cur_slot_) { cur_label = s.label; found = true; break; }
      if (!found) cur_slot_ = -1;  // slot disparu -> Tous
    }
    ImGui::SetNextItemWidth(200.0f);
    if (ro::RoBeginCombo("##cs_slot", cur_label)) {
      if (ImGui::Selectable("Emplacement: tous", cur_slot_ == -1)) cur_slot_ = -1;
      for (const auto& s : slots)
        if (ImGui::Selectable(s.label, cur_slot_ == s.key)) cur_slot_ = s.key;
      ro::RoEndCombo();
    }
  } else {
    cur_slot_ = -1;
  }

  // Tri : Nom / ID / Cout + sens ascendant/descendant.
  if (slot_filter_useful) ImGui::SameLine();
  ImGui::TextUnformatted("Tri");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  const char* kSortLabels[] = {"Nom", "ID", "Coût"};
  if (ro::RoBeginCombo("##cs_sort", kSortLabels[cur_sort_])) {
    for (int s = 0; s < 3; ++s)
      if (ImGui::Selectable(kSortLabels[s], cur_sort_ == s)) cur_sort_ = s;
    ro::RoEndCombo();
  }
  ImGui::SameLine();
  if (ro::RoButton(sort_asc_ ? "Asc" : "Desc")) sort_asc_ = !sort_asc_;

  //  Disposition : grille à gauche, panier à droite (comme le cash shop natif) 
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  // Panier auto-masqué quand il est vide -> la grille prend toute la largeur. Il
  // réapparaît dès qu'on ajoute un item et se re-masque après achat/vidage. (Avec
  // l'achat 1-clic, le panier ne sert qu'à l'achat groupé.)
  const bool   show_cart = !cart_.empty();
  const float  cart_w = 220.0f;
  const float  grid_w =
      show_cart ? std::max(120.0f, avail.x - cart_w - 8.0f) : 0.0f;
  // Taille de la fenêtre principale (pour mesurer le chrome du snap de resize).
  const float  main_win_w = ImGui::GetWindowWidth();
  const float  main_win_h = ImGui::GetWindowHeight();

  //  Grille d'items : cartes à TAILLE FIXE (child) -> le texte est borné à la
  // carte (sinon TextWrapped s'étale sur toute la fenêtre et casse la grille) 
  // Si un aperçu était survolé la frame précédente, on gèle le scroll-molette de la
  // grille -> la molette reste libre pour tourner le perso (rotation dans basic_info).
  ImGui::BeginChild("cs_grid", ImVec2(grid_w, 0), true,
                    preview_active_ ? ImGuiWindowFlags_NoScrollWithMouse : 0);
  bool preview_now = false;  // un aperçu survolé cette frame ?
  {
    const float card_w = 172.0f, card_h = 100.0f, gap = 4.0f, box = 78.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const float grid_inner_w = ImGui::GetContentRegionAvail().x;
    const float grid_inner_h = ImGui::GetContentRegionAvail().y;
    // Mesure du chrome (fenetre - zone cartes) -> le callback de snap l'utilise la
    // frame suivante pour faire sauter la taille par colonne/ligne entiere.
    g_snap.cardw = card_w; g_snap.cardh = card_h; g_snap.gap = gap;
    g_snap.chromew = main_win_w - grid_inner_w;
    g_snap.chromeh = main_win_h - grid_inner_h;
    g_snap.valid = true;
    const int cols =
        std::max(1, static_cast<int>((grid_inner_w + gap) / (card_w + gap)));
    // Liste filtrée (ptrs) : une catégorie peut avoir 2500+ items -> on CLIPPE par
    // rangée (ImGuiListClipper) pour ne dessiner que les cartes visibles (sinon
    // 2500 child-windows/frame = chute de FPS).
    std::vector<const CashItem*> vis;
    vis.reserve(tabs_[cur_tab_].size());
    for (const auto& ci : tabs_[cur_tab_]) {
      if (cur_slot_ == kSlotHatEffect) {
        if (!IsHatEffect(ci)) continue;          // filtre "Costumes hat-effect"
      } else if (cur_slot_ != -1 && SlotOf(ci.location).key != cur_slot_) {
        continue;
      }
      if (g_filter.PassFilter(ShortName(ci.id))) vis.push_back(&ci);
    }
    // Tri (Nom / ID / Cout, asc/desc).
    std::sort(vis.begin(), vis.end(),
              [&](const CashItem* a, const CashItem* b) {
                int c;
                if (cur_sort_ == 1)
                  c = (a->id < b->id) ? -1 : (a->id > b->id ? 1 : 0);
                else if (cur_sort_ == 2)
                  c = (a->price < b->price) ? -1 : (a->price > b->price ? 1 : 0);
                else
                  c = std::strcmp(ShortName(a->id), ShortName(b->id));
                return sort_asc_ ? c < 0 : c > 0;
              });

    // Dessine une carte pour l'item ci.
    auto draw_card = [&](const CashItem& ci) {
      // Couleurs de carte pilotees par le skin RO (customisables + persistees).
      const ro::RoSkinConfig& sc = ro::SkinConfig();
      auto U32 = [](const float* c) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
      };
      const ImU32 card_bg   = U32(sc.card_col);
      const ImU32 card_head = U32(sc.card_head_col);
      const ImU32 card_txt  = U32(sc.card_head_text);
      ImGui::PushID(static_cast<int>(ci.id));
      ImGui::PushStyleColor(ImGuiCol_ChildBg, card_bg);  // fond carte (couleur skin)
      // Marges resserrees (image plus grande) ; le ChildRounding est global (fenetre).
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 3.0f));
      // NoScrollWithMouse : la carte ne capture PAS la molette -> le scroll va à la
      // grille parente (sinon survoler une carte casse la navigation du shop).
      ImGui::BeginChild("card", ImVec2(card_w, card_h), true,
                        ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoScrollWithMouse);
      // 1) Nom EN HAUT (texte foncé, coupé à la largeur de la carte).
      // Nom EN HAUT dans un BANDEAU plus fonce que la carte (isole du corps) :
      // bande remplie sur toute la largeur, du haut jusqu'au debut de la rangee du
      // bas, texte clair par-dessus.
      const float header_h = card_h - box;  // marge bas reduite -> image + grande
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 wp = ImGui::GetWindowPos();
      dl->AddRectFilled(wp, ImVec2(wp.x + card_w, wp.y + header_h),
                        card_head, 6.0f,
                        ImDrawFlags_RoundCornersTop);  // bandeau (couleur skin), coins hauts arrondis
      // Nom sur UNE SEULE ligne : au lieu de wrapper, on REDUIT la police pour tenir
      // dans la largeur de la carte (draw-list a taille custom). Centre verticalement
      // dans le bandeau. Plancher a 55% pour rester lisible (leger debord au pire).
      {
        const char* nm = ShortName(ci.id);
        ImFont* font = ImGui::GetFont();
        const float base = ImGui::GetFontSize();
        const float availw = card_w - 8.0f;
        const float tw = ImGui::CalcTextSize(nm).x;  // largeur a la taille de base
        float fsz = (tw > availw && tw > 0.0f) ? base * (availw / tw) : base;
        if (fsz < base * 0.55f) fsz = base * 0.55f;
        const ImVec2 tp(wp.x + 4.0f, wp.y + (header_h - fsz) * 0.5f);
        dl->AddText(font, fsz, tp, card_txt, nm);
      }
      // 2) Rangée du bas ancrée : image à GAUCHE, prix + Buy à DROITE.
      // Bloc du bas [image | prix+boutons] CENTRE sur tous les bords (coords contenu).
      const float pad_x = 5.0f, pad_y = 3.0f;   // = WindowPadding de la carte
      const float cont_w = card_w - 2.0f * pad_x;
      const float cont_h = card_h - 2.0f * pad_y;
      const float low_top = header_h - pad_y;    // Y contenu = bas de la bande
      const float LH = cont_h - low_top;         // hauteur de la zone basse
      const float frameH = ImGui::GetFrameHeight();
      const float sp = ImGui::GetStyle().ItemSpacing.y;
      const float gap2 = 8.0f;
      // Image de COLLECTION (art de preview), pas la petite icône d'inventaire :
      // c'est ce que le cash shop natif affiche, et c'est la raison d'être de la
      // vignette large. Repli automatique sur l'icône quand l'art n'existe pas.
      ro::IconTex ic = ro::ItemCollectionIcon(ci.id);
      const float img = LH - 10.0f;              // image un peu plus petite -> marges
      float iw = img, ih = img;
      if (ic.tex && ic.w > 0 && ic.h > 0) {
        const float s = img / std::max(ic.w, ic.h);
        iw = ic.w * s; ih = ic.h * s;
      }
      // Image a GAUCHE (cellule largeur `img`), centree verticalement dans la zone.
      // Teinte = luminosite du skin (title_brightness) ; l'alpha suit deja style.Alpha.
      // ImGui 1.92 : Image() n'a plus de tint_col -> ImageWithBg (bg transparent).
      const float ib = ro::SkinImageBrightness();
      const ImVec4 img_tint(ib, ib, ib, 1.0f);
      ImGui::SetCursorPos(ImVec2((img - iw) * 0.5f, low_top + (LH - ih) * 0.5f));
      if (ic.tex) ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(ic.tex),
                                     ImVec2(iw, ih), ImVec2(0, 0), ImVec2(1, 1),
                                     ImVec4(0, 0, 0, 0), img_tint);
      else        ImGui::Dummy(ImVec2(iw, ih));
      // Survol de l'image -> aperçu porté (viewID + emplacement) ET/OU hat effect :
      // basic_info rend le perso portant l'item (sprites capturés + effet .str superposé
      // pour les costumes SANS viewid). Molette = tourner.
      if (ImGui::IsItemHovered()) {
        if (auto* bi = Bourgeon::Instance().basic_info()) {
          const int ord = bi->ItemToHatOrdinal(static_cast<int>(ci.id));
          const bool can_sprite =
              ci.view != 0 && bi->CanPreview(static_cast<int>(ci.location));
          if (can_sprite || ord != 0) {
            bi->RenderItemPreviewTooltip(static_cast<int>(ci.view),
                                         static_cast<int>(ci.location), ord);
            preview_now = true;  // gele le scroll grille la frame suivante (rotation)
          }
        }
      }
      // Colonne DROITE = TOUT l'espace restant a droite de l'image : prix + 2
      // boutons pleine largeur (remplissent la colonne), le tout centre
      // verticalement, prix centre horizontalement sur la colonne.
      const float cx = img + gap2;
      const float colw = cont_w - cx;            // remplit jusqu'au bord droit
      const float col_h = ImGui::GetTextLineHeight() + 2.0f * frameH + 2.0f * sp;
      const float cy = low_top + (LH - col_h) * 0.5f;
      char pbuf[24];
      std::snprintf(pbuf, sizeof(pbuf), "%d pts", ci.price);
      const float tw = ImGui::CalcTextSize(pbuf).x;
      ImGui::SetCursorPos(ImVec2(cx + (colw > tw ? (colw - tw) * 0.5f : 0.0f), cy));
      ImGui::TextColored(kBlack, "%s", pbuf);
      // Grise Panier + Achat 1-Click si le solde ne couvre pas le prix de l'item.
      // Solde REEL selon la coche "Utiliser mes points d'Event d'abord" : cumul
      // Vote+Event si cochee, Vote seul sinon (= ce que l'achat depensera vraiment).
      const long long combined =
          static_cast<long long>(cash_points_) + kafra_points_;
      const long long avail =
          use_kafra_ ? combined : static_cast<long long>(cash_points_);
      const bool afford = static_cast<long long>(ci.price) <= avail;
      // Si cocher "Utiliser Event" suffirait a couvrir, on le suggere dans le tooltip.
      const bool event_helps =
          !use_kafra_ && static_cast<long long>(ci.price) <= combined;
      const char* why =
          event_helps
              ? i18n::Tr("Solde Vote insuffisant - cochez \"Utiliser Event\" pour cumuler") : i18n::Tr("Solde Vote + Event insuffisant pour cet item");
      if (!afford) ImGui::BeginDisabled();
      ImGui::SetCursorPos(ImVec2(cx, cy + ImGui::GetTextLineHeight() + sp));
      if (ro::RoButton("Panier", colw, frameH))
        AddToCart(ci.id, cur_tab_, ci.price);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(afford ? i18n::Tr("Ajouter au panier (achat groupe via Acheter)")
                                 : why);
      ImGui::SetCursorPos(
          ImVec2(cx, cy + ImGui::GetTextLineHeight() + frameH + 2.0f * sp));
      if (ro::RoButton("Achat 1-Click", colw, frameH))
        BuyNow(ci.id, cur_tab_, ci.price);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(afford ? i18n::Tr("Achat immédiat d'1 unité, puis fermeture du shop")
                                 : why);
      if (!afford) ImGui::EndDisabled();
      ImGui::EndChild();
      ImGui::PopStyleVar();    // WindowPadding (carte)
      ImGui::PopStyleColor();  // ChildBg
      if (mui::IsLastItemRightClicked()) {
        // DIFFÉRÉE au relâchement (itemcell::FlushDeferredDesc) : ouverte dès le
        // clic, un appui PROLONGÉ faisait passer la description DERRIÈRE nous.
        const ImVec2 mp = ImGui::GetMousePos();
        itemcell::DeferDescById(ci.id, ci.view, ci.location,
                                static_cast<int>(mp.x), static_cast<int>(mp.y));
      }
      ImGui::PopID();
    };

    const int n = static_cast<int>(vis.size());
    const int rows = (n + cols - 1) / cols;
    ImGuiListClipper clipper;
    clipper.Begin(rows, card_h + gap);
    while (clipper.Step()) {
      for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
        for (int c = 0; c < cols; ++c) {
          const int idx = r * cols + c;
          if (idx >= n) break;
          if (c > 0) ImGui::SameLine(0, gap);
          draw_card(*vis[idx]);
        }
      }
    }
    clipper.End();
    ImGui::PopStyleVar();
    if (n == 0) ImGui::TextDisabled(i18n::Tr("(aucun item dans cette catégorie)"));
  }
  ImGui::EndChild();
  preview_active_ = preview_now;  // gele le scroll grille tant qu'on survole un apercu

  //  Panier (à droite) 
  if (show_cart) {  // panier masqué quand vide -> grille pleine largeur
  ImGui::SameLine();
  ImGui::BeginChild("cs_cart", ImVec2(cart_w, 0), true);
  ImGui::TextUnformatted("Panier");
  ImGui::SameLine();
  if (!cart_.empty() && ro::RoButton("Vider")) cart_.clear();
  ImGui::Separator();
  long long total = 0;
  int remove = -1;
  ImGui::BeginChild("cs_cart_items", ImVec2(0, -64), false);
  for (int i = 0; i < static_cast<int>(cart_.size()); ++i) {
    CartEntry& e = cart_[i];
    total += static_cast<long long>(e.price) * e.amount;
    ImGui::PushID(1000 + i);
    ImGui::TextWrapped("%s", ShortName(e.id));
    // Controle quantite : [-] [champ] [+], petits boutons RO carres (skin bouton,
    // police inchangee). InputInt en step=0 -> pas de +/- natifs (non skinnes).
    const float step = ImGui::GetFrameHeight();  // bouton carre = hauteur de ligne
    if (ro::RoButton("-", step, step) && e.amount > 1) --e.amount;
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(42.0f);
    if (ImGui::InputInt("##qty", &e.amount, 0, 0)) {
      if (e.amount < 1) e.amount = 1;
      if (e.amount > 9999) e.amount = 9999;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (ro::RoButton("+", step, step) && e.amount < 9999) ++e.amount;
    // Sous-total (noir, centre verticalement sur la ligne des champs).
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kBlack, i18n::Tr("%lld pts"), static_cast<long long>(e.price) * e.amount);
    // Bouton supprimer : meme petit bouton RO carre, aligne a droite.
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
  ImGui::Text(i18n::Tr("Total: %lld pts"), total);
  // Solde depensable selon la coche "Utiliser Event" (cumul si cochee, Vote seul
  // sinon) : identique a la logique des cartes + a ce que l'achat depense reellement.
  const long long buy_avail =
      use_kafra_ ? static_cast<long long>(cash_points_) + kafra_points_
                 : static_cast<long long>(cash_points_);
  const bool afford = total <= buy_avail;
  if (cart_.empty()) ImGui::BeginDisabled();
  if (ro::RoButton(afford ? "Acheter" : i18n::Tr("Points insuffisants"),
                   ImGui::GetContentRegionAvail().x, 0))
    SendBuy();
  if (cart_.empty()) ImGui::EndDisabled();
  ImGui::EndChild();
  }  // if (show_cart) : panier masqué quand vide

  ro::EndRoWindow();
  ImGui::PopStyleVar(5);
}
