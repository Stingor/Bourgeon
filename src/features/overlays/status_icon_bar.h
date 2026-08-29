#pragma once

#include <cstdint>
#include <vector>

#include "features/plugin.h"

// Persisted status-icon-bar configuration (plain ints so moonlight_ui can
// serialize it without the internal enums).  Defaults reproduce the stock bar.
struct StatusIconConfig {
  bool enabled    = false;
  int  corner     = 1;    // 0=top-left 1=top-right 2=bottom-left 3=bottom-right
  int  margin_x   = 32;   // px from the anchor corner
  int  margin_y   = 185;
  int  step_dir   = 0;    // within-line: 0=down 1=up 2=right 3=left
  int  wrap_dir   = 3;    // new line:    0=down 1=up 2=right 3=left
  int  per_line   = 17;
  int  icon_pitch = 3;    // EDGE gap between icons in px (0 = touching; stock 35-32)
  int  line_pitch = 13;   // EDGE gap between lines/columns in px (0 = touching; stock 45-32)
  int  sort_mode  = 0;    // 0=none 1=longest-first 2=shortest-first
  bool show_remaining = false;
  bool time_bg = false;   // draw a dark background behind the remaining-time text
  int  icon_alpha = 100;  // icon opacity %, 100 = fully opaque (stock); fades the icons
  int  icon_size  = 32;   // icon size in px (stock 32x32; engine scale = size/2)

  // Remaining-time text styling (mirrors the skill-bar text options).
  int   time_place  = 0;  // 0=below icon 1=above icon 2=inside the icon
  int   time_anchor = 7;  // when inside: 0..8 = TL,T,TR,L,C,R,BL,B,BR (default bottom)
  bool  time_bold   = false;                          // faux-bold (re-draw offset 1px)
  float col_time_text[4]   = {1.0f, 1.0f, 1.0f, 1.0f};  // glyph colour
  float col_time_shadow[4] = {0.0f, 0.0f, 0.0f, 0.9f};  // 8-way halo/shadow colour
};

// Buff/debuff status-icon BAR relayout for the 20250716 client.
//
// The on-screen status icons (top-right cluster of small effect icons) are NOT
// a UIWindow — there is no UIStateIconWnd.  They are individual effect-sprite
// nodes built once per change by FUN_00bd4230(scene), which lays them out
// anchored to the bottom/right of the screen: each icon stacks DOWN 35px, and
// when a column fills it wraps to a new column 45px to the LEFT.
//
// This plugin JMP-hooks FUN_00bd4230 and reimplements the build with a fully
// configurable layout model:
//   pos(i) = corner-origin + (i / perLine)*wrapVec + (i % perLine)*stepVec
// so the bar can be anchored to any corner, stacked in any direction (down/up/
// left/right, vertical- or horizontal-first) with arbitrary spacing and icons
// per line.  Node creation reuses the engine's own helpers, so textures /
// lifetime / tooltips behave exactly as stock.  When disabled the hook simply
// forwards to the original, so the stock layout is byte-for-byte preserved.
//
// A live ImGui panel ("Status Icons") drives the settings and forces a rebuild
// on change for instant preview.
//
// KNOWN LIMITATION (v1): the mouseover tooltip hit-test (FUN_00c93cb0) keeps its
// own copy of the stock right-anchored layout constants, so when the bar is
// moved the icon tooltips will be offset.  Syncing the hit-test is a follow-up.
// ── Ce que le CLIENT sait de MES états ──────────────────────────────────────
//
// 🔴 Sa liste est PLUS COMPLÈTE que tout ce qui passe par le réseau. Le sommeil,
// le silence, le gel n'arrivent pas en EFST : le serveur les transmet dans
// `opt1`/`opt2` (ZC_STATE_CHANGE), et c'est le CLIENT qui les convertit en
// entrées de cette liste — c'est pour cela que sa barre les affiche alors
// qu'aucun paquet d'état ne les nomme.
//
// Les surfaces qui listent MES états doivent donc lire ICI, et non attendre du
// réseau ce qu'il ne porte pas. Pour les AUTRES entités, la question ne se pose
// pas : cette liste n'est que la mienne.
//
// `end_tick` est l'horloge `timeGetTime` d'expiration ; 999999 = permanent, rendu
// tel quel — c'est la sentinelle du client, à l'appelant de la reconnaître.
namespace statusicons {

struct Active {
  uint16_t id = 0;
  uint32_t end_tick = 0;
};

// Sentinelle « pas d'échéance » du client.
inline constexpr uint32_t kInfinite = 999999;

// Remplit `out` avec MES états actifs. Faux si la liste n'est pas lisible
// (hors jeu, ou pointeurs pas encore posés).
bool ReadOwn(std::vector<Active>* out);

}  // namespace statusicons

class StatusIconBar : public Plugin {
 public:
  StatusIconBar();

  const char* name() const override { return "StatusIcons"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Persisted config access (used by moonlight_ui's settings save/load).
  StatusIconConfig& config();
  void MarkDirty();  // request a bar rebuild after an external config change

  // Renders the status-icon settings controls (no window of its own); hosted
  // by moonlight_ui inside its "Icônes de statut" section.
  void DrawSettings();
};
