#include "ragnarok/render.h"
#include "features/gameplay/fps_view.h"

#include <Windows.h>

#include <cstdint>

#include "bourgeon.h"
#include "ragnarok/camera.h"
#include "utils/log_console.h"

// ── Addresses (20250716 client, no-ASLR: Ghidra addr == live) ────────────────
// 🔴 La caméra elle-même (pCam, ses offsets, et le hook de capture qui l'attrape)
// a déménagé dans ragnarok/camera.h : le détour de capture tient en 5 octets sur
// une seule adresse, et AfkScreen la veut aussi. Ce fichier ne garde que ce qui
// lui est propre — le relevage du point de vue à hauteur d'yeux.
namespace {
// Le builder de caméra (FUN_00a7ae20) finit par appeler SetView(eye,lookat,up)
// sur l'objet renderer [0x012515f8], via le slot vtable +4. eye = pCam+0x80 et
// lookat = pCam+0x20 ; dans les deux, l'indice 1 (la composante Y, +0x84 / +0x24)
// porte la hauteur — c'est celle qui a le clamp anti-sol. Relever les deux
// soulève toute la vue à hauteur d'yeux sans changer la direction du regard.
constexpr int kSetViewVtSlot = 0x04;  // vtable+4 = SetView(eye,lookat,up)
constexpr int kEyeYIndex     = 1;     // indice flottant de l'axe vertical (Y)
}  // namespace

// File scope (NOT namespaced) so the naked __asm can resolve them by name.
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

FpsView::FpsView() {
  g_fps_owner = this;
  // Capture posée INCONDITIONNELLEMENT (comme WeaponLayer) : le timestamp du
  // client n'est pas encore connu au LoadPlugins() — il n'est posé que plus tard
  // dans RagnarokClient::Initialize — si bien qu'un install gaté dessus ne se
  // ferait JAMAIS. C'est très exactement le bug qui avait rendu F9 muet. Les
  // méthodes d'exécution (OnTick/SetEnabled), elles, gardent la garde timestamp.
  ro::camera::Install();
}

void FpsView::SetEnabled(bool on) {
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  if (on == enabled_) return;
  enabled_ = on;
  if (!enabled_) Restore();  // Apply() runs from OnTick once pCam is available
  // LogInfo("[FpsView] {}", enabled_ ? "ON" : "OFF");
}

void FpsView::Apply() {
  if (!ro::camera::Get()) return;  // pas encore en jeu ; on retente au tick suivant

  // Pose CIBLE de repos capturée une fois par activation, pour que Restore() la
  // remette exactement.
  if (!base_ok_) {
    base_pitch_ = ro::camera::Read(ro::camera::kTgtPitch);
    base_dist_  = ro::camera::Read(ro::camera::kTgtDist);
    base_ok_ = true;
  }

  // Snap the pitch/distance targets ONLY when they change (on enable, or when the
  // user drags a slider) — NOT every frame. Continuously forcing them would fight
  // the player's own mouse look/zoom, making the view feel locked. Between snaps
  // the engine leaves the camera free, so you can look around and wheel-zoom. The
  // engine's smoothing lerps the current pose toward the snapped target.
  if (fps_tilt_ != last_pitch_) {
    ro::camera::Write(ro::camera::kTgtPitch, fps_tilt_);  // 0 = horizontal
    last_pitch_ = fps_tilt_;
  }
  if (fps_zoom_ != last_dist_) {
    ro::camera::Write(ro::camera::kTgtDist, fps_zoom_);   // petit = 1ère personne
    last_dist_ = fps_zoom_;
  }
}

void FpsView::Restore() {
  if (!base_ok_) return;
  ro::camera::Write(ro::camera::kTgtPitch, base_pitch_);
  ro::camera::Write(ro::camera::kTgtDist,  base_dist_);
  base_ok_ = false;         // recapture fresh on the next enable
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
