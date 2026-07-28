#pragma once

#include <cstdint>

#include "features/plugin.h"

// Experimental first-person ("FPS") camera for the in-world view. RO is a real 3D
// engine (3D terrain + billboarded sprites) whose camera is just locked to an
// isometric tilt/distance, so an FPS view is only a matter of re-parametering the
// existing camera each frame:
//   pCam = *(CGameMode + 0xd0)   (camera object, vtable g_CCamera_vtable 0x0104dee4)
//   pCam+0x30 / +0x48 : latitude/tilt (default 45deg)  -> force ~0 = look horizontal
//   pCam+0x34 / +0x4c : zoom/distance (from g_cam_zoomWork) -> force ~0 = eye height
// The tilt is per-instance so we capture pCam via a trampoline hook on
// Camera_ApplyViewDistanceClamp (0x00c82340, ECX = CGameMode) and write it in
// OnTick; the zoom is driven through the engine's own view-distance globals (the
// clamp function rewrites pCam+0x34 from them every frame, so writing the field
// directly would fight it). See project_fps_view_re memory for the full RE.
//
// Toggle with F9. 20250716-specific addresses.
//
// v1 limitations (documented, not yet solved):
//  - Outdoor is clean; INDOOR maps flicker (the clamp function re-forces tilt=45
//    every frame indoors, fighting our write).
//  - Your own character sprite is not hidden yet, so its back can block the view
//    (TODO: hook CActorSprite_RenderLayered like WeaponLayerTweaks).
class FpsViewTweaks : public Plugin {
 public:
  FpsViewTweaks();

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
