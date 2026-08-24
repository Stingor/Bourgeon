#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
#include "ragnarok/lua.h"
#include "ui/game_texture.h"
#include "features/overlays/skill_bar.h"

#include "ragnarok/uiwnd.h"
#include "ragnarok/homunculus.h"  // skill d'homoncule posé dans une case : lecture + lancement
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"
#include "features/gameplay/quick_cast.h"  // répétition d'un objet touche maintenue
#include "features/moonlight_ui/moonlight_ui.h"  // grille d'alignement partagée (grid_.SnapAxis)
#include "features/windows/inventory_viewer.h"  // DraggedItemNameId (drag inventaire -> case de barre)
#include "ui/imgui_escape.h"
#include "ragnarok/skill_cooldowns.h"  // cooldowns serveur (ZC_SKILL_POSTDELAY)
#include "ui/window_clamp.h"  // ClampWindowPosToScreen (barre déplacée à la main)
#include "ui/window_zorder.h"  // HUD maintenu sous les vraies fenêtres
#include "ui/ro_imgui.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"
#include "utils/i18n.h"
#include "ragnarok/user_hotkey.h"  // userhotkey::kGetHotKeyAddr
#include "ui/ro_widgets.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

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
constexpr int       kMgrShortCutPtr = 0x1e8;       // mgr+0x1e8 = instance UIShortCutWnd cachée
constexpr int       kShortCutId     = 0x24;

// ---- skill manager / données slots -----------------------------------------
constexpr uintptr_t kSetShortCut  = 0x00d96c20;    // SkillMgr_SetShortCutSlot
constexpr uintptr_t kGetOption    = 0x008e1d50;    // SkillInfoMgr_GetOption(mgr,key) ; key10=onglet courant
constexpr uintptr_t kSetOption    = 0x005c5950;    // SkillMgr_SetOption(mgr,key,val,0) ; key5=UI-lock (≥1 bloque OnMsg 0x29)

// ---- vtable / champs UIShortCutWnd -----------------------------------------
constexpr int kVfSetVisible = 0x38;   // FUN_009030c0(this, vis) -> this+0x28
constexpr int kSlotArr      = 0xc4;   // this+0xc4 = 36 pointeurs de record
constexpr int kVisFlag      = 0x28;   // flag visibilité (0=caché) écrit par vtable+0x38
constexpr int kMsgUseSlot   = 0x29;   // OnMsg : active le slot (p3=col, p4=row)
constexpr int kMsgRebuild   = 0x17;   // OnMsg : reconstruit this+0xc4 depuis les globals
constexpr int kMaxSlots     = 36;     // 0x24

// ---- description / tooltip clic-droit (réplique UIShortCutWnd OnRButtonDown 0x008f91a0) ----
constexpr uintptr_t kGetSkillInfo  = 0x00d5a980;  // SkillMgr_GetSkillInfo(mgr,out,id,gate) ; out+4!=0 => trouvé
// ⚠ Ces quatre-là étaient INTERVERTIS (corrigé le 2026-07-28) : la branche SKILL
// ouvrait un « itemdb::kItemDescWndId » et la branche OBJET un « itemdb::kSkillDescWndId ». Le
// comportement était juste — c'est l'appariement id/message qui compte, et il
// n'a pas bougé — mais les noms disaient le contraire du code, à rebours des
// huit autres fichiers du projet et de character_sheet.cc:1713 qui documente
// explicitement « 0x2e ≠ 0xc, qui est celle des objets ».
constexpr int kSkillInfoSize  = 0x100; // SkillInfo ~0xf8 o (2 std::string @ +0x2c / +0x44)
constexpr int kSkillInfoFound = 0x04;  // out+0x04 != 0 => skill trouvé
constexpr int kSkillStr0      = 0x2c;  // std::string resname
constexpr int kSkillStr1      = 0x44;  // std::string nom

// ItemSkillInfo standalone (ctor+SetId par id, INDÉPENDANT de l'inventaire courant — au contraire
// de kGetSkillInfo/FUN_00d5a980 qui exige une quantité inventaire > 0). Utilisé pour la description
// d'un OBJET grisé (épuisé) dans la barre : cf. project_item_skill_desc_window_re, section
// "Accès STANDALONE". +0x5c = flag skill (laissé à 0 = objet par le ctor / FUN_006a5ff0).
constexpr int kItemSkillInfoSize = 0x100;  // struct ~0xf4 o

// ---- tooltip survol (réplique UIShortCutWnd OnMouseMove 0x008f7f50) ----
// OBJET (rec[0]==1) : nom via la DB item (FUN_006a0d40, table 0x01255130 ; record+0x04 = nom EN,
//   +0x08 = localisé). Les objets y sont chargés au boot (FUN_006a4e20 parse item.txt etc.).
// SKILL (rec[0]==0) : les ids de skills NE SONT PAS dans la DB item — VÉRIFIÉ live (traversée de
//   l'arbre RB de 0x01255130 : clé 29 Inc AGI introuvable ; la DB ne contient que des objets). Le
//   nom vient de Lua GetSkillName(id) via le wrapper __cdecl FUN_0073a1f0 (format "d>s", renvoie
//   "Unknown-Skill" si l'id est inconnu). C'est exactement la source de la barre native pour les
//   skills standard (les skills custom ~12622 sont, eux, aussi dans la DB item).
constexpr int kItemNameEn  = 0x04;  // record+0x04 = nom anglais (ASCII, propre à l'affichage)
constexpr int kItemNameLoc = 0x08;  // record+0x08 = nom localisé (CP949, repli)

using SetVisible_t = void  (__fastcall*)(void*, void*, int);
using SetSlot_t    = void  (__fastcall*)(void*, void*, int, int, int, int, int);
using GetOption_t  = int   (__thiscall*)(void*, int);
using SetOption_t  = void  (__thiscall*)(void*, int, int, int);  // (mgr,key,val,0)
using GetSkillInfo_t = void (__fastcall*)(void*, void*, void*, int, int);  // (mgr,edx,out,id,gate)
using StrFree_t      = void (__fastcall*)(void*);                          // (ecx=std::string base)
using ShowTip_t      = void (__fastcall*)(void*, void*, const char*, int, int, unsigned, unsigned char, char);
using ItemDbGet_t    = void* (__cdecl*)(int, void*);                       // (nameid, table) -> record / sentinelle
using ItemSkillInfoCtor_t  = void* (__fastcall*)(void*);                  // (ecx=this) -> this
using ItemSkillInfoSetId_t = void  (__thiscall*)(void*, int);             // (this, id)

struct SlotRec { bool valid; uint8_t type; uint32_t id; int16_t level; };

inline void* ShortCutWnd() {
  return *reinterpret_cast<void**>(uiwnd::kUIWindowMgrAddr + kMgrShortCutPtr);
}

void EnsureCreated() {
  if (ShortCutWnd()) return;  // déjà créée (cas normal) ou recréée ci-dessous
  uiwnd::MakeWindow(kShortCutId);
}
void RebuildSlotPtrs(void* w) {  // OnMsg 0x17 : reconstruit this+0xc4 depuis les globals
  uiwnd::OnMsg(w, kMsgRebuild, 0, 0, 0, 0);
}
void HideNative(void* w) {  // this+0x28 = 0 + délink draw-list ; ne détruit PAS l'objet
  uiwnd::Vf<SetVisible_t>(w, kVfSetVisible)(w, nullptr, 0);
}
void ShowNative(void* w) {
  uiwnd::Vf<SetVisible_t>(w, kVfSetVisible)(w, nullptr, 1);
}
int CurrentTab() {
  return reinterpret_cast<GetOption_t>(kGetOption)(
             reinterpret_cast<void*>(rag::kSessionAddr), 10) ? 1 : 0;
}

// ── Anti-flicker : empêcher tout RÉ-AFFICHAGE de la native (barre + boutons) ──
// Cacher la barre par polling en OnRenderUI (EndScene) arrive TROP TARD : la passe UI du jeu a déjà
// dessiné CE frame ; on ne re-cache qu'au frame suivant. Quand @refresh/@load fait ré-afficher
// UIShortCutWnd, on voit donc 1 frame de native (flicker).
//
// Point d'ancrage CORRECT (validé RE) : UIWnd_SetVisible (0x009030c0, vtable+0x38) écrit this+0x28
// PUIS LIE (FUN_00a4ccf0) / délie (FUN_00a2e5c0) toute la fenêtre dans le set de rendu (mgr+0x194).
// La barre native ET ses BOUTONS ENFANTS (onglets 1/2, croix de fermeture, bascule barre d'items)
// forment UN sous-arbre lié d'un bloc -> ils apparaissent/disparaissent ensemble. Donc : tant que le
// plugin veut la native cachée, on FORCE tout show de NOTRE fenêtre à hidden -> elle n'est jamais
// re-liée -> aucun dessin (barre NI boutons), zéro flicker. C'est le seul niveau suffisant.
//
// (Le hook OnDraw ci-dessous reste en défense : il n'annule QUE le contenu-slots — pas les boutons
//  enfants — donc insuffisant seul, mais utile si la fenêtre restait liée par une voie hors SetVisible.)
constexpr uintptr_t kOnDraw = 0x008f5800;  // UIShortCutWnd::OnDraw (vtable+0x50) — contenu slots
using OnDraw_t = void (__fastcall*)(void*, void*);
OnDraw_t g_orig_shortcut_draw = nullptr;
bool     g_suppress_native_draw = false;   // MAJ chaque frame par OnRenderUI ; lu par les 2 hooks
void __fastcall ShortCutDrawHook(void* self, void* edx) {
  if (g_suppress_native_draw) return;      // supprime le contenu natif (défense)
  if (g_orig_shortcut_draw) g_orig_shortcut_draw(self, edx);
}

using SetVisibleFn_t = void (__thiscall*)(void*, int);
SetVisibleFn_t g_orig_setvisible = nullptr;
void __fastcall SetVisibleHook(void* self, void* /*edx*/, int visible) {
  // self == ShortCutWnd() : fn de base PARTAGÉE -> on ne touche QUE la barre de raccourcis.
  if (visible && g_suppress_native_draw && self == ShortCutWnd()) visible = 0;
  if (g_orig_setvisible) g_orig_setvisible(self, visible);
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
uint8_t g_itemStore[SkillBar::kItemSlotMax * 7] = {};  // records 7o (type/id/level), plugin-owned
void WriteSlotRecord(int region, int slot, bool is_item, uint32_t id, int level);  // fwd (défini plus bas)
const RegionDef kRegions[3] = {  // const (pas constexpr) : la base items = adresse runtime de g_itemStore
    {0x015fa850, 36,  0,  0, "Onglet 1"},
    {0x015fa94c, 36,  1,  3, "Onglet 2"},
    {reinterpret_cast<uintptr_t>(g_itemStore), SkillBar::kItemSlotMax, -1, -1, "Items"},
};
inline bool RegionIsItems(int r) { return r == 2; }

// ── Un raccourci ne déclenche QUE ce que le joueur VOIT ──────────────────────
// Les cases vivent dans les GLOBALS du client, pas dans nos barres : masquer une
// barre — ou réduire son « Nb slots » — n'efface rien côté natif. La touche liée
// au slot 12 continuait donc de lancer la compétence du slot 12 alors que plus
// aucune case ne la montrait : une compétence partie toute seule, sans rien à
// l'écran pour l'expliquer, et l'inverse exact de ce que le joueur a réglé.
//
// On filtre à la SOURCE plutôt qu'à la touche : UIShortCutWnd::OnMsg case 0x29
// est le point où aboutissent F1-F9 (RE : `this+0xc4[col + 9*row]`, donc
// slot = p2 + 9*p3) ET tout autre chemin d'activation natif. Filtrer là couvre
// les deux onglets d'un coup, sans avaler la touche elle-même — la frappe reste
// disponible pour le chat, les fenêtres et les autres modules.
//
// `g_slot_drawn` est refait à chaque frame par SkillBar::RefreshDrawnSlots().
static_assert(SkillBar::kItemSlotMax <= kMaxSlots, "g_slot_drawn trop étroit");
bool g_slot_drawn[3][kMaxSlots] = {};
bool g_slot_filter_on  = false;  // filtre armé (module actif, en jeu, native cachée)
bool g_self_activation = false;  // notre propre ActivateSlot -> jamais filtré
// Case visée par notre propre activation. Indispensable pour la LIRE : la barre
// d'objets détourne this+0xc4[0] le temps de l'appel (cf. UseItemSlot), donc les
// col/row reçus par le OnMsg ne désignent alors pas la vraie case.
int g_active_region = -1;
int g_active_slot   = -1;

SlotRec ReadSlot(int region, int i);  // fwd (défini plus bas)

constexpr uintptr_t kShortCutOnMsg = 0x00901310;  // UIShortCutWnd::OnMsg (vtable+0x94)
// __thiscall à SIX arguments pile (`retn 18h`) : ecx=this, edx ignoré, puis
// arg0 / msg / p2..p5 — exactement la forme qu'envoie uiwnd::OnMsg.
using ShortCutOnMsg_t = int (__fastcall*)(void*, void*, int, int, int, int, int, int);
ShortCutOnMsg_t g_orig_shortcut_onmsg = nullptr;
int __fastcall ShortCutOnMsgHook(void* self, void* edx, int arg0, int msg,
                                 int p2, int p3, int p4, int p5) {
  if (!g_orig_shortcut_onmsg) return 0;

  // Quelle case ce message active-t-il ? La nôtre s'annonce (et doit s'annoncer :
  // le détournement de la barre d'objets rend p2/p3 muets) ; celle du natif ne
  // peut venir que de l'onglet affiché, seul que le 0x29 sache lire.
  int region = -1, slot = -1;
  if (msg == kMsgUseSlot) {
    region = g_self_activation ? g_active_region : CurrentTab();
    slot   = g_self_activation ? g_active_slot   : p2 + 9 * p3;
    if (g_slot_filter_on && !g_self_activation &&
        (slot < 0 || slot >= kMaxSlots || !g_slot_drawn[region][slot]))
      return 0;
  }

  const int ret = g_orig_shortcut_onmsg(self, edx, arg0, msg, p2, p3, p4, p5);

  // Un OBJET vient d'être utilisé : QuickCast peut vouloir enchaîner tant que la
  // touche reste enfoncée (le client, lui, ignore l'auto-répétition clavier).
  // Après coup, pour ne rien annoncer que le natif aurait refusé. Ce point voit
  // TOUTES les voies d'activation — touche de l'onglet affiché, routage de
  // l'autre onglet, barre d'objets, clic sur la case — d'où le choix de l'y
  // mettre plutôt que dans OnKeyDown, qui n'en connaît qu'une.
  if (msg == kMsgUseSlot && region >= 0 && region < 3) {
    const SlotRec rec = ReadSlot(region, slot);
    if (rec.valid && rec.type == 0) {  // 0 = OBJET (convention du record, cf. en-tête)
      if (auto* quick_cast = Bourgeon::Instance().quick_cast())
        quick_cast->OnUseItemSlot(region, slot, rec.id);
    }
  }
  return ret;
}

// Assignation/vidage d'un slot d'ITEM : SkillMgr_SetShortCutItemSlot(mgr, id, slot) (__thiscall ;
// id==0 vide ; écrit g_ShortCutItemSlotExt, type=0/objet). Use objet (réplique branche objet de
// OnMsg 0x29) : dispatcher (*(0x0121333c)) ->vtable[0x18](0x71, info, count, 0, 0) (__thiscall).
using SetItemSlot_t = void  (__thiscall*)(void*, int, int);
using DispUse_t     = void  (__thiscall*)(void*, int, void*, int, int, int);
using GetInvItemU_t = void* (__stdcall*)(void*, int);   // itemdb::kFillInfoByIdAddr ; found out+0x04, qty out+0x10
constexpr uintptr_t kSetItemSlot    = 0x00da8f90;

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
    void* mgr = reinterpret_cast<void*>(rag::kSessionAddr);
    const int lock = reinterpret_cast<GetOption_t>(kGetOption)(mgr, 5);
    if (lock >= 1) reinterpret_cast<SetOption_t>(kSetOption)(mgr, 5, 0, 0);
    uint8_t** arr = reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(w) + kSlotArr);  // this+0xc4
    uint8_t* saved = arr[0];
    arr[0] = itemRec;                                                  // détourne le slot 0 -> item
    uiwnd::OnMsg(w, kMsgUseSlot, 0, 0, 0, 0);  // OnMsg 0x29 col=0,row=0 -> arr[0]
    arr[0] = saved;                                                    // restaure
    if (lock >= 1) reinterpret_cast<SetOption_t>(kSetOption)(mgr, 5, lock, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Active le slot `slot` de la région `region`. SKILLS (onglets) : exactement comme une touche F
// (OnMsg 0x29), MAIS 0x29 ne lit que this+0xc4 = onglet ACTIF -> on bascule temporairement sur
// l'onglet de la barre (SetOption 10 + OnMsg 0x17) puis on restaure. ITEMS : UseItemSlot.
// Gate option #5 (UI-lock natif, >=1 bloque) neutralisé le temps de l'appel puis restauré. SEH (Lua/jeu).
void ActivateSlotRaw(int region, int slot) {
  if (RegionIsItems(region)) { UseItemSlot(slot); return; }
  void* w = ShortCutWnd();
  if (!w) return;
  void* mgr = reinterpret_cast<void*>(rag::kSessionAddr);
  const int tab = kRegions[region].tab;
  const int cur = CurrentTab();
  const int lock = reinterpret_cast<GetOption_t>(kGetOption)(mgr, 5);
  __try {
    if (lock >= 1) reinterpret_cast<SetOption_t>(kSetOption)(mgr, 5, 0, 0);
    if (tab != cur) {  // bascule sur l'onglet de la barre + reconstruit this+0xc4
      reinterpret_cast<SetOption_t>(kSetOption)(mgr, 10, tab, 0);
      uiwnd::OnMsg(w, kMsgRebuild, 0, 0, 0, 0);
    }
    uiwnd::OnMsg(w, kMsgUseSlot, slot % 9, slot / 9, 0, 0);
    if (tab != cur) {  // restaure l'onglet d'origine
      reinterpret_cast<SetOption_t>(kSetOption)(mgr, 10, cur, 0);
      uiwnd::OnMsg(w, kMsgRebuild, 0, 0, 0, 0);
    }
    if (lock >= 1) reinterpret_cast<SetOption_t>(kSetOption)(mgr, 5, lock, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Activation VOULUE par nous (clic sur une case, interception clavier de l'autre
// onglet) : elle passe par le même OnMsg 0x29 que les touches F, donc elle doit
// être exemptée du filtre — UseItemSlot détourne d'ailleurs this+0xc4[0], dont
// le slot 0 n'a aucune raison d'être dessiné. Fonction séparée à dessein : le SEH
// d'ActivateSlotRaw interdit tout objet à destructeur dans son corps, donc pas de
// garde RAII là-bas.
void ActivateSlot(int region, int slot) {
  // 🔴 COMPÉTENCE D'HOMONCULE : elle ne peut PAS passer par le natif. L'OnMsg 0x29
  // de UIShortCutWnd résout l'id dans le bundle du PERSONNAGE (0x00D7FA90) ; un id
  // 8001+ y est introuvable, la case ne fait donc rien du tout. Le chemin correct
  // prend la fiche dans la liste de l'homoncule avant d'appeler le dispatcher —
  // c'est ce que fait rag::homun::LaunchSkill (cf. ragnarok/homunculus.h).
  if (!RegionIsItems(region)) {
    const SlotRec r = ReadSlot(region, slot);
    if (r.valid && r.type != 0 && rag::homun::IsSkillId(static_cast<int>(r.id))) {
      rag::homun::LaunchSkill(static_cast<int>(r.id), r.level);
      return;
    }
  }
  g_self_activation = true;
  g_active_region   = region;  // la case RÉELLE, que les col/row du 0x29 ne diront pas
  g_active_slot     = slot;
  ActivateSlotRaw(region, slot);
  g_active_region = g_active_slot = -1;
  g_self_activation = false;
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
  p.isSkill = type;            // 0=OBJET, 1=SKILL (hotkey_data.isSkill côté serveur) ; round-trip brut
  p.id = id;
  p.count = static_cast<uint16_t>(level);
  Bourgeon::Instance().SendPacket(reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}

// Vide le slot i de la région (skills -> SetShortCutSlot + persist serveur ; items -> store plugin).
void ClearSlot(int region, int i) {
  void* mgr = reinterpret_cast<void*>(rag::kSessionAddr);
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
  void* mgr = reinterpret_cast<void*>(rag::kSessionAddr);
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
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr, info, static_cast<int>(nameid), 1);
    if (*reinterpret_cast<int*>(info + kSkillInfoFound) != 0)
      cnt = *reinterpret_cast<int*>(info + 0x10);
    reinterpret_cast<StrFree_t>(rag::kStdStringDtorAddr)(info + kSkillStr1);
    reinterpret_cast<StrFree_t>(rag::kStdStringDtorAddr)(info + kSkillStr0);
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
//   détruites (rag::kStdStringDtorAddr, sinon fuite si nom > 15 car.). SEH : on touche Lua + globals.
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
    reinterpret_cast<GetHotKey_t>(userhotkey::kGetHotKeyAddr)(buf, category, slot);
    CopyMsvcString(buf + 0x08, out, n);                  // out+0x08 = nom de la touche
    reinterpret_cast<StrFree_t>(rag::kStdStringDtorAddr)(buf + 0x08);   // détruit les 2 std::string du wrapper
    reinterpret_cast<StrFree_t>(rag::kStdStringDtorAddr)(buf + 0x20);
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
    reinterpret_cast<GetHotKey_t>(userhotkey::kGetHotKeyAddr)(buf, category, slot);
    *kc1 = *reinterpret_cast<int*>(buf + 0x00);
    *kc2 = *reinterpret_cast<int*>(buf + 0x04);
    reinterpret_cast<StrFree_t>(rag::kStdStringDtorAddr)(buf + 0x08);
    reinterpret_cast<StrFree_t>(rag::kStdStringDtorAddr)(buf + 0x20);
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

// Fraction de cooldown restante [0,1] (0 = prêt). Match exact par id (les
// pseudo-ids de groupe 0x241/9999 = polish v2).
//
// La source est ro::SkillCooldownFraction, alimentée par le paquet serveur
// ZC_SKILL_POSTDELAY. Lire la liste native g_ShortCutCooldownList — ce que
// faisait cette fonction — ne pouvait PAS marcher ici : le handler natif du
// paquet (0x00cd60b0) ne crée un nœud que si le skill figure dans la liste
// d'icônes reconstruite à chaque dessin de la barre NATIVE, or on la cache. La
// liste restait donc vide et aucune case ne s'assombrissait jamais — visible
// surtout sur les compétences de guilde, aux cooldowns de plusieurs minutes.
// Détails du RE dans ragnarok/skill_cooldowns.h.
float CooldownFraction(uint32_t skillId) {
  return ro::SkillCooldownFraction(static_cast<uint16_t>(skillId));
}

// ── Icônes (recette menu_icons.cc : TexMgr -> BGRA -> Overlay_CreateTextureARGB) ──
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;
// Chemin d'icône construit par fonctions natives (RE) :
//   OBJET : ro::texmgr::kBuildItemIconPath __stdcall(id_str, out, identified)
//           (resname via ResolveItemResNameById/DB objets -> marche même HORS inventaire). C'est la clé :
//           les .bmp du GRF sont nommés par resource-name CP949, pas par id.
//   SKILL : Lua_GetSkillIdName 0x0073a140 __cdecl(id) -> identifiant du skill (ex. "AL_BLESSING"),
//           puis "유저인터페이스\item\<idname>.bmp". C'est la source d'icône NATIVE (le builder d'effet
//           0x00bda890 fait le même sprintf) et surtout INDÉPENDANTE de l'état APPRIS : elle lit la DB
//           Lua SkillInfoList, pas la liste apprise g_SkillInfoMgr+0xc. (L'ancien getter 0x00d7fa90 lit
//           la liste apprise via FUN_00737e00 -> resname VIDE pour un skill d'une AUTRE classe -> icône
//           perdue après relog sur un perso GM multi-classe. Le nom du slot survivait car il vient de
//           GetSkillName Lua, elle aussi indép. de l'appris.) Repli sur 0x00d7fa90 si idname invalide.
using GetInvInfo_t = void* (__stdcall*)(void*, int);          // itemdb::kFillInfoByIdAddr (out, id)

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
    // L'enveloppe porte la signature à TROIS arguments, tranchée au
    // désassemblage : la déclarer ici à deux — ce que faisait ce fichier —
    // laissait la fonction prendre son drapeau `identified` dans de la pile non
    // initialisée. Cf. ro::texmgr::BuildItemIconPath.
    ro::texmgr::BuildItemIconPath(static_cast<unsigned>(id), out);
    if (LooksUnknown(out)) { out[0] = '\0'; return false; }  // pas de ressource -> ne pas charger
    return out[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
}
bool SkillPath(int id, char* out, int n) {
  __try {
    // 1) Source d'icône NATIVE, indépendante de l'état appris : Lua GetSkillIdName(id) -> identifiant
    //    (ex. "AL_BLESSING"). Marche pour un skill d'une AUTRE classe (perso GM multi-classe) alors que
    //    l'ancien getter renvoyait vide -> icône perdue après relog. Voir le bloc de commentaire ci-dessus.
    const char* idn = lua::SkillIdName(id);
    if (idn && idn[0] && !LooksUnknown(idn)) {                      // rejette "Zero Skill"/"Unknown"
      std::snprintf(out, n, "%s\\item\\%s.bmp", ro::uipath::kUiRoot, idn);
      return out[0] != '\0';
    }
    // 2) Repli : ancien getter 0x00d7fa90 (resname de la liste APPRISE à +0x20). Utile si GetSkillIdName
    //    ne couvre pas un id custom mais que le skill est appris.
    alignas(8) uint8_t info[0xA0] = {};
    reinterpret_cast<GetInvInfo_t>(itemdb::kFillInfoByIdAddr)(info, id);          // __stdcall(out, id)
    const char* rn = *reinterpret_cast<const char**>(info + 0x20);  // resname (déréférencé)
    if (rn && rn[0] && !LooksUnknown(rn)) {                         // rejette "Unknown-Skill"
      std::snprintf(out, n, "%s\\item\\%s.bmp", ro::uipath::kUiRoot, rn);
      return out[0] != '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return false;
}
void* UploadBmp(const char* path) {
  void* t = ro::texmgr::LoadResource(path);
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
  // SkillPath (itemdb::kFillInfoByIdAddr, qui aiguille par PLAGE d'id) ;
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
// skill rec0!=0) : itemdb::kFillInfoByIdAddr(out, id) -> out+0x04 (found) != 0 = connu/
// utilisable (un skill lié à un item disparu repasse à found=0 -> le natif cesse de le dessiner).
// Nettoyage via FUN_00739cd0 (même signature __fastcall(void*) que StrFree_t) -> pas de fuite malgré
// l'appel par frame. SEH (POD only). id = id canonique stocké (rec+1), comme OnDraw.
bool SkillKnown(uint32_t id) {
  // Une compétence d'HOMONCULE n'est pas dans le bundle du personnage : le getter
  // natif la dirait toujours inconnue et la case resterait grisée en permanence.
  // On la cherche là où elle est — la liste de l'homoncule.
  if (rag::homun::IsSkillId(static_cast<int>(id)))
    return rag::homun::SkillLevel(static_cast<int>(id)) > 0;
  bool known = false;
  __try {
    alignas(8) uint8_t info[0xC0] = {};
    reinterpret_cast<GetInvInfo_t>(itemdb::kFillInfoByIdAddr)(info, static_cast<int>(id));
    known = (*reinterpret_cast<int*>(info + 0x04) != 0);
    reinterpret_cast<StrFree_t>(itemdb::kInfoDtorAddr)(info);  // = FUN_00739cd0 (cleanup de la struct)
  } __except (EXCEPTION_EXECUTE_HANDLER) { known = false; }
  return known;
}

// (Le pont drag NATIF -> slot ImGui a disparu avec ses deux sources, le grimoire
// et l'inventaire natifs : ils appartiennent au même groupe « Interface moderne »
// que cette barre, donc leurs fenêtres ne naissent plus quand elle est active.
// Le format de la charge de drag reste documenté dans docs/skill_tree_re.md.)

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
      void* mgr = uiwnd::Mgr();
      if (rec[0] != 0) {  // ── SKILL (rec[0]==1) ──
        void* wnd = uiwnd::MakeWindow(itemdb::kSkillDescWndId);
        if (wnd) {
          if (*reinterpret_cast<int*>(reinterpret_cast<char*>(wnd) + itemdb::kSkillDescShownId) == id) {
            uiwnd::CloseWindow(itemdb::kSkillDescWndId);  // bascule
          } else {
            uiwnd::OnMsg(wnd, itemdb::kSkillDescMsgSet, id, 0, 0, 0);
            uiwnd::SetPos(wnd, mx, my);
          }
        }
      } else {            // ── OBJET (rec[0]==0) ──
        alignas(8) uint8_t info[kSkillInfoSize] = {};
        reinterpret_cast<GetSkillInfo_t>(kGetSkillInfo)(
            reinterpret_cast<void*>(rag::kSessionAddr), nullptr, info, id, 1);
        void* wnd = uiwnd::MakeWindow(itemdb::kItemDescWndId);
        if (wnd) {
          uiwnd::OnMsg(wnd, itemdb::kItemDescMsgSet,
                                     static_cast<int>(reinterpret_cast<uintptr_t>(info)), 0, 0, 0);
          uiwnd::SetPos(wnd, mx, my);
        }
        reinterpret_cast<StrFree_t>(rag::kStdStringDtorAddr)(info + kSkillStr1);
        reinterpret_cast<StrFree_t>(rag::kStdStringDtorAddr)(info + kSkillStr0);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Survol d'un slot rempli -> tooltip ImGui avec le nom (réplique le NOM de la barre native
// UIShortCutWnd::OnMouseMove 0x008f7f50) : OBJET = DB item (FUN_006a0d40) ; SKILL = Lua
// GetSkillName(id) (FUN_0073a1f0) car les ids skills sont absents de la DB item (prouvé par
// traversée RB live de 0x01255130). Rendu via ImGui::BeginTooltip (le tooltip natif FUN_00a753d0
// passait SOUS la barre ImGui + 1 frame de retard). SEH (POD only ; le rendu hors __try).
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
              reinterpret_cast<ItemDbGet_t>(itemdb::kLookupAddr)(id, reinterpret_cast<void*>(itemdb::kTableAddr)));
          if (dbrec && reinterpret_cast<uintptr_t>(dbrec) != itemdb::kNilAddr) {
            const char* en  = *reinterpret_cast<const char**>(dbrec + kItemNameEn);
            const char* loc = *reinterpret_cast<const char**>(dbrec + kItemNameLoc);
            const char* name = (en && *en) ? en : loc;
            if (name && *name) std::snprintf(nm, sizeof(nm), "%s", name);
          }
        } else {
          // SKILL : nom via Lua GetSkillName(id) — les ids skills sont absents de la DB item (les
          // skills custom y sont parfois, mais GetSkillName couvre TOUT, source que la fenêtre de
          // skills/le tooltip natif utilisent). "" ou "Unknown-Skill" => repli ci-dessous.
          const char* sn = lua::SkillName(id);
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
  if (is_item) std::snprintf(out, sizeof(out), i18n::Tr("%s (ID: %d)"), nm, id);
  else         std::snprintf(out, sizeof(out), i18n::Tr("%s - Lv: %d (ID: %d)"), nm, level, id);
  // Habillage : le style ImGui par défaut donne un rectangle à angles vifs, à marge
  // asymétrique (WindowPadding x != y). On force des coins arrondis, un liseré discret
  // et un padding ÉGAL sur les deux axes -> même marge en haut/bas qu'à gauche/droite.
  constexpr float kTipPad = 4.0f, kTipRound = 6.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kTipPad, kTipPad));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kTipRound);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 18, 22, 240));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(95, 95, 105, 190));
  if (ImGui::BeginTooltip()) {
    ImGui::TextUnformatted(out);  // Unformatted : un nom d'objet peut contenir un '%'
    ImGui::EndTooltip();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
}

// Callback ImGui : applique le filtre texture choisi (POINT net / LINEAR flou)
// aux dessins suivants de la barre. Réglage miroité ici pour le callback.
bool g_sb_bilinear = false;
void SbApplyFilter(const ImDrawList*, const ImDrawCmd*) { Overlay_SetTextureFilter(g_sb_bilinear); }

}  // namespace

SkillBar::SkillBar() {
  // Anti-flicker au ré-affichage (@refresh/@load). Voir le bloc de commentaire sur SetVisibleHook /
  // ShortCutDrawHook. Adresses statiques dans l'exe -> installables au load.
  auto& hm = hooking::HookManager::Instance();
  // Principal : empêche la native (barre + boutons enfants) d'être re-liée tant qu'on la veut cachée.
  g_orig_setvisible = reinterpret_cast<SetVisibleFn_t>(
      hm.SetHook(hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(uiwnd::kSetVisibleAddr),
                 reinterpret_cast<uint8_t*>(&SetVisibleHook)));
  // Défense : annule le contenu-slots natif si la fenêtre restait liée par une voie hors SetVisible.
  g_orig_shortcut_draw = reinterpret_cast<OnDraw_t>(
      hm.SetHook(hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kOnDraw),
                 reinterpret_cast<uint8_t*>(&ShortCutDrawHook)));
  // Filtre d'activation : une case non dessinée ne répond plus à son raccourci.
  g_orig_shortcut_onmsg = reinterpret_cast<ShortCutOnMsg_t>(
      hm.SetHook(hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kShortCutOnMsg),
                 reinterpret_cast<uint8_t*>(&ShortCutOnMsgHook)));
}

// Recalcule les cases DESSINÉES de la frame (barre visible ET slot dans la plage
// affichée), telles que les parcourt DrawBar. C'est la seule définition de « visible
// à l'écran » que le filtre de raccourci consulte : dessin et activation restent
// donc décrits au même endroit, comme le reste du module.
void SkillBar::RefreshDrawnSlots() {
  std::memset(g_slot_drawn, 0, sizeof(g_slot_drawn));
  for (int b = 0; b < kBarCount; ++b) {
    const BarCfg& bc = bars_[b];
    if (!bc.visible) continue;
    const int maxSlots = kRegions[b].count;
    const int count    = std::min(bc.slot_count, maxSlots);
    for (int k = 0; k < count; ++k) {
      const int slot = bc.first_slot + k;
      if (slot >= maxSlots) break;
      if (slot >= 0) g_slot_drawn[b][slot] = true;
    }
  }
}

void SkillBar::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
  if (!in_game_) {
    native_hidden_ = false; last_tab_ = -1; items_restored_ = false;
    ForgetDrag();                    // un glisser interrompu par un changement de carte
                                     // ne décidera de rien au retour en jeu
    g_suppress_native_draw = false;  // hors jeu : ne pas bloquer le dessin natif
    g_slot_filter_on = false;        // ni filtrer des activations qu'on ne dessine plus
  }
  FlushIconCache();  // recharge les icônes au changement de zone
}

// Lit les slots d'items du store plugin (g_itemStore) -> item_slots_ (pour la sauvegarde yaml). SEH.
// Garde : avant la restauration en jeu le store est vide -> on garde la valeur chargée du yaml.
void SkillBar::SnapshotItemSlots() {
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

// Rejoue l'utilisation d'une case d'OBJET pour QuickCast (touche maintenue). On
// repasse par ActivateSlot, donc par la voie EXACTE de la touche : rien à tenir
// en parallèle du client, et ses refus restent les siens.
//
// Le contrat tient dans le booléen : `false` = il n'y a plus rien à répéter, la
// boucle appelante s'arrête. Deux raisons, et elles comptent toutes les deux —
//   • la case a changé sous nos pieds (vidée, réarrangée, autre objet) : rejouer
//     « ce qu'il y a maintenant à cet endroit » utiliserait autre chose que ce
//     que le joueur a lancé ;
//   • il n'en reste plus en sac : le serveur refuserait, et la case est déjà
//     grisée à l'écran.
// (La quantité est celle du client, donc en retard d'un aller-retour : au pire
// un usage de trop part sur le dernier exemplaire, que le serveur écarte.)
bool SkillBar::RepeatItemSlot(int region, int slot, uint32_t nameid) {
  if (!in_game_) return false;
  if (region < 0 || region >= kBarCount) return false;
  // Barre rangée entre-temps (masquée, « Nb slots » réduit) : la case n'est plus
  // à l'écran, donc plus une source d'action. Même règle que le filtre de
  // raccourci — sauf qu'ici c'est à nous de l'appliquer, une activation qui vient
  // de nous étant par construction exemptée de ce filtre.
  if (g_slot_filter_on &&
      (slot < 0 || slot >= kMaxSlots || !g_slot_drawn[region][slot]))
    return false;
  const SlotRec rec = ReadSlot(region, slot);
  if (!rec.valid || rec.type != 0 || rec.id != nameid) return false;
  if (GetItemLiveCount(nameid) <= 0) return false;
  ActivateSlot(region, slot);
  return true;
}

void SkillBar::OnKeyDown(unsigned long vkey, int, int) {
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
  // Slot lié à (vkey, pressedMod) qui soit OCCUPÉ *et* DESSINÉ, ou -1. Le test
  // « dessiné » n'est pas un simple confort d'affichage : une case hors des barres
  // visibles est de toute façon arrêtée par ShortCutOnMsgHook, donc la croire
  // servie par le natif reviendrait à ne rien déclencher du tout alors que l'autre
  // onglet, lui, montre bien une case pour cette touche.
  auto occupiedSlot = [&](int region) -> int {
    for (const SlotKeyBind& b : g_keyCache[region])
      if (b.mainK == static_cast<int>(vkey) && b.modK == pressedMod &&
          b.slot >= 0 && b.slot < kMaxSlots && g_slot_drawn[region][b.slot] &&
          ReadSlot(region, b.slot).valid)
        return b.slot;
    return -1;
  };
  const int cur = CurrentTab();
  if (occupiedSlot(cur) != -1) return;  // onglet actif occupé pour cette touche -> le natif s'en charge
  const int other = 1 - cur;            // (barre masquée => aucun slot dessiné => -1)
  const int s = occupiedSlot(other);
  // ⚠ Cette activation-ci part depuis un OnKeyDown, donc AVANT que le jeu n'ait
  // dispatché la frappe. QuickCast doit avoir vu la touche pour pouvoir répéter
  // l'objet : il le peut parce qu'il est enregistré AVANT nous dans Bourgeon
  // (LoadPlugins), donc son OnKeyDown a déjà noté la frappe quand on arrive ici.
  if (s != -1) ActivateSlot(other, s);
}

void SkillBar::OnRenderUI() {
  if (!in_game_) return;

  void* w = ShortCutWnd();

  // ── Bascule cacher / restaurer la barre native selon l'activation ──────────
  const bool want_hidden = enabled_ &&
      Bourgeon::Instance().client().session().aid() != 0;
  g_suppress_native_draw = want_hidden;  // arme le hook OnDraw AVANT la passe UI du frame suivant
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

  // ── Cases dessinées de la frame -> filtre des raccourcis (ShortCutOnMsgHook) ──
  // Calculé AVANT le dessin, et même quand la barre native reprend la main : le
  // filtre doit alors se désarmer, sans quoi les touches resteraient bridées par
  // une configuration de barres qui ne s'affiche plus.
  RefreshDrawnSlots();
  g_slot_filter_on = native_hidden_ && w != nullptr;

  // ── Dessin des barres ImGui + drag natif au-dessus ─────────────────────────────
  if (native_hidden_ && w) {
    // Suivi du glisser d'une case AVANT le dessin : l'aperçu qui suit le curseur
    // doit porter la croix rouge dès la frame où il quitte les barres, sinon
    // l'avertissement arrive après le geste qu'il est censé annoncer.
    UpdateDragRemoval();
    for (int b = 0; b < kBarCount; ++b)
      if (bars_[b].visible) DrawBar(b);  // 3 barres fixes (0=Onglet1, 1=Onglet2, 2=Items)

    // (Le redessin de l'icône d'un glisser NATIF au-dessus des barres a disparu :
    // ses deux sources — le grimoire natif et l'inventaire natif — ne naissent
    // plus, la barre étant membre du même groupe « Interface moderne » qu'eux.)
  } else {
    // Barres rendues au natif (ou fenêtre absente) : on oublie tout glisser en
    // cours. Son état survivrait sinon jusqu'au prochain, qui serait alors lu
    // avec la décision de l'ancien.
    ForgetDrag();
  }
}

// (Plus de HandleNativeDrop : il accueillait un glisser NATIF venu du grimoire ou
// de l'inventaire. Ces deux fenêtres appartiennent au même groupe « Interface
// moderne » que cette barre — quand elle est active, elles le sont aussi et leurs
// natives ne naissent plus. Remplir une case passe par le glisser ImGui.)

// ---- contenu des réglages (fenêtre standalone ²/~ ET onglet MoonlightUi "Barre d'action") -----
void SkillBar::DrawSettings() {
  bool changed = false;
  // Cette barre est membre du groupe « Interface moderne » (tout-ImGui ou
  // tout-natif, plus de mixe) : `SetModernInterface` écrit `enabled_` avec les
  // autres, et la persistance passe par dirty_ (drainé par MoonlightUi, dont le
  // SaveSettings réécrit tous les flags du groupe à jour).
  //
  // 🔴 Plus de CASE ici : l'interrupteur du groupe vit uniquement en tête de
  // l'en-tête « Interface de jeu ». Cinq sections en portaient une copie
  // synchronisée, ce qui laissait croire à un réglage local alors qu'il en basculait
  // douze. Ce que la bascule apporte ICI reste dit — mais comme une DESCRIPTION :
  // section grisée ou non, elle doit donner envie d'essayer.
  ImGui::TextDisabled(i18n::Tr("Fenêtre du groupe « Interface moderne »"));
  SameLine(); HelpMarker(
      i18n::Tr("Désactivé = barres classiques.\nActivé = barres modernes entièrement "
      "customisables."));

  SeparatorText(i18n::Tr("Réglages généraux"));
  ImGui::BeginDisabled(!enabled_);
  changed |= ro::RoCheckbox(i18n::Tr("Verrouiller"), &locked_);
  SameLine(); HelpMarker(
      i18n::Tr("Coche = barres fixe. Décoche = glisser n'importe où pour la déplacer.\n"
      "Verrouillée, les slots restent utilisables et réarrangeables."));

  // ── Réglages COMMUNS (taille/espacement sont PAR BARRE, dans chaque section ci-dessous) ──
  changed |= ro::RoCheckbox(i18n::Tr("Filtre bilinéaire (flou)"), &bilinear_);
  SameLine(); HelpMarker(i18n::Tr("Décoche = pixels nets (POINT).\nCoche = lissage (BILINEAR)."));
  changed |= ro::RoCheckbox(i18n::Tr("Clic-traversant (maintenir Shift pour interagir)"), &clickthrough_);
  changed |= ro::RoCheckbox(i18n::Tr("Afficher les raccourcis dans les touches"), &show_keys_);
  changed |= ro::RoCheckbox(i18n::Tr("Texte \"gras\""), &bold_text_);  // faux-gras (touches + nombres)
  changed |= WheelSliderFloat(i18n::Tr("Taille texte raccourcis"), &key_scale_, 0.5f, 2.0f, "%.2fx");
  changed |= WheelSliderFloat(i18n::Tr("Taille texte level/qté"), &count_scale_, 0.5f, 2.0f, "%.2fx");
  ImGui::TextDisabled(i18n::Tr("Aimantation : Réglages interface > \"Aimanter à la grille\" (grille commune à tout l'UI)."));

  // ── 3 barres FIXES (jeu fixe) : Onglet 1 / Onglet 2 / Items ──
  SeparatorText(i18n::Tr("Barres"));
  static const char* const kBarNames[kBarCount] = {"Onglet 1", "Onglet 2", "Items"};
  for (int b = 0; b < kBarCount; ++b) {
    BarCfg& bc = bars_[b];
    ImGui::PushID(b);
    bool vis = bc.visible;
    // Tr sur la VARIABLE : ces libellés viennent d'une table, donc invisibles à
    // l'extracteur statique — et le gabarit « Réglages %s » n'était traduit qu'à
    // moitié, son trou restant français.
    if (ro::RoCheckbox(i18n::Tr(kBarNames[b]), &vis)) { bc.visible = vis; changed = true; }
    if (bc.visible && ImGui::TreeNode("cfg", i18n::Tr("Réglages %s"),
                                      i18n::Tr(kBarNames[b]))) {
      changed |= WheelSliderInt(i18n::Tr("Colonnes"), &bc.columns, 1, 12);
      changed |= WheelSliderInt(i18n::Tr("Nb slots"), &bc.slot_count, 1, kRegions[b].count);
      changed |= WheelSliderFloat(i18n::Tr("Taille"), &bc.icon_size, 16.0f, 64.0f, "%.0f px", 1.0f);
      changed |= WheelSliderFloat(i18n::Tr("Espacement"), &bc.spacing, 0.0f, 12.0f, "%.0f px", 1.0f);
      changed |= WheelSliderInt("X", &bc.x, -200, 4000);
      changed |= WheelSliderInt("Y", &bc.y, -200, 4000);
      Text(i18n::Tr("Position : %d, %d"), bc.x, bc.y);
      ImGui::TreePop();
    }
    ImGui::PopID();
  }

  SeparatorText(i18n::Tr("Couleurs"));
  changed |= mui::RoColorSwatch(i18n::Tr("Fond du cadre"), col_frame_);
  changed |= mui::RoColorSwatch(i18n::Tr("Fond objet"), col_item_);
  changed |= mui::RoColorSwatch(i18n::Tr("Fond skill"), col_skill_);
  changed |= mui::RoColorSwatch(i18n::Tr("Fond vide"), col_empty_);
  changed |= mui::RoColorSwatch(i18n::Tr("Bordure"), col_border_);
  changed |= mui::RoColorSwatch(i18n::Tr("Bordure survol"), col_borderhi_);
  changed |= mui::RoColorSwatch(i18n::Tr("Texte touches"), col_keytext_);
  changed |= mui::RoColorSwatch(i18n::Tr("Texte nombre (count/lv)"), col_count_);
  changed |= mui::RoColorSwatch(i18n::Tr("Contour texte (ombre)"), col_textout_);

  SeparatorText(i18n::Tr("Aide : souris"));
  TextWrapped(
      i18n::Tr("- Clic gauche : utiliser. Clic droit : description.\n"
      "- Glisser une case sur une autre : déplacer / échanger (les 3 barres\n"
      "  se répondent entre elles).\n"
      "- Glisser une case HORS des barres et relâcher : la vider. L'aperçu se\n"
      "  barre d'une croix rouge dès qu'on est en zone de retrait.\n"
      "- Clic molette sur une case : la vider aussi, sans glisser."));

  SeparatorText(i18n::Tr("Aide : clavier & onglets"));
  TextWrapped(
      i18n::Tr("Le jeu ne pilote qu'UN onglet au clavier à la fois.\n"
      "Bourgeon route les touches vers l'autre onglet visible :\n"
      "- Touche PARTAGEE (ex. F2 sur les 2 onglets) : la case OCCUPEE répond.\n"
      "  Si les deux cases sont occupées, seul l'onglet actif répond.\n"
      "- Pour piloter l'Onglet 2 de façon dédiée, rebinde ses cases\n"
      "  sur des combos Ctrl+/Alt+ (touches uniques) dans les raccourcis clavier du jeu.\n"
      "- La barre d'items n'a pas de raccourci clavier (clic gauche = utiliser).\n"
      "- Une case NON AFFICHÉE ne répond plus à sa touche : masquer une barre,\n"
      "  ou baisser son \"Nb slots\", désarme aussi les raccourcis correspondants."));

  ImGui::EndDisabled();

  if (changed) dirty_ = true;  // persistance drainée par MoonlightUi
}

// ---- retrait d'une case glissée hors des barres -----------------------------
// Marge de pardon autour d'une barre : un relâchement au ras du bord vise encore
// la barre. Sans elle, viser la case du bout de rangée et rater d'un pixel
// effacerait le raccourci — la même main hésitante que le geste doit servir.
constexpr float kBarDropMargin = 8.0f;

bool SkillBar::PointOverAnyBar(float x, float y) const {
  for (int b = 0; b < kBarCount; ++b) {
    const BarCfg& bc = bars_[b];
    if (!bc.visible) continue;
    // Même géométrie que DrawBar : winw == cols*step, winh == rows*step (la marge
    // intérieure pad == spacing/2 de chaque côté compense le -spacing).
    const int   maxSlots = kRegions[b].count;
    const int   cols  = std::max(1, bc.columns);
    const int   count = std::min(bc.slot_count, maxSlots);
    const int   rows  = (count + cols - 1) / cols;
    // À l'échelle, comme le dessin (cf. DrawBar) : ce rect doit recouvrir ce que
    // le joueur voit, sinon le lâcher d'un raccourci rate la barre agrandie.
    const float step  = ro::Px(bc.icon_size + bc.spacing);
    const float margin = ro::Px(kBarDropMargin);
    const float x0 = static_cast<float>(bc.x) - margin;
    const float y0 = static_cast<float>(bc.y) - margin;
    const float x1 = static_cast<float>(bc.x) + cols * step + margin;
    const float y1 = static_cast<float>(bc.y) + rows * step + margin;
    if (x >= x0 && x < x1 && y >= y0 && y < y1) return true;
  }
  return false;
}

// Suit le glisser d'une case (payload "SBSLOT") et, à la fin, retire le raccourci
// si personne ne l'a accueilli ET qu'il a été lâché hors des barres — vers le sol,
// une autre fenêtre, le décor. Un dépôt sur une case reste un déplacement.
//
// ⚠ Deux temps distincts, sinon le geste se lit à l'envers :
//   • la DÉCISION se fige à l'instant du relâchement (le curseur peut ensuite
//     passer sur une barre sans que cela change ce que le joueur a fait) ;
//   • l'EXÉCUTION attend la disparition du payload, car ImGui ne marque une
//     livraison (`Delivery`) qu'après la passe des cibles, une à deux frames plus
//     tard : agir plus tôt effacerait la case qu'on vient de déplacer.
// Un glisser qui s'éteint sans relâchement observé (perte de focus, alt-tab) ne
// retire rien : on n'efface que sur un geste vu en entier.
void SkillBar::UpdateDragRemoval() {
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  const bool ours = pl != nullptr && pl->IsDataType("SBSLOT") &&
                    pl->Data != nullptr &&
                    pl->DataSize == static_cast<int>(sizeof(int) * 2);
  if (ours) {
    const int* d = static_cast<const int*>(pl->Data);
    if (drag_src_region_ < 0) {  // début d'un nouveau glisser
      drag_delivered_ = false;
      drag_released_  = false;
      drag_over_bars_ = true;    // par défaut : ne rien retirer
    }
    drag_src_region_ = d[0];
    drag_src_slot_   = d[1];
    if (pl->Delivery) drag_delivered_ = true;  // accepté (par une case, ou par ailleurs)
    if (!drag_released_) {
      const ImVec2 mp = ImGui::GetIO().MousePos;
      drag_over_bars_ = PointOverAnyBar(mp.x, mp.y);
      if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) drag_released_ = true;
    }
    return;
  }
  if (drag_src_region_ < 0) return;  // aucun glisser en cours

  const int region = drag_src_region_, slot = drag_src_slot_;
  drag_src_region_ = drag_src_slot_ = -1;
  const bool remove = drag_released_ && !drag_delivered_ && !drag_over_bars_;
  drag_delivered_ = drag_released_ = false;
  drag_over_bars_ = true;
  if (!remove) return;
  if (region < 0 || region >= kBarCount) return;
  if (!ReadSlot(region, slot).valid) return;  // déjà vidée entre-temps
  ClearSlot(region, slot);
  if (RegionIsItems(region)) dirty_ = true;   // barre d'items : persistance client (yaml)
}

// ---- la barre d'action elle-même -------------------------------------------
void SkillBar::DrawBar(int bar) {
  if (bar < 0 || bar >= kBarCount) return;
  BarCfg& bc = bars_[bar];
  // Taille/espacement PAR BARRE, MIS À L'ÉCHELLE ici et une seule fois : tout le
  // reste de la fonction en dérive (step, winw/winh, hit-test des cases, texte de
  // niveau, cooldown, libellé de touche), donc c'est le seul endroit à convertir.
  //
  // 🔴 `bc.icon_size` reste le CHOIX du joueur, en pixels à 100 % — c'est lui
  // qu'on persiste et qu'affiche le slider « Taille ». L'échelle s'applique au
  // dessin, pas au réglage : sinon changer d'échelle réécrirait le réglage, et le
  // joueur retrouverait « 128 px » dans un slider borné à 64.
  //
  // ⚠ En revanche `bc.x` / `bc.y` ne passent PAS par Px : ce sont des positions
  // d'écran posées à la main par le joueur. Les mettre à l'échelle déplacerait la
  // barre au premier changement de réglage.
  const float icon_size_ = ro::Px(bc.icon_size);
  const float spacing_   = ro::Px(bc.spacing);
  const int region   = bar;                     // index de barre == région native
  const int maxSlots = kRegions[region].count;  // 36 (skills) / 9 (items)
  const int keyCat   = kRegions[region].hotkeyCat;
  const int cols  = std::max(1, bc.columns);
  const int count = std::min(bc.slot_count, maxSlots);
  const int rows  = (count + cols - 1) / cols;
  const float step = icon_size_ + spacing_;
  // Marge intérieure = MOITIÉ de l'espacement : chaque icône est ainsi CENTRÉE dans sa cellule
  // de taille `step`, avec spacing/2 de chaque côté (au lieu de spacing entier collé à droite/bas).
  // Du coup le bord de la fenêtre = bord de cellule : quand bc.x s'aimante sur une grille = step,
  // les icônes tombent centrées sur les cases de la grille. Constante -> aucun décalage édition/normal.
  const float pad  = spacing_ * 0.5f;
  const float winw = cols * step - spacing_ + pad * 2;  // = cols*step (pad*2 == spacing)
  const float winh = rows * step - spacing_ + pad * 2;  // = rows*step

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
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground |  // on dessine notre propre fond
      ro::kBackgroundWindowFlags;  // décor : jamais devant une vraie fenêtre

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
    // Coordonnées locales relatives à la 1ère cellule ICÔNE (on retire la marge intérieure pad,
    // sinon le hit-test serait décalé de spacing/2 par rapport aux icônes centrées).
    const float lx = mp.x - static_cast<float>(bc.x) - pad, ly = mp.y - static_cast<float>(bc.y) - pad;
    if (lx >= 0 && ly >= 0) {
      const int cc = static_cast<int>(lx / step), cr = static_cast<int>(ly / step);
      if (cc < cols && (lx - cc * step) < icon_size_ && (ly - cr * step) < icon_size_) {
        const int k = cr * cols + cc;
        if (k >= 0 && k < count && bc.first_slot + k < maxSlots)
          over_filled = ReadSlot(region, bc.first_slot + k).valid;
      }
    }
  }
  // Un drag ImGui en cours (réarrangement interne "SBSLOT" OU item glissé du viewer
  // inventaire "INV_ITEM") doit TOUJOURS pouvoir se déposer sur la barre -> on force la
  // capture souris pendant le drag, quel que soit le réglage clic-traversant / Shift / verrou.
  const bool imgui_dragging = ImGui::GetDragDropPayload() != nullptr;
  bool no_input = false;
  if (clickthrough_ && !shift) no_input = true;  // option : toute la barre
  else if (locked_ && !over_filled && !imgui_dragging)
    no_input = true;                             // cases vides / espaces
  if (imgui_dragging) no_input = false;          // drag en cours -> capturer pour permettre le drop
  if (no_input) flags |= ImGuiWindowFlags_NoMouseInputs;

  char winName[32];
  std::snprintf(winName, sizeof(winName), "##SkillActionBar%d", bar);
  ro::MarkBackgroundWindow(winName);
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
  // Contour/ombre RÉGLABLE (8 directions, comme l'inventaire & la fiche perso : texte noir + halo blanc
  // tout autour -> lisible sur n'importe quelle icône). La couleur du contour est configurable (col_textout_)
  // -> le joueur peut inverser (texte clair + contour sombre) ou ajuster l'opacité du halo. + faux-gras
  // optionnel (ImGui n'a pas de fonte bold).
  const bool bold = bold_text_;
  const ImU32 cOutline = ImGui::GetColorU32(ImVec4(col_textout_[0], col_textout_[1], col_textout_[2], col_textout_[3]));
  auto boldAdd = [&](ImVec2 p, ImU32 c, const char* t) {
    for (int oy = -1; oy <= 1; ++oy)
      for (int ox = -1; ox <= 1; ++ox)
        if (ox || oy) dl->AddText(ImVec2(p.x + ox, p.y + oy), cOutline, t);
    dl->AddText(p, c, t);
    if (bold) dl->AddText(ImVec2(p.x + 1.0f, p.y), c, t);
  };
  auto boldAddF = [&](ImFont* f, float s, ImVec2 p, ImU32 c, const char* t) {
    for (int oy = -1; oy <= 1; ++oy)
      for (int ox = -1; ox <= 1; ++ox)
        if (ox || oy) dl->AddText(f, s, ImVec2(p.x + ox, p.y + oy), cOutline, t);
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
      // La barre reste entièrement dans l'écran de jeu. APRÈS l'aimantation (le snap
      // peut repousser dehors) ; le clamp global de ui/window_clamp.h ne peut rien
      // ici, la fenêtre étant réépinglée à (bc.x,bc.y) en Cond_Always chaque frame.
      const ImVec2 in_screen =
          ro::ClampWindowPosToScreen(ImVec2(nx, ny), ImVec2(winw, winh));
      nx = in_screen.x;
      ny = in_screen.y;
      bc.x = static_cast<int>(nx + 0.5f);
      bc.y = static_cast<int>(ny + 0.5f);
    }
    if (ImGui::IsItemDeactivated()) dirty_ = true;  // persiste la position en fin de glisser
  }

  int move_from = -1, move_to = -1, move_region = -1;  // glisser-déposer différé hors de la boucle
  int inv_drop_slot = -1; uint32_t inv_drop_id = 0;    // item d'inventaire lâché sur une case (différé)
  int skill_drop_slot = -1, skill_drop_id = 0, skill_drop_level = 0;  // skill lâché depuis un panneau ImGui

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
        else ImGui::Text("%s %u", r.type == 0 ? i18n::Tr("Objet") : i18n::Tr("Skill"), r.id);
        // Hors des barres, l'aperçu se barre d'une croix rouge : relâcher là
        // RETIRE la case (UpdateDragRemoval) au lieu de simplement annuler. Le
        // geste est sans confirmation, comme sur la barre native — l'avertir
        // pendant qu'on le fait est le seul moment où cela sert encore.
        if (!drag_over_bars_) {
          ImDrawList* tdl = ImGui::GetWindowDrawList();
          const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
          tdl->AddRectFilled(a, b, IM_COL32(190, 30, 30, 90));
          tdl->AddLine(a, b, IM_COL32(240, 70, 70, 235), 2.0f);
          tdl->AddLine(ImVec2(b.x, a.y), ImVec2(a.x, b.y), IM_COL32(240, 70, 70, 235), 2.0f);
        }
        ImGui::EndDragDropSource();
      }
      ImGui::PopStyleVar(3);
      ImGui::PopStyleColor(2);
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SBSLOT")) {
          const int* d = static_cast<const int*>(pl->Data);
          move_region = d[0]; move_from = d[1]; move_to = slot;
          // Une case a accueilli le glisser : c'est un déplacement, pas un retrait.
          // Noté ICI parce qu'UpdateDragRemoval passe AVANT le dessin : le drapeau
          // `Delivery` d'ImGui n'existe qu'après cette passe, et le payload aura
          // disparu au tour suivant.
          drag_delivered_ = true;
        } else if (ImGui::AcceptDragDropPayload("INV_ITEM")) {
          // Item lâché depuis le viewer inventaire ImGui (comme le drag natif inventaire->barre,
          // que HandleNativeDrop gère pour la fenêtre native). Le viewer expose le nameid glissé ;
          // un slot d'item (region 2) OU une case de barre skill (regions 0/1) accepte un OBJET.
          if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
            const uint32_t nameid = iv->DraggedItemNameId();
            if (nameid != 0) { inv_drop_slot = slot; inv_drop_id = nameid; }
          }
        } else if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("BGN_SKILL")) {
          // Skill glissé depuis un panneau ImGui (feuille de perso : compétences de guilde).
          // La barre d'ITEMS (région 2) ne stocke que des nameid -> elle le refuse.
          if (!RegionIsItems(region)) {
            const int* d = static_cast<const int*>(pl->Data);
            skill_drop_slot = slot; skill_drop_id = d[0]; skill_drop_level = d[1];
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
        ImFont* font = ImGui::GetFont();
        const float ns = std::max(7.0f, ImGui::GetFontSize() * count_scale_);  // taille réglable
        const ImVec2 ls = font->CalcTextSizeA(ns, 1.0e30f, 0.0f, lv);
        boldAddF(font, ns, ImVec2(p1.x - ls.x - 1, p1.y - ls.y - 1), cCount, lv);
      }
      // Overlay de cooldown : la table est indexée par SKILL id, donc réservée aux
      // cases de type skill — l'id d'un objet est un nameid, qui tombe dans la
      // même plage numérique et désignerait un cooldown au hasard.
      const float f = isItem ? 0.0f : CooldownFraction(r.id);
      if (f > 0.0f) {
        const float h = icon_size_ * f;
        dl->AddRectFilled(ImVec2(p0.x, p1.y - h), p1, IM_COL32(0, 0, 0, 150), 3.0f);
      }
      // Case INUTILISABLE (objet épuisé / skill non appris) : voile gris sombre par-dessus.
      if (!usable)
        dl->AddRectFilled(p0, p1, IM_COL32(10, 10, 15, 165), 3.0f);
      // Décompte au centre, par-dessus les voiles. Le voile seul ne dit rien d'un
      // cooldown de guilde de plusieurs minutes : il bouge d'un pixel par seconde.
      // (`ms` == 0 alors que le voile est là = cooldown venu du repli natif, sans
      // durée exploitable côté table : on garde le voile, sans chiffre.)
      const unsigned long ms =
          f > 0.0f ? ro::SkillCooldownRemainingMs(static_cast<uint16_t>(r.id)) : 0;
      if (ms > 0) {
        char left[16];
        if (ms >= 60000)  // au-delà de la minute, m:ss ; en deçà, la seconde suffit
          std::snprintf(left, sizeof(left), "%lu:%02lu", ms / 60000,
                        (ms / 1000) % 60);
        else
          std::snprintf(left, sizeof(left), "%lu", (ms + 999) / 1000);
        ImFont* font = ImGui::GetFont();
        // Taille de police EXPLICITE : elle ne suit pas FontScaleMain, mais elle
        // dérive d'`icon_size_` qui est déjà à l'échelle — seul le plancher
        // devait être converti.
        const float cs = std::max(ro::Px(8.0f), icon_size_ * 0.42f);
        const ImVec2 cz = font->CalcTextSizeA(cs, 1.0e30f, 0.0f, left);
        boldAddF(font, cs,
                 ImVec2(p0.x + (icon_size_ - cz.x) * 0.5f,
                        p0.y + (icon_size_ - cz.y) * 0.5f),
                 IM_COL32(255, 235, 150, 255), left);
      }
    }

    dl->AddRect(p0, p1, (hover == k) ? cBordHi : cBorder, 3.0f);

    // Étiquette de touche en haut-gauche : touche RÉELLE lue depuis UserKeys.lua (rebinds inclus,
    // layout-aware). Dessinée même sur slot vide (la touche dépend de la position, pas du contenu).
    if (show_keys_) {
      char key[24];
      GetSlotKeyLabel(keyCat, slot, key, sizeof(key));
      if (key[0]) {
        ImFont* font = ImGui::GetFont();
        float ks = std::max(ro::Px(7.0f), icon_size_ * 0.30f * key_scale_);  // ∝ icône × réglage
        const float maxW = icon_size_ - ro::Px(3.0f);   // marge à droite de la case
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

  // Idem pour un SKILL venu d'un panneau ImGui (payload "BGN_SKILL") : record type 1 +
  // persistance serveur. Écriture directe comme ci-dessus — passer par
  // SkillMgr_SetShortCutSlot notifierait le dispatcher 0x139 pendant le drag.
  if (skill_drop_slot >= 0 && skill_drop_id != 0) {
    WriteSlotRecord(region, skill_drop_slot, /*is_item*/ false,
                    static_cast<uint32_t>(skill_drop_id), skill_drop_level);
    SendHotkeyChange(kRegions[region].tab, skill_drop_slot, /*isSkill*/ 1,
                     static_cast<uint32_t>(skill_drop_id), skill_drop_level);
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
