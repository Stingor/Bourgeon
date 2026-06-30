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
  LogDiag("[SkillBar] L-CLICK activate slot={} (col={} row={})", i, i % 9, i / 9);
  void* mgr = reinterpret_cast<void*>(kSkillInfoMgr);
  // Le handler 0x29 est GATE par l'option #5 (UI-lock natif, >=1 bloque). On le neutralise le temps
  // de l'appel puis on le restaure (état natif préservé). VÉRIFIÉ live : =1.
  const int lock = reinterpret_cast<GetOption_t>(kGetOption)(mgr, 5);
  if (lock >= 1) reinterpret_cast<SetOption_t>(kSetOption)(mgr, 5, 0, 0);
  Vf<OnMsg_t>(w, kVfOnMsg)(w, nullptr, 0, kMsgUseSlot, i % 9, i / 9, 0, 0);
  if (lock >= 1) reinterpret_cast<SetOption_t>(kSetOption)(mgr, 5, lock, 0);
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
void GetSlotKeyLabel(int tab, int slot, char* out, int n) {
  out[0] = '\0';
  const int category = (tab == 0) ? 0 : 3;
  __try {
    alignas(4) uint8_t buf[0x40];
    std::memset(buf, 0, sizeof(buf));
    reinterpret_cast<GetHotKey_t>(kGetHotKey)(buf, category, slot);
    CopyMsvcString(buf + 0x08, out, n);                  // out+0x08 = nom de la touche
    reinterpret_cast<StrFree_t>(kStrFree)(buf + 0x08);   // détruit les 2 std::string du wrapper
    reinterpret_cast<StrFree_t>(kStrFree)(buf + 0x20);
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
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

// Resname sentinelle quand l'id n'a pas de ressource (0x00d7fa90 -> "Unknown-Skill",
// 0x00d5a720 -> "...unknown item..."). Tenter de charger ces .bmp échoue et SPAMME la console
// ("Resource File Loading fail"). On les rejette -> repli propre sur la boîte-id. ("nknown"
// couvre Unknown/unknown sans souci de casse de l'initiale).
inline bool LooksUnknown(const char* s) { return s && std::strstr(s, "nknown") != nullptr; }

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
bool SkillPath(int id, char* out, int n) {  // 0x00d7fa90 (+0x20 déréf) = ce qui marchait pour les skills
  __try {
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

struct NativeDrag { bool isItem; uint32_t id; int level; int srcSlot; int cat; int octet;
                    int dbg_raw; int dbg_inv; int dbg_res24; int dbg_res3c; };

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
    d->octet   = p[kPL_type];
    d->cat     = *reinterpret_cast<int*>(p + kPL_cat);
    // octet 0 = FullPayload (inventaire => OBJET) ; octet 1 = LitePayload (grimoire => SKILL).
    d->isItem  = (p[kPL_type] == 0);
    d->srcSlot = *reinterpret_cast<int*>(p + kPL_src);
    // --- DIAGNOSTIC : candidats d'id (pour trancher quel champ = id castable/nameid) ---
    d->dbg_raw = rawid;                                        // payload+0x04 brut
    { alignas(8) uint8_t inv[0xC0] = {};
      reinterpret_cast<GetInvInfo_t>(kGetInvItem)(inv, rawid);
      d->dbg_inv = *reinterpret_cast<int*>(inv + 0x08); }      // GetInvItemById(raw)+0x08
    { const char* s = PayloadStr(p, 0x24); d->dbg_res24 = (s && s[0]) ? std::atoi(s) : 0; }
    { const char* s = PayloadStr(p, 0x3c); d->dbg_res3c = (s && s[0]) ? std::atoi(s) : 0; }
    // ID UNIFIÉ = atoi(resname BRUT à payload+0x3c = mgr+0x344) : nameid (objet) / id skill (29...).
    // (payload+0x24 = resname transformé pour l'icône ; inv08 = INDEX inventaire -> FAUX, cf. user.)
    if (d->isItem) {
      d->id    = static_cast<uint32_t>(d->dbg_res3c != 0 ? d->dbg_res3c
                                       : (d->dbg_inv != 0 ? d->dbg_inv : rawid));
      d->level = 0;  // OBJET : on NE stocke PAS le count -> affiché LIVE via GetItemLiveCount (le 0x29
                     // objet/rec0==0 ne lit pas rec[5]). Évite la corruption 940->255 à la persistance.
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

// Écrit le record 7 octets (type/id/level) directement via this+0xc4[slot] (= là où
// ReadSlot lit). AUCUN appel de fonction du jeu (SkillMgr_SetShortCutSlot notifie le
// dispatcher 0x139 -> crash pendant un drag) ; ReadSlot voit la donnée au frame
// suivant. type record : 0=skill, !=0=objet (OnDrop écrit 1 objet / 0 skill). SEH.
void WriteSlotRecord(void* w, int slot, bool is_item, uint32_t id, int level) {
  __try {
    uint8_t* rec = *reinterpret_cast<uint8_t**>(
        reinterpret_cast<char*>(w) + kSlotArr + slot * 4);
    if (rec) {
      rec[0] = is_item ? 0 : 1;  // CONVENTION NATIVE (capture live barre native: Angelus rec0=1) : OBJET=0, SKILL=1
      std::memcpy(rec + 1, &id, 4);
      const int16_t lv = static_cast<int16_t>(level);
      std::memcpy(rec + 5, &lv, 2);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Clic-droit sur un slot -> ouvre la description en jeu, RÉPLIQUE EXACTE du handler
// natif UIShortCutWnd::OnRButtonDown 0x008f91a0 (par index de slot, sans le HitTest) :
//   OBJET (rec[0]==1) : fenêtre tooltip 0x2e ; bascule si elle montre déjà ce nameid
//     (w+0x104), sinon OnMsg(0x3d, nameid brut). Positionne au curseur.
//   SKILL (rec[0]==0) : SkillMgr_GetSkillInfo remplit une struct (2 std::string), si
//     trouvé (out+4) -> fenêtre 0xc + OnMsg(0x18, &struct) ; libère les 2 strings.
// OnMsg 0x18 COPIE la struct (sûr de libérer après). À n'appeler QUE hors drag natif
// (clic-droit simple) — appeler une fn jeu pendant un drag crashe. SEH (POD only).
void OpenSlotDescription(void* w, int slot, int mx, int my) {
  int d_id = 0, d_isItem = -1, d_found = -1, d_toggle = 0;
  uintptr_t d_wnd = 0;
  __try {
    uint8_t* rec = *reinterpret_cast<uint8_t**>(
        reinterpret_cast<char*>(w) + kSlotArr + slot * 4);
    if (rec) {
      const int id = *reinterpret_cast<int*>(rec + 1);
      d_id = id;
      if (id != 0) {
        void* mgr = reinterpret_cast<void*>(kUIWindowMgr);
        if (rec[0] != 0) {  // ── OBJET (rec[0]==1) ──
          d_isItem = 1;
          void* wnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
              mgr, nullptr, reinterpret_cast<void*>(kWinItemDesc));
          d_wnd = reinterpret_cast<uintptr_t>(wnd);
          if (wnd) {
            if (*reinterpret_cast<int*>(reinterpret_cast<char*>(wnd) + kItemWinShownId) == id) {
              d_toggle = 1;
              reinterpret_cast<CloseWin_t>(kCloseWindow)(mgr, nullptr, kWinItemDesc);  // bascule
            } else {
              Vf<OnMsg_t>(wnd, kVfOnMsg)(wnd, nullptr, 0, kMsgSetItem, id, 0, 0, 0);
              Vf<SetPos_t>(wnd, kVfSetPos)(wnd, nullptr, mx, my);
            }
          }
        } else {            // ── SKILL (rec[0]==0) ──
          d_isItem = 0;
          alignas(8) uint8_t info[kSkillInfoSize] = {};
          reinterpret_cast<GetSkillInfo_t>(kGetSkillInfo)(
              reinterpret_cast<void*>(kSkillInfoMgr), nullptr, info, id, 1);
          d_found = (*reinterpret_cast<int*>(info + kSkillInfoFound) != 0) ? 1 : 0;
          if (d_found) {
            void* wnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
                mgr, nullptr, reinterpret_cast<void*>(kWinSkillDesc));
            d_wnd = reinterpret_cast<uintptr_t>(wnd);
            if (wnd) {
              Vf<OnMsg_t>(wnd, kVfOnMsg)(wnd, nullptr, 0, kMsgSetSkill,
                                         static_cast<int>(reinterpret_cast<uintptr_t>(info)), 0, 0, 0);
              Vf<SetPos_t>(wnd, kVfSetPos)(wnd, nullptr, mx, my);
            }
          }
          reinterpret_cast<StrFree_t>(kStrFree)(info + kSkillStr1);
          reinterpret_cast<StrFree_t>(kStrFree)(info + kSkillStr0);
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  LogDiag("[SkillBar] DESC slot={} id={} isItem={} found={} wnd={:#x} toggle={}",
          slot, d_id, d_isItem, d_found, d_wnd, d_toggle);
}

// Survol d'un slot rempli -> tooltip ImGui avec le nom (réplique le NOM de la barre native
// UIShortCutWnd::OnMouseMove 0x008f7f50) : OBJET = DB item (FUN_006a0d40) ; SKILL = Lua
// GetSkillName(id) (FUN_0073a1f0) car les ids skills sont absents de la DB item (prouvé par
// traversée RB live de 0x01255130). Rendu via ImGui::SetTooltip (le tooltip natif FUN_00a753d0
// passait SOUS la barre ImGui + 1 frame de retard). SEH (POD only ; SetTooltip hors __try).
void ShowSlotTooltip(void* w, int slot) {
  char nm[160] = {};  // nom extrait (POD)
  bool valid = false, is_item = false;
  int id = 0, level = 0;
  unsigned rec0 = 0;  // octet type brut (diagnostic)
  __try {
    uint8_t* rec = *reinterpret_cast<uint8_t**>(
        reinterpret_cast<char*>(w) + kSlotArr + slot * 4);
    if (rec) {
      id = *reinterpret_cast<int*>(rec + 1);
      if (id != 0) {
        valid = true;
        rec0 = rec[0];
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
  static int s_lastTipSlot = -1;  // throttle : 1 ligne par changement de slot survolé
  if (slot != s_lastTipSlot) {
    s_lastTipSlot = slot;
    LogDiag("[SkillBar] TOOLTIP slot={} rec0={} id={} is_item={} name='{}'",
            slot, rec0, id, is_item, nm);
  }
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
  if (!in_game_) { native_hidden_ = false; last_tab_ = -1; }
  FlushIconCache();  // recharge les icônes au changement de zone
}

void SkillBarTweaks::OnKeyDown(unsigned long vkey, int, int) {
  if (vkey == VK_OEM_3) panel_visible_ = !panel_visible_;  // touche ²/~ : panneau
}

void SkillBarTweaks::OnRenderUI() {
  static bool s_banner = false;  // bannière 1x : confirme quel code de convention tourne
  if (!s_banner) {
    s_banner = true;
    LogDiag("[SkillBar] CONVENTION NATIVE active: rec0==0=SKILL, rec0==1=OBJET "
            "(is_item=rec0!=0 ; write rec0=is_item?1:0). Hash DLL dans la ligne [Integrity].");
  }
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

  LogDiag("[SkillBar] DROP isItem={} id={} lvl={} src={} octet={} cat={} | raw04={} inv08={} res24={} res3c={} -> slot={} (rec0={})",
          d.isItem, d.id, d.level, d.srcSlot, d.octet, d.cat,
          d.dbg_raw, d.dbg_inv, d.dbg_res24, d.dbg_res3c, slot, d.isItem ? 1 : 0);
  WriteSlotRecord(w, slot, d.isItem, d.id, d.level);
  // Écriture directe -> le serveur n'est pas notifié par SetShortCutSlot : on envoie
  // nous-même CZ_SHORTCUT_KEY_CHANGE pour persister l'assignation (drop inventaire/grimoire).
  SendHotkeyChange(CurrentTab(), slot, d.isItem ? 0 : 1, d.id, d.level);  // isSkill=rec[0] : OBJET=0, SKILL=1
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
  changed |= ImGui::Checkbox("Afficher les touches", &show_keys_);
  ImGui::SameLine(); ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Affiche la touche de chaque slot en haut-gauche (F1-F9 ou la touche\n"
                      "rebindee si tu l'as changee dans les raccourcis clavier).");
  changed |= ImGui::Checkbox("Texte gras", &bold_text_);  // faux-gras (touches + nombres)
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
  const ImU32 cKeyTxt = ImGui::GetColorU32(ImVec4(col_keytext_[0], col_keytext_[1], col_keytext_[2], col_keytext_[3]));
  const ImU32 cCount  = ImGui::GetColorU32(ImVec4(col_count_[0], col_count_[1], col_count_[2], col_count_[3]));
  // Faux-gras : re-dessine le texte décalé d'1px (ImGui n'a pas de fonte bold chargée).
  const bool bold = bold_text_;
  auto boldAdd = [&](ImVec2 p, ImU32 c, const char* t) {
    dl->AddText(p, c, t);
    if (bold) dl->AddText(ImVec2(p.x + 1.0f, p.y), c, t);
  };
  auto boldAddF = [&](ImFont* f, float s, ImVec2 p, ImU32 c, const char* t) {
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
        else ImGui::Text("%s %u", r.type == 0 ? "Objet" : "Skill", r.id);
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
    if (r.valid) bg = (r.type == 0) ? cItem : cSkill;  // NATIF : type0=objet(bleu), type1=skill(vert)
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
        boldAdd(ImVec2(p0.x + (icon_size_ - ts.x) * 0.5f,
                       p0.y + (icon_size_ - ts.y) * 0.5f),
                IM_COL32(235, 235, 235, 255), idbuf);
      }
      // Nombre en bas-droite : OBJET = quantité LIVE de l'inventaire (décrémente à l'usage) ;
      // SKILL = niveau (rec[5], statique). Convention NATIVE : type0=OBJET, type1=SKILL.
      const int shown = (r.type == 0) ? GetItemLiveCount(r.id) : r.level;
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
    }

    dl->AddRect(p0, p1, (hover == k) ? cBordHi : cBorder, 3.0f);

    // Étiquette de touche en haut-gauche : touche RÉELLE lue depuis UserKeys.lua (rebinds inclus,
    // layout-aware). Dessinée même sur slot vide (la touche dépend de la position, pas du contenu).
    if (show_keys_) {
      char key[24];
      GetSlotKeyLabel(tab, slot, key, sizeof(key));
      if (key[0]) {
        const float ks = std::max(7.0f, icon_size_ * 0.30f);  // petit, proportionnel à l'icône
        boldAddF(ImGui::GetFont(), ks, ImVec2(p0.x + 1.5f, p0.y + 0.5f), cKeyTxt, key);
      }
    }

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
    LogDiag("[SkillBar] R-CLICK slot={} shift={}", slot, ImGui::GetIO().KeyShift);
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
