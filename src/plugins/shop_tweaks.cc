#include "plugins/shop_tweaks.h"

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
#include "plugins/imgui_escape.h"
#include "ui/ro_imgui.h"          // BeginRoWindow (skin RO)

// ── Constantes RE (client 20250716, base 0x400000 ; cf. project_npc_shop_re) ──
namespace {

// UIWindowMgr + factory.
constexpr uintptr_t kUIWindowMgr = 0x0131f4e8;
constexpr uintptr_t kFindWindow  = 0x00a47b90;  // __thiscall(mgr, id) -> wnd|null
constexpr uintptr_t kCloseWindow = 0x00a2e770;  // UIWindowMgr_Close(mgr, edx, id)
using FindWindow_t  = void* (__thiscall*)(void*, int);
using CloseWindow_t = void (__fastcall*)(void*, void*, int);

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
constexpr int kOffVisible = 0x28;  // flag visibilité (Show/Hide)
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
constexpr uintptr_t kInfoCtor  = 0x006a1b20;  // ItemSkillInfo_ctor(this) __fastcall
constexpr uintptr_t kInfoSetId = 0x006a6570;  // ItemSkillInfo_SetId(this,id) __thiscall
using InfoCtor_t  = void(__fastcall*)(void*);
using InfoSetId_t = void(__thiscall*)(void*, int);

// Description d'item (clic-droit) : MakeWindow(0xc) + OnMsg 0x18 (comme cashshop).
constexpr int kWinItemDesc = 0xc;
constexpr int kMsgSetItem  = 0x18;
constexpr int kVfOnMsg     = 0x94;
constexpr int kVfSetPos    = 0x10;
constexpr uintptr_t kMakeWindow = 0x00a39340;  // UIWindowMgr_MakeWindow(mgr, edx, id)
using OnMsg_t      = int (__fastcall*)(void*, void*, int, int, int, int, int, int);
using SetPos_t     = void(__fastcall*)(void*, void*, int, int);
using MakeWindow_t = void* (__fastcall*)(void*, void*, void*);
template <typename Fn>
inline Fn Vf(void* self, int off) {
  return reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t**>(self))[off / 4]);
}

// Nom par id : DB de description (map id->record), name = *(rec+4).
constexpr uintptr_t kDescDbLookup = 0x006a0d40;
constexpr uintptr_t kDescDb       = 0x01255130;
constexpr uintptr_t kDescDbNil    = 0x01255138;
constexpr uintptr_t kEnsureLoaded = 0x006a06b0;
constexpr uintptr_t kEnsureCache  = 0x0125510c;
using DescLookup_t   = void*(__cdecl*)(int, void*);
using EnsureLoaded_t = char (__thiscall*)(void*, int);

// Icône d'item (image d'inventaire).
constexpr uintptr_t kBuildIconPath = 0x00d5a720;  // __stdcall(id_str, out[128], identified)
constexpr uintptr_t kTexMgr  = 0x00a90350;
constexpr uintptr_t kMakeKey = 0x00a9f030;
constexpr uintptr_t kLoadTex = 0x00a8d4a0;
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;
using BuildIconPath_t = void*(__stdcall*)(const char*, char*, int);
using TexMgr_t  = void*(__cdecl*)();
using MakeKey_t = void*(__cdecl*)(const char*);
using LoadTex_t = void*(__fastcall*)(void*, void*, void*);

// ── Fenêtres natives (SEH-gardé) ──
void* FindWnd(int id) {
  __try {
    return reinterpret_cast<FindWindow_t>(kFindWindow)(
        reinterpret_cast<void*>(kUIWindowMgr), id);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void CloseWnd(int id) {
  __try {
    reinterpret_cast<CloseWindow_t>(kCloseWindow)(
        reinterpret_cast<void*>(kUIWindowMgr), nullptr, id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void HideWnd(void* w) {
  __try {
    if (w) *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(w) + kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Cache nom d'item (id -> nom) ──
std::unordered_map<uint32_t, std::string> g_name_cache;

void ResolveNameSEH(uint32_t id, char* out, size_t cap) {
  out[0] = '\0';
  __try {
    void* cache = *reinterpret_cast<void**>(kEnsureCache);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(kEnsureLoaded)(cache, static_cast<int>(id));
    void* rec = reinterpret_cast<DescLookup_t>(kDescDbLookup)(
        static_cast<int>(id), reinterpret_cast<void*>(kDescDb));
    if (rec && rec != reinterpret_cast<void*>(kDescDbNil)) {
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

// ── Icônes ImGui (cache id -> texture) ──
struct IconTex { void* tex = nullptr; int w = 0; int h = 0; };
std::unordered_map<uint32_t, IconTex> g_icon_cache;

struct RawTex { const uint8_t* bgra; int w; int h; };
bool GetRawTex(const char* path, RawTex* out) {
  __try {
    void* mgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
    if (!mgr) return false;
    void* key = reinterpret_cast<MakeKey_t>(kMakeKey)(path);
    if (!key) return false;
    void* t = reinterpret_cast<LoadTex_t>(kLoadTex)(mgr, nullptr, key);
    if (!t) return false;
    const int w = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexW);
    const int h = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexH);
    const uint8_t* bgra =
        *reinterpret_cast<const uint8_t**>(static_cast<char*>(t) + kTexPix);
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || !bgra) return false;
    out->bgra = bgra; out->w = w; out->h = h;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool BuildIconPathSafe(uint32_t id, char* out) {
  char idstr[16];
  std::snprintf(idstr, sizeof(idstr), "%u", id);
  out[0] = '\0';
  __try {
    reinterpret_cast<BuildIconPath_t>(kBuildIconPath)(idstr, out, 1);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

IconTex LoadItemIcon(uint32_t id) {
  char path[192];
  if (!BuildIconPathSafe(id, path)) return {};
  RawTex rt{};
  if (!GetRawTex(path, &rt)) return {};
  std::vector<uint8_t> argb(static_cast<size_t>(rt.w) * rt.h * 4);
  for (int i = 0; i < rt.w * rt.h; ++i) {
    const uint8_t b = rt.bgra[i * 4], g = rt.bgra[i * 4 + 1], r = rt.bgra[i * 4 + 2];
    const bool ck = (r == 0xFF && g == 0 && b == 0xFF);  // magenta -> transparent
    argb[i * 4] = b; argb[i * 4 + 1] = g; argb[i * 4 + 2] = r;
    argb[i * 4 + 3] = ck ? 0 : 0xFF;
  }
  return {Overlay_CreateTextureARGB(argb.data(), rt.w, rt.h), rt.w, rt.h};
}

IconTex ResolveIcon(uint32_t id) {
  auto it = g_icon_cache.find(id);
  if (it != g_icon_cache.end()) return it->second;
  return g_icon_cache[id] = LoadItemIcon(id);
}

// Ouvre la fenetre de description native (id 0xc) pour l'item `id` a (mx,my), via un
// ItemSkillInfo standalone (comme cashshop_tweaks) : ctor + SetId + EnsureLoaded +
// flag identifie. view/location = gate du bouton apercu (0 pour la vente = pas
// d'apercu, la description reste correcte). SEH-garde (POD only).
void OpenItemDesc(uint32_t id, uint16_t view, uint32_t location, int mx, int my) {
  if (id == 0) return;
  __try {
    uint8_t info[0x100];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(kInfoCtor)(info);
    reinterpret_cast<InfoSetId_t>(kInfoSetId)(info, static_cast<int>(id));
    info[0x5c] = 1;                                        // identifie
    *reinterpret_cast<uint32_t*>(info + 0x8)  = location;  // equip point (gate apercu)
    *reinterpret_cast<uint32_t*>(info + 0x70) = view;      // viewID (gate apercu)
    void* cache = *reinterpret_cast<void**>(kEnsureCache);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(kEnsureLoaded)(cache, static_cast<int>(id));
    void* dwnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
        reinterpret_cast<void*>(kUIWindowMgr), nullptr,
        reinterpret_cast<void*>(kWinItemDesc));
    if (dwnd) {
      Vf<OnMsg_t>(dwnd, kVfOnMsg)(dwnd, nullptr, 0, kMsgSetItem,
                                  static_cast<int>(reinterpret_cast<uintptr_t>(info)),
                                  0, 0, 0);
      Vf<SetPos_t>(dwnd, kVfSetPos)(dwnd, nullptr, mx, my);
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
// Envois (CZ).
constexpr uint16_t kOpDealAck  = 0x00c5;  // CZ_ACK_SELECT_DEALTYPE {GID:4,type:1}
constexpr uint16_t kOpBuyReq   = 0x00c8;  // CZ_PC_PURCHASE_ITEMLIST {amount:2,itemId:4}*
constexpr uint16_t kOpSellReq  = 0x00c9;  // CZ_PC_SELL_ITEMLIST {index:2,amount:2}*
constexpr uint16_t kOpCloseNpc = 0x0146;  // CZ_CLOSE_DIALOG {GID:4} -> ferme la session NPC serveur

}  // namespace

ShopTweaks::ShopTweaks() {
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
}

void ShopTweaks::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                              uint16_t len) {
  if (!imgui_enabled_) return;

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

void ShopTweaks::RequestList(Mode mode) {
  if (npc_id_ == 0) return;
  if (mode == kBuy) {
    // Achat : requête brute CZ_ACK_SELECT_DEALTYPE(0) suffit — on parse 0x0b77
    // nous-mêmes, pas besoin de la fenêtre native.
    uint8_t pkt[7];
    *reinterpret_cast<uint16_t*>(pkt + 0) = kOpDealAck;
    *reinterpret_cast<uint32_t*>(pkt + 2) = npc_id_;
    pkt[6] = 0;
    Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
    buy_requested_ = true;
  } else {
    // Vente : la requête 0xc5(1) BRUTE ne crée PAS la fenêtre native (le client
    // n'est pas en "mode vente" car on a zappé le clic du chooser). On dispatche
    // cmd 0x25 sur CMode::SendMsg = bouton "Vendre" natif -> pose l'état vente +
    // envoie 0xc5(1) -> le client crée la fenêtre native de vente (qu'on lit).
    __try {
      void* disp = *reinterpret_cast<void**>(0x0121333c);  // g_UICommandDispatcher
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

void ShopTweaks::AddToCart(uint32_t id, int index, int32_t price, int max) {
  if (max < 1) max = 1;
  for (auto& e : cart_) {
    if (e.id == id && e.index == index) {
      if (e.amount < e.max) ++e.amount;  // borné à la quantité dispo
      return;
    }
  }
  cart_.push_back(CartEntry{id, index, 1, price, max});
}

// CZ_PC_PURCHASE_ITEMLIST 0xc8 : [op:2][len:2][ {amount:2, itemId:4} *count ]
void ShopTweaks::SendBuy() {
  if (cart_.empty()) return;
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
void ShopTweaks::SendSell() {
  if (cart_.empty()) return;
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

// Lit la liste de vente RÉSOLUE depuis le sous-window liste natif, pointé par le
// global g_ShopSellMirrorWnd_ptr (DAT_0131f738), vtable 0x0103cbf0, std::list @+0xe8.
// Nœud MSVC std::list : {next, prev, value@+8}. Offsets CONFIRMÉS live (jellopy) :
// +0x0c index inv, +0x18 qté, +0x1c/+0x20 prix (overcharge), +0x34 std::string =
// itemId EN TEXTE ("909"), +0x90 slots. SEH (POD only).
void ShopTweaks::RefreshSellFromNative() {
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

void ShopTweaks::CloseNativeShop() {
  // Le perso reste BLOQUÉ (état "dialogue NPC" CÔTÉ CLIENT) tant qu'on ne réplique
  // pas le CANCEL natif : le bouton Annuler du chooser dispatche cmd 0x28 sur
  // CMode::SendMsg (g_UICommandDispatcher @[0x0121333c], vtable+0x18) — c'est LUI
  // qui réinitialise l'état dialogue client (débloque) + notifie le serveur.
  // CZ_CLOSE_DIALOG seul ne suffit pas (blocage client, et gate serveur sur npc_id).
  __try {
    void* disp = *reinterpret_cast<void**>(0x0121333c);  // g_UICommandDispatcher
    if (disp) {
      void** vtbl = *reinterpret_cast<void***>(disp);
      using CmdDispatch_t = int(__thiscall*)(void*, int, int, int, int, int);
      reinterpret_cast<CmdDispatch_t>(vtbl[6])(disp, 0x28, 0, 0, 0, 0);  // vtable+0x18
    }
    // ShopCart_ResetAll(session) — le natif le fait juste après cmd 0x28.
    reinterpret_cast<void(__fastcall*)(int)>(0x00d55f80)(0x15fa3c0);
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

void ShopTweaks::HideNativeAtCreation(void* win) {
  if (!win || !imgui_enabled_) return;
  HideWnd(win);  // l'appelant a déjà filtré sur l'id (0x16/0x17/0x18/0x19)
}

void ShopTweaks::HideDetailWindow(void* win) {
  if (!win || !imgui_enabled_ || !open_) return;
  __try {
    if (*reinterpret_cast<uintptr_t*>(win) == kDetailVTable)
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ShopTweaks::OnTick() {
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
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(sl) + kOffVisible) = 0;
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

void ShopTweaks::OnRenderUI() {
  if (!open_ || !imgui_enabled_) return;

  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(spawn_x_),
                                   static_cast<float>(spawn_y_)),
                            ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
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
  const uint32_t zeny = *reinterpret_cast<uint32_t*>(0x015fba90);  // g_PlayerZeny
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

  static ImGuiTextFilter filter;
  ImGui::SetNextItemWidth(-1.0f);
  // Placeholder grise "Filtrer..." quand le champ est vide (pilote InputBuf/Build).
  if (ImGui::InputTextWithHint("##shop_filter", "Filtrer...", filter.InputBuf,
                               IM_ARRAYSIZE(filter.InputBuf)))
    filter.Build();
  ImGui::Separator();

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float cart_w = 200.0f;
  const float list_w = std::max(160.0f, avail.x - cart_w - 8.0f);

  // ── Liste (gauche) ──
  ImGui::BeginChild("shop_list", ImVec2(list_w, 0), true);
  if (cur_mode_ == kBuy) {
    if (ImGui::BeginTable("buytbl", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Objet");
      ImGui::TableSetupColumn("Prix", ImGuiTableColumnFlags_WidthFixed, 110.0f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44.0f);
      ImGui::TableHeadersRow();
      for (const auto& b : buy_items_) {
        const char* nm = ItemName(b.id);
        if (!filter.PassFilter(nm)) continue;
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(b.id));
        ImGui::TableNextColumn();
        IconTex ic = ResolveIcon(b.id);
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
        if (ro::RoButton("+")) AddToCart(b.id, -1, b.discount, 30000);
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
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44.0f);
      ImGui::TableHeadersRow();
      for (const auto& s : sell_items_) {
        const char* nm = ItemName(s.id);
        if (!filter.PassFilter(nm)) continue;
        ImGui::TableNextRow();
        ImGui::PushID(s.index);
        ImGui::TableNextColumn();
        IconTex ic = ResolveIcon(s.id);
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
        if (ro::RoButton("+")) AddToCart(s.id, s.index, s.price, s.amount);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (sell_items_.empty())
      ImGui::TextDisabled("(rien à vendre / liste en attente...)");
  }
  ImGui::EndChild();

  // ── Panier (droite) ──
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

  ro::EndRoWindow();
  ImGui::PopStyleVar(5);
}
