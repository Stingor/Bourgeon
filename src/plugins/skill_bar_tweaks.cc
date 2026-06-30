#include "plugins/skill_bar_tweaks.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"
#include "imgui.h"
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

// ---- tooltip survol (réplique UIShortCutWnd OnMouseMove 0x008f7f50 -> FUN_00a753d0) ----
constexpr uintptr_t kCmdDispatcher  = 0x0121333c;  // g_UICommandDispatcher : this = *(void**)addr
constexpr uintptr_t kShowTooltip    = 0x00a753d0;  // tooltip-curseur(this,text,x,y,color,p5,p6) ; appel/frame
constexpr uintptr_t kGetSkillName   = 0x006a2ce0;  // FUN_006a2ce0(SkillInfo,out,0) -> nom (std::string)
constexpr uintptr_t kItemDbGet      = 0x006a0d40;  // ItemDB_GetRecordById(__cdecl nameid, table)
constexpr uintptr_t kItemDbTable    = 0x01255130;  // table std::map du DB item (arg)
constexpr uintptr_t kItemDbSentinel = 0x01255138;  // retour si id inconnu -> NE PAS déréf
constexpr uintptr_t kItemRecVtable  = 0x0100a5ec;  // vtable d'un vrai record (garde anti-sentinelle)
constexpr int kItemNameEn  = 0x04;  // record+0x04 = nom anglais (ASCII, propre à l'affichage)
constexpr int kItemNameLoc = 0x08;  // record+0x08 = nom localisé (CP949, repli)
constexpr int kStrCap      = 0x14;  // std::string : capacité (>0xf => heap, lire *(char**))

// ---- cooldown (g_ShortCutCooldownList 0x015ff7e0) --------------------------
constexpr uintptr_t kCooldownList  = 0x015ff7e0;   // sentinelle (la liste EST la sentinelle)
constexpr uintptr_t kUseGameClock  = 0x015beecc;   // 0 -> timeGetTime(), sinon horloge jeu
constexpr uintptr_t kGameClockGet  = 0x00b1fac0;   // -> ptr, DWORD horloge à +0x20

using OnMsg_t      = int   (__fastcall*)(void*, void*, int, int, int, int, int, int);
using SetVisible_t = void  (__fastcall*)(void*, void*, int);
using MakeWindow_t = void* (__fastcall*)(void*, void*, void*);
using SetSlot_t    = void  (__fastcall*)(void*, void*, int, int, int, int, int);
using GetOption_t  = int   (__thiscall*)(void*, int);
using GameClock_t  = void* (__cdecl*)();
using GetSkillInfo_t = void (__fastcall*)(void*, void*, void*, int, int);  // (mgr,edx,out,id,gate)
using StrFree_t      = void (__fastcall*)(void*);                          // (ecx=std::string base)
using CloseWin_t     = char (__fastcall*)(void*, void*, int);             // (mgr,edx,id)
using SetPos_t       = void (__fastcall*)(void*, void*, int, int);        // (wnd,edx,x,y)
using ShowTip_t      = void (__fastcall*)(void*, void*, const char*, int, int, unsigned, unsigned char, char);
using ItemDbGet_t    = void* (__cdecl*)(int, void*);                       // (nameid, table) -> record / sentinelle
using GetSkillName_t = void* (__fastcall*)(void*, void*, void*, int);     // (SkillInfo,edx,out,p2=0) -> out

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

// Lit le slot logique i (0..35) via this+0xc4[i] -> record 7 octets dans les globals.
SlotRec ReadSlot(void* w, int i) {
  SlotRec s{false, 0, 0, 0};
  if (i < 0 || i >= kMaxSlots) return s;
  uint8_t* rec = *reinterpret_cast<uint8_t**>(
      reinterpret_cast<uint8_t*>(w) + kSlotArr + i * 4);
  if (!rec) return s;
  uint32_t id;
  std::memcpy(&id, rec + 1, 4);  // id NON aligné dans le record packé
  if (id == 0) return s;         // slot vide (le natif teste id!=0)
  int16_t lvl;
  std::memcpy(&lvl, rec + 5, 2);
  s = {true, rec[0], id, lvl};
  return s;
}

// Active le slot logique i exactement comme une touche F (OnMsg case 0x29).
void ActivateSlot(void* w, int i) {
  Vf<OnMsg_t>(w, kVfOnMsg)(w, nullptr, 0, kMsgUseSlot, i % 9, i / 9, 0, 0);
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
  p.isSkill = type;            // record type tel quel (0=skill, !=0=objet) ; round-trip via le serveur
  p.id = id;
  p.count = static_cast<uint16_t>(level);
  Bourgeon::Instance().SendPacket(reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}

// Vide le slot logique i de l'onglet tab (écrit directement les globals).
void ClearSlot(int i, int tab) {
  reinterpret_cast<SetSlot_t>(kSetShortCut)(
      reinterpret_cast<void*>(kSkillInfoMgr), nullptr, /*type*/0, /*id=0 -> clear*/0,
      /*level*/0, /*slot*/i, /*tab*/tab);
  SendHotkeyChange(tab, i, 0, 0, 0);  // persiste l'effacement côté serveur
}
// Écrit un slot (id==0 => efface) via SkillMgr_SetShortCutSlot.
void SetSlot(int slot, int tab, uint8_t type, uint32_t id, int level) {
  reinterpret_cast<SetSlot_t>(kSetShortCut)(
      reinterpret_cast<void*>(kSkillInfoMgr), nullptr,
      static_cast<int>(type), static_cast<int>(id), level, slot, tab);
  SendHotkeyChange(tab, slot, type, id, level);  // persiste côté serveur
}
// Échange le contenu de deux slots (ou déplace vers un slot vide) = réarrangement.
void MoveSlot(void* w, int src, int dst, int tab) {
  if (src == dst || src < 0 || dst < 0) return;
  const SlotRec a = ReadSlot(w, src);
  const SlotRec b = ReadSlot(w, dst);
  SetSlot(dst, tab, a.type, a.id, a.level);  // a.id==0 si vide -> efface dst
  SetSlot(src, tab, b.type, b.id, b.level);
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
//   SKILL : 0x00d7fa90 __stdcall(out, id) -> resname *(char**)(out+0x20) (= ce qui marchait ; getter
//           unifié qui route vers le skill-mgr pour les ranges custom, sinon DB).
using GetInvInfo_t = void* (__stdcall*)(void*, int);          // 0x00d7fa90 (out, id)
using BuildPath_t  = void  (__stdcall*)(const char*, char*);  // 0x00d5a720 (id_str, out_path[>=260])

std::unordered_map<uint32_t, void*> g_iconCache;  // (type0?hi:0)|id -> ImTextureID (null=miss connu)

// Chemins SEH-protégés (aucun objet C++ -> SEH OK), écrits dans out (taille >=260).
bool ItemPath(int id, char* out, int /*n*/) {
  __try {
    char idstr[16];
    std::snprintf(idstr, sizeof(idstr), "%d", id);
    out[0] = '\0';
    reinterpret_cast<BuildPath_t>(0x00d5a720)(idstr, out);  // écrit le chemin complet dans out
    return out[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
}
bool SkillPath(int id, char* out, int n) {  // 0x00d7fa90 (+0x20 déréf) = ce qui marchait pour les skills
  __try {
    alignas(8) uint8_t info[0xA0] = {};
    reinterpret_cast<GetInvInfo_t>(0x00d7fa90)(info, id);          // __stdcall(out, id)
    const char* rn = *reinterpret_cast<const char**>(info + 0x20);  // resname (déréférencé)
    if (rn && rn[0]) {
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
// type0=objet, sinon skill ; essaie la source prioritaire selon le type puis l'autre
// (robuste à l'ambiguïté du type). Fail-safe : null mis en cache (boîte id) si échec.
void* GetIconTex(uint8_t type, uint32_t id) {
  // ⚠️ ORDRE EMPIRIQUE (calé en jeu) — NE PAS "logiquer". Convention jeu : type 0 =
  // SKILL, !=0 = OBJET. Mais ItemPath(0x00d5a720)/SkillPath(0x00d7fa90) ne sont PAS
  // strictement objet/skill ; le seul ordre qui donne les bonnes icônes des DEUX
  // est : type0 -> 0x00d5a720 d'abord, type!=0 -> 0x00d7fa90 d'abord (le fallback
  // 2-passes couvre l'autre). L'inverser CASSE les icônes de sorts (vu en jeu).
  const uint32_t k = (type == 0 ? 0x80000000u : 0u) | (id & 0x7fffffffu);
  auto it = g_iconCache.find(k);
  if (it != g_iconCache.end()) return it->second;
  char path[300] = {};
  void* tex = nullptr;
  const bool item_first = (type == 0);  // (nom historique) type0 -> 0x00d5a720 d'abord ; sinon 0x00d7fa90
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

// ---- pont drag NATIF (grimoire/inventaire) -> slot ImGui (RE drag-drop) -----
// FUN_00a75340(0x1213338) renvoie l'objet de drag en cours (gate +0x58==1) ou null.
// Charge (== OnDrop param_3) à objet+0x308 : +0x80 byte type (0=skill/1=objet),
// +0x04 id, +0x14 srcSlot (-1 si source = fenêtre), +0x6c count/level.
//   OBJET : id assignable = ItemMgr_GetInvItemById(id)+0x8 (nameid) ; level = count.
//   SKILL : id assignable = atoi(SkillInfo+0x2c) via FUN_00d5aa40 ; level = 0.
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

struct NativeDrag { bool isItem; uint32_t id; int level; int srcSlot; };

inline void* DragObj() {  // objet de drag en cours, ou null
  return reinterpret_cast<GetDragObj_t>(kGetDragObj)(reinterpret_cast<void*>(kDragMgr));
}
// Décode la charge -> {isItem, id assignable, level, srcSlot}. SEH-protégé.
bool DecodeDrag(void* obj, NativeDrag* d) {
  __try {
    uint8_t* p = reinterpret_cast<uint8_t*>(obj) + kPayloadOff;
    const int rawid = *reinterpret_cast<int*>(p + kPL_id);
    if (rawid == 0) return false;
    d->isItem  = p[kPL_type] != 0;
    d->srcSlot = *reinterpret_cast<int*>(p + kPL_src);
    if (d->isItem) {
      alignas(8) uint8_t inv[0xC0] = {};
      reinterpret_cast<GetInvInfo_t>(kGetInvItem)(inv, rawid);  // __stdcall(out,id)
      const int nameid = *reinterpret_cast<int*>(inv + 0x08);
      d->id    = static_cast<uint32_t>(nameid != 0 ? nameid : rawid);
      d->level = *reinterpret_cast<int*>(p + kPL_cnt);          // count
    } else {
      alignas(8) uint8_t info[0xF8] = {};
      reinterpret_cast<LookupSkill_t>(kLookupSkill)(
          reinterpret_cast<void*>(kSkillInfoMgr), nullptr, info, rawid);
      const int sid = reinterpret_cast<SkillId_t>(kSkillIdAtoi)(info);
      d->id    = static_cast<uint32_t>(sid != 0 ? sid : rawid);
      d->level = 0;
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

// Écrit le record 7 octets (type/id/level) directement via this+0xc4[slot] (= là où
// ReadSlot lit). AUCUN appel de fonction du jeu (SkillMgr_SetShortCutSlot notifie le
// dispatcher 0x139 -> crash pendant un drag) ; ReadSlot voit la donnée au frame
// suivant. type record : 0=skill, !=0=objet (OnDrop écrit 1 objet / 0 skill). SEH.
void WriteSlotRecord(void* w, int slot, bool is_item, uint32_t id, int level) {
  __try {
    uint8_t* rec = *reinterpret_cast<uint8_t**>(
        reinterpret_cast<char*>(w) + kSlotArr + slot * 4);
    if (rec) {
      rec[0] = is_item ? 1 : 0;
      std::memcpy(rec + 1, &id, 4);
      const int16_t lv = static_cast<int16_t>(level);
      std::memcpy(rec + 5, &lv, 2);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Clic-droit sur un slot -> ouvre la description en jeu, RÉPLIQUE EXACTE du handler
// natif UIShortCutWnd::OnRButtonDown 0x008f91a0 (par index de slot, sans le HitTest) :
//   OBJET (rec[0]!=0) : fenêtre tooltip 0x2e ; bascule si elle montre déjà ce nameid
//     (w+0x104), sinon OnMsg(0x3d, nameid brut). Positionne au curseur.
//   SKILL (rec[0]==0) : SkillMgr_GetSkillInfo remplit une struct (2 std::string), si
//     trouvé (out+4) -> fenêtre 0xc + OnMsg(0x18, &struct) ; libère les 2 strings.
// OnMsg 0x18 COPIE la struct (sûr de libérer après). À n'appeler QUE hors drag natif
// (clic-droit simple) — appeler une fn jeu pendant un drag crashe. SEH (POD only).
void OpenSlotDescription(void* w, int slot, int mx, int my) {
  __try {
    uint8_t* rec = *reinterpret_cast<uint8_t**>(
        reinterpret_cast<char*>(w) + kSlotArr + slot * 4);
    if (!rec) return;
    const int id = *reinterpret_cast<int*>(rec + 1);
    if (id == 0) return;
    void* mgr = reinterpret_cast<void*>(kUIWindowMgr);

    if (rec[0] != 0) {  // ── OBJET ──
      void* wnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
          mgr, nullptr, reinterpret_cast<void*>(kWinItemDesc));
      if (!wnd) return;
      if (*reinterpret_cast<int*>(reinterpret_cast<char*>(wnd) + kItemWinShownId) == id) {
        reinterpret_cast<CloseWin_t>(kCloseWindow)(mgr, nullptr, kWinItemDesc);  // bascule fermer
      } else {
        Vf<OnMsg_t>(wnd, kVfOnMsg)(wnd, nullptr, 0, kMsgSetItem, id, 0, 0, 0);
        Vf<SetPos_t>(wnd, kVfSetPos)(wnd, nullptr, mx, my);
      }
    } else {            // ── SKILL ──
      alignas(8) uint8_t info[kSkillInfoSize] = {};
      reinterpret_cast<GetSkillInfo_t>(kGetSkillInfo)(
          reinterpret_cast<void*>(kSkillInfoMgr), nullptr, info, id, 1);
      if (*reinterpret_cast<int*>(info + kSkillInfoFound) != 0) {
        void* wnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
            mgr, nullptr, reinterpret_cast<void*>(kWinSkillDesc));
        if (wnd) {
          Vf<OnMsg_t>(wnd, kVfOnMsg)(wnd, nullptr, 0, kMsgSetSkill,
                                     static_cast<int>(reinterpret_cast<uintptr_t>(info)), 0, 0, 0);
          Vf<SetPos_t>(wnd, kVfSetPos)(wnd, nullptr, mx, my);
        }
      }
      reinterpret_cast<StrFree_t>(kStrFree)(info + kSkillStr1);  // libère nom puis resname
      reinterpret_cast<StrFree_t>(kStrFree)(info + kSkillStr0);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Survol d'un slot rempli -> tooltip-curseur NATIF (le même que la barre native),
// réplique du handler natif UIShortCutWnd::OnMouseMove 0x008f7f50 : on récupère le NOM
// (skill via SkillMgr_GetSkillInfo+FUN_006a2ce0 ; objet via le DB item FUN_006a0d40) et
// on le passe à FUN_00a753d0 (tooltip-curseur du dispatcher). À APPELER CHAQUE FRAME au
// survol -> il rafraîchit son timer et s'auto-masque quand on cesse d'appeler. Aucune fn
// jeu pendant un drag (ici = simple survol, OK). SEH (POD only).
void ShowSlotTooltip(void* w, int slot) {
  char nm[160] = {};  // nom extrait (POD) -> rendu ensuite en tooltip ImGui (z-order garanti)
  __try {
    uint8_t* rec = *reinterpret_cast<uint8_t**>(
        reinterpret_cast<char*>(w) + kSlotArr + slot * 4);
    if (rec) {
      const int id = *reinterpret_cast<int*>(rec + 1);
      if (id != 0) {
        if (rec[0] != 0) {  // ── OBJET : nom depuis le DB item ──
          char* dbrec = static_cast<char*>(
              reinterpret_cast<ItemDbGet_t>(kItemDbGet)(id, reinterpret_cast<void*>(kItemDbTable)));
          if (dbrec && reinterpret_cast<uintptr_t>(dbrec) != kItemDbSentinel &&
              *reinterpret_cast<uintptr_t*>(dbrec) == kItemRecVtable) {  // garde anti-sentinelle
            const char* en  = *reinterpret_cast<const char**>(dbrec + kItemNameEn);
            const char* loc = *reinterpret_cast<const char**>(dbrec + kItemNameLoc);
            const char* name = (en && *en) ? en : loc;
            if (name && *name) std::snprintf(nm, sizeof(nm), "%s", name);
          }
        } else {            // ── SKILL : nom via GetSkillInfo + FUN_006a2ce0 ──
          alignas(8) uint8_t info[kSkillInfoSize] = {};
          reinterpret_cast<GetSkillInfo_t>(kGetSkillInfo)(
              reinterpret_cast<void*>(kSkillInfoMgr), nullptr, info, id, 1);
          alignas(8) uint8_t nameStr[0x18] = {};  // std::string construit par FUN_006a2ce0
          reinterpret_cast<GetSkillName_t>(kGetSkillName)(info, nullptr, nameStr, 0);
          const uint32_t cap = *reinterpret_cast<uint32_t*>(nameStr + kStrCap);
          const char* name = (cap > 0xf) ? *reinterpret_cast<const char**>(nameStr)
                                         : reinterpret_cast<const char*>(nameStr);
          if (name && *name) std::snprintf(nm, sizeof(nm), "%s", name);
          reinterpret_cast<StrFree_t>(kStrFree)(nameStr);           // free le nom
          reinterpret_cast<StrFree_t>(kStrFree)(info + kSkillStr1);  // puis les 2 strings du SkillInfo
          reinterpret_cast<StrFree_t>(kStrFree)(info + kSkillStr0);
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { nm[0] = '\0'; }
  // Rendu en tooltip ImGui (au-dessus de tout) — le tooltip-curseur natif FUN_00a753d0
  // passait SOUS notre barre (fenêtre jeu rendue avant l'overlay) et n'apparaissait pas.
  if (nm[0]) ImGui::SetTooltip("%s", nm);
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
  if (!in_game_) { native_hidden_ = false; last_tab_ = -1; }
  FlushIconCache();  // recharge les icônes au changement de zone
}

void SkillBarTweaks::OnKeyDown(unsigned long vkey, int, int) {
  if (vkey == VK_OEM_3) panel_visible_ = !panel_visible_;  // touche ²/~ : panneau
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
    }
  } else if (native_hidden_) {
    if (w) ShowNative(w);
    native_hidden_ = false;
  }

  if (panel_visible_) DrawPanel();
  if (native_hidden_ && w) DrawBar(w);
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

  const int   cols = (columns_ < 1) ? 1 : columns_;
  const float step = icon_size_ + spacing_;
  const float lx = static_cast<float>(mx) - static_cast<float>(bar_x_);
  const float ly = static_cast<float>(my) - static_cast<float>(bar_y_);
  if (lx < 0 || ly < 0) return false;
  const int cc = static_cast<int>(lx / step), cr = static_cast<int>(ly / step);
  if (cc >= cols) return false;
  if ((lx - cc * step) >= icon_size_ || (ly - cr * step) >= icon_size_) return false;  // gap
  const int k = cr * cols + cc;
  if (k < 0 || k >= slot_count_) return false;
  const int slot = first_slot_ + k;
  if (slot >= kMaxSlots) return false;

  WriteSlotRecord(w, slot, d.isItem, d.id, d.level);
  // Écriture directe -> le serveur n'est pas notifié par SetShortCutSlot : on envoie
  // nous-même CZ_SHORTCUT_KEY_CHANGE pour persister l'assignation (drop inventaire/grimoire).
  SendHotkeyChange(CurrentTab(), slot, d.isItem ? 1 : 0, d.id, d.level);
  CancelNativeDrag(obj);
  return true;
}

// ---- panneau de configuration ----------------------------------------------
void SkillBarTweaks::DrawPanel() {
  ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
  bool open = true;
  if (!ImGui::Begin("Skill Bar###SkillBarPanel", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End(); if (!open) panel_visible_ = false; return;
  }
  if (!open) panel_visible_ = false;

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
  changed |= WheelInt("Colonnes", &columns_, 1, 12);
  changed |= WheelInt("Nb slots", &slot_count_, 1, kMaxSlots);
  changed |= WheelFloat("Taille", &icon_size_, 16.0f, 64.0f, "%.0f px");
  changed |= WheelFloat("Espacement", &spacing_, 0.0f, 12.0f, "%.0f px");
  changed |= ImGui::Checkbox("Filtre bilineaire (flou)", &bilinear_);
  ImGui::SameLine(); ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Decoche = pixels nets (POINT). Coche = lissage ImGui (LINEAR).");
  changed |= ImGui::Checkbox("Clic-traversant (Shift = interagir)", &clickthrough_);
  ImGui::SameLine(); ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Coche = les clics traversent la barre (vont au jeu).\n"
                      "Maintiens Shift pour utiliser / rearranger / vider.");
  if (ImGui::CollapsingHeader("Couleurs")) {
    changed |= ColorSwatch("Fond du cadre", col_frame_);
    changed |= ColorSwatch("Fond objet", col_item_);
    changed |= ColorSwatch("Fond skill", col_skill_);
    changed |= ColorSwatch("Fond vide", col_empty_);
    changed |= ColorSwatch("Bordure", col_border_);
    changed |= ColorSwatch("Bordure survol", col_borderhi_);
  }
  ImGui::Text("Position : %d, %d  (onglet %d)", bar_x_, bar_y_, native_hidden_ ? last_tab_ : 0);
  ImGui::EndDisabled();
  if (changed) dirty_ = true;  // persistance drainée par MoonlightUi

  ImGui::Separator();
  ImGui::TextDisabled("Clic G = utiliser. Glisser = rearranger. Clic D = description. Shift+Clic D = vider.");
  ImGui::TextDisabled("Bleu = objet, Vert = skill. Toggle panneau : 2/~");
  ImGui::End();
}

// ---- la barre d'action elle-même -------------------------------------------
void SkillBarTweaks::DrawBar(void* w) {
  const int cols = std::max(1, columns_);
  const int rows = (slot_count_ + cols - 1) / cols;
  const float step = icon_size_ + spacing_;
  const float pad  = 0.0f;  // identique en édition et normal -> aucun décalage au changement de mode
  const float winw = cols * step - spacing_ + pad * 2;
  const float winh = rows * step - spacing_ + pad * 2;

  // Fenêtre toujours épinglée à (bar_x_,bar_y_) ; le déplacement en édition se
  // fait en glissant un slot (delta souris), comme menu_icons -> dessin et
  // hit-rect jamais désynchronisés.
  ImGui::SetNextWindowPos(ImVec2((float)bar_x_, (float)bar_y_), ImGuiCond_Always);
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
    const float lx = mp.x - static_cast<float>(bar_x_), ly = mp.y - static_cast<float>(bar_y_);
    if (lx >= 0 && ly >= 0) {
      const int cc = static_cast<int>(lx / step), cr = static_cast<int>(ly / step);
      if (cc < cols && (lx - cc * step) < icon_size_ && (ly - cr * step) < icon_size_) {
        const int k = cr * cols + cc;
        if (k >= 0 && k < slot_count_ && first_slot_ + k < kMaxSlots)
          over_filled = ReadSlot(w, first_slot_ + k).valid;
      }
    }
  }
  bool no_input = false;
  if (clickthrough_ && !shift) no_input = true;  // option : toute la barre
  else if (locked_ && !over_filled && ImGui::GetDragDropPayload() == nullptr)
    no_input = true;                             // cases vides / espaces
  if (no_input) flags |= ImGuiWindowFlags_NoMouseInputs;

  ImGui::Begin("##SkillActionBar", nullptr, flags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const int tab = native_hidden_ ? last_tab_ : 0;

  const ImU32 cFrame  = ImGui::GetColorU32(ImVec4(col_frame_[0], col_frame_[1], col_frame_[2], col_frame_[3]));
  const ImU32 cSkill  = ImGui::GetColorU32(ImVec4(col_skill_[0], col_skill_[1], col_skill_[2], col_skill_[3]));
  const ImU32 cItem   = ImGui::GetColorU32(ImVec4(col_item_[0], col_item_[1], col_item_[2], col_item_[3]));
  const ImU32 cEmpty  = ImGui::GetColorU32(ImVec4(col_empty_[0], col_empty_[1], col_empty_[2], col_empty_[3]));
  const ImU32 cBorder = ImGui::GetColorU32(ImVec4(col_border_[0], col_border_[1], col_border_[2], col_border_[3]));
  const ImU32 cBordHi = ImGui::GetColorU32(ImVec4(col_borderhi_[0], col_borderhi_[1], col_borderhi_[2], col_borderhi_[3]));

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
    if (ImGui::IsItemActive()) {
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      bar_x_ += (int)d.x;
      bar_y_ += (int)d.y;
    }
    if (ImGui::IsItemDeactivated()) dirty_ = true;  // persiste la position en fin de drag
  }

  int move_from = -1, move_to = -1;  // glisser-déposer différé hors de la boucle

  for (int k = 0; k < slot_count_; ++k) {
    const int slot = first_slot_ + k;
    if (slot >= kMaxSlots) break;
    const int cx = k % cols, cy = k / cols;
    const ImVec2 p0(wp.x + pad + cx * step, wp.y + pad + cy * step);
    const ImVec2 p1(p0.x + icon_size_, p0.y + icon_size_);

    const SlotRec r = ReadSlot(w, slot);

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
        ImGui::SetDragDropPayload("SBSLOT", &slot, sizeof(slot));
        void* ptex = GetIconTex(r.type, r.id);
        if (ptex) ImGui::Image((ImTextureID)(uintptr_t)ptex,
                               ImVec2(icon_size_, icon_size_));
        else ImGui::Text("%s %u", r.type != 0 ? "Objet" : "Skill", r.id);
        ImGui::EndDragDropSource();
      }
      ImGui::PopStyleVar(3);
      ImGui::PopStyleColor(2);
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SBSLOT"))
          { move_from = *static_cast<const int*>(pl->Data); move_to = slot; }
        ImGui::EndDragDropTarget();
      }
    }

    ImU32 bg = cEmpty;
    if (r.valid) bg = (r.type != 0) ? cItem : cSkill;  // type!=0=objet(bleu), type0=skill(vert)
    dl->AddRectFilled(p0, p1, bg, 3.0f);

    if (r.valid) {
      // Icône du jeu si dispo (V2), sinon repli sur la boîte-id (fail-safe).
      void* tex = GetIconTex(r.type, r.id);
      if (tex) {
        dl->AddImage((ImTextureID)(uintptr_t)tex, p0, p1);
      } else {
        char idbuf[16];
        std::snprintf(idbuf, sizeof(idbuf), "%u", r.id);
        const ImVec2 ts = ImGui::CalcTextSize(idbuf);
        dl->AddText(ImVec2(p0.x + (icon_size_ - ts.x) * 0.5f,
                           p0.y + (icon_size_ - ts.y) * 0.5f),
                    IM_COL32(235, 235, 235, 255), idbuf);
      }
      if (r.level > 0) {
        char lv[8];
        std::snprintf(lv, sizeof(lv), "%d", r.level);
        const ImVec2 ls = ImGui::CalcTextSize(lv);
        dl->AddText(ImVec2(p1.x - ls.x - 1, p1.y - ls.y - 1),
                    IM_COL32(255, 230, 120, 255), lv);
      }
      // Overlay de cooldown : indexé par skill id ; un objet n'y figure pas -> 0.
      const float f = CooldownFraction(r.id);
      if (f > 0.0f) {
        const float h = icon_size_ * f;
        dl->AddRectFilled(ImVec2(p0.x, p1.y - h), p1, IM_COL32(0, 0, 0, 150), 3.0f);
      }
    }

    dl->AddRect(p0, p1, (hover == k) ? cBordHi : cBorder, 3.0f);

    if (locked_) {
      if (clicked && r.valid) ActivateSlot(w, slot);
      if (hover == k && r.valid)  // tooltip ImGui avec le nom natif (skill/objet)
        ShowSlotTooltip(w, slot);
      ImGui::PopID();
    }
  }

  // Restaure l'état de rendu ImGui par défaut (filtre LINEAR + render state).
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

  // Réarrangement différé (glisser-déposer interne) : swap src <-> dst.
  if (move_from >= 0 && move_to >= 0) MoveSlot(w, move_from, move_to, tab);

  // ── Verrouillé, clic droit sur le slot survolé : Shift = vider, sinon description ──
  if (locked_ && hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    const int slot = first_slot_ + hover;
    if (ImGui::GetIO().KeyShift) {              // Shift+clic D = vider la case
      const SlotRec r = ReadSlot(w, slot);
      if (r.valid) ClearSlot(slot, tab);
    } else {                                    // clic D = description en jeu (objet/skill)
      const ImVec2 mp = ImGui::GetIO().MousePos;
      OpenSlotDescription(w, slot, static_cast<int>(mp.x), static_cast<int>(mp.y));
    }
  }

  ImGui::End();
  ImGui::PopStyleVar(2);
}
