#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
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
#include "imgui.h"
#include "ui/imgui_escape.h"
#include "ui/ro_imgui.h"          // BeginRoWindow (skin RO)

// ── Constantes RE (client 20250716, base 0x400000 ; cf. project_npc_shop_re) ──
namespace {

// UIWindowMgr + factory.

// Fenêtres shop NPC (cf. project_npc_shop_re).
constexpr int kWinBuy    = 0x16;  // UIItemPurchaseWnd (vtable 0x0103cda0)
constexpr int kWinSell   = 0x17;  // UIItemSellWnd     (vtable 0x0103ce78)
constexpr int kWinDetail = 0x18;  // panneau détail item (ATK/DEF) — fermé nativement
                                  // avec 0x16/0x17 par les OnMsg shop
constexpr int kWinChoose = 0x19;  // UIChooseSellBuyWnd
constexpr uintptr_t kSellVTable   = 0x0103ce78;  // UIItemSellWnd (conteneur, id 0x17)
// La LISTE de vente n'est pas dans le conteneur 0x17 mais dans un sous-window
// (vtable 0x0103cbf0) pointé par le global g_ShopSellMirrorWnd_ptr = DAT_0131f738.
// Confirmé live : node = {index@+0x0c, qty@+0x18, prix@+0x1c/+0x20, itemId-en-texte
// @+0x34 (std::string), slots@+0x90}.
constexpr uintptr_t kSellListVTable = 0x0103cbf0;
constexpr uintptr_t kSellListGlobal = 0x0131f738;
constexpr uintptr_t kDetailVTable = 0x010323ec;  // UIItemParamChangeDisplayWnd
                                                 // (comparateur ATK/DEF, id variable)

// Offsets UIWindow.
constexpr int kOffList    = 0xe8;  // std::list<ItemSkillInfo> (buy/sell display)

// Nœud de la liste d'affichage (std::list) : value=node+8, puis dans le payload
// ItemSkillInfo : +0x04 index inv, +0x10 qté, +0x14/+0x18 prix, +0x2c nom
// (std::string), +0x88 slots (short). -> en offsets NŒUD : +0x0c/+0x18/+0x1c/
// +0x20/+0x34/+0x90.
constexpr int kNodeIndex = 0x0c;
constexpr int kNodeQty   = 0x18;
constexpr int kNodePrice = 0x1c;
constexpr int kNodePrice2 = 0x20;  // overcharge (prix de vente réel)
constexpr int kNodeName  = 0x34;   // std::string (MSVC : +0x10 size, +0x14 cap)
constexpr int kNodeSlots = 0x90;   // short

// ItemSkillInfo standalone (résolution nom/icône par id) — comme cashshop.
using InfoCtor_t  = void(__fastcall*)(void*);
using InfoSetId_t = void(__thiscall*)(void*, int);

// Description d'item (clic-droit) : MakeWindow(0xc) + OnMsg 0x18 (comme cashshop).
// Nom par id : DB de description (map id->record), name = *(rec+4).
using DescLookup_t   = void*(__cdecl*)(int, void*);
using EnsureLoaded_t = char (__thiscall*)(void*, int);

// Icône d'item (image d'inventaire).

// ── Fenêtres natives (SEH-gardé) ──
void* FindWnd(int id) {
  __try {
    return uiwnd::FindWindow(id);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void CloseWnd(int id) {
  __try {
    uiwnd::CloseWindow(id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void HideWnd(void* w) {
  __try {
    if (w) *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(w) + uiwnd::kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Cache nom d'item (id -> nom) ──
std::unordered_map<uint32_t, std::string> g_name_cache;

void ResolveNameSEH(uint32_t id, char* out, size_t cap) {
  out[0] = '\0';
  __try {
    void* cache = *reinterpret_cast<void**>(itemdb::kEnsureCachePtr);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(itemdb::kEnsureLoadedAddr)(cache, static_cast<int>(id));
    void* rec = reinterpret_cast<DescLookup_t>(itemdb::kLookupAddr)(
        static_cast<int>(id), reinterpret_cast<void*>(itemdb::kTableAddr));
    if (rec && rec != reinterpret_cast<void*>(itemdb::kNilAddr)) {
      const char* nm = *reinterpret_cast<char**>(reinterpret_cast<char*>(rec) + 4);
      if (nm) { std::strncpy(out, nm, cap - 1); out[cap - 1] = '\0'; }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

const char* ItemName(uint32_t id) {
  auto it = g_name_cache.find(id);
  if (it != g_name_cache.end()) return it->second.c_str();
  char buf[64];
  ResolveNameSEH(id, buf, sizeof(buf));
  if (buf[0] == '\0') std::snprintf(buf, sizeof(buf), "#%u", id);
  return (g_name_cache[id] = buf).c_str();
}






// Ouvre la fenetre de description native (id 0xc) pour l'item `id` a (mx,my), via un
// ItemSkillInfo standalone (comme cashshop_window) : ctor + SetId + EnsureLoaded +
// flag identifie. view/location = gate du bouton apercu (0 pour la vente = pas
// d'apercu, la description reste correcte). SEH-garde (POD only).
void OpenItemDesc(uint32_t id, uint16_t view, uint32_t location, int mx, int my) {
  if (id == 0) return;
  __try {
    uint8_t info[0x100];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(itemdb::kInfoCtorAddr)(info);
    reinterpret_cast<InfoSetId_t>(itemdb::kInfoSetIdAddr)(info, static_cast<int>(id));
    info[0x5c] = 1;                                        // identifie
    *reinterpret_cast<uint32_t*>(info + 0x8)  = location;  // equip point (gate apercu)
    *reinterpret_cast<uint32_t*>(info + 0x70) = view;      // viewID (gate apercu)
    void* cache = *reinterpret_cast<void**>(itemdb::kEnsureCachePtr);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(itemdb::kEnsureLoadedAddr)(cache, static_cast<int>(id));
    void* dwnd = uiwnd::MakeWindow(itemdb::kItemDescWndId);
    if (dwnd) {
      uiwnd::OnMsg(dwnd, itemdb::kItemDescMsgSet,
                                  static_cast<int>(reinterpret_cast<uintptr_t>(info)),
                                  0, 0, 0);
      uiwnd::SetPos(dwnd, mx, my);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

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
constexpr uint16_t kOpNpcName  = 0x0adf;  // ZC_ACK_REQNAMEALL_NPC {gid:4,groupId:4,name[24],title[24]}
constexpr uint16_t kOpMapChange  = 0x0091; // ZC_NPCACK_MAPMOVE (changement de map : @load, warp)
constexpr uint16_t kOpServerMove = 0x0092; // ZC_NPCACK_SERVERMOVE (changement de serveur)
// Envois (CZ).
constexpr uint16_t kOpDealAck  = 0x00c5;  // CZ_ACK_SELECT_DEALTYPE {GID:4,type:1}
constexpr uint16_t kOpBuyReq   = 0x00c8;  // CZ_PC_PURCHASE_ITEMLIST {amount:2,itemId:4}*
constexpr uint16_t kOpSellReq  = 0x00c9;  // CZ_PC_SELL_ITEMLIST {index:2,amount:2}*
constexpr uint16_t kOpCloseNpc = 0x0146;  // CZ_CLOSE_DIALOG {GID:4} -> ferme la session NPC serveur

}  // namespace

NpcShopWindow::NpcShopWindow() {
  // TOUT en OBSERVE (jamais RegisterRecvOpcode) : le handler natif doit TOUJOURS
  // tourner, sinon le shop natif est cassé quand le toggle est OFF (l'interception
  // patche la table de dispatch en permanence, sans dé-registration possible).
  // Quand ON, on lit les paquets pour bâtir notre modèle et on CACHE les fenêtres
  // natives (dont le comparateur ATK/DEF). Listes variables : on forwarde 4 octets
  // (assez pour packetLength @+0) et on lit le corps dans le buffer recv live.
  Bourgeon::Instance().RegisterObserveOpcode(kOpChoose, 4);   // npcId
  Bourgeon::Instance().RegisterObserveOpcode(kOpBuyList, 4);  // header (var)
  Bourgeon::Instance().RegisterObserveOpcode(kOpSellList, 4); // header (var)
  Bourgeon::Instance().RegisterObserveOpcode(kOpBuyRes, 1);   // result
  Bourgeon::Instance().RegisterObserveOpcode(kOpSellRes, 1);  // result
  // Nom des NPC (pour le titre) : gid:4 + groupId:4 + name[24] = 32 octets utiles.
  Bourgeon::Instance().RegisterObserveOpcode(kOpNpcName, 32);
  // Changement de map / serveur (@load, warp) : le warp invalide la session shop
  // cote serveur (npc_shopid=0) -> on ferme le viewer pour ne pas laisser une
  // fenetre orpheline. On ne lit pas le payload, juste la reception.
  Bourgeon::Instance().RegisterObserveOpcode(kOpMapChange, 4);
  Bourgeon::Instance().RegisterObserveOpcode(kOpServerMove, 4);
}

void NpcShopWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                              uint16_t len) {
  if (!imgui_enabled_) return;

  if (opcode == kOpMapChange || opcode == kOpServerMove) {
    // Warp / changement de map -> la session shop est morte cote serveur. On ferme
    // le viewer au prochain OnTick (thread principal ; jamais depuis le thread recv).
    map_changed_ = true;
    return;
  }

  if (opcode == kOpNpcName) {
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
    open_ = true;
    cur_mode_ = kBuy;
    buy_items_.clear();
    sell_items_.clear();
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
  // 0xc7 (sell list) : le handler natif peuple la fenêtre 0x17 (cachée) ; on lira
  // sa liste résolue en OnTick (RefreshSellFromNative). Rien à parser ici.
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
  if (mode == kBuy) {
    // Achat : requête brute CZ_ACK_SELECT_DEALTYPE(0) suffit — on parse 0x0b77
    // nous-mêmes, pas besoin de la fenêtre native.
    SendDealSelect(0);  // 0xc5 type 0 = achat (arme npc_shopid + declenche 0x0b77)
    buy_requested_ = true;
  } else {
    // Vente : la requête 0xc5(1) BRUTE ne crée PAS la fenêtre native (le client
    // n'est pas en "mode vente" car on a zappé le clic du chooser). On dispatche
    // cmd 0x25 sur CMode::SendMsg = bouton "Vendre" natif -> pose l'état vente +
    // envoie 0xc5(1) -> le client crée la fenêtre native de vente (qu'on lit).
    __try {
      void* disp = rag::ActiveMode();
      if (disp) {
        void** vtbl = *reinterpret_cast<void***>(disp);
        using CmdDispatch_t = int(__thiscall*)(void*, int, int, int, int, int);
        reinterpret_cast<CmdDispatch_t>(vtbl[6])(
            disp, 0x25, static_cast<int>(npc_id_), 0, 0, 0);  // vtable+0x18
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    sell_requested_ = true;
  }
}

void NpcShopWindow::AddToCart(uint32_t id, int index, int32_t price, int max, int qty) {
  if (max < 1) max = 1;
  if (qty < 1) qty = 1;
  for (auto& e : cart_) {
    if (e.id == id && e.index == index) {
      e.amount += qty;
      if (e.amount > e.max) e.amount = e.max;  // borné à la quantité dispo
      return;
    }
  }
  cart_.push_back(CartEntry{id, index, qty > max ? max : qty, price, max});
}

// CZ_PC_PURCHASE_ITEMLIST 0xc8 : [op:2][len:2][ {amount:2, itemId:4} *count ]
void NpcShopWindow::SendBuy() {
  if (cart_.empty()) return;
  SendDealSelect(0);  // re-arme npc_shopid (efface apres chaque achat cote serveur)
  const int count = static_cast<int>(cart_.size());
  const int plen = 4 + 6 * count;
  std::vector<uint8_t> pkt(plen);
  uint8_t* p = pkt.data();
  *reinterpret_cast<uint16_t*>(p + 0) = kOpBuyReq;
  *reinterpret_cast<uint16_t*>(p + 2) = static_cast<uint16_t>(plen);
  uint8_t* it = p + 4;
  for (const auto& e : cart_) {
    *reinterpret_cast<uint16_t*>(it + 0) = static_cast<uint16_t>(e.amount);
    *reinterpret_cast<uint32_t*>(it + 2) = e.id;
    it += 6;
  }
  Bourgeon::Instance().SendPacket(pkt.data(), pkt.size());
}

// CZ_PC_SELL_ITEMLIST 0xc9 : [op:2][len:2][ {index:2, amount:2} *count ]
void NpcShopWindow::SendSell() {
  if (cart_.empty()) return;
  SendDealSelect(1);  // re-arme npc_shopid (efface apres chaque vente cote serveur)
  const int count = static_cast<int>(cart_.size());
  const int plen = 4 + 4 * count;
  std::vector<uint8_t> pkt(plen);
  uint8_t* p = pkt.data();
  *reinterpret_cast<uint16_t*>(p + 0) = kOpSellReq;
  *reinterpret_cast<uint16_t*>(p + 2) = static_cast<uint16_t>(plen);
  uint8_t* it = p + 4;
  for (const auto& e : cart_) {
    *reinterpret_cast<uint16_t*>(it + 0) = static_cast<uint16_t>(e.index);
    *reinterpret_cast<uint16_t*>(it + 2) = static_cast<uint16_t>(e.amount);
    it += 4;
  }
  Bourgeon::Instance().SendPacket(pkt.data(), pkt.size());
}

// Achat IMMEDIAT de `qty` unites de `id` (bypass panier) : CZ_PC_PURCHASE_ITEMLIST
// 0xc8 a 1 item. Le serveur calcule le cout (discount inclus) et valide.
void NpcShopWindow::QuickBuy(uint32_t id, int qty) {
  if (npc_id_ == 0 || qty < 1) return;
  SendDealSelect(0);  // re-arme npc_shopid (efface apres chaque achat cote serveur)
  uint8_t pkt[10];
  const uint16_t plen = 10;  // 4 (en-tete) + 6 (1 item)
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpBuyReq;
  *reinterpret_cast<uint16_t*>(pkt + 2) = plen;
  *reinterpret_cast<uint16_t*>(pkt + 4) = static_cast<uint16_t>(qty);
  *reinterpret_cast<uint32_t*>(pkt + 6) = id;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// Vente IMMEDIATE de `qty` unites de l'item a l'index inventaire `index` (bypass
// panier) : CZ_PC_SELL_ITEMLIST 0xc9 a 1 item. Le serveur calcule le gain (overcharge).
void NpcShopWindow::QuickSell(int index, int qty) {
  if (npc_id_ == 0 || qty < 1) return;
  SendDealSelect(1);  // re-arme npc_shopid (efface apres chaque vente cote serveur)
  uint8_t pkt[8];
  const uint16_t plen = 8;  // 4 (en-tete) + 4 (1 item)
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpSellReq;
  *reinterpret_cast<uint16_t*>(pkt + 2) = plen;
  *reinterpret_cast<uint16_t*>(pkt + 4) = static_cast<uint16_t>(index);
  *reinterpret_cast<uint16_t*>(pkt + 6) = static_cast<uint16_t>(qty);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// Lit la liste de vente RÉSOLUE depuis le sous-window liste natif, pointé par le
// global g_ShopSellMirrorWnd_ptr (DAT_0131f738), vtable 0x0103cbf0, std::list @+0xe8.
// Nœud MSVC std::list : {next, prev, value@+8}. Offsets CONFIRMÉS live (jellopy) :
// +0x0c index inv, +0x18 qté, +0x1c/+0x20 prix (overcharge), +0x34 std::string =
// itemId EN TEXTE ("909"), +0x90 slots. SEH (POD only).
void NpcShopWindow::RefreshSellFromNative() {
  sell_items_.clear();
  void* wnd = nullptr;
  __try { wnd = *reinterpret_cast<void**>(kSellListGlobal); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return; }
  if (!wnd) return;
  __try {
    if (*reinterpret_cast<uintptr_t*>(wnd) != kSellListVTable) return;
    uint8_t* w = reinterpret_cast<uint8_t*>(wnd);
    void* sentinel = *reinterpret_cast<void**>(w + kOffList);
    if (!sentinel) return;
    void* node = *reinterpret_cast<void**>(sentinel);
    int guard = 0;
    while (node && node != sentinel && guard < 4096) {
      uint8_t* n = reinterpret_cast<uint8_t*>(node);
      SellItem s;
      s.index  = *reinterpret_cast<int*>(n + kNodeIndex);
      s.amount = *reinterpret_cast<int*>(n + kNodeQty);
      int32_t p2 = *reinterpret_cast<int32_t*>(n + kNodePrice2);
      int32_t p1 = *reinterpret_cast<int32_t*>(n + kNodePrice);
      s.price = (p2 != 0) ? p2 : p1;  // overcharge (prix réel) sinon prix de base
      s.base_price = p1;              // base (avant Overcharge) pour l'affichage base->final
      s.slots = *reinterpret_cast<int16_t*>(n + kNodeSlots);
      // node+0x34 = std::string (MSVC : +0x14 cap) = itemId EN TEXTE -> atoi.
      const char* base = reinterpret_cast<const char*>(n + kNodeName);
      const uint32_t cap = *reinterpret_cast<const uint32_t*>(base + 0x14);
      const char* str = (cap > 15) ? *reinterpret_cast<const char* const*>(base)
                                   : base;
      s.id = str ? static_cast<uint32_t>(atoi(str)) : 0;
      if (s.amount > 0) sell_items_.push_back(s);
      node = *reinterpret_cast<void**>(node);
      ++guard;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { /* liste incohérente : on garde ce qu'on a */ }
}

void NpcShopWindow::CloseNativeShop() {
  // Le perso reste BLOQUÉ (état "dialogue NPC" CÔTÉ CLIENT) tant qu'on ne réplique
  // pas le CANCEL natif : le bouton Annuler du chooser dispatche cmd 0x28 sur
  // CMode::SendMsg (g_UICommandDispatcher @[0x0121333c], vtable+0x18) — c'est LUI
  // qui réinitialise l'état dialogue client (débloque) + notifie le serveur.
  // CZ_CLOSE_DIALOG seul ne suffit pas (blocage client, et gate serveur sur npc_id).
  __try {
    void* disp = rag::ActiveMode();
    if (disp) {
      void** vtbl = *reinterpret_cast<void***>(disp);
      using CmdDispatch_t = int(__thiscall*)(void*, int, int, int, int, int);
      reinterpret_cast<CmdDispatch_t>(vtbl[6])(disp, 0x28, 0, 0, 0, 0);  // vtable+0x18
    }
    // ShopCart_ResetAll(session) — le natif le fait juste après cmd 0x28.
    reinterpret_cast<void(__fastcall*)(int)>(0x00d55f80)(static_cast<int>(rag::kSessionAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  // Filet serveur : CZ_CLOSE_DIALOG (no-op côté serveur si npc_id déjà nettoyé).
  if (npc_id_ != 0) {
    uint8_t pkt[6];
    *reinterpret_cast<uint16_t*>(pkt + 0) = kOpCloseNpc;
    *reinterpret_cast<uint32_t*>(pkt + 2) = npc_id_;
    Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  }
  CloseWnd(kWinChoose);
  CloseWnd(kWinBuy);
  CloseWnd(kWinSell);
  CloseWnd(kWinDetail);
  sell_all_close_ = false;  // desarme la fermeture auto
}

void NpcShopWindow::HideNativeAtCreation(void* win) {
  if (!win || !imgui_enabled_) return;
  HideWnd(win);  // l'appelant a déjà filtré sur l'id (0x16/0x17/0x18/0x19)
}

void NpcShopWindow::HideDetailWindow(void* win) {
  if (!win || !imgui_enabled_ || !open_) return;
  __try {
    if (*reinterpret_cast<uintptr_t*>(win) == kDetailVTable)
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + uiwnd::kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void NpcShopWindow::OnTick() {
  if (!imgui_enabled_) { open_ = false; was_open_ = false; return; }

  // Fermeture AUTO demandee (vente d'un "Tout ajouter au panier" reussie) : on ferme
  // comme un clic X (cmd 0x28 + destruction native) depuis le thread principal.
  if (want_close_) {
    want_close_ = false;
    CloseNativeShop();
    open_ = false;
    was_open_ = false;
    show_panel_ = true;
    return;
  }

  // Changement de map (@load, warp...) recu : la session shop est invalidee cote
  // serveur (pc_setpos remet npc_shopid=0 / npc_id=0). On ferme le viewer et on
  // DETRUIT les fenetres natives orphelines (sinon, en cessant de les cacher, elles
  // reapparaitraient). PAS de cmd 0x28 : le warp a deja reset le dialogue serveur.
  if (map_changed_) {
    map_changed_ = false;
    if (open_) {
      CloseWnd(kWinChoose);
      CloseWnd(kWinBuy);
      CloseWnd(kWinSell);
      CloseWnd(kWinDetail);
      open_ = false;
      was_open_ = false;
      show_panel_ = true;
      npc_id_ = 0;
      buy_items_.clear();
      sell_items_.clear();
      cart_.clear();
      buy_requested_ = false;
      sell_requested_ = false;
      have_buy_ = false;
      sell_all_close_ = false;
      want_close_ = false;
    }
    return;
  }

  // Le shop est "ouvert" tant qu'une de ses fenêtres natives existe (le serveur
  // les crée ; on les cache). open_ posé aussi par 0xc4. Signal de fermeture :
  // plus aucune fenêtre shop native + on a fermé le viewer.
  const bool any_native = FindWnd(kWinChoose) || FindWnd(kWinBuy) ||
                          FindWnd(kWinSell) || FindWnd(kWinDetail);
  if (any_native) open_ = true;

  if (open_) {
    // Cache les fenêtres natives chaque tick (le natif peut remettre +0x28=1).
    HideWnd(FindWnd(kWinChoose));
    HideWnd(FindWnd(kWinBuy));
    HideWnd(FindWnd(kWinSell));
    HideWnd(FindWnd(kWinDetail));
    // Sous-window liste de vente (vtable 0x0103cbf0, via son global) : caché aussi
    // au cas où il serait top-level (pas un enfant du conteneur 0x17).
    __try {
      void* sl = *reinterpret_cast<void**>(kSellListGlobal);
      if (sl && *reinterpret_cast<uintptr_t*>(sl) == kSellListVTable)
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(sl) + uiwnd::kOffVisible) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!was_open_) need_pos_ = true;
    // Demande la liste du mode courant si pas encore faite (envoi thread principal).
    if (cur_mode_ == kBuy && !buy_requested_) RequestList(kBuy);
    if (cur_mode_ == kSell && !sell_requested_) RequestList(kSell);
    // En mode Vente : recharge la liste depuis la fenêtre native cachée.
    if (cur_mode_ == kSell) RefreshSellFromNative();
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
  std::snprintf(title, sizeof(title), "%s###bourgeon_shop",
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
  if (ImGui::BeginTabBar("shop_tabs")) {
    if (ImGui::BeginTabItem("Acheter")) { cur_mode_ = kBuy; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Vendre"))  { cur_mode_ = kSell; ImGui::EndTabItem(); }
    ImGui::EndTabBar();
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
  const ImVec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);  // texte noir (skin RO clair)
  const uint32_t zeny = static_cast<uint32_t>(rag::Zeny());
  ImGui::TextColored(kBlack, "Zeny: %uz", zeny);
  if (last_result_ == 0) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "  %s OK",
                       last_result_sell_ ? "Vente" : "Achat");
  } else if (last_result_ > 0) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "  Echec (%d)", last_result_);
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

  // Clic-droit sur le DERNIER item dessine (icone ou nom) -> description native.
  auto rclick_desc = [&](uint32_t id, uint16_t view, uint32_t loc) {
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      const ImVec2 mp = ImGui::GetMousePos();
      OpenItemDesc(id, view, loc, static_cast<int>(mp.x), static_cast<int>(mp.y));
    }
  };

  // Boutons quantite +1/+10/+100/+1k d'une ligne. Clic = ajout au panier ;
  // Ctrl+clic = transaction immediate (bypass panier). Le grisage n'a lieu QU'EN
  // mode immediat (Ctrl) : sans Ctrl on empile librement (le bouton Acheter/Vendre
  // gere la solvabilite du total). Immediat : achat grise si qty*prix > zeny ;
  // vente grisee si qty > quantite possedee.
  static const int   kQty[4]    = {1, 10, 100, 1000};
  static const char* kQtyLbl[4] = {"+1", "+10", "+100", "+1k"};
  auto qty_buttons = [&](uint32_t id, int index, int32_t unit_price, int max_avail,
                         bool is_buy) {
    const bool ctrl = ImGui::GetIO().KeyCtrl;
    for (int k = 0; k < 4; ++k) {
      if (k) ImGui::SameLine(0.0f, 2.0f);
      const int q = kQty[k];
      // Sans Ctrl (ajout panier) : jamais grise. Avec Ctrl (transaction immediate) :
      // grise si non abordable (achat) ou quantite insuffisante (vente).
      bool ok = true;
      if (ctrl)
        ok = is_buy ? (static_cast<long long>(unit_price) * q <=
                       static_cast<long long>(zeny))
                    : (q <= max_avail);
      if (!ok) ImGui::BeginDisabled();
      if (ro::RoButton(kQtyLbl[k], 34.0f, 0.0f)) {
        if (ctrl) { if (is_buy) QuickBuy(id, q); else QuickSell(index, q); }
        else      AddToCart(id, index, unit_price, max_avail, q);
      }
      if (!ok) ImGui::EndDisabled();
    }
  };

  static ImGuiTextFilter filter;
  ImGui::SetNextItemWidth(-1.0f);
  // Placeholder grise "Filtrer..." quand le champ est vide (pilote InputBuf/Build).
  if (ImGui::InputTextWithHint("##shop_filter", "Filtrer...", filter.InputBuf,
                               IM_ARRAYSIZE(filter.InputBuf)))
    filter.Build();
  ImGui::TextDisabled("Clic = panier   -   Ctrl+clic = achat/vente immediat");
  ImGui::Separator();

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  // Panier auto-masque quand il est vide -> la liste prend toute la largeur. (Avec
  // l'achat/vente immediat en Ctrl+clic, le panier ne sert qu'a l'achat groupe ; il
  // reapparait des qu'on y ajoute un item.)
  const bool show_cart = !cart_.empty();
  const float cart_w = 200.0f;
  const float list_w =
      show_cart ? std::max(160.0f, avail.x - cart_w - 8.0f) : 0.0f;

  // ── Liste (gauche) ──
  ImGui::BeginChild("shop_list", ImVec2(list_w, 0), true);
  if (cur_mode_ == kBuy) {
    if (ImGui::BeginTable("buytbl", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Objet");
      ImGui::TableSetupColumn("Prix", ImGuiTableColumnFlags_WidthFixed, 110.0f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150.0f);
      ImGui::TableHeadersRow();
      for (const auto& b : buy_items_) {
        const char* nm = ItemName(b.id);
        if (!filter.PassFilter(nm)) continue;
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(b.id));
        ImGui::TableNextColumn();
        ro::IconTex ic = ro::ItemIcon(b.id);
        if (ic.tex) {
          ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(20, 20));
          rclick_desc(b.id, b.view, b.location);  // clic-droit icone -> desc
          ImGui::SameLine();
        }
        ImGui::TextUnformatted(nm);
        rclick_desc(b.id, b.view, b.location);  // clic-droit nom -> desc
        ImGui::TableNextColumn();
        const bool afford = static_cast<uint32_t>(b.discount) <= zeny;
        // Prix noir si abordable, rouge sombre si trop cher ; "base -> remise" si Discount.
        draw_price(b.price, b.discount,
                   afford ? kBlack : ImVec4(0.75f, 0.15f, 0.15f, 1.0f));
        ImGui::TableNextColumn();
        qty_buttons(b.id, -1, b.discount, 30000, true);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (buy_items_.empty())
      ImGui::TextDisabled("(liste d'achat en attente du serveur...)");
  } else {  // kSell
    // "Tout vendre" : remplit le panier avec TOUS les items vendables au stack
    // complet ; l'utilisateur confirme ensuite via le bouton "Vendre".
    if (!sell_items_.empty() &&
        ro::RoButton("Tout ajouter au panier")) {
      cart_.clear();
      for (const auto& s : sell_items_)
        cart_.push_back(CartEntry{s.id, s.index, s.amount, s.price, s.amount});
      sell_all_close_ = true;  // arme la fermeture auto du shop apres la vente
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Ajoute tout l'inventaire vendable ; le shop se fermera "
                        "automatiquement apres la vente.");
    if (ImGui::BeginTable("selltbl", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Objet");
      ImGui::TableSetupColumn("Vente", ImGuiTableColumnFlags_WidthFixed, 110.0f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150.0f);
      ImGui::TableHeadersRow();
      for (const auto& s : sell_items_) {
        const char* nm = ItemName(s.id);
        if (!filter.PassFilter(nm)) continue;
        ImGui::TableNextRow();
        ImGui::PushID(s.index);
        ImGui::TableNextColumn();
        ro::IconTex ic = ro::ItemIcon(s.id);
        // view/loc inconnus en vente -> 0 : pas d'apercu, la desc reste correcte.
        if (ic.tex) {
          ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(20, 20));
          rclick_desc(s.id, 0, 0);  // clic-droit icone -> desc
          ImGui::SameLine();
        }
        ImGui::Text("%s x%d", nm, s.amount);
        rclick_desc(s.id, 0, 0);  // clic-droit nom -> desc
        ImGui::TableNextColumn();
        // "base -> majore" si Overcharge, sinon juste le prix (lu du noeud natif).
        draw_price(s.base_price, s.price, kBlack);
        ImGui::TableNextColumn();
        qty_buttons(s.id, s.index, s.price, s.amount, false);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (sell_items_.empty())
      ImGui::TextDisabled("(rien à vendre / liste en attente...)");
  }
  ImGui::EndChild();

  // ── Panier (droite) — masque automatiquement quand vide (cf. show_cart) ──
  if (show_cart) {
  ImGui::SameLine();
  ImGui::BeginChild("shop_cart", ImVec2(cart_w, 0), true);
  ImGui::TextUnformatted(cur_mode_ == kBuy ? "Panier d'achat" : "Panier de vente");
  ImGui::SameLine();
  if (!cart_.empty() && ro::RoButton("Vider")) { cart_.clear(); sell_all_close_ = false; }
  ImGui::Separator();
  long long total = 0;
  int remove = -1;
  ImGui::BeginChild("shop_cart_items", ImVec2(0, -56), false);
  for (int i = 0; i < static_cast<int>(cart_.size()); ++i) {
    CartEntry& e = cart_[i];
    total += static_cast<long long>(e.price) * e.amount;
    ImGui::PushID(2000 + i);
    ImGui::TextWrapped("%s", ItemName(e.id));
    // Controle quantite : [-] [champ] [+], petits boutons RO carres (comme le
    // cashshop). InputInt en step=0 -> pas de +/- natifs non skinnes.
    const float step = ImGui::GetFrameHeight();
    if (ro::RoButton("-", step, step) && e.amount > 1) --e.amount;
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(42.0f);
    if (ImGui::InputInt("##qty", &e.amount, 0, 0)) {
      if (e.amount < 1) e.amount = 1;
      if (e.amount > e.max) e.amount = e.max;  // vente = qté possédée ; achat = stack
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (ro::RoButton("+", step, step) && e.amount < e.max) ++e.amount;
    // Sous-total (noir) centre verticalement sur la ligne des champs.
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kBlack, "%lldz", static_cast<long long>(e.price) * e.amount);
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
  ImGui::Text("Total: %lldz", total);

  if (cur_mode_ == kBuy) {
    const bool afford = total <= static_cast<long long>(zeny);
    if (cart_.empty()) ImGui::BeginDisabled();
    if (ro::RoButton(afford ? "Acheter" : "Zeny insuffisant",
                     ImGui::GetContentRegionAvail().x, 0))
      SendBuy();
    if (cart_.empty()) ImGui::EndDisabled();
  } else {
    if (cart_.empty()) ImGui::BeginDisabled();
    if (ro::RoButton("Vendre", ImGui::GetContentRegionAvail().x, 0)) SendSell();
    if (cart_.empty()) ImGui::EndDisabled();
  }
  ImGui::EndChild();
  }  // if (show_cart) : panier masque quand vide

  ro::EndRoWindow();
  ImGui::PopStyleVar(5);
}
