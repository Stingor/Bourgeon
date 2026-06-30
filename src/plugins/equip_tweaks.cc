#include "plugins/equip_tweaks.h"

#include <Windows.h>

#include <algorithm>
#include <climits>
#include <cstdint>

#include "bourgeon.h"
#include "plugins/moonlight_ui.h"
#include "utils/log_console.h"

// ===========================================================================
// UIEquipWnd widen (20250716 client / Moonlight-Destiny.exe, base 0x400000)
//
// Window id 0xa, ctor 0x0088d740, vtable 0x0103223c, stock size 280x167.
// Slot grid renderer FUN_00897d60 draws 10 slots in two mirrored columns:
//   LEFT  : icon x=6   (imm @0x00897df1's sibling, kept), name x=33 (28+5)
//   RIGHT : name x=172 (167+5), icon x=251
// Item name text is drawn by FUN_008976a0 with a max width of 0x46=70px
// (FUN_00897d60 `push 0x46` @0x00897ffb), which is the truncation cause.
//
// We widen by a single knob kExtraWidth: grow the window, push the RIGHT icon
// right by the same amount (keeping the right name x so the name gains the new
// room toward the icon), and raise the name-width cap.  The left name gains room
// because the avatar centre (= width/2) moves right.  The slot hit-test
// (FUN_0089ec70) keys off the row + the [170 .. width-6] right zone, so it
// follows automatically.
// ===========================================================================

namespace {

// ---- THE KNOB --------------------------------------------------------------
// Extra width in pixels added to the equipment window (stock 280).  0 = no-op
// (stock layout preserved exactly).  BEFORE setting > 0, widen BOTH background
// bitmaps to (280 + kExtraWidth) px wide in the data\ folder, else a strip on
// the right stays unpainted:
//   data\texture\<ui>\basic_interface\equipwin_bg.bmp
//   data\texture\<ui>\basic_interface\equipwin_bg2.bmp
// First value: 60 -> 340px window (matches the 340px-wide equipwin_bg bitmaps).
constexpr int kExtraWidth = 60;

// The avatar is centred at width/2 and grows with the window, so the RIGHT column
// name (stock x=172, drawn rightward) ends up ON the avatar after widening. Shift
// it right by this much to clear the enlarged avatar. Tune against a screenshot.
constexpr int kRightNameShift = 33;  // 167 -> 200 (name drawn at 205)

// Item-name width cap (shared by both columns). The name renderer WRAPS the text
// to up to 3 lines at this width, so a BIGGER cap means fewer wrap-lines (long
// names draw on one line). HARD MAX = 127: the site is a `push imm8` that sign-
// extends, so 0x80+ would become a negative width. The centred avatar is drawn
// AFTER the slots, so left-column overflow is hidden behind the character (clean,
// not garbled); the right column can overlap its icon (x~311) for very long names.
// Names longer than 127px (some costumes) still wrap — exceeding 127 needs a hook
// to re-encode the push as imm32.
constexpr int kNameCap = 127;
constexpr int kStockNameCap = 0x46;  // 70 (guard)

// ---- patch sites + stock values (guards) -----------------------------------
constexpr uintptr_t kWidthOwnImm   = 0x00a3a2e4;  // push 0x118 imm32 (own equip window)
constexpr uintptr_t kWidthOtherImm = 0x00a3f0f6;  // push 0x118 imm32 (view-other-player)
constexpr uintptr_t kRightIconImm  = 0x00897df1;  // mov [ebp-0x534],0xfb  (right icon x=251)
constexpr uintptr_t kRightNameImm  = 0x00897e05;  // mov [ebp-0x544],0xa7  (right name x=167)
constexpr uintptr_t kNameCapImm[2] = {0x00897ffb,   // push 0x46 imm8 — slots 0-7 (2 columns)
                                      0x00898040};  // push 0x46 imm8 — slots 8-9 (top headgear row)
constexpr uint32_t  kStockWidth    = 0x118;       // 280
constexpr uint32_t  kStockRightIcon = 0xfb;       // 251
constexpr uint32_t  kStockRightName = 0xa7;       // 167

// Left-click DRAG/GRIP hit-test FUN_0089e460 uses TIGHT icon rectangles (NOT the
// width-following zone logic the tooltip/right-click + the Swap window use): left
// [6,30], right [251,275]. We moved the right ICON draw to 251+extra, so the right
// grip-rect must move too (else right items grip at the old empty spot). Only the
// rect BASE needs shifting — the column split (x/251, magic-compiled) still buckets
// the new icon as "right". 'lea eax,[edi-0xfb]' disp32 = -(rect base). General +
// Costume share this; the Swap window's hit-test is zone-based and already follows.
constexpr uintptr_t kDragRectImm   = 0x0089e4c8;  // lea eax,[edi-0xfb] disp32 (right grip-rect base)
constexpr int32_t   kStockDragRect = -0xfb;       // -251
// Hover-highlight (item_invert.bmp) right-column x in DrawContent, hardcoded 248
// (icon-3). Move it with the icon so the hover box sits under the moved icon.
constexpr uintptr_t kHighlightImm   = 0x008b3427;  // mov [ebp-0x128],0xf8 imm32
constexpr uint32_t  kStockHighlight = 0xf8;        // 248

// ---- Swap Equipment window (UISwapEquipmentWnd, id 0x2711, docked below) -----
// Same equip-doll layout drawn by FUN_007f8810 (its own immediates), created at
// 280x179 in the main equip WndProc. Widened with the SAME knobs. REQUIRES its
// own bgs widened to (280+kExtraWidth)px: 유저인터페이스\swap_equipment\
// equipwin_bg2_cha.bmp + equipwin_special.bmp. It has no titlebar_fix (the bg
// covers the whole window; the "Swap Equipment" title is drawn centred at x=140).
constexpr bool kEnableSwap = true;
constexpr uintptr_t kSwapWidthImm     = 0x008c049c;  // push 0x118 imm32 (swap panel SetSize width)
constexpr uintptr_t kSwapTitleImm     = 0x007f9688;  // push 0x8c imm32  (title x, centred)
constexpr uintptr_t kSwapRightIconImm = 0x007f8969;  // mov [ebp-0x4d0],0xfa (right icon x=250)
constexpr uintptr_t kSwapRightNameImm = 0x007f897d;  // mov [ebp-0x4d8],0xad (right name x=173)
constexpr uintptr_t kSwapNameCapImm[4] = {0x007f89ef, 0x007f8ab0, 0x007f8c3d, 0x007f8d5b};  // 4x push 0x46
constexpr uint32_t  kSwapStockTitle    = 0x8c;   // 140
constexpr uint32_t  kSwapStockRightIcon = 0xfa;  // 250
constexpr uint32_t  kSwapStockRightName = 0xad;  // 173

// ---- Position persistence (the engine never saves window id 0xa's position) --
// Identical mechanism to StatusTweaks (see status_tweaks.cc): hook the message
// handler (vtable +0x94 = FUN_008bf7d0, ret 0x18 = SIX stack args), snapshot the
// live x/y on close (msg 6 sub 0xc9), and on layout-restore (msg 0x22) override
// SetPos with the saved x/y AFTER the native restore. Confirmed via Ghidra:
// case 0x22 ends with a vtable+0x10 (SetPos) call; case 6/0xc9 is the X-close.
// SCOPE: only the OWN equip window (this+0xb4 == 0); the other-player view
// (this+0xb4 == 1, id 0x8b) is transient and must never touch our saved pos.
constexpr uintptr_t kMsgSlot   = 0x010322d0;  // UIEquipWnd vtable +0x94 (message handler slot)
constexpr uintptr_t kMsgOrig   = 0x008bf7d0;  // FUN_008bf7d0 equip msg handler (ret 0x18 = SIX stack args!)
constexpr int kModeFlag   = 0xb4;  // own=0, other-player view=1
constexpr int kVfSetPos   = 0x10;  // UIWindow::SetPos(x,y) (vtable+0x10)
constexpr int kWinX       = 0x1c;  // window live x
constexpr int kWinY       = 0x20;  // window live y
constexpr int kMsgCmd     = 6;     // command message
constexpr int kSubClose   = 0xc9;  // msg 6 sub-command = close
constexpr int kMsgRestore = 0x22;  // layout-restore message
constexpr uintptr_t kUIWindowMgr = 0x0131f4e8;  // g_UIWindowMgr
constexpr uintptr_t kFindWindow  = 0x00a47b90;  // FUN_00a47b90(mgr,id) -> window* (or null)
constexpr int kEquipId    = 0xa;   // OWN UIEquipWnd window id

using EquipMsg_t = int (__fastcall*)(void*, void*, int, int, int, int, int, int);  // ret 0x18
using SetPos_t     = void (__fastcall*)(void*, void*, int, int);                    // UIWindow SetPos(this,x,y)
using FindWindow_t = void* (__thiscall*)(void*, int);                               // FUN_00a47b90(mgr,id)

const auto g_equip_msg_orig = reinterpret_cast<EquipMsg_t>(kMsgOrig);
int g_posX = INT_MIN, g_posY = INT_MIN;  // saved OWN equip window position (INT_MIN = unset)

inline bool IsOwnEquip(void* self) {
  return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + kModeFlag) == 0;
}

// Replacement for the equip message handler FUN_008bf7d0 (vtable +0x94). On X-close
// (msg 6 sub 0xc9) of the OWN window, snapshot the live x/y + trigger a save; on
// layout-restore (msg 0x22) override SetPos with the saved x/y AFTER the native
// restore. SIX stack args / ret 0x18 — a wrong arg count corrupts the stack. The
// handler re-dispatches msg 6/0xca to itself inside case 0x22; our specific guards
// (0xc9 / 0x22) ignore it, so the re-entrancy is harmless.
int __fastcall EquipMsgHook(void* self, void* edx, int p1, int msg, int p3,
                            int p4, int p5, int p6) {
  __try {
    if (msg == kMsgCmd && p3 == kSubClose && IsOwnEquip(self)) {  // X close -> persist
      g_posX = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + kWinX);
      g_posY = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + kWinY);
      const int r = g_equip_msg_orig(self, edx, p1, msg, p3, p4, p5, p6);
      if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
      return r;
    }
    const int r = g_equip_msg_orig(self, edx, p1, msg, p3, p4, p5, p6);
    // After the native layout-restore, override with the saved position (we
    // override, so the native validator/default are irrelevant). OnTick (live
    // FindWindow read, throttled) + this close-save cover drag-end and closes.
    if (msg == kMsgRestore && IsOwnEquip(self) &&
        g_posX != INT_MIN && g_posX >= 0 && g_posY >= 0) {
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

EquipTweaks::EquipTweaks() {
  // Position persistence — install regardless of the widening knob: hook the
  // message handler (vtable +0x94) so the window remembers where it was.
  const uintptr_t cur_msg = *reinterpret_cast<uintptr_t*>(kMsgSlot);
  if (cur_msg == kMsgOrig) {
    PatchValue<void*>(kMsgSlot, reinterpret_cast<void*>(&EquipMsgHook));
    LogInfo("[Equip] message-handler hook installed (position persistence)");
  } else {
    LogError("[Equip] msg vtable slot 0x{:x} = 0x{:x}, expected 0x{:x}; pos-persist skipped",
             kMsgSlot, cur_msg, kMsgOrig);
  }

  if (kExtraWidth <= 0) {
    LogInfo("[Equip] widening disabled (kExtraWidth=0); window left at stock 280x167");
    return;
  }

  const uint32_t new_width = kStockWidth + static_cast<uint32_t>(kExtraWidth);
  const uint32_t new_icon  = kStockRightIcon + static_cast<uint32_t>(kExtraWidth);

  // 1) Window width: own + view-other instances (both stock 0x118).
  bool ok = true;
  if (*reinterpret_cast<uint32_t*>(kWidthOwnImm) == kStockWidth) {
    PatchValue<uint32_t>(kWidthOwnImm, new_width);
  } else {
    LogError("[Equip] own-width imm @0x{:x} = {}, expected 280; width patch skipped",
             kWidthOwnImm, *reinterpret_cast<uint32_t*>(kWidthOwnImm));
    ok = false;
  }
  if (*reinterpret_cast<uint32_t*>(kWidthOtherImm) == kStockWidth) {
    PatchValue<uint32_t>(kWidthOtherImm, new_width);
  } else {
    LogError("[Equip] other-width imm @0x{:x} = {}, expected 280; width patch skipped",
             kWidthOtherImm, *reinterpret_cast<uint32_t*>(kWidthOtherImm));
  }

  // 2) Right column icon x (= 251 + extra) and name x (= 167 + shift, to clear the
  //    enlarged centred avatar). The name gains room between it and the icon.
  if (*reinterpret_cast<uint32_t*>(kRightIconImm) == kStockRightIcon) {
    PatchValue<uint32_t>(kRightIconImm, new_icon);
  } else {
    LogError("[Equip] right-icon imm @0x{:x} = {}, expected 251; icon patch skipped",
             kRightIconImm, *reinterpret_cast<uint32_t*>(kRightIconImm));
    ok = false;
  }
  if (*reinterpret_cast<uint32_t*>(kRightNameImm) == kStockRightName) {
    PatchValue<uint32_t>(kRightNameImm, kStockRightName + static_cast<uint32_t>(kRightNameShift));
  } else {
    LogError("[Equip] right-name imm @0x{:x} = {}, expected 167; name-x patch skipped",
             kRightNameImm, *reinterpret_cast<uint32_t*>(kRightNameImm));
    ok = false;
  }

  // 3) Item-name width cap (push imm8) — BOTH sites: slots 0-7 (2 columns) and
  //    slots 8-9 (top headgear row, e.g. "3D Glasses"). Missing the 2nd left
  //    headgear names wrapping while body names didn't.
  for (uintptr_t cap : kNameCapImm) {
    if (*reinterpret_cast<uint8_t*>(cap) == kStockNameCap) {
      PatchValue<uint8_t>(cap, static_cast<uint8_t>(kNameCap));
    } else {
      LogError("[Equip] name-cap imm @0x{:x} = {}, expected 70; cap patch skipped",
               cap, *reinterpret_cast<uint8_t*>(cap));
      ok = false;
    }
  }

  // 3b) Right-column DRAG grip-rect + hover-highlight follow the moved icon
  //     (General + Costume). Without these the right items grip at the old spot.
  if (*reinterpret_cast<int32_t*>(kDragRectImm) == kStockDragRect)
    PatchValue<int32_t>(kDragRectImm, -static_cast<int32_t>(kStockRightIcon + kExtraWidth));
  else
    LogError("[Equip] drag-rect disp @0x{:x} = {}, expected -251; grip patch skipped",
             kDragRectImm, *reinterpret_cast<int32_t*>(kDragRectImm));
  if (*reinterpret_cast<uint32_t*>(kHighlightImm) == kStockHighlight)
    PatchValue<uint32_t>(kHighlightImm, kStockHighlight + static_cast<uint32_t>(kExtraWidth));

  if (ok) {
    LogInfo("[Equip] widened: window {}px, right icon x={}, name cap {}px",
            new_width, new_icon, kNameCap);
  }

  // 4) Swap Equipment window (UISwapEquipmentWnd) — same knobs, its own immediates.
  if (kEnableSwap) {
    const uint32_t swap_title = new_width / 2;  // re-centre the title
    if (*reinterpret_cast<uint32_t*>(kSwapWidthImm) == kStockWidth)
      PatchValue<uint32_t>(kSwapWidthImm, new_width);
    else
      LogError("[Equip] swap-width imm = {}, expected 280; skipped",
               *reinterpret_cast<uint32_t*>(kSwapWidthImm));
    if (*reinterpret_cast<uint32_t*>(kSwapTitleImm) == kSwapStockTitle)
      PatchValue<uint32_t>(kSwapTitleImm, swap_title);
    if (*reinterpret_cast<uint32_t*>(kSwapRightIconImm) == kSwapStockRightIcon)
      PatchValue<uint32_t>(kSwapRightIconImm,
                           kSwapStockRightIcon + static_cast<uint32_t>(kExtraWidth));
    if (*reinterpret_cast<uint32_t*>(kSwapRightNameImm) == kSwapStockRightName)
      PatchValue<uint32_t>(kSwapRightNameImm,
                           kSwapStockRightName + static_cast<uint32_t>(kRightNameShift));
    for (uintptr_t cap : kSwapNameCapImm) {
      if (*reinterpret_cast<uint8_t*>(cap) == kStockNameCap)
        PatchValue<uint8_t>(cap, static_cast<uint8_t>(kNameCap));
    }
    LogInfo("[Equip] swap window widened: {}px, title x={}", new_width, swap_title);
  }
}

// Saved-position accessors for MoonlightUi's settings yaml (g_posX/g_posY live in
// the anonymous namespace above; these bridge them to the persistence layer).
int  EquipTweaks_SavedX() { return g_posX; }
int  EquipTweaks_SavedY() { return g_posY; }
void EquipTweaks_SetSavedPos(int x, int y) { g_posX = x; g_posY = y; }

// Persist the OWN equip window's position when it changes. We read the LIVE
// position straight from the window each tick via FindWindow(id 0xa) — like the
// status window it does not redraw every frame, so a draw-time capture would lag.
// Throttled to once per 200ms (final spot lands within ~200ms of a drag release).
// Covers drag-end + every close path. While the window is closed FindWindow
// returns null, so g_posX keeps the last spot for the next restore.
void EquipTweaks::OnTick() {
  static int savedX = INT_MIN, savedY = INT_MIN;  // last persisted position
  static DWORD lastSave = 0;                       // GetTickCount of the last save
  static bool init = false;
  void* win = reinterpret_cast<FindWindow_t>(kFindWindow)(
      reinterpret_cast<void*>(kUIWindowMgr), kEquipId);
  if (!win) return;                               // equip window not open
  g_posX = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kWinX);
  g_posY = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kWinY);
  if (!init) { savedX = g_posX; savedY = g_posY; init = true; return; }  // baseline
  if ((g_posX != savedX || g_posY != savedY) && GetTickCount() - lastSave >= 200) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    savedX = g_posX; savedY = g_posY;
    lastSave = GetTickCount();
  }
}
