#include "plugins/inventory_viewer.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket
#include "plugins/moonlight_ui.h"  // API alootid (IsAlootId/AddAlootId/RemoveAlootId)
#include "plugins/storage_tweaks.h"  // PointOverViewer (dépôt par glisser vers le viewer storage)
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "imgui.h"
#include "ui/ro_imgui.h"     // skin RO (BeginRoWindow / RoButton / RoCheckbox / DrawBar)

// ── Constantes RE (client 20250716, base 0x400000) ─────────────────────────────
// Voir project_inventory_viewer_wip + project_inventory_window + workflow slim RE.
namespace {

// Fenêtre inventaire native (id 8) : global + vtable. Non-nul <=> ouverte.
constexpr uintptr_t kInvWndGlobal = 0x0131f6bc;
constexpr uintptr_t kInvVTable    = 0x0103d460;
constexpr int kOffWidth   = 0x14;
constexpr int kOffHeight  = 0x18;
constexpr int kOffPosX    = 0x1c;
constexpr int kOffPosY    = 0x20;
constexpr int kOffVisible = 0x28;  // flag visibilité (0=caché, hors rendu+hit-test)

// Modèle SESSION de l'inventaire (indépendant de la fenêtre => marche natif caché).
// RE : FUN_00d5ce30(g_session)=*(g_session+0x16f4)=count ; FUN_00d5acb0 parcourt la
// std::list @ g_session+0x16f0. g_session = 0x015fa3c0.
constexpr uintptr_t kInvListHead = 0x015fbab0;  // sentinelle std::list (head)
constexpr uintptr_t kInvCount    = 0x015fbab4;  // nb d'items
constexpr uintptr_t kSessionObj  = 0x015fa3c0;  // g_session (base) — pour les compteurs natifs
constexpr uintptr_t kCntEquipped = 0x00d9aa70;  // __fastcall(session) : nb items ÉQUIPÉS distincts (10 slots @+0x17d4)
constexpr uintptr_t kCntCostume  = 0x00d9a960;  // __fastcall(session) : nb items COSTUME distincts (10 slots @+0x2b34)
constexpr int kNodeNext = 0x00;  // nœud : next
constexpr int kNodeInfo = 0x08;  // nœud : value = ItemSkillInfo
constexpr int kNodeAmt  = 0x18;  // nœud : quantité (= info+0x10)
constexpr int kInfoFav  = 0x74;  // favori dans l'ItemSkillInfo (RE FUN_0095af80 : local_98 = info+0x74)
// Champs DANS l'ItemSkillInfo (= node+0x08) :
constexpr int kInfoType   = 0x00;  // type d'item (onglets)
constexpr int kInfoIndex  = 0x04;  // index inventaire CLIENT (arg use/equip/drop/fav)
constexpr int kInfoLoc    = 0x08;  // masque d'emplacement d'équip (arg2 du msg 0x13/0x57 ; RE double-clic natif)
constexpr int kInfoIdStr  = 0x2c;  // std::string id (le jeu fait atoi dessus)
constexpr int kInfoIdCap  = 0x40;  // capacité SSO de la std::string id (+0x2c+0x14)
constexpr int kInfoIdent  = 0x5c;  // byte : item identifié ?
constexpr int kInfoRefine = 0x60;  // niveau de refine (int) ; RE character_sheet keRefine

// Poids / zeny / compteur.
constexpr uintptr_t kWeightCur     = 0x015fbaa0;
constexpr uintptr_t kWeightMax     = 0x015fba9c;
constexpr uintptr_t kZeny          = 0x015fba90;
constexpr uintptr_t kOverweightPct = 0x01602324;
constexpr uintptr_t kInvExpansion  = 0x01602354;  // extension serveur (capacité = +200)
constexpr int kInvBase = 200;  // moonlight INVENTORY_BASE_SIZE ; max = expansion + 200

// Nom de base + nom complet (refine/cartes/enchant), comme le storage.
constexpr uintptr_t kGetBaseName = 0x006a2b50;  // __thiscall(info, out, &cap, flag)
constexpr uintptr_t kBuildName   = 0x008a0570;  // BuildDisplayName
constexpr uintptr_t kGameFree    = 0x00dbbc7f;
using GetBaseName_t = size_t(__thiscall*)(void*, char*, size_t*, char);
struct GVec { int* first; int* last; int* end; };  // std::vector MSVC (jeu)
using BuildName_t = int(__thiscall*)(void*, void*, int*, GVec*, char**, size_t*,
                                     char**, char, char);
using GameFree_t  = void(__cdecl*)(void*);

// Construit le nom d'affichage d'un item sous SEH ISOLÉ. Un item dont BuildDisplayName
// plante avortait TOUT l'Extract (d'où des items manquants vs le natif) ; ici le
// plantage est confiné -> l'énumération continue (repli GetBaseName / nom vide).
inline void SafeBuildName(void* wnd, void* info, char* out, size_t outsz) {
  out[0] = '\0';
  __try {
    char nbuf[128]; nbuf[0] = '\0';
    char* bufptr = nbuf; size_t ncap = sizeof(nbuf);
    int colorOut = 0; char* hlptr = nullptr;
    GVec off = {nullptr, nullptr, nullptr};
    reinterpret_cast<BuildName_t>(kBuildName)(wnd, info, &colorOut, &off,
                                              &bufptr, &ncap, &hlptr, 0, 0);
    size_t k = 0;
    while (k + 1 < outsz && nbuf[k]) { out[k] = nbuf[k]; ++k; }
    out[k] = '\0';
    if (off.first) reinterpret_cast<GameFree_t>(kGameFree)(off.first);
    if (out[0] == '\0') {
      size_t cap = outsz;
      reinterpret_cast<GetBaseName_t>(kGetBaseName)(info, out, &cap, 0);
      out[outsz - 1] = '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// Icône d'item : BuildItemIconGrfPath(id_str, out[128], identified) __stdcall (RET 0xc).
constexpr uintptr_t kBuildIconPath = 0x00d5a720;
constexpr uintptr_t kTexMgr  = 0x00a90350;
constexpr uintptr_t kMakeKey = 0x00a9f030;
constexpr uintptr_t kLoadTex = 0x00a8d4a0;
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;
using BuildIconPath_t = void(__stdcall*)(const char*, char*, int);
using TexMgr_t  = void*(__cdecl*)();
using MakeKey_t = void*(__cdecl*)(const char*);
using LoadTex_t = void*(__fastcall*)(void*, void*, void*);
using FmtComma_t = char*(__cdecl*)(int, char*, int);  // FUN_00a948d0 séparateur milliers
constexpr uintptr_t kFmtComma = 0x00a948d0;

// Fenêtre de description (id 0xc) : MakeWindow + OnMsg(0x18, &ItemSkillInfo).
constexpr uintptr_t kUIWindowMgr = 0x0131f4e8;
constexpr uintptr_t kMakeWindow  = 0x00a39340;   // __fastcall(mgr, edx, id)
constexpr uintptr_t kFindWindow  = 0x00a47b90;   // __thiscall(mgr, id) : ptr fenêtre par id (0 si absente)
constexpr uintptr_t kToggleWndById = 0x00812e60;  // FUN_00812e60(id) __stdcall (RET 0x4, vérifié désasm) : bascule fenêtre (ferme si ouverte via SaveWindowRect, sinon ouvre) = chemin de l'icône de menu
constexpr int kWinItemDesc = 0xc;
constexpr int kWinInventory = 8;
constexpr int kMsgSetItem  = 0x18;
constexpr int kVfOnMsg     = 0x94;
constexpr int kVfSetPos    = 0x10;
using MakeWindow_t   = void*(__fastcall*)(void*, void*, void*);
using FindWindow_t   = void*(__thiscall*)(void*, int);
using ToggleById_t   = int (__stdcall*)(int);  // FUN_00812e60(id) : ferme la fenêtre si ouverte
using OnMsg_t        = int (__fastcall*)(void*, void*, int, int, int, int, int, int);
using SetPos_t       = void(__fastcall*)(void*, void*, int, int);

// Dispatcher (CMode) : FUN_00a75340(0x1213338) renvoie l'objet mode actif (ou 0 hors
// jeu — c'est *(0x0121333c) gardé). Son vtbl+0x18 = CMode::SendMsg (le gros switch).
// Commandes (confirmées via le double-clic natif 0x00949fc0 et le clic-droit 0x0094f380) :
//   use conso 0x1b / équiper 0x13 / carte 0x7b / munition-costume-ombre 0x57 ;
//   transfert vers chariot 0x4c / vers storage Kafra 0x37 (0x33 = guilde, fenêtre 0x271b).
constexpr uintptr_t kGetMode = 0x00a75340;
constexpr uintptr_t kModeArg = 0x1213338;
constexpr int kVfDispCmd = 0x18;
constexpr int kCmdUse       = 0x1b;
constexpr int kCmdEquip     = 0x13;
constexpr int kCmdCard      = 0x7b;
constexpr int kCmdAmmo      = 0x57;
constexpr int kCmdEquipAlt  = 0x12e;  // Ctrl+double-clic : équipe en MAIN GAUCHE (dual-wield).
constexpr uintptr_t kLeftHandEquipOpt = 0x01602278;  // DAT_01602278 : option client "équip main gauche" active ?
constexpr int kCmdToCart    = 0x4c;
constexpr int kCmdCartToBody = 0x4d;  // chariot -> inventaire (retrait) ; RE UIInventoryWnd_OnMsg case 0x26 (contexte cart).
constexpr int kCmdToStorage = 0x37;  // storage KAFRA (g_StorageWnd_ptr 0x0131f770 ouvert).
                                     // 0x33 = guilde (fenêtre 0x271b), 0x4c = chariot.
                                     // RE UIInventoryWnd_OnRButtonDown : le natif choisit selon
                                     // la fenêtre ouverte ; notre StorageOpen() lit 0x0131f770.
using GetMode_t = void*(__fastcall*)(int);
using DispCmd_t = void(__thiscall*)(void*, int, int, int, int, int);

// Opcodes client->serveur (bloc PACKETVER 20250716 ACTIF ; RE workflow confirmée).
//   Drop     : CZ_ITEM_THROW 0x0438  [op:2][index:2][amount:2] (amount 16-bit)
//   Favorite : CZ_INVENTORY_TAB 0x0907 [op:2][index:2][fav:1]
//     fav=0 => AJOUTE aux favoris ; fav=1 => RETIRE (seulement si déjà favori).
//     Donc bascule : envoyer (déjà favori ? 1 : 0). index = index CLIENT (info+4).
constexpr uint16_t kOpDrop     = 0x0363;  // CZ_ITEM_THROW. ATTENTION SHUFFLE : 0x0438 reecrit -> UseSkillToId (len 10) = disconnect ; drop shuffle = 0x0363 (clif_shuffle.hpp bloc > 20180307). Format [op:2][index:2][amount:2] inchange.
constexpr uint16_t kOpFavorite = 0x0907;
constexpr uint16_t kOpUnequip  = 0x00AB;  // CZ_REQ_TAKEOFF_EQUIP {op, invIndex} : dés-équiper.

// Fenêtres cible d'un transfert (chariot / storage), pour le drag-out + menu.
constexpr uintptr_t kCartWndGlobal = 0x0131f6a0;
constexpr uintptr_t kCartVTable    = 0x0103d538;
constexpr uintptr_t kStorageSlot   = 0x0131f770;
constexpr uintptr_t kStorageVTable = 0x0103ca40;

// ── Helpers vtable ──────────────────────────────────────────────────────────
template <typename Fn>
inline Fn Vf(void* self, int off) {
  return reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t**>(self))[off / 4]);
}

// Objet mode courant (dispatcher), ou nullptr hors d'un mode jouable. SEH-gardé.
void* Dispatcher() {
  __try {
    return reinterpret_cast<GetMode_t>(kGetMode)(static_cast<int>(kModeArg));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Items PORTÉS (équipés + costume) : la liste session 0x015fbab0 les EXCLUT, mais le
// compteur du footer NATIF les compte dans le total (Inventory_GetCount + FUN_00d9aa70
// équipés + FUN_00d9a960 costume). On appelle les deux (items distincts, 10 slots chacun)
// pour que notre « N/max » matche le natif. SEH (POD).
using WornCountFn_t = int(__fastcall*)(int);
int WornItemCount() {
  __try {
    return reinterpret_cast<WornCountFn_t>(kCntEquipped)(static_cast<int>(kSessionObj)) +
           reinterpret_cast<WornCountFn_t>(kCntCostume)(static_cast<int>(kSessionObj));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Envoie une commande UI native (use/equip/transfer...) via le dispatcher.
void SendCmd(int cmd, int index, int arg2) {
  __try {
    void* d = Dispatcher();
    if (d) Vf<DispCmd_t>(d, kVfDispCmd)(d, cmd, index, arg2, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Utiliser ou équiper l'item selon son type (miroir du double-clic natif 0x00949fc0).
// arg2 = masque d'emplacement (info+8, `loc`) pour ÉQUIP/MUNITION — le natif le passe ;
// arg2=0 => le serveur reçoit position 0 => l'équip échoue (c'était LE bug). arg2=0 pour
// conso/carte (le natif le force à 0). leftHand (Ctrl+double-clic) = cmd 0x12e (main
// gauche dual-wield) si l'option client DAT_01602278 est active.
void UseOrEquip(int index, int type, uint32_t loc, bool leftHand) {
  int cmd = 0, arg2 = 0;
  switch (type) {
    case 0: case 1: case 2: case 0x12: cmd = kCmdUse; break;  // consommables (arg2=0)
    case 4: case 5: case 8: case 9:
    case 0xb: case 0xc: case 0xd: case 0xe: case 0xf:
      cmd = kCmdEquip; arg2 = static_cast<int>(loc); break;  // équipement
    case 6: cmd = kCmdCard; break;                           // carte (arg2=0)
    case 0xa: case 0x10: case 0x11: case 0x13:
      cmd = kCmdAmmo; arg2 = static_cast<int>(loc); break;   // munition/costume/ombre
    default: return;  // etc/divers : le double-clic ne fait rien
  }
  // Ctrl = main gauche (équip/munition), si l'option client est active (comme le natif).
  if (leftHand && (cmd == kCmdEquip || cmd == kCmdAmmo)) {
    bool opt = false;
    __try { opt = *reinterpret_cast<uint8_t*>(kLeftHandEquipOpt) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (opt) cmd = kCmdEquipAlt;
  }
  SendCmd(cmd, index, arg2);
}

// Type équipable (équipement OU munition/costume/ombre) : pour le drop sur la fenêtre
// Équipement — un consommable lâché dessus ne doit PAS être consommé.
bool IsEquippable(int type) {
  switch (type) {
    case 4: case 5: case 8: case 9:
    case 0xb: case 0xc: case 0xd: case 0xe: case 0xf:
    case 0xa: case 0x10: case 0x11: case 0x13:
      return true;
    default: return false;
  }
}

void SendDrop(int index, int amount) {
  if (amount <= 0) return;
  uint8_t pkt[6];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpDrop;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(index);
  *reinterpret_cast<uint16_t*>(pkt + 4) = static_cast<uint16_t>(amount);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// ── Drag NATIF entrant (fenêtre Équipement -> viewer = dés-équiper) ──────────────
// Le drag natif est porté par l'objet mode (Dispatcher(), charge à +0x308) : payload
// +0x80=0 (FullPayload item), +0x04 = index inventaire client. Repris de StorageTweaks.
constexpr int kDragPayloadOff = 0x308;
constexpr int kDragPL_type = 0x80, kDragPL_index = 0x04, kDragPL_id = 0x04, kDragPL_cat = 0x00;
constexpr int kDragPL_count = 0x10;  // quantité (pile) du drag natif

bool ReadDraggedItem(void* obj, int* index, int* qty) {  // false si pas un item / invalide
  __try {
    uint8_t* p = reinterpret_cast<uint8_t*>(obj) + kDragPayloadOff;
    if (p[kDragPL_type] != 0) return false;  // FullPayload (item) uniquement
    const int idx = *reinterpret_cast<int*>(p + kDragPL_index);
    if (idx <= 0) return false;
    *index = idx;
    const int c = *reinterpret_cast<int*>(p + kDragPL_count);
    *qty = c > 0 ? c : 1;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void CancelNativeDrag(void* obj) {  // vide la charge -> le jeu ne drop rien au relâché
  __try {
    uint8_t* p = reinterpret_cast<uint8_t*>(obj) + kDragPayloadOff;
    *reinterpret_cast<int*>(p + kDragPL_cat) = 0;
    *reinterpret_cast<int*>(p + kDragPL_id)  = 0;
    p[kDragPL_type] = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// CZ_REQ_TAKEOFF_EQUIP 0x00AB {op, invIndex} : dés-équipe l'item à cet index inventaire.
void SendUnequip(int index) {
  if (index <= 0) return;
  uint8_t pkt[4];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpUnequip;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(index);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// Lit l'id (nameid) de l'item d'un drag natif : resname = std::string @payload+0x3c (SSO,
// heap si cap>0xf), atoi. Sert à redessiner l'icône du drag AU-DESSUS du viewer (le jeu la
// rend DERRIÈRE l'overlay ImGui). 0 si ce n'est pas un item.
uint32_t ReadDragItemId(void* obj) {
  __try {
    uint8_t* p = reinterpret_cast<uint8_t*>(obj) + kDragPayloadOff;
    if (p[kDragPL_type] != 0) return 0;  // FullPayload (item) uniquement
    const uint32_t cap = *reinterpret_cast<uint32_t*>(p + 0x3c + 0x14);  // capacité SSO
    const char* s = (cap > 0xf) ? *reinterpret_cast<char**>(p + 0x3c)
                                : reinterpret_cast<const char*>(p + 0x3c);
    return (s && s[0]) ? static_cast<uint32_t>(atoi(s)) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Bascule le favori d'un item : envoie son état ACTUEL (le serveur toggle).
void SendFavoriteToggle(int index, bool currently_fav) {
  uint8_t pkt[5];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpFavorite;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(index);
  pkt[4] = currently_fav ? 1 : 0;  // 1=retire (si déjà favori) / 0=ajoute
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// ── Verrous footer (RE workflow, 100% CLIENT-SIDE, aucun paquet) ────────────────
// Drop lock (cmd natif 0xd5, byte @0x015fffa0) : testé par le drag/drop natif AU SOL ;
// on le teste AUSSI dans notre "Jeter". Deal lock (cmd 0x1fb, byte @0x01600553) :
// FUN_00cd0f00 (liste de vente NPC) exclut les favoris quand ON.
constexpr uintptr_t kDropLockGlobal = 0x015fffa0;
constexpr uintptr_t kDealLockGlobal = 0x01600553;
inline bool ReadLock(uintptr_t g) {
  __try { return *reinterpret_cast<uint8_t*>(g) != 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline void ToggleLock(uintptr_t g) {
  __try { *reinterpret_cast<uint8_t*>(g) ^= 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Ferme l'inventaire natif côté client via le MÊME chemin que l'icône de menu :
// FUN_00812e60(8) (__cdecl) bascule la fenêtre — ouverte -> SaveWindowRect la ferme
// (0x0131f6bc = 0, viewer fermé au prochain tick).
// CRASH RÉGLÉ : l'ancien ToggleWindow 0x00a4bf30 (a) ne gère PAS l'id 8 et surtout
// (b) était appelé avec une mauvaise convention (la vraie fait `ret 8` = 2 args pile,
// mon typedef 1 seul = `ret 4`) -> désync de pile de 4 o à CHAQUE fermeture ->
// registre corrompu -> AV 0xC0000005 DIFFÉRÉE dans le rendu/hook (pas au site de
// l'appel). FUN_00812e60 est __stdcall (RET 0x4, désasm vérifié) : callee nettoie.
void CloseInventory() {
  __try {
    reinterpret_cast<ToggleById_t>(kToggleWndById)(kWinInventory);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Icônes ImGui (recette storage/item_desc) ────────────────────────────────
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

bool BuildIconPathSafe(uint32_t id, char* out, int identified) {
  char idstr[16];
  std::snprintf(idstr, sizeof(idstr), "%u", id);
  out[0] = '\0';
  __try {
    reinterpret_cast<BuildIconPath_t>(kBuildIconPath)(idstr, out, identified);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

IconTex LoadItemIcon(uint32_t id, int identified) {
  char path[160];
  if (!BuildIconPathSafe(id, path, identified)) return {};
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

IconTex ResolveIcon(uint32_t id, int identified) {
  // Les textures cachées meurent avec le device (D3DPOOL_DEFAULT) : on jette le
  // cache si le device a été reset/recréé, sinon AddImage() plante dans ddraw.
  static unsigned s_epoch = 0;
  const unsigned e = Overlay_DeviceEpoch();
  if (e != s_epoch) { g_icon_cache.clear(); s_epoch = e; }
  auto it = g_icon_cache.find(id);
  if (it != g_icon_cache.end()) return it->second;
  IconTex t = LoadItemIcon(id, identified);
  g_icon_cache[id] = t;
  return t;
}

// ── Description (clic-droit) : passe l'ItemSkillInfo COMPLET du nœud à OnMsg 0x18 ──
// On re-parcourt la liste session (0x015fbab0) pour retrouver le nœud par id (comme
// storage). item_desc_tweaks détecte la fenêtre 0xc et rend sa version enrichie.
void OpenItemDesc(uint32_t id, int mx, int my) {
  if (id == 0) return;
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (!head) return;
    uint8_t* found = nullptr;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    for (int guard = 0; node && node != head && guard < 2000; ++guard) {
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

// Retrouve le pointeur ItemSkillInfo (node+8) d'un item par son INDEX inventaire, en
// re-parcourant la liste session (comme OpenItemDesc, mais par index). nullptr si absent.
void* FindInfoByIndex(int index) {
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (!head) return nullptr;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    for (int guard = 0; node && node != head && guard < 2000; ++guard) {
      uint8_t* info = node + kNodeInfo;
      if (*reinterpret_cast<int*>(info + kInfoIndex) == index) return info;
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return nullptr;
}

// Shift+clic G : insère le LIEN de l'item dans l'input de la fenêtre qui a le FOCUS
// (chat), comme le natif UIInventoryWnd_OnLButtonDown (0x0094afb0, branche SHIFT) :
// fenêtre focus = g_UIWindowMgr+0x1a0, son type = wnd+0x2c ; les types d'input chat
// appellent UIChatWnd_InsertItemLink(wnd, info) (0x008217f0). L'utilisateur envoie
// ensuite avec Entrée. Ne fait RIEN si la fenêtre focus n'est pas un input chat.
using ChatInsertLink_t = void(__thiscall*)(void*, void*);
void PostItemLinkToChat(int index) {
  __try {
    void* info = FindInfoByIndex(index);
    if (!info) return;
    void* focused = *reinterpret_cast<void**>(kUIWindowMgr + 0x1a0);
    if (!focused) return;
    const int type = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(focused) + 0x2c);
    auto insert = reinterpret_cast<ChatInsertLink_t>(0x008217f0);
    if (type == 0x1ea || type == 0x1ee) {   // input chat (focus direct)
      insert(focused, info);
    } else if (type == 0x1ed) {              // input via fenêtre dédiée (DAT_0131f6b0+0xbc)
      void* base = *reinterpret_cast<void**>(0x0131f6b0);
      if (base) insert(*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + 0xbc), info);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Lit un pointeur de fenêtre valide depuis un slot (vtable vérifiée). SEH.
uint8_t* ReadValidWnd(uintptr_t slot, uintptr_t expected_vtable) {
  __try {
    auto* wnd = *reinterpret_cast<uint8_t**>(slot);
    if (!wnd) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(wnd) != expected_vtable) return nullptr;
    return wnd;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool MouseOverWnd(uintptr_t slot, uintptr_t vt, float x, float y) {
  uint8_t* w = ReadValidWnd(slot, vt);
  if (!w) return false;
  __try {
    const int wx = *reinterpret_cast<int*>(w + kOffPosX);
    const int wy = *reinterpret_cast<int*>(w + kOffPosY);
    const int ww = *reinterpret_cast<int*>(w + kOffWidth);
    const int wh = *reinterpret_cast<int*>(w + kOffHeight);
    return x >= wx && y >= wy && x < wx + ww && y < wy + wh;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool MouseOverCart(float x, float y)    { return MouseOverWnd(kCartWndGlobal, kCartVTable, x, y); }
bool MouseOverStorage(float x, float y) { return MouseOverWnd(kStorageSlot, kStorageVTable, x, y); }
bool CartOpen()    { return ReadValidWnd(kCartWndGlobal, kCartVTable) != nullptr; }
bool StorageOpen() { return ReadValidWnd(kStorageSlot, kStorageVTable) != nullptr; }

// True si le point est au-dessus du VIEWER storage ImGui : quand les DEUX (inventaire +
// storage) sont des viewers ImGui, le rect natif du storage est caché donc MouseOverStorage
// échoue -> on teste le rect du viewer storage pour router le dépôt par glisser.
bool StorageViewerOver(float x, float y) {
  auto* st = Bourgeon::Instance().storage_tweaks();
  return st && st->PointOverViewer(static_cast<int>(x), static_cast<int>(y));
}

// Fenêtre Équipement (id 0xa) via FindWindow ; nullptr si absente OU cachée (flag +0x28).
// Sert au drop d'équip : glisser un item dessus l'équipe.
constexpr int kEquipWndId = 0xa;
uint8_t* EquipWnd() {
  __try {
    auto* w = reinterpret_cast<uint8_t*>(reinterpret_cast<FindWindow_t>(kFindWindow)(
        reinterpret_cast<void*>(kUIWindowMgr), kEquipWndId));
    if (!w || *reinterpret_cast<int*>(w + kOffVisible) == 0) return nullptr;
    return w;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool EquipOpen() { return EquipWnd() != nullptr; }
bool MouseOverEquip(float x, float y) {
  uint8_t* w = EquipWnd();
  if (!w) return false;
  __try {
    const int wx = *reinterpret_cast<int*>(w + kOffPosX);
    const int wy = *reinterpret_cast<int*>(w + kOffPosY);
    const int ww = *reinterpret_cast<int*>(w + kOffWidth);
    const int wh = *reinterpret_cast<int*>(w + kOffHeight);
    return x >= wx && y >= wy && x < wx + ww && y < wy + wh;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Lecture SEH (POD only) des globals du footer -> hors OnRenderUI, qui contient des
// objets C++ (vector/filter) et ne peut donc pas héberger de __try (C2712).
struct FooterVals { int wmax = 0, wcur = 0, zeny = 0, expansion = 0, overPct = 0; };
FooterVals ReadFooterVals() {
  FooterVals v;
  __try {
    v.wmax = *reinterpret_cast<int*>(kWeightMax);
    v.wcur = *reinterpret_cast<int*>(kWeightCur);
    v.zeny = *reinterpret_cast<int*>(kZeny);
    v.expansion = *reinterpret_cast<int*>(kInvExpansion);
    v.overPct = *reinterpret_cast<int*>(kOverweightPct);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return v;
}

// ── Onglets de catégorie (par type d'item, repris du filtre natif case 0x17) ──
// Use {0,1,2,0x12} / Equip {4,5,8,9,0xb-0xf} / Etc {3,7,10,0x10,0x11,0x13} /
// Cartes {6} / Favoris (flag). "Tout" = tout. (Les cartes ont leur onglet -> exclues d'Etc.)
// img = base name des .bmp d'onglet (basic_interface\<img>1.bmp actif / <img>2.bmp
// inactif), repris de inventory_tweaks (onglets images du natif). nullptr => texte.
struct Cat { const char* label; const int* types; int n; bool fav; const char* img; };
const int kUse[]   = {0, 1, 2, 0x12};
const int kEquip[] = {4, 5, 8, 9, 0xb, 0xc, 0xd, 0xe, 0xf};
const int kEtc[]   = {3, 7, 10, 0x10, 0x11, 0x13};
const int kCard[]  = {6};
const Cat kCats[] = {
    {"Tout", nullptr, 0, false, "tab_all"},
    {"Conso", kUse, 4, false, "tab_use"},
    {"Equip", kEquip, 9, false, "tab_cos"},
    {"Etc", kEtc, 6, false, "tab_etc"},
    {"Cartes", kCard, 1, false, "tab_card"},
    {"Favoris", nullptr, 0, true, "tab_fav"},
};
constexpr int kNumCats = 6;

// Convertit une texture jeu en ImTextureID ImGui.
inline ImTextureID TexId(void* t) { return reinterpret_cast<ImTextureID>(t); }

// ── Assets natifs : barre 3-slice + fond de tuile + icônes du footer ───────────
// Barre custom btnbar_left3/mid3/right3.bmp + fond de tuile itemwin_mid.bmp (tuile
// native 32px) + icônes du footer natif icon_weight/icon_num.bmp. Chargés en textures
// ImGui. Préfixe CP949 pris sur les strings exe (basic_interface\ pour barre/tuile ;
// inventory\ pour les icônes, qui ont leur propre string).
constexpr uintptr_t kBtnbarPath     = 0x010357b8;  // "유저인터페이스\basic_interface\btnbar_left.bmp"
constexpr uintptr_t kIconWeightPath = 0x0103db00;  // "유저인터페이스\inventory\icon_weight.bmp"
constexpr uintptr_t kIconNumPath    = 0x0103dad4;  // "유저인터페이스\inventory\icon_num.bmp"

struct BarTex { void* tex = nullptr; int w = 0; int h = 0; };
BarTex g_bar[3];       // btnbar 3-slice : 0=left, 1=mid, 2=right
BarTex g_tile;         // itemwin_mid.bmp : fond de tuile d'item (32px)
BarTex g_tile_lock;    // itemwin_mid_lock.bmp : fond quand deal-lock actif sur Favoris
BarTex g_ico_weight;   // icon_weight.bmp : icône poids (footer)
BarTex g_ico_num;      // icon_num.bmp : icône compteur d'items (footer)
BarTex g_tab[kNumCats][2];  // onglets images [catégorie][0=actif(1.bmp), 1=inactif(2.bmp)]
BarTex g_btn_drop[2];  // item_drop_lock [0=off/déverrouillé, 1=on/verrouillé]
BarTex g_btn_deal[2];  // bt_itemDeal_lock [0=off, 1=on/verrouillé (anti-vente NPC)]
BarTex g_btn_sort[2];  // bt_sort [0=off (_off.bmp), 1=on (.bmp) = vue triée]
bool   g_assets_tried = false;

// Charge un .bmp (chemin CP949) en texture ImGui (colorkey magenta -> alpha 0).
BarTex LoadTexByPath(const char* path) {
  RawTex rt{};
  if (!GetRawTex(path, &rt)) return {};
  std::vector<uint8_t> argb(static_cast<size_t>(rt.w) * rt.h * 4);
  for (int p = 0; p < rt.w * rt.h; ++p) {
    const uint8_t b = rt.bgra[p * 4], g = rt.bgra[p * 4 + 1], r = rt.bgra[p * 4 + 2];
    const bool ck = (r == 0xFF && g == 0 && b == 0xFF);  // magenta -> transparent
    argb[p * 4] = b; argb[p * 4 + 1] = g; argb[p * 4 + 2] = r;
    argb[p * 4 + 3] = ck ? 0 : 0xFF;
  }
  return {Overlay_CreateTextureARGB(argb.data(), rt.w, rt.h), rt.w, rt.h};
}

// Construit `<préfixe basic_interface\> + <file>` depuis la string exe du btnbar.
void BasicInterfacePath(const char* file, char* out, size_t out_sz) {
  const char* base = reinterpret_cast<const char*>(kBtnbarPath);
  const char* slash = std::strrchr(base, '\\');
  const size_t n = slash ? static_cast<size_t>(slash - base + 1) : 0;
  if (n && n < out_sz) std::memcpy(out, base, n);
  std::snprintf(out + n, out_sz - n, "%s", file);
}

// Idem pour \inventory\ : base = string exe icon_weight (kIconWeightPath, préfixe CP949
// 유저인터페이스\inventory\ + nom de fichier), on remplace le nom -> réutilise le préfixe
// natif (pas de hardcode). Sert aux bmps de boutons du footer, qui sont stockés SANS
// préfixe dans l'exe (le code natif le prépend).
void InventoryPath(const char* file, char* out, size_t out_sz) {
  const char* base = reinterpret_cast<const char*>(kIconWeightPath);
  const char* slash = std::strrchr(base, '\\');
  const size_t n = slash ? static_cast<size_t>(slash - base + 1) : 0;
  if (n && n < out_sz) std::memcpy(out, base, n);
  std::snprintf(out + n, out_sz - n, "%s", file);
}

void LoadFooterAssets() {
  if (g_assets_tried) return;
  g_assets_tried = true;
  char path[160];
  const char* names[3] = {"btnbar_left3.bmp", "btnbar_mid3.bmp", "btnbar_right3.bmp"};
  for (int i = 0; i < 3; ++i) {
    BasicInterfacePath(names[i], path, sizeof(path));
    g_bar[i] = LoadTexByPath(path);
  }
  BasicInterfacePath("itemwin_mid.bmp", path, sizeof(path));
  g_tile = LoadTexByPath(path);
  // Variante « verrouillée » du fond de tuile (onglet Favoris + deal-lock actif) : le
  // natif remplace itemwin_mid par itemwin_mid_lock (\inventory\, RE UIInventoryWnd_DrawContent).
  InventoryPath("itemwin_mid_lock.bmp", path, sizeof(path));
  g_tile_lock = LoadTexByPath(path);
  g_ico_weight = LoadTexByPath(reinterpret_cast<const char*>(kIconWeightPath));
  g_ico_num    = LoadTexByPath(reinterpret_cast<const char*>(kIconNumPath));
  // Onglets images (basic_interface\<img>1.bmp actif / <img>2.bmp inactif).
  for (int c = 0; c < kNumCats; ++c) {
    if (!kCats[c].img) continue;
    char nm[40];
    std::snprintf(nm, sizeof(nm), "%s1.bmp", kCats[c].img);
    BasicInterfacePath(nm, path, sizeof(path)); g_tab[c][0] = LoadTexByPath(path);
    std::snprintf(nm, sizeof(nm), "%s2.bmp", kCats[c].img);
    BasicInterfacePath(nm, path, sizeof(path)); g_tab[c][1] = LoadTexByPath(path);
  }
  // Boutons footer natifs (\inventory\..., préfixe CP949 via InventoryPath). idx1 =
  // état ACTIF (verrouillé / trié). Noms exacts = strings exe (RE 2026-07-09).
  InventoryPath("item_drop_lock_off.bmp", path, sizeof(path)); g_btn_drop[0] = LoadTexByPath(path);
  InventoryPath("item_drop_lock_on.bmp",  path, sizeof(path)); g_btn_drop[1] = LoadTexByPath(path);
  InventoryPath("bt_itemDeal_lock_off.bmp", path, sizeof(path)); g_btn_deal[0] = LoadTexByPath(path);
  InventoryPath("bt_itemdeal_lock_on.bmp",  path, sizeof(path)); g_btn_deal[1] = LoadTexByPath(path);
  InventoryPath("bt_sort_off.bmp", path, sizeof(path)); g_btn_sort[0] = LoadTexByPath(path);
  InventoryPath("bt_sort.bmp",     path, sizeof(path)); g_btn_sort[1] = LoadTexByPath(path);
}

// Hauteur de la barre = hauteur du morceau milieu (repli 22 px si non chargé).
int FooterBarHeight() { return g_bar[1].h > 0 ? g_bar[1].h : 22; }

// Teinte skin (luminosité + opacité) pour les AddImage. Définie plus bas (avant
// DrawTiledBg) ; déclarée ici pour que DrawFooterBar/DrawFooterIcon la voient.
inline ImU32 SkinImgTint();

// Dessine la barre 3-slice dans [x0..x1] à y0 (hauteur barH). Repli rect plein RO.
void DrawFooterBar(ImDrawList* dl, float x0, float y0, float x1, float barH) {
  const float y1 = y0 + barH;
  const float lw = g_bar[0].w > 0 ? static_cast<float>(g_bar[0].w) : 0.0f;
  const float rw = g_bar[2].w > 0 ? static_cast<float>(g_bar[2].w) : 0.0f;
  if (g_bar[1].tex) {
    const ImU32 t = SkinImgTint();
    if (g_bar[0].tex) dl->AddImage(TexId(g_bar[0].tex), ImVec2(x0, y0), ImVec2(x0 + lw, y1), ImVec2(0, 0), ImVec2(1, 1), t);
    dl->AddImage(TexId(g_bar[1].tex), ImVec2(x0 + lw, y0), ImVec2(x1 - rw, y1), ImVec2(0, 0), ImVec2(1, 1), t);  // étiré
    if (g_bar[2].tex) dl->AddImage(TexId(g_bar[2].tex), ImVec2(x1 - rw, y0), ImVec2(x1, y1), ImVec2(0, 0), ImVec2(1, 1), t);
  } else {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), 2.0f);
  }
}

// Dessine une icône BarTex centrée verticalement à (x, cy) ; renvoie sa largeur (0 si absente).
float DrawFooterIcon(ImDrawList* dl, const BarTex& ic, float x, float cy) {
  if (!ic.tex || ic.w <= 0 || ic.h <= 0) return 0.0f;
  dl->AddImage(TexId(ic.tex), ImVec2(x, cy - ic.h * 0.5f),
               ImVec2(x + ic.w, cy + ic.h * 0.5f), ImVec2(0, 0), ImVec2(1, 1), SkinImgTint());
  return static_cast<float>(ic.w);
}

// Bouton-bascule IMAGE du footer : dessine le bmp natif on/off à sa taille native,
// centré verticalement sur cyc, + InvisibleButton (survol = pleine luminosité). Repli
// carré+glyphe si le bmp n'a pas chargé. Renvoie true au clic ; *out_w = largeur posée.
bool FooterImgToggle(const char* id, float x, float cyc, const BarTex& on,
                     const BarTex& off, bool active, const char* glyph, const char* tip,
                     float* out_w) {
  const BarTex& t = active ? on : off;
  const bool haveTex = t.tex && t.w > 0 && t.h > 0;
  const float w = haveTex ? static_cast<float>(t.w) : 18.0f;
  const float h = haveTex ? static_cast<float>(t.h) : 18.0f;
  const float y = cyc - h * 0.5f;
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0(x, y), p1(x + w, y + h);
  if (haveTex) {
    const ImU32 tint = hov ? IM_COL32(255, 255, 255, 255) : SkinImgTint();
    dl->AddImage(TexId(t.tex), p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint);
  } else {  // repli glyphe (bmp absent)
    const ImU32 bg = active ? IM_COL32(120, 165, 225, 220)
                            : (hov ? IM_COL32(205, 205, 205, 210)
                                   : IM_COL32(185, 185, 185, 140));
    dl->AddRectFilled(p0, p1, bg, 2.0f);
    dl->AddRect(p0, p1, IM_COL32(110, 110, 110, 220), 2.0f);
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f),
                active ? IM_COL32(255, 255, 255, 255) : IM_COL32(45, 45, 45, 255), glyph);
  }
  if (hov && tip) ImGui::SetTooltip(" %s ", tip);
  if (out_w) *out_w = w;
  return clicked;
}

// Largeur du strip d'onglets = plus grande largeur d'image d'onglet (repli 22 px).
float TabStripWidth() {
  float w = 0.0f;
  for (int c = 0; c < kNumCats; ++c)
    for (int s = 0; s < 2; ++s)
      if (g_tab[c][s].w > w) w = static_cast<float>(g_tab[c][s].w);
  return w > 0.0f ? w : 22.0f;
}

// Teinte des AddImage (icônes/tuiles/onglets/footer) = luminosité + opacité du skin RO,
// pour que ces réglages s'appliquent AUSSI aux images du jeu (dessinées en draw-list
// brut). b>1 ne peut pas sur-exposer via col -> capé à 1 ; a = style.Alpha du skin.
inline ImU32 SkinImgTint() {
  float b = ro::SkinImageBrightness();
  if (b > 1.0f) b = 1.0f;
  const int c = static_cast<int>(b * 255.0f + 0.5f);
  const int a = static_cast<int>(ImGui::GetStyle().Alpha * 255.0f + 0.5f);
  return IM_COL32(c, c, c, a);
}

// Dessine itemwin_mid PAVÉ (répété à sa taille native) dans [mn..mx], clippé — le fond
// continu du natif (au lieu d'étirer une copie par tuile). Repli rect plein sinon.
void DrawTiledBg(ImDrawList* dl, const BarTex& tile, ImVec2 origin, ImVec2 mn, ImVec2 mx) {
  if (!tile.tex || tile.w <= 0 || tile.h <= 0) {
    dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGuiCol_FrameBg));
    return;
  }
  const float tw = static_cast<float>(tile.w), th = static_cast<float>(tile.h);
  // Pavage ALIGNÉ sur `origin` (la 1re tuile de la grille) : le fond et les items
  // partagent la même marge (fin du désync). Démarre à la 1re tuile <= mn (floor -inf).
  auto floorTo = [](float v, float o, float step) {
    int n = static_cast<int>((v - o) / step);
    if (o + n * step > v) --n;
    return o + n * step;
  };
  const float sx = floorTo(mn.x, origin.x, tw);
  const float sy = floorTo(mn.y, origin.y, th);
  dl->PushClipRect(mn, mx, true);
  for (float y = sy; y < mx.y; y += th)
    for (float x = sx; x < mx.x; x += tw)
      dl->AddImage(TexId(tile.tex), ImVec2(x, y), ImVec2(x + tw, y + th),
                   ImVec2(0, 0), ImVec2(1, 1), SkinImgTint());
  dl->PopClipRect();
}

// ── Resize par PALIER de tuile (repris de cashshop_tweaks) ─────────────────────
// La fenêtre saute d'une COLONNE / LIGNE de tuiles à la fois -> jamais de colonne
// partielle ni d'espace vide (le vrai fix d'overflow). Le chrome (fenêtre - zone de
// grille) est mesuré la frame précédente et stocké dans g_snap.
struct SnapState { float cell = 32, gap = 2, chromew = 0, chromeh = 0; bool valid = false; };
SnapState g_snap;
void SnapWindowSize(ImGuiSizeCallbackData* d) {
  const SnapState& s = g_snap;
  const float step = s.cell + s.gap;
  int cols = static_cast<int>((d->DesiredSize.x - s.chromew + s.gap) / step + 0.5f);
  int rows = static_cast<int>((d->DesiredSize.y - s.chromeh + s.gap) / step + 0.5f);
  if (cols < 5) cols = 5;  // minimum 5 colonnes de tuiles (pas de resize sous 5x5)
  if (rows < 5) rows = 5;  // minimum 5 lignes de tuiles
  d->DesiredSize.x = s.chromew + cols * step - s.gap;
  d->DesiredSize.y = s.chromeh + rows * step - s.gap;
}

// Vide les caches de textures quand le device D3D a été reset/recréé (handles morts).
unsigned g_tex_epoch = 0;
void MaybeFlushTextures() {
  const unsigned e = Overlay_DeviceEpoch();
  if (e == g_tex_epoch) return;
  g_tex_epoch = e;
  g_icon_cache.clear();
  for (auto& b : g_bar) b = BarTex{};
  g_tile = g_tile_lock = g_ico_weight = g_ico_num = BarTex{};
  for (auto& row : g_tab) for (auto& b : row) b = BarTex{};
  for (auto& b : g_btn_drop) b = BarTex{};
  for (auto& b : g_btn_deal) b = BarTex{};
  for (auto& b : g_btn_sort) b = BarTex{};
  g_assets_tried = false;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════
InventoryViewer::InventoryViewer() {}

// Cache la fenêtre native DÈS sa création (avant le 1er rendu) -> zéro flicker.
void InventoryViewer::HideNativeAtCreation(void* win) {
  if (!win || !imgui_enabled_) return;
  __try {
    if (*reinterpret_cast<uintptr_t*>(win) != kInvVTable) return;
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Remplit items_/item_count_ depuis le MODÈLE SESSION (0x015fbab0), donc marche même
// fenêtre native cachée. POD-only sous SEH ; le nom complet est bâti via BuildDisplayName
// (repli GetBaseName), en passant la fenêtre inventaire native comme contexte.
void InventoryViewer::Extract() {
  item_count_ = 0;
  void* wnd = nullptr;
  uint8_t* head = nullptr;
  uint8_t* node = nullptr;
  // Tête de liste + 1er nœud (contexte fenêtre = pour BuildName). RE workflow : la
  // liste 0x015fbab0 (g_session+0x16f0) contient DÉJÀ tous les items (équipés inclus),
  // = la même source que le natif. Le "16 au lieu de 22" venait d'un try/except GLOBAL
  // qu'UN SEUL item fautif (id SSO corrompue / BuildDisplayName forgé) faisait avorter.
  __try {
    wnd  = *reinterpret_cast<void**>(kInvWndGlobal);
    head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (head) node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
  if (!head) return;

  int guard = 0;
  while (node && node != head && item_count_ < kMaxItems && guard < kMaxItems) {
    // Pointeur SUIVANT lu D'ABORD sous garde : un nœud corrompu arrête net (pas de
    // boucle infinie / faute) sans perdre les items déjà lus.
    uint8_t* next = nullptr;
    __try { next = *reinterpret_cast<uint8_t**>(node + kNodeNext); }
    __except (EXCEPTION_EXECUTE_HANDLER) { break; }

    // Extraction PAR ITEM sous SEH ISOLÉ -> une faute est confinée à l'item (sauté),
    // l'énumération continue jusqu'au bout (fix du "16 au lieu de 22").
    __try {
      Item& it = items_[item_count_];
      uint8_t* info = node + kNodeInfo;
      const uint32_t idcap = *reinterpret_cast<uint32_t*>(info + kInfoIdCap);
      const char* ids = (idcap > 0xf) ? *reinterpret_cast<char**>(info + kInfoIdStr)
                                      : reinterpret_cast<const char*>(info + kInfoIdStr);
      it.id = ids ? static_cast<uint32_t>(atoi(ids)) : 0;
      it.identified = *reinterpret_cast<uint8_t*>(info + kInfoIdent);
      it.amount = *reinterpret_cast<int*>(node + kNodeAmt);
      it.index  = *reinterpret_cast<int*>(info + kInfoIndex);
      it.loc    = *reinterpret_cast<uint32_t*>(info + kInfoLoc);
      it.refine = *reinterpret_cast<int*>(info + kInfoRefine);
      it.type   = *reinterpret_cast<int*>(info + kInfoType);
      it.favorite = *reinterpret_cast<uint8_t*>(info + kInfoFav);
      SafeBuildName(wnd, info, it.name, sizeof(it.name));  // nom (SEH isolé + repli GetBaseName)
      ++item_count_;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    node = next;
    ++guard;
  }
}

void InventoryViewer::OnTick() {
  open_ = false;
  uint8_t* wnd = ReadValidWnd(kInvWndGlobal, kInvVTable);
  if (wnd) {
    __try {
      if (!was_open_) {
        spawn_x_ = *reinterpret_cast<int*>(wnd + kOffPosX);
        spawn_y_ = *reinterpret_cast<int*>(wnd + kOffPosY);
        need_pos_ = true;
      }
      // Master switch : ON -> cache le natif (hors rendu+hit-test) + viewer ; OFF ->
      // natif seul, aucun viewer. Forcé chaque tick (le natif peut remettre à 1).
      *reinterpret_cast<int*>(wnd + kOffVisible) = imgui_enabled_ ? 0 : 1;
      open_ = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { open_ = false; }
    if (open_) Extract();
  }
  was_open_ = open_;
}

void InventoryViewer::OnMouseDown(int mx, int my) {
  mousedown_over_viewer_ =
      win_valid_ && mx >= win_x_ && my >= win_y_ &&
      mx < win_x_ + win_w_ && my < win_y_ + win_h_;
  // Source d'un drag NATIF relâché sur le viewer : Équipement -> dés-équiper ; Chariot
  // -> retirer vers l'inventaire.
  mousedown_over_equip_ =
      MouseOverEquip(static_cast<float>(mx), static_cast<float>(my));
  mousedown_over_cart_ =
      MouseOverCart(static_cast<float>(mx), static_cast<float>(my));
}

// Appelé par le hook WndProc au WM_LBUTTONUP (pré-input). Un drag NATIF relâché sur le
// viewer : depuis l'Équipement => dés-équiper (CZ_REQ_TAKEOFF_EQUIP) ; depuis le Chariot
// => retirer vers l'inventaire (cmd 0x4d). Puis vider la charge (pas de drop au sol).
bool InventoryViewer::HandleNativeDrop(int mx, int my) {
  if (!open_ || !imgui_enabled_ || !win_valid_) return false;
  if (!mousedown_over_equip_ && !mousedown_over_cart_) return false;  // seules sources gérées
  if (ImGui::GetDragDropPayload() != nullptr) return false;  // pas pendant un drag ImGui
  const bool over = !(mx < win_x_ || my < win_y_ ||
                      mx >= win_x_ + win_w_ || my >= win_y_ + win_h_);
  if (!over) return false;
  void* obj = Dispatcher();
  if (!obj) return false;
  int index = 0, qty = 0;
  if (!ReadDraggedItem(obj, &index, &qty)) return false;
  if (mousedown_over_equip_) SendUnequip(index);                   // équip -> inventaire
  else                       SendCmd(kCmdCartToBody, index, qty);  // chariot -> inventaire
  CancelNativeDrag(obj);
  return true;
}

// Équipe l'item d'inventaire actuellement glissé : drag_index_/type_/loc_ sont les
// valeurs SERVEUR stables (it.index/type/loc, rafraîchies chaque frame par la source du
// drag dont l'ID est semé par l'index stable) -> robuste à une renumérotation d'items_
// pendant le glisser, et indépendant de l'ordre de rendu des fenêtres. Utilisé par le
// drag-drop cross-plugin de character_sheet.
bool InventoryViewer::EquipDraggedItem(bool leftHand) {
  if (!drag_active_ || !IsEquippable(drag_type_)) return false;
  UseOrEquip(drag_index_, drag_type_, drag_loc_, leftHand);
  return true;
}

// Wrapper public sur le helper interne PostItemLinkToChat (insère le lien dans l'input
// chat focalisé). Réutilisé par character_sheet (Maj+clic droit sur un slot équipé).
void InventoryViewer::LinkItemToChat(int invIndex) { PostItemLinkToChat(invIndex); }

void InventoryViewer::OnRenderUI() {
  if (!open_ || !imgui_enabled_) return;
  MaybeFlushTextures();  // device reset/TDR -> lâche les handles morts

  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(spawn_x_),
                                   static_cast<float>(spawn_y_)),
                            ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(300, 360), ImGuiCond_FirstUseEver);
  // Resize par PALIER de tuile (chrome mesuré la frame précédente) : largeur/hauteur
  // saute d'une colonne/ligne de tuiles -> jamais de colonne partielle (fix overflow).
  if (g_snap.valid) {
    const float minGrid = 5.0f * (g_snap.cell + g_snap.gap) - g_snap.gap;  // min 5 tuiles
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(g_snap.chromew + minGrid, g_snap.chromeh + minGrid),
        ImVec2(10000.0f, 10000.0f), SnapWindowSize);
  }

  // Pas de NoCollapse -> le skin RO affiche le bouton minimiser (repli barre de titre),
  // comme le natif ; clic dessus = SetWindowCollapsed (géré par BeginRoWindow).
  const bool begun =
      ro::BeginRoWindow("Inventaire###bourgeon_inventory", &show_panel_, 0);
  // X du viewer -> ferme l'inventaire natif (client-side). Réarme show_panel_.
  if (!show_panel_) { CloseInventory(); show_panel_ = true; }
  if (!begun) { ro::EndRoWindow(); return; }

  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  win_x_ = wp.x; win_y_ = wp.y; win_w_ = ws.x; win_h_ = ws.y;
  win_valid_ = true;

  // ── Action en attente (drop/transfert d'une pile) -> prompt quantité ──
  auto do_move = [this](int amount) {
    switch (pend_action_) {
      case kPendDrop:      SendDrop(pend_index_, amount); break;
      case kPendToCart:    SendCmd(kCmdToCart, pend_index_, amount); break;
      case kPendToStorage: SendCmd(kCmdToStorage, pend_index_, amount); break;
      default: break;
    }
  };
  if (pend_id_ != 0) {
    if (pend_open_prompt_) { ImGui::OpenPopup("Quantité"); pend_open_prompt_ = false; }
    else if (pend_max_ <= 1) { do_move(1); pend_id_ = 0; }
  }
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));
  ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
  const bool popen =
      ImGui::BeginPopupModal("Quantité", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PopStyleColor();
  if (popen) {
    const char* verb = pend_action_ == kPendDrop      ? "Jeter"
                     : pend_action_ == kPendToCart     ? "Vers le chariot"
                     : pend_action_ == kPendToStorage  ? "Vers le storage"
                                                       : "Déplacer";
    ImGui::Text("%s combien ? (max %d)", verb, pend_max_);
    static int dq = 1;
    if (ImGui::IsWindowAppearing()) { dq = pend_max_; ImGui::SetKeyboardFocusHere(); }
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("##dq", &dq);
    if (dq < 1) dq = 1;
    if (dq > pend_max_) dq = pend_max_;
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (ImGui::Button("OK") || enter) { do_move(dq); pend_id_ = 0; dq = 1; ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Tout")) { do_move(pend_max_); pend_id_ = 0; dq = 1; ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Annuler")) { pend_id_ = 0; dq = 1; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
  }

  // ── Aide raccourcis (?) + recherche (occupe le reste de la ligne) ──
  ImGui::AlignTextToFramePadding();
  std::string desc = "Raccourcis inventaire\n\n"
                     "- Double-clic gauche : utiliser / équiper\n"
                     "- Ctrl + double-clic gauche : équiper en main gauche\n"
                     "- Maj + clic gauche : lien de l'item dans l'input du chat (puis Entrée)\n"
                     "- Clic droit : menu contextuel\n"
                     "- Ctrl + clic droit : description\n"
                     "- Maj + clic droit : (dé)favori\n"
                     "- Alt + clic droit : transfert rapide (storage / chariot si ouvert)\n"
                     "- Glisser : chariot / storage / équipement / barre d'action / sol\n"
                     "- Glisser un favori sur un autre onglet : le retirer des favoris";
  HelpMarker(desc.c_str());
  ImGui::SameLine();
  static ImGuiTextFilter filter;
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::InputTextWithHint("##inv_filter", "Filtrer...", filter.InputBuf,
                               IM_ARRAYSIZE(filter.InputBuf)))
    filter.Build();

  // ── Dimensions communes (footer réservé + snap) ──
  LoadFooterAssets();
  const float lineH = ImGui::GetTextLineHeight();
  const float footerH = 2.0f * lineH + 6.0f;  // 2 lignes compactes (barre btnbar étirée)
  const ImGuiStyle& style = ImGui::GetStyle();
  const float mainW = ImGui::GetWindowWidth();
  const float mainH = ImGui::GetWindowHeight();
  const float childH = -(footerH + style.ItemSpacing.y);

  // ── Onglets IMAGES verticaux à GAUCHE (comme inventory_tweaks : tab_use/cos/etc/
  //    card/fav ; actif = <img>1.bmp, inactif = <img>2.bmp). Tout = libellé texte. ──
  const float tabW = TabStripWidth();
  // Rect (écran) de l'onglet ACTIF, capturé dans la boucle -> sert à "manger" le bord
  // C5C5C5 entre le strip et la grille (passage blanc continu = souligne l'onglet actif).
  ImVec2 activeTabMin(0, 0), activeTabMax(0, 0);
  bool haveActiveTab = false;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));  // strip sans marge
  ImGui::BeginChild("inv_tabs", ImVec2(tabW, childH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, -1.0f));  // onglets jointifs
  {
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    for (int c = 0; c < kNumCats; ++c) {
      const bool sel = (cur_tab_ == c);
      ImGui::PushID(c);
      const BarTex& img = g_tab[c][sel ? 0 : 1].tex ? g_tab[c][sel ? 0 : 1]
                                                     : g_tab[c][sel ? 1 : 0];
      if (img.tex && img.w > 0 && img.h > 0) {
        const float iw = tabW, ih = tabW * img.h / static_cast<float>(img.w);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("tab", ImVec2(tabW, ih))) cur_tab_ = c;
        const ImVec2 pe(p.x + iw, p.y + ih);
        // L'image active/inactive indique déjà la sélection -> pas de cadre jaune.
        tdl->AddImage(TexId(img.tex), p, pe, ImVec2(0, 0), ImVec2(1, 1), SkinImgTint());
        if (!sel && ImGui::IsItemHovered())
          tdl->AddRectFilled(p, pe, IM_COL32(255, 255, 255, 45));  // survol : éclaircir (pas de bleu)
      } else {
        if (ImGui::Selectable(kCats[c].label, sel, 0, ImVec2(tabW, 0.0f)))
          cur_tab_ = c;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(" %s ", kCats[c].label);
      // Glisser un item sur un onglet : sur Favoris = l'AJOUTE aux favoris ; hors de
      // Favoris (item déjà favori) = le RETIRE (comme le natif).
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("INV_ITEM")) {
          const int di = *static_cast<const int*>(pl->Data);
          if (di >= 0 && di < item_count_) {
            if (kCats[c].fav && !items_[di].favorite)
              SendFavoriteToggle(items_[di].index, false);  // -> ajoute aux favoris
            else if (!kCats[c].fav && items_[di].favorite)
              SendFavoriteToggle(items_[di].index, true);   // -> retire des favoris
          }
        }
        ImGui::EndDragDropTarget();
      }
      if (sel) {  // onglet actif -> mémorise son rect pour le passage blanc vers la grille
        activeTabMin = ImGui::GetItemRectMin();
        activeTabMax = ImGui::GetItemRectMax();
        haveActiveTab = true;
      }
      ImGui::PopID();
    }
  }
  ImGui::PopStyleVar();  // ItemSpacing
  ImGui::EndChild();
  ImGui::PopStyleVar();  // WindowPadding (strip)

  // ── Vue filtrée (onglet courant + recherche), triée par type puis nom ──
  auto in_tab = [](int tab, const Item& it) -> bool {
    const Cat& c = kCats[tab];
    if (c.fav) return it.favorite != 0;   // onglet Favoris : uniquement les favoris
    if (it.favorite != 0) return false;   // ailleurs : favoris MASQUÉS (comme le natif)
    if (!c.types) return true;            // Tout (hors favoris)
    for (int i = 0; i < c.n; ++i) if (c.types[i] == it.type) return true;
    return false;
  };
  std::vector<int> view;
  view.reserve(item_count_);
  for (int i = 0; i < item_count_; ++i)
    if (in_tab(cur_tab_, items_[i]) && filter.PassFilter(items_[i].name))
      view.push_back(i);
  if (sort_enabled_) {  // bouton Tri (footer, onglet Favoris)
    std::sort(view.begin(), view.end(), [this](int a, int b) {
      if (items_[a].type != items_[b].type) return items_[a].type < items_[b].type;
      return _stricmp(items_[a].name, items_[b].name) < 0;
    });
  } else {
    std::sort(view.begin(), view.end(),
              [this](int a, int b) { return items_[a].index < items_[b].index; });
  }

  // ── Grille de tuiles 32px (fond itemwin_mid), à DROITE des onglets. Défile. ──
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));  // grille sans marge
  ImGui::BeginChild("invgrid", ImVec2(0.0f, childH), true,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
  {
    const float cell = 32.0f, gap = 0.0f;  // tuiles natives 32px, jointives (sans padding)
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const float availw = ImGui::GetContentRegionAvail().x;  // exclut déjà la scrollbar
    const float availh = ImGui::GetContentRegionAvail().y;
    // Mesure du chrome (fenêtre - zone grille) pour le snap de resize (frame +1).
    g_snap.cell = cell; g_snap.gap = gap;
    g_snap.chromew = mainW - availw;
    g_snap.chromeh = mainH - availh;
    g_snap.valid = true;
    int cols = static_cast<int>((availw + gap) / (cell + gap));
    if (cols < 1) cols = 1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Fond itemwin_mid PAVÉ, ALIGNÉ sur la grille d'items (même marge que les tuiles).
    {
      const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();  // = position de la 1re tuile
      const ImVec2 gmn = ImGui::GetWindowPos();
      const ImVec2 gsz = ImGui::GetWindowSize();
      // Onglet Favoris + deal-lock actif -> fond « verrouillé » (itemwin_mid_lock), comme
      // le natif (UIInventoryWnd_DrawContent : this+0x10c==3 && g_inv_dealLock). Repli sur
      // le fond normal si le bmp verrouillé n'a pas chargé.
      const bool tilesLocked = kCats[cur_tab_].fav && ReadLock(kDealLockGlobal);
      const BarTex& bg = (tilesLocked && g_tile_lock.tex) ? g_tile_lock : g_tile;
      DrawTiledBg(dl, bg, gridOrigin, gmn, ImVec2(gmn.x + gsz.x, gmn.y + gsz.y));
    }
    for (int k = 0; k < static_cast<int>(view.size()); ++k) {
      if (k % cols != 0) ImGui::SameLine();
      const int idx = view[k];
      const Item& it = items_[idx];
      // ID semé par l'index serveur STABLE (pas la position volatile) : si l'inventaire
      // est renuméroté pendant un glisser (conso/autoloot serveur), le drag reste collé
      // au bon item et le payload de position s'auto-corrige à la frame suivante.
      ImGui::PushID(it.index);
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton("cell", ImVec2(cell, cell));
      const bool hovered = ImGui::IsItemHovered();
      const ImVec2 p1 = ImVec2(p0.x + cell, p0.y + cell);

      // Fond = pavage itemwin_mid dessiné globalement (DrawTiledBg). Ici : juste la
      // surbrillance au survol (pas de cadre favori : les favoris sont sur leur onglet).
      if (hovered)  // survol : léger éclaircissement (le HeaderHovered du skin est BLEU)
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 55), 0.0f);

      // Icône à sa taille NATIVE (comme le natif : bmp dessiné 1:1, ~24px dans une
      // tuile 32px), centrée ; réduite seulement si plus grande que la tuile.
      const IconTex ic = ResolveIcon(it.id, it.identified);
      if (ic.tex && ic.w > 0 && ic.h > 0) {
        float dw = static_cast<float>(ic.w), dh = static_cast<float>(ic.h);
        if (dw > cell || dh > cell) {
          const float s = cell / (dw > dh ? dw : dh);
          dw *= s; dh *= s;
        }
        const ImVec2 ip(p0.x + (cell - dw) * 0.5f, p0.y + (cell - dh) * 0.5f);
        dl->AddImage(TexId(ic.tex), ip, ImVec2(ip.x + dw, ip.y + dh), ImVec2(0, 0), ImVec2(1, 1), SkinImgTint());
      } else {
        dl->AddText(ImVec2(p0.x + cell * 0.5f - 4, p0.y + cell * 0.5f - 7),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), "?");
      }

      // Coin bas-droit : refine "+N" (équipement) OU quantité (pile) — exclusifs (un
      // équipement ne s'empile pas). Texte NOIR cerné de BLANC (lisible sur tout fond).
      char q[16] = {0};
      if (it.refine > 0)      std::snprintf(q, sizeof(q), "+%d", it.refine);
      else if (it.amount > 1) std::snprintf(q, sizeof(q), "%d", it.amount);
      if (q[0]) {
        const ImVec2 ts = ImGui::CalcTextSize(q);
        const ImVec2 qp(p1.x - ts.x - 2, p1.y - ts.y - 1);
        const ImU32 white = IM_COL32(255, 255, 255, 255);
        for (int oy = -1; oy <= 1; ++oy)
          for (int ox = -1; ox <= 1; ++ox)
            if (ox || oy) dl->AddText(ImVec2(qp.x + ox, qp.y + oy), white, q);
        dl->AddText(qp, IM_COL32(0, 0, 0, 255), q);
      }

      // Survol : tooltip + double-clic = utiliser/équiper.
      if (hovered) {
        ImGui::BeginTooltip();
        ImGui::Text(" %s [%d] ", it.name[0] ? it.name : "(?)", it.id);
        if (it.amount > 1) ImGui::TextDisabled(" Quantité : %d ", it.amount);
        ImGui::EndTooltip();
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
          UseOrEquip(it.index, it.type, it.loc, ImGui::GetIO().KeyCtrl);  // Ctrl = main gauche
      }
      // Raccourcis clavier+souris natifs (RE UIInventoryWnd_OnRButtonDown) :
      //   Shift + clic GAUCHE  = poster le lien de l'item dans le chat (0x14e) ;
      //   Ctrl  + clic DROIT   = ouvrir la description directement (sans menu) ;
      //   Shift + clic DROIT   = (dé)favori ;
      //   Alt   + clic DROIT   = transfert rapide vers storage (sinon chariot) si ouvert ;
      //   clic DROIT seul      = menu contextuel.
      const ImGuiIO& mods = ImGui::GetIO();
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && mods.KeyShift)
        PostItemLinkToChat(it.index);  // Shift+clic G : lien item -> input chat focus
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (mods.KeyCtrl) {
          POINT pt; if (GetCursorPos(&pt)) OpenItemDesc(it.id, pt.x, pt.y);
        } else if (mods.KeyShift) {
          SendFavoriteToggle(it.index, it.favorite != 0);
        } else if (mods.KeyAlt) {
          if (StorageOpen())   SendCmd(kCmdToStorage, it.index, it.amount);
          else if (CartOpen()) SendCmd(kCmdToCart, it.index, it.amount);
        } else {
          ImGui::OpenPopup("ctx");
        }
      }

      // Source de drag (transfert chariot/storage ou jet au sol selon la cible).
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        drag_active_ = true;
        drag_index_ = it.index; drag_amount_ = it.amount; drag_type_ = it.type; drag_loc_ = it.loc;
        drag_id_ = it.id;  // nameid : cible d'un dépôt sur la barre d'action (skill_bar_tweaks)
        ImGui::SetDragDropPayload("INV_ITEM", &idx, sizeof(idx));
        if (ic.tex) { ImGui::Image(TexId(ic.tex), ImVec2(24, 24)); ImGui::SameLine(); }
        ImGui::TextUnformatted(it.name[0] ? it.name : "(?)");
        ImGui::EndDragDropSource();
      }
      // (Le déséquip par glisser-vers-l'inventaire est détecté côté character_sheet via
      //  PointOverViewer -> couvre TOUTE la fenêtre, pas seulement les cases avec item.)

      // Menu contextuel : toutes les actions.
      if (ImGui::BeginPopup("ctx")) {
        ImGui::TextDisabled("%s", it.name[0] ? it.name : "(?)");
        ImGui::Separator();
        if (ImGui::MenuItem("Description")) {
          POINT pt; if (GetCursorPos(&pt)) OpenItemDesc(it.id, pt.x, pt.y);
        }
        const char* act = (it.type == 0 || it.type == 1 || it.type == 2 ||
                           it.type == 0x12) ? "Utiliser"
                        : "Équiper";
        const bool usable = it.type <= 2 || it.type == 0x12 ||
                            (it.type >= 4 && it.type <= 0xf) || it.type == 6 ||
                            it.type == 0xa || it.type == 0x10 || it.type == 0x11 ||
                            it.type == 0x13;
        if (usable && ImGui::MenuItem(act)) UseOrEquip(it.index, it.type, it.loc, false);
        if (ImGui::MenuItem(it.favorite ? "Retirer des favoris" : "Ajouter aux favoris"))
          SendFavoriteToggle(it.index, it.favorite != 0);
        // alootid : ramassage auto par ID (via MoonlightUi, comme le bouton d'item_desc).
        if (auto* mui = Bourgeon::Instance().moonlight_ui()) {
          const bool inAloot = mui->IsAlootId(it.id);
          if (ImGui::MenuItem(inAloot ? "Retirer de l'alootid" : "Ajouter à l'alootid")) {
            if (inAloot) mui->RemoveAlootId(it.id); else mui->AddAlootId(it.id);
          }
        }
        ImGui::Separator();
        // Jeter (au sol) — grisé si le verrou drop (bouton footer) est actif.
        const bool dropLocked = ReadLock(kDropLockGlobal);
        if (dropLocked) ImGui::BeginDisabled();
        if (it.amount <= 1) {
          if (dropLocked) 
            ImGui::MenuItem("Jeter - verrouillé");
          else
            if (ImGui::MenuItem("Jeter")) SendDrop(it.index, 1);
        }
        else
          if (dropLocked) 
            ImGui::MenuItem("Jeter - verrouillé");
          else if (ImGui::MenuItem("Jeter...")) {
            pend_id_ = it.index; pend_index_ = it.index; pend_max_ = it.amount;
            pend_action_ = kPendDrop; pend_open_prompt_ = true;
          }
        if (dropLocked) ImGui::EndDisabled();
        // Transferts (si la fenêtre cible est ouverte).
        if (CartOpen() || StorageOpen()) ImGui::Separator();
        if (CartOpen() && ImGui::MenuItem("Vers le chariot"))
          SendCmd(kCmdToCart, it.index, it.amount);
        if (StorageOpen() && ImGui::MenuItem("Vers le storage"))
          SendCmd(kCmdToStorage, it.index, it.amount);
        ImGui::EndPopup();
      }

      ImGui::PopID();
    }
    ImGui::PopStyleVar();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();  // WindowPadding (grille)

  // Onglet actif "mange" le bord entre le strip et la grille : petit pont sur son bord
  // droit -> passage continu = souligne l'onglet actif. Couleur = corps de l'onglet
  // actif : BLANC pour la plupart, mais D1DCE8 (gris-bleu) pour Favoris (dont l'image a
  // ce corps) -> le pont se fond au lieu de trancher en blanc.
  if (haveActiveTab) {
    const ImU32 pont = kCats[cur_tab_].fav ? IM_COL32(0xD1, 0xDC, 0xE8, 255)
                                           : IM_COL32(255, 255, 255, 255);
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(activeTabMax.x - 1.0f, activeTabMin.y + 1.0f),
        ImVec2(activeTabMax.x + 2.0f, activeTabMax.y - 1.0f),
        pont);
  }

  // ── Drag terminé : router selon la cible (chariot / storage / sol) ──
  if (drag_active_) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType("INV_ITEM")) {
      const ImVec2 m = ImGui::GetMousePos();
      drag_mx_ = m.x; drag_my_ = m.y;
    } else {
      int action = -1;
      if (drag_index_ > 0) {
        const bool over_self = drag_mx_ >= win_x_ && drag_my_ >= win_y_ &&
                               drag_mx_ < win_x_ + win_w_ && drag_my_ < win_y_ + win_h_;
        if (MouseOverEquip(drag_mx_, drag_my_)) {         // drop sur la fenêtre Équipement
          if (IsEquippable(drag_type_))
            UseOrEquip(drag_index_, drag_type_, drag_loc_, false);  // -> équiper (sinon no-op)
        }
        else if (MouseOverCart(drag_mx_, drag_my_))    action = kPendToCart;
        else if (MouseOverStorage(drag_mx_, drag_my_) ||
                 StorageViewerOver(drag_mx_, drag_my_)) action = kPendToStorage;
        else if (!over_self && !ImGui::GetIO().WantCaptureMouse && !ReadLock(kDropLockGlobal))
          action = kPendDrop;  // verrou drop actif -> pas de jet au sol
      }
      if (action != -1) {
        pend_id_ = drag_index_; pend_index_ = drag_index_;
        pend_max_ = drag_amount_ > 0 ? drag_amount_ : 1;
        pend_action_ = action;
        pend_open_prompt_ = (pend_max_ > 1);
      }
      drag_active_ = false;
    }
  }

  // ── Footer : barre 3-slice (btnbar_left3/mid3/right3.bmp) portant poids / items /
  //    zeny. Épinglée au bas de la fenêtre en coords ABSOLUES (hors flux ImGui), donc
  //    jamais recouverte par la grille (footerH réservé dans BeginChild ci-dessus).
  const FooterVals fv = ReadFooterVals();
  const int maxSlots = fv.expansion + kInvBase;
  const int pct = fv.wmax > 0
                      ? static_cast<int>(static_cast<long long>(fv.wcur) * 100 / fv.wmax)
                      : 0;
  const bool over = fv.wmax > 0 && pct >= fv.overPct;

  const ImVec2 fwp = ImGui::GetWindowPos(), fws = ImGui::GetWindowSize();
  const float fx0 = fwp.x + style.WindowPadding.x;
  const float fx1 = fwp.x + fws.x - style.WindowPadding.x;
  const float fy1 = fwp.y + fws.y - style.WindowPadding.y;
  const float fy0 = fy1 - footerH;
  ImDrawList* fdl = ImGui::GetWindowDrawList();
  DrawFooterBar(fdl, fx0, fy0, fx1, footerH);

  // Deux lignes avec les icônes NATIVES du footer : ligne 1 = icône poids + "cur/max
  // (pct%)" (zeny à droite) ; ligne 2 = icône compteur + "N/max".
  const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
  const ImU32 colOver = IM_COL32(230, 60, 60, 255);
  const float th = ImGui::GetTextLineHeight();
  const float cy1 = fy0 + footerH * 0.25f;  // centre ligne 1
  const float cy2 = fy0 + footerH * 0.75f;  // centre ligne 2
  // ── Groupe de boutons NATIFS (bmp \inventory\...) sur la LIGNE 2 (celle du compteur),
  //    collé à droite, ordre natif G->D : Drop (verrou drop, TOUS onglets), puis Deal
  //    (verrou vente NPC) + Tri (vue) sur les Favoris. Sur la ligne 2 -> le zeny garde
  //    TOUTE la ligne 1 (plus poussé sur le poids). idx1 = état actif.
  const bool favTab = kCats[cur_tab_].fav;
  const bool dropOn = ReadLock(kDropLockGlobal);
  const bool dealOn = ReadLock(kDealLockGlobal);
  auto bwidth = [](const BarTex& on, const BarTex& off, bool a) -> float {
    const BarTex& t = a ? on : off;
    return (t.tex && t.w > 0) ? static_cast<float>(t.w) : 18.0f;
  };
  auto bheight = [](const BarTex& on, const BarTex& off, bool a) -> float {
    const BarTex& t = a ? on : off;
    return (t.tex && t.h > 0) ? static_cast<float>(t.h) : 18.0f;
  };
  const float bgap = 4.0f, gripM = 16.0f;
  const float wDrop = bwidth(g_btn_drop[1], g_btn_drop[0], dropOn);
  const float wDeal = favTab ? bwidth(g_btn_deal[1], g_btn_deal[0], dealOn) : 0.0f;
  const float wSort = favTab ? bwidth(g_btn_sort[1], g_btn_sort[0], sort_enabled_) : 0.0f;
  const float groupW = wDrop + (favTab ? wDeal + wSort + 2.0f * bgap : 0.0f);
  const float grpL = fx1 - gripM - groupW;   // bord gauche du groupe
  // Centré verticalement sur la ligne 2 (cy2), mais borné dans la barre (les bmps natifs
  // ~20px peuvent dépasser une demi-ligne -> on remonte juste assez pour ne pas clipper).
  float maxBtnH = bheight(g_btn_drop[1], g_btn_drop[0], dropOn);
  if (favTab) {
    const float hDeal = bheight(g_btn_deal[1], g_btn_deal[0], dealOn);
    const float hSort = bheight(g_btn_sort[1], g_btn_sort[0], sort_enabled_);
    if (hDeal > maxBtnH) maxBtnH = hDeal;
    if (hSort > maxBtnH) maxBtnH = hSort;
  }
  const float halfH = maxBtnH * 0.5f;
  float cyc = cy2;
  if (cyc + halfH > fy1 - 1.0f) cyc = fy1 - 1.0f - halfH;
  if (cyc - halfH < fy0 + 1.0f) cyc = fy0 + 1.0f + halfH;

  // Ligne 1 : poids (icône + valeur) à gauche, zeny à droite (ligne 1 entièrement libre).
  char wbuf[48];
  std::snprintf(wbuf, sizeof(wbuf), "%d/%d (%d%%)", fv.wcur, fv.wmax, pct);
  float x = fx0 + 6.0f;
  x += DrawFooterIcon(fdl, g_ico_weight, x, cy1) + 3.0f;
  fdl->AddText(ImVec2(x, cy1 - th * 0.5f), over ? colOver : colText, wbuf);
  char zbuf[40];
  reinterpret_cast<FmtComma_t>(kFmtComma)(fv.zeny, zbuf, sizeof(zbuf));
  char zline[56];
  std::snprintf(zline, sizeof(zline), "%sz", zbuf);
  const float zw = ImGui::CalcTextSize(zline).x;
  fdl->AddText(ImVec2(fx1 - gripM - zw, cy1 - th * 0.5f), colText, zline);  // plein droite
  // Ligne 2 : compteur d'items (icône + N/max).
  char cbuf[32];
  std::snprintf(cbuf, sizeof(cbuf), "%d/%d", item_count_ + WornItemCount(), maxSlots);
  x = fx0 + 6.0f;
  x += DrawFooterIcon(fdl, g_ico_num, x, cy2) + 3.0f;
  fdl->AddText(ImVec2(x, cy2 - th * 0.5f), colText, cbuf);

  // Boutons (dessinés par-dessus la barre) : Drop, puis Deal + Tri sur les Favoris.
  float bx = grpL, bwOut = 0.0f;
  if (FooterImgToggle("##inv_droplock", bx, cyc, g_btn_drop[1], g_btn_drop[0], dropOn, "D",
                      "Verrou drop : empêche de jeter des items (tous onglets)", &bwOut))
    ToggleLock(kDropLockGlobal);
  bx += bwOut + bgap;
  if (favTab) {
    if (FooterImgToggle("##inv_deallock", bx, cyc, g_btn_deal[1], g_btn_deal[0], dealOn, "V",
                        "Verrou vente : les favoris ne peuvent pas être vendus aux NPC", &bwOut))
      ToggleLock(kDealLockGlobal);
    bx += bwOut + bgap;
    if (FooterImgToggle("##inv_sort", bx, cyc, g_btn_sort[1], g_btn_sort[0], sort_enabled_, "T",
                        "Trier la vue (type puis nom) ; sinon ordre d'inventaire", &bwOut))
      sort_enabled_ = !sort_enabled_;
  }

  // Icône du drag NATIF au-dessus du viewer : le jeu rend l'icône du drag AVANT l'overlay
  // ImGui -> elle passe DERRIÈRE le viewer (ex. équip glissé depuis la fenêtre Équipement).
  // Drag natif entrant = bouton gauche tenu + clic NON parti du viewer (sinon = notre drag
  // ImGui) + pas de payload ImGui. Redessinée sur le curseur en taille tuile.
  void* dobj = (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !mousedown_over_viewer_ &&
                ImGui::GetDragDropPayload() == nullptr) ? Dispatcher() : nullptr;
  if (dobj) {
    const uint32_t did = ReadDragItemId(dobj);
    const ImVec2 m = ImGui::GetMousePos();
    const bool over = m.x >= win_x_ && m.y >= win_y_ &&
                      m.x < win_x_ + win_w_ && m.y < win_y_ + win_h_;
    if (did != 0 && over) {
      const IconTex ic = ResolveIcon(did, 1);
      if (ic.tex && ic.w > 0 && ic.h > 0) {
        const float ih = 24.0f, iw = ih * static_cast<float>(ic.w) / ic.h;
        ImGui::GetForegroundDrawList()->AddImage(
            TexId(ic.tex), ImVec2(m.x - iw * 0.5f, m.y - ih * 0.5f),
            ImVec2(m.x + iw * 0.5f, m.y + ih * 0.5f));
      }
    }
  }

  ro::EndRoWindow();
}
