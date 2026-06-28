#include "plugins/inventory_tweaks.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "utils/log_console.h"

// ===========================================================================
// Inventory weight readout + bigger-resize (20250716 client, base 0x400000)
//
// The inventory DrawContent (FUN_00946da0, vtable 0x0103d460 +0x50) draws the
// slot grid and a bottom-left item count "X/200" — but no character weight.
// We swap that slot for a hook that runs the original then draws, bottom-RIGHT:
//     [scale icon]  cur / max (NN%)
// reusing the cart's own weight icon (유저인터페이스\inventory\icon_weight.bmp,
// path string @ 0x0103db00, blitted by the cart's DrawContent FUN_00948610).
//
// Weight globals (raw, no /10), confirmed via UIBasicInfoWnd::DrawContent:
//     0x015fba9c = max     0x015fbaa0 = current     0x01602324 = red %% thresh
//
// The window also clamps user-resize (msg handler FUN_00955530 case 0xe sub-7)
// to max 0x140 x 0xf0; we raise those two immediates so it can grow large (the
// inventory background is tiled, so it renders fine at any size).
// ===========================================================================

namespace {

// ---- patch sites -----------------------------------------------------------
constexpr uintptr_t kDrawSlot     = 0x0103d4b0;  // UIItemWnd vtable +0x50
constexpr uintptr_t kDrawOrig     = 0x00946da0;  // original inventory DrawContent
constexpr uintptr_t kMaxWidthImm  = 0x00955980;  // MOV EDI,0x140 imm (resize max W)
constexpr uintptr_t kMaxHeightImm = 0x0095598b;  // MOV EDX,0x0f0 imm (resize max H)

constexpr uint32_t kStockMaxW = 0x140;  // 320  (sanity guard)
constexpr uint32_t kStockMaxH = 0x0f0;  // 240
constexpr uint32_t kNewMaxW   = 0x500;  // 1280
constexpr uint32_t kNewMaxH   = 0x400;  // 1024

// ---- engine functions ------------------------------------------------------
constexpr uintptr_t kDrawText  = 0x00a25a70;  // __thiscall(this,x,y,str,len,face,size,color,bold,ital)
constexpr uintptr_t kMeasureW  = 0x00a21c90;  // __thiscall(this,str,len,face,size,_,_) -> width
constexpr uintptr_t kTexMgr    = 0x00a90350;  // __cdecl() -> tex mgr
constexpr uintptr_t kMakeKey   = 0x00a9f030;  // __cdecl(path) -> key
constexpr uintptr_t kLoadTex   = 0x00a8d4a0;  // __thiscall(mgr, key) -> UITexture*
constexpr uintptr_t kBlit      = 0x00a1d260;  // __thiscall(this,x,y,img,flag)
constexpr uintptr_t kFill      = 0x00a1d460;  // __thiscall(this,x,y,w,h,color) filled rect
constexpr uintptr_t kIconPath  = 0x0103db00;  // "유저인터페이스\inventory\icon_weight.bmp"
constexpr uintptr_t kBtnbarPath = 0x010357b8; // "유저인터페이스\basic_interface\btnbar_left.bmp" (bottom-frame height)
constexpr int       kBarHeight  = 0x29;       // btnbar bmp height fallback (41px) if the live read fails
constexpr uintptr_t kFmtComma  = 0x00a948d0;  // __cdecl(value,buf,size) -> thousands-separated

// ---- session globals -------------------------------------------------------
constexpr uintptr_t kWeightCur     = 0x015fbaa0;  // current weight (raw)
constexpr uintptr_t kWeightMax     = 0x015fba9c;  // max weight (raw)
constexpr uintptr_t kOverweightPct = 0x01602324;  // red-tint % threshold
constexpr uintptr_t kZeny          = 0x015fba90;  // player zeny (int32, next to weight)

// ---- UIWindow field offsets / UITexture fields -----------------------------
constexpr int kWndWidth   = 0x14;
constexpr int kWndHeight  = 0x18;
constexpr int kCollapsedH = 0x11;   // title-bar-only height when minimized
constexpr int kTexW       = 0x114;  // UITexture width
constexpr int kTexH       = 0x118;  // UITexture height

constexpr unsigned kColorNormal = 0x232323;  // match the item-count text
constexpr unsigned kColorOver   = 0x0000ff;  // red, like UIBasicInfoWnd
constexpr unsigned kBarFill     = 0xc0c0c0;  // grid-gap filler, matches the btnbar grey (palette idx 7)
constexpr int kRightMargin = 20;  // clear the scrollbar / resize grip on the right
constexpr int kLeftMargin  = 0x14;  // weight (left) start x, ~aligned with the grid
constexpr int kIconGap     = 3;   // px between icon and number

// ---- bottom-bar child buttons (UIItemWnd this+offset; see FUN_0093f100) -----
constexpr int kBtnExpand   = 0x120;  // icon_num "expand" btn (neutralize, stays line 1)
constexpr int kBtnDropLock = 0x114;  // item_drop_lock
constexpr int kBtnCompare  = 0x11c;  // item_compare (loupe)
constexpr int kBtnDealLock = 0x124;  // bt_itemDeal_lock (block NPC sell)
constexpr int kBtnSort     = 0x12c;  // bt_sort
constexpr int kVfSetPos    = 0x10;   // UIWindow::SetPos(x,y)
constexpr int kVfSetCmd    = 0xb4;   // button::SetCommandId(id)
constexpr uintptr_t kSetName = 0x00831a50;  // UIItemLinkBtn_SetName(this, char*)
constexpr int kRows        = 0xdc;   // grid row count (UIItemWnd this+0xdc)
constexpr int kSlot        = 0x20;   // grid cell size (32px)
constexpr int kCountTop    = 0x1c;   // native count line top (strip stops just above it)
constexpr int kBarX        = 0x16;   // bottom-bar strip LEFT margin (clears the grey border @0x14)
constexpr int kBarXR       = 0x0c;   // bottom-bar strip RIGHT margin (reach the right frame, fill the gap)
constexpr int kLine2Y      = 0x26;   // weight/zeny baseline = height - this (raised into the 41px btnbar)
constexpr int kTabCat      = 0x10c;  // UIItemWnd current category (3 + DAT_01600553 == FAV view)
constexpr uintptr_t kFavFlag = 0x01600553;  // FAV-mode global (matches FUN_00946da0)

// ---- bottom-reserve enlarge: ONLY the grid-EXTENT uses of 0x26 move to kRsvNew
//      (row count, resize snap/final-height, scrollbar, slot-cell rows) so the
//      grid leaves room for the bottom bar. Enlarging shrinks the grid for a
//      fixed window height — which IS the point here: the bottom BAR grows
//      ("c'est la barre du bas qu'il faut grandir"). The user sizes the window;
//      the resize snap uses the new reserve consistently.
//      The BACKGROUND-COVERAGE uses of 0x26 (colorchip bg fills, grey separator,
//      side edge frames in FUN_00946da0) are LEFT at 0x26 so the bg still reaches
//      the window's bottom border — else a transparent strip appears between the
//      grid bg and the bottom bar.
//      Positive sites = SUB/LEA +0x26 ; negative = LEA disp8 -0x26 (0xDA).
constexpr uint8_t kRsvOld  = 0x26;   // 38px (stock)
constexpr uint8_t kRsvNew  = 0x3a;   // grid stops ~41px from bottom = the btnbar height (21x41 bmp)
constexpr uintptr_t kRsvPos[] = {
    0x0093f1f9,  // FUN_0093f100 SUB EAX,0x26   : scrollbar height
    0x009559b9,  // case 0xe     LEA [ECX+0x26] : tabH + reserve (snap)
    0x009559c3,  // case 0xe     SUB EAX,0x26   : available grid height (snap)
    0x009559d7,  // case 0xe     LEA [EAX+0x26] : FINAL window height (SetSize)
    0x00955a69,  // case 0xe     SUB EAX,0x26   : scrollbar height (resize)
    0x0094708b,  // FUN_00946da0 SUB EAX,0x26   : slot-cell grid row count
    0x00947053,  // FUN_00946da0 SUB EAX,0x26   : grey separator LINE length (stop at grid bottom, not into the btnbar)
};
constexpr uintptr_t kRsvNeg[] = {
    0x0093f278,  // FUN_0093f100 LEA [EBX-0x26] : item row count
};
// Background-coverage 0x26 sites deliberately LEFT stock so the bg fills the whole
// interior down to the bottom border: 0x00946f17 / 0x00946f46 (colorchip fills),
// 0x00946df9 / 0x00946e61 (side edge frames).

// ---- tab fill colour from colorchip (so users re-theme via colorchip.bmp) ---
// The 3 category tabs (Use/Eqp/Etc) use a hardcoded WHITE fill (FUN_00844710);
// the FAV tab already samples colorchip. We route a colorchip chip into the 3
// tabs' fill fields so they follow the .bmp like FAV — each user themes them by
// editing colorchip.bmp (data\ overrides the GRF). Stock chip (16,0) = #797979.
constexpr uintptr_t kColorChip  = 0x007a6df0;  // FUN_007a6df0(x,y,&r,&g,&b) -> colorchip.bmp pixel
constexpr int kTabCtrl      = 0x118;  // inventory tab control (this+0x118)
constexpr int kTabSel       = 0x7c;   // tab control: selected/active tab index (Use active <=> ==0)
constexpr int kTabSizeArr   = 0xcc;   // tab control: per-tab size int-vector begin (size[i] = tab height)
constexpr int kTabThickness = 0x84;   // tab control: strip cross-thickness; width = this+0x84+1 (via Recompute)
constexpr int kTabMaxWidth  = 36;     // clamp the strip width so it stays clear of the slot grid (x=0x28=40)
constexpr int kTabLabelVec  = 0xb4;   // tab control: per-tab std::string label vector begin (stride 0x18)
constexpr int kTabStripX    = 3;      // tab strip origin in the inventory window (native SetPos(3,0x12))
constexpr int kTabStripY    = 0x12;
constexpr uintptr_t kTabRecompute = 0x0085fca0;  // FUN_0085fca0(tabctrl): recompute strip size after a size[] change
// Tab control DrawContent (vtable +0x50): FUN_00857910 clears the strip node
// (colorkey 0xffff00ff) then draws each tab. We hook it so the Use-tab image is
// repainted on EVERY strip render (a per-frame blit from the inventory hook gets
// wiped by this clear, and only survived a manual resize by node-recreate luck).
// The tab class is shared (chat/skill/etc.), so the hook filters on OUR control.
constexpr uintptr_t kTabDrawSlot  = 0x0102dd94;  // tab control vtable +0x50 slot (static .rdata)
constexpr uintptr_t kTabDrawOrig  = 0x00857910;  // FUN_00857910 tab DrawContent
constexpr uintptr_t kInvWndGlobal = 0x0131f6bc;  // inventory UIWindow* global (identifies our tab control)

// ---- NEW "Cards" category (client-side; players asked for a cards tab) --------
// The inventory message handler FUN_00955530 (inv vtable +0x94) owns the category
// filter: case 0x16 writes the clicked tab index -> inv+0x10c then sends 0x17;
// case 0x17 rebuilds the slot list inv+0xe8 (cases 0/1/2 + 3=Fav). We hook +0x94
// to (a) REMAP the clicked visual slot -> a category (so we can order tabs freely
// without disturbing the hardcoded ==3 Fav gates), and (b) populate cat 4 (Cards)
// by building it as Etc then dropping non-cards at draw time. Fav stays cat 3.
constexpr uintptr_t kMsgSlot  = 0x0103d4f4;  // inventory vtable +0x94 (message handler slot)
constexpr uintptr_t kMsgOrig  = 0x00955530;  // FUN_00955530 inventory message handler
constexpr uintptr_t kAddTab   = 0x00864690;  // tab control AddTab(this, label, tooltip)
constexpr uintptr_t kListErase   = 0x0080d1a0;  // std::list::erase(first,last) __thiscall(listObj,&out,first,last): frees nodes + dtors + size-=n
constexpr uintptr_t kInvFinalize = 0x00950400;  // FUN_00950400(inv): refresh scrollbar/scroll from inv+0xec count
// FUN_0096b700 layout-restore validator: 'CMP [ECX+0x14],3 / JA fail' rejects a saved
// tab/category > 3, so closing on Cards (cat 4) makes the client discard the saved
// layout and fall back to FUN_0096c610's DEFAULT (x=0 -> stuck left, tab 0 = Use).
// Bumping the imm 3->4 accepts category 4 so the layout (position + Cards) persists.
constexpr uintptr_t kLayoutTabBoundImm = 0x0096b72b;  // the imm8 (0x03) of that CMP
constexpr int kMsgSelectTab = 0x16;  // tab clicked: param_3 = visual slot index
constexpr int kMsgRefresh   = 0x17;  // rebuild inv+0xe8 for the current category
constexpr int kMsgRestore   = 0x22;  // layout restore (sets category + selected tab)
constexpr int kEtcCat  = 2;   // Etc category (admits cards) — populated then filtered
constexpr int kFavCat  = 3;   // Fav category (pinned to literal 3 in native ==3 gates)
constexpr int kCardCat = 4;   // our new client-side Cards category
constexpr int kItCard  = 6;   // item TYPE for cards (node+0x08 == 6)
constexpr int kEtcSub  = 0x108;  // inv+0x108 = Etc sub-filter (==4 => "show all etc")
constexpr int kListHead  = 0xe8;  // inv+0xe8 -> head sentinel node (node+0 next, +4 prev)
constexpr int kListCount = 0xec;  // inv+0xec element count
constexpr int kNodeNext  = 0x00;  // list node: next
constexpr int kNodePrev  = 0x04;  // list node: prev
constexpr int kNodeType  = 0x08;  // list node: item TYPE (record+0x00)

// Per-tab image base names in the btnbar folder, in VISUAL slot order (top->bottom
// on screen). Cards sits before Fav. AddTab appends Cards as native index 4; we
// DECOUPLE the visual order from the native category via kSlotCategory + a message
// remap, so no hardcoded ==3 Fav gate in native code is disturbed (Fav stays cat 3).
// Each name has active "<name>1.bmp" / inactive "<name>2.bmp"; strip width (this+0x14)
// is shared, so all images should be <= that width or they clip on the right.
// Cards tab, incremental re-enable after the v1 crash (UIWindow_SetSize, corrupted
// vtable from AddTab-during-render). Two flags so we isolate the cause:
//   kEnableCardsTab    = the 5th VISUAL tab: AddTab now runs in OnTick (LOGIC phase,
//                        NOT during render) + the per-tab image. v2a tests this alone.
//   kEnableCardsFilter = the FUNCTIONAL part: the msg-handler hook (slot remap + the
//                        cards-only list filter). Enable only once v2a is proven stable.
constexpr bool kEnableCardsTab    = true;
constexpr bool kEnableCardsFilter = true;   // v2b: msg-hook remap + cards-only filter ON
constexpr int kTabCount = 5;
// Visual slot order (on-screen top->bottom). AddTab appends the new tab at native
// index 4, but we DECOUPLE visual order from native category via kSlotCategory + the
// msg remap, so Card shows before Fav while Fav keeps native category 3 (every native
// ==3 Fav gate stays correct). slot3 = Card image, slot4 = Fav image.
const char* const kTabImgName[kTabCount] = {"tab_use", "tab_cos", "tab_etc", "tab_card", "tab_fav"};
// Visual slot -> native category (inv+0x10c): slot3->cat4 (Cards), slot4->cat3 (Fav).
constexpr int kSlotCategory[kTabCount] = {0, 1, 2, kCardCat, kFavCat};
constexpr int kTabFillSel   = 0x90;   // tab control: selected-tab fill colour
constexpr int kTabFillUnsel = 0x94;   // tab control: unselected-tab fill colour
constexpr int kTabChipX     = 16;     // colorchip coord of the tab chip
constexpr int kTabChipY     = 0;
constexpr int kTabBorderSel   = 0x98;  // tab control: selected-tab border colour
constexpr int kTabBorderUnsel = 0x9c;  // tab control: unselected-tab border colour
constexpr int kBorderChipX  = 26;      // colorchip coord for tab border
constexpr int kBorderChipY  = 0;
constexpr uintptr_t kTitleColorImm = 0x00898cc5;  // imm of PUSH 0xffffff in DrawTitleBar (title text, ALL windows)
constexpr int kTitleChipX   = 0;       // colorchip coord for window title text
constexpr int kTitleChipY   = 0;
// Item-quantity white outline = 4x PUSH 0xffffff in FUN_008711a0 (immediates @ +1).
constexpr int kItemChipX    = 0;       // colorchip coord for item-count outline
constexpr int kItemChipY    = 0;
constexpr uintptr_t kItemNumImm[4] = {0x00871276, 0x008712b4, 0x008712d3, 0x008712f5};

using DrawOrig_t = void(__fastcall*)(void*, void*);
using DrawText_t = void(__fastcall*)(void*, void*, int, int, const char*, unsigned,
                                     int, int, unsigned, unsigned char, unsigned char);
using MeasureW_t = int(__fastcall*)(void*, void*, const char*, int, int, int, int, int);
using TexMgr_t   = void*(__cdecl*)();
using MakeKey_t  = void*(__cdecl*)(const char*);
using LoadTex_t  = void*(__fastcall*)(void*, void*, void*);
using Blit_t     = void(__fastcall*)(void*, void*, int, int, void*, int);
using SetName_t   = void(__fastcall*)(void*, void*, const char*);
using ColorChip_t = void(__stdcall*)(int, int, unsigned*, unsigned*, unsigned*);  // FUN_007a6df0 ends RET 0x14
using FmtComma_t  = char*(__cdecl*)(int, char*, int);  // FUN_00a948d0 thousands-separated
using Fill_t      = void(__fastcall*)(void*, void*, int, int, int, int, unsigned);  // FUN_00a1d460 filled rect
using Recompute_t = void(__fastcall*)(void*, void*);  // FUN_0085fca0 __thiscall(tabctrl) -> recompute strip size
using TabDraw_t   = void(__fastcall*)(void*, void*);  // FUN_00857910 __thiscall(tabctrl) -> draw strip into node
// FUN_00955530 inventory message handler (__thiscall -> __fastcall, edx unused).
// IMPORTANT: it cleans 0x18 = SIX stack params (ret 0x18), NOT five (Ghidra mis-typed
// it as 5). A 5-arg hook under-pops 4 bytes -> stack imbalance -> corrupt frame -> the
// v1/v2b crash. Layout: (this, _, p1, msg, p3, p4, p5, p6).
using MsgFn_t    = int(__fastcall*)(void*, void*, int, int, int, int, int, int);
using AddTab_t   = void(__fastcall*)(void*, void*, const char*, const char*);  // FUN_00864690
using Erase_t    = void(__fastcall*)(void*, void*, void*, void*, void*);  // FUN_0080d1a0(listObj,edx,&out,first,last)
using Finalize_t = void(__fastcall*)(void*);                               // FUN_00950400(inv) — single ecx arg

const auto g_draw_orig     = reinterpret_cast<DrawOrig_t>(kDrawOrig);
const auto g_tab_draw_orig = reinterpret_cast<TabDraw_t>(kTabDrawOrig);
const auto g_msg_orig      = reinterpret_cast<MsgFn_t>(kMsgOrig);
// Per-tab images, loaded + published once per frame by the inventory hook so the
// tab DrawContent hook can blit them (A = active "<name>1", B = inactive "2").
void* g_tabTexA[kTabCount] = {nullptr};
void* g_tabTexB[kTabCount] = {nullptr};

inline int RD(uintptr_t a) { return *reinterpret_cast<volatile int*>(a); }

// Child window pointer at this+off, and helpers to call its vtable methods
// (__thiscall -> __fastcall; edx unused).
inline void* Child(void* wnd, int off) {
  return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(wnd) + off);
}
inline void VCall(void* obj, int vf, int a, int b) {
  using Fn = void(__fastcall*)(void*, void*, int, int);
  reinterpret_cast<Fn>(*reinterpret_cast<uintptr_t*>(
      *reinterpret_cast<uintptr_t*>(obj) + vf))(obj, nullptr, a, b);
}
inline void VCall1(void* obj, int vf, int a) {
  using Fn = void(__fastcall*)(void*, void*, int);
  reinterpret_cast<Fn>(*reinterpret_cast<uintptr_t*>(
      *reinterpret_cast<uintptr_t*>(obj) + vf))(obj, nullptr, a);
}

// True iff `tabobj` is the inventory window's tab control. The tab class is
// shared by chat/skill/other windows (FUN_00864690 has many callers), so the
// shared-vtable DrawContent hook must only theme ours.
inline bool IsInventoryTab(void* tabobj) {
  void* inv = *reinterpret_cast<void**>(kInvWndGlobal);
  return inv && *reinterpret_cast<void**>(
                    reinterpret_cast<uint8_t*>(inv) + kTabCtrl) == tabobj;
}

// Live tab count = label-vector size: (end - begin) / sizeof(std::string=0x18).
inline int TabCount(void* tabobj) {
  auto* t = reinterpret_cast<uint8_t*>(tabobj);
  const int begin = *reinterpret_cast<int*>(t + kTabLabelVec);
  const int end   = *reinterpret_cast<int*>(t + kTabLabelVec + 4);
  return (end - begin) / 0x18;
}

// The image for tab i in the given state, falling back to the other state's bmp
// if a user only supplied one variant (so a missing "2.bmp" still shows "1").
inline void* TabImg(int i, bool active) {
  void* primary  = active ? g_tabTexA[i] : g_tabTexB[i];
  void* fallback = active ? g_tabTexB[i] : g_tabTexA[i];
  return primary ? primary : fallback;
}

template <typename T> void PatchValue(uintptr_t addr, T value);  // defined below

// Sample colorchip.bmp pixel (x,y) -> game colour 0x00BBGGRR.
inline unsigned ChipColor(int x, int y) {
  unsigned r = 0, g = 0, b = 0;
  reinterpret_cast<ColorChip_t>(kColorChip)(x, y, &r, &g, &b);
  return (b << 16) | (g << 8) | r;
}

// Build the per-tab image paths once (btnbar folder) and (re)load each tab's
// active/inactive bmp into g_tabTexA/B (cached by the tex mgr). Safe to call every
// frame and from the logic phase. Publishes the images for TabDrawContentHook.
void LoadTabImages() {
  static char s_paths[kTabCount][2][128] = {};
  if (s_paths[0][0][0] == 0) {
    const char* base = reinterpret_cast<const char*>(kBtnbarPath);
    const char* slash = std::strrchr(base, '\\');
    const size_t n = slash ? static_cast<size_t>(slash - base + 1) : 0;
    for (int i = 0; i < kTabCount; ++i)
      for (int v = 0; v < 2; ++v) {
        std::memcpy(s_paths[i][v], base, n);
        std::snprintf(s_paths[i][v] + n, sizeof(s_paths[i][v]) - n,
                      "%s%d.bmp", kTabImgName[i], v + 1);
      }
  }
  void* ttmgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
  for (int i = 0; i < kTabCount; ++i) {
    g_tabTexA[i] = reinterpret_cast<LoadTex_t>(kLoadTex)(
        ttmgr, nullptr, reinterpret_cast<MakeKey_t>(kMakeKey)(s_paths[i][0]));
    g_tabTexB[i] = reinterpret_cast<LoadTex_t>(kLoadTex)(
        ttmgr, nullptr, reinterpret_cast<MakeKey_t>(kMakeKey)(s_paths[i][1]));
  }
}

// Widen the (shared) tab strip to the widest tab image and size each tab to its
// image height, then recompute the strip. Leaves the strip at its FINAL rendered
// height. Must run BEFORE the native layout-restore (msg 0x22): that restore
// clamps the window height up to (tabStripHeight + bottom reserve); pre-render the
// tabs sit at their taller NATIVE height, so the floor is inflated and a saved
// short height is clamped away (window springs back to "standard" on reopen).
// Sizing first makes the restore floor match the live resize floor (= rendered
// strip), so the saved height survives. Width derives from this+0x84+1 (Recompute
// calls SetSize with it), so set that source field — not this+0x14.
void SizeTabsToImages(void* tabobj) {
  auto* t = reinterpret_cast<uint8_t*>(tabobj);
  bool changed = false;
  int maxW = 0;
  for (int i = 0; i < kTabCount; ++i)
    if (void* img = TabImg(i, true)) {
      const int iw = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(img) + kTexW);
      if (iw > maxW) maxW = iw;
    }
  if (maxW > kTabMaxWidth) maxW = kTabMaxWidth;  // keep clear of the slot grid
  if (maxW > 0 && *reinterpret_cast<int*>(t + kTabThickness) != maxW - 1) {
    *reinterpret_cast<int*>(t + kTabThickness) = maxW - 1;
    changed = true;
  }
  if (int* sz = *reinterpret_cast<int**>(t + kTabSizeArr)) {
    const int count = TabCount(tabobj);
    for (int i = 0; i < count && i < kTabCount; ++i) {
      void* img = TabImg(i, true);
      if (!img) continue;
      const int imgH = *reinterpret_cast<int*>(
          reinterpret_cast<uint8_t*>(img) + kTexH);
      if (imgH > 0 && sz[i] != imgH) { sz[i] = imgH; changed = true; }
    }
  }
  if (changed) reinterpret_cast<Recompute_t>(kTabRecompute)(tabobj, nullptr);
}

// Replacement for UIItemWnd::DrawContent (__thiscall -> __fastcall, edx unused).
// POD-only locals so the body can live under SEH.
void __fastcall DrawContentHook(void* wnd, void* /*edx*/) {
  __try {
    auto* w = reinterpret_cast<uint8_t*>(wnd);
    const int height = *reinterpret_cast<const int*>(w + kWndHeight);
    const int width  = *reinterpret_cast<const int*>(w + kWndWidth);

    // ---- Tab strip -> images. Load each tab's active/inactive bmp (published to
    // g_tabTexA/B for TabDrawContentHook) and size the strip to them. The blit +
    // label-empty happen in the tab hook: the strip node is cleared+redrawn
    // (FUN_00857910) every render, so a blit from here would be wiped. NB: the
    // extra tab is appended in OnTick/MsgHook (logic phase), NOT here — the AddTab
    // structural mutation during the render pass corrupted window state (v1 crash);
    // here we only load + SIZE/width the tabs that already exist.
    LoadTabImages();
    if (void* tabobj = Child(wnd, kTabCtrl)) SizeTabsToImages(tabobj);

    // Original: slot grid + bottom-left "X/200" count + the btnbar bottom frame.
    // The cards/Etc filtering is done for REAL in DoRefresh (PruneList erases nodes
    // from inv+0xe8), so the list DrawContent walks is already correct — no per-draw
    // surgery (which desynced the hit-test/click index from the drawn slots).
    g_draw_orig(wnd, nullptr);

    if (height <= kCollapsedH) return;  // minimized -> nothing below the bar

    // Native action buttons (drop-lock / compare / deal-lock / sort) stay at
    // their line-1 positions — moving them desyncs the hit-test from the drawn
    // spot (clicks land where they used to be, ghosts reappear on hover). So we
    // never touch a native button: the WEIGHT goes on the 2nd line instead.
    // Expand/"sword" icon (icon_num): kept in place, just strip its tooltip +
    // click so it's image-only (the slot-expand feature is unused).
    static void* s_neutralized = nullptr;
    if (void* exp = Child(wnd, kBtnExpand)) {
      if (s_neutralized != wnd) {
        reinterpret_cast<SetName_t>(kSetName)(exp, nullptr, "");  // no tooltip
        VCall1(exp, kVfSetCmd, 0);                               // no click action
        s_neutralized = wnd;
      }
    }

    // Route colorchip chips into the tab fill/border + the window-title colour so
    // users re-theme everything via colorchip.bmp. Sampled once (cached).
    static unsigned s_tabFill = 0, s_tabBorder = 0;
    static bool s_haveTabCol = false;
    if (!s_haveTabCol) {
      s_tabFill   = ChipColor(kTabChipX, kTabChipY);
      s_tabBorder = ChipColor(kBorderChipX, kBorderChipY);
      // window-title text colour (DrawTitleBar, shared by ALL windows): patch the
      // PUSH 0xffffff immediate once from a colorchip chip.
      PatchValue<uint32_t>(kTitleColorImm, ChipColor(kTitleChipX, kTitleChipY));
      // item-quantity white outline (FUN_008711a0, 4 immediates): patch once.
      const unsigned itemCol = ChipColor(kItemChipX, kItemChipY);
      for (int i = 0; i < 4; ++i) PatchValue<uint32_t>(kItemNumImm[i], itemCol);
      s_haveTabCol = true;
    }
    if (void* tab = Child(wnd, kTabCtrl)) {
      auto* t = reinterpret_cast<uint8_t*>(tab);
      *reinterpret_cast<unsigned*>(t + kTabFillSel)     = s_tabFill;
      *reinterpret_cast<unsigned*>(t + kTabFillUnsel)   = s_tabFill;
      *reinterpret_cast<unsigned*>(t + kTabBorderSel)   = s_tabBorder;
      *reinterpret_cast<unsigned*>(t + kTabBorderUnsel) = s_tabBorder;
    }

    // Fill the small grid-quantisation gap (grid bottom -> btnbar top) with the
    // GRID bg colour (tab-aware: normal=colorchip(2,2), FAV=colorchip(0,8)), so it
    // blends invisibly with the grid like the FAV tab already does — no transparent
    // hole on the 3 normal tabs (their native fill is narrow / leaves the right
    // side empty) and NO grey block. Stops at the live btnbar top (bar untouched).
    static unsigned s_gapNormal = 0, s_gapFav = 0;
    static bool s_haveGap = false;
    if (!s_haveGap) {
      s_gapNormal = ChipColor(2, 2);
      s_gapFav    = ChipColor(0, 8);
      s_haveGap = true;
    }
    {
      const bool fav = *reinterpret_cast<int*>(w + kTabCat) == 3 &&
                       *reinterpret_cast<uint8_t*>(kFavFlag) == 1;
      void* bmgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
      void* bkey = reinterpret_cast<MakeKey_t>(kMakeKey)(
          reinterpret_cast<const char*>(kBtnbarPath));
      void* btex = reinterpret_cast<LoadTex_t>(kLoadTex)(bmgr, nullptr, bkey);
      const int btnbarH = btex
          ? *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(btex) + kTexH)
          : kBarHeight;
      const int gridBottom = kCollapsedH + *reinterpret_cast<int*>(w + kRows) * kSlot;
      const int frameTop   = height - btnbarH;
      if (frameTop > gridBottom)
        reinterpret_cast<Fill_t>(kFill)(wnd, nullptr, kBarX, gridBottom,
            width - kBarX - kBarXR, frameTop - gridBottom, fav ? s_gapFav : s_gapNormal);
    }

    const int max = RD(kWeightMax);
    if (max <= 0) return;  // weight not populated yet (pre-world)
    const int cur = RD(kWeightCur);
    const int pct = static_cast<int>(static_cast<long long>(cur) * 100 / max);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d / %d (%d%%)", cur, max, pct);
    const unsigned color = (pct >= RD(kOverweightPct)) ? kColorOver : kColorNormal;

    // The cart's weight scale icon (texture manager caches by key per frame).
    void* mgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
    void* key = reinterpret_cast<MakeKey_t>(kMakeKey)(
        reinterpret_cast<const char*>(kIconPath));
    void* tex = reinterpret_cast<LoadTex_t>(kLoadTex)(mgr, nullptr, key);
    int iconW = 0, iconH = 0;
    if (tex) {
      iconW = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(tex) + kTexW);
      iconH = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(tex) + kTexH);
    }

    // Both share one line just above the native count: WEIGHT (icon + value)
    // left, ZENY right. (Kept low, not centred, so enlarging the zone grows the
    // empty colorchip area ABOVE them rather than moving the text.)
    const int textY = height - kLine2Y;

    const int iconX = kLeftMargin;
    const int iconY = textY - (iconH - 11) / 2;  // center icon against the glyphs
    if (tex) reinterpret_cast<Blit_t>(kBlit)(wnd, nullptr, iconX, iconY, tex, 1);
    reinterpret_cast<DrawText_t>(kDrawText)(
        wnd, nullptr, iconX + iconW + kIconGap, textY, buf, 0, 0, 0xb, color, 0, 0);

    // Zeny, right-aligned, comma-formatted like the basic-info window.
    char zbuf[40];
    reinterpret_cast<FmtComma_t>(kFmtComma)(RD(kZeny), zbuf, sizeof(zbuf));
    char zline[64];
    std::snprintf(zline, sizeof(zline), "%sz", zbuf);
    const int zW = reinterpret_cast<MeasureW_t>(kMeasureW)(
        wnd, nullptr, zline, static_cast<int>(std::strlen(zline)), 0, 0xb, 0, 0);
    reinterpret_cast<DrawText_t>(kDrawText)(
        wnd, nullptr, (width - kRightMargin) - zW, textY, zline, 0, 0, 0xb, kColorNormal, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Replacement for the tab control's DrawContent (FUN_00857910, vtable +0x50).
// The class is shared, so only OUR inventory tab is themed: empty each tab's
// label (so the native draw paints no text), let the original clear+draw the
// strip into the node, then blit each tab's image (active = "<name>1", inactive
// = "<name>2"). Riding inside the strip draw means the images are repainted on
// every render -> they survive tab switches and map changes. Tab i's top y and
// width match the native layout in FUN_00847330 (tabs overlap 1px, full width).
void __fastcall TabDrawContentHook(void* tabobj, void* /*edx*/) {
  __try {
    const bool ours = IsInventoryTab(tabobj);
    auto* t = reinterpret_cast<uint8_t*>(tabobj);
    const int count = ours ? TabCount(tabobj) : 0;
    if (ours) {
      // Empty each tab's label where its image is available (else keep its text).
      if (uint8_t* lbeg = *reinterpret_cast<uint8_t**>(t + kTabLabelVec)) {
        for (int i = 0; i < count && i < kTabCount; ++i) {
          if (!TabImg(i, true)) continue;
          uint8_t* s = lbeg + i * 0x18;
          if (*reinterpret_cast<unsigned*>(s + 0x10) != 0) {
            const unsigned cap = *reinterpret_cast<unsigned*>(s + 0x14);
            char* data = (cap > 0xf) ? *reinterpret_cast<char**>(s)
                                     : reinterpret_cast<char*>(s);
            data[0] = '\0';
            *reinterpret_cast<unsigned*>(s + 0x10) = 0;
          }
        }
      }
    }
    g_tab_draw_orig(tabobj, nullptr);  // clears the strip node + draws all tabs
    if (ours) {
      const int sel = *reinterpret_cast<int*>(t + kTabSel);
      const int sw  = *reinterpret_cast<int*>(t + kWndWidth);
      int* sz = *reinterpret_cast<int**>(t + kTabSizeArr);
      int y = 0;  // tab i top = sum(size[0..i-1] - 1), matching FUN_00847330
      for (int i = 0; i < count && i < kTabCount; ++i) {
        if (void* ttex = TabImg(i, i == sel)) {
          const int iw = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ttex) + kTexW);
          const int bx = (sw > iw) ? (sw - iw) / 2 : 0;  // centre in the strip width
          reinterpret_cast<Blit_t>(kBlit)(tabobj, nullptr, bx, y, ttex, 0);
        }
        if (sz) y += sz[i] - 1;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Append the extra tab(s) so the control has kTabCount tabs. Idempotent (only adds
// while short). AddTab resets the selected index to 0 — fine before a layout-restore
// (case 0x22 re-sets it) and at creation (opens on Use). Runs in the LOGIC phase
// (message handling / OnTick), never during render — render-time AddTab is untested
// but the v1 crash was the msg-hook arity, not this.
inline void EnsureExtraTab(void* tabobj) {
  for (int guard = 0; TabCount(tabobj) < kTabCount && guard < kTabCount; ++guard)
    reinterpret_cast<AddTab_t>(kAddTab)(tabobj, nullptr, "", "");
}

// Erase nodes from the slot list inv+0xe8 by item type, FOR REAL (frees the node via
// the native std::list::erase so size, draw AND hit-test all stay consistent — a
// draw-only filter desynced the click index). keep=true erases everything whose type
// != `type` (keep only cards); keep=false erases type == `type` (drop cards from Etc).
void PruneList(void* self, int type, bool keep) {
  void* listObj = reinterpret_cast<uint8_t*>(self) + kListHead;  // {head ptr @+0, size @+4}
  void* head = *reinterpret_cast<void**>(listObj);
  if (!head) return;
  void* cur = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(head) + kNodeNext);
  while (cur != head) {
    void* next = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + kNodeNext);
    const int t = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(cur) + kNodeType);
    if (keep ? (t != type) : (t == type)) {
      void* out = nullptr;
      reinterpret_cast<Erase_t>(kListErase)(listObj, nullptr, &out, cur, next);  // erase [cur,next)
    }
    cur = next;
  }
}

// Rebuild the filtered slot list inv+0xe8 for the CURRENT category, then prune so the
// REAL list matches the tab: Cards (cat 4) is built as Etc (which admits cards) then
// pruned to cards-only; Etc (cat 2) drops the cards. FUN_00950400 re-syncs the
// scrollbar/scroll to the new count. Other categories run the native refresh as-is.
void DoRefresh(void* self, void* edx) {
  auto* cat = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + kTabCat);
  if (*cat == kCardCat) {
    auto* sub = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + kEtcSub);
    const int savedSub = *sub;
    *cat = kEtcCat;
    *sub = 4;  // "show all etc" so every card is in the build
    g_msg_orig(self, edx, 0, kMsgRefresh, 0, 0, 0, 0);
    *cat = kCardCat;
    *sub = savedSub;
    PruneList(self, kItCard, /*keep=*/true);   // keep only cards
    reinterpret_cast<Finalize_t>(kInvFinalize)(self);
  } else if (*cat == kEtcCat) {
    g_msg_orig(self, edx, 0, kMsgRefresh, 0, 0, 0, 0);
    PruneList(self, kItCard, /*keep=*/false);  // remove cards from Etc
    reinterpret_cast<Finalize_t>(kInvFinalize)(self);
  } else {
    g_msg_orig(self, edx, 0, kMsgRefresh, 0, 0, 0, 0);
  }
}

// Replacement for the inventory message handler FUN_00955530 (vtable +0x94). Only
// three messages are intercepted; everything else passes straight through:
//  - 0x16 (tab clicked): remap the clicked VISUAL slot -> a category (so we can
//    order tabs freely; Fav stays cat 3 so native ==3 gates are untouched), then
//    refresh (cards-aware).
//  - 0x17 (refresh, e.g. item add/remove while a tab is open): cards-aware rebuild.
//  - 0x22 (layout restore): native sets inv+0x10c = saved visual slot; remap it.
int __fastcall MsgHook(void* self, void* edx, int p1, int msg, int p3, int p4, int p5, int p6) {
  __try {
    // Ensure the 5th tab exists BEFORE any message is handled — especially the
    // layout-restore (0x22): with only 4 tabs the saved tab index is out of range
    // and the resize miscomputes (window jumps left + tab not remembered).
    if (void* tab = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(self) + kTabCtrl))
      EnsureExtraTab(tab);
    if (msg == kMsgSelectTab) {
      const int cat = (p3 >= 0 && p3 < kTabCount) ? kSlotCategory[p3] : p3;
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + kTabCat) = cat;
      DoRefresh(self, edx);
      return 0;
    }
    if (msg == kMsgRefresh) {
      DoRefresh(self, edx);
      return 0;
    }
    if (msg == kMsgRestore) {
      // Size the tab strip to its image height BEFORE the native restore. That
      // restore clamps the window height up to (tabStripHeight + bottom reserve);
      // pre-render the tabs sit at their taller NATIVE height, so a saved SHORT
      // height gets clamped up to an inflated floor and lost (the window springs
      // back to "standard" size on reopen). Sizing first makes the restore floor
      // match the rendered strip (= the live resize floor), so the saved height
      // survives. EnsureExtraTab above guarantees all 5 tabs exist first.
      if (void* tab = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(self) + kTabCtrl)) {
        LoadTabImages();
        SizeTabsToImages(tab);
      }
      const int r = g_msg_orig(self, edx, p1, msg, p3, p4, p5, p6);
      // The save persists the CATEGORY (inv+0x10c); native restore copied it into BOTH
      // inv+0x10c AND tabctrl+0x7c. Keep the category (the internal 0x17 already built
      // its list); just move the highlighted SLOT to the tab that shows that category
      // (kSlotCategory is the 3<->4 swap, self-inverse, so it maps cat->slot too).
      const int v = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + kTabCat);
      if (v >= 0 && v < kTabCount) {
        if (void* tab = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(self) + kTabCtrl))
          *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(tab) + kTabSel) = kSlotCategory[v];
      }
      return r;
    }
    return g_msg_orig(self, edx, p1, msg, p3, p4, p5, p6);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

template <typename T>
void PatchValue(uintptr_t addr, T value) {
  DWORD old_protect;
  if (VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T),
                     PAGE_EXECUTE_READWRITE, &old_protect)) {
    *reinterpret_cast<T*>(addr) = value;
    VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), sizeof(T));
  }
}

}  // namespace

InventoryTweaks::InventoryTweaks() {
  // 1) Swap the DrawContent vtable slot (in .data, RW) for the weight readout.
  const uintptr_t cur_slot = *reinterpret_cast<uintptr_t*>(kDrawSlot);
  if (cur_slot == kDrawOrig) {
    PatchValue<void*>(kDrawSlot, reinterpret_cast<void*>(&DrawContentHook));
    LogInfo("[Inventory] DrawContent vtable hook installed (weight readout)");
  } else {
    LogError("[Inventory] vtable slot 0x0103d4b0 = 0x{:x}, expected 0x00946da0; "
             "hook skipped", cur_slot);
  }

  // 1b) Hook the tab control's DrawContent (shared-class vtable +0x50) so the
  //     tab images are repainted inside every strip render -> they survive tab
  //     switches and map changes. Guarded per-instance via IsInventoryTab.
  const uintptr_t tab_slot = *reinterpret_cast<uintptr_t*>(kTabDrawSlot);
  if (tab_slot == kTabDrawOrig) {
    PatchValue<void*>(kTabDrawSlot, reinterpret_cast<void*>(&TabDrawContentHook));
    LogInfo("[Inventory] tab DrawContent vtable hook installed (tab images)");
  } else {
    LogError("[Inventory] tab vtable slot 0x{:x} = 0x{:x}, expected 0x{:x}; "
             "tab hook skipped", kTabDrawSlot, tab_slot, kTabDrawOrig);
  }

  // 1c) Hook the inventory message handler (vtable +0x94 = FUN_00955530) so the
  //     extra Cards tab works: remap clicked slot -> category (0x16), cards-aware
  //     refresh (0x17), and remap on layout restore (0x22). All other messages
  //     pass straight through. DrawContentHook appends the 5th tab + filters cards.
  const uintptr_t msg_slot = *reinterpret_cast<uintptr_t*>(kMsgSlot);
  if (kEnableCardsFilter && msg_slot == kMsgOrig) {
    PatchValue<void*>(kMsgSlot, reinterpret_cast<void*>(&MsgHook));
    LogInfo("[Inventory] message-handler vtable hook installed (Cards tab)");
  } else if (kEnableCardsFilter) {
    LogError("[Inventory] msg vtable slot 0x{:x} = 0x{:x}, expected 0x{:x}; "
             "Cards tab hook skipped", kMsgSlot, msg_slot, kMsgOrig);
  }

  // 1d) Let the layout-restore validator (FUN_0096b700) accept the Cards category 4:
  //     bump its 'tab <= 3' bound to 'tab <= 4'. Without this, closing on Cards saves
  //     category 4, the validator rejects it, and the client restores the DEFAULT
  //     layout (window at x=0 + Use tab) instead of the saved position/tab.
  if (kEnableCardsFilter) {
    const uint8_t bound = *reinterpret_cast<uint8_t*>(kLayoutTabBoundImm);
    if (bound == 3) {
      PatchValue<uint8_t>(kLayoutTabBoundImm, 4);
      LogInfo("[Inventory] layout-restore tab bound 3->4 (Cards layout persists)");
    } else {
      LogError("[Inventory] layout bound imm @0x{:x} = {}, expected 3; patch skipped",
               kLayoutTabBoundImm, bound);
    }
  }

  // 2) Raise the user-resize max clamp (MOV EDI,0x140 / MOV EDX,0xf0 in the
  //    case-0xe handler).  Background is tiled, so larger sizes render fine.
  //    Runtime equivalent of the WARP 'BiggerInventoryWindow' patch — a no-op
  //    if that exe patch already rewrote these immediates.
  const uint32_t cw = *reinterpret_cast<uint32_t*>(kMaxWidthImm);
  const uint32_t ch = *reinterpret_cast<uint32_t*>(kMaxHeightImm);
  if (cw == kStockMaxW && ch == kStockMaxH) {
    PatchValue<uint32_t>(kMaxWidthImm, kNewMaxW);
    PatchValue<uint32_t>(kMaxHeightImm, kNewMaxH);
    LogInfo("[Inventory] resize max unlocked to {}x{}", kNewMaxW, kNewMaxH);
  } else {
    LogInfo("[Inventory] resize clamp already non-stock (w=0x{:x} h=0x{:x}); "
            "likely the WARP patch — runtime unlock skipped", cw, ch);
  }

  // 3) Enlarge the bottom reserve (0x26 -> kRsvNew) at ALL sites so the grid,
  //    bg fills, scrollbar and resize snap agree on a 3-line bottom bar. The
  //    window itself is grown by kRsvGrow in the hook so the grid keeps its rows.
  bool rsv_ok = true;
  for (uintptr_t a : kRsvPos)
    if (*reinterpret_cast<uint8_t*>(a) != kRsvOld) rsv_ok = false;
  for (uintptr_t a : kRsvNeg)
    if (*reinterpret_cast<uint8_t*>(a) != static_cast<uint8_t>(-kRsvOld)) rsv_ok = false;
  if (rsv_ok) {
    for (uintptr_t a : kRsvPos) PatchValue<uint8_t>(a, kRsvNew);
    for (uintptr_t a : kRsvNeg) PatchValue<uint8_t>(a, static_cast<uint8_t>(-kRsvNew));
    LogInfo("[Inventory] bottom reserve enlarged 0x{:x}->0x{:x} (3-line bar)",
            kRsvOld, kRsvNew);
  } else {
    LogError("[Inventory] bottom-reserve immediates unexpected; enlarge skipped");
  }
}

// Logic-phase (throttled, NOT during render): append the extra "Cards" tab once
// per inventory instance. AddTab -> Recompute recreates the strip node; doing that
// mid-render corrupted window state in v1, so it lives here. Hit-test + draw derive
// the tab count from the label vector, so the new tab is honored automatically.
void InventoryTweaks::OnTick() {
  if (!kEnableCardsTab) return;
  __try {
    void* inv = *reinterpret_cast<void**>(kInvWndGlobal);
    if (!inv) return;
    if (void* tabobj = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(inv) + kTabCtrl))
      EnsureExtraTab(tabobj);  // fallback; MsgHook already adds it before the restore
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
