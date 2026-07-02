#pragma once

#include "plugins/plugin.h"

// DOOM (1993) running inside Ragnarok Online, because we can. Embeds the
// vendored doomgeneric port (thirdparty/doomgeneric) compiled as a static C lib:
// we implement its DG_* platform callbacks (video/timing/input) in doom_tweaks.cc
// and pump the engine ourselves — doomgeneric_Create() once on first enable, then
// doomgeneric_Tick() paced at DOOM's native 35 Hz from OnRenderUI (the engine
// NEVER runs its own blocking loop, so the RO render thread is never stalled).
// The 640x400 32-bit framebuffer is uploaded to a dynamic DX9 texture and drawn
// in an ImGui window; keyboard reaches DOOM via ImGui key state while the window
// is focused (the game stops seeing the keys, WantCaptureKeyboard).
//
// Safety: the vendored engine's exit() paths are patched (BOURGEON PATCH markers)
// to hand control back here instead of killing the RO client — I_Error longjmps
// out (state -> Dead), the in-game Quit menu just stops the ticking (state ->
// Quit). The engine is a global-state museum and cannot be restarted in-process:
// one DOOM per client run.
//
// Needs doom1.wad (shareware) next to the client exe. DX9 only.
class DoomTweaks : public Plugin {
 public:
  const char* name() const override { return "DOOM"; }

  void OnRenderUI() override;

  // Menu checkbox toggle. Disabling hides the window and pauses the engine
  // (ticking stops); re-enabling resumes where it was. Re-enabling after a
  // "WAD not found" retries the WAD check (Create was never called there, so
  // it's safe — unlike kDead/kQuit, which stay terminal for the process).
  bool enabled() const { return enabled_; }
  void SetEnabled(bool on);

  // What the settings menu shows next to the checkbox.
  const char* StatusText() const;

  // True while the DOOM window wants exclusive keyboard input (focused, or in
  // the process of being focused by the current click) — the RO WndProc gate
  // (ragnarok_client.cc) swallows keyboard messages for the game while this
  // holds, closing the 1-frame gap left by ImGui's own next-frame capture flag.
  static bool WantsKeyboard();

 private:
  enum class State {
    kIdle,     // not started yet (starts on first enabled frame)
    kNoWad,    // doom1.wad not found — engine NOT started (I_Error would fire)
    kRunning,  // engine alive, ticking while enabled
    kQuit,     // user quit from DOOM's own menu — engine is a zombie, never tick
    kDead,     // I_Error fired (longjmp'd out) — never tick again
  };

  void Start();          // WAD check + doomgeneric_Create (once per process)
  void PumpEngine();     // 35 Hz-paced doomgeneric_Tick + texture upload
  void DrawWindow();     // the ImGui window (framebuffer + input capture)

  bool  enabled_ = false;
  State state_   = State::kIdle;

  void* texture_ = nullptr;  // IDirect3DTexture9* (created lazily, owned here)
};
