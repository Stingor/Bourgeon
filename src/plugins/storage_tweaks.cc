#include "plugins/storage_tweaks.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "imgui.h"
#include "plugins/bourgeon_opcodes.h"  // bopcodes::kStoragePrices

// ── Constantes RE (client 20250716, base 0x400000 ; cf. project_storage_window_re)
namespace {

// Slot manager de la fenêtre storage (id 0x21) : mgr+0x288. Non-nul <=> ouverte,
// remis à 0 à la fermeture. = ce que FindWindow(0x21) renvoie. Relire FRAIS.
constexpr uintptr_t kStorageSlot = 0x0131f770;
constexpr uintptr_t kStorageVTable = 0x0103ca40;  // UIItemStoreWnd

// Offsets UIWindow / UIItemStoreWnd.
constexpr int kOffWidth  = 0x14;
constexpr int kOffHeight = 0x18;
constexpr int kOffPosX   = 0x1c;
constexpr int kOffPosY   = 0x20;
constexpr int kOffVisible = 0x28;  // flag visibilité (Show/Hide 0x005aad80 : wnd+0x28=flag)
constexpr int kOffList   = 0xe8;   // std::list _Myhead (sentinelle)
constexpr int kOffCount  = 0xec;   // std::list _Mysize
constexpr int kOffUsed   = 0x188;  // items utilisés
constexpr int kOffMax    = 0x18c;  // capacité max

// Nœud de liste (std::list MSVC) : next@+0, prev@+4, value@+8.
constexpr int kNodeNext = 0x00;
constexpr int kNodeAmt  = 0x18;  // value+0x10 = quantité
constexpr int kNodeInfo = 0x08;  // value = ItemSkillInfo (arg de GetBaseName)

// Champs DANS l'ItemSkillInfo (= node+kNodeInfo), tels que lus par FUN_008711a0 :
// l'id est une std::string à +0x2c (le jeu fait atoi dessus pour l'icône), le
// flag identifié est à +0x5c. (node+0xc N'EST PAS l'id fiable pour la liste vue.)
constexpr int kInfoIdStr = 0x2c;  // std::string id (SSO ; heap si cap>0xf)
constexpr int kInfoIdCap = 0x40;  // capacité de la std::string id (= +0x2c+0x14)
constexpr int kInfoIdent = 0x5c;  // byte : item identifié ?

// Nom de base de l'item : __thiscall(info, char* out, size_t* cap, char flag).
constexpr uintptr_t kGetBaseName = 0x006a2b50;
using GetBaseName_t = size_t(__thiscall*)(void*, char*, size_t*, char);

// Nom COMPLET (raffinement +N / [slots] / cartes / enchant) : BuildDisplayName.
// (this=wnd, info, &colorOut, &offVec, &bufptr, &cap, &hlptr, f7, f8). offVec est
// alloué par le jeu -> à libérer avec game_free.
constexpr uintptr_t kBuildName = 0x008a0570;
constexpr uintptr_t kGameFree  = 0x00dbbc7f;
struct GVec { int* first; int* last; int* end; };  // std::vector MSVC (jeu)
using BuildName_t = int(__thiscall*)(void*, void*, int*, GVec*, char**, size_t*,
                                     char**, char, char);
using GameFree_t  = void(__cdecl*)(void*);

// ── Icônes d'item (bmp inventaire) ──────────────────────────────────────────
// BuildItemIconGrfPath(id_str, out[128], identified) __stdcall (RET 0xc, 3 args) :
// atoi(id) -> ResolveItemResNameById -> sprintf "유저인터페이스\item\<res>.bmp"
// (identified!=0 -> resname [rec+8], sinon [rec+0x1c]). On passe identified=1.
constexpr uintptr_t kBuildIconPath = 0x00d5a720;
constexpr uintptr_t kTexMgr  = 0x00a90350;
constexpr uintptr_t kMakeKey = 0x00a9f030;
constexpr uintptr_t kLoadTex = 0x00a8d4a0;
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;
using BuildIconPath_t = void(__stdcall*)(const char*, char*, int);
using TexMgr_t  = void*(__cdecl*)();
using MakeKey_t = void*(__cdecl*)(const char*);
using LoadTex_t = void*(__fastcall*)(void*, void*, void*);

// Texture ImGui d'une icône + dimensions natives (ratio préservé).
struct IconTex { void* tex = nullptr; int w = 0; int h = 0; };
// Cache id -> IconTex (tex null = miss connu, pas de reload chaque frame).
std::unordered_map<uint32_t, IconTex> g_icon_cache;

// Résout le .bmp en pixels bruts BGRA via le TexMgr natif. SEH (POD only) ; la
// conversion C++ est faite hors __try par l'appelant.
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

// SEH isolé (POD only, pas d'objet C++ à dérouler -> évite C2712) : construit le
// chemin d'icône `유저인터페이스\item\<res>.bmp` depuis l'id.
bool BuildIconPathSafe(uint32_t id, char* out, int identified) {
  char idstr[16];
  std::snprintf(idstr, sizeof(idstr), "%u", id);
  out[0] = '\0';
  __try {
    reinterpret_cast<BuildIconPath_t>(kBuildIconPath)(idstr, out, identified);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Charge l'icône d'un item (par id) en texture ImGui (colorkey magenta -> alpha 0).
IconTex LoadItemIcon(uint32_t id, int identified) {
  char path[160];
  if (!BuildIconPathSafe(id, path, identified)) return {};
  RawTex rt{};
  if (!GetRawTex(path, &rt)) return {};
  std::vector<uint8_t> argb(static_cast<size_t>(rt.w) * rt.h * 4);
  for (int i = 0; i < rt.w * rt.h; ++i) {
    const uint8_t b = rt.bgra[i * 4], g = rt.bgra[i * 4 + 1],
                  r = rt.bgra[i * 4 + 2];
    const bool ck = (r == 0xFF && g == 0 && b == 0xFF);  // magenta -> transparent
    argb[i * 4] = b; argb[i * 4 + 1] = g; argb[i * 4 + 2] = r;
    argb[i * 4 + 3] = ck ? 0 : 0xFF;
  }
  return {Overlay_CreateTextureARGB(argb.data(), rt.w, rt.h), rt.w, rt.h};
}

// Résout (cache + charge) l'icône d'un item. Appelé au rendu (création de
// texture D3D à EndScene, comme item_desc_tweaks).
IconTex ResolveIcon(uint32_t id, int identified) {
  auto it = g_icon_cache.find(id);
  if (it != g_icon_cache.end()) return it->second;
  IconTex t = LoadItemIcon(id, identified);
  g_icon_cache[id] = t;
  return t;
}

// ── Ouverture de la description d'item (clic-droit) ─────────────────────────
// FIDÈLE AU NATIF : le clic-droit du storage passe l'ItemSkillInfo COMPLET du
// nœud (chargé par le serveur, avec tout ce dont la desc a besoin) à OnMsg 0x18.
// Un ItemSkillInfo reconstruit (ctor+SetId) pose l'id mais PAS la desc -> vide.
// Donc on re-parcourt la liste live au clic pour retrouver le nœud par id et
// passer SON info (node+8). OnMsg 0x18 copie ce qu'il faut (on ne possède pas
// l'info -> aucun free). item_desc_tweaks détecte 0xc et rend sa version enrichie.
constexpr uintptr_t kUIWindowMgr = 0x0131f4e8;
constexpr uintptr_t kMakeWindow  = 0x00a39340;  // __fastcall(mgr, edx, id) -> wnd
constexpr int kWinItemDesc = 0xc;    // fenêtre desc ITEM (OnMsg 0x18 + &ItemSkillInfo)
constexpr int kMsgSetItem  = 0x18;
constexpr int kVfOnMsg     = 0x94;   // vtable+0x94 = OnMsg
constexpr int kVfSetPos    = 0x10;   // vtable+0x10 = SetPos(x,y)
using MakeWindow_t = void*(__fastcall*)(void*, void*, void*);
using OnMsg_t      = int  (__fastcall*)(void*, void*, int, int, int, int, int, int);
using SetPos_t     = void (__fastcall*)(void*, void*, int, int);

// Appelle une méthode virtuelle (offset en octets) de `self`.
template <typename Fn>
inline Fn Vf(void* self, int off) {
  return reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t**>(self))[off / 4]);
}

// Ouvre la fenêtre de description native (id 0xc) pour l'item `id` du storage,
// à (mx,my) écran. Re-parcourt la liste live pour passer l'info COMPLÈTE du nœud.
void OpenItemDesc(uint32_t id, int mx, int my) {
  if (id == 0) return;
  __try {
    // Parcourir le MODÈLE SESSION (0x015fbad8), PAS la liste de la fenêtre (wnd+0xe8) :
    // quand on cache le natif (wnd+0x28=0), sa liste d'affichage n'est plus peuplée,
    // mais le modèle session l'est toujours (c'est ce que lit Extract). Même struct.
    uint8_t* head = *reinterpret_cast<uint8_t**>(0x015fbad8);
    if (!head) return;
    uint8_t* found = nullptr;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    for (int guard = 0; node && node != head && guard < 1000; ++guard) {
      uint8_t* info = node + kNodeInfo;
      const uint32_t cap = *reinterpret_cast<uint32_t*>(info + kInfoIdCap);
      const char* ids = (cap > 0xf) ? *reinterpret_cast<char**>(info + kInfoIdStr)
                                    : reinterpret_cast<const char*>(info + kInfoIdStr);
      if (ids && static_cast<uint32_t>(atoi(ids)) == id) { found = info; break; }
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
    }
    if (!found) return;
    void* mgr = reinterpret_cast<void*>(kUIWindowMgr);
    void* dwnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
        mgr, nullptr, reinterpret_cast<void*>(kWinItemDesc));
    if (dwnd) {
      Vf<OnMsg_t>(dwnd, kVfOnMsg)(dwnd, nullptr, 0, kMsgSetItem,
                                  static_cast<int>(reinterpret_cast<uintptr_t>(found)),
                                  0, 0, 0);
      Vf<SetPos_t>(dwnd, kVfSetPos)(dwnd, nullptr, mx, my);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Retrait d'un item vers l'inventaire (interactif) ────────────────────────
// Réplique la branche ALT du clic-droit natif (UIItemStoreWnd_OnRButtonUp) :
// dispatcher = *(0x0121333c) ; dispatcher->vtable[0x18](0x38, index, amount, 0, 0)
// __thiscall. cmd 0x38 = "storage -> inventaire" ; index = info+4 (node+0xc),
// amount = quantité à retirer. Le serveur renvoie l'update -> le modèle et le
// viewer se rafraîchissent seuls (synchro). Le flag natif disp+0x5ce est un
// simple anti-rebond côté appelant, pas requis par la commande.
constexpr uintptr_t kUICmdDisp   = 0x0121333c;  // *(void**) = g_UICommandDispatcher
constexpr int       kCmdWithdraw = 0x38;         // storage -> body/inventaire
using DispCmd_t = void(__thiscall*)(void*, int, int, int, int, int);

void WithdrawItem(int index, int amount) {
  if (amount <= 0) return;
  __try {
    void* disp = *reinterpret_cast<void**>(kUICmdDisp);
    if (disp)
      Vf<DispCmd_t>(disp, 0x18)(disp, kCmdWithdraw, index, amount, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Fenêtre inventaire (id 8) : global + vtable pour tester un drop dessus.
constexpr uintptr_t kInvWndGlobal = 0x0131f6bc;
constexpr uintptr_t kInvVTable    = 0x0103d460;
bool MouseOverInventory(float x, float y) {
  __try {
    uint8_t* inv = *reinterpret_cast<uint8_t**>(kInvWndGlobal);
    if (!inv || *reinterpret_cast<uintptr_t*>(inv) != kInvVTable) return false;
    const int ix = *reinterpret_cast<int*>(inv + 0x1c);
    const int iy = *reinterpret_cast<int*>(inv + 0x20);
    const int iw = *reinterpret_cast<int*>(inv + 0x14);
    const int ih = *reinterpret_cast<int*>(inv + 0x18);
    return x >= ix && y >= iy && x < ix + iw && y < iy + ih;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Fenêtre CART (chariot marchand) : MÊME framework générique que l'inventaire
// (vtable sœur 0x0103d538, rect aux mêmes offsets). Global 0x0131f6a0 trouvé en
// RE live (contient le ptr fenêtre cart quand ouvert, 0 sinon). Utilisé pour le
// hit-test « lâcher un item storage sur le cart » (storage->cart).
constexpr uintptr_t kCartWndGlobal = 0x0131f6a0;
constexpr uintptr_t kCartVTable    = 0x0103d538;
bool MouseOverCart(float x, float y) {
  __try {
    uint8_t* cart = *reinterpret_cast<uint8_t**>(kCartWndGlobal);
    if (!cart || *reinterpret_cast<uintptr_t*>(cart) != kCartVTable) return false;
    const int cx = *reinterpret_cast<int*>(cart + 0x1c);
    const int cy = *reinterpret_cast<int*>(cart + 0x20);
    const int cw = *reinterpret_cast<int*>(cart + 0x14);
    const int ch = *reinterpret_cast<int*>(cart + 0x18);
    return x >= cx && y >= cy && x < cx + cw && y < cy + ch;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── Onglets de catégorie (groupes de types d'item, repris du filtre natif) ──
// `sub` = dimension de sous-catégorie de l'onglet (combo déroulant) : voir SubDim.
enum SubDim { kSubNone = 0, kSubWeapon, kSubArmor, kSubCard, kSubAmmo, kSubCostume };
// Masque des slots costume (rAthena EQP_COSTUME_* : head top/mid/low + garment).
// Un item avec un de ces bits = costume -> onglet Costumes, exclu des Armures.
constexpr uint32_t kCostumeMask = 0x3C00;
struct StgCat { const char* label; const int* types; int n; int sub; };
const int kCatConso[]  = {0, 1, 2};
const int kCatArme[]   = {5, 8, 9, 0xf};
const int kCatArmure[] = {4, 0xb, 0xc, 0xd, 0xe};
const int kCatCarte[]  = {6};
const int kCatMuni[]   = {10, 0x10, 0x11, 0x13};
const int kCatCash[]   = {0x12};
const int kCatDivers[] = {3, 7};
const StgCat kStgCats[] = {
    {"Tout", nullptr, 0, kSubNone},
    {"Consos", kCatConso, 3, kSubNone},
    {"Armes", kCatArme, 4, kSubWeapon},
    {"Armures", kCatArmure, 5, kSubArmor},
    {"Costumes", nullptr, 0, kSubCostume},  // filtré par equip mask, pas par type
    {"Cartes", kCatCarte, 1, kSubCard},
    {"Munitions", kCatMuni, 4, kSubAmmo},
    {"Cash", kCatCash, 1, kSubNone},
    {"Etc", kCatDivers, 2, kSubNone},
};
constexpr int kNumStgCats = 9;
bool ItemInCat(int tab, int type) {
  if (tab <= 0 || tab >= kNumStgCats) return true;  // Tout
  const StgCat& c = kStgCats[tab];
  for (int i = 0; i < c.n; ++i)
    if (c.types[i] == type) return true;
  return false;
}
// Filtrage d'onglet meta-aware (a besoin du masque equip pour les costumes) :
//   - onglet Costumes : uniquement les items avec un bit costume ;
//   - autres onglets (sauf Tout) : les costumes sont EXCLUS (ils vont dans Costumes) ;
//   - reste : filtre par type (Tout inclut tout, costumes compris).
bool ItemInTab(int tab, int type, uint32_t equip) {
  if (kStgCats[tab].sub == kSubCostume) return (equip & kCostumeMask) != 0;
  if (tab != 0 && (equip & kCostumeMask) != 0) return false;
  return ItemInCat(tab, type);
}

// ── Sous-catégories (subtype d'arme/munition, slot d'équip) ──────────────────
// Type d'arme (item_data.subtype pour IT_WEAPON = rAthena e_weapon_type W_*).
const char* WeaponLabel(uint8_t st) {
  switch (st) {
    case 0:  return "Fists";       case 1:  return "Dagger";
    case 2:  return "Sword 1H";     case 3:  return "Sword 2H";
    case 4:  return "Spear 1H";    case 5:  return "Spear 2H";
    case 6:  return "Axe 1H";    case 7:  return "Axe 2H";
    case 8:  return "Mace";       case 9:  return "Mace 2H";
    case 10: return "Staff";       case 11: return "Bow";
    case 12: return "Knuckle";     case 13: return "Instrument";
    case 14: return "Whip";       case 15: return "Book";
    case 16: return "Katar";       case 17: return "Revolver";
    case 18: return "Rifle";       case 19: return "Gatling";
    case 20: return "Shotgun";     case 21: return "Grenade Launcher";
    case 22: return "Huuma";       case 23: return "Two-Handed Staff";
    default: return "Other";
  }
}
// Type de munition (item_data.subtype pour IT_AMMO = rAthena e_ammo_type A_*).
const char* AmmoLabel(uint8_t st) {
  switch (st) {
    case 1: return "Arrow";     case 2: return "Throwing Dagger";
    case 3: return "Bullet";      case 4: return "Cartridge";
    case 5: return "Grenade";    case 6: return "Shuriken";
    case 7: return "Kunai";      case 8: return "Cannonball";
    case 9: return "Throwing Weapon";   default: return "Other";
  }
}
// Slot d'équipement principal depuis le masque item_data.equip (rAthena EQP_*).
// Renvoie {clé d'ordre stable, label} ; sert aux armures ET aux cartes (cible).
struct SubCat { int key; const char* label; };
SubCat PrimaryEquipSlot(uint32_t e) {
  // Coiffe séparée en 3 slots distincts (priorité haut > milieu > bas si multi-slot).
  if (e & 0x100)                    return {0,  "Tete haut"};   // HEAD_TOP
  if (e & 0x200)                    return {1,  "Tete milieu"}; // HEAD_MID
  if (e & 0x001)                    return {2,  "Tete bas"};    // HEAD_LOW
  if (e & 0x010)                    return {3,  "Body armor"};       // ARMOR
  if (e & 0x004)                    return {4,  "Garment"};     // GARMENT
  if (e & 0x040)                    return {5,  "Shoes"};  // SHOES
  if (e & (0x008 | 0x080))          return {6,  "Accessory"};  // ACC L/R
  if (e & 0x020)                    return {7,  "Shield"};    // HAND_L
  if (e & 0x002)                    return {8,  "Weapon"};        // HAND_R (cartes d'arme)
  if (e & 0x8000)                   return {9,  "Ammunition"};    // AMMO
  if (e & 0x3C00)                   return {10, "Costume"};     // COSTUME_*
  if (e & 0x3F0000)                 return {11, "Shadow"};      // SHADOW_*
  return {99, "Other"};  // pas de slot principal connu (ex: cartes d'arme, cartes de costume)
}
// Slot d'un COSTUME depuis le masque equip (bits COSTUME_* distincts des slots
// normaux). Labels alignés sur PrimaryEquipSlot pour la cohérence visuelle.
SubCat CostumeSlot(uint32_t e) {
  if (e & 0x0400) return {0, "Tete haut"};    // COSTUME_HEAD_TOP
  if (e & 0x0800) return {1, "Tete milieu"};  // COSTUME_HEAD_MID
  if (e & 0x1000) return {2, "Tete bas"};     // COSTUME_HEAD_LOW
  if (e & 0x2000) return {3, "Garment"};      // COSTUME_GARMENT
  return {99, "Other"};
}
// Sous-catégorie d'un item pour la dimension `dim` de l'onglet courant.
// Renvoie {-1, nullptr} si pas de sous-catégorie applicable.
SubCat SubCatOf(int dim, uint8_t subtype, uint32_t equip) {
  switch (dim) {
    case kSubWeapon:  return {subtype, WeaponLabel(subtype)};
    case kSubAmmo:    return {subtype, AmmoLabel(subtype)};
    case kSubArmor:   return PrimaryEquipSlot(equip);
    case kSubCard:    return PrimaryEquipSlot(equip);  // cible de la carte
    case kSubCostume: return CostumeSlot(equip);
    default:          return {-1, nullptr};
  }
}

// ── Dépôt inventaire -> storage par drag natif ──────────────────────────────
// Paquet CZ_MOVE_ITEM_FROM_BODY_TO_STORE 0x00f3 : [op:2][index:2][amount:4] (8o).
// Lecture du drag natif (repris de skill_bar) : DragObj = FUN_00a75340(0x1213338),
// charge à obj+0x308 (+0x80 format 0=item, +0x18 nameid). Index/qté inventaire via
// ItemMgr_GetInvItemById (out+0x04 found, +0x08 index, +0x10 qty).
// Opcode ACTIF pour ce packetver (>= 20130320) = 0x08ac (MoveToKafra) ; 0x00f3
// est réassigné à un autre paquet (48o) -> disconnect. Confirmé serveur.
constexpr uint16_t  kOpDeposit  = 0x08ac;
// Fermeture de l'entrepôt : CZ_CloseKafra, opcode fixe 2 octets (juste l'opcode).
// Confirmé client (opcode_map.md : 0x0193 CZ FIX 2 "CloseKafra") ET serveur moonlight
// (clif_packetdb.hpp: parseable_packet(0x0193,2,clif_parse_CloseKafra) -> storage_storageclose).
// PAS remappé par le shuffle 20130320 (contrairement à MoveToKafra 0x08ac / MoveFromKafra 0x0874).
constexpr uint16_t  kOpCloseStorage = 0x0193;
// storage -> cart : CZ_MOVE_ITEM_FROM_STORE_TO_CART, fixe 8 octets [op][index:2][amount:4].
// Confirmé client (opcode_map.md 0x0128 CZ FIX 8) + serveur (server_storage_index -> -1).
// On envoie items_[idx].index (= index storage CLIENT, le serveur fait -1). PAS remappé.
constexpr uint16_t  kOpStorageToCart = 0x0128;
// cart -> storage : CZ_MOVE_ITEM_FROM_CART_TO_STORE, fixe 8 octets, serveur server_index -> -2.
constexpr uint16_t  kOpCartToStorage = 0x0129;
constexpr uintptr_t kDragMgr    = 0x01213338;
constexpr uintptr_t kGetDragObj = 0x00a75340;
constexpr uintptr_t kGetInvItem = 0x00d7fa90;
constexpr int kPayloadOff = 0x308;
constexpr int kPL_type = 0x80, kPL_nameid = 0x18, kPL_id = 0x04, kPL_cat = 0x00;
constexpr int kInvFound = 0x04, kInvIndex = 0x08, kInvQty = 0x10;
using GetDragObj_t = void*(__fastcall*)(void*);
using GetInvItem_t = void*(__stdcall*)(void*, int);

void* DragObj() {
  __try {
    return reinterpret_cast<GetDragObj_t>(kGetDragObj)(
        reinterpret_cast<void*>(kDragMgr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Vide la charge du drag natif -> au relâché le jeu ne voit plus d'objet (pas de
// drop au sol, curseur-suiveur natif disparaît). NE touche PAS le gate dispatcher.
void CancelNativeDrag(void* obj) {
  __try {
    uint8_t* p = reinterpret_cast<uint8_t*>(obj) + kPayloadOff;
    *reinterpret_cast<int*>(p + kPL_cat) = 0;
    *reinterpret_cast<int*>(p + kPL_id)  = 0;
    p[kPL_type] = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SendDeposit(int index, int amount) {
  if (amount <= 0) return;
  uint8_t pkt[8];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpDeposit;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(index);
  *reinterpret_cast<uint32_t*>(pkt + 4) = static_cast<uint32_t>(amount);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// Demande au serveur de fermer l'entrepôt (CZ_CloseKafra, 2 octets = juste l'opcode).
// Le serveur ferme la session storage -> les DEUX fenêtres (native + viewer) se ferment.
void SendCloseStorage() {
  uint16_t op = kOpCloseStorage;
  Bourgeon::Instance().SendPacket(reinterpret_cast<uint8_t*>(&op), sizeof(op));
}

// storage -> cart : envoie un item de l'entrepôt vers le chariot. index = index
// storage CLIENT (items_[idx].index) ; le serveur applique server_storage_index (-1).
void SendStorageToCart(int index, int amount) {
  if (amount <= 0) return;
  uint8_t pkt[8];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpStorageToCart;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(index);
  *reinterpret_cast<uint32_t*>(pkt + 4) = static_cast<uint32_t>(amount);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// cart -> storage : envoie un item du chariot vers l'entrepôt. index = index cart
// CLIENT (lu du payload de drag natif) ; le serveur applique server_index (-2).
void SendCartToStorage(int index, int amount) {
  if (amount <= 0) return;
  uint8_t pkt[8];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpCartToStorage;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(index);
  *reinterpret_cast<uint32_t*>(pkt + 4) = static_cast<uint32_t>(amount);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// Lit l'item d'un drag natif d'inventaire -> {index inventaire, qté}. Confirmé
// par dump live : payload+0x04 = index inventaire client, +0x10 = count, +0x80=0
// (FullPayload item). SEH (POD). false si pas un item / index invalide.
bool ReadDraggedInvItem(void* obj, int* index, int* qty) {
  __try {
    uint8_t* p = reinterpret_cast<uint8_t*>(obj) + kPayloadOff;
    if (p[kPL_type] != 0) return false;  // FullPayload (item) uniquement
    const int idx = *reinterpret_cast<int*>(p + 0x04);  // index inventaire client
    if (idx <= 0) return false;
    *index = idx;
    const int c = *reinterpret_cast<int*>(p + 0x10);    // count (pile)
    *qty = c > 0 ? c : 1;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Lit un pointeur de fenêtre valide depuis le slot (vtable vérifiée). SEH-gardé.
uint8_t* ReadValidWnd(uintptr_t slot, uintptr_t expected_vtable) {
  __try {
    auto* wnd = *reinterpret_cast<uint8_t**>(slot);
    if (wnd == nullptr) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(wnd) != expected_vtable) return nullptr;
    return wnd;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

}  // namespace

// Opcode standard ZC_INVENTORY_START (0x0b08) : porte le nom de l'entrepôt ouvert
// (invType STORAGE=2). On l'OBSERVE (le handler natif tourne toujours) pour lire le
// nom -> titre du viewer. Forward : [len:2][invType:1][name:≤24] à partir de +2.
constexpr uint16_t kOpInventoryStart = 0x0b08;
constexpr int      kInvTypeStorage   = 2;  // e_inventory_type INVTYPE_STORAGE

StorageTweaks::StorageTweaks() {
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kStoragePrices);
  Bourgeon::Instance().RegisterObserveOpcode(kOpInventoryStart, 27);
}

// Prix de vente NPC du storage (ZC_BOURGEON_STORAGE_PRICES). data = payload après
// [op:2][len:2] : [count:2] puis count * [id:4][sell:4].
void StorageTweaks::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  // ZC_INVENTORY_START (observé) : data = [len:2][invType:1][name:≤24]. On garde le
  // nom seulement pour un entrepôt (invType STORAGE) -> titre du viewer.
  if (opcode == kOpInventoryStart) {
    if (len < 4 || data[2] != kInvTypeStorage) return;
    const char* name = reinterpret_cast<const char*>(data + 3);
    size_t i = 0;
    const size_t cap = sizeof(storage_name_) - 1;
    while (i < cap && i + 3 < len && name[i]) { storage_name_[i] = name[i]; ++i; }
    storage_name_[i] = '\0';
    return;
  }
  // ZC_BOURGEON_STORAGE_PRICES : [count:2] puis count * [id:4][sell:4][subtype:1][equip:4].
  if (opcode != bopcodes::kStoragePrices || len < 2) return;
  const int16_t count = *reinterpret_cast<const int16_t*>(data);
  // MERGE (pas de clear) : subtype/equip/prix sont statiques (itemdb) -> on accumule ;
  // ainsi une MAJ 1-item (ajout au storage) n'efface pas les métas de l'ouverture.
  size_t off = 2;
  for (int i = 0; i < count && off + 13 <= len; ++i) {
    const uint32_t id      = *reinterpret_cast<const uint32_t*>(data + off);
    const uint32_t sell    = *reinterpret_cast<const uint32_t*>(data + off + 4);
    const uint8_t  subtype = data[off + 8];
    const uint32_t equip   = *reinterpret_cast<const uint32_t*>(data + off + 9);
    prices_[id] = sell;
    meta_[id] = ItemMeta{subtype, equip};
    off += 13;
  }
}

// Remplit items_/item_count_ depuis le MODÈLE COMPLET (g_session+0x1718), pas la
// vue filtrée de la fenêtre : le viewer voit TOUS les items et fait son propre
// filtrage par onglet. POD-only sous SEH.
void StorageTweaks::Extract(uint8_t* wnd) {
  item_count_ = 0;
  __try {
    // 0x015fbad8 = g_session+0x1718 : sentinelle de la std::list storage complète.
    uint8_t* head = *reinterpret_cast<uint8_t**>(0x015fbad8);
    if (!head) return;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);  // 1er nœud
    int guard = 0;
    while (node && node != head && item_count_ < kMaxItems && guard < kMaxItems) {
      Item& it = items_[item_count_];
      uint8_t* info = node + kNodeInfo;  // node+8 = ItemSkillInfo
      // id : lu de la std::string à info+0x2c (là où le jeu le lit pour l'icône).
      const uint32_t idcap = *reinterpret_cast<uint32_t*>(info + kInfoIdCap);
      const char* ids = (idcap > 0xf)
                            ? *reinterpret_cast<char**>(info + kInfoIdStr)
                            : reinterpret_cast<const char*>(info + kInfoIdStr);
      it.id = ids ? static_cast<uint32_t>(atoi(ids)) : 0;
      it.identified = *reinterpret_cast<uint8_t*>(info + kInfoIdent);
      it.amount = *reinterpret_cast<int*>(node + kNodeAmt);
      it.index = *reinterpret_cast<int*>(info + 4);  // storage index (arg du retrait)
      it.type  = *reinterpret_cast<int*>(info);      // info+0 = type (onglets)
      // Nom COMPLET (refine/cartes/enchant) via BuildDisplayName ; repli sur le
      // nom de base. offVec est game-alloué -> libéré par game_free.
      it.name[0] = '\0';
      {
        char nbuf[128]; nbuf[0] = '\0';
        char* bufptr = nbuf; size_t ncap = sizeof(nbuf);
        int colorOut = 0; char* hlptr = nullptr;
        GVec off = {nullptr, nullptr, nullptr};
        reinterpret_cast<BuildName_t>(kBuildName)(wnd, info, &colorOut, &off,
                                                  &bufptr, &ncap, &hlptr, 0, 0);
        size_t k = 0;
        while (k < sizeof(it.name) - 1 && nbuf[k]) { it.name[k] = nbuf[k]; ++k; }
        it.name[k] = '\0';
        if (off.first) reinterpret_cast<GameFree_t>(kGameFree)(off.first);
      }
      if (it.name[0] == '\0') {
        size_t cap = sizeof(it.name);
        reinterpret_cast<GetBaseName_t>(kGetBaseName)(info, it.name, &cap, 0);
        it.name[sizeof(it.name) - 1] = '\0';
      }
      ++item_count_;
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
      ++guard;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // laisse item_count_ à ce qui a été extrait avant la faute
  }
}

void StorageTweaks::OnTick() {
  open_ = false;
  uint8_t* wnd = ReadValidWnd(kStorageSlot, kStorageVTable);
  if (wnd) {
    __try {
      used_ = *reinterpret_cast<int*>(wnd + kOffUsed);
      max_  = *reinterpret_cast<int*>(wnd + kOffMax);
      // Placement à la 1re ouverture : si on cache le natif, le viewer prend SA place
      // (nx, ny) ; sinon à droite (côte à côte pour valider la synchro).
      if (!was_open_) {
        const int nx = *reinterpret_cast<int*>(wnd + kOffPosX);
        const int ny = *reinterpret_cast<int*>(wnd + kOffPosY);
        const int nw = *reinterpret_cast<int*>(wnd + kOffWidth);
        spawn_x_ = hide_native_ ? nx : (nx + nw + 10);
        spawn_y_ = ny;
        need_pos_ = true;
      }
      // Remplacement complet : force le flag de visibilité natif (wnd+0x28) selon le
      // setting. 0 = caché (hors rendu + hors hit-test input), 1 = montré. On le force
      // chaque tick car le natif peut le remettre à 1 sur certains événements.
      *reinterpret_cast<int*>(wnd + kOffVisible) = hide_native_ ? 0 : 1;
      open_ = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      open_ = false;
    }
    if (open_) Extract(wnd);
  }
  was_open_ = open_;
}

// Mémorise si le clic a démarré sur la fenêtre cart (pour router le drop, cf. header).
void StorageTweaks::OnMouseDown(int mx, int my) {
  mousedown_over_cart_ =
      MouseOverCart(static_cast<float>(mx), static_cast<float>(my));
}

// Cache la fenêtre native DÈS sa création (avant le 1er rendu) -> zéro flicker.
// Vérifie la vtable storage par sûreté (le hook passe l'id 0x21, mais on confirme).
void StorageTweaks::HideNativeAtCreation(void* win) {
  if (!win || !hide_native_) return;
  __try {
    if (*reinterpret_cast<uintptr_t*>(win) != kStorageVTable) return;
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Appelé par le hook WndProc au WM_LBUTTONUP (pré-input). Consomme un drop de drag
// natif (inventaire OU cart) au-dessus du viewer -> pose un déplacement en attente +
// annule le drag. Le paquet réel part de OnRenderUI (sûr, + prompt pour les piles).
// Source = mousedown_over_cart_ (le payload n'expose pas la source de façon fiable).
bool StorageTweaks::HandleNativeDrop(int mx, int my) {
  if (!open_ || !show_panel_ || !win_valid_) return false;
  if (ImGui::GetDragDropPayload() != nullptr) return false;  // pas pendant un drag ImGui
  void* obj = DragObj();
  if (!obj) return false;  // pas de drag natif -> silencieux (pas de spam à chaque clic)
  const bool over = !(mx < win_x_ || my < win_y_ ||
                      mx >= win_x_ + win_w_ || my >= win_y_ + win_h_);
  int index = 0, qty = 0;
  const bool read = ReadDraggedInvItem(obj, &index, &qty);
  if (!over || !read) return false;
  // Déplacement en attente (traité au prochain rendu) + annulation du drag natif.
  // Clic parti du cart -> cart->storage (0x0129) ; sinon -> dépôt inventaire (0x08ac).
  pend_id_ = index;  // != 0 => action en attente
  pend_index_ = index;
  pend_max_ = qty > 0 ? qty : 1;
  pend_action_ = mousedown_over_cart_ ? kPendCartToSto : kPendDeposit;
  pend_open_prompt_ = (pend_max_ > 1);  // pile -> prompt ; 1 seul -> direct
  CancelNativeDrag(obj);
  return true;
}

void StorageTweaks::OnRenderUI() {
  if (!open_ || !show_panel_) return;

  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(spawn_x_),
                                   static_cast<float>(spawn_y_)),
                            ImGuiCond_Appearing);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);

  // Titre = nom de l'entrepôt envoyé par le serveur (ZC_INVENTORY_START), ex.
  // "Storage" / "Guild Storage" / nom premium. Repli "Entrepot" si pas encore reçu.
  // L'id ImGui (###) reste stable -> position/taille persistent malgré le nom variable.
  char title[64];
  std::snprintf(title, sizeof(title), "%s###bourgeon_storage",
                storage_name_[0] ? storage_name_ : "Entrepot");
  const bool begun = ImGui::Begin(title, &show_panel_, ImGuiWindowFlags_NoCollapse);
  // Le X du viewer a été cliqué ce frame (show_panel_ était vrai à l'entrée, cf. le
  // early-return en tête) -> on FERME l'entrepôt côté serveur (CZ_CloseKafra). Le
  // serveur ferme la session -> native + viewer se ferment (open_ passe à false au
  // prochain OnTick). On remet show_panel_ à true pour que le viewer réapparaisse à
  // la prochaine ouverture (le masquage effectif vient de open_, pas de show_panel_).
  if (!show_panel_) {
    SendCloseStorage();
    show_panel_ = true;
  }
  if (!begun) {
    ImGui::End();
    return;
  }

  // Rect écran du viewer (pour tester le drop d'un drag natif dessus).
  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  win_x_ = wp.x; win_y_ = wp.y; win_w_ = ws.x; win_h_ = ws.y;
  win_valid_ = true;

  // Action en attente (dépôt/retrait via HandleNativeDrop ou drag-end) : 1 seul =
  // direct ; pile = prompt quantité. do_move applique le sens choisi.
  auto do_move = [this](int amount) {
    switch (pend_action_) {
      case kPendWithdraw:   WithdrawItem(pend_index_, amount); break;
      case kPendStoToCart:  SendStorageToCart(pend_index_, amount); break;
      case kPendCartToSto:  SendCartToStorage(pend_index_, amount); break;
      default:              SendDeposit(pend_index_, amount); break;  // kPendDeposit
    }
  };
  if (pend_id_ != 0) {
    if (pend_open_prompt_) {
      ImGui::OpenPopup("Quantite");
      pend_open_prompt_ = false;
    } else if (pend_max_ <= 1) {
      do_move(1);  // 1 seul item -> direct
      pend_id_ = 0;
    }
  }
  // Pas de voile (dim) derrière la modale, et prompt au curseur.
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));
  ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
  const bool popen =
      ImGui::BeginPopupModal("Quantite", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PopStyleColor();
  if (popen) {
    const char* verb = pend_action_ == kPendWithdraw ? "Retirer"
                     : pend_action_ == kPendStoToCart ? "Vers le chariot"
                     : pend_action_ == kPendCartToSto ? "Depuis le chariot"
                     : "Deposer";
    ImGui::Text("%s combien ? (max %d)", verb, pend_max_);
    static int dq = 1;
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("##dq", &dq);
    if (dq < 1) dq = 1;
    if (dq > pend_max_) dq = pend_max_;
    if (ImGui::Button("OK")) {
      do_move(dq);
      pend_id_ = 0; dq = 1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Tout")) {
      do_move(pend_max_);
      pend_id_ = 0; dq = 1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Annuler")) { pend_id_ = 0; dq = 1; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
  }

  // Onglets de catégorie (filtre par type d'item). Changer d'onglet remet la
  // sous-catégorie à Tout (les clés diffèrent d'un onglet à l'autre).
  if (ImGui::BeginTabBar("storage_cats", ImGuiTabBarFlags_FittingPolicyScroll)) {
    for (int c = 0; c < kNumStgCats; ++c) {
      if (ImGui::BeginTabItem(kStgCats[c].label)) {
        if (cur_tab_ != c) cur_sub_ = -1;
        cur_tab_ = c;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }

  // meta serveur d'un item (subtype/equip) pour les sous-catégories.
  auto submeta = [this](uint32_t id) -> ItemMeta {
    auto it = meta_.find(id);
    return it != meta_.end() ? it->second : ItemMeta{};
  };

  // Barre de recherche (filtre par nom), persistante entre frames.
  static ImGuiTextFilter filter;
  ImGui::SetNextItemWidth(-1.0f);
  filter.Draw("##storage_filter");

  // Vue onglet+nom (avant sous-catégorie) : sert à connaître les sous-cats présentes.
  const int sub_dim = kStgCats[cur_tab_].sub;
  std::vector<int> tabview;
  tabview.reserve(item_count_);
  for (int i = 0; i < item_count_; ++i)
    if (ItemInTab(cur_tab_, items_[i].type, submeta(items_[i].id).equip) &&
        filter.PassFilter(items_[i].name))
      tabview.push_back(i);

  // Combo de sous-catégorie (armes par type, armures/cartes par slot, munitions par
  // type) : ne liste que les sous-cats réellement présentes, triées par clé.
  std::vector<SubCat> subs;
  if (sub_dim != kSubNone) {
    for (int i : tabview) {
      const ItemMeta m = submeta(items_[i].id);
      const SubCat sc = SubCatOf(sub_dim, m.subtype, m.equip);
      if (!sc.label) continue;
      bool seen = false;
      for (const auto& s : subs) if (s.key == sc.key) { seen = true; break; }
      if (!seen) {
        size_t p = subs.size();
        while (p > 0 && subs[p - 1].key > sc.key) --p;
        subs.insert(subs.begin() + p, sc);
      }
    }
    // Label courant (repli Tout si la clé sélectionnée n'existe plus).
    const char* cur_label = "Tout";
    if (cur_sub_ != -1) {
      bool found = false;
      for (const auto& s : subs) if (s.key == cur_sub_) { cur_label = s.label; found = true; break; }
      if (!found) cur_sub_ = -1;
    }
    ImGui::TextUnformatted("Sous-type");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##storage_subcat", cur_label)) {
      if (ImGui::Selectable("Tout", cur_sub_ == -1)) cur_sub_ = -1;
      for (const auto& s : subs)
        if (ImGui::Selectable(s.label, cur_sub_ == s.key)) cur_sub_ = s.key;
      ImGui::EndCombo();
    }
  }

  // Vue finale = tabview filtrée par la sous-catégorie choisie.
  std::vector<int> view;
  view.reserve(tabview.size());
  for (int i : tabview) {
    if (sub_dim == kSubNone || cur_sub_ == -1) { view.push_back(i); continue; }
    const ItemMeta m = submeta(items_[i].id);
    if (SubCatOf(sub_dim, m.subtype, m.equip).key == cur_sub_) view.push_back(i);
  }

  // Valeur = prix de vente NPC (reçu du serveur) * quantité.
  auto price = [this](uint32_t id) -> long long {
    auto it = prices_.find(id);
    return it != prices_.end() ? static_cast<long long>(it->second) : 0;
  };
  long long total_val = 0;
  for (int i : view) total_val += price(items_[i].id) * items_[i].amount;

  ImGui::Text("%d / %d items  (%d affiches)  |  Valeur estimée: %lldz", used_, max_,
              static_cast<int>(view.size()), total_val);
  ImGui::SameLine();
  // ── Settings (début) : colonnes optionnelles Idx / ID ──
  ImGui::Checkbox("Idx", &show_index_col_);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Afficher l'index storage (slot) — un item recemment ajoute a un index eleve");
  ImGui::SameLine();
  ImGui::Checkbox("ID", &show_id_col_);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Afficher une colonne avec l'id d'item");
  ImGui::SameLine();
  ImGui::Checkbox("Cacher natif", &hide_native_);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Masquer la fenetre de storage native (remplacement complet par ce viewer)");
  ImGui::Separator();

  // Ordre courant des colonnes : [Index], Item, [ID], Qté, Prix revente. Les index
  // de tri sont calculés dynamiquement (les colonnes optionnelles décalent tout).
  const int ncols = 3 + (show_index_col_ ? 1 : 0) + (show_id_col_ ? 1 : 0);
  int colc = 0;
  const int kColIdx = show_index_col_ ? colc++ : -1;  // Index
  ++colc;                                             // Item -> tri par nom (branche else)
  const int kColId  = show_id_col_ ? colc++ : -1;      // ID
  const int kColQte = colc++;                          // Qté
  const int kColVal = colc++;                          // Prix revente
  const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_Sortable |
                             ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("storage_items", ncols, tf)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    if (show_index_col_)
      ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed |
                                         ImGuiTableColumnFlags_PreferSortDescending,
                              54.0f);
    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch |
                                        ImGuiTableColumnFlags_DefaultSort);
    if (show_id_col_)
      ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Qté", ImGuiTableColumnFlags_WidthFixed |
                                       ImGuiTableColumnFlags_PreferSortDescending,
                            56.0f);
    ImGui::TableSetupColumn("Prix revente", ImGuiTableColumnFlags_WidthFixed |
                                          ImGuiTableColumnFlags_PreferSortDescending,
                            84.0f);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) { // tri demandé
      if (sort->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& sp = sort->Specs[0];
        const bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
        std::sort(view.begin(), view.end(), [&](int a, int b) {
          int c;
          if (sp.ColumnIndex == kColQte) {
            c = (items_[a].amount < items_[b].amount) ? -1
                : (items_[a].amount > items_[b].amount) ? 1 : 0;
          } else if (sp.ColumnIndex == kColVal) {
            const long long va = price(items_[a].id) * items_[a].amount;
            const long long vb = price(items_[b].id) * items_[b].amount;
            c = (va < vb) ? -1 : (va > vb) ? 1 : 0;
          } else if (sp.ColumnIndex == kColIdx) {
            c = (items_[a].index < items_[b].index) ? -1
                : (items_[a].index > items_[b].index) ? 1 : 0;
          } else if (sp.ColumnIndex == kColId) {
            c = (items_[a].id < items_[b].id) ? -1
                : (items_[a].id > items_[b].id) ? 1 : 0;
          } else {
            c = _stricmp(items_[a].name, items_[b].name);
          }
          return asc ? c < 0 : c > 0;
        });
      }
    }

    constexpr float kIcon = 22.0f;  // hauteur d'affichage de l'icône
    for (int idx : view) {
      ImGui::TableNextRow();
      // ── Colonne Idx (optionnelle) : index storage (slot) ──
      if (show_index_col_) {
        ImGui::TableNextColumn();
        ImGui::Text("%d", items_[idx].index);
      }
      // ── Colonne Item : icône + nom cliquable (clic-droit = description) ──
      ImGui::TableNextColumn();
      const IconTex ic = ResolveIcon(items_[idx].id, items_[idx].identified);
      if (ic.tex && ic.w > 0 && ic.h > 0) {
        const float w = kIcon * static_cast<float>(ic.w) / ic.h;
        ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(w, kIcon));
      } else {
        ImGui::Dummy(ImVec2(kIcon, kIcon));  // garde l'alignement si pas d'icône
      }
      ImGui::SameLine();
      ImGui::PushID(idx);
      // Clic GAUCHE : Shift -> tout retirer (hotkey native) ; 1 seul item ->
      // retrait direct ; sinon (pile) -> ouvre le menu de choix de quantité.
      if (ImGui::Selectable(items_[idx].name[0] ? items_[idx].name : "(?)")) {
        if (ImGui::GetIO().KeyShift || items_[idx].amount <= 1)
          WithdrawItem(items_[idx].index, items_[idx].amount);
        else
          ImGui::OpenPopup("ctx");
      }
      // Source de DRAG : glisser un item du viewer -> relâché sur l'inventaire
      // natif = retrait (le fantôme suit le curseur). Le drop est traité en fin
      // de OnRenderUI (MouseOverInventory).
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        drag_active_ = true;
        drag_index_ = items_[idx].index;
        drag_amount_ = items_[idx].amount;
        ImGui::SetDragDropPayload("STG_ITEM", &idx, sizeof(idx));
        if (ic.tex && ic.w > 0 && ic.h > 0) {
          const float w = kIcon * static_cast<float>(ic.w) / ic.h;
          ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(w, kIcon));
          ImGui::SameLine();
        }
        ImGui::TextUnformatted(items_[idx].name[0] ? items_[idx].name : "(?)");
        ImGui::EndDragDropSource();
      }
      // Clic DROIT : toujours le menu contextuel.
      if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("ctx");
      if (ImGui::BeginPopup("ctx")) {
        ImGui::TextDisabled("%s", items_[idx].name[0] ? items_[idx].name : "(?)");
        ImGui::Separator();
        if (ImGui::MenuItem("Description")) {
          POINT pt;
          if (GetCursorPos(&pt)) OpenItemDesc(items_[idx].id, pt.x, pt.y);
        }
        ImGui::Separator();
        const int amt = items_[idx].amount;
        const int index = items_[idx].index;
        if (ImGui::MenuItem("Retirer 1")) WithdrawItem(index, 1);
        if (amt > 1) {
          char lbl[40];
          std::snprintf(lbl, sizeof(lbl), "Retirer tout (%d)", amt);
          if (ImGui::MenuItem(lbl)) WithdrawItem(index, amt);
          static int qty = 1;
          ImGui::SetNextItemWidth(90);
          ImGui::InputInt("##qty", &qty);
          ImGui::SameLine();
          if (ImGui::SmallButton("Retirer")) {
            int q = qty < 1 ? 1 : (qty > amt ? amt : qty);
            WithdrawItem(index, q);
            ImGui::CloseCurrentPopup();
          }
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
      // ── Colonne ID (optionnelle) ──
      if (show_id_col_) {
        ImGui::TableNextColumn();
        ImGui::Text("%u", items_[idx].id);
      }
      // ── Colonne Qte ──
      ImGui::TableNextColumn();
      ImGui::Text("%d", items_[idx].amount);
      // ── Colonne Valeur (prix de vente NPC * quantité) ──
      ImGui::TableNextColumn();
      const long long val = price(items_[idx].id) * items_[idx].amount;
      if (val > 0) ImGui::Text("%lld", val);
      else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
  }

  // DRAG d'un item storage : suit le curseur ; au relâché, la CIBLE décide du sens :
  //   - lâché sur l'INVENTAIRE -> retrait (storage -> inventaire)
  //   - lâché sur le CART       -> storage -> cart
  // 1 seul = direct ; pile = prompt quantité (comme le dépôt).
  if (drag_active_) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType("STG_ITEM")) {
      const ImVec2 m = ImGui::GetMousePos();
      drag_mx_ = m.x; drag_my_ = m.y;
    } else {  // drag terminé ce frame
      int action = -1;
      if (drag_index_ > 0) {
        if (MouseOverInventory(drag_mx_, drag_my_))   action = kPendWithdraw;
        else if (MouseOverCart(drag_mx_, drag_my_))   action = kPendStoToCart;
      }
      if (action != -1) {
        pend_id_ = drag_index_;
        pend_index_ = drag_index_;
        pend_max_ = drag_amount_ > 0 ? drag_amount_ : 1;
        pend_action_ = action;
        pend_open_prompt_ = (pend_max_ > 1);
      }
      drag_active_ = false;
    }
  }

  ImGui::End();
}
