#pragma once

#include "features/plugin.h"

// Generic UIWindow position persistence (20250716 client, base 0x400000).
//
// Many native windows never persist their screen position across a client
// restart: the engine only saves a window's position if its ctor sets the
// save-enable byte [this+0xb4]=1 (see project_window_position_persistence).
// Status (0xb) and Equip (0xa) were fixed one-off in their own plugins; this
// plugin generalises that mechanism for every OTHER movable window (Achievement,
// Bank, Mail, ...) with a single table-driven engine — no per-window RE of the
// message handler is required, only the window id.
//
// Mechanism (table-driven over {id, key}):
//   * RESTORE — a single jmp-hook on UIWindowMgr_MakeWindow overrides the window's
//     position (SetPos, vtable+0x10) with the saved one right after the factory
//     builds it, BEFORE the first render → no flicker. Works whatever way the
//     window positions itself internally (direct SetPos or msg 0x22). Restore
//     happens here, not in OnTick.
//   * SAVE — OnTick reads the live x/y (win+0x1c / win+0x20) via FindWindow and
//     persists drags (throttled ~200ms).
// The saved positions round-trip through MoonlightUi's settings yaml via the
// enumeration accessors below (one <key>_pos_x / <key>_pos_y pair per window).
//
// To make a new window persist: add one {id, "key"} row to the table in
// window_pos_tweaks.cc.
class WindowPosTweaks : public Plugin {
 public:
  WindowPosTweaks();

  const char* name() const override { return "Window Positions"; }

  // Reads each tracked window's live position (throttled) and restores a
  // freshly-loaded position on the first tick the window is open.
  void OnTick() override;
};

// ── Enumeration API for MoonlightUi Save/LoadSettings ───────────────────────
// MoonlightUi loops over these so adding a window needs no yaml edit there.
int         WindowPosTweaks_Count();
const char* WindowPosTweaks_Key(int i);   // yaml key prefix, e.g. "achievement"
int         WindowPosTweaks_X(int i);     // saved x (INT_MIN = unset)
int         WindowPosTweaks_Y(int i);     // saved y (INT_MIN = unset)
void        WindowPosTweaks_SetSavedPos(int i, int x, int y);  // called on load

// ── Persistance par DÉTOUR DU HANDLER : le corps commun des deux « one-off » ──
//
// Status (0xb) et Equip (0xa) ont été traités AVANT le moteur table-driven
// ci-dessus, chacun par un détour de son handler de messages (vtable +0x94).
// Leur corps était le MÊME à 96 % : capturer la position vivante quand la croix
// ferme la fenêtre, laisser le natif faire, demander l'écriture ; puis, APRÈS la
// restauration de disposition du natif, réimposer la position enregistrée.
//
// 🔜 LA MIGRATION N'EST PAS FAITE, ET C'EST DÉLIBÉRÉ. Le moteur ci-dessus les
// remplacerait par une ligne de table chacun, sans détour de handler — mais il
// changerait les CLÉS du yaml (les joueurs perdraient leur position
// enregistrée), et la fenêtre d'équipement porte un garde sur son drapeau de
// mode (+0xb4 : la sienne vs celle d'un autre joueur) que le moteur, qui ne
// connaît que l'identifiant de fenêtre, n'a pas. En attendant cette décision, le
// corps commun vit ici : un seul endroit à corriger au lieu de deux.
//
// `orig` = le trampoline du plugin ; `saved_x`/`saved_y` = ses deux entiers
// persistés (INT_MIN = jamais enregistré) ; `applies` = filtre optionnel, pour
// la fenêtre qui n'agit que sur l'un de ses modes. Renvoie ce que le natif rend.
//
// ⚠ SIX arguments pile / `retn 0x18` : un détour à cinq arguments corrompt la
// pile. La signature ci-dessous est celle des deux handlers, vérifiée sur les
// deux.
using WindowPosMsgFn = int(__fastcall*)(void*, void*, int, int, int, int, int, int);
int WindowPos_PersistOnMsg(void* self, void* edx, int arg0, int msg, int p2,
                           int p3, int p4, int p5, WindowPosMsgFn orig,
                           int* saved_x, int* saved_y,
                           bool (*applies)(void*) = nullptr);
