#include "features/minigames/doom.h"

#include <Windows.h>
#include <mmsystem.h>  // timeGetTime (winmm)

#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "imgui.h"

#include "d3d9/d3d9_hook.h"
#include "ragnarok/ragnarok_client.h"
#include "utils/log_console.h"
// 🔴 AVANT le bloc extern "C" ci-dessous, et surtout pas dedans : i18n.h déclare
// une fonction qui rend un std::vector, ce qu'une liaison C ne peut pas porter
// (C2526). Un include ajouté « après le dernier #include » atterrissait dans le
// bloc, et cassait la compilation de tout le projet.
#include "utils/i18n.h"

// doomgeneric.h declares DG_ScreenBuffer OUTSIDE its own extern "C" guard
// (upstream bug) — from C++ it would link against a mangled symbol and fail.
// Pre-include the CRT headers it pulls (their include guards then make the
// nested includes no-ops), and wrap the whole header in extern "C" so EVERY
// symbol, including DG_ScreenBuffer, gets C linkage.
#include <stdint.h>
#include <stdlib.h>
extern "C" {
#include "doomgeneric.h"
#include "doomkeys.h"
}

extern bool g_imgui_dx7_active;  // DX7 proxy path — this feature is DX9-only

// Engine 35 Hz clock (thirdparty/doomgeneric/i_timer.c) — C linkage in the
// static lib. Safe to call once state_ == kRunning: basetime is latched
// during doomgeneric_Create.
extern "C" int I_GetTime(void);

namespace {
constexpr int    kW     = DOOMGENERIC_RESX;  // 640 (2x pixel-perfect of 320x200)
constexpr int    kH     = DOOMGENERIC_RESY;  // 400
constexpr char   kWad[] = "doom1.wad";       // relative to the client CWD
}  // namespace

// ── Engine-facing state (file scope: referenced by the extern "C" callbacks) ──
static std::jmp_buf g_doom_jmp;                // armed around Create/Tick
static bool         g_doom_jmp_armed = false;
static char         g_doom_fatal[512];         // I_Error message (post-longjmp)
static bool         g_doom_user_quit = false;  // set by the patched I_Quit

// Staging framebuffer: DOOM writes 0x00RRGGBB (alpha byte never set) — the
// overlay renders with alpha blending, so alpha 0 would be invisible. DrawFrame
// copies with alpha forced to 0xFF; OnRenderUI uploads when dirty.
static uint32_t g_frame[kW * kH];
static bool     g_frame_dirty = false;

// Key-event ring buffer feeding DG_GetKey (drained by the engine each tic).
// Sized generously: I_GetEvent consumes at most ONE key-up per tic (engine
// quirk), and our focus-loss flush can queue a burst of releases.
struct DoomKeyEvent {
  unsigned char pressed;
  unsigned char key;
};
static DoomKeyEvent g_key_ring[128];
static unsigned     g_key_head = 0;  // write index
static unsigned     g_key_tail = 0;  // read index

// True while the DOOM window wants exclusive keyboard input this frame — set
// from DrawWindow (see below) and read by Doom::WantsKeyboard(). Backs
// the RO WndProc gate in addition to ImGui's own WantCaptureKeyboard, closing
// the 1-frame gap left by SetNextFrameWantCaptureKeyboard (which only takes
// effect at the NEXT NewFrame — a key pressed right after the focusing click
// would otherwise reach the game too, e.g. Escape opening both menus).
static bool g_doom_wants_keys = false;

// Returns false when the ring is full (caller must NOT advance its held-state
// mirror in that case, or a dropped edge desyncs from the engine forever — see
// PumpDoomKeys/PumpDoomMouse, which retry every frame until this succeeds).
static bool QueueDoomKey(bool pressed, unsigned char key) {
  const unsigned next = (g_key_head + 1) % 128;
  if (next == g_key_tail) return false;  // full — drop, retry next frame
  g_key_ring[g_key_head].pressed = pressed ? 1 : 0;
  g_key_ring[g_key_head].key = key;
  g_key_head = next;
  return true;
}

// ── doomgeneric platform callbacks ────────────────────────────────────────────
extern "C" {

void DG_Init() {}

void DG_DrawFrame() {
  const pixel_t* src = DG_ScreenBuffer;
  if (!src) return;
  for (int i = 0; i < kW * kH; ++i) g_frame[i] = src[i] | 0xff000000u;
  g_frame_dirty = true;
}

void DG_SleepMs(uint32_t ms) { ::Sleep(ms); }

uint32_t DG_GetTicksMs() { return ::timeGetTime(); }

int DG_GetKey(int* pressed, unsigned char* doom_key) {
  if (g_key_tail == g_key_head) return 0;
  *pressed = g_key_ring[g_key_tail].pressed;
  *doom_key = g_key_ring[g_key_tail].key;
  g_key_tail = (g_key_tail + 1) % 128;
  return 1;
}

void DG_SetWindowTitle(const char* title) {
  // LogInfo("[DOOM] engine says hello: {}", title ? title : "?");
}

// Called by the vendored I_Error patch (i_system.c) instead of exit(): stash the
// message and longjmp out of the engine — the guarded Create/Tick wrapper below
// catches it and marks DOOM dead. Frames unwound are plain C (no destructors).
void DG_OnFatalError(const char* msg) {
  strncpy_s(g_doom_fatal, sizeof(g_doom_fatal), msg ? msg : i18n::Tr("(no message)"),
            _TRUNCATE);
  if (g_doom_jmp_armed) std::longjmp(g_doom_jmp, 1);
  // Not armed (impossible: all engine code runs inside the wrappers) — the
  // vendored code falls through to its exit() fallback.
}

// Called by the vendored I_Quit patch when the user confirms Quit in DOOM's own
// menu. The atexit chain already ran (config saved) — the engine is a zombie.
void DG_OnDoomQuit(void) { g_doom_user_quit = true; }

// Game HWND for the DirectSound backend's cooperative level (i_dsound.c).
void* DG_GetGameWindow(void) { return RagnarokClient::GameWindow(); }

}  // extern "C"

// ── Guarded engine entry points (setjmp catches I_Error's longjmp) ────────────
static bool GuardedCreate(int argc, char** argv) {
  g_doom_jmp_armed = true;
  bool ok;
  if (setjmp(g_doom_jmp) == 0) {
    doomgeneric_Create(argc, argv);
    ok = true;
  } else {
    ok = false;  // I_Error fired somewhere inside
  }
  g_doom_jmp_armed = false;
  return ok;
}

static bool GuardedTick() {
  g_doom_jmp_armed = true;
  bool ok;
  if (setjmp(g_doom_jmp) == 0) {
    doomgeneric_Tick();
    ok = true;
  } else {
    ok = false;
  }
  g_doom_jmp_armed = false;
  return ok;
}

// ── Keyboard: ImGui key state → DOOM key events ───────────────────────────────
// The Win32 backend receives every WM_KEYDOWN/WM_KEYUP unconditionally (the
// WndProc hook feeds ImGui before any gating), so ImGui::IsKeyDown gives clean
// per-key edges. WASD is mapped to movement AND still emits its ASCII letter
// (dual event) so cheat codes (iddqd!) and savegame naming keep working.
struct KeyMapEntry {
  ImGuiKey      imgui;
  unsigned char doom;
  bool          held;
};
static std::vector<KeyMapEntry> g_keymap;

static void BuildKeyMap() {
  auto add = [](ImGuiKey k, unsigned char d) { g_keymap.push_back({k, d, false}); };
  add(ImGuiKey_UpArrow, KEY_UPARROW);
  add(ImGuiKey_DownArrow, KEY_DOWNARROW);
  add(ImGuiKey_LeftArrow, KEY_LEFTARROW);
  add(ImGuiKey_RightArrow, KEY_RIGHTARROW);
  add(ImGuiKey_W, KEY_UPARROW);        // WASD movement (QWERTY)...
  add(ImGuiKey_S, KEY_DOWNARROW);
  add(ImGuiKey_A, KEY_STRAFE_L);
  add(ImGuiKey_D, KEY_STRAFE_R);
  add(ImGuiKey_Z, KEY_UPARROW);        // ...and ZQSD (AZERTY): Windows VKs are
  add(ImGuiKey_Q, KEY_STRAFE_L);       // per key LABEL, so aliasing Z/Q makes
                                       // both layouts work with no detection.
                                       // (S/D are shared between the two.)
  add(ImGuiKey_LeftCtrl, KEY_FIRE);
  add(ImGuiKey_RightCtrl, KEY_FIRE);
  add(ImGuiKey_Space, KEY_USE);
  add(ImGuiKey_E, KEY_USE);
  add(ImGuiKey_LeftShift, KEY_RSHIFT);  // run
  add(ImGuiKey_RightShift, KEY_RSHIFT);
  add(ImGuiKey_Escape, KEY_ESCAPE);
  add(ImGuiKey_Enter, KEY_ENTER);
  add(ImGuiKey_KeypadEnter, KEY_ENTER);
  add(ImGuiKey_Tab, KEY_TAB);
  add(ImGuiKey_Backspace, KEY_BACKSPACE);
  add(ImGuiKey_Minus, KEY_MINUS);       // screen size
  add(ImGuiKey_Equal, KEY_EQUALS);
  for (int i = 0; i < 26; ++i)          // ...and every letter as lowercase ASCII
    add(static_cast<ImGuiKey>(ImGuiKey_A + i),
        static_cast<unsigned char>('a' + i));
  for (int i = 0; i < 10; ++i)          // digits (weapons 1-8)
    add(static_cast<ImGuiKey>(ImGuiKey_0 + i),
        static_cast<unsigned char>('0' + i));
  for (int i = 0; i < 9; ++i)           // F1-F9 (help/save/load/...); F10 (quit
    add(static_cast<ImGuiKey>(ImGuiKey_F1 + i),  // prompt) deliberately omitted
        static_cast<unsigned char>(KEY_F1 + i));
  add(ImGuiKey_F11, KEY_F11);           // gamma
}

// Edge-detects against the previous frame; focused=false releases every held
// key (no stuck movement when the window loses focus or the plugin is hidden).
// The mirror only advances when QueueDoomKey actually accepted the event: if
// the ring is momentarily full, the edge is retried every subsequent frame
// until it drains, instead of desyncing from the engine (a dropped release
// would otherwise leave DOOM's key state stuck down forever).
static void PumpDoomKeys(bool focused) {
  if (g_keymap.empty()) BuildKeyMap();
  for (auto& e : g_keymap) {
    const bool down = focused && ImGui::IsKeyDown(e.imgui);
    if (down != e.held && QueueDoomKey(down, e.doom)) e.held = down;
  }
}

// Mouse on the DOOM view: left button = fire, right button = use (doors /
// switches). Queued as the same key events as their keyboard equivalents
// (doomgeneric has no mouse interface — and DOOM treats them identically).
// over_image=false releases both (mouse dragged off / window hidden).
static bool g_mouse_fire_held = false;
static bool g_mouse_use_held  = false;
static void PumpDoomMouse(bool over_image) {
  const bool fire = over_image && ImGui::IsMouseDown(ImGuiMouseButton_Left);
  if (fire != g_mouse_fire_held && QueueDoomKey(fire, KEY_FIRE))
    g_mouse_fire_held = fire;
  const bool use = over_image && ImGui::IsMouseDown(ImGuiMouseButton_Right);
  if (use != g_mouse_use_held && QueueDoomKey(use, KEY_USE))
    g_mouse_use_held = use;
}

// ── Plugin ────────────────────────────────────────────────────────────────────
void Doom::SetEnabled(bool on) {
  // Retry the WAD check when re-enabling after "not found": Create was never
  // called in kNoWad, so this is safe (unlike kDead/kQuit, which stay
  // terminal — the engine cannot restart in-process once it has run).
  if (on && state_ == State::kNoWad) state_ = State::kIdle;
  enabled_ = on;
}

bool Doom::WantsKeyboard() { return g_doom_wants_keys; }

void Doom::Start() {
  if (GetFileAttributesA(kWad) == INVALID_FILE_ATTRIBUTES) {
    state_ = State::kNoWad;
    // LogInfo("[DOOM] {} not found in the client folder — not starting", kWad);
    return;
  }
  // argv is kept BY POINTER by the engine (myargv) — must outlive the process.
  static char  arg0[] = "doom";
  static char  arg1[] = "-iwad";
  static char  arg2[] = "doom1.wad";
  static char  arg3[] = "-nogui";  // I_Error must never pop a modal MessageBox
  static char* argv[] = {arg0, arg1, arg2, arg3};
  // LogInfo("[DOOM] starting engine ({})...", kWad);
  const unsigned long t0 = GetTickCount();
  if (GuardedCreate(4, argv)) {
    state_ = State::kRunning;
    // LogInfo("[DOOM] engine up in {} ms — rip and tear!", GetTickCount() - t0);
  } else {
    state_ = State::kDead;
    LogError("[DOOM] I_Error during startup: {}", g_doom_fatal);
  }
}

void Doom::PumpEngine() {
  if (g_doom_user_quit) {
    state_ = State::kQuit;
    // LogInfo("[DOOM] user quit from the DOOM menu");
    return;
  }
  // Pace doomgeneric_Tick on the ENGINE's own 35 Hz clock (I_GetTime), NOT a
  // timeGetTime mirror: the engine latches `basetime` at its first I_GetTime
  // call inside Create, so a mirror computed from timeGetTime directly is
  // phase-shifted by a random per-run constant in [0, 28.6 ms). On frames
  // where the mirror had ticked but the engine clock had not yet, TryRunTics
  // saw 0 available tics and blocked in its NetUpdate + I_Sleep(1) wait loop
  // (d_loop.c) until the engine boundary — a synchronous stall inside EndScene.
  // Gating on I_GetTime itself guarantees NetUpdate can always build >= 1 tic,
  // so the wait loop is never entered and one call costs just the game logic +
  // the 320x200 software render (< 5 ms). Compare with == (not <=) so the
  // engine's own uint32 wraparound after ~34h of uptime can't wedge the pump.
  static int last_tic = -1;
  const int tic = I_GetTime();
  if (tic == last_tic) return;
  last_tic = tic;

  if (!GuardedTick()) {
    state_ = State::kDead;
    LogError("[DOOM] I_Error: {}", g_doom_fatal);
    return;
  }
  if (g_doom_user_quit) {  // quit confirmed during this very tick
    state_ = State::kQuit;
    // LogInfo("[DOOM] user quit from the DOOM menu");
    return;
  }

  // Texture D3DPOOL_DEFAULT : morte après reset/recréation du device -> on lâche le
  // handle et on force une recréation (sinon Update/Image sur handle mort = crash).
  const unsigned dev_e = Overlay_DeviceEpoch();
  if (texture_ && dev_e != tex_epoch_) { texture_ = nullptr; g_frame_dirty = true; }
  if (g_frame_dirty) {
    if (!texture_) {
      texture_ = D3D9_CreateTextureARGB(g_frame, kW, kH);
      tex_epoch_ = dev_e;
    } else {
      D3D9_UpdateTextureARGB(texture_, g_frame, kW, kH);  // failure: retry next
    }
    if (texture_) g_frame_dirty = false;
  }
}

void Doom::DrawWindow() {
  bool open = true;
  // Default OFF; only the kRunning branch below turns it on. Every other
  // state (or the window being closed) must not hold the game's keyboard
  // hostage.
  g_doom_wants_keys = false;
  const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoCollapse;
  if (ImGui::Begin("DOOM", &open, flags)) {
    switch (state_) {
      case State::kNoWad:
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           i18n::Tr("doom1.wad introuvable !"));
        ImGui::TextUnformatted(
            i18n::Tr("Place le shareware doom1.wad à côté du client (même dossier que\n"
            "l'exe du jeu), puis décoche/recoche la case dans le menu."));
        break;
      case State::kDead:
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           i18n::Tr("DOOM a crashé (I_Error) :"));
        ImGui::TextWrapped("%s", g_doom_fatal);
        break;
      case State::kQuit:
        ImGui::TextUnformatted(
            i18n::Tr("Tu as quitté DOOM depuis son menu. Le moteur ne peut pas\n"
            "redémarrer dans le même processus — relance le client pour rejouer."));
        break;
      case State::kRunning: {
        bool over_image = false;
        // Garde défensif : si le device a changé depuis la création (et que OnTick
        // n'a pas encore recréé), ne dessine PAS un handle mort -> "Chargement...".
        if (texture_ && tex_epoch_ != Overlay_DeviceEpoch()) texture_ = nullptr;
        if (texture_) {
          ImGui::Image((ImTextureID)(uintptr_t)texture_,
                       ImVec2(static_cast<float>(kW), static_cast<float>(kH)));
          over_image = ImGui::IsItemHovered();  // the Image is the last item
        } else {
          ImGui::TextUnformatted(i18n::Tr("Chargement..."));
        }
        const bool focused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        // A click that is ABOUT to focus this window (mouse down while
        // hovering it, focus not granted until ImGui's EndFrame) must also
        // claim the keyboard THIS frame — otherwise a key pressed in the gap
        // between the click and IsWindowFocused()==true next frame reaches
        // the RO client too (e.g. Escape opening both DOOM's and RO's menu).
        const bool focusing_click =
            ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool want_keys = focused || focusing_click;
        g_doom_wants_keys = want_keys;
        if (want_keys) {
          // Also raise ImGui's own flag for next frame (covers the steady
          // state once focused; the WndProc gate below covers this frame).
          ImGui::SetNextFrameWantCaptureKeyboard(true);
          ImGui::TextDisabled(
              i18n::Tr("ZQSD/WASD/flèches bouger - clic/Ctrl tirer - clic droit/Espace "
              "ouvrir - Shift courir - Échap menu"));
        } else {
          ImGui::TextDisabled(
              i18n::Tr("Clique dans la fenêtre pour capturer le clavier."));
        }
        PumpDoomKeys(want_keys);      // also flushes releases on focus loss
        PumpDoomMouse(over_image);    // click = fire, right-click = use
        break;
      }
      case State::kIdle:
        break;
    }
  }
  ImGui::End();
  if (!open) enabled_ = false;  // window closed = pause + hide
}

void Doom::OnRenderUI() {
  if (!enabled_) {
    // Hidden while running: release held keys/buttons once so nothing stays
    // pressed for the (paused) engine when it resumes, and stop hogging the
    // game's keyboard.
    if (state_ == State::kRunning) {
      PumpDoomKeys(false);
      PumpDoomMouse(false);
    }
    g_doom_wants_keys = false;
    return;
  }
  if (g_imgui_dx7_active) {
    if (ImGui::Begin("DOOM", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted(i18n::Tr("DOOM nécessite le rendu DX9 (mode DX7 actif)."));
    }
    ImGui::End();
    g_doom_wants_keys = false;
    return;
  }
  if (state_ == State::kIdle) Start();
  if (state_ == State::kRunning) PumpEngine();
  DrawWindow();
}

const char* Doom::StatusText() const {
  switch (state_) {
    case State::kIdle:    return i18n::Tr("prêt (nécessite doom1.wad à côté du client)");
    case State::kNoWad:   return i18n::Tr("doom1.wad INTROUVABLE dans le dossier du client");
    case State::kRunning: return i18n::Tr("en cours — knee-deep in the dead");
    case State::kQuit:    return i18n::Tr("quitté (relance le client pour rejouer)");
    case State::kDead:    return i18n::Tr("crashé (I_Error) — voir bourgeon.log");
  }
  return "";
}
