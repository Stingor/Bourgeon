#pragma once

#include <cstdint>

#include "features/plugin.h"

// Experimental first-person ("FPS") camera for the in-world view. RO is a real 3D
// engine (3D terrain + billboarded sprites) whose camera is just locked to an
// isometric tilt/distance, so an FPS view is only a matter of re-parametering the
// existing camera each frame:
// il suffit d'écrire dans les CIBLES du rig (+0x44 tilt, +0x4c distance) : le
// moteur lisse le reste. Le rig, ses offsets et la capture de pCam vivent dans
// ragnarok/camera.h — ce module n'ajoute que la hauteur d'yeux, en relevant le
// point de vue dans SetView.
//
// Toggle with F9. 20250716-specific addresses.
//
// v1 limitations (documented, not yet solved):
//  - Outdoor is clean; INDOOR maps flicker (the clamp function re-forces tilt=45
//    every frame indoors, fighting our write).
//  - Your own character sprite is not hidden yet, so its back can block the view
//    (TODO: hook CActorSprite_RenderLayered like WeaponLayer).
class FpsView : public Plugin {
 public:
  FpsView();

  const char* name() const override { return "FpsView"; }

  void OnTick() override;

  // Public toggle (used by the moonlight_ui checkbox and the F9 hotkey).
  bool enabled() const { return enabled_; }
  void SetEnabled(bool on);

  // Live-tunable knobs, exposed for the moonlight_ui sliders.
  float eye_height() const { return fps_height_; }
  float* p_pitch()  { return &fps_tilt_; }
  float* p_dist()   { return &fps_zoom_; }
  float* p_height() { return &fps_height_; }

 private:
  void Apply();
  void Restore();

  bool enabled_ = false;

  // Stock TARGET values captured on enable so Restore() puts them back exactly.
  bool  base_ok_    = false;
  float base_pitch_ = 0.0f;  // pCam+0x44 target latitude at enable time
  float base_dist_  = 0.0f;  // pCam+0x4c target distance at enable time
  // Last snapped values; a sentinel means "snap on the next Apply". Change-detect
  // so pitch/distance are written only on enable / slider change, leaving the
  // camera free for mouse look in between.
  float last_pitch_ = -99999.0f;
  float last_dist_  = -99999.0f;

  // FPS parameters (written into the camera's TARGET fields; the engine smooths).
  float fps_zoom_   = 6.0f;   // target distance from the eye point (small = first person)
  float fps_tilt_   = 0.0f;   // 0 = perfectly horizontal gaze
  float fps_height_ = 15.0f;  // eye height raised above the look-at (feet) point
};
