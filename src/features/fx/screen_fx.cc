#include "features/fx/screen_fx.h"

#include "ragnarok/uiwnd.h"
#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "imgui.h"

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "ui/ro_imgui.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// ── Native UIWindow flicker-free position restore ────────────────────────────
// For both the ESC "Game Options" window (UIEscOptionWnd, vtable 0x010384a0) and
// the "Game Settings" window (CUIGameSettingsUI, vtable 0x01047d7c). The engine
// centres each on open through its SetPos vtable slot (vtable+0x10 = the shared
// UIWindow_SetPos 0x00874af0), BEFORE the first draw. Restoring from OnTick was one
// frame (~100 ms) late, so the window could be seen at the default centre before
// jumping to the saved spot. We swap each class's SetPos slot so the engine's
// initial open-centring is rewritten to the saved position in place (no jump). The
// ctor/OnCreate never move the window, so the centring is the FIRST SetPos on a
// fresh instance — detected by instance-pointer newness. A second swap on the dtor
// slot (vtable[0]) clears that token the instant the window is destroyed (race-free
// re-arm, even on a fast same-address reopen). User drags pass straight through and
// are persisted by OnTick's polling below.
using SetPosFn = void  (__fastcall*)(void*, void*, int, int);
using DtorFn   = void* (__fastcall*)(void*, void*, char);

ScreenFx* g_owner          = nullptr;  // to read the saved positions
void*           g_orig_setpos    = nullptr;  // shared stock SetPos (0x00874af0)
void*           g_esc_positioned = nullptr;  // armed token: ESC window
void*           g_gs_positioned  = nullptr;  // armed token: Game Settings window
void*           g_esc_orig_dtor  = nullptr;  // stock 0x008db420
void*           g_gs_orig_dtor   = nullptr;  // stock 0x009eb410

// Substitutes the saved (sx,sy) for the engine's open-centring on the FIRST SetPos
// of a fresh instance; otherwise forwards (x,y) unchanged.
void SetPosRestore(void*& token, void* self, int sx, int sy,
                   void* edx, int x, int y) {
  SetPosFn orig = reinterpret_cast<SetPosFn>(g_orig_setpos);
  if (self != token) {
    token = self;
    if (sx != INT_MIN && sx >= 0 && sy >= 0) {
      orig(self, nullptr, sx, sy);
      return;
    }
  }
  orig(self, edx, x, y);
}

void __fastcall Hooked_EscSetPos(void* self, void* edx, int x, int y) {
  SetPosRestore(g_esc_positioned, self,
                g_owner ? g_owner->esc_x() : INT_MIN,
                g_owner ? g_owner->esc_y() : INT_MIN, edx, x, y);
}
void __fastcall Hooked_GsSetPos(void* self, void* edx, int x, int y) {
  SetPosRestore(g_gs_positioned, self,
                g_owner ? g_owner->gopt_x() : INT_MIN,
                g_owner ? g_owner->gopt_y() : INT_MIN, edx, x, y);
}

void* __fastcall Hooked_EscDtor(void* self, void* edx, char flag) {
  if (self == g_esc_positioned) g_esc_positioned = nullptr;
  return reinterpret_cast<DtorFn>(g_esc_orig_dtor)(self, edx, flag);
}
void* __fastcall Hooked_GsDtor(void* self, void* edx, char flag) {
  if (self == g_gs_positioned) g_gs_positioned = nullptr;
  return reinterpret_cast<DtorFn>(g_gs_orig_dtor)(self, edx, flag);
}

// Swaps one vtable slot for `hook` iff it still holds `expected` (defensive), via
// VirtualProtect; saves the original into *save_orig.
void PatchVtableSlot(uintptr_t vtable, int off, void* expected, void* hook, void** save_orig) {
  void** slot = reinterpret_cast<void**>(vtable + off);
  if (*slot != expected) return;
  DWORD old;
  if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
    *save_orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
  }
}

}  // namespace

ScreenFx::ScreenFx() { g_owner = this; }

void ScreenFx::Apply() {
  D3D9_SetPostFx(fx_);
  D3D9_SetTextureFilter(tex_filter_);
}

namespace {

// Writes a float into a read-only (.rdata) location via VirtualProtect; no-op if
// already equal. Used to scale the engine's wheel-zoom step constants.
void PatchFloatRO(float* addr, float value) {
  if (*addr == value) return;
  DWORD old;
  if (VirtualProtect(addr, sizeof(float), PAGE_EXECUTE_READWRITE, &old)) {
    *addr = value;
    VirtualProtect(addr, sizeof(float), old, &old);
  }
}

}  // namespace

void ScreenFx::DrawSettings() {
  bool apply = false;  // push to the renderer this frame (live preview)
  bool save  = false;  // persist to disk (on release, not every drag frame)
  if (ro::RoCheckbox(i18n::Tr("Overlay FPS"), &fps_overlay_)) save = true;
  auto slider = [&](const char* label, float* v, float lo, float hi) {
    ImGui::SetNextItemWidth(160.0f);
    if (WheelSliderFloat(label, v, lo, hi)) apply = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
  };

  if (ro::RoCheckbox(i18n::Tr("Post-processing (effets d'écran)"), &fx_.enabled)) {
    apply = true;
    save  = true;
  }
  ImGui::TextDisabled(i18n::Tr("Affecte le rendu du moteur (monde + UI native), pas l'overlay."));

  if (fx_.enabled) {
    // ── Presets ──
    auto preset = [&](const char* name, D3D9PostFx p) {
      if (ImGui::Button(name)) {
        p.enabled = true;
        fx_ = p;
        apply = true;
        save  = true;
      }
    };
    D3D9PostFx neutral;  // all-default
    preset(i18n::Tr("Neutre"), neutral);
    SameLine();
    { D3D9PostFx p; p.brightness=-0.06f; p.contrast=1.08f; p.saturation=0.85f;
      p.temperature=-0.25f; p.vignette=0.35f; preset(i18n::Tr("Nuit"), p); }
    SameLine();
    { D3D9PostFx p; p.contrast=1.15f; p.saturation=1.12f; p.vignette=0.30f;
      p.sharpen=0.30f; p.aberration=0.20f; p.fxaa=true; preset(i18n::Tr("Cinéma"), p); }
    SameLine();
    { D3D9PostFx p; p.contrast=1.10f; p.saturation=1.20f; p.grain=0.35f;
      p.vignette=0.45f; preset(i18n::Tr("Rétro"), p); }
    SameLine();
    { D3D9PostFx p; p.filter=1; preset(i18n::Tr("N&B"), p); }

    SeparatorText(i18n::Tr("Couleur"));
    slider(i18n::Tr("Luminosité"),  &fx_.brightness, -0.5f, 0.5f);
    slider(i18n::Tr("Contraste"),   &fx_.contrast,    0.5f, 2.0f);
    slider(i18n::Tr("Gamma"),       &fx_.gamma,       0.5f, 2.0f);
    slider(i18n::Tr("Saturation"),  &fx_.saturation,  0.0f, 2.0f);
    slider(i18n::Tr("Température"),  &fx_.temperature,-1.0f, 1.0f);
    // 🔴 Libellés NUS : `ro::RoCombo` traduit ses items lui-même, à la lecture.
    // Les envelopper ici les traduirait DEUX fois — l'anglais rendu par le
    // premier `Tr` repartirait dans le second, qui ne le trouverait pas au
    // catalogue et l'inscrirait comme « à traduire ». Rien ne se voit à l'écran,
    // mais le gabarit d'export se remplit de textes déjà traduits.
    const char* filters[] = {"Aucun", "Noir & blanc", "Sépia", "Négatif", "Daltonien"};
    ImGui::SetNextItemWidth(180.0f);
    if (ro::RoCombo(i18n::Tr("Filtre"), &fx_.filter, filters, IM_ARRAYSIZE(filters))) {
      apply = true;
      save  = true;
    }

    SeparatorText(i18n::Tr("Effets"));
    slider(i18n::Tr("Vignette"),      &fx_.vignette,   0.0f, 1.0f);
    slider(i18n::Tr("Grain"),         &fx_.grain,      0.0f, 1.0f);
    slider(i18n::Tr("Aberration"),    &fx_.aberration, 0.0f, 1.0f);
    slider(i18n::Tr("Netteté"),       &fx_.sharpen,    0.0f, 1.0f);
    if (ro::RoCheckbox(i18n::Tr("FXAA (anti-crénelage)"), &fx_.fxaa)) { apply = true; save = true; }
    if (fx_.fxaa) {
      slider(i18n::Tr("  Force FXAA"), &fx_.fxaa_strength, 0.0f, 0.5f);  // >0.5 wrecks UI text
      SameLine();
      HelpMarker(i18n::Tr("Le FXAA plein écran adoucit aussi le texte de l'UI.\n"
                          "Plafonné à 0.5 — au-delà le texte devient illisible."));
    }

    if (ro::RoButton(i18n::Tr("Réinitialiser"))) {
      fx_ = D3D9PostFx{};
      fx_.enabled = true;
      apply = true;
      save  = true;
    }
  }

  // NB: global texture-filter tweak (tex_filter_ + SetSamplerState hook) is kept
  // in the code but NOT exposed — RO renders sprites ~1:1 so POINT/LINEAR look
  // identical (no visual payoff), same as the camera zoom below.

  // NB: camera zoom-out tweak (zoom_enabled_/factor/speed + OnTick) is kept in the
  // code but intentionally NOT exposed in the UI (disabled by default).

  if (apply) Apply();
  if (save) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}

void ScreenFx::OnRenderUI() {
  if (!fps_overlay_) return;
  const ImGuiIO& io = ImGui::GetIO();
  fps_hist_[fps_head_] = io.DeltaTime * 1000.0f;  // ms
  fps_head_ = (fps_head_ + 1) % IM_ARRAYSIZE(fps_hist_);

  ImGui::SetNextWindowBgAlpha(0.45f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                           ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##fps_overlay", nullptr, flags)) {
    ImGui::Text(i18n::Tr("%.0f FPS  (%.1f ms)"), io.Framerate, 1000.0f / io.Framerate);
    ImGui::PlotLines("##ft", fps_hist_, IM_ARRAYSIZE(fps_hist_), fps_head_, nullptr,
                     0.0f, 33.3f, ImVec2(140, 32));
  }
  ImGui::End();
}

void ScreenFx::OnTick() {
  // Extended camera zoom-out: keep the engine's max view-distance clamp globals
  // (read every camera update by Camera_ApplyViewDistanceClamp @ 0x00c82340)
  // raised to default * zoom_factor_. 20250716-specific addresses.
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;

  // Install the flicker-free position hooks once: swap the SetPos slot (vtable+0x10
  // = stock UIWindow_SetPos 0x00874af0) and the dtor slot (vtable[0]) of both the
  // ESC "Game Options" (UIEscOptionWnd, vtbl 0x010384a0) and "Game Settings"
  // (CUIGameSettingsUI, vtbl 0x01047d7c) window classes. Runs well before the user
  // can open either window (OnTick fires every ~100 ms).
  static bool hooks_installed = false;
  if (!hooks_installed) {
    hooks_installed = true;
    void* setpos = reinterpret_cast<void*>(0x00874af0);
    PatchVtableSlot(0x010384a0, 0x10, setpos, reinterpret_cast<void*>(&Hooked_EscSetPos), &g_orig_setpos);
    PatchVtableSlot(0x010384a0, 0x00, reinterpret_cast<void*>(0x008db420), reinterpret_cast<void*>(&Hooked_EscDtor), &g_esc_orig_dtor);
    PatchVtableSlot(0x01047d7c, 0x10, setpos, reinterpret_cast<void*>(&Hooked_GsSetPos), &g_orig_setpos);
    PatchVtableSlot(0x01047d7c, 0x00, reinterpret_cast<void*>(0x009eb410), reinterpret_cast<void*>(&Hooked_GsDtor), &g_gs_orig_dtor);
  }

  auto* max_out = reinterpret_cast<float*>(0x012291c0);  // g_cam_zoomMaxOutdoor
  auto* max_in  = reinterpret_cast<float*>(0x012291c4);  // g_cam_zoomMaxIndoor
  if (!zoom_base_ok_) {  // capture stock defaults once (after OptionInfo load)
    zoom_base_out_ = *max_out;
    zoom_base_in_  = *max_in;
    zoom_base_ok_  = true;
  }
  if (zoom_enabled_) {
    *max_out = zoom_base_out_ * zoom_factor_;
    *max_in  = zoom_base_in_  * zoom_factor_;
  } else {
    *max_out = zoom_base_out_;
    *max_in  = zoom_base_in_;
  }

  // Wheel zoom step (responsiveness): scale the .rdata step constants read by the
  // wheel-zoom handler FUN_00c7d4f0 so each notch moves further.
  auto* step1 = reinterpret_cast<float*>(0x01091520);
  auto* step2 = reinterpret_cast<float*>(0x01091528);
  if (!zoom_step_ok_) {
    zoom_step_base1_ = *step1;
    zoom_step_base2_ = *step2;
    zoom_step_ok_ = true;
  }
  PatchFloatRO(step1, zoom_enabled_ ? zoom_step_base1_ * zoom_speed_ : zoom_step_base1_);
  PatchFloatRO(step2, zoom_enabled_ ? zoom_step_base2_ * zoom_speed_ : zoom_step_base2_);

  // ── Game Settings window (CUIGameSettingsUI, id 0x271e): persist position ─────
  // Hooked_GsSetPos (the SetPos vtable hook above) makes the restore flicker-free
  // WHEN it fires. But this window is created by a generic registry (DAT_0131ef08),
  // NOT UIWindowMgr, and UIWindow_SetPos is a shared base function — so we cannot
  // rule out the registry centring it via a DIRECT (non-vtable) call that bypasses
  // the hook. We therefore KEEP the one-frame OnTick restore-on-open as a safety
  // net: it restores BEFORE the save branch can run, so a bypassed hook can never
  // let the engine's centre overwrite the saved position. When the hook did fire,
  // the window is already in place and this restore is a harmless no-op. (ESC above
  // is save-only because its centring-through-vtable is proven by disassembly.)
  // Found via FindWindow(0x271e); pos = win+0x1c/0x20; SetPos via vtable slot.
  void* gw = uiwnd::FindWindow(0x271e);
  if (gw) {
    int* px = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(gw) + 0x1c);
    int* py = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(gw) + 0x20);
    if (!gopt_was_open_ && gopt_x_ != INT_MIN && gopt_x_ >= 0 && gopt_y_ >= 0) {
      // Just opened with a saved position -> ensure it (no-op if the hook placed it).
      reinterpret_cast<SetPosFn>(*reinterpret_cast<uintptr_t*>(
          *reinterpret_cast<uintptr_t*>(gw) + 0x10))(gw, nullptr, gopt_x_, gopt_y_);
      gopt_x_ = *px; gopt_y_ = *py;  // sync to the applied (possibly clamped) value
    } else if (*px != gopt_x_ || *py != gopt_y_) {
      gopt_x_ = *px; gopt_y_ = *py;
      const unsigned long now = GetTickCount();
      if (now - gopt_last_save_ >= 200) {  // throttle disk writes during a drag
        gopt_last_save_ = now;
        if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
      }
    }
    gopt_was_open_ = true;
  } else {
    if (gopt_was_open_) {  // just closed -> re-arm restore + flush final position
      g_gs_positioned = nullptr;
      if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
    }
    gopt_was_open_ = false;
  }

  // ── ESC "Game Options" menu (UIEscOptionWnd): persist position on move ────────
  // The ESC pop-up (Character Select / game settings / Shortcut Config / Exit /
  // Return). Restore-on-open is handled flicker-free by Hooked_EscSetPos (the
  // SetPos vtable hook installed above), so here we only persist the user's drags
  // (poll win+0x1c/0x20, throttled) and re-arm the restore hook on close. The live
  // pointer is g_UIWindowMgr+0x408 (null while closed), vtable-guarded against any
  // stale slot value.
  void* ew = *reinterpret_cast<void**>(uiwnd::kUIWindowMgrAddr + 0x408);
  if (ew && *reinterpret_cast<uintptr_t*>(ew) == 0x010384a0) {
    const int ex = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ew) + 0x1c);
    const int ey = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ew) + 0x20);
    if (ex != esc_x_ || ey != esc_y_) {
      esc_x_ = ex; esc_y_ = ey;
      const unsigned long now = GetTickCount();
      if (now - esc_last_save_ >= 200) {  // throttle disk writes during a drag
        esc_last_save_ = now;
        if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
      }
    }
    esc_was_open_ = true;
  } else {
    if (esc_was_open_) {  // just closed -> re-arm restore + flush final position
      g_esc_positioned = nullptr;
      if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
    }
    esc_was_open_ = false;
  }
}
