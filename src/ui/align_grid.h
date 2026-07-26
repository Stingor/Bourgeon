#pragma once

#include <cmath>

// ── Grille d'alignement du HUD ───────────────────────────────────────────────
// Overlay plein écran partagé, pour aligner n'importe quel widget ImGui déplaçable
// (barres EXP/HP/SP, icônes de menu, portrait…). Une instance vit sur MoonlightUi ;
// les consommateurs y accèdent via Bourgeon::Instance().moonlight_ui()->grid_.
//
// Extrait de plugins/moonlight_ui.h : c'est un service d'UI générique, pas une
// partie du panneau de réglages.
struct AlignGrid {
  bool  show = false;
  bool  snap = false;   // snap dragged/resized widgets to grid cells
  int   size = 32;      // cell size in px (4..128)
  float color[4] = {1.0f, 1.0f, 1.0f, 0.15f};

  // Cell size clamped to a sane minimum.
  int cell() const { return size < 4 ? 4 : size; }

  // Snaps one absolute screen coordinate to the nearest visible grid line.
  // The grid is centred on the screen, so `extent` (the screen size along this
  // axis, ds.x or ds.y) is needed to locate the lines — it must match Draw()'s
  // offset or the snap targets and the drawn lines drift apart. Returns v
  // unchanged when snapping is off, so callers can apply it unconditionally.
  // Snap a window EDGE (move: its top-left; resize: its bottom-right corner),
  // never a width/height, or the edge won't land on a line.
  float SnapAxis(float v, float extent) const {
    if (!snap) return v;
    const float g   = static_cast<float>(cell());
    const float off = std::fmod(extent * 0.5f, g);  // same anchor as Draw()
    return off + g * std::floor((v - off) / g + 0.5f);
  }

  // Draws the grid full-screen on the background draw list (behind all windows).
  void Draw() const;
};

// (Le test « une UI plein écran remplace le HUD » vivait ici sous ro::HudReplaced,
//  en TROIS copies avec celles de basic_info.cc et menu_icons.cc. Il est désormais
//  unique : uiwnd::IsHudReplaced(), dans ragnarok/uiwnd.h.)
