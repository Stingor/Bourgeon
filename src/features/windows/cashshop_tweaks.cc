#include "features/windows/cashshop_tweaks.h"

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
#include "features/overlays/basic_info.h"   // aperçu porté (RenderItemPreviewTooltip / CanPreview)
#include "ui/imgui_escape.h"
#include "ui/ro_imgui.h"          // BeginRoWindow (skin RO)

//  Constantes RE (client 20250716, base 0x400000 ; cf. project_cashshop_re) 
namespace {

// UICashShopWnd : id 0x13e (318), vtable 0x0101ca18. TrouvΓ©e par FindWindow.
constexpr int       kWinCashShop  = 0x13e;
constexpr uintptr_t kCashVTable   = 0x0101ca18;
constexpr uintptr_t kMakeWindow   = 0x00a39340;  // __fastcall(mgr, edx, id) -> wnd
constexpr uintptr_t kCloseWindow  = 0x00a2e770;  // UIWindowMgr_Close(mgr, edx, id)
using MakeWindow_t = void* (__fastcall*)(void*, void*, void*);
using CloseWindow_t = void (__fastcall*)(void*, void*, int);

// Offsets UIWindow.
constexpr int kOffPosX    = 0x1c;
constexpr int kOffPosY    = 0x20;
constexpr int kOffVisible = 0x28;  // flag visibilitΓ© (Show/Hide : wnd+0x28=flag)

// Description d'item (clic-droit) : MakeWindow(0xc) + OnMsg 0x18 
constexpr int kWinItemDesc = 0xc;
constexpr int kMsgSetItem  = 0x18;
constexpr int kVfOnMsg     = 0x94;
constexpr int kVfSetPos    = 0x10;
using OnMsg_t  = int (__fastcall*)(void*, void*, int, int, int, int, int, int);
using SetPos_t = void(__fastcall*)(void*, void*, int, int);

// ItemSkillInfo standalone (ctor + SetId par id) β indΓ©pendant de l'inventaire.
constexpr uintptr_t kInfoCtor  = 0x006a1b20;  // ItemSkillInfo_ctor(this) __fastcall
constexpr uintptr_t kInfoSetId = 0x006a6570;  // ItemSkillInfo_SetId(this,id) __thiscall
using InfoCtor_t  = void(__fastcall*)(void*);
using InfoSetId_t = void(__thiscall*)(void*, int);

// Nom d'item par id : DB de description (map id->record), name = *(rec+4) 
constexpr uintptr_t kDescDbLookup = 0x006a0d40;  // ItemSkillDescDB_Lookup(id,&db)
constexpr uintptr_t kDescDb       = 0x01255130;
constexpr uintptr_t kDescDbNil    = 0x01255138;
constexpr uintptr_t kEnsureLoaded = 0x006a06b0;  // EnsureLoaded(cache,id) __thiscall
constexpr uintptr_t kEnsureCache  = 0x0125510c;  // *(void**) = cache
using DescLookup_t   = void*(__cdecl*)(int, void*);
using EnsureLoaded_t = char (__thiscall*)(void*, int);

// Icône d'item : le cash shop affiche l'image de COLLECTION (art de preview,
// bien plus grande que l'icône d'inventaire). Sa résolution vit dans le cache
// partagé, ro::ItemCollectionIcon (ui/icon_cache.h), avec le repli sur la petite
// icône quand l'art n'existe pas.

template <typename Fn>
inline Fn Vf(void* self, int off) {
  return reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t**>(self))[off / 4]);
}

// DΓ©truit la fenΓͺtre native du cash shop (id 0x13e). SEH-gardΓ© (POD only).
void CloseNativeCashShop() {
  __try {
    reinterpret_cast<CloseWindow_t>(kCloseWindow)(
        uiwnd::Mgr(), nullptr, kWinCashShop);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Lit le pointeur de fenΓͺtre valide (vtable vΓ©rifiΓ©e). SEH-gardΓ©.
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

//  Cache nom d'item (id -> nom)
std::unordered_map<uint32_t, std::string> g_name_cache;

// SEH isolΓ© (POD only, pas de std::string -> Γ©vite C2712) : Γ©crit le nom dans out.
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
  // Retire le prefixe "Costume " (redondant : l'onglet/le contexte l'indique deja).
  const char* nm = buf;
  if (std::strncmp(nm, "Costume ", 8) == 0) nm += 8;
  return (g_name_cache[id] = nm).c_str();
}







// Ouvre la fenΓͺtre de description native (id 0xc) pour l'item `id` Γ  (mx,my).
// ItemSkillInfo standalone renseignΓ© pour que OnMsg 0x18 (UIItemSkillDescWnd_OnMsg
// 0x008c18b0) reconstruise TOUT depuis l'info (qu'il COPIE dans wnd+0xb8) :
//   - ctor + SetId       -> id @info+0x2c (base de GetResName/GetDescLines) ;
//   - EnsureLoaded(id)   -> parse rec+0x0c dans le cache (desc) ;
//   - info+0x5c = 1      -> "identifiΓ©" : resname/desc lus dans rec + gate preview ;
//   - info+0x8  = location  (equip point) : gate du bouton PREVIEW (doit Γͺtre dans
//     l'ensemble coiffe/costume) ;
//   - info+0x70 = viewID  : gate du bouton PREVIEW (!=0) + modΓ¨le 3D (wnd 0x126).
// Offsets confirmΓ©s par dΓ©sasm (local_230[2]=+0x8, [0x17]=+0x5c, [0x1c]=+0x70).
// Le chemin collection (wnd+0x1c4) est bΓ’ti par GetResName(info) -> avec le flag
// identifiΓ© il pointe sur μ μ μΈν°νμ΄μ€\collection\<resname>.bmp.
void OpenItemDesc(uint32_t id, uint16_t view, uint32_t location, int mx, int my) {
  if (id == 0) return;
  __try {
    uint8_t info[0x100];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(kInfoCtor)(info);
    reinterpret_cast<InfoSetId_t>(kInfoSetId)(info, static_cast<int>(id));
    info[0x5c] = 1;                                                  // identifiΓ©
    *reinterpret_cast<uint32_t*>(info + 0x8)  = location;            // equip point
    *reinterpret_cast<uint32_t*>(info + 0x70) = view;                // viewID
    void* cache = *reinterpret_cast<void**>(kEnsureCache);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(kEnsureLoaded)(cache, static_cast<int>(id));
    void* dwnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
        uiwnd::Mgr(), nullptr,
        reinterpret_cast<void*>(kWinItemDesc));
    if (dwnd) {
      Vf<OnMsg_t>(dwnd, kVfOnMsg)(dwnd, nullptr, 0, kMsgSetItem,
                                  static_cast<int>(reinterpret_cast<uintptr_t>(info)),
                                  0, 0, 0);
      Vf<SetPos_t>(dwnd, kVfSetPos)(dwnd, nullptr, mx, my);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Labels des catΓ©gories (e_cash_shop_tab, serveur cashshop.hpp).
const char* kTabLabels[] = {"Nouveautes", "Populaire", "Limite", "Location",
                            "Permanent", "Parchemins", "Consommables",
                            "Divers",     "Soldes"};
// Onglets AFFICHΓS (l'index serveur reste 0..8 pour la rΓ©ception 0x08ca / l'achat
// 0x848 ; on masque juste ceux toujours vides). CachΓ©s : Limite/Location/Permanent/
// Parchemins/Soldes.
const bool kTabShown[] = {true, true, false, false, false, false, true, true, false};

// Emplacement d'Γ©quipement d'un item depuis son masque `location` (EQP_*, =
// pc_equippoint). Renvoie {clΓ© d'ordre stable, label} ; {99,"Autre"} = non
// Γ©quipable (consommable...) ou slot inconnu. Costumes prioritaires (les items
// cash sont majoritairement des costumes/coiffes).
// Clé de filtre virtuelle : "Costumes hat-effect" = costumes sans viewID rendus
// via effet .str (ItemToHatOrdinal != 0). N'existe pas dans SlotOf (dérivé de
// l'emplacement) -> clé dédiée hors de la plage des slots réels (0..13, 99).
constexpr int kSlotHatEffect = 100;

struct Slot { int key; const char* label; };
Slot SlotOf(uint32_t e) {
  if (e & 0x2000)            return {13, "Costume cape"};       // COSTUME_GARMENT
  if (e & (0x0400 | 0x0800 | 0x1000))
                            return {10, "Costume tete"};        // COSTUME_HEAD_*
  if (e & 0x0100)            return {0,  "Tete haut"};          // HEAD_TOP
  if (e & 0x0200)            return {1,  "Tete milieu"};        // HEAD_MID
  if (e & 0x0001)            return {2,  "Tete bas"};           // HEAD_LOW
  if (e & 0x0010)            return {3,  "Armure"};             // ARMOR
  if (e & 0x0004)            return {4,  "Cape"};               // GARMENT
  if (e & 0x0040)            return {5,  "Chaussures"};         // SHOES
  if (e & (0x0008 | 0x0080)) return {6,  "Accessoire"};        // ACC L/R
  if (e & 0x0020)            return {7,  "Bouclier"};           // HAND_L
  if (e & 0x0002)            return {8,  "Arme"};               // HAND_R
  if (e & 0x8000)            return {9,  "Munition"};           // AMMO
  return {99, "Autre"};
}

}  // namespace

//  Opcodes cash shop (vanilla, < 0x0C35 -> handler natif intact, on OBSERVE) 
// NOTE (RE 2026-07-05) : sur ce packetver (20250716) le peuplement se fait par
// **ZC_ACK_SCHEDULER_CASHITEM 0x08ca** (un paquet par onglet), dΓ©clenchΓ© par la
// list-request **0x08c9** (2 octets) que le client natif envoie Γ  l'ouverture.
// Le couple CZ_REQ_SE_CASH_TAB_CODE 0x846 / ZC_ACK_SE_CASH_ITEM_LIST2 0x8c0 est
// du code serveur mort ici (#if PACKETVER 2011 uniquement) -> ne rien en attendre.
constexpr uint16_t kOpOpen     = 0x0b6e;  // ZC_SE_CASHSHOP_OPEN (points)
constexpr uint16_t kOpItemList = 0x08ca;  // ZC_ACK_SCHEDULER_CASHITEM (items/onglet)
constexpr uint16_t kOpResult   = 0x0849;  // ZC_SE_PC_BUY_CASHITEM_RESULT
// Envois (CZ).
constexpr uint16_t kOpListReq  = 0x08c9;  // list request -> dΓ©clenche les 0x08ca
constexpr uint16_t kOpBuy      = 0x0848;  // CZ_SE_PC_BUY_CASHITEM_LIST
constexpr uint16_t kOpClose    = 0x084a;  // CZ cashshop close (2 octets)

CashShopTweaks::CashShopTweaks() {
  // ZC_SE_CASHSHOP_OPEN : [cash:4][kafra:4][tab:4] = 12 octets aprΓ¨s l'opcode.
  Bourgeon::Instance().RegisterObserveOpcode(kOpOpen, 12);
  // ZC_ACK_SCHEDULER_CASHITEM (var) : [len:2][count:2][tabNum:2] = 6 octets pour
  // atteindre l'en-tΓͺte ; les items sont lus directement dans le buffer live.
  Bourgeon::Instance().RegisterObserveOpcode(kOpItemList, 6);
  // ZC_SE_PC_BUY_CASHITEM_RESULT : [itemId:4][result:2][cash:4][kafra:4] = 14 o.
  Bourgeon::Instance().RegisterObserveOpcode(kOpResult, 14);
}

void CashShopTweaks::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                  uint16_t len) {
  if (opcode == kOpOpen) {
    if (len < 8) return;
    cash_points_  = *reinterpret_cast<const uint32_t*>(data);
    kafra_points_ = *reinterpret_cast<const uint32_t*>(data + 4);
    return;
  }
  if (opcode == kOpItemList) {
    // data = [packetLength:2][count:2][tabNum:2][items...] (ZC_ACK_SCHEDULER_CASHITEM,
    // un paquet par onglet). packetLength inclut l'en-tΓͺte [opcode:2] -> le pas d'un
    // item = (packetLength - 8) / count : robuste au ENABLE_CASHSHOP_PREVIEW_PATCH
    // (item = itemId:4 + price:4 [+ viewSprite:2 + location:4]). data pointe dans le
    // buffer recv live (paquet complet prΓ©sent) -> on lit au-delΓ  des `len` forwardΓ©s.
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
    // 1er paquet de CET onglet (tabNum != prΓ©cΓ©dent) -> on repart de zΓ©ro ;
    // sinon = continuation d'un onglet dΓ©coupΓ© -> on APPEND (ne pas Γ©craser).
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
    if (last_result_ == 0) cart_.clear();  // succΓ¨s -> panier vidΓ©
    return;
  }
}

// Demande la liste des items du cash shop (2 octets = juste l'opcode). Le serveur
// rΓ©pond par une sΓ©rie de ZC_ACK_SCHEDULER_CASHITEM 0x08ca (un par onglet rempli),
// mais UNE SEULE FOIS par session (flag cashshop_sent) : le client natif l'envoie
// dΓ©jΓ  Γ  l'ouverture, donc c'est surtout un filet de sΓ©curitΓ©.
void CashShopTweaks::RequestTab(int /*tab*/) {
  uint16_t op = kOpListReq;
  Bourgeon::Instance().SendPacket(reinterpret_cast<uint8_t*>(&op), sizeof(op));
}

void CashShopTweaks::AddToCart(uint32_t id, int tab, int32_t price) {
  for (auto& e : cart_) {
    if (e.id == id && e.tab == tab) { ++e.amount; return; }
  }
  cart_.push_back(CartEntry{id, tab, 1, price});
}

// CZ_SE_PC_BUY_CASHITEM_LIST 0x848 :
//   [type:2][packetLength:2][count:2][kafraPoints:4][ {id:4,amount:4,tab:2} *count ]
void CashShopTweaks::SendBuy() {
  if (cart_.empty()) return;
  const int count = static_cast<int>(cart_.size());
  const int plen = 10 + 10 * count;
  // kafraPoints = montant EXACT de points d'Event Γ  dΓ©penser (le serveur paie le
  // reste `price - kafraPoints` en points de Vote : cash = price - points, SANS
  // borner points au prix). Donc envoyer tout le solde draine tout + fausse le
  // Vote. On dΓ©pense au plus le total du panier (min(total, solde)).
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
void CashShopTweaks::BuyNow(uint32_t id, int tab, int32_t price) {
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
  // Fermeture (idem bouton X) : le serveur ferme la session, la fenêtre disparaît.
  uint16_t op = kOpClose;
  Bourgeon::Instance().SendPacket(reinterpret_cast<uint8_t*>(&op), sizeof(op));
  CloseNativeCashShop();
}

void CashShopTweaks::HideNativeAtCreation(void* win) {
  if (!win || !imgui_enabled_) return;
  __try {
    if (*reinterpret_cast<uintptr_t*>(win) != kCashVTable) return;
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void CashShopTweaks::OnTick() {
  open_ = false;
  void* wnd = FindCashWnd();
  if (wnd) {
    __try {
      if (!was_open_) {
        spawn_x_ = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(wnd) + kOffPosX);
        spawn_y_ = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(wnd) + kOffPosY);
        need_pos_ = true;
      }
      // Master switch : viewer ON -> cache le natif chaque tick (le natif peut
      // remettre +0x28=1 sur Γ©vΓ©nement) ; OFF -> laisse le natif visible.
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(wnd) + kOffVisible) =
          imgui_enabled_ ? 0 : 1;
      open_ = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { open_ = false; }
  }
  // Γ la 1re ouverture : demander la liste (filet de sΓ©curitΓ© ; le natif l'a dΓ©jΓ 
  // demandΓ©e -> le serveur peut ne rien renvoyer si cashshop_sent est dΓ©jΓ  posΓ©,
  // auquel cas notre observe a captΓ© les 0x08ca du natif).
  if (open_ && !was_open_) {
    last_result_ = -1;
    last_recv_tab_ = -1;  // nouvelle salve de listes -> le 1er paquet videra son onglet
    RequestTab(0);
  }
  was_open_ = open_;
}

void CashShopTweaks::OnRenderUI() {
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

  // MΓͺme style de fenΓͺtre que MoonlightUi (cadre arrondi / roundframe).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);  // grille/panier/cartes arrondis
  const bool begun =
      ro::BeginRoWindow("Vote Shop###bourgeon_cashshop", &show_panel_,
                        ImGuiWindowFlags_NoCollapse);
  if (!show_panel_) {
    // X (ou Γchap) -> on FERME rΓ©ellement le cash shop : paquet de fermeture serveur
    // (CZ 0x084a, reset npc_shopid) + destruction de la fenΓͺtre native (id 0x13e).
    // Ensuite FindWindow(0x13e) rend null -> open_ passe Γ  false -> le viewer
    // disparaΓ�t. On remet show_panel_ Γ  true pour la prochaine ouverture.
    uint16_t op = kOpClose;
    Bourgeon::Instance().SendPacket(reinterpret_cast<uint8_t*>(&op), sizeof(op));
    CloseNativeCashShop();
    show_panel_ = true;
  }
  if (!begun) { ro::EndRoWindow(); ImGui::PopStyleVar(5); return; }

  //  En-tΓͺte : points du compte 
  const ImVec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);  // texte noir (skin RO clair)
  ImGui::TextColored(kBlack, "Vote: %u", cash_points_);
  ImGui::SameLine();
  ImGui::TextColored(kBlack, " | Points d'Event: %u", kafra_points_);
  ImGui::SameLine();
  ro::RoCheckbox("Utiliser mes points d'Event d'abord", &use_kafra_);
  if (last_result_ == 0) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), " | Achat OK");
  } else if (last_result_ > 0) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), " | Achat refuse (%d)",
                       last_result_);
  }
  ImGui::Separator();

  //  Onglets de catΓ©gorie 
  if (ImGui::BeginTabBar("cashshop_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
    for (int t = 0; t < kNumTabs; ++t) {
      if (!kTabShown[t]) continue;  // onglet toujours vide -> masquΓ©
      char lbl[48];
      std::snprintf(lbl, sizeof(lbl), "%s (%d)###cstab%d", kTabLabels[t],
                    static_cast<int>(tabs_[t].size()), t);
      if (ImGui::BeginTabItem(lbl)) {
        if (cur_tab_ != t) cur_slot_ = -1;  // changer d'onglet -> filtre slot remis Γ  Tous
        cur_tab_ = t;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }

  static ImGuiTextFilter filter;
  ImGui::SetNextItemWidth(-1.0f);
  // Champ filtre avec texte d'aide grise (placeholder) quand il est vide. On pilote
  // directement le buffer du ImGuiTextFilter (InputBuf/Build) pour garder le filtrage
  // natif tout en ayant le hint (filter.Draw() n'expose pas de hint).
  if (ImGui::InputTextWithHint("##cs_filter", "Filtrer...", filter.InputBuf,
                               IM_ARRAYSIZE(filter.InputBuf)))
    filter.Build();

  //  Filtre par emplacement d'Γ©quipement (slots prΓ©sents dans l'onglet)
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
      size_t p = slots.size();  // insertion triΓ©e par clΓ©
      while (p > 0 && slots[p - 1].key > s.key) --p;
      slots.insert(slots.begin() + p, s);
    }
  }
  // Entrée virtuelle "Costumes hat-effect" (clé 100 > toutes les clés réelles ->
  // en fin de liste), présente seulement si l'onglet en contient.
  if (has_hateffect) slots.push_back({kSlotHatEffect, "Costumes hat-effect"});
  // Un seul slot "Autre" (99) => onglet non-Γ©quipable : pas de filtre utile.
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
  const char* kSortLabels[] = {"Nom", "ID", "Cout"};
  if (ro::RoBeginCombo("##cs_sort", kSortLabels[cur_sort_])) {
    for (int s = 0; s < 3; ++s)
      if (ImGui::Selectable(kSortLabels[s], cur_sort_ == s)) cur_sort_ = s;
    ro::RoEndCombo();
  }
  ImGui::SameLine();
  if (ro::RoButton(sort_asc_ ? "Asc" : "Desc")) sort_asc_ = !sort_asc_;

  //  Disposition : grille Γ  gauche, panier Γ  droite (comme le cash shop natif) 
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

  //  Grille d'items : cartes Γ  TAILLE FIXE (child) -> le texte est bornΓ© Γ  la
  // carte (sinon TextWrapped s'Γ©tale sur toute la fenΓͺtre et casse la grille) 
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
    // Liste filtrΓ©e (ptrs) : une catΓ©gorie peut avoir 2500+ items -> on CLIPPE par
    // rangΓ©e (ImGuiListClipper) pour ne dessiner que les cartes visibles (sinon
    // 2500 child-windows/frame = chute de FPS).
    std::vector<const CashItem*> vis;
    vis.reserve(tabs_[cur_tab_].size());
    for (const auto& ci : tabs_[cur_tab_]) {
      if (cur_slot_ == kSlotHatEffect) {
        if (!IsHatEffect(ci)) continue;          // filtre "Costumes hat-effect"
      } else if (cur_slot_ != -1 && SlotOf(ci.location).key != cur_slot_) {
        continue;
      }
      if (filter.PassFilter(ItemName(ci.id))) vis.push_back(&ci);
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
                  c = std::strcmp(ItemName(a->id), ItemName(b->id));
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
      // 1) Nom EN HAUT (texte foncΓ©, coupΓ© Γ  la largeur de la carte).
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
        const char* nm = ItemName(ci.id);
        ImFont* font = ImGui::GetFont();
        const float base = ImGui::GetFontSize();
        const float availw = card_w - 8.0f;
        const float tw = ImGui::CalcTextSize(nm).x;  // largeur a la taille de base
        float fsz = (tw > availw && tw > 0.0f) ? base * (availw / tw) : base;
        if (fsz < base * 0.55f) fsz = base * 0.55f;
        const ImVec2 tp(wp.x + 4.0f, wp.y + (header_h - fsz) * 0.5f);
        dl->AddText(font, fsz, tp, card_txt, nm);
      }
      // 2) RangΓ©e du bas ancrΓ©e : image Γ  GAUCHE, prix + Buy Γ  DROITE.
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
              ? "Solde Vote insuffisant - cochez \"Utiliser Event\" pour cumuler"
              : "Solde Vote + Event insuffisant pour cet item";
      if (!afford) ImGui::BeginDisabled();
      ImGui::SetCursorPos(ImVec2(cx, cy + ImGui::GetTextLineHeight() + sp));
      if (ro::RoButton("Panier", colw, frameH))
        AddToCart(ci.id, cur_tab_, ci.price);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(afford ? "Ajouter au panier (achat groupe via Acheter)"
                                 : why);
      ImGui::SetCursorPos(
          ImVec2(cx, cy + ImGui::GetTextLineHeight() + frameH + 2.0f * sp));
      if (ro::RoButton("Achat 1-Click", colw, frameH))
        BuyNow(ci.id, cur_tab_, ci.price);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(afford ? "Achat immediat d'1 unite, puis fermeture du shop"
                                 : why);
      if (!afford) ImGui::EndDisabled();
      ImGui::EndChild();
      ImGui::PopStyleVar();    // WindowPadding (carte)
      ImGui::PopStyleColor();  // ChildBg
      if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const ImVec2 mp = ImGui::GetMousePos();
        OpenItemDesc(ci.id, ci.view, ci.location, static_cast<int>(mp.x),
                     static_cast<int>(mp.y));
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
    if (n == 0) ImGui::TextDisabled("(aucun item dans cette categorie)");
  }
  ImGui::EndChild();
  preview_active_ = preview_now;  // gele le scroll grille tant qu'on survole un apercu

  //  Panier (Γ  droite) 
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
    ImGui::TextWrapped("%s", ItemName(e.id));
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
    ImGui::TextColored(kBlack, "%lld pts", static_cast<long long>(e.price) * e.amount);
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
  ImGui::Text("Total: %lld pts", total);
  // Solde depensable selon la coche "Utiliser Event" (cumul si cochee, Vote seul
  // sinon) : identique a la logique des cartes + a ce que l'achat depense reellement.
  const long long buy_avail =
      use_kafra_ ? static_cast<long long>(cash_points_) + kafra_points_
                 : static_cast<long long>(cash_points_);
  const bool afford = total <= buy_avail;
  if (cart_.empty()) ImGui::BeginDisabled();
  if (ro::RoButton(afford ? "Acheter" : "Points insuffisants",
                   ImGui::GetContentRegionAvail().x, 0))
    SendBuy();
  if (cart_.empty()) ImGui::EndDisabled();
  ImGui::EndChild();
  }  // if (show_cart) : panier masqué quand vide

  ro::EndRoWindow();
  ImGui::PopStyleVar(5);
}
