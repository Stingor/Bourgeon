#pragma once

#include <climits>   // INT_MIN : sentinelle « position jamais enregistrée »
#include <cstdint>   // uint32_t : le GetTickCount du limiteur

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

// ── Le corps commun des deux « one-off » : Status (0xb) et Equip (0xa) ───────
//
// Ces deux fenêtres ont été traitées AVANT le moteur table-driven ci-dessus,
// chacune dans son propre plugin. Leur persistance a DEUX moitiés, et les deux
// étaient recopiées mot pour mot d'un plugin à l'autre :
//   · le DÉTOUR DU HANDLER de messages (vtable +0x94) — capturer la position
//     vivante quand la croix ferme la fenêtre, puis réimposer la position
//     enregistrée après la restauration de disposition du natif ;
//   · le SUIVI AU TICK — forcer une fois la position lue du yaml, puis
//     enregistrer les déplacements réels.
// Les deux vivent ici désormais : `WindowPos_PersistOnMsg` et
// `WindowPos_TrackLive`. Un seul endroit à corriger au lieu de deux.
//
// 🔜 LA MIGRATION VERS LA TABLE N'EST PAS FAITE, ET C'EST DÉLIBÉRÉ — mais pour
// UNE raison, pas deux. Celle qui tient : la fenêtre d'équipement porte un garde
// sur son drapeau de mode (+0xb4 : la sienne vs celle d'un autre joueur) que le
// moteur, qui ne connaît que l'identifiant de fenêtre, ne sait pas exprimer.
//
// ⚠ L'AUTRE raison, longtemps écrite ici — « le moteur changerait les CLÉS du
// yaml, les joueurs perdraient leur position » — est FAUSSE. Vérifié le
// 2026-09-03 : la table écrit « <clé>_pos_x/y », donc {0xa, "equip"} produirait
// « equip_pos_x/y », très exactement la clé qu'écrit WriteWindowPositions
// aujourd'hui. Personne ne perdrait rien. Ne pas la ressortir pour refuser la
// migration une deuxième fois.
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

// ── Suivi de la position VIVANTE, au tick ────────────────────────────────────
//
// L'état de suivi d'UNE fenêtre. Il existait sous forme de `static` de fonction
// dans chacun des deux OnTick ; un corps partagé ne peut plus les porter (deux
// fenêtres, deux états), donc l'appelant en tient un.
struct WindowPosTracker {
  int      tracked_x = INT_MIN;   // dernière position effectivement enregistrée
  int      tracked_y = INT_MIN;
  uint32_t last_save_ms = 0;      // GetTickCount du dernier enregistrement
  bool     baselined = false;     // une référence a été prise sur la fenêtre vivante
};

// Lit la position vivante de la fenêtre `window_id` et persiste les vrais
// déplacements (limités à un enregistrement par kWindowPosSaveThrottleMs).
//
// `saved_x`/`saved_y` = les deux entiers persistés du plugin (INT_MIN = jamais
// enregistré) ; `restore_pending` = son drapeau one-shot, posé au chargement du
// yaml et CONSOMMÉ ici : tant qu'il est vrai, la position lue est réimposée à la
// fenêtre au lieu d'être lue depuis elle. Sans cela le client, qui rouvre ses
// fenêtres à leur emplacement natif en dur, écraserait la valeur chargée puis la
// réenregistrerait par-dessus — et la position ne survivrait pas au redémarrage.
//
// Sans effet tant que la fenêtre est fermée (FindWindow rend nullptr) : les deux
// entiers gardent alors le dernier emplacement connu, pour la prochaine
// restauration.
void WindowPos_TrackLive(int window_id, WindowPosTracker* tracker,
                         int* saved_x, int* saved_y, bool* restore_pending);
