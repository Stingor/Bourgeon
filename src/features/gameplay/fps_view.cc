#include "ragnarok/render.h"
#include "features/gameplay/fps_view.h"

#include <Windows.h>

#include <cstdint>

#include "bourgeon.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// ── Addresses (20250716 client, no-ASLR: Ghidra addr == live) ────────────────
namespace {
constexpr uintptr_t kCameraVtable    = 0x0104dee4;  // g_CCamera_vtable (validates pCam)
constexpr int       kCamOffInMode    = 0xd0;        // CGameMode+0xd0 = pCam
// The camera is a spherical rig with a CURRENT state (+0x2c pitch / +0x30 yaw /
// +0x34 dist) that LERPS toward a TARGET each frame (camera-smoothing
// FUN_00a7ab90). Writing the CURRENT fields is futile — the smoothing drags them
// back to the target. The persistent levers are the TARGET fields, confirmed live:
constexpr int       kCamPitchTarget  = 0x44;        // target latitude/pitch (0 = horizontal)
constexpr int       kCamDistTarget   = 0x4c;        // target distance (small = first person)
// Capture hook site: the per-camera-update clamp (writes the targets from input).
constexpr uintptr_t kCamClamp        = 0x00c82340;  // Camera_ApplyViewDistanceClamp
// The camera builder (FUN_00a7ae20) ends by calling SetView(eye,lookat,up) on the
// renderer object at [0x012515f8], via vtable slot +4. eye = pCam+0x80 and
// lookat = pCam+0x20; in both, index 1 (the Y component, +0x84 / +0x24) is the
// height (the one with the anti-ground clamp). Raising both by fps_height_ lifts
// the whole view to eye level without changing the gaze direction.
constexpr int       kSetViewVtSlot   = 0x04;        // vtable+4 = SetView(eye,lookat,up)
constexpr int       kEyeYIndex       = 1;           // float index of the up (Y) axis
}  // namespace

// File scope (NOT namespaced) so the naked __asm can resolve them by name.
static void* g_fpscam_pcam    = nullptr;  // last-seen camera object (captured live)
static void* g_tramp_camclamp = nullptr;  // -> relocated prologue + original body
static FpsView* g_fps_owner = nullptr;  // to read enabled()/eye_height() in the hook
static void* g_setview_orig       = nullptr;  // stock SetView (vtable+4)

// SetView(eye, lookat, up) — __thiscall emulated as __fastcall (same trick as
// ScreenFx' SetPos hook). When FPS is on, lift the eye and the look-at
// point up the Y axis so the camera sits at head height instead of at the feet.
void __fastcall Hooked_SetView(void* self, void* edx, float* eye, float* lookat,
                               float* up) {
  __try {
    if (g_fps_owner && g_fps_owner->enabled() && eye && lookat) {
      // Up is the -Y direction here: the builder's anti-ground clamp forces
      // eye.Y <= ground, and the live look-at Y is negative. So raising the eye
      // means SUBTRACTING the height (adding it makes the camera dive/plunge).
      const float h = g_fps_owner->eye_height();
      eye[kEyeYIndex]    -= h;
      lookat[kEyeYIndex] -= h;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  reinterpret_cast<void(__fastcall*)(void*, void*, float*, float*, float*)>(
      g_setview_orig)(self, edx, eye, lookat, up);
}

// Capture the camera object from the game mode (ECX at the clamp function's entry).
// __try-guarded: the pointer chain is only valid once in-world.
void __fastcall FpsCamCapture(void* gamemode) {
  __try {
    if (!gamemode) return;
    void* pcam = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(gamemode) + kCamOffInMode);
    if (pcam && *reinterpret_cast<uintptr_t*>(pcam) == kCameraVtable)
      g_fpscam_pcam = pcam;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Naked entry stub: ECX = param_1 (CGameMode) at entry; forward it to the
// __fastcall capture fn (ecx untouched by the pushes), then continue into the
// original via the trampoline. Mirrors WeaponLayer' DeferEntryStub.
__declspec(naked) static void CamClampEntryStub() {
  __asm {
    push eax
    push ecx                   // preserve param_1 across the call
    push edx
    call FpsCamCapture         // __fastcall(ecx = gamemode)
    pop  edx
    pop  ecx
    pop  eax
    jmp  [g_tramp_camclamp]
  }
}

FpsView::FpsView() {
  g_fps_owner = this;
  // Install the capture hook unconditionally (like WeaponLayer): the client
  // timestamp is NOT yet known at LoadPlugins() time — it's set later in
  // RagnarokClient::Initialize — so gating the ctor on it would skip the install
  // forever. Runtime methods (OnTick/SetEnabled) keep the timestamp
  // guard, and this build targets the 20250716 client only.
  using namespace hooking;
  g_tramp_camclamp = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kCamClamp),
      reinterpret_cast<uint8_t*>(&CamClampEntryStub));
  // LogInfo("[FpsView] camera capture hook installed ({})",
          // g_tramp_camclamp != nullptr);
}

void FpsView::SetEnabled(bool on) {
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  if (on == enabled_) return;
  enabled_ = on;
  if (!enabled_) Restore();  // Apply() runs from OnTick once pCam is available
  // LogInfo("[FpsView] {}", enabled_ ? "ON" : "OFF");
}

void FpsView::Apply() {
  if (!g_fpscam_pcam) return;  // not in-world yet; retry next tick
  auto* pcam = reinterpret_cast<char*>(g_fpscam_pcam);

  // Capture stock TARGET values once per enable, so Restore() puts them back.
  if (!base_ok_) {
    base_pitch_ = *reinterpret_cast<float*>(pcam + kCamPitchTarget);
    base_dist_  = *reinterpret_cast<float*>(pcam + kCamDistTarget);
    base_ok_ = true;
  }

  // Snap the pitch/distance targets ONLY when they change (on enable, or when the
  // user drags a slider) — NOT every frame. Continuously forcing them would fight
  // the player's own mouse look/zoom, making the view feel locked. Between snaps
  // the engine leaves the camera free, so you can look around and wheel-zoom. The
  // engine's smoothing lerps the current pose toward the snapped target.
  if (fps_tilt_ != last_pitch_) {
    *reinterpret_cast<float*>(pcam + kCamPitchTarget) = fps_tilt_;  // 0 = horizontal
    last_pitch_ = fps_tilt_;
  }
  if (fps_zoom_ != last_dist_) {
    *reinterpret_cast<float*>(pcam + kCamDistTarget) = fps_zoom_;   // small = eye height
    last_dist_ = fps_zoom_;
  }
}

void FpsView::Restore() {
  if (!base_ok_) return;
  if (g_fpscam_pcam) {
    auto* pcam = reinterpret_cast<char*>(g_fpscam_pcam);
    *reinterpret_cast<float*>(pcam + kCamPitchTarget) = base_pitch_;
    *reinterpret_cast<float*>(pcam + kCamDistTarget)  = base_dist_;
  }
  base_ok_ = false;      // recapture fresh on the next enable
  last_pitch_ = -99999.0f;  // force a re-snap when re-enabled
  last_dist_  = -99999.0f;
}

void FpsView::OnTick() {
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;

  // Install the SetView vtable hook once, the moment the renderer object exists.
  // Swap slot +4 of the object at [0x012515f8] for Hooked_SetView (VirtualProtect
  // guarded), saving the original. Runtime-resolved because the object is a heap
  // singleton, not a fixed .data address.
  if (!g_setview_orig) {
    auto* obj = *reinterpret_cast<void***>(render::kContextPtr);  // [0x012515f8] -> object
    if (obj) {
      void** vtable = *reinterpret_cast<void***>(obj);        // *object = vtable
      if (vtable) {
        void** slot = vtable + (kSetViewVtSlot / sizeof(void*));
        DWORD old;
        if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
          g_setview_orig = *slot;
          *slot = reinterpret_cast<void*>(&Hooked_SetView);
          VirtualProtect(slot, sizeof(void*), old, &old);
        }
      }
    }
  }

  if (enabled_) Apply();
}
