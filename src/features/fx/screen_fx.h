#pragma once

#include <climits>
#include <cstdint>

#include "features/plugin.h"
#include "d3d9/d3d9_hook.h"  // D3D9PostFx
#include "imgui.h"           // IM_COL32 (défauts de l'overlay FPS)

// Client "Game Settings"-style tweaks the native options window can't do.
//
// Real-time post-processing of the engine's rendered frame via the DX9 hook
// (see d3d9/d3d9_hook.cc): colour grade (brightness/contrast/gamma/saturation/
// temperature), filters (B&W / sepia / negative / colourblind), vignette, film
// grain, chromatic aberration, sharpen, and FXAA. Plus a clean PNG screenshot
// (no overlay) and an FPS/frametime overlay. Config persists through MoonlightUi's
// shared bourgeon_settings.yaml (same pattern as StatusIconBar). DX9 only.
class ScreenFx : public Plugin {
 public:
  ScreenFx();

  const char* name() const override { return "ScreenFx"; }

  // Persisted config access (used by moonlight_ui's settings save/load).
  D3D9PostFx& fx() { return fx_; }
  bool& fps_overlay() { return fps_overlay_; }
  // ── Habillage de l'overlay FPS ────────────────────────────────────────────
  // Les couleurs sont des ImU32 (ABGR empaqueté, l'ordre d'`IM_COL32`) : la
  // table de réglages ne connaît pas `ImVec4`, et un entier se relit à
  // l'identique d'une version à l'autre.
  bool&     fps_graph()    { return fps_graph_; }
  bool&     fps_shadow()   { return fps_shadow_; }
  bool&     fps_show_ping(){ return fps_show_ping_; }
  float&    fps_scale()    { return fps_scale_; }
  uint32_t& fps_bg_col()   { return fps_bg_col_; }
  uint32_t& fps_text_col() { return fps_text_col_; }
  bool& zoom_enabled() { return zoom_enabled_; }
  float& zoom_factor() { return zoom_factor_; }
  float& zoom_speed() { return zoom_speed_; }
  int& tex_filter() { return tex_filter_; }
  int& gopt_x() { return gopt_x_; }
  int& gopt_y() { return gopt_y_; }
  int& esc_x() { return esc_x_; }
  int& esc_y() { return esc_y_; }

  // Pushes the current post-fx config to the d3d9 layer. Safe before the device
  // exists (values are just stored).
  void Apply();

  // Ceiling of the FXAA strength slider, in blend units. The FXAA pass runs on
  // the ENGINE frame — native UI text included — and past ~0.5 that text turns
  // to mush, hence the stock cap. It doubles when the ImGui chatbox is on: the
  // native chatbox and its log-options window are then DESTROYED (see
  // ChatWindow::SuppressNativeChat) and every line is drawn in the Bourgeon
  // overlay, which the post-fx passes never touch. The densest, smallest text of
  // the frame is simply no longer IN the frame, so the ceiling can go all the way
  // to the full blend. What is left of native text — entity nameplates, the
  // native windows still standing — is what keeps this a PLAYER call above 0.5.
  float FxaaMaxStrength() const;

  // Renders the graphics-tweak controls (no window of its own); hosted by
  // moonlight_ui inside its "Graphismes" section.
  void DrawSettings();

  // Always-on FPS / frametime overlay (its own small window).
  void OnRenderUI() override;

  // Maintains the extended camera zoom-out each tick (patches the engine's max
  // view-distance clamp globals). 20250716 only.
  void OnTick() override;

 private:
  D3D9PostFx fx_;
  int        tex_filter_ = 0;       // 0=default, 1=point (crisp), 2=linear (smooth)
  bool       fps_overlay_ = false;
  float      fps_hist_[120] = {};  // frametime ring buffer (ms)
  int        fps_head_ = 0;

  // ── Habillage de l'overlay ────────────────────────────────────────────────
  // Défauts choisis pour rester lisibles SANS rien régler : blanc sur un noir à
  // 45 % — c'est ce que l'overlay affichait en dur — plus l'ombrage, qui ne
  // coûte qu'un second `AddText` et sauve le texte quand le fond est mis à zéro
  // au-dessus d'une carte claire.
  bool     fps_graph_     = true;
  bool     fps_shadow_    = true;
  bool     fps_show_ping_ = true;
  float    fps_scale_     = 1.0f;                       // multiplicateur de police
  uint32_t fps_bg_col_    = IM_COL32(0, 0, 0, 115);     // ≈ l'ancien BgAlpha 0.45
  uint32_t fps_text_col_  = IM_COL32(255, 255, 255, 255);
  // Dessine le texte à la position courante, avec son ombre si elle est active.
  void FpsText(const char* text) const;

  // ── Camera zoom-out extension ────────────────────────────────────────────
  // Camera_ApplyViewDistanceClamp (0x00c82340) clamps the zoom to the engine's
  // max view-distance globals g_cam_zoomMaxOutdoor (0x012291c0) / Indoor
  // (0x012291c4). 🔴 Le moteur les réécrit AUSSI — à chaque bascule de la
  // commande /zoom (0x006918c0), là où vit le patch WARP `ZoomMax`. On ne les
  // touche donc que si l'option est active, et zoom_base_* est RE-CAPTURÉ dès
  // que la valeur lue n'est plus zoom_written_* (= le moteur a écrit).
  bool  zoom_enabled_      = false;
  float zoom_factor_       = 1.0f;   // 1.0 = stock, up to ~2.5 (max view distance)
  float zoom_speed_        = 1.0f;   // wheel step multiplier 1..4 (responsiveness)
  bool  zoom_applied_      = false;  // notre valeur est-elle en place ?
  float zoom_base_out_     = 0.0f;   // dernière valeur vue DU MOTEUR
  float zoom_base_in_      = 0.0f;
  float zoom_written_out_  = 0.0f;   // dernière valeur écrite PAR NOUS
  float zoom_written_in_   = 0.0f;
  // Wheel step constants DAT_01091520 / 01091528 (.rdata, read by the wheel-zoom
  // handler FUN_00c7d4f0); scaled by zoom_speed_ so each notch moves further.
  bool  zoom_step_ok_      = false;
  float zoom_step_base1_   = 0.0f;
  float zoom_step_base2_   = 0.0f;

  // ── Game Settings window (ESC > Game Option, id 0x271e) position persistence ─
  // The engine never saves this window's position. We restore it on open and
  // persist it on move/close via OnTick (FindWindow live read), like StatusTweaks.
  int           gopt_x_ = INT_MIN;   // saved position (INT_MIN = none)
  int           gopt_y_ = INT_MIN;
  bool          gopt_was_open_ = false;
  unsigned long gopt_last_save_ = 0;  // GetTickCount of last save (throttle)

  // ── ESC "Game Options" window (UIEscOptionWnd) position persistence ──────────
  // The ESC pop-up menu (Character Select / game settings / Shortcut Config /
  // Exit to Windows / Return to game). A DIFFERENT native window from the Game
  // Settings one above. It has no FindWindow id in the switch; its live pointer
  // sits in the window manager's dedicated slot g_UIWindowMgr(0x0131f4e8)+0x408
  // (null when the menu is closed). vtable 0x010384a0; standard UIWindow base
  // (pos win+0x1c/0x20, SetPos vtable+0x10 = 0x00874af0, same as Game Settings).
  int           esc_x_ = INT_MIN;
  int           esc_y_ = INT_MIN;
  bool          esc_was_open_ = false;
  unsigned long esc_last_save_ = 0;
};
