#include "plugins/window_pos_tweaks.h"

#include <Windows.h>

#include <climits>
#include <cstdint>

#include "bourgeon.h"
#include "plugins/moonlight_ui.h"  // full MoonlightUi type for SaveSettings()
#include "plugins/storage_tweaks.h"  // hide-native-at-creation (id 0x21)
#include "plugins/inventory_viewer.h"  // hide-native-at-creation (id 8)
#include "plugins/cashshop_tweaks.h"  // hide-native-at-creation (id 0x13e)
#include "plugins/shop_tweaks.h"  // hide-native-at-creation (id 0x16/0x17/0x19)
#include "plugins/npc_dialog_tweaks.h"  // hide-native-at-creation (dialogue 0x10/0x11/0x38/0x64/0xe2)
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// ===========================================================================
// Generic UIWindow position persistence — see window_pos_tweaks.h for the why.
//
// Two halves, table-driven over {id, key}:
//   RESTORE (flicker-free): a single jmp-hook on UIWindowMgr_MakeWindow. After
//     the factory finishes building a tracked window (SetSize + SetPos-to-centre
//     + OnCreate all run INSIDE MakeWindow), we override its position with the
//     saved one BEFORE the function returns — i.e. before the window is ever
//     rendered. This works regardless of how a given window positions itself
//     internally (direct SetPos vs. msg 0x22); the earlier per-vtable msg-0x22
//     hook silently missed every window that centres via a direct SetPos in its
//     MakeWindow case (achievement, bank, mail, rodex) — they never get msg 0x22.
//   SAVE: OnTick reads the live position via FindWindow and persists drags.
// Status/Equip keep their own richer plugins (relayout + their own hooks).
// ===========================================================================

namespace {

// UIWindow base field layout (universal for every window class).
constexpr int kWinX     = 0x1c;  // live x
constexpr int kWinY     = 0x20;  // live y
constexpr int kVfSetPos = 0x10;  // UIWindow::SetPos(x,y) — vtable slot +0x10

constexpr uintptr_t kUIWindowMgr = 0x0131f4e8;  // g_UIWindowMgr
constexpr uintptr_t kFindWindow  = 0x00a47b90;  // UIWindowMgr_FindWindow(mgr,id) -> win|null
constexpr uintptr_t kMakeWindow  = 0x00a39340;  // UIWindowMgr_MakeWindow(mgr,id) -> win (factory)

// MakeWindow is __thiscall(mgr, int windowID) returning the window (id = [ebp+8]).
using MakeWindow_t = void* (__fastcall*)(void*, void*, int);
using SetPos_t     = void  (__fastcall*)(void*, void*, int, int);  // SetPos(this,,x,y)
using FindWindow_t = void* (__thiscall*)(void*, int);              // FindWindow(mgr,id)

// A saved coordinate is valid if it isn't the "unset" sentinel and isn't absurdly
// off-screen. NEGATIVE coords are legal (a window dragged partly off the left/top
// edge, e.g. x=-93) and MUST restore — an earlier `>= 0` guard wrongly refused
// them, so such a window saved its position but never came back to it.
constexpr int kOffscreenFloor = -2000;  // below this = garbage / fully off-screen
inline bool ValidPos(int x, int y) {
  return x != INT_MIN && y != INT_MIN && x > kOffscreenFloor && y > kOffscreenFloor;
}

// One tracked window. `key` is the yaml prefix: "<key>_pos_x" / "<key>_pos_y".
struct TrackedWindow {
  int         id;                    // FindWindow / MakeWindow key (UIWindow id)
  const char* key;                   // yaml key prefix
  int         posX = INT_MIN;        // saved x (INT_MIN = unset)
  int         posY = INT_MIN;        // saved y
  // OnTick runtime state:
  int         trackedX = INT_MIN;    // last position we persisted
  int         trackedY = INT_MIN;
  bool        wasOpen = false;       // window was open on the previous tick
  DWORD       lastSave = 0;          // GetTickCount of the last persist (throttle)
};

// ── THE TABLE ───────────────────────────────────────────────────────────────
// Add an entry here to make a window remember its position. `id` = the UIWindow
// id; `key` = a unique, stable yaml key prefix. Nothing else is required — the
// restore hook keys off the id, no per-window vtable/handler RE needed.
//
// IDs confirmed via Ghidra (20250716 client). Status (0xb) and Equip (0xa) are
// intentionally NOT here: they own a richer plugin (relayout + their own hooks).
TrackedWindow g_windows[] = {
    // {id,     "yaml key"}
    {0x10e, "achievement"},  // UIAchievementWnd (270), ctor 0x00778250, vt 0x01019584
    {0x113, "bank"},         // UIBank_NewWnd    (275), vt 0x01030fd4 — LIVE id (0x169 was wrong)
    {0x106, "mail"},         // UIOpenMailBoxWnd (262), ctor 0x0090cc70, vt 0x01039dac
    {0x107, "rodex"},        // UIRodexInboxWnd  (263), ctor 0x007cd7d0, vt 0x01022170
    {0x16d, "party"},        // UIPartyInfoWnd   (365), vt 0x0101a040 (id field this+0x8c)
    // Confirmed native-persisting (do NOT add): Inventory 8, Storage 0x21, Quest
    // journal 0x141, Skill 0xc, Pet 0x58, Homun 0x71, Merc 0x7d, shops/vending.
    // Still lacking persistence, id UNRESOLVED (class in packed region, needs a live
    // MakeWindow-breakpoint capture): Guild main window (UIGuildTotalInfoWnd). Add its
    // id here once known — the hook no-ops safely on an untracked id.
};

constexpr int kWindowCount = static_cast<int>(sizeof(g_windows) / sizeof(g_windows[0]));

inline void* FindWin(int id) {
  return reinterpret_cast<FindWindow_t>(kFindWindow)(
      reinterpret_cast<void*>(kUIWindowMgr), id);
}

inline void SetWinPos(void* win, int x, int y) {
  reinterpret_cast<SetPos_t>(*reinterpret_cast<uintptr_t*>(
      *reinterpret_cast<uintptr_t*>(win) + kVfSetPos))(win, nullptr, x, y);
}

MakeWindow_t g_orig_makewindow = nullptr;  // trampoline to the real MakeWindow

// Replacement for UIWindowMgr_MakeWindow. Build the window normally, then — for a
// tracked id with a valid saved position — override its position before returning.
// MakeWindow does ALL of a window's open-time positioning internally (SetSize /
// centre SetPos / OnCreate), so overriding here is the last word before the first
// render → no flicker. Untracked windows pass straight through.
void* __fastcall MakeWindowHook(void* mgr, void* edx, int windowID) {
  void* win = g_orig_makewindow(mgr, edx, windowID);
  if (win) {
    for (auto& w : g_windows) {
      if (w.id != windowID) continue;
      if (ValidPos(w.posX, w.posY)) {
        __try {
          SetWinPos(win, w.posX, w.posY);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
      }
      break;
    }
    // Remplacement complet de l'entrepôt : StorageTweaks masque la fenêtre native
    // (win+0x28=0) DÈS ici, avant son 1er rendu -> pas de flicker (le OnTick seul
    // laissait passer la frame de création). No-op si imgui_enabled_ est off (natif).
    if (windowID == 0x21) {
      if (auto* st = Bourgeon::Instance().storage_tweaks())
        st->HideNativeAtCreation(win);
    }
    // Remplacement complet de l'inventaire : InventoryViewer cache la fenetre
    // native (id 8) des la creation -> pas de flicker (comme le storage 0x21).
    if (windowID == 8) {
      if (auto* iv = Bourgeon::Instance().inventory_viewer())
        iv->HideNativeAtCreation(win);
    }
    // Idem pour le cash shop (UICashShopWnd id 0x13e) : redraw ImGui complet.
    if (windowID == 0x13e) {
      if (auto* cs = Bourgeon::Instance().cashshop_tweaks())
        cs->HideNativeAtCreation(win);
    }
    // Shop NPC (achat 0x16, vente 0x17, chooser 0x19) : ShopTweaks les cache dès
    // la création -> l'utilisateur atterrit direct sur la fenêtre ImGui unifiée.
    if (windowID == 0x16 || windowID == 0x17 || windowID == 0x18 ||
        windowID == 0x19) {
      if (auto* sh = Bourgeon::Instance().shop_tweaks())
        sh->HideNativeAtCreation(win);
    }
    // Dialogue NPC (say 0x10 / secondaire 0xe2, menu 0x11, input nombre 0x38 /
    // texte 0x64) : NpcDialogTweaks les cache dès la création -> pas de flicker
    // (le seul OnTick@100ms laissait la fenêtre native visible ~100 ms).
    if (windowID == 0x10 || windowID == 0x11 || windowID == 0x38 ||
        windowID == 0x64 || windowID == 0xe2) {
      if (auto* nd = Bourgeon::Instance().npc_dialog_tweaks())
        nd->HideNativeAtCreation(win, windowID);
    }
    // Comparateur ATK/DEF (UIItemParamChangeDisplayWnd) : id variable, créé par le
    // handler d'achat natif -> détecté par vtable (no-op hors session shop).
    if (auto* sh = Bourgeon::Instance().shop_tweaks())
      sh->HideDetailWindow(win);
  }
  return win;
}

// ── Snap hidden-window filter (native drag-snap fix) ─────────────────────────
// Injected into the native snap calculator FUN_00a32eb0 at 0x00a33005 — the
// point in its per-candidate loop right before it computes the align offset to a
// candidate window (candidate window ptr = ESI, set at 0x00a32fc7). The native
// loop NEVER checks whether the candidate is visible (UIWnd_SetVisible 0x009030c0
// stores the visible flag at window+0x28; the snap ignores it), so a HIDDEN
// window that still has an on-screen rect (the top-left "ghost" HUD pieces)
// magnetises every dragged window onto it. This trampoline skips a candidate
// whose +0x28 visible flag is 0 (jump to the loop's iterator-advance at
// 0x00a3304b); otherwise it re-runs the overwritten `MOV ECX,[EBP-0x88]` and
// falls back into the loop body at 0x00a3300b. Net effect: visible windows still
// snap to each other (the feature players like), hidden windows never do.
//
// Returns non-zero to SKIP this snap candidate (do not magnetise onto it). A
// candidate is skipped when it is NOT a real on-screen window: hidden (the
// UIWnd_SetVisible flag at +0x28 is 0) OR pinned off-screen (live x/y <= -2000,
// e.g. a HUD piece parked at -10000). Live diagnosis (2026-07-04) confirmed the
// top-left "ghost" is exactly such a window — present in the snap set with a
// stale on-screen rect — so both conditions are needed to kill it while leaving
// legitimate window-to-window snapping intact.
int SnapDecideSkip(void* window) {
  auto* b = static_cast<char*>(window);
  const int vis = *reinterpret_cast<int*>(b + 0x28);  // UIWnd_SetVisible flag
  const int x   = *reinterpret_cast<int*>(b + 0x1c);  // live screen x
  const int y   = *reinterpret_cast<int*>(b + 0x20);  // live screen y
  if (vis == 0) return 1;                 // hidden window (the ghost)
  if (x <= -2000 || y <= -2000) return 1; // pinned off-screen (e.g. -10000)
  return 0;                               // real on-screen window -> snap allowed
}

// Trampoline hooked into FUN_00a32eb0 at 0x00a33005 (candidate window = ESI). It
// asks SnapDecideSkip whether to skip this candidate; if so it jumps to the loop's
// iterator-advance (0x00a3304b), otherwise it re-runs the overwritten
// `MOV ECX,[EBP-0x88]` and falls back into the loop body (0x00a3300b).
// EAX/ECX/EDX are caller-saved (the loop reloads them); ESI/EBP survive the cdecl
// call, so SnapDecideSkip(esi) is safe. push/ret = absolute jump, esp-neutral.
__declspec(naked) void SnapHiddenFilter() {
  __asm {
    push esi                 // arg: candidate window
    call SnapDecideSkip
    add  esp, 4
    test eax, eax
    jnz  skip
    mov  ecx, [ebp-88h]      // replicate the overwritten MOV ECX,[EBP-0x88]
    push 0a3300bh            // -> loop body: compute snap to this window
    ret
  skip:
    push 0a3304bh            // -> iterator++ : skip this candidate
    ret
  }
}

}  // namespace

WindowPosTweaks::WindowPosTweaks() {
  using namespace hooking;
  g_orig_makewindow = reinterpret_cast<MakeWindow_t>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kMakeWindow),
          reinterpret_cast<uint8_t*>(&MakeWindowHook)));
  LogInfo("[WinPos] tracking {} window(s); MakeWindow restore hook {}",
          kWindowCount, g_orig_makewindow ? "installed" : "FAILED");

  // ── Disable native window dock-SNAP (kills the invisible "ghost" magnetism) ──
  // The base window-move FUN_00880e00 branches into its dock-SNAP path when the
  // dragged window belongs to a dock group (FUN_007a8bb0 test), via `JNZ 0x00880e9f`
  // at 0x00880e7f. That path magnetises the window to the group's bounding box —
  // which, because the hidden Basic Info + Status + shortcut bar form a top-left
  // dock group, is the invisible "ghost" other windows stuck to. Live-confirmed via
  // x32dbg: the snap query FUN_007a8840 is reached ONLY from this branch (return
  // 0x00880ed2). NOP-ing the JNZ forces the plain FREE-move branch → no snap, no
  // ghost, dragging otherwise unchanged. (Also disables docked children following a
  // dragged parent — fine here; those are hidden/HUD windows.)
  constexpr uintptr_t kSnapJnz = 0x00880e7f;  // JNZ 0x00880e9f  (bytes 75 1e)
  DWORD old;
  auto* p = reinterpret_cast<uint8_t*>(kSnapJnz);
  if (VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) {
    if (p[0] == 0x75 && p[1] == 0x1e) { p[0] = 0x90; p[1] = 0x90; }  // guard exact JNZ
    VirtualProtect(p, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
    LogInfo("[WinPos] window dock-snap {}", (p[0] == 0x90) ? "disabled" : "UNCHANGED");
  }

  // ── Fix the native drag-snap: skip HIDDEN candidate windows ─────────────────
  // The snap the player sees is applied by a shared vtable method at 0x008818a0
  // (found live via SnapDiag, 2026-07-04): it computes a magnetic-align delta via
  // FUN_00a32eb0 (align to screen edges AND to every other window's rect) and adds
  // it to the dragged window's live +0x1c/+0x20. The delta computer never checks
  // visibility, so a HIDDEN HUD window with an on-screen rect (the top-left
  // "ghost") is a snap target → every window "bounced" onto it (Y forced to 134,
  // live-confirmed). We used to NOP the two ADDs in 0x008818a0, but that killed
  // ALL snapping (players want window-to-window snap) and broke a native alt-tab
  // window-clamp loop. Instead we hook FUN_00a32eb0's candidate loop to skip
  // non-visible windows (see SnapHiddenFilter): legitimate snap stays, ghost dies.
  constexpr uintptr_t kSnapLoopHook = 0x00a33005;  // MOV ECX,[EBP-0x88] (8B 8D 78 FF FF FF)
  DWORD old2;
  auto* h = reinterpret_cast<uint8_t*>(kSnapLoopHook);
  if (VirtualProtect(h, 6, PAGE_EXECUTE_READWRITE, &old2)) {
    if (h[0] == 0x8B && h[1] == 0x8D && h[2] == 0x78 &&
        h[3] == 0xFF && h[4] == 0xFF && h[5] == 0xFF) {  // guard exact bytes
      const int32_t rel = static_cast<int32_t>(
          reinterpret_cast<uintptr_t>(&SnapHiddenFilter) - (kSnapLoopHook + 5));
      h[0] = 0xE9;                                    // JMP rel32 -> SnapHiddenFilter
      *reinterpret_cast<int32_t*>(h + 1) = rel;
      h[5] = 0x90;                                    // NOP-pad the 6th byte
    }
    VirtualProtect(h, 6, old2, &old2);
    FlushInstructionCache(GetCurrentProcess(), h, 6);
    LogInfo("[WinPos] snap hidden-window filter {}", (h[0] == 0xE9) ? "installed" : "UNCHANGED");
  }
}

// SAVE-ONLY. Restoration is done entirely by the pre-render MakeWindow hook above
// (no repositioning here — that is what caused the visible flicker). Each tick we
// read the live position and persist genuine moves. On the close->open edge we
// re-baseline WITHOUT saving: the hook has already placed the window at its saved
// spot, so baselining to it means only later drags are recorded (never the restored
// value re-saved, and never a clobber). FindWindow is null while closed.
void WindowPosTweaks::OnTick() {
  bool dirty = false;
  for (auto& w : g_windows) {
    void* win = FindWin(w.id);
    if (!win) {            // closed: force a re-baseline on the next open
      w.wasOpen = false;
      continue;
    }

    const int liveX = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kWinX);
    const int liveY = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kWinY);

    if (!w.wasOpen) {  // just opened — the hook already positioned it; baseline
      w.trackedX = liveX;
      w.trackedY = liveY;
      if (!ValidPos(w.posX, w.posY)) {  // first-ever use: seed the yaml with the spot
        w.posX = liveX;
        w.posY = liveY;
      }
      w.wasOpen = true;
      continue;
    }

    // Persist genuine moves (drag), throttled to 200ms.
    if ((liveX != w.trackedX || liveY != w.trackedY) &&
        GetTickCount() - w.lastSave >= 200) {
      w.posX = liveX;
      w.posY = liveY;
      w.trackedX = liveX;
      w.trackedY = liveY;
      w.lastSave = GetTickCount();
      dirty = true;
    }
  }
  if (dirty) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  }
}

// ── Enumeration API for MoonlightUi Save/LoadSettings ───────────────────────
int         WindowPosTweaks_Count() { return kWindowCount; }
const char* WindowPosTweaks_Key(int i) { return g_windows[i].key; }
int         WindowPosTweaks_X(int i) { return g_windows[i].posX; }
int         WindowPosTweaks_Y(int i) { return g_windows[i].posY; }

void WindowPosTweaks_SetSavedPos(int i, int x, int y) {
  // Just load the saved position. The MakeWindow hook applies it on the next open.
  g_windows[i].posX = x;
  g_windows[i].posY = y;
}
