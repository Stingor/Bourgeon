#include "features/patches/status_tweaks.h"

#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <climits>
#include <cstdint>
#include <cstdio>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "utils/log_console.h"

// ===========================================================================
// UIStatusWnd relayout (20250716 client / Moonlight-Destiny.exe, base 0x400000)
//
// Background bitmap (the game always reads this, skins aside):
//   data\texture\<유저인터페이스>\statuswnd\w_statwin_bg.bmp  = 302 x 115, labels
//   baked into the art.  Code draws only the dynamic numbers.
//
// Bitmap row centers (bitmap-Y of each label band), shared by the left stat
// rows and the right derived-stat rows:
//   row 0..6 = { 12, 27, 43, 59, 76, 91, 106 }
// The bg is blitted at node-Y = *(this+0xd4) (= 17, the title-bar height), so a
// value's node-Y = blitY + rowCenter - 6  (text top vs label center).
//
// Layout produced (see overlay verification):
//   LEFT box [34..84 | 84..98 | 98..123]:
//     base value   left  x=36
//     +bonus       left  x=49 (base<100) / x=54,y+2,sz12 (base>=100)
//     + arrow btn  reposi  x=86 (box arrow-cell)
//     raise-cost   right x=121
//   LEFT row 6: Status Point value right x=121
//   RIGHT (right-aligned at x):
//     row0 Atk        "%d + %d"  x=294
//     row1 Aspd       "%d"       x=208     Flee  "%d + %d"  x=294 (sz12)
//     row2 Def        "%d + %d"  x=208(sz12)  Mdef "%d + %d" x=294 (sz12)
//     row3 Matk       "%d ~ %d"  x=294
//     row4 Hit        "%d"       x=294
//     row5 Critical   "%d"       x=294
//     row6 Guild name (if present) right x=294, sz12, fontFace 0
// ===========================================================================

namespace {

// ---- patch sites -----------------------------------------------------------
constexpr uintptr_t kHeightImm = 0x00a3a48b;  // push 0x8d (h=141) imm in MakeWindow
constexpr uintptr_t kWidthImm  = 0x00a3a490;  // push 0x118 (w=280) imm in MakeWindow
constexpr uintptr_t kDrawSlot  = 0x01032a24;  // UIStatusWnd vtable +0x50 (DrawContent)
constexpr uintptr_t kDrawOrig  = 0x008b66a0;  // original UIStatusWnd::DrawContent
// Position persistence (the engine never saves window id 0xb — see workflow RE).
constexpr uintptr_t kMsgSlot   = 0x01032a68;  // UIStatusWnd vtable +0x94 (message handler slot)
constexpr uintptr_t kMsgOrig   = 0x008cb7c0;  // FUN_008cb7c0 status msg handler (ret 0x18 = SIX stack args!)
constexpr int kVfSetPos  = 0x10;   // UIWindow::SetPos(x,y) (vtable+0x10)
constexpr int kWinX      = 0x1c;   // window live x
constexpr int kWinY      = 0x20;   // window live y
constexpr int kMsgCmd    = 6;      // command message
constexpr int kSubClose  = 0xc9;   // msg 6 sub-command = close
constexpr int kMsgRestore = 0x22;  // layout-restore message
constexpr int kStatusId  = 0xb;    // UIStatusWnd window id

constexpr uint32_t  kNewWidth  = 302;
constexpr uint32_t  kNewHeight = 132;         // 17 title bar + 115 bitmap

// ---- engine functions ------------------------------------------------------
constexpr uintptr_t kDrawTitleBar = 0x00898bc0;  // __thiscall(this, char hasClose, char* title, int width)
constexpr uintptr_t kDrawText     = 0x00a25a70;  // __thiscall(this,x,y,str,len,face,size,color,bold,ital)  LEFT
constexpr uintptr_t kDrawTextR    = 0x00a27b50;  // __thiscall(this,x,y,str,len,face,size,color,bold) RIGHT@x
constexpr uintptr_t kBlit         = 0x00a1d260;  // __thiscall(this,x,y,img,flag)
constexpr uintptr_t kTexMgr       = 0x00a90350;  // __cdecl() -> tex mgr
constexpr uintptr_t kMakeKey      = 0x00a9f030;  // __cdecl(path) -> key
constexpr uintptr_t kLoadTex      = 0x00a8d4a0;  // __thiscall(mgr, key) -> tex
constexpr uintptr_t kGetMsg       = 0x00a9ed30;  // __cdecl(uint id) -> char*
constexpr uintptr_t kBgNormalPath = 0x010361b4;  // "...\statuswnd\w_statwin_bg.bmp"

// ---- GLOBAL title-bar text offset (shared UIWindow_DrawTitleBar 0x00898bc0) -
// Moves the title text in EVERY window (the title position is hardcoded in this
// shared function).  +dx = right, +dy = down.  0/0 leaves it unchanged.
constexpr int kTitleDx = 0;
constexpr int kTitleDy = -2;
constexpr uintptr_t kTitleWhiteX = 0x00898cd5;  // push 0x13  (white title x)
constexpr uintptr_t kTitleWhiteY = 0x00898cd2;  // lea eax,[edi-0xd] disp8 (white y-off)
constexpr uintptr_t kTitleBlackX = 0x00898cef;  // push 0x12  (black edge x)
constexpr uintptr_t kTitleBlackY = 0x00898cea;  // lea eax,[edi-0xe] disp8 (black y-off)

using DrawOrig_t = void (__fastcall*)(void*, void*);
using TitleBar_t = void (__fastcall*)(void*, void*, char, const char*, int);
using DrawText_t = void (__fastcall*)(void*, void*, int, int, const char*, unsigned,
                                      int, int, unsigned, unsigned char, unsigned char);
using DrawTextR_t = void (__fastcall*)(void*, void*, int, int, const char*, unsigned,
                                       int, int, unsigned, unsigned char);
using Blit_t   = void (__fastcall*)(void*, void*, int, int, void*, int);
using TexMgr_t = void* (__cdecl*)();
using MakeKey_t = void* (__cdecl*)(const char*);
using LoadTex_t = void* (__fastcall*)(void*, void*, void*);
using GetMsg_t  = char* (__cdecl*)(unsigned);
using StatusMsg_t = int (__fastcall*)(void*, void*, int, int, int, int, int, int);  // FUN_008cb7c0 — SIX stack args (ret 0x18)
using SetPos_t    = void (__fastcall*)(void*, void*, int, int);                       // UIWindow SetPos(this,x,y)

const auto g_status_msg_orig = reinterpret_cast<StatusMsg_t>(kMsgOrig);
int g_posX = INT_MIN, g_posY = INT_MIN;  // saved STATUS window position (INT_MIN = unset)
bool g_restorePending = false;           // a freshly-loaded position waiting to be re-applied

// ---- session fields (g_session = 0x015fa3c0) -------------------------------
const uintptr_t kBase[6]  = {0x015fba24, 0x015fba28, 0x015fba2c,
                             0x015fba30, 0x015fba34, 0x015fba38};
const uintptr_t kBonus[6] = {0x015fba0c, 0x015fba10, 0x015fba14,
                             0x015fba18, 0x015fba1c, 0x015fba20};
const uintptr_t kRaise[6] = {0x015fba3c, 0x015fba40, 0x015fba44,
                             0x015fba48, 0x015fba4c, 0x015fba50};
constexpr uintptr_t kAtk1     = 0x015fba58, kAtk2     = 0x015fba6c;
constexpr uintptr_t kDefSoft  = 0x015fba64, kDefHard  = 0x015fba68;
constexpr uintptr_t kMdefSoft = 0x015fba5c, kMdefHard = 0x015fba78;
constexpr uintptr_t kMatkMin  = 0x015fba74, kMatkMax  = 0x015fba70;
constexpr uintptr_t kHit      = 0x015fba7c, kCrit     = 0x015fba84;
constexpr uintptr_t kFlee     = 0x015fba80, kPdodge   = 0x015fba88;
constexpr uintptr_t kAspdRaw  = 0x015fba54;
constexpr uintptr_t kStatusPt = 0x015fb9f4;
constexpr uintptr_t kGuildLen = 0x0159c198;  // std::string _Mysize (0 == no guild)
constexpr uintptr_t kGuildCap = 0x0159c19c;  // std::string _Myres  (>=0x10 => heap ptr)
constexpr uintptr_t kGuildBuf = 0x0159c188;  // std::string buffer/ptr union

const int kRowCenter[7] = {12, 28, 44, 60, 76, 92, 106}; // bitmap-Y of each label row, shared left/right

// Mouseover tooltip hit-rects (OnMouseMove FUN_008ba610, vtable+0x70). 16 rects,
// each {x,y} stored as two int32 in .rdata; the handler tests
// (x <= cx <= x+0x50) && (y <= cy <= y+0xc) and shows GetMsg(helpid). The stock
// rects sit at the OLD layout; we relocate each to its stat's new position.
// Rect is 80px wide: left rects cover label+box; right rects are anchored on the
// label (x=130 colA / x=216 colB) so hovering the stat NAME shows its tooltip.
// help-id -> stat resolved from data\msgstringtable.csv (MSI_DESC_*).
struct HoverRect { uintptr_t xa, ya; int nx, ny; };  // ny = blitY(17)+rowCenter-6
const HoverRect kHoverRects[16] = {
    {0x010371e0, 0x010371e4,   6,  23},  // STR  0xc28  left r0
    {0x010371e8, 0x010371ec,   6,  38},  // AGI  0xc29  left r1
    {0x01037240, 0x01037244,   6,  54},  // VIT  0xc2a  left r2
    {0x01037248, 0x0103724c,   6,  70},  // INT  0xc2b  left r3
    {0x010372b0, 0x010372b4,   6,  87},  // DEX  0xc2c  left r4
    {0x010372b8, 0x010372bc,   6, 102},  // LUK  0xc2d  left r5
    {0x010371c0, 0x010371c4, 130,  23},  // ATK  0xc2e  right r0 (full)
    {0x010371c8, 0x010371cc, 130,  70},  // MATK 0xc34  right r3 (full)
    {0x01037220, 0x01037224, 130,  87},  // HIT  0xc30  right r4 (full)
    {0x01037228, 0x0103722c, 130, 102},  // CRI  0xc31  right r5 (full)
    {0x010372a0, 0x010372a4,   6, 117},  // POINT0xc33  left  r6 (Status Point)
    {0x010372a8, 0x010372ac, 130, 117},  // GUILD0xc32  right r6
    {0x010371d0, 0x010371d4, 130,  54},  // DEF  0xc2f  right r2 colA
    {0x010371d8, 0x010371dc, 216,  54},  // MDEF 0xc35  right r2 colB
    {0x01037230, 0x01037234, 216,  38},  // FLEE 0xc36  right r1 colB
    {0x01037238, 0x0103723c, 130,  38},  // ASPD 0xc37  right r1 colA
};
constexpr uintptr_t kRectGuardAddr = 0x010371c0;  // ATK rect x; stock value 108

inline int RD(uintptr_t a) { return *reinterpret_cast<int*>(a); }
inline int RDo(void* base, int off) {
  return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(base) + off);
}

void DTextL(void* w, int x, int y, const char* s, int size, int face = 1) {
  reinterpret_cast<DrawText_t>(kDrawText)(w, nullptr, x, y, s, 0, face, size, 0, 0, 0);
}
void DTextR(void* w, int x, int y, const char* s, int size, int face = 1) {
  reinterpret_cast<DrawTextR_t>(kDrawTextR)(w, nullptr, x, y, s, 0, face, size, 0, 0);
}

// Draw all NORMAL-view content at the new 302x115 coordinates.  POD-only
// locals so the caller can wrap us in SEH.
void DrawNormal(void* wnd, int blitY) {
  char buf[40];

  // ---- left column: base / +bonus / raise-cost --------------------------
  for (int i = 0; i < 6; ++i) {
    const int y = blitY + kRowCenter[i] - 6;
    const int base = RD(kBase[i]);
    int txtsize = 13;
    if( base > 999) txtsize = 11;
    snprintf(buf, sizeof(buf), "%d", base);
    DTextL(wnd, 37, y, buf, txtsize);
    txtsize = 13;
    const int bonus = RD(kBonus[i]);
    if (bonus != 0) {
      if( bonus > 999) txtsize = 11;
      snprintf(buf, sizeof(buf), (bonus > 0) ? "+" : "-");
      DTextL(wnd, 55, y, buf, txtsize);
      snprintf(buf, sizeof(buf), "%d", (bonus > 0) ? bonus : bonus * -1);
      DTextL(wnd, 61, y, buf, txtsize);
    }

    snprintf(buf, sizeof(buf), "%d", RD(kRaise[i]));
    DTextR(wnd, 121, y, buf, 13);
  }

  const int y6 = blitY + kRowCenter[6] - 6;  // row 7 (Status Point / Guild)

  // ---- left row 7: Status Point -----------------------------------------
  snprintf(buf, sizeof(buf), "%d", RD(kStatusPt));
  DTextR(wnd, 121, y6, buf, 13);

  // ---- right side --------------------------------------------------------
  const int y0 = blitY + kRowCenter[0] - 6;
  snprintf(buf, sizeof(buf), "%d + %d", RD(kAtk1), RD(kAtk2));
  DTextR(wnd, 294, y0, buf, 13);

  const int y1 = blitY + kRowCenter[1] - 6;
  snprintf(buf, sizeof(buf), "%d", (2000 - RD(kAspdRaw)) / 10);  // displayed ASPD
  DTextR(wnd, 208, y1, buf, 13);
  snprintf(buf, sizeof(buf), "%d", RD(kFlee));
  DTextR(wnd, 294, y1, buf, 13);

  const int y2 = blitY + kRowCenter[2] - 6;
  snprintf(buf, sizeof(buf), "%d + %d", RD(kDefSoft), RD(kDefHard));
  DTextR(wnd, 208, y2, buf, 13);
  snprintf(buf, sizeof(buf), "%d + %d", RD(kMdefSoft), RD(kMdefHard));
  DTextR(wnd, 294, y2, buf, 13);

  const int y3 = blitY + kRowCenter[3] - 6;
  snprintf(buf, sizeof(buf), "%d ~ %d", RD(kMatkMin), RD(kMatkMax));
  DTextR(wnd, 294, y3, buf, 13);

  const int y4 = blitY + kRowCenter[4] - 6;
  snprintf(buf, sizeof(buf), "%d", RD(kHit));
  DTextR(wnd, 208, y4, buf, 13);  // col A (aligns under Aspd/Def)

  const int y5 = blitY + kRowCenter[5] - 6;
  snprintf(buf, sizeof(buf), "%d", RD(kCrit));
  DTextR(wnd, 208, y5, buf, 13);  // col A (aligns under Aspd/Def)
  snprintf(buf, sizeof(buf), "%d", RD(kPdodge));
  DTextR(wnd, 294, y5, buf, 13);

  // ---- right row 7: guild name (if any) ---------------------------------
  if (RD(kGuildLen) != 0) {
    const unsigned cap = *reinterpret_cast<unsigned*>(kGuildCap);
    const char* gname = (cap < 0x10) ? reinterpret_cast<const char*>(kGuildBuf)
                                     : *reinterpret_cast<const char**>(kGuildBuf);
    DTextL(wnd, 163, y6 - 1, gname, 12, 0);  // left-aligned right after the Guild label
  }

  // ---- reposition the 6 stat-up arrow buttons into the box arrow-cell -----
  // Replicate the native show/hide rule (FUN_008cb7c0 case 0x23, RE-verified): show
  // the arrow only when the stat can still be raised (raise-cost != 0 — the server
  // sends 0 at max level) AND the player has enough points (kStatusPt >= cost);
  // otherwise hide it off-screen exactly like the native (-100,-100). We rewrite the
  // button x/y every frame, so without this our relayout re-showed arrows the native
  // had hidden. Mirror the native arithmetic literally — no extra max-level logic.
  const int points = RD(kStatusPt);
  for (int i = 0; i < 6; ++i) {
    void* btn = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(wnd) + 0xb4 + i * 4);
    if (!btn) continue;
    const int cost = RD(kRaise[i]);
    const bool show = (cost != 0) && (points >= cost);
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(btn) + 0x1c) = show ? 88 : -100;
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(btn) + 0x20) =
        show ? (blitY + kRowCenter[i] - 5) : -100;
  }
}

// Replacement for UIStatusWnd::DrawContent (__thiscall -> __fastcall, edx unused).
void __fastcall DrawContentHook(void* wnd, void* /*edx*/) {
  __try {
    // 4th-job expanded trait view: leave it to the original for now.
    if (*(reinterpret_cast<char*>(wnd) + 0xfc) == 1) {
      reinterpret_cast<DrawOrig_t>(kDrawOrig)(wnd, nullptr);
      return;
    }

    char* title = reinterpret_cast<GetMsg_t>(kGetMsg)(0x69);
    reinterpret_cast<TitleBar_t>(kDrawTitleBar)(wnd, nullptr, 1, title, 0);

    const int blitY = RDo(wnd, 0xd4);
    if (RDo(wnd, 0x18) == blitY || RDo(wnd, 0x30) != 0) return;  // minimized/guard

    void* mgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
    void* key = reinterpret_cast<MakeKey_t>(kMakeKey)(
        reinterpret_cast<const char*>(kBgNormalPath));
    void* tex = reinterpret_cast<LoadTex_t>(kLoadTex)(mgr, nullptr, key);
    if (tex) reinterpret_cast<Blit_t>(kBlit)(wnd, nullptr, 0, blitY, tex, 1);

    DrawNormal(wnd, blitY);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Replacement for the STATUS message handler FUN_008cb7c0 (vtable +0x94). The engine
// never persists window id 0xb's position, so we do: snapshot live x/y on close (msg 6
// sub 0xc9) + trigger a settings save, and on layout-restore (msg 0x22) override SetPos
// with the saved x/y AFTER the native restore (we override, so the native validator/
// default are irrelevant). SIX stack args / ret 0x18 — a 5-arg hook corrupts the stack.
int __fastcall StatusMsgHook(void* self, void* edx, int p1, int msg, int p3,
                             int p4, int p5, int p6) {
  __try {
    if (msg == kMsgCmd && p3 == kSubClose) {  // X close -> persist immediately
      g_posX = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + kWinX);
      g_posY = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + kWinY);
      const int r = g_status_msg_orig(self, edx, p1, msg, p3, p4, p5, p6);
      if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
      return r;
    }
    const int r = g_status_msg_orig(self, edx, p1, msg, p3, p4, p5, p6);
    // After the native layout-restore, override with the saved position (we override,
    // so the native validator/default are irrelevant). OnTick (live FindWindow read,
    // throttled 200ms) + this close-save cover drag-end and every close path.
    if (msg == kMsgRestore && g_posX != INT_MIN && g_posX >= 0 && g_posY >= 0) {
      reinterpret_cast<SetPos_t>(*reinterpret_cast<uintptr_t*>(
          *reinterpret_cast<uintptr_t*>(self) + kVfSetPos))(self, nullptr, g_posX, g_posY);
    }
    return r;
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

StatusTweaks::StatusTweaks() {
  // 1) Resize the window at creation: 280x141 -> 302x132 to match the bitmap.
  const uint32_t cur_h = *reinterpret_cast<uint32_t*>(kHeightImm);
  const uint32_t cur_w = *reinterpret_cast<uint32_t*>(kWidthImm);
  if (cur_h == 141 && cur_w == 280) {
    PatchValue<uint32_t>(kHeightImm, kNewHeight);
    PatchValue<uint32_t>(kWidthImm, kNewWidth);
    // LogInfo("[Status] window size patched to {}x{}", kNewWidth, kNewHeight);
  } else {
    LogError("[Status] SetSize immediates unexpected (w={} h={}); size patch skipped",
             cur_w, cur_h);
  }

  // 2) Swap the DrawContent vtable slot to our relayout (slot is in .data, RW).
  const uintptr_t cur_slot = *reinterpret_cast<uintptr_t*>(kDrawSlot);
  if (cur_slot == kDrawOrig) {
    PatchValue<void*>(kDrawSlot, reinterpret_cast<void*>(&DrawContentHook));
    // LogInfo("[Status] DrawContent vtable hook installed");
  } else {
    LogError("[Status] vtable slot 0x01032a24 = 0x{:x}, expected 0x008b66a0; hook skipped",
             cur_slot);
  }

  // 3) Relocate the 16 mouseover tooltip hit-rects to the new layout.
  const int guard = *reinterpret_cast<int*>(kRectGuardAddr);
  if (guard == 108 || guard == 130) {  // stock (108) or already-patched (130)
    for (const HoverRect& r : kHoverRects) {
      PatchValue<int32_t>(r.xa, r.nx);
      PatchValue<int32_t>(r.ya, r.ny);
    }
    // LogInfo("[Status] 16 tooltip hit-rects relocated to new layout");
  } else {
    LogError("[Status] tooltip rect guard 0x010371c0 = {}, expected 108; rect patch skipped",
             guard);
  }

  // 4) GLOBAL title-bar text offset — moves EVERY window's title text (shared
  //    DrawTitleBar). y-disp8 = -(base - dy): base 13/14, +dy moves down.
  if (*reinterpret_cast<uint8_t*>(kTitleWhiteX) == 0x13) {
    PatchValue<uint8_t>(kTitleWhiteX, static_cast<uint8_t>(19 + kTitleDx));
    PatchValue<uint8_t>(kTitleBlackX, static_cast<uint8_t>(18 + kTitleDx));
    PatchValue<uint8_t>(kTitleWhiteY, static_cast<uint8_t>(kTitleDy - 13));
    PatchValue<uint8_t>(kTitleBlackY, static_cast<uint8_t>(kTitleDy - 14));
    // LogInfo("[Status] title-bar text offset patched dx={} dy={} (all windows)",
            // kTitleDx, kTitleDy);
  } else {
    LogError("[Status] DrawTitleBar title-x = 0x{:x}, expected 0x13; title offset skipped",
             *reinterpret_cast<uint8_t*>(kTitleWhiteX));
  }

  // 5) Hook the message handler (vtable +0x94) to persist the window position the
  //    engine never saves for id 0xb: snapshot x/y on close, re-apply on open.
  const uintptr_t cur_msg = *reinterpret_cast<uintptr_t*>(kMsgSlot);
  if (cur_msg == kMsgOrig) {
    PatchValue<void*>(kMsgSlot, reinterpret_cast<void*>(&StatusMsgHook));
    // LogInfo("[Status] message-handler hook installed (position persistence)");
  } else {
    LogError("[Status] msg vtable slot 0x{:x} = 0x{:x}, expected 0x{:x}; pos-persist skipped",
             kMsgSlot, cur_msg, kMsgOrig);
  }
}

// Saved-position accessors for MoonlightUi's settings yaml (g_posX/g_posY live in
// the anonymous namespace above; these bridge them to the persistence layer).
int  StatusTweaks_SavedX() { return g_posX; }
int  StatusTweaks_SavedY() { return g_posY; }
void StatusTweaks_SetSavedPos(int x, int y) {
  g_posX = x;
  g_posY = y;
  // A newly-loaded on-screen position must be (re)applied to the live window on the
  // next tick: the client re-opens saved-open windows at their hardcoded native spot,
  // and the ordering of that vs. our msg-0x22 override is not guaranteed across a full
  // client restart, so OnTick force-applies it. This is what survives a restart.
  g_restorePending = (x != INT_MIN && x >= 0 && y >= 0);
}

// Persist the window position when it changes. We read the LIVE position straight from
// the window each tick via FindWindow — the status window does NOT redraw every frame
// (only on stat changes / periodic refresh), so capturing inside DrawContent lagged by
// up to ~1s. Throttled to once per 200ms: the first move saves within ~100ms, then at
// most every 200ms during a drag (final spot lands within ~200ms of release). Covers
// drag-end + every close path (X / Escape / Alt+A toggle / game exit). While the window
// is closed FindWindow returns null, so g_posX keeps the last spot for the next restore.
void StatusTweaks::OnTick() {
  static int savedX = INT_MIN, savedY = INT_MIN;  // last persisted position
  static DWORD last_save_ms = 0;                       // GetTickCount of the last save
  static bool init = false;
  void* win = uiwnd::FindWindow(kStatusId);
  if (!win) return;                               // status window not open

  // A position was just loaded from the yaml (fresh launch, or a re-entry into game):
  // force the live window onto it once, then switch to tracking. The client re-opens the
  // window at its hardcoded native spot, so without this the loaded value would be
  // clobbered by the live read below and the native position saved back over it. This is
  // what makes the position survive a full client restart.
  if (g_restorePending) {
    reinterpret_cast<SetPos_t>(*reinterpret_cast<uintptr_t*>(
        *reinterpret_cast<uintptr_t*>(win) + kVfSetPos))(win, nullptr, g_posX, g_posY);
    savedX = g_posX; savedY = g_posY;
    g_restorePending = false;
    init = true;
    return;
  }

  const int liveX = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kWinX);
  const int liveY = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kWinY);
  if (!init) {  // no saved pos to restore: baseline off the current spot
    savedX = liveX; savedY = liveY; g_posX = liveX; g_posY = liveY; init = true; return;
  }
  if ((liveX != savedX || liveY != savedY) && GetTickCount() - last_save_ms >= 200) {
    g_posX = liveX; g_posY = liveY;
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    savedX = liveX; savedY = liveY;
    last_save_ms = GetTickCount();
  }
}
