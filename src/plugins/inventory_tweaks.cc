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

const auto g_draw_orig = reinterpret_cast<DrawOrig_t>(kDrawOrig);

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

template <typename T> void PatchValue(uintptr_t addr, T value);  // defined below

// Sample colorchip.bmp pixel (x,y) -> game colour 0x00BBGGRR.
inline unsigned ChipColor(int x, int y) {
  unsigned r = 0, g = 0, b = 0;
  reinterpret_cast<ColorChip_t>(kColorChip)(x, y, &r, &g, &b);
  return (b << 16) | (g << 8) | r;
}

// Replacement for UIItemWnd::DrawContent (__thiscall -> __fastcall, edx unused).
// POD-only locals so the body can live under SEH.
void __fastcall DrawContentHook(void* wnd, void* /*edx*/) {
  __try {
    auto* w = reinterpret_cast<uint8_t*>(wnd);
    const int height = *reinterpret_cast<const int*>(w + kWndHeight);
    const int width  = *reinterpret_cast<const int*>(w + kWndWidth);

    // Original: slot grid + bottom-left "X/200" count + the btnbar bottom frame.
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
