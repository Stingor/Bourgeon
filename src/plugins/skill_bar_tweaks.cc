#include "plugins/skill_bar_tweaks.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"
#include "plugins/moonlight_ui.h"  // grille d'alignement partagée (grid_.SnapAxis)
#include "plugins/inventory_viewer.h"  // DraggedItemNameId (drag inventaire -> case de barre)
#include "d3d9/d3d9_hook.h"
#include "imgui.h"
#include "plugins/imgui_escape.h"
#include "utils/log_console.h"

#pragma comment(lib, "winmm.lib")  // timeGetTime (matche l'horloge cooldown du jeu)

// ===========================================================================
// Barre d'action ImGui remplaçant UIShortCutWnd (id 0x24). DESSIN PUR (pas de
// texture du jeu). 20250716 client / Moonlight-Destiny.exe, base 0x400000
// (no-ASLR : adresses Ghidra == live). RE : memory project_shortcut_bar_re.md.
//
// Invariant clé (validé par revue adverse) : DESSIN et ACTIVATION partagent la
// même source d'index. On lit chaque slot via this+0xc4[i] (pointeur vers le
// record 7 octets dans les globals -> données toujours fraîches) et on active
// par OnMsg(0,0x29,i%9,i/9) (la voie exacte des touches F1-F9). this+0xc4 est
// reconstruit par OnMsg(0x17) au moment où l'on cache la barre + au changement
// d'onglet ; les pointeurs restant valides, les données se rafraîchissent seules.
// ===========================================================================

namespace {

// ---- gestionnaire de fenêtres / instance singleton -------------------------
constexpr uintptr_t kUIWindowMgr    = 0x0131f4e8;  // g_UIWindowMgr
constexpr int       kMgrShortCutPtr = 0x1e8;       // mgr+0x1e8 = instance UIShortCutWnd cachée
constexpr uintptr_t kMakeWindow     = 0x00a39340;  // UIWindowMgr_MakeWindow(mgr, id) -> wnd (idempotent)
constexpr int       kShortCutId     = 0x24;

// ---- skill manager / données slots -----------------------------------------
constexpr uintptr_t kSkillInfoMgr = 0x015fa3c0;    // g_SkillInfoMgr (this des setters)
constexpr uintptr_t kSetShortCut  = 0x00d96c20;    // SkillMgr_SetShortCutSlot
constexpr uintptr_t kGetOption    = 0x008e1d50;    // SkillInfoMgr_GetOption(mgr,key) ; key10=onglet courant
constexpr uintptr_t kSetOption    = 0x005c5950;    // SkillMgr_SetOption(mgr,key,val,0) ; key5=UI-lock (≥1 bloque OnMsg 0x29)

// ---- vtable / champs UIShortCutWnd -----------------------------------------
constexpr int kVfSetVisible = 0x38;   // FUN_009030c0(this, vis) -> this+0x28
constexpr int kVfOnMsg      = 0x94;   // UIShortCutWnd::OnMsg
constexpr int kSlotArr      = 0xc4;   // this+0xc4 = 36 pointeurs de record
constexpr int kVisFlag      = 0x28;   // flag visibilité (0=caché) écrit par vtable+0x38
constexpr int kMsgUseSlot   = 0x29;   // OnMsg : active le slot (p3=col, p4=row)
constexpr int kMsgRebuild   = 0x17;   // OnMsg : reconstruit this+0xc4 depuis les globals
constexpr int kMaxSlots     = 36;     // 0x24

// ---- description / tooltip clic-droit (réplique UIShortCutWnd OnRButtonDown 0x008f91a0) ----
constexpr uintptr_t kGetSkillInfo  = 0x00d5a980;  // SkillMgr_GetSkillInfo(mgr,out,id,gate) ; out+4!=0 => trouvé
constexpr uintptr_t kStrFree       = 0x004f08f0;  // libère une std::string MSVC (ecx=base)
constexpr uintptr_t kCloseWindow   = 0x00a2e770;  // UIWindowMgr_Close(mgr,id)
constexpr int kWinSkillDesc   = 0xc;   // fenêtre description (skill : OnMsg 0x18 + &SkillInfo)
constexpr int kWinItemDesc    = 0x2e;  // fenêtre tooltip objet (OnMsg 0x3d + nameid brut)
constexpr int kMsgSetSkill    = 0x18;  // OnMsg fenêtre 0xc : montre le skill (p3=&SkillInfo)
constexpr int kMsgSetItem     = 0x3d;  // OnMsg fenêtre 0x2e : montre l'objet (p3=nameid)
constexpr int kItemWinShownId = 0x104; // (fenêtre 0x2e)+0x104 = nameid affiché (bascule ouvrir/fermer)
constexpr int kVfSetPos       = 0x10;  // vtable+0x10 = SetPos(x,y)
constexpr int kSkillInfoSize  = 0x100; // SkillInfo ~0xf8 o (2 std::string @ +0x2c / +0x44)
constexpr int kSkillInfoFound = 0x04;  // out+0x04 != 0 => skill trouvé
constexpr int kSkillStr0      = 0x2c;  // std::string resname
constexpr int kSkillStr1      = 0x44;  // std::string nom

// ItemSkillInfo standalone (ctor+SetId par id, INDÉPENDANT de l'inventaire courant — au contraire
// de kGetSkillInfo/FUN_00d5a980 qui exige une quantité inventaire > 0). Utilisé pour la description
// d'un OBJET grisé (épuisé) dans la barre : cf. project_item_skill_desc_window_re, section
// "Accès STANDALONE". +0x5c = flag skill (laissé à 0 = objet par le ctor / FUN_006a5ff0).
constexpr uintptr_t kItemSkillInfoCtor  = 0x006a1b20;  // ItemSkillInfo_ctor(this) __fastcall
constexpr uintptr_t kItemSkillInfoSetId = 0x006a6570;  // ItemSkillInfo_SetId(this,id) __thiscall (itoa->resname)
constexpr int kItemSkillInfoSize = 0x100;  // struct ~0xf4 o

// ---- tooltip survol (réplique UIShortCutWnd OnMouseMove 0x008f7f50) ----
// OBJET (rec[0]==1) : nom via la DB item (FUN_006a0d40, table 0x01255130 ; record+0x04 = nom EN,
//   +0x08 = localisé). Les objets y sont chargés au boot (FUN_006a4e20 parse item.txt etc.).
// SKILL (rec[0]==0) : les ids de skills NE SONT PAS dans la DB item — VÉRIFIÉ live (traversée de
//   l'arbre RB de 0x01255130 : clé 29 Inc AGI introuvable ; la DB ne contient que des objets). Le
//   nom vient de Lua GetSkillName(id) via le wrapper __cdecl FUN_0073a1f0 (format "d>s", renvoie
//   "Unknown-Skill" si l'id est inconnu). C'est exactement la source de la barre native pour les
//   skills standard (les skills custom ~12622 sont, eux, aussi dans la DB item).
constexpr uintptr_t kItemDbGet      = 0x006a0d40;  // ItemDB_GetRecordById(__cdecl nameid, table)
constexpr uintptr_t kItemDbTable    = 0x01255130;  // table std::map du DB item (arg)
constexpr uintptr_t kItemDbSentinel = 0x01255138;  // retour si id inconnu -> NE PAS déréf
constexpr int kItemNameEn  = 0x04;  // record+0x04 = nom anglais (ASCII, propre à l'affichage)
constexpr int kItemNameLoc = 0x08;  // record+0x08 = nom localisé (CP949, repli)
constexpr uintptr_t kGetSkillNameLua = 0x0073a1f0;  // char* GetSkillName(int id) (__cdecl, via Lua)

// ---- cooldown (g_ShortCutCooldownList 0x015ff7e0) --------------------------
constexpr uintptr_t kCooldownList  = 0x015ff7e0;   // sentinelle (la liste EST la sentinelle)
constexpr uintptr_t kUseGameClock  = 0x015beecc;   // 0 -> timeGetTime(), sinon horloge jeu
constexpr uintptr_t kGameClockGet  = 0x00b1fac0;   // -> ptr, DWORD horloge à +0x20

using OnMsg_t      = int   (__fastcall*)(void*, void*, int, int, int, int, int, int);
using SetVisible_t = void  (__fastcall*)(void*, void*, int);
using MakeWindow_t = void* (__fastcall*)(void*, void*, void*);
using SetSlot_t    = void  (__fastcall*)(void*, void*, int, int, int, int, int);
using GetOption_t  = int   (__thiscall*)(void*, int);
using SetOption_t  = void  (__thiscall*)(void*, int, int, int);  // (mgr,key,val,0)
using GameClock_t  = void* (__cdecl*)();
using GetSkillInfo_t = void (__fastcall*)(void*, void*, void*, int, int);  // (mgr,edx,out,id,gate)
using StrFree_t      = void (__fastcall*)(void*);                          // (ecx=std::string base)
using CloseWin_t     = char (__fastcall*)(void*, void*, int);             // (mgr,edx,id)
using SetPos_t       = void (__fastcall*)(void*, void*, int, int);        // (wnd,edx,x,y)
using ShowTip_t      = void (__fastcall*)(void*, void*, const char*, int, int, unsigned, unsigned char, char);
using ItemDbGet_t    = void* (__cdecl*)(int, void*);                       // (nameid, table) -> record / sentinelle
using GetSkillNameLua_t = char* (__cdecl*)(int);                          // GetSkillName(id) -> nom skill / "Unknown-Skill"
using ItemSkillInfoCtor_t  = void* (__fastcall*)(void*);                  // (ecx=this) -> this
using ItemSkillInfoSetId_t = void  (__thiscall*)(void*, int);             // (this, id)

struct CooldownNode {       // 0x24 octets
  CooldownNode* next;       // +0x00
  CooldownNode* prev;       // +0x04
  uint32_t      skillId;    // +0x08
  uint32_t      endTick;    // +0x0C = clock + duration
  uint32_t      duration;   // +0x10 (ms)
  // ... listes d'animation imbriquées +0x14.. (inutiles ici)
};

struct SlotRec { bool valid; uint8_t type; uint32_t id; int16_t level; };

inline void* ShortCutWnd() {
  return *reinterpret_cast<void**>(kUIWindowMgr + kMgrShortCutPtr);
}
template <typename Fn>
inline Fn Vf(void* self, int off) {
  return reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t**>(self))[off / 4]);
}

void EnsureCreated() {
  if (ShortCutWnd()) return;  // déjà créée (cas normal) ou recréée ci-dessous
  reinterpret_cast<MakeWindow_t>(kMakeWindow)(
      reinterpret_cast<void*>(kUIWindowMgr), nullptr,
      reinterpret_cast<void*>(kShortCutId));
}
void RebuildSlotPtrs(void* w) {  // OnMsg 0x17 : reconstruit this+0xc4 depuis les globals
  Vf<OnMsg_t>(w, kVfOnMsg)(w, nullptr, 0, kMsgRebuild, 0, 0, 0, 0);
}
void HideNative(void* w) {  // this+0x28 = 0 + délink draw-list ; ne détruit PAS l'objet
  Vf<SetVisible_t>(w, kVfSetVisible)(w, nullptr, 0);
}
void ShowNative(void* w) {
  Vf<SetVisible_t>(w, kVfSetVisible)(w, nullptr, 1);
}
int CurrentTab() {
  return reinterpret_cast<GetOption_t>(kGetOption)(
             reinterpret_cast<void*>(kSkillInfoMgr), 10) ? 1 : 0;
}

// ── 3 régions natives = 3 barres fixes (index de barre == index de région) ───
// LECTURE DIRECTE des stores globals (record 7 o : +0 type, +1 id dword, +5 level short), au lieu
// de this+0xc4 (qui ne tient que l'onglet ACTIF) -> on affiche les 3 régions simultanément.
//   region 0 = Onglet 1 (g_ShortCutSlots_Tab0), 36 slots, onglet 0, catégorie hotkey 0
//   region 1 = Onglet 2 (g_ShortCutSlots_Tab1), 36 slots, onglet 1, catégorie hotkey 3
//   region 2 = Items    -> STORE PLUGIN g_itemStore (le natif g_ShortCutItemSlotExt ne fait que 9 ;
//              nos slots d'items n'ont pas de raccourci clavier & sont persistés client -> on en offre
//              kItemSlotMax). Records au MÊME format 7o -> tout le code base+i*7 marche à l'identique.
struct RegionDef { uintptr_t base; int count; int tab; int hotkeyCat; const char* name; };
uint8_t g_itemStore[SkillBarTweaks::kItemSlotMax * 7] = {};  // records 7o (type/id/level), plugin-owned
void WriteSlotRecord(int region, int slot, bool is_item, uint32_t id, int level);  // fwd (défini plus bas)
const RegionDef kRegions[3] = {  // const (pas constexpr) : la base items = adresse runtime de g_itemStore
    {0x015fa850, 36,  0,  0, "Onglet 1"},
    {0x015fa94c, 36,  1,  3, "Onglet 2"},
    {reinterpret_cast<uintptr_t>(g_itemStore), SkillBarTweaks::kItemSlotMax, -1, -1, "Items"},
};
inline bool RegionIsItems(int r) { return r == 2; }

// Assignation/vidage d'un slot d'ITEM : SkillMgr_SetShortCutItemSlot(mgr, id, slot) (__thiscall ;
// id==0 vide ; écrit g_ShortCutItemSlotExt, type=0/objet). Use objet (réplique branche objet de
// OnMsg 0x29) : dispatcher (*(0x0121333c)) ->vtable[0x18](0x71, info, count, 0, 0) (__thiscall).
using SetItemSlot_t = void  (__thiscall*)(void*, int, int);
using DispUse_t     = void  (__thiscall*)(void*, int, void*, int, int, int);
using GetInvItemU_t = void* (__stdcall*)(void*, int);   // 0x00d7fa90 (out, id) ; found out+0x04, qty out+0x10
constexpr uintptr_t kSetItemSlot    = 0x00da8f90;
constexpr uintptr_t kUICmdDisp      = 0x0121333c;        // *(void**) = g_UICommandDispatcher
constexpr uintptr_t kGetInvItemAddr = 0x00d7fa90;

// Lit le slot logique i de la région `region` directement dans le store global (record 7 o). SEH.
SlotRec ReadSlot(int region, int i) {
  SlotRec s{false, 0, 0, 0};
  if (region < 0 || region >= 3) return s;
  const RegionDef& rg = kRegions[region];
  if (i < 0 || i >= rg.count) return s;
  __try {
    uint8_t* rec = reinterpret_cast<uint8_t*>(rg.base) + i * 7;
    uint32_t id;
    std::memcpy(&id, rec + 1, 4);  // id NON aligné dans le record packé
    if (id != 0) {                 // slot vide (le natif teste id!=0)
      int16_t lvl;
      std::memcpy(&lvl, rec + 5, 2);
      s = {true, rec[0], id, lvl};
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { s = {false, 0, 0, 0}; }
  return s;
}

// Use d'un slot d'ITEM. La barre native d'items NE passe PAS par OnMsg 0x29 (records en this+0x160,
// pas this+0xc4). Plutôt que répliquer la branche objet (les getters/cmds sont piégés par les noms
// Ghidra inversés -> on prenait la branche SKILL : 0x00d7fa90(607)=found 0), on DÉTOURNE temporairement
// this+0xc4[0] vers le record d'item et on appelle le VRAI OnMsg 0x29 (col=0,row=0) : le natif exécute
// sa branche objet complète (bon getter + type + dispatch), comme un objet posé dans le grid. La branche
// objet ne lit PAS rec[5] -> rec[5]=0 des items OK. Gate option #5 neutralisé le temps de l'appel. SEH.
void UseItemSlot(int slot) {
  void* w = ShortCutWnd();
  if (!w) return;
  __try {
    uint8_t* itemRec = reinterpret_cast<uint8_t*>(kRegions[2].base) + slot * 7;
    uint32_t id;
    std::memcpy(&id, itemRec + 1, 4);
    if (id == 0) return;
    void* mgr = reinterpret_cast<void*>(kSkillInfoMgr);
    const int lock = reinterpret_cast<GetOption_t>(kGetOption)(mgr, 5);
    if (lock >= 1) reinterpret_cast<SetOption_t>(kSetOption)(mgr, 5, 0, 0);
    uint8_t** arr = reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(w) + kSlotArr);  // this+0xc4
    uint8_t* saved = arr[0];
    arr[0] = itemRec;                                                  // détourne le slot 0 -> item
    Vf<OnMsg_t>(w, kVfOnMsg)(w, nullptr, 0, kMsgUseSlot, 0, 0, 0, 0);  // OnMsg 0x29 col=0,row=0 -> arr[0]
    arr[0] = saved;                                                    // restaure
    if (lock >= 1) reinterpret_cast<SetOption_t>(kSetOption)(mgr, 5, lock, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Active le slot `slot` de la région `region`. SKILLS (onglets) : exactement comme une touche F
// (OnMsg 0x29), MAIS 0x29 ne lit que this+0xc4 = onglet ACTIF -> on bascule temporairement sur
// l'onglet de la barre (SetOption 10 + OnMsg 0x17) puis on restaure. ITEMS : UseItemSlot.
// Gate option #5 (UI-lock natif, >=1 bloque) neutralisé le temps de l'appel puis restauré. SEH (Lua/jeu).
void ActivateSlot(int region, int slot) {
  if (RegionIsItems(region)) { UseItemSlot(slot); return; }
  void* w = ShortCutWnd();
  if (!w) return;
  void* mgr = reinterpret_cast<void*>(kSkillInfoMgr);
  const int tab = kRegions[region].tab;
  const int cur = CurrentTab();
  const int lock = reinterpret_cast<GetOption_t>(kGetOption)(mgr, 5);
  __try {
    if (lock >= 1) reinterpret_cast<SetOption_t>(kSetOption)(mgr, 5, 0, 0);
    if (tab != cur) {  // bascule sur l'onglet de la barre + reconstruit this+0xc4
      reinterpret_cast<SetOption_t>(kSetOption)(mgr, 10, tab, 0);
      Vf<OnMsg_t>(w, kVfOnMsg)(w, nullptr, 0, kMsgRebuild, 0, 0, 0, 0);
    }
    Vf<OnMsg_t>(w, kVfOnMsg)(w, nullptr, 0, kMsgUseSlot, slot % 9, slot / 9, 0, 0);
    if (tab != cur) {  // restaure l'onglet d'origine
      reinterpret_cast<SetOption_t>(kSetOption)(mgr, 10, cur, 0);
      Vf<OnMsg_t>(w, kVfOnMsg)(w, nullptr, 0, kMsgRebuild, 0, 0, 0, 0);
    }
    if (lock >= 1) reinterpret_cast<SetOption_t>(kSetOption)(mgr, 5, lock, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Notifie le serveur d'un changement de raccourci -> PERSISTANCE. Sur ce serveur
// (moonlight/rAthena) les barres sont sauvées côté serveur : le client envoie
// CZ_SHORTCUT_KEY_CHANGE2 (0x0b21, RE>=20190508) à chaque changement, le serveur met
// à jour sd->status.hotkeys[index+tab*MAX_HOTKEYS] (sauvé au save perso). Sans cet
// envoi, nos écritures ne survivent pas (la barre est rechargée du serveur au login).
// Format : [op u16][tab u16][index u16][isSkill u8 = type][id u32][count u16 = level] (13 o).
void SendHotkeyChange(int tab, int index, uint8_t type, uint32_t id, int level) {
#pragma pack(push, 1)
  struct {
    uint16_t op; uint16_t tab; uint16_t index;
    uint8_t isSkill; uint32_t id; uint16_t count;
  } p;
#pragma pack(pop)
  p.op = 0x0b21;
  p.tab = static_cast<uint16_t>(tab);
  p.index = static_cast<uint16_t>(index);
  p.isSkill = type;            // record type BRUT (NATIF : 0=SKILL, 1=OBJET) ; round-trip serveur (chargt = copie brute)
  p.id = id;
  p.count = static_cast<uint16_t>(level);
  Bourgeon::Instance().SendPacket(reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}

// Vide le slot i de la région (skills -> SetShortCutSlot + persist serveur ; items -> store plugin).
void ClearSlot(int region, int i) {
  void* mgr = reinterpret_cast<void*>(kSkillInfoMgr);
  if (RegionIsItems(region)) {
    WriteSlotRecord(region, i, /*is_item*/ true, 0, 0);  // vide le store plugin (persist client)
    return;
  }
  const int tab = kRegions[region].tab;
  reinterpret_cast<SetSlot_t>(kSetShortCut)(mgr, nullptr, 0, 0, 0, i, tab);
  SendHotkeyChange(tab, i, 0, 0, 0);
}
// Écrit un slot (id==0 => efface). Skills via SkillMgr_SetShortCutSlot (+ persist serveur) ; items
// dans le store plugin (type/level ignorés : un slot d'item ne porte qu'un nameid, type=0).
void SetSlot(int region, int i, uint8_t type, uint32_t id, int level) {
  void* mgr = reinterpret_cast<void*>(kSkillInfoMgr);
  if (RegionIsItems(region)) {
    WriteSlotRecord(region, i, /*is_item*/ true, id, 0);  // écrit le store plugin (persist client)
    return;
  }
  const int tab = kRegions[region].tab;
  reinterpret_cast<SetSlot_t>(kSetShortCut)(
      mgr, nullptr, static_cast<int>(type), static_cast<int>(id), level, i, tab);
  SendHotkeyChange(tab, i, type, id, level);
}
// Échange le contenu de deux slots de la MÊME région (ou déplace vers un slot vide) = réarrangement.
void MoveSlot(int region, int src, int dst) {
  if (src == dst || src < 0 || dst < 0) return;
  const SlotRec a = ReadSlot(region, src);
  const SlotRec b = ReadSlot(region, dst);
  SetSlot(region, dst, a.type, a.id, a.level);  // a.id==0 si vide -> efface dst
  SetSlot(region, src, b.type, b.id, b.level);
}
// Déplace/échange entre DEUX régions (drag INTER-onglets : Onglet 1 <-> Onglet 2, ou vers/depuis la
// barre d'items). ⚠️ la barre d'items (region 2) ne reçoit QUE des objets -> un skill vers/depuis
// items est refusé. Skills = SetShortCutSlot(tab) ; items = SetShortCutItemSlot (via SetSlot).
void MoveSlotCross(int srcRegion, int srcSlot, int dstRegion, int dstSlot) {
  if (srcRegion == dstRegion) { MoveSlot(srcRegion, srcSlot, dstSlot); return; }
  const SlotRec a = ReadSlot(srcRegion, srcSlot);  // source
  const SlotRec b = ReadSlot(dstRegion, dstSlot);  // cible (échangée vers la source)
  if (RegionIsItems(dstRegion) && a.valid && a.type != 0) return;  // skill -> barre d'items : interdit
  if (RegionIsItems(srcRegion) && b.valid && b.type != 0) return;  // (échange) skill -> items : interdit
  SetSlot(dstRegion, dstSlot, a.type, a.id, a.level);  // a.id==0 => vide dst
  SetSlot(srcRegion, srcSlot, b.type, b.id, b.level);  // b.id==0 => vide src (déplacement simple)
}

// Quantité LIVE d'un OBJET en inventaire — comme la barre native OnDraw (branche OBJET) : le getter
// unifié ItemSkillMgr_GetInfoByResId_UNIFIED (0x00d5a980, ex-"GetSkillInfo") par nameid -> info+0x04
// trouvé, info+0x10 = quantité courante. Interrogé À CHAQUE FRAME -> décrémente à l'usage, jamais figé.
// (Le natif ne STOCKE PAS le count ; nous non plus -> plus de corruption 940->255 à la persistance.) SEH.
int GetItemLiveCount(uint32_t nameid) {
  int cnt = 0;
  __try {
    alignas(8) uint8_t info[kSkillInfoSize] = {};
    reinterpret_cast<GetSkillInfo_t>(kGetSkillInfo)(
        reinterpret_cast<void*>(kSkillInfoMgr), nullptr, info, static_cast<int>(nameid), 1);
    if (*reinterpret_cast<int*>(info + kSkillInfoFound) != 0)
      cnt = *reinterpret_cast<int*>(info + 0x10);
    reinterpret_cast<StrFree_t>(kStrFree)(info + kSkillStr1);
    reinterpret_cast<StrFree_t>(kStrFree)(info + kSkillStr0);
  } __except (EXCEPTION_EXECUTE_HANDLER) { cnt = 0; }
  return cnt;
}

// ---- étiquette de touche réelle par slot (lue depuis UserKeys.lua) ----------
// Le client expose le getter Lua "GetHotKey" via le wrapper natif 0x00d80950 :
//   GetHotKey(out, category, slot)   (__stdcall, RET 0xc). C'est EXACTEMENT la source du
//   tooltip de la barre native (UIShortCutWnd_OnMouseMove 0x008f7f50, branche GetOption(10)) :
//     category : onglet -> 0 = SkillBar_1Tab, 3 = SkillBar_2Tab  (= GetOption(mgr,10) ? 3 : 0)
//     slot     : index natif du slot (colonne + ligne*9)
//   Remplit out (le wrapper ré-initialise les 2 std::string lui-même) :
//     out+0x00 = keycode, out+0x08 = std::string nom de touche ("F1","A","Shift+F1"...),
//     out+0x20 = std::string 2e touche.
//   Source unique = UserKeys.lua -> touche RÉELLE (rebinds inclus) ET layout-aware (AZERTY/
//   QWERTY, car le nom vient du jeu). Les 2 std::string créées par le wrapper DOIVENT être
//   détruites (kStrFree, sinon fuite si nom > 15 car.). SEH : on touche Lua + globals.
constexpr uintptr_t kGetHotKey = 0x00d80950;
using GetHotKey_t = void* (__stdcall*)(void* out, int category, int slot);

// Copie une std::string MSVC vers dst (base = objet string ; +0x10 = size, +0x14 = cap ;
// data inline si cap<0x10, sinon pointeur). dst NUL-terminé, tronqué à n.
void CopyMsvcString(const uint8_t* base, char* dst, int n) {
  if (n < 1) return;
  dst[0] = '\0';
  const uint32_t size = *reinterpret_cast<const uint32_t*>(base + 0x10);
  const uint32_t cap  = *reinterpret_cast<const uint32_t*>(base + 0x14);
  const char* s = (cap >= 0x10) ? *reinterpret_cast<const char* const*>(base)
                                : reinterpret_cast<const char*>(base);
  if (!s || size == 0) return;
  const int len = size < static_cast<uint32_t>(n - 1) ? static_cast<int>(size) : n - 1;
  std::memcpy(dst, s, len);
  dst[len] = '\0';
}

// Étiquette de touche d'un slot, lue depuis UserKeys.lua (touche réelle, layout-aware). out>=2.
// category = catégorie GetHotKey (0=Onglet1, 3=Onglet2) ; <0 (items) => pas d'étiquette.
void GetSlotKeyLabel(int category, int slot, char* out, int n) {
  out[0] = '\0';
  if (category < 0) return;
  __try {
    alignas(4) uint8_t buf[0x40];
    std::memset(buf, 0, sizeof(buf));
    reinterpret_cast<GetHotKey_t>(kGetHotKey)(buf, category, slot);
    CopyMsvcString(buf + 0x08, out, n);                  // out+0x08 = nom de la touche
    reinterpret_cast<StrFree_t>(kStrFree)(buf + 0x08);   // détruit les 2 std::string du wrapper
    reinterpret_cast<StrFree_t>(kStrFree)(buf + 0x20);
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// Keycodes bruts d'un slot (out+0x00 = touche, out+0x04 = 2e touche / modificateur). false si vide.
// Sert à intercepter au clavier les touches d'une barre d'onglet non-active (le natif ne sert que
// l'onglet actif). SEH (Lua). category<0 (items) -> pas de touche.
bool GetSlotKeyCodes(int category, int slot, int* kc1, int* kc2) {
  *kc1 = 0; *kc2 = 0;
  if (category < 0) return false;
  bool ok = false;
  __try {
    alignas(4) uint8_t buf[0x40];
    std::memset(buf, 0, sizeof(buf));
    reinterpret_cast<GetHotKey_t>(kGetHotKey)(buf, category, slot);
    *kc1 = *reinterpret_cast<int*>(buf + 0x00);
    *kc2 = *reinterpret_cast<int*>(buf + 0x04);
    reinterpret_cast<StrFree_t>(kStrFree)(buf + 0x08);
    reinterpret_cast<StrFree_t>(kStrFree)(buf + 0x20);
    ok = (*kc1 != 0 || *kc2 != 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

// ── Cache clavier : (touche principale + modificateur) -> slot, par région (0/1). Évite un scan Lua
// GetHotKey à CHAQUE frappe (jusqu'à 72 appels). Reconstruit périodiquement (capte les rebinds). ──
struct SlotKeyBind { int mainK; int modK; int slot; };
std::vector<SlotKeyBind> g_keyCache[2];
DWORD g_keyCacheTick = 0;
void RebuildKeyCache() {
  auto isMod = [](int k) { return k == VK_CONTROL || k == VK_SHIFT || k == VK_MENU; };
  for (int region = 0; region < 2; ++region) {
    g_keyCache[region].clear();
    const int cat = kRegions[region].hotkeyCat;
    for (int k = 0; k < kRegions[region].count; ++k) {
      int kc1, kc2;
      if (!GetSlotKeyCodes(cat, k, &kc1, &kc2)) continue;
      int mainK = kc1, modK = 0;
      if (isMod(kc1)) { modK = kc1; mainK = kc2; }
      else if (isMod(kc2)) { modK = kc2; mainK = kc1; }
      if (mainK != 0) g_keyCache[region].push_back({mainK, modK, k});
    }
  }
}

DWORD ShortCutNow() {  // même sélection d'horloge que OnDraw/OnMsg 0x8f
  if (*reinterpret_cast<int*>(kUseGameClock) == 0) return timeGetTime();
  void* clk = reinterpret_cast<GameClock_t>(kGameClockGet)();
  return clk ? *reinterpret_cast<DWORD*>(reinterpret_cast<uint8_t*>(clk) + 0x20)
             : timeGetTime();
}
// Fraction de cooldown restante [0,1] (0 = prêt). Match exact par id (les
// pseudo-ids de groupe 0x241/9999 = polish v2).
//
// IMPORTANT : 0x015ff7e0 est l'OBJET std::list MSVC, pas la sentinelle. La
// sentinelle (_Myhead, sur le tas) = *(0x015ff7e0). Le natif (OnDraw) itère
// `head=*(0x015ff7e0); n=head->next; tant que n!=head`. Comparer contre
// l'adresse 0x015ff7e0 (mon 1er bug) bouclait à l'infini (freeze). Garde
// d'itérations + SEH = anti-freeze/anti-crash définitif.
float CooldownFraction(uint32_t skillId) {
  __try {
    CooldownNode* head = *reinterpret_cast<CooldownNode**>(kCooldownList);  // _Myhead
    if (!head) return 0.0f;
    const DWORD now = ShortCutNow();
    int guard = 0;
    for (CooldownNode* n = head->next; n && n != head && guard < 256;
         n = n->next, ++guard) {
      if (n->skillId != skillId) continue;
      if (n->duration == 0) return 0.0f;
      const int remain = static_cast<int>(n->endTick - now);   // unsigned-safe
      if (remain <= 0) return 0.0f;                            // expiré
      DWORD r = static_cast<DWORD>(remain);
      if (r > n->duration) r = n->duration;
      return static_cast<float>(r) / static_cast<float>(n->duration);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return 0.0f;
}

// ── Icônes (recette menu_icons.cc : TexMgr -> BGRA -> Overlay_CreateTextureARGB) ──
constexpr uintptr_t kTexMgr  = 0x00a90350;
constexpr uintptr_t kMakeKey = 0x00a9f030;
constexpr uintptr_t kLoadTex = 0x00a8d4a0;
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;
const char kUIDir[] = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA";  // CP949 유저인터페이스
using TexMgr_t  = void* (__cdecl*)();
using MakeKey_t = void* (__cdecl*)(const char*);
using LoadTex_t = void* (__fastcall*)(void*, void*, void*);
// Chemin d'icône construit par fonctions natives (RE) :
//   OBJET : BuildItemIconGrfPath 0x00d5a720 __stdcall(id_str, out) -> "유저인터페이스\item\<resname>.bmp"
//           (resname via ResolveItemResNameById/DB objets -> marche même HORS inventaire). C'est la clé :
//           les .bmp du GRF sont nommés par resource-name CP949, pas par id.
//   SKILL : Lua_GetSkillIdName 0x0073a140 __cdecl(id) -> identifiant du skill (ex. "AL_BLESSING"),
//           puis "유저인터페이스\item\<idname>.bmp". C'est la source d'icône NATIVE (le builder d'effet
//           0x00bda890 fait le même sprintf) et surtout INDÉPENDANTE de l'état APPRIS : elle lit la DB
//           Lua SkillInfoList, pas la liste apprise g_SkillInfoMgr+0xc. (L'ancien getter 0x00d7fa90 lit
//           la liste apprise via FUN_00737e00 -> resname VIDE pour un skill d'une AUTRE classe -> icône
//           perdue après relog sur un perso GM multi-classe. Le nom du slot survivait car il vient de
//           GetSkillName Lua, elle aussi indép. de l'appris.) Repli sur 0x00d7fa90 si idname invalide.
using GetInvInfo_t = void* (__stdcall*)(void*, int);          // 0x00d7fa90 (out, id) — liste APPRISE
using BuildPath_t  = void  (__stdcall*)(const char*, char*);  // 0x00d5a720 (id_str, out_path[>=260])
using GetSkillIdNameLua_t = char* (__cdecl*)(int);            // 0x0073a140 GetSkillIdName(id) -> idname
constexpr uintptr_t kGetSkillIdNameLua = 0x0073a140;

std::unordered_map<uint32_t, void*> g_iconCache;  // (type0?hi:0)|id -> ImTextureID (null=miss connu)

// Resname sentinelle quand l'id n'a pas de ressource (0x00d7fa90 -> "Unknown-Skill",
// 0x00d5a720 -> "...unknown item...", GetSkillIdName -> "Zero Skill"). Tenter de charger ces .bmp
// échoue et SPAMME la console ("Resource File Loading fail"). On les rejette -> repli propre sur la
// boîte-id. ("nknown" couvre Unknown/unknown sans souci de casse de l'initiale.)
inline bool LooksUnknown(const char* s) {
  return s && (std::strstr(s, "nknown") != nullptr || std::strcmp(s, "Zero Skill") == 0);
}

// Chemins SEH-protégés (aucun objet C++ -> SEH OK), écrits dans out (taille >=260).
bool ItemPath(int id, char* out, int /*n*/) {
  __try {
    char idstr[16];
    std::snprintf(idstr, sizeof(idstr), "%d", id);
    out[0] = '\0';
    reinterpret_cast<BuildPath_t>(0x00d5a720)(idstr, out);  // écrit le chemin complet dans out
    if (LooksUnknown(out)) { out[0] = '\0'; return false; }  // pas de ressource -> ne pas charger
    return out[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
}
bool SkillPath(int id, char* out, int n) {
  __try {
    // 1) Source d'icône NATIVE, indépendante de l'état appris : Lua GetSkillIdName(id) -> identifiant
    //    (ex. "AL_BLESSING"). Marche pour un skill d'une AUTRE classe (perso GM multi-classe) alors que
    //    l'ancien getter renvoyait vide -> icône perdue après relog. Voir le bloc de commentaire ci-dessus.
    const char* idn = reinterpret_cast<GetSkillIdNameLua_t>(kGetSkillIdNameLua)(id);
    if (idn && idn[0] && !LooksUnknown(idn)) {                      // rejette "Zero Skill"/"Unknown"
      std::snprintf(out, n, "%s\\item\\%s.bmp", kUIDir, idn);
      return out[0] != '\0';
    }
    // 2) Repli : ancien getter 0x00d7fa90 (resname de la liste APPRISE à +0x20). Utile si GetSkillIdName
    //    ne couvre pas un id custom mais que le skill est appris.
    alignas(8) uint8_t info[0xA0] = {};
    reinterpret_cast<GetInvInfo_t>(0x00d7fa90)(info, id);          // __stdcall(out, id)
    const char* rn = *reinterpret_cast<const char**>(info + 0x20);  // resname (déréférencé)
    if (rn && rn[0] && !LooksUnknown(rn)) {                         // rejette "Unknown-Skill"
      std::snprintf(out, n, "%s\\item\\%s.bmp", kUIDir, rn);
      return out[0] != '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return false;
}
void* UploadBmp(const char* path) {
  void* mgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
  if (!mgr) return nullptr;
  void* key = reinterpret_cast<MakeKey_t>(kMakeKey)(path);
  if (!key) return nullptr;
  void* t = reinterpret_cast<LoadTex_t>(kLoadTex)(mgr, nullptr, key);
  if (!t) return nullptr;
  const int w = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexW);
  const int h = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexH);
  const uint8_t* bgra = *reinterpret_cast<const uint8_t**>(static_cast<char*>(t) + kTexPix);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !bgra) return nullptr;
  std::vector<uint8_t> argb(static_cast<size_t>(w) * h * 4);
  for (int i = 0; i < w * h; ++i) {
    const uint8_t b = bgra[i * 4], g = bgra[i * 4 + 1], r = bgra[i * 4 + 2];
    const bool ck = (r == 0xFF && g == 0 && b == 0xFF);  // colorkey magenta
    argb[i * 4] = b; argb[i * 4 + 1] = g; argb[i * 4 + 2] = r; argb[i * 4 + 3] = ck ? 0 : 0xFF;
  }
  return Overlay_CreateTextureARGB(argb.data(), w, h);
}
// type0=skill, type1=objet ; essaie la source prioritaire selon le type puis l'autre
// (robuste à l'ambiguïté du type). Fail-safe : null mis en cache (boîte id) si échec.
void* GetIconTex(uint8_t type, uint32_t id) {
  // CONVENTION NATIVE CERTIFIÉE (OnDrop 0x008dd70b + SetShortCutSlot + tooltip read) :
  // type/rec[0] == 0 => SKILL, == 1 => OBJET. Source d'icône par type : SKILL ->
  // SkillPath (0x00d7fa90 ItemMgr_GetInvItemById, gère aussi les plages d'ids skills) ;
  // OBJET -> ItemPath (0x00d5a720 BuildItemIconGrfPath). On tente la bonne source en
  // 1er ; fallback 2-passes + garde LooksUnknown pour les cas limites.
  // Textures D3DPOOL_DEFAULT : mortes après reset/recréation du device -> vider.
  static unsigned s_epoch = 0;
  const unsigned dev_e = Overlay_DeviceEpoch();
  if (dev_e != s_epoch) { g_iconCache.clear(); s_epoch = dev_e; }
  const uint32_t k = (type == 0 ? 0x80000000u : 0u) | (id & 0x7fffffffu);
  auto it = g_iconCache.find(k);
  if (it != g_iconCache.end()) return it->second;
  char path[300] = {};
  void* tex = nullptr;
  const bool item_first = (type == 0);  // OBJET (type0) -> ItemPath d'abord ; SKILL (type1) -> SkillPath
  for (int pass = 0; pass < 2 && !tex; ++pass) {
    const bool try_item = (pass == 0) ? item_first : !item_first;
    const bool ok = try_item ? ItemPath(static_cast<int>(id), path, sizeof(path))
                             : SkillPath(static_cast<int>(id), path, sizeof(path));
    if (ok) tex = UploadBmp(path);
  }
  g_iconCache[k] = tex;
  return tex;
}
void FlushIconCache() { g_iconCache.clear(); }  // changement de zone (textures fuient, négligeable)

// Le joueur connaît-il / peut-il utiliser ce skill ? Réplique le test de rendu natif (OnDraw, branche
// skill rec0!=0) : ItemMgr_GetInvItemById(0x00d7fa90)(out, id) -> out+0x04 (found) != 0 = connu/
// utilisable (un skill lié à un item disparu repasse à found=0 -> le natif cesse de le dessiner).
// Nettoyage via FUN_00739cd0 (même signature __fastcall(void*) que StrFree_t) -> pas de fuite malgré
// l'appel par frame. SEH (POD only). id = id canonique stocké (rec+1), comme OnDraw.
bool SkillKnown(uint32_t id) {
  bool known = false;
  __try {
    alignas(8) uint8_t info[0xC0] = {};
    reinterpret_cast<GetInvInfo_t>(0x00d7fa90)(info, static_cast<int>(id));
    known = (*reinterpret_cast<int*>(info + 0x04) != 0);
    reinterpret_cast<StrFree_t>(0x00739cd0)(info);  // = FUN_00739cd0 (cleanup de la struct)
  } __except (EXCEPTION_EXECUTE_HANDLER) { known = false; }
  return known;
}

// ---- pont drag NATIF (grimoire/inventaire) -> slot ImGui (RE drag-drop) -----
// FUN_00a75340(0x1213338) renvoie l'objet de drag en cours (gate +0x58==1) ou null.
// Charge (== OnDrop param_3) à objet+0x308 : +0x00 catégorie, +0x80 OCTET DE FORMAT, +0x04 id,
// +0x14 srcSlot (-1 si source externe), +0x18 nameid (FullPayload), +0x08 count (FullPayload),
// +0x6c count/level (LitePayload).
// ⚠️ L'octet +0x80 N'EST PAS "skill/objet" mais le FORMAT du payload (DragDropMgr_BeginDrag_*) :
//   octet 0 = FullPayload  -> inventaire/cart/stockage = OBJET (nameid @+0x18, count @+0x08)
//   octet 1 = LitePayload  -> grimoire de skills       = SKILL (id @+0x04, level @+0x6c)
// (Les re-drags de la barre inversent ce mapping mais ont srcSlot>=0 -> rejetés par HandleNativeDrop.)
//   OBJET : id = nameid @+0x18 (repli ItemMgr_GetInvItemById+0x8) ; level = count.
//   SKILL : id = atoi(SkillInfo+0x2c) via FUN_00d5aa40 (repli rawid) ; level = count LitePayload.
using GetDragObj_t  = void* (__fastcall*)(void*);                    // FUN_00a75340(mgr)
using LookupSkill_t = void  (__fastcall*)(void*, void*, void*, int); // FUN_00d5aa40(mgr,edx,out,id)
using SkillId_t     = int   (__fastcall*)(void*);                    // FUN_005d98a0(&info) -> skill id
constexpr uintptr_t kDragMgr      = 0x1213338;
constexpr uintptr_t kGetDragObj   = 0x00a75340;
constexpr uintptr_t kLookupSkill  = 0x00d5aa40;
constexpr uintptr_t kSkillIdAtoi  = 0x005d98a0;
constexpr uintptr_t kGetInvItem   = 0x00d7fa90;  // ItemMgr_GetInvItemById(out,id) ; nameid=out+0x8
constexpr int kPayloadOff = 0x308;
constexpr int kPL_type = 0x80, kPL_id = 0x04, kPL_src = 0x14, kPL_cnt = 0x6c;
constexpr int kPL_cat = 0x00;      // catégorie/source du drag
constexpr int kPL_nameid = 0x18;   // FullPayload : nameid objet (param_2[0])
constexpr int kPL_fullcnt = 0x08;  // FullPayload : count objet (param_2[5])

struct NativeDrag { bool isItem; uint32_t id; int level; int srcSlot; };

// c_str d'une std::string MSVC à p+off (SSO : si cap@+0x14 > 0xf -> heap *(char**), sinon inline).
inline const char* PayloadStr(uint8_t* p, int off) {
  const uint32_t cap = *reinterpret_cast<uint32_t*>(p + off + 0x14);
  return (cap > 0xf) ? *reinterpret_cast<char**>(p + off) : reinterpret_cast<const char*>(p + off);
}

inline void* DragObj() {  // objet de drag en cours, ou null
  return reinterpret_cast<GetDragObj_t>(kGetDragObj)(reinterpret_cast<void*>(kDragMgr));
}
// Décode la charge -> {isItem, id assignable, level, srcSlot}. SEH-protégé.
bool DecodeDrag(void* obj, NativeDrag* d) {
  __try {
    uint8_t* p = reinterpret_cast<uint8_t*>(obj) + kPayloadOff;
    const int rawid = *reinterpret_cast<int*>(p + kPL_id);
    if (rawid == 0) return false;
    // octet 0 = FullPayload (inventaire => OBJET) ; octet 1 = LitePayload (grimoire => SKILL).
    d->isItem  = (p[kPL_type] == 0);
    d->srcSlot = *reinterpret_cast<int*>(p + kPL_src);
    if (d->isItem) {
      // ID OBJET = atoi(resname BRUT @payload+0x3c) = nameid ; replis GetInvItemById(raw)+0x08 puis raw.
      // (payload+0x24 = resname transformé pour l'icône ; +0x08 = INDEX inventaire -> repli seulement.)
      int resId = 0, invIdx = 0;
      { const char* s = PayloadStr(p, 0x3c); resId = (s && s[0]) ? std::atoi(s) : 0; }
      { alignas(8) uint8_t inv[0xC0] = {};
        reinterpret_cast<GetInvInfo_t>(kGetInvItem)(inv, rawid);
        invIdx = *reinterpret_cast<int*>(inv + 0x08); }
      d->id    = static_cast<uint32_t>(resId != 0 ? resId : (invIdx != 0 ? invIdx : rawid));
      d->level = 0;  // OBJET : count NON stocké -> affiché LIVE via GetItemLiveCount (évite corruption 940->255)
    } else {
      // SKILL (LitePayload grimoire) : id = raw04 (payload+0x04) = l'id skill du grimoire. C'est ce que
      // la barre NATIVE stocke aussi (vérifié par capture live : Angelus -> rec0=1, id=33). Convention
      // record : rec[0]=is_item?0:1 -> SKILL=1. Le cast (OnMsg 0x29) et la desc passent par les handlers
      // NATIFS qui branchent sur rec[0]=1 -> tout marche directement (pas de conversion d'id nécessaire).
      d->id    = static_cast<uint32_t>(rawid);
      d->level = *reinterpret_cast<int*>(p + kPL_cnt);       // LitePayload count = niveau skill
    }
    return d->id != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Annule le drag natif en VIDANT la charge (dragobj+0x308) -> au release le jeu ne
// voit plus d'objet -> pas de drop au sol, et le curseur-suiveur natif disparaît.
// On NE touche PAS le gate 0x1213390 : ce n'est pas un flag de drag mais l'état du
// dispatcher (FUN_00a75340) que le jeu requiert (le mettre à 0 -> null-deref dans
// SkillMgr_SetShortCutSlot -> crash, cf. 3 crashes documentés). SEH (POD only).
void CancelNativeDrag(void* dragobj) {
  __try {
    uint8_t* p = reinterpret_cast<uint8_t*>(dragobj) + kPayloadOff;
    *reinterpret_cast<int*>(p + 0x00)   = 0;  // catégorie
    *reinterpret_cast<int*>(p + kPL_id) = 0;  // id
    p[kPL_type] = 0;                          // type
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Écrit le record 7 octets (type/id/level) DIRECTEMENT dans le store global de la région
// (= là où ReadSlot lit). AUCUN appel de fonction du jeu (SkillMgr_SetShortCutSlot notifie le
// dispatcher 0x139 -> crash pendant un drag) ; ReadSlot voit la donnée au frame suivant.
// CONVENTION NATIVE (capture live: Angelus rec0=1) : type 0=OBJET, 1=SKILL. Items = toujours OBJET. SEH.
void WriteSlotRecord(int region, int slot, bool is_item, uint32_t id, int level) {
  if (region < 0 || region >= 3) return;
  __try {
    uint8_t* rec = reinterpret_cast<uint8_t*>(kRegions[region].base) + slot * 7;
    rec[0] = is_item ? 0 : 1;
    std::memcpy(rec + 1, &id, 4);
    const int16_t lv = static_cast<int16_t>(level);
    std::memcpy(rec + 5, &lv, 2);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Clic-droit sur un slot -> ouvre la description en jeu, RÉPLIQUE EXACTE du handler
// natif UIShortCutWnd::OnRButtonDown 0x008f91a0 (par index de slot, sans le HitTest) :
//   SKILL (rec[0]==1) : fenêtre tooltip 0x2e ; bascule si elle montre déjà ce id
//     (w+0x104), sinon OnMsg(0x3d, id brut). Positionne au curseur.
//   OBJET (rec[0]==0) : SkillMgr_GetSkillInfo (kGetSkillInfo, gate=1) remplit une struct
//     (2 std::string) ET amorce le cache de la DB desc (0x01255130) pour cet id ; sans cet
//     appel, OnMsg(0x18) sur la fenêtre 0xc ouvre bien le nom/icône mais les lignes de
//     description restent VIDES (testé : struct maison ItemSkillInfo_ctor/SetId sans ce
//     gate = titre OK, desc vide). Le natif ne procède QUE si trouvé (out+4, = quantité
//     inventaire > 0) -> case OBJET GRISÉE (stack épuisé) jamais décrite. FIX : on ignore
//     ce garde-fou (le natif l'utilise pour la quantité, pas pour la desc) et on ouvre la
//     fenêtre 0xc + OnMsg(0x18, &struct) dans tous les cas -> desc dispo même à 0 en stock.
// OnMsg 0x18 COPIE la struct (sûr de libérer après). À n'appeler QUE hors drag natif
// (clic-droit simple) — appeler une fn jeu pendant un drag crashe. SEH (POD only).
void OpenSlotDescription(int region, int slot, int mx, int my) {
  if (region < 0 || region >= 3) return;
  __try {
    uint8_t* rec = reinterpret_cast<uint8_t*>(kRegions[region].base) + slot * 7;
    const int id = *reinterpret_cast<int*>(rec + 1);
    if (id != 0) {
      void* mgr = reinterpret_cast<void*>(kUIWindowMgr);
      if (rec[0] != 0) {  // ── SKILL (rec[0]==1) ──
        void* wnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
            mgr, nullptr, reinterpret_cast<void*>(kWinItemDesc));
        if (wnd) {
          if (*reinterpret_cast<int*>(reinterpret_cast<char*>(wnd) + kItemWinShownId) == id) {
            reinterpret_cast<CloseWin_t>(kCloseWindow)(mgr, nullptr, kWinItemDesc);  // bascule
          } else {
            Vf<OnMsg_t>(wnd, kVfOnMsg)(wnd, nullptr, 0, kMsgSetItem, id, 0, 0, 0);
            Vf<SetPos_t>(wnd, kVfSetPos)(wnd, nullptr, mx, my);
          }
        }
      } else {            // ── OBJET (rec[0]==0) ──
        alignas(8) uint8_t info[kSkillInfoSize] = {};
        reinterpret_cast<GetSkillInfo_t>(kGetSkillInfo)(
            reinterpret_cast<void*>(kSkillInfoMgr), nullptr, info, id, 1);
        void* wnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
            mgr, nullptr, reinterpret_cast<void*>(kWinSkillDesc));
        if (wnd) {
          Vf<OnMsg_t>(wnd, kVfOnMsg)(wnd, nullptr, 0, kMsgSetSkill,
                                     static_cast<int>(reinterpret_cast<uintptr_t>(info)), 0, 0, 0);
          Vf<SetPos_t>(wnd, kVfSetPos)(wnd, nullptr, mx, my);
        }
        reinterpret_cast<StrFree_t>(kStrFree)(info + kSkillStr1);
        reinterpret_cast<StrFree_t>(kStrFree)(info + kSkillStr0);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Survol d'un slot rempli -> tooltip ImGui avec le nom (réplique le NOM de la barre native
// UIShortCutWnd::OnMouseMove 0x008f7f50) : OBJET = DB item (FUN_006a0d40) ; SKILL = Lua
// GetSkillName(id) (FUN_0073a1f0) car les ids skills sont absents de la DB item (prouvé par
// traversée RB live de 0x01255130). Rendu via ImGui::SetTooltip (le tooltip natif FUN_00a753d0
// passait SOUS la barre ImGui + 1 frame de retard). SEH (POD only ; SetTooltip hors __try).
void ShowSlotTooltip(int region, int slot) {
  char nm[160] = {};  // nom extrait (POD)
  bool valid = false, is_item = false;
  int id = 0, level = 0;
  if (region < 0 || region >= 3) return;
  __try {
    uint8_t* rec = reinterpret_cast<uint8_t*>(kRegions[region].base) + slot * 7;
    {
      id = *reinterpret_cast<int*>(rec + 1);
      if (id != 0) {
        valid = true;
        is_item = (rec[0] == 0);  // NATIF (capture live) : rec[0]==0 => OBJET, ==1 => SKILL
        int16_t lv; std::memcpy(&lv, rec + 5, 2); level = lv;
        if (is_item) {
          // OBJET : DB item (chargée au boot) ; record+0x04 = nom EN, +0x08 = localisé.
          char* dbrec = static_cast<char*>(
              reinterpret_cast<ItemDbGet_t>(kItemDbGet)(id, reinterpret_cast<void*>(kItemDbTable)));
          if (dbrec && reinterpret_cast<uintptr_t>(dbrec) != kItemDbSentinel) {
            const char* en  = *reinterpret_cast<const char**>(dbrec + kItemNameEn);
            const char* loc = *reinterpret_cast<const char**>(dbrec + kItemNameLoc);
            const char* name = (en && *en) ? en : loc;
            if (name && *name) std::snprintf(nm, sizeof(nm), "%s", name);
          }
        } else {
          // SKILL : nom via Lua GetSkillName(id) — les ids skills sont absents de la DB item (les
          // skills custom y sont parfois, mais GetSkillName couvre TOUT, source que la fenêtre de
          // skills/le tooltip natif utilisent). "" ou "Unknown-Skill" => repli ci-dessous.
          const char* sn = reinterpret_cast<GetSkillNameLua_t>(kGetSkillNameLua)(id);
          if (sn && *sn && std::strcmp(sn, "Unknown-Skill") != 0)
            std::snprintf(nm, sizeof(nm), "%s", sn);
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { valid = false; }
  if (!valid) return;
  if (!nm[0]) std::snprintf(nm, sizeof(nm), "%s", is_item ? "Objet" : "Skill");  // repli si nom absent
  // Format : objet = "Nom (ID: n)" ; skill = "Nom - Lv: l (ID: n)". Rendu ImGui (au-dessus de tout ;
  // le tooltip natif FUN_00a753d0 passait sous la barre + 1 frame de retard).
  char out[224];
  if (is_item) std::snprintf(out, sizeof(out), "%s (ID: %d)", nm, id);
  else         std::snprintf(out, sizeof(out), "%s - Lv: %d (ID: %d)", nm, level, id);
  ImGui::SetTooltip("%s", out);
}

// Callback ImGui : applique le filtre texture choisi (POINT net / LINEAR flou)
// aux dessins suivants de la barre. Réglage miroité ici pour le callback.
bool g_sb_bilinear = false;
void SbApplyFilter(const ImDrawList*, const ImDrawCmd*) { Overlay_SetTextureFilter(g_sb_bilinear); }

// Sélecteur de couleur = même pattern que le fond du chat (swatch -> popup ColorPicker4).
// Renvoie true si la couleur a changé (pour marquer la config à persister).
bool ColorSwatch(const char* label, float col[4]) {
  bool changed = false;
  ImGui::PushID(label);
  if (ImGui::ColorButton("##sw", ImVec4(col[0], col[1], col[2], col[3]),
                         ImGuiColorEditFlags_AlphaPreview, ImVec2(20, 20)))
    ImGui::OpenPopup("pick");
  ImGui::SameLine();
  ImGui::TextUnformatted(label);
  if (ImGui::BeginPopup("pick")) {
    changed = ImGui::ColorPicker4("##p", col,
                        ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview);
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

// Sliders ajustables à la molette (à l'unité) quand survolés.
bool WheelInt(const char* label, int* v, int mn, int mx) {
  bool ch = ImGui::SliderInt(label, v, mn, mx);
  if (ImGui::IsItemHovered()) {
    const float wh = ImGui::GetIO().MouseWheel;
    if (wh != 0.0f) {
      int n = static_cast<int>(wh);
      if (n == 0) n = (wh > 0.0f) ? 1 : -1;  // touchpad fractionnaire -> +/-1
      *v = std::clamp(*v + n, mn, mx);
      ch = true;
    }
  }
  return ch;
}
bool WheelFloat(const char* label, float* v, float mn, float mx, const char* fmt) {
  bool ch = ImGui::SliderFloat(label, v, mn, mx, fmt);
  if (ImGui::IsItemHovered()) {
    const float wh = ImGui::GetIO().MouseWheel;
    if (wh != 0.0f) {
      int n = static_cast<int>(wh);
      if (n == 0) n = (wh > 0.0f) ? 1 : -1;
      *v = std::clamp(*v + static_cast<float>(n), mn, mx);  // pas de 1 unité
      ch = true;
    }
  }
  return ch;
}

}  // namespace

void SkillBarTweaks::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
  if (!in_game_) { native_hidden_ = false; last_tab_ = -1; items_restored_ = false; }
  FlushIconCache();  // recharge les icônes au changement de zone
}

// Lit les slots d'items du store plugin (g_itemStore) -> item_slots_ (pour la sauvegarde yaml). SEH.
// Garde : avant la restauration en jeu le store est vide -> on garde la valeur chargée du yaml.
void SkillBarTweaks::SnapshotItemSlots() {
  if (!in_game_ || !items_restored_) return;
  __try {
    for (int i = 0; i < kItemSlotMax; ++i) {
      uint8_t* rec = reinterpret_cast<uint8_t*>(kRegions[2].base) + i * 7;
      uint32_t id;
      std::memcpy(&id, rec + 1, 4);
      item_slots_[i] = id;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkillBarTweaks::OnKeyDown(unsigned long vkey, int, int) {
  if (!enabled_ || !native_hidden_ || !in_game_) return;

  // Le natif ne dispatche qu'à l'onglet ACTIF (DispatchHotkeyBehavior : behavior<0x2d -> singleton
  // this+0x1e8 via l'onglet actif ; les 2 tables UserKeys.lua = jeux de touches ALTERNATIFS par onglet).
  // On route les touches vers l'autre onglet (visible) selon la logique de l'utilisateur, pour gérer
  // AUSSI les touches partagées (F2-F9) : si l'onglet ACTIF a un slot OCCUPÉ pour cette touche -> on
  // laisse le natif (priorité onglet 1 ; couvre "les 2 occupés"). Sinon, si l'onglet non-actif visible
  // a un slot OCCUPÉ pour cette touche -> on l'active. Aucun occupé -> rien. Cache anti-lag (Lua).
  const DWORD now = GetTickCount();
  if (now - g_keyCacheTick > 1500 || (g_keyCache[0].empty() && g_keyCache[1].empty())) {
    RebuildKeyCache();
    g_keyCacheTick = now;
  }
  auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
  int pressedMod = 0;  // un seul modificateur (priorité Ctrl > Alt > Shift ; combos multiples rares)
  if (held(VK_CONTROL)) pressedMod = VK_CONTROL;
  else if (held(VK_MENU)) pressedMod = VK_MENU;
  else if (held(VK_SHIFT)) pressedMod = VK_SHIFT;
  // Slot OCCUPÉ de la région lié à (vkey, pressedMod), ou -1.
  auto occupiedSlot = [&](int region) -> int {
    for (const SlotKeyBind& b : g_keyCache[region])
      if (b.mainK == static_cast<int>(vkey) && b.modK == pressedMod && ReadSlot(region, b.slot).valid)
        return b.slot;
    return -1;
  };
  const int cur = CurrentTab();
  if (occupiedSlot(cur) != -1) return;  // onglet actif occupé pour cette touche -> le natif s'en charge
  const int other = 1 - cur;
  if (!bars_[other].visible) return;
  const int s = occupiedSlot(other);
  if (s != -1) ActivateSlot(other, s);
}

void SkillBarTweaks::OnRenderUI() {
  if (!in_game_) return;

  void* w = ShortCutWnd();

  // ── Bascule cacher / restaurer la barre native selon l'activation ──────────
  const bool want_hidden = enabled_ &&
      Bourgeon::Instance().client().session().aid() != 0;
  if (want_hidden) {
    EnsureCreated();
    w = ShortCutWnd();
    if (w) {
      // Re-cacher CHAQUE fois que la native (ré)apparaît visible (relog, retour en
      // jeu, ré-affichage par le client) -> jamais de doublon natif + ImGui.
      // Idempotent : HideNative met this+0x28=0, donc pas de re-hide en boucle.
      const bool visible = *reinterpret_cast<uint8_t*>(
          reinterpret_cast<char*>(w) + kVisFlag) != 0;
      const int tab = CurrentTab();
      if (visible || tab != last_tab_) {  // (re)cache + resync this+0xc4
        if (visible) HideNative(w);
        RebuildSlotPtrs(w);
        last_tab_ = tab;
      }
      native_hidden_ = true;
      // Restaure 1×/session le contenu persisté de la barre d'items (store plugin g_itemStore ;
      // ni le serveur ni le natif ne le fournissent). WriteSlotRecord écrit g_itemStore.
      if (!items_restored_) {
        for (int i = 0; i < kItemSlotMax; ++i)
          if (item_slots_[i] != 0) WriteSlotRecord(2, i, /*is_item*/ true, item_slots_[i], 0);
        items_restored_ = true;
      }
    }
  } else if (native_hidden_) {
    if (w) {
      ShowNative(w);
      // Recovery: an earlier build wrongly pinned the native bar off-screen while
      // hidden, which persisted -10000 into its saved position (QUICKSLOTWNDINFO).
      // If we find it off-screen when re-showing, restore it to a sane on-screen
      // spot so it isn't lost. (One-off safety net; harmless once positions are ok.)
      int* nx = reinterpret_cast<int*>(reinterpret_cast<char*>(w) + 0x1c);
      int* ny = reinterpret_cast<int*>(reinterpret_cast<char*>(w) + 0x20);
      if (*nx < -5000 || *ny < -5000) { *nx = 200; *ny = 100; }
    }
    native_hidden_ = false;
  }

  // Le panneau de réglages vit désormais dans MoonlightUi (onglet "Barre d'action"), plus de
  // fenêtre flottante détachée (DrawPanel/²~ retirés).
  if (native_hidden_ && w) {
    for (int b = 0; b < kBarCount; ++b)
      if (bars_[b].visible) DrawBar(b);  // 3 barres fixes (0=Onglet1, 1=Onglet2, 2=Items)

    // Icône du drag NATIF (grimoire de skills / inventaire natif) redessinée AU-DESSUS des
    // barres : le jeu rend l'icône-curseur du drag AVANT l'overlay ImGui -> elle passe DERRIÈRE
    // nos cases dès qu'elle survole une barre. On la recolle sur le curseur (ForegroundDrawList,
    // au-dessus de tout) quand un drag natif est en cours ET survole une barre visible. Même
    // recette que InventoryViewer::OnRenderUI. On saute pendant un drag ImGui interne (payload
    // non-null : SBSLOT / INV_ITEM ont déjà leur propre aperçu).
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        ImGui::GetDragDropPayload() == nullptr) {
      void* obj = DragObj();
      NativeDrag d{};
      if (obj && DecodeDrag(obj, &d)) {
        const ImVec2 m = ImGui::GetMousePos();
        bool over = false;
        for (int b = 0; b < kBarCount && !over; ++b) {
          if (!bars_[b].visible) continue;
          const BarCfg& bc = bars_[b];
          const int cols  = std::max(1, bc.columns);
          const int count = std::min(bc.slot_count, kRegions[b].count);
          const int rows  = (count + cols - 1) / cols;
          const float step = bc.icon_size + bc.spacing;
          const float bw = cols * step - bc.spacing, bh = rows * step - bc.spacing;
          if (m.x >= bc.x && m.y >= bc.y && m.x < bc.x + bw && m.y < bc.y + bh) over = true;
        }
        if (over) {
          // GetIconTex : type 0 = OBJET, !=0 = SKILL (convention record). d.isItem -> objet.
          void* tex = GetIconTex(d.isItem ? 0 : 1, d.id);
          if (tex) {
            const float sz = 24.0f;  // taille bmp native d'icône (comme le redraw inventaire)
            ImGui::GetForegroundDrawList()->AddImage(
                (ImTextureID)(uintptr_t)tex,
                ImVec2(m.x - sz * 0.5f, m.y - sz * 0.5f),
                ImVec2(m.x + sz * 0.5f, m.y + sz * 0.5f));
          }
        }
      }
    }
  }
}

// Appelé par le hook WndProc au WM_LBUTTONUP (PRÉ-input). Si un drag natif est
// relâché sur une case de la barre : écriture directe du record + vidage de la
// charge, AVANT que le jeu ne traite le up. Le drag reste vivant pendant la
// traverse (pas de clic-au-sol) ; ici on le solde sur la case survolée. Aucune
// fonction du jeu appelée hors les getters de décodage (sûrs, cf. RE). Géométrie =
// celle de DrawBar (pad=0). (mx,my en coords client == coords écran ImGui.)
bool SkillBarTweaks::HandleNativeDrop(int mx, int my) {
  if (!enabled_ || !native_hidden_) return false;
  if (ImGui::GetDragDropPayload() != nullptr) return false;  // pas pendant un drag ImGui interne
  void* w = ShortCutWnd();
  if (!w) return false;
  void* obj = DragObj();
  if (!obj) return false;
  NativeDrag d{};
  if (!DecodeDrag(obj, &d) || d.srcSlot >= 0) return false;  // pas de self-drag de la barre
  (void)w;

  // Cherche LAQUELLE des barres visibles couvre (mx,my). Géométrie = celle de DrawBar (pad=0).
  for (int b = 0; b < kBarCount; ++b) {
    if (!bars_[b].visible) continue;
    const BarCfg& bc = bars_[b];
    const float step = bc.icon_size + bc.spacing;  // taille/espacement PAR barre
    const int cols = (bc.columns < 1) ? 1 : bc.columns;
    const float lx = static_cast<float>(mx) - static_cast<float>(bc.x);
    const float ly = static_cast<float>(my) - static_cast<float>(bc.y);
    if (lx < 0 || ly < 0) continue;
    const int cc = static_cast<int>(lx / step), cr = static_cast<int>(ly / step);
    if (cc >= cols) continue;
    if ((lx - cc * step) >= bc.icon_size || (ly - cr * step) >= bc.icon_size) continue;  // gap
    const int k = cr * cols + cc;
    if (k < 0 || k >= bc.slot_count) continue;
    const int slot = bc.first_slot + k;
    if (slot < 0 || slot >= kRegions[b].count) continue;
    if (RegionIsItems(b) && !d.isItem) continue;  // un slot d'item ne reçoit qu'un OBJET

    WriteSlotRecord(b, slot, d.isItem, d.id, d.level);  // écriture directe (sûr en drag, 0 appel jeu)
    if (!RegionIsItems(b))  // skills : persiste côté serveur (CZ_SHORTCUT_KEY_CHANGE)
      SendHotkeyChange(kRegions[b].tab, slot, d.isItem ? 0 : 1, d.id, d.level);
    else
      dirty_ = true;  // items : persistance CLIENT (yaml) -> déclenche une sauvegarde
    CancelNativeDrag(obj);
    return true;
  }
  return false;
}

// ---- panneau de configuration ----------------------------------------------
void SkillBarTweaks::DrawPanel() {
  ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
  bool open = true;
  if (!ImGui::Begin("Skill Bar###SkillBarPanel", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End(); if (!open) panel_visible_ = false; return;
  }
  bourgeon::CloseWindowOnEscape(open);
  if (!open) panel_visible_ = false;
  DrawSettingsContent();
  ImGui::End();
}

// ---- contenu des réglages (fenêtre standalone ²/~ ET onglet MoonlightUi "Barre d'action") -----
void SkillBarTweaks::DrawSettingsContent() {
  bool changed = false;
  changed |= ImGui::Checkbox("Activer (barres ImGui)", &enabled_);
  ImGui::SameLine();
  ImGui::BeginDisabled(!enabled_);
  ImGui::Checkbox("Verrouiller", &locked_);  // décoché = barre déplaçable ; transitoire, non persisté
  ImGui::SameLine(); ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Coche = barre fixe. Decoche = glisser n'importe ou pour la deplacer.\n"
                      "Verrouillee, les slots restent utilisables et rearrangeables.");
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!enabled_);
  // ── Réglages COMMUNS (taille/espacement sont PAR BARRE, dans chaque section ci-dessous) ──
  changed |= ImGui::Checkbox("Filtre bilineaire (flou)", &bilinear_);
  ImGui::SameLine(); ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Decoche = pixels nets (POINT). Coche = lissage ImGui (LINEAR).");
  changed |= ImGui::Checkbox("Clic-traversant (Shift = interagir)", &clickthrough_);
  changed |= ImGui::Checkbox("Afficher les touches", &show_keys_);
  changed |= ImGui::Checkbox("Texte gras", &bold_text_);  // faux-gras (touches + nombres)
  ImGui::TextDisabled("Aimantation : Reglages interface > \"Aimanter a la grille\" (grille commune a tout l'UI).");

  // ── 3 barres FIXES (jeu fixe) : Onglet 1 / Onglet 2 / Items ──
  ImGui::Separator();
  ImGui::TextDisabled("Barres");
  static const char* const kBarNames[kBarCount] = {"Onglet 1", "Onglet 2", "Items"};
  for (int b = 0; b < kBarCount; ++b) {
    BarCfg& bc = bars_[b];
    ImGui::PushID(b);
    bool vis = bc.visible;
    if (ImGui::Checkbox(kBarNames[b], &vis)) { bc.visible = vis; changed = true; }
    if (bc.visible && ImGui::TreeNode("cfg", "Reglages %s", kBarNames[b])) {
      changed |= WheelInt("Colonnes", &bc.columns, 1, 12);
      changed |= WheelInt("Nb slots", &bc.slot_count, 1, kRegions[b].count);
      changed |= WheelFloat("Taille", &bc.icon_size, 16.0f, 64.0f, "%.0f px");
      changed |= WheelFloat("Espacement", &bc.spacing, 0.0f, 12.0f, "%.0f px");
      changed |= WheelInt("X", &bc.x, -200, 4000);
      changed |= WheelInt("Y", &bc.y, -200, 4000);
      ImGui::Text("Position : %d, %d", bc.x, bc.y);
      ImGui::TreePop();
    }
    ImGui::PopID();
  }

  if (ImGui::CollapsingHeader("Aide : clavier & onglets")) {
    ImGui::TextWrapped(
        "Le jeu ne pilote qu'UN onglet au clavier a la fois. Bourgeon route les touches vers l'autre "
        "onglet visible :\n"
        "- Touche PARTAGEE (ex. F2 sur les 2 onglets) : la case OCCUPEE repond. Si les deux cases "
        "sont occupees, seul l'onglet actif repond (l'onglet 2 reste inerte pour cette touche).\n"
        "- Pour piloter l'Onglet 2 de facon dediee, rebinde ses cases sur des combos Ctrl+/Alt+ "
        "(touches uniques) dans les raccourcis clavier du jeu -> elles agissent toujours sur l'onglet 2.\n"
        "- La barre d'items n'a pas de raccourci clavier (clic gauche = utiliser).");
  }
  if (ImGui::CollapsingHeader("Couleurs")) {
    changed |= ColorSwatch("Fond du cadre", col_frame_);
    changed |= ColorSwatch("Fond objet", col_item_);
    changed |= ColorSwatch("Fond skill", col_skill_);
    changed |= ColorSwatch("Fond vide", col_empty_);
    changed |= ColorSwatch("Bordure", col_border_);
    changed |= ColorSwatch("Bordure survol", col_borderhi_);
    changed |= ColorSwatch("Texte touches", col_keytext_);
    changed |= ColorSwatch("Texte nombre (count/lv)", col_count_);
  }
  ImGui::EndDisabled();
  if (changed) dirty_ = true;  // persistance drainée par MoonlightUi

  ImGui::Separator();
  ImGui::TextDisabled("Clic G = utiliser. Glisser = rearranger. Clic D = description. Clic molette = vider.");
  ImGui::TextDisabled("Bleu = objet, Vert = skill. Items = barre d'objets. Toggle panneau : 2/~");
}

// ---- la barre d'action elle-même -------------------------------------------
void SkillBarTweaks::DrawBar(int bar) {
  if (bar < 0 || bar >= kBarCount) return;
  BarCfg& bc = bars_[bar];
  const float icon_size_ = bc.icon_size;  // taille/espacement PAR BARRE (locals = alias des champs bc)
  const float spacing_   = bc.spacing;
  const int region   = bar;                     // index de barre == région native
  const int maxSlots = kRegions[region].count;  // 36 (skills) / 9 (items)
  const int keyCat   = kRegions[region].hotkeyCat;
  const int cols  = std::max(1, bc.columns);
  const int count = std::min(bc.slot_count, maxSlots);
  const int rows  = (count + cols - 1) / cols;
  const float step = icon_size_ + spacing_;
  const float pad  = 0.0f;  // identique en édition et normal -> aucun décalage au changement de mode
  const float winw = cols * step - spacing_ + pad * 2;
  const float winh = rows * step - spacing_ + pad * 2;

  // Fenêtre toujours épinglée à (bc.x,bc.y) ; le déplacement en édition se
  // fait en glissant un slot (delta souris), comme menu_icons -> dessin et
  // hit-rect jamais désynchronisés.
  ImGui::SetNextWindowPos(ImVec2((float)bc.x, (float)bc.y), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(winw, winh), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;  // on dessine notre propre fond

  // Capture souris. Par défaut la barre intercepte ; on la rend "click-through"
  // (NoMouseInputs -> le WndProc laisse passer au jeu) dans 2 cas :
  //  1) option Clic-traversant active et Shift NON maintenu (toute la barre) ;
  //  2) verrouillée, hors drag ImGui, et le curseur n'est PAS sur une case REMPLIE
  //     (cases vides / espaces : inutile de capturer -> on peut cliquer le jeu ou
  //     fermer une fenêtre derrière). On garde la capture pendant un drag pour
  //     pouvoir déposer sur une case vide, et déverrouillée pour saisir la barre.
  const bool shift = ImGui::GetIO().KeyShift;
  bool over_filled = false;
  {
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const float lx = mp.x - static_cast<float>(bc.x), ly = mp.y - static_cast<float>(bc.y);
    if (lx >= 0 && ly >= 0) {
      const int cc = static_cast<int>(lx / step), cr = static_cast<int>(ly / step);
      if (cc < cols && (lx - cc * step) < icon_size_ && (ly - cr * step) < icon_size_) {
        const int k = cr * cols + cc;
        if (k >= 0 && k < count && bc.first_slot + k < maxSlots)
          over_filled = ReadSlot(region, bc.first_slot + k).valid;
      }
    }
  }
  bool no_input = false;
  if (clickthrough_ && !shift) no_input = true;  // option : toute la barre
  else if (locked_ && !over_filled && ImGui::GetDragDropPayload() == nullptr)
    no_input = true;                             // cases vides / espaces
  if (no_input) flags |= ImGuiWindowFlags_NoMouseInputs;

  char winName[32];
  std::snprintf(winName, sizeof(winName), "##SkillActionBar%d", bar);
  ImGui::Begin(winName, nullptr, flags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();

  const ImU32 cFrame  = ImGui::GetColorU32(ImVec4(col_frame_[0], col_frame_[1], col_frame_[2], col_frame_[3]));
  const ImU32 cSkill  = ImGui::GetColorU32(ImVec4(col_skill_[0], col_skill_[1], col_skill_[2], col_skill_[3]));
  const ImU32 cItem   = ImGui::GetColorU32(ImVec4(col_item_[0], col_item_[1], col_item_[2], col_item_[3]));
  const ImU32 cEmpty  = ImGui::GetColorU32(ImVec4(col_empty_[0], col_empty_[1], col_empty_[2], col_empty_[3]));
  const ImU32 cBorder = ImGui::GetColorU32(ImVec4(col_border_[0], col_border_[1], col_border_[2], col_border_[3]));
  const ImU32 cBordHi = ImGui::GetColorU32(ImVec4(col_borderhi_[0], col_borderhi_[1], col_borderhi_[2], col_borderhi_[3]));
  const ImU32 cKeyTxt = ImGui::GetColorU32(ImVec4(col_keytext_[0], col_keytext_[1], col_keytext_[2], col_keytext_[3]));
  const ImU32 cCount  = ImGui::GetColorU32(ImVec4(col_count_[0], col_count_[1], col_count_[2], col_count_[3]));
  // Contour noir (4 directions) pour la lisibilité + faux-gras optionnel (ImGui n'a pas de fonte bold).
  const bool bold = bold_text_;
  const ImU32 cOutline = IM_COL32(0, 0, 0, 230);
  auto boldAdd = [&](ImVec2 p, ImU32 c, const char* t) {
    dl->AddText(ImVec2(p.x - 1, p.y), cOutline, t);
    dl->AddText(ImVec2(p.x + 1, p.y), cOutline, t);
    dl->AddText(ImVec2(p.x, p.y - 1), cOutline, t);
    dl->AddText(ImVec2(p.x, p.y + 1), cOutline, t);
    dl->AddText(p, c, t);
    if (bold) dl->AddText(ImVec2(p.x + 1.0f, p.y), c, t);
  };
  auto boldAddF = [&](ImFont* f, float s, ImVec2 p, ImU32 c, const char* t) {
    dl->AddText(f, s, ImVec2(p.x - 1, p.y), cOutline, t);
    dl->AddText(f, s, ImVec2(p.x + 1, p.y), cOutline, t);
    dl->AddText(f, s, ImVec2(p.x, p.y - 1), cOutline, t);
    dl->AddText(f, s, ImVec2(p.x, p.y + 1), cOutline, t);
    dl->AddText(f, s, p, c, t);
    if (bold) dl->AddText(f, s, ImVec2(p.x + 1.0f, p.y), c, t);
  };

  // Fond du cadre (derrière tous les boutons).
  dl->AddRectFilled(wp, ImVec2(wp.x + winw, wp.y + winh), cFrame, 4.0f);

  // Filtre texture des icônes (POINT net par défaut, LINEAR flou si activé).
  g_sb_bilinear = bilinear_;
  dl->AddCallback(SbApplyFilter, nullptr);

  int hover = -1;  // index local du slot survolé

  // NB : le drop d'un drag NATIF (inventaire/grimoire) n'est PAS géré ici. Le faire
  // en OnRenderUI (post-input) obligeait à annuler le drag en cours de route, ce qui
  // fait que le jeu reclasse le bouton maintenu en clic-au-sol (le perso marche).
  // -> géré dans le hook WndProc au WM_LBUTTONUP (pré-input) via HandleNativeDrop :
  // le drag natif reste vivant pendant la traverse (pas de marche), assignation au
  // relâchement sur la case survolée, charge vidée avant que le jeu ne voie le up.

  // ── Déverrouillé : surface de drag plein-cadre -> déplacer la barre depuis PARTOUT
  if (!locked_) {
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##bardrag", ImVec2(winw, winh));
    const ImVec2 mp = ImGui::GetIO().MousePos;
    if (ImGui::IsItemActivated()) {  // mémorise le décalage curseur -> coin de la barre
      bar_drag_off_x_ = mp.x - static_cast<float>(bc.x);
      bar_drag_off_y_ = mp.y - static_cast<float>(bc.y);
    }
    if (ImGui::IsItemActive()) {
      float nx = mp.x - bar_drag_off_x_, ny = mp.y - bar_drag_off_y_;
      // Aimante sur la grille d'alignement PARTAGÉE de MoonlightUi (la même que menu icons / basic
      // info ; réglée dans Réglages interface > "Aimanter à la grille"). SnapAxis = no-op si off.
      if (auto* mui = Bourgeon::Instance().moonlight_ui()) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        nx = mui->grid_.SnapAxis(nx, ds.x);
        ny = mui->grid_.SnapAxis(ny, ds.y);
      }
      bc.x = static_cast<int>(nx + 0.5f);
      bc.y = static_cast<int>(ny + 0.5f);
    }
    if (ImGui::IsItemDeactivated()) dirty_ = true;  // persiste la position en fin de glisser
  }

  int move_from = -1, move_to = -1, move_region = -1;  // glisser-déposer différé hors de la boucle
  int inv_drop_slot = -1; uint32_t inv_drop_id = 0;    // item d'inventaire lâché sur une case (différé)

  for (int k = 0; k < count; ++k) {
    const int slot = bc.first_slot + k;
    if (slot >= maxSlots) break;
    const int cx = k % cols, cy = k / cols;
    const ImVec2 p0(wp.x + pad + cx * step, wp.y + pad + cy * step);
    const ImVec2 p1(p0.x + icon_size_, p0.y + icon_size_);

    const SlotRec r = ReadSlot(region, slot);

    bool clicked = false;
    if (locked_) {  // verrouillé = interactif : clic = use, glisser = réarranger, survol = tooltip
      ImGui::PushID(k);
      ImGui::SetCursorPos(ImVec2(pad + cx * step, pad + cy * step));
      clicked = ImGui::InvisibleButton("s", ImVec2(icon_size_, icon_size_));
      if (ImGui::IsItemHovered()) hover = k;
      // Source = slot occupé ; cible = n'importe quel slot (swap / déplacement).
      // Aperçu de drag = JUSTE l'icône : fond du tooltip transparent + AUCUN liseré.
      // ImageBorderSize=0 ne suffit pas toujours -> on rend AUSSI la couleur du bord
      // (ImGuiCol_Border) transparente : neutralise bord d'image ET bord de fenêtre.
      ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_ImageBorderSize, 0.0f);
      if (r.valid && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        const int pl[2] = {region, slot};  // région + slot : interdit le mélange entre barres
        ImGui::SetDragDropPayload("SBSLOT", pl, sizeof(pl));
        void* ptex = GetIconTex(r.type, r.id);
        if (ptex) ImGui::Image((ImTextureID)(uintptr_t)ptex,
                               ImVec2(icon_size_, icon_size_));
        else ImGui::Text("%s %u", r.type == 0 ? "Objet" : "Skill", r.id);
        ImGui::EndDragDropSource();
      }
      ImGui::PopStyleVar(3);
      ImGui::PopStyleColor(2);
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SBSLOT")) {
          const int* d = static_cast<const int*>(pl->Data);
          move_region = d[0]; move_from = d[1]; move_to = slot;
        } else if (ImGui::AcceptDragDropPayload("INV_ITEM")) {
          // Item lâché depuis le viewer inventaire ImGui (comme le drag natif inventaire->barre,
          // que HandleNativeDrop gère pour la fenêtre native). Le viewer expose le nameid glissé ;
          // un slot d'item (region 2) OU une case de barre skill (regions 0/1) accepte un OBJET.
          if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
            const uint32_t nameid = iv->DraggedItemNameId();
            if (nameid != 0) { inv_drop_slot = slot; inv_drop_id = nameid; }
          }
        }
        ImGui::EndDragDropTarget();
      }
    }

    ImU32 bg = cEmpty;
    if (r.valid) bg = (r.type == 0) ? cItem : cSkill;  // NATIF : type0=objet(bleu), type1=skill(vert)
    dl->AddRectFilled(p0, p1, bg, 3.0f);

    if (r.valid) {
      // Utilisable ? OBJET = quantité inventaire > 0 ; SKILL = connu/appris (getter natif). Sinon la
      // case reste affichée mais GRISÉE (objet épuisé à 0, ou skill non appris / item source perdu).
      const bool isItem    = (r.type == 0);
      const int  liveCount = isItem ? GetItemLiveCount(r.id) : 0;
      const bool usable    = isItem ? (liveCount > 0) : SkillKnown(r.id);

      // Icône du jeu si dispo (V2), sinon repli sur la boîte-id (fail-safe).
      void* tex = GetIconTex(r.type, r.id);
      if (tex) {
        dl->AddImage((ImTextureID)(uintptr_t)tex, p0, p1);
      } else {
        char idbuf[16];
        std::snprintf(idbuf, sizeof(idbuf), "%u", r.id);
        const ImVec2 ts = ImGui::CalcTextSize(idbuf);
        boldAdd(ImVec2(p0.x + (icon_size_ - ts.x) * 0.5f,
                       p0.y + (icon_size_ - ts.y) * 0.5f),
                IM_COL32(235, 235, 235, 255), idbuf);
      }
      // Nombre en bas-droite : OBJET = quantité LIVE de l'inventaire (décrémente à l'usage) ;
      // SKILL = niveau (rec[5], statique). Convention NATIVE : type0=OBJET, type1=SKILL.
      const int shown = isItem ? liveCount : r.level;
      if (shown > 0) {
        char lv[12];
        std::snprintf(lv, sizeof(lv), "%d", shown);
        const ImVec2 ls = ImGui::CalcTextSize(lv);
        boldAdd(ImVec2(p1.x - ls.x - 1, p1.y - ls.y - 1), cCount, lv);
      }
      // Overlay de cooldown : indexé par skill id ; un objet n'y figure pas -> 0.
      const float f = CooldownFraction(r.id);
      if (f > 0.0f) {
        const float h = icon_size_ * f;
        dl->AddRectFilled(ImVec2(p0.x, p1.y - h), p1, IM_COL32(0, 0, 0, 150), 3.0f);
      }
      // Case INUTILISABLE (objet épuisé / skill non appris) : voile gris sombre par-dessus.
      if (!usable)
        dl->AddRectFilled(p0, p1, IM_COL32(10, 10, 15, 165), 3.0f);
    }

    dl->AddRect(p0, p1, (hover == k) ? cBordHi : cBorder, 3.0f);

    // Étiquette de touche en haut-gauche : touche RÉELLE lue depuis UserKeys.lua (rebinds inclus,
    // layout-aware). Dessinée même sur slot vide (la touche dépend de la position, pas du contenu).
    if (show_keys_) {
      char key[24];
      GetSlotKeyLabel(keyCat, slot, key, sizeof(key));
      if (key[0]) {
        ImFont* font = ImGui::GetFont();
        float ks = std::max(7.0f, icon_size_ * 0.30f);  // petit, proportionnel à l'icône
        const float maxW = icon_size_ - 3.0f;           // marge à droite de la case
        const float w = font->CalcTextSizeA(ks, 1.0e30f, 0.0f, key).x;
        if (w > maxW && w > 0.0f) ks *= maxW / w;        // rétrécit un libellé long (ex "Ctrl + F1")
        boldAddF(font, ks, ImVec2(p0.x + 1.5f, p0.y + 0.5f), cKeyTxt, key);
      }
    }

    if (locked_) {
      if (clicked && r.valid) ActivateSlot(region, slot);
      if (hover == k && r.valid)  // tooltip ImGui avec le nom natif (skill/objet)
        ShowSlotTooltip(region, slot);
      ImGui::PopID();
    }
  }

  // Restaure l'état de rendu ImGui par défaut (filtre LINEAR + render state).
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

  // Réarrangement différé (glisser-déposer interne) : swap src <-> dst.
  if (move_from >= 0 && move_to >= 0) {
    if (move_region == region) MoveSlot(region, move_from, move_to);        // même barre
    else MoveSlotCross(move_region, move_from, region, move_to);            // INTER-onglets
    if (RegionIsItems(region) || RegionIsItems(move_region)) dirty_ = true;  // items -> persist client
  }

  // Dépôt différé d'un item d'inventaire (payload "INV_ITEM") sur une case. MÊME logique que
  // HandleNativeDrop pour un drag natif d'objet : écriture directe du record + persistance
  // (skills 0/1 -> serveur CZ_SHORTCUT_KEY_CHANGE ; barre d'items -> yaml client).
  if (inv_drop_slot >= 0 && inv_drop_id != 0) {
    WriteSlotRecord(region, inv_drop_slot, /*is_item*/ true, inv_drop_id, 0);
    if (!RegionIsItems(region))
      SendHotkeyChange(kRegions[region].tab, inv_drop_slot, /*isSkill*/ 0, inv_drop_id, 0);
    else
      dirty_ = true;
  }

  // ── Verrouillé, slot survolé : clic molette = vider, clic droit = description ──
  if (locked_ && hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
    const int slot = bc.first_slot + hover;
    const SlotRec r = ReadSlot(region, slot);
    if (r.valid) { ClearSlot(region, slot); if (RegionIsItems(region)) dirty_ = true; }
  }
  if (locked_ && hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    const int slot = bc.first_slot + hover;
    const ImVec2 mp = ImGui::GetIO().MousePos;
    OpenSlotDescription(region, slot, static_cast<int>(mp.x), static_cast<int>(mp.y));
  }

  ImGui::End();
  ImGui::PopStyleVar(2);
}
