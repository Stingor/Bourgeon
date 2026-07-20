#include "utils/frame_profiler.h"

#include <Windows.h>

#include "utils/log_console.h"

// ── Réglages ──────────────────────────────────────────────────────────────────

// Un pic doit dépasser la baseline d'un FACTEUR *et* d'une MARGE absolue. Le
// facteur seul déclencherait en permanence à haut framerate (à 7 ms, 2.5x = 17 ms,
// atteint par le moindre soubresaut) ; la marge seule raterait les pics à bas
// framerate. Les deux conditions ensemble s'adaptent à 141 comme à 58 FPS. Sert à
// alimenter les compteurs "pics" et "ms perdus" du résumé.
static constexpr double kSpikeFactor  = 2.5;
static constexpr double kSpikeMarginMs = 4.0;

// Au-delà, ce n'est pas un freeze de rendu : alt-tab, chargement de map, fenêtre
// minimisée, breakpoint du débogueur. On l'ignore pour ne pas polluer la baseline.
static constexpr double kIgnoreAboveMs = 1000.0;

// Poids de la moyenne mobile exponentielle de la baseline. 0.02 ≈ moyenne sur les
// ~50 dernières frames normales : assez réactif pour suivre un changement de zone,
// assez lent pour ne pas se laisser tirer par une rafale de pics.
static constexpr double kBaselineAlpha = 0.02;

// Résumé périodique : filet de sécurité pour repérer une régression de perf. Une
// ligne toutes les 10 s, silencieuse le reste du temps.
static constexpr double kSummaryEveryMs = 10000.0;

// ── État ──────────────────────────────────────────────────────────────────────

static double  g_qpc_to_ms      = 0.0;   // 0 = pas encore initialisé
static LONGLONG g_last_frame_qpc = 0;

static double  g_baseline_ms    = 0.0;   // 0 = pas encore amorcée
static double  g_clock_ms       = 0.0;   // horloge locale cumulée (ms)

// Fenêtre de résumé
static double  g_win_start_ms   = 0.0;
static int     g_win_frames     = 0;
static double  g_win_sum_ms     = 0.0;
static double  g_win_max_ms     = 0.0;
static int     g_win_spikes     = 0;
static double  g_win_spike_sum  = 0.0;   // temps total perdu en pics

static bool    g_had_focus      = true;

// ── Implémentation ────────────────────────────────────────────────────────────

// Sans focus, le jeu et Windows brident le rendu : toutes les frames deviennent
// longues et seraient comptées comme des pics. On sort du mesurage tant que la
// fenêtre au premier plan n'appartient pas à ce processus, et on ré-amorce la
// baseline au retour (la première frame après un alt-tab n'est pas représentative).
static bool HasFocus() {
  const HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

void FrameProfiler_Tick() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);

  if (g_qpc_to_ms == 0.0) {  // première frame : amorçage
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart == 0) return;
    g_qpc_to_ms = 1000.0 / static_cast<double>(freq.QuadPart);
    g_last_frame_qpc = now.QuadPart;
    return;
  }

  const double dt = static_cast<double>(now.QuadPart - g_last_frame_qpc) * g_qpc_to_ms;
  g_last_frame_qpc = now.QuadPart;

  if (!HasFocus()) {          // en arrière-plan : on ne mesure rien
    g_had_focus = false;
    return;
  }
  if (!g_had_focus) {         // retour au premier plan : ré-amorçage
    g_had_focus = true;
    g_baseline_ms = 0.0;
    return;
  }

  if (dt <= 0.0 || dt > kIgnoreAboveMs) return;

  g_clock_ms += dt;

  if (g_baseline_ms == 0.0) {  // amorce la baseline sur la 1re frame plausible
    g_baseline_ms = dt;
    g_win_start_ms = g_clock_ms;
    return;
  }

  const bool is_spike = (dt > g_baseline_ms * kSpikeFactor) &&
                        (dt > g_baseline_ms + kSpikeMarginMs);

  // La baseline ne se nourrit que des frames normales, sinon une série de pics la
  // ferait dériver vers le haut et le profiler deviendrait aveugle à son propre sujet.
  if (!is_spike)
    g_baseline_ms += (dt - g_baseline_ms) * kBaselineAlpha;

  ++g_win_frames;
  g_win_sum_ms += dt;
  if (dt > g_win_max_ms) g_win_max_ms = dt;

  if (is_spike) {
    ++g_win_spikes;
    g_win_spike_sum += dt - g_baseline_ms;
  }

  if (g_clock_ms - g_win_start_ms >= kSummaryEveryMs) {
    const double secs = (g_clock_ms - g_win_start_ms) / 1000.0;
    const double avg  = (g_win_frames > 0) ? (g_win_sum_ms / g_win_frames) : 0.0;
    const double fps  = (avg > 0.0) ? (1000.0 / avg) : 0.0;
    LogDiag("[FrameProf] {:.0f}s : {} frames, {:.1f} FPS ({:.1f} ms moy), max {:.1f} ms,"
            " {} pics, {:.0f} ms perdus",
            secs, g_win_frames, fps, avg, g_win_max_ms, g_win_spikes, g_win_spike_sum);
    g_win_start_ms  = g_clock_ms;
    g_win_frames    = 0;
    g_win_sum_ms    = 0.0;
    g_win_max_ms    = 0.0;
    g_win_spikes    = 0;
    g_win_spike_sum = 0.0;
  }
}
