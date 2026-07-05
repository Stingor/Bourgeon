#pragma once

#include "plugins/plugin.h"

// Client-side tweaks for the "Basic Info" character window (UIBasicInfoWnd) and
// its surrounding HUD.  Renders standalone, freely movable & resizable bars for
// Base EXP, Job EXP, HP and SP as an ImGui overlay (the client already feeds
// mouse input to ImGui, so the windows are draggable/resizable out of the box);
// more Basic-Info customizations will be added here over time.
//
// Each bar is an INDEPENDENT widget with its own position/size/colour/show flag.
// A global "lock" flips the windows to NoMove|NoResize|NoInputs, which freezes
// them AND makes them click-through (locked bars never set io.WantCaptureMouse,
// so clicks fall through to the game).  "Sticky" is a magnetic snap: while you
// drag a bar, its edges snap-align to any other shown bar (pull away to detach).
//
// Values are read live from the session globals the native UIBasicInfoWnd gauges
// use: Base EXP cur/max @0x015fb9d0/0x015fb9d8 and Job EXP cur/max
// @0x015fb9e8/0x015fb9e0 (INT64); HP cur/max @0x015ff908/0x015ff90c and SP
// cur/max @0x015ff910/0x015ff914 (INT32, like the client's own BasicInfo draw).
//
// This plugin owns all the settings state; the settings UI + persistence live
// in MoonlightUi's "EXP Bar" section, which reaches in via Bourgeon::basic_info().
class BasicInfoTweaks : public Plugin {
 public:
  BasicInfoTweaks();  // installs the msg-0x22 hook that hides native Basic Info pre-render

  const char* name() const override { return "Basic Info"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRenderUI() override;
  void OnTick() override;  // enforces the "hide native Basic Info" option

  // Tooltip d'aperçu (perso portant l'item) au survol de « ViewID : N » dans
  // item_desc_tweaks. Réutilise le moteur de capture sprite du portrait. No-op si
  // l'item n'est pas un headgear/garment. À appeler entre Begin/End d'une frame UI.
  void RenderItemPreviewTooltip(int view_id, int emplacement);
  // true si l'item (par son emplacement) est prévisualisable (headgear/garment).
  bool CanPreview(int emplacement) const;

  // ── Bars (read/written by MoonlightUi) ────────────────────────────────────
  struct Bar {
    bool  show;
    int   x, y, w, h;
    float fill[4];  // ImGui RGBA picker state
  };

  enum BarId { kBaseExp = 0, kJobExp, kHp, kSp, kBarCount };

  // Persistence key suffix + UI label, indexed by BarId.
  static constexpr const char* kBarKeys[kBarCount]   = {"base", "job", "hp", "sp"};
  static constexpr const char* kBarLabels[kBarCount] = {"Base", "Job", "HP", "SP"};

  // Global style shared by every bar.
  bool  visible_   = true;   // master toggle for the whole feature
  bool  locked_    = false;  // freeze + click-through (all bars)
  bool  sticky_    = false;  // magnetic edge-snap between bars on drag
  int   text_mode_ = 1;      // 0=none 1=percent 2=values 3=both
  bool  vertical_  = false;  // false = horizontal fill, true = vertical fill
  bool  border_    = true;   // draw the 1px dark outline around each bar
  float rounding_  = 4.0f;   // corner rounding of the drawn bars (0..16)
  float bg_color_[4] = {0.05f, 0.05f, 0.07f, 0.70f};  // shared background + alpha

  // The alignment grid moved to a shared AlignGrid owned by MoonlightUi
  // (Bourgeon::Instance().moonlight_ui()->grid_); bars read it for snapping.

  Bar bars_[kBarCount] = {
    /* Base EXP */ {true, 100, 76, 220, 16, {0.36f, 0.78f, 1.00f, 1.00f}},
    /* Job EXP  */ {true, 100, 94, 220, 16, {1.00f, 0.82f, 0.30f, 1.00f}},
    /* HP       */ {true, 100, 40, 220, 16, {0.85f, 0.27f, 0.27f, 1.00f}},
    /* SP       */ {true, 100, 58, 220, 16, {0.30f, 0.62f, 0.95f, 1.00f}},
  };

  // ── Status portrait: independent, movable HUD elements ────────────────────
  // The head sprite, pseudo, class and level are each a STANDALONE draggable +
  // resizable frame with its own position/size, background colour+opacity and
  // corner rounding, so players can build a custom status UI to their taste.
  // (Continuation of the Basic Info HUD; the head sprite capture lands in a
  // follow-up — placeholder for now. Eventual goal: hide the native Basic Info
  // window once this replaces it.) Persisted by MoonlightUi under "portrait_".
  struct PortraitElem {
    bool  show;
    int   x, y, w, h;
    float bg[4];      // background colour + opacity
    float fg[4];      // text colour (ignored for the head element)
    float rounding;   // background corner rounding (0..16)
  };
  enum PortId { kPortHead = 0, kPortName, kPortClass, kPortLevel, kPortCount };
  static constexpr const char* kPortKeys[kPortCount] = {"head", "name", "class",
                                                        "level"};
  static constexpr const char* kPortLabels[kPortCount] = {"Portrait", "Pseudo",
                                                          "Classe", "Niveau"};

  bool portrait_visible_         = false;  // master toggle (opt-in)
  bool portrait_locked_          = false;  // freeze + click-through (all elems)
  bool portrait_hide_basic_info_ = false;  // hide native UIBasicInfoWnd (id 0)
  bool portrait_border_          = false;  // draw a 1px border around each frame
  bool portrait_head_sprite_     = true;   // render the real head sprite (vs box)
  bool portrait_head_only_       = true;   // keep only head-region layers (no body)
  bool portrait_debug_log_       = false;  // opt-in per-layer capture diagnostics
  float portrait_head_zoom_      = 1.0f;   // zoom into the head (0.10 .. 2.00)
  float portrait_head_offx_      = 0.0f;   // pan X from frame centre (0 = centred)
  float portrait_head_offy_      = 0.0f;   // pan Y from frame centre (0 = centred)
  int  portrait_anim_            = 4;      // animType (0=idle..4=standby..8=dead)
  int  portrait_dir_             = 0;      // facing direction 0..7 (0 = front)
  bool portrait_animate_         = true;   // play the action frames (vs freeze)
  bool portrait_show_garment_    = true;   // feed the equipped garment/cape (full body)

  PortraitElem ports_[kPortCount] = {
    /* head  */ {true,  60,  60, 100, 100, {0.05f, 0.05f, 0.07f, 0.78f},
                 {1.00f, 1.00f, 1.00f, 1.00f}, 4.0f},
    /* name  */ {true,  60, 164, 110,  20, {0.05f, 0.05f, 0.07f, 0.78f},
                 {1.00f, 0.82f, 0.30f, 1.00f}, 4.0f},
    /* class */ {true,  60, 186, 110,  20, {0.05f, 0.05f, 0.07f, 0.78f},
                 {1.00f, 1.00f, 1.00f, 1.00f}, 4.0f},
    /* level */ {true,  60, 208, 110,  20, {0.05f, 0.05f, 0.07f, 0.78f},
                 {0.36f, 0.78f, 1.00f, 1.00f}, 4.0f},
  };

  // Set by MoonlightUi when a size preset is applied so DrawBar force-applies
  // the new geometry for one frame even while unlocked.
  bool force_apply_ = false;

  // Set after the user finishes a drag/resize; MoonlightUi drains it (saves the
  // YAML once) so we never write the file every frame.
  bool geometry_dirty_ = false;

 private:
  bool in_game_      = false;
  bool drag_pending_ = false;  // geometry changed mid-drag, awaiting mouse-up

  // Custom drag/resize state (only one bar is ever dragged at a time).
  enum DragEdge { kEdgeL = 1, kEdgeR = 2, kEdgeT = 4, kEdgeB = 8 };
  int   drag_mode_  = 0;     // 0=none, 1=move, 2=resize
  int   drag_edges_ = 0;     // DragEdge bitmask of the edge(s) grabbed (resize)
  float drag_off_x_ = 0.0f;  // mouse-to-anchor offset captured at drag start
  float drag_off_y_ = 0.0f;
  int   drag_group_mask_ = 0;  // CTRL-move: PortId bitmask dragged as one block

  // Draws bar `id` with the given current/max value. Returns true if its stored
  // geometry changed this frame (user drag/resize).
  bool DrawBar(BarId id, long long cur, long long max);

  // Draws all shown portrait elements (each its own movable/resizable frame).
  // Sets geometry_dirty_ on drag-end so MoonlightUi persists the new layout.
  void DrawPortrait();
  // Draws one portrait element `id`; returns true if its geometry changed.
  bool DrawPortraitElem(PortId id);
  // Connected group of SHOWN portrait elements whose frames touch/overlap `seed`
  // (transitive). Returns a PortId bitmask. Used for CTRL block-move.
  int  PortraitTouchGroup(int seed) const;
  bool portrait_drag_pending_ = false;

  // "Hide native Basic Info" state: while on, OnTick pins the native
  // UIBasicInfoWnd (id 0) off-screen and remembers its real position so it can
  // be restored when the option is turned off.
  bool bi_pinned_off_ = false;
  int  bi_saved_x_ = 0, bi_saved_y_ = 0;

  // Nearest magnetic alignment of value `v` (extent `ext`) on one axis to any
  // other shown bar's edges, within the snap threshold; else returns `v`.
  float SnapValue(float v, float ext, int self_id, bool y_axis) const;
};
