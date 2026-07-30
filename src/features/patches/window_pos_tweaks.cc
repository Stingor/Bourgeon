#include "features/patches/window_pos_tweaks.h"

#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <climits>
#include <cstdint>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"  // full MoonlightUi type for SaveSettings()
#include "features/windows/storage_window.h"  // hide-native-at-creation (id 0x21)
#include "features/windows/inventory_viewer.h"  // hide-native-at-creation (id 8)
#include "features/windows/bank_window.h"  // hide-native-at-creation (banque id 275)
#include "features/windows/cart_viewer.h"  // hide-native-at-creation (cart id 0x28)
#include "features/windows/make_item_window.h"  // hide-native-at-creation (94 / 79)
#include "features/windows/cashshop_window.h"  // hide-native-at-creation (id 0x13e)
#include "features/windows/npc_shop_window.h"  // hide-native-at-creation (id 0x16/0x17/0x19)
#include "features/windows/vending_window.h"  // hide-native-at-creation (id 0x29/0xAE)
#include "features/windows/weapon_refine_window.h"  // hide-native-at-creation (id 111)
#include "features/windows/trade_window.h"  // hide-native-at-creation (échange, par vtable)
#include "features/windows/rodex_window.h"  // hide-native-at-creation (courrier 0x107/0x109)
#include "features/windows/npc_dialog_window.h"  // hide-native-at-creation (dialogue 0x10/0x11/0x38/0x64/0xe2)
#include "features/windows/character_sheet.h"  // hide-native-at-creation (grimoire 0x25)
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

// MakeWindow is __thiscall(mgr, int windowID) returning the window (id = [ebp+8]).
using MakeWindow_t = void* (__fastcall*)(void*, void*, int);
using SetPos_t     = void  (__fastcall*)(void*, void*, int, int);  // SetPos(this,,x,y)

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
  int         pos_x = INT_MIN;        // saved x (INT_MIN = unset)
  int         pos_y = INT_MIN;        // saved y
  // OnTick runtime state:
  int         tracked_x = INT_MIN;    // last position we persisted
  int         tracked_y = INT_MIN;
  bool        was_open = false;       // window was open on the previous tick
  DWORD       last_save_ms = 0;          // GetTickCount of the last persist (throttle)
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
  return uiwnd::FindWindow(id);
}

inline void SetWinPos(void* win, int x, int y) {
  reinterpret_cast<SetPos_t>(*reinterpret_cast<uintptr_t*>(
      *reinterpret_cast<uintptr_t*>(win) + uiwnd::kVfSetPos))(win, nullptr, x, y);
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
      if (ValidPos(w.pos_x, w.pos_y)) {
        __try {
          SetWinPos(win, w.pos_x, w.pos_y);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
      }
      break;
    }
    // Remplacement complet de l'entrepôt : StorageWindow masque la fenêtre native
    // (win+0x28=0) DÈS ici, avant son 1er rendu -> pas de flicker (le OnTick seul
    // laissait passer la frame de création). No-op si imgui_enabled_ est off (natif).
    if (windowID == 0x21) {
      if (auto* st = Bourgeon::Instance().storage_window())
        st->HideNativeAtCreation(win);
    }
    // Remplacement complet de l'inventaire : InventoryViewer cache la fenetre
    // native (id 8) des la creation -> pas de flicker (comme le storage 0x21).
    if (windowID == 8) {
      if (auto* iv = Bourgeon::Instance().inventory_viewer())
        iv->HideNativeAtCreation(win);
    }
    // Cart (UICartWnd id 0x28) : fenêtre sœur de l'inventaire, même traitement.
    if (windowID == 0x28) {
      if (auto* cv = Bourgeon::Instance().cart_viewer())
        cv->HideNativeAtCreation(win);
    }
    // Banque de zeny (UIBank_NewWnd id 275) : créée par le handler de
    // ZC_BANKING_CHECK, donc ENTRE deux OnTick — sans ce hook une frame native
    // passait à l'écran avant que le viewer ne la masque (cf. le sertissage 0x4A).
    if (windowID == 275) {
      if (auto* bt = Bourgeon::Instance().bank_window())
        bt->HideNativeAtCreation(win);
    }
    // Sertissage de cartes (UIItemCompositionWnd id 0x4A) : ce popup est créé par le
    // handler du paquet ZC 0x017B, donc entre deux OnTick -> sans ce hook une frame
    // native passerait à l'écran avant que le viewer ne la masque.
    if (windowID == 0x4A) {
      if (auto* iv = Bourgeon::Instance().inventory_viewer())
        iv->HideCardInsertAtCreation(win);
    }
    // Grimoire (UINewSkillListWnd id 0x25) : remplacé par l'onglet « Grimoire » de la
    // feuille de personnage. On masque la native dès sa création et on bascule la
    // feuille sur cet onglet — l'icône « Skill » et Alt+S continuent donc de marcher,
    // mais atterrissent sur l'interface moderne (cf. docs/skill_tree_re.md partie II).
    if (windowID == 0x25) {
      if (auto* cs = Bourgeon::Instance().character_sheet())
        cs->HideSkillWndAtCreation(win);
    }
    // Fabrication : les DEUX listes natives (UIMakingArrowListWnd id 94 « LIST »
    // et UIMakeTargetListWnd id 79 « Manufacturing List »). Elles naissent dans un
    // handler de paquet, donc ENTRE deux OnTick — sans ce hook une frame native
    // passerait à l'écran.
    // La 80 (choix des matériaux) est masquée DEPUIS QU'ON NE L'OUVRE PLUS : la
    // commande 130 prend les trois matériaux en paramètre, le plugin les
    // collecte lui-même, et toute apparition de cette fenêtre est désormais un
    // résidu qui venait s'intercaler entre deux fabrications d'une série.
    if (windowID == 94 || windowID == 79 || windowID == 80) {
      if (auto* mk = Bourgeon::Instance().make_item_window())
        mk->HideNativeAtCreation(win);
    }
    // Idem pour le cash shop (UICashShopWnd id 0x13e) : redraw ImGui complet.
    if (windowID == 0x13e) {
      if (auto* cs = Bourgeon::Instance().cashshop_window())
        cs->HideNativeAtCreation(win);
    }
    // Shop NPC (achat 0x16, vente 0x17, chooser 0x19) : NpcShopWindow les cache dès
    // la création -> l'utilisateur atterrit direct sur la fenêtre ImGui unifiée.
    if (windowID == 0x16 || windowID == 0x17 || windowID == 0x18 ||
        windowID == 0x19) {
      if (auto* sh = Bourgeon::Instance().npc_shop_window())
        sh->HideNativeAtCreation(win);
    }
    // Échoppe joueur : la COMPOSITION (vente 0x29, achat 0xAE) ET la grille des
    // objets disponibles (0x2A/0xAF) sont remplacées par VendingWindow, qui les
    // fusionne en une seule fenêtre ImGui.
    // 0x2D / 0xB0 = « My Shop » (UIMerchantItemMyShopWnd), ouverte APRÈS le
    // lancement de l'échoppe — elle a son propre cycle de vie dans le plugin.
    // 0x101 / 0x102 = « Item Sell History » (UIMerchantItemLogWnd), ouverte par la
    // fermeture de la boutique.
    // 0x2B / 0x2C = côté ACHETEUR (UIMerchantItemShopWnd = l'offre du vendeur,
    // UIMerchantItemPurchaseWnd = le panier), ouvertes en cliquant sur l'échoppe
    // d'un autre joueur.
    // 0xB1 / 0xB2 / 0xB3 = les MÊMES classes quand on VEND à un buying store
    // (recherche / vente / stock proposable) — trois fenêtres cette fois.
    if (windowID == 0x29 || windowID == 0x2A || windowID == 0xAE ||
        windowID == 0xAF || windowID == 0x2D || windowID == 0xB0 ||
        windowID == 0x2B || windowID == 0x2C ||
        windowID == 0xB1 || windowID == 0xB2 || windowID == 0xB3 ||
        windowID == 0x101 || windowID == 0x102) {
      if (auto* vt = Bourgeon::Instance().vending_window())
        vt->HideNativeAtCreation(win);
    }
    // Dialogue NPC (say 0x10 / secondaire 0xe2, menu 0x11, input nombre 0x38 /
    // texte 0x64) : NpcDialogWindow les cache dès la création -> pas de flicker
    // (le seul OnTick@100ms laissait la fenêtre native visible ~100 ms).
    if (windowID == 0x10 || windowID == 0x11 || windowID == 0x38 ||
        windowID == 0x64 || windowID == 0xe2) {
      if (auto* nd = Bourgeon::Instance().npc_dialog_window())
        nd->HideNativeAtCreation(win, windowID);
    }
    // Refine d'arme Whitesmith (UIWeaponRefineWnd id 111) : cette fenêtre est
    // créée par le handler de ZC_NOTIFY_WEAPONITEMLIST (0x0221), donc ENTRE deux
    // OnTick -> sans ce hook une frame native passerait à l'écran avant que le
    // plugin ne la masque (même cas que la banque et le sertissage).
    if (windowID == 111) {
      if (auto* wr = Bourgeon::Instance().weapon_refine_window())
        wr->HideNativeAtCreation(win);
    }
    // Comparateur ATK/DEF (UIItemParamChangeDisplayWnd) : id variable, créé par le
    // handler d'achat natif -> détecté par vtable (no-op hors session shop).
    if (auto* sh = Bourgeon::Instance().npc_shop_window())
      sh->HideDetailWindow(win);
    // Courrier RODEX : la LISTE (0x107) est créée sur commande du joueur, mais la
    // LECTURE (0x109) et l'ÉCRITURE (0x108) le sont par des handlers de paquet
    // (ZC 0x0B63 et l'ack « commencer un courrier »), donc entre deux OnTick ->
    // sans ce hook une frame native passerait à l'écran.
    if (windowID == 0x107 || windowID == 0x108 || windowID == 0x109) {
      if (auto* rodex = Bourgeon::Instance().rodex_window())
        rodex->HideNativeAtCreation(win, windowID);
    }
    // Échange joueur-joueur : appel INCONDITIONNEL (la fenêtre est la classe
    // CUIExchangeUI, détectée par VTABLE 0x010457d8 — cf. docs/trade_window_re.md),
    // capture de l'id runtime + masquage du natif. No-op si le viewer est désactivé.
    if (auto* tt = Bourgeon::Instance().trade_window())
      tt->HideNativeAtCreation(win, windowID);
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
          reinterpret_cast<uint8_t*>(uiwnd::kMakeWindowAddr),
          reinterpret_cast<uint8_t*>(&MakeWindowHook)));
  // LogInfo("[WinPos] tracking {} window(s); MakeWindow restore hook {}",
          // kWindowCount, g_orig_makewindow ? "installed" : "FAILED");

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
    // LogInfo("[WinPos] window dock-snap {}", (p[0] == 0x90) ? "disabled" : "UNCHANGED");
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
    // LogInfo("[WinPos] snap hidden-window filter {}", (h[0] == 0xE9) ? "installed" : "UNCHANGED");
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
      w.was_open = false;
      continue;
    }

    const int liveX = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kWinX);
    const int liveY = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kWinY);

    if (!w.was_open) {  // just opened — the hook already positioned it; baseline
      w.tracked_x = liveX;
      w.tracked_y = liveY;
      if (!ValidPos(w.pos_x, w.pos_y)) {  // first-ever use: seed the yaml with the spot
        w.pos_x = liveX;
        w.pos_y = liveY;
      }
      w.was_open = true;
      continue;
    }

    // Persist genuine moves (drag), throttled to 200ms.
    if ((liveX != w.tracked_x || liveY != w.tracked_y) &&
        GetTickCount() - w.last_save_ms >= 200) {
      w.pos_x = liveX;
      w.pos_y = liveY;
      w.tracked_x = liveX;
      w.tracked_y = liveY;
      w.last_save_ms = GetTickCount();
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
int         WindowPosTweaks_X(int i) { return g_windows[i].pos_x; }
int         WindowPosTweaks_Y(int i) { return g_windows[i].pos_y; }

void WindowPosTweaks_SetSavedPos(int i, int x, int y) {
  // Just load the saved position. The MakeWindow hook applies it on the next open.
  g_windows[i].pos_x = x;
  g_windows[i].pos_y = y;
}
