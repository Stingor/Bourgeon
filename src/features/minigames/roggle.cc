#include "ragnarok/globals.h"
#include "ui/game_texture.h"
#include "ui/mob_sprite.h"
#include "features/minigames/roggle.h"

#include <Windows.h>  // GetTickCount (seed)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "imgui.h"
#include "ui/imgui_escape.h"

#include "d3d9/d3d9_hook.h"  // D3D9_CreateTextureARGB
#include "utils/i18n.h"

// ── Tunables (board-local pixels; physics in px/second) ───────────────────────
namespace {
constexpr float kBoardW = 520.0f;
constexpr float kBoardH = 640.0f;
constexpr float kBallR  = 7.0f;
constexpr float kPegR   = 10.0f;
constexpr float kGravity        = 950.0f;   // px/s^2
constexpr float kLaunchSpeed    = 560.0f;   // px/s
constexpr float kMaxSpeed       = 1700.0f;  // clamp (repositioning can add a hair)
constexpr float kWallRestitution = 0.86f;
constexpr float kPegRestitution  = 0.98f;
constexpr float kCannonX = kBoardW * 0.5f;
constexpr float kCannonY = 34.0f;
constexpr float kBucketW = 82.0f;
constexpr float kBucketH = 16.0f;
constexpr float kBucketY = kBoardH - 22.0f;
constexpr float kBucketSpeed = 155.0f;      // px/s
constexpr int   kStartBalls  = 10;
constexpr float kBallTimeout = 12.0f;       // s — safety against a stuck ball

// State machine.
enum { kAiming = 0, kFlying = 1, kWon = 2, kLost = 3 };

void PlayRoSound(const char* name);  // fwd — plays a RO .wav via the sound mgr

struct Peg { float x, y; bool orange; bool hit; bool removed; };

struct Game {
  bool  inited = false;
  int   state  = kAiming;
  std::vector<Peg> pegs;
  int   balls_left  = 0;
  int   score       = 0;
  int   orange_left = 0;
  // ball
  float bx = 0, by = 0, vx = 0, vy = 0;
  float ball_time = 0.0f;
  bool  bucket_caught = false;
  // bucket
  float bucket_x = kBoardW * 0.5f;
  int   bucket_dir = 1;
  // aim (board-local target = mouse)
  float aim_x = kBoardW * 0.5f, aim_y = kBoardH;
};

Game g;

float Frand() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); }

void NewGame() {
  g.pegs.clear();
  // A hex-offset grid in the middle of the board, with random gaps for variety.
  const float x0 = 44.0f, x1 = kBoardW - 44.0f;
  const float y0 = 148.0f, y1 = kBoardH - 150.0f;
  const float sx = 44.0f, sy = 42.0f;
  int row = 0;
  for (float y = y0; y <= y1; y += sy, ++row) {
    const float off = (row & 1) ? sx * 0.5f : 0.0f;
    for (float x = x0 + off; x <= x1; x += sx) {
      if (Frand() < 0.14f) continue;
      g.pegs.push_back(Peg{x, y, false, false, false});
    }
  }
  // Designate ~1/4 of them orange (the must-clear pegs), picked at random.
  const int n = static_cast<int>(g.pegs.size());
  int oranges = n / 4;
  if (oranges < 1) oranges = 1;
  std::vector<int> idx(n);
  for (int i = 0; i < n; ++i) idx[i] = i;
  for (int i = 0; i < oranges && i < n; ++i) {
    int j = i + static_cast<int>(Frand() * (n - i));
    if (j >= n) j = n - 1;
    std::swap(idx[i], idx[j]);
    g.pegs[idx[i]].orange = true;
  }
  g.orange_left = oranges;
  g.balls_left  = kStartBalls;
  g.score       = 0;
  g.state       = kAiming;
  g.bucket_x    = kBoardW * 0.5f;
  g.bucket_dir  = 1;
  g.inited      = true;
}

void LaunchBall(float tx, float ty) {
  float dx = tx - kCannonX, dy = ty - kCannonY;
  float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1.0f) { dx = 0.0f; dy = 1.0f; len = 1.0f; }
  dx /= len; dy /= len;
  if (dy < 0.15f) {  // always fire into the lower hemisphere
    dy = 0.15f;
    const float nx = std::sqrt(std::max(0.0f, 1.0f - dy * dy));
    dx = (dx < 0.0f) ? -nx : nx;
  }
  g.bx = kCannonX; g.by = kCannonY;
  g.vx = dx * kLaunchSpeed; g.vy = dy * kLaunchSpeed;
  g.ball_time = 0.0f;
  g.bucket_caught = false;
  g.state = kFlying;
}

// Ball left play: bank the lit pegs, refund on a bucket catch, then advance the
// state machine (win when all oranges are cleared, lose when out of balls).
void EndBall() {
  for (auto& p : g.pegs) {
    if (p.hit && !p.removed) {
      p.removed = true;
      if (p.orange && g.orange_left > 0) --g.orange_left;
    }
    p.hit = false;
  }
  if (!g.bucket_caught) --g.balls_left;
  if (g.orange_left <= 0)      g.state = kWon;
  else if (g.balls_left <= 0)  g.state = kLost;
  else                         g.state = kAiming;
}

void StepPhysics(float dt) {
  // The bucket slides back and forth whatever the state.
  g.bucket_x += g.bucket_dir * kBucketSpeed * dt;
  if (g.bucket_x < kBucketW * 0.5f) { g.bucket_x = kBucketW * 0.5f; g.bucket_dir = 1; }
  if (g.bucket_x > kBoardW - kBucketW * 0.5f) {
    g.bucket_x = kBoardW - kBucketW * 0.5f; g.bucket_dir = -1;
  }
  if (g.state != kFlying) return;

  g.ball_time += dt;
  g.vy += kGravity * dt;

  // Clamp speed (energy can creep up a hair from push-out repositioning).
  const float sp2 = g.vx * g.vx + g.vy * g.vy;
  if (sp2 > kMaxSpeed * kMaxSpeed) {
    const float s = kMaxSpeed / std::sqrt(sp2);
    g.vx *= s; g.vy *= s;
  }

  g.bx += g.vx * dt;
  g.by += g.vy * dt;

  // Walls (left/right/top).
  if (g.bx < kBallR)            { g.bx = kBallR;            g.vx = -g.vx * kWallRestitution; }
  if (g.bx > kBoardW - kBallR)  { g.bx = kBoardW - kBallR;  g.vx = -g.vx * kWallRestitution; }
  if (g.by < kBallR)            { g.by = kBallR;            g.vy = -g.vy * kWallRestitution; }

  // Pegs: circle-circle reflection about the contact normal.
  const float rr = kBallR + kPegR;
  for (auto& p : g.pegs) {
    if (p.removed) continue;
    const float dx = g.bx - p.x, dy = g.by - p.y;
    const float d2 = dx * dx + dy * dy;
    if (d2 < rr * rr) {
      float d = std::sqrt(d2);
      if (d < 0.0001f) d = 0.0001f;
      const float nx = dx / d, ny = dy / d;
      g.bx = p.x + nx * rr;  g.by = p.y + ny * rr;  // push out of the peg
      const float vn = g.vx * nx + g.vy * ny;
      g.vx -= (1.0f + kPegRestitution) * vn * nx;
      g.vy -= (1.0f + kPegRestitution) * vn * ny;
      if (!p.hit) {
        p.hit = true;
        g.score += p.orange ? 200 : 100;
        // Real RO hit sounds (present at wav\effect\...; the drop_*.wav the
        // item-drop code references aren't in this client's GRF). Two different
        // hit clips so orange/blue pegs sound distinct.
        PlayRoSound(p.orange ? "effect\\EF_hit2.wav" : "effect\\EF_hit4.wav");
      }
    }
  }

  // Bucket catch near the bottom (chime once on the transition).
  if (g.by > kBucketY - kBucketH && g.by < kBucketY + kBucketH &&
      g.bx > g.bucket_x - kBucketW * 0.5f && g.bx < g.bucket_x + kBucketW * 0.5f) {
    if (!g.bucket_caught) PlayRoSound("_heal_effect.wav");  // reward chime
    g.bucket_caught = true;
  }

  if (g.by > kBoardH + kBallR || g.ball_time > kBallTimeout) EndBall();
}

// ── RO assets: load item icons as ImGui textures via the game's own loader ─────
// Exact pipeline from ChatTweaks' BlitIconAtSEH (chat.cc): BuildPath resolves an
// item id -> "유저인터페이스\item\<resname>.bmp" and the native TexMgr loads it,
// so we never touch the filesystem ourselves (dodges the CP949 folder gotcha).
// The loaded UITexture exposes +0x114 W, +0x118 H, +0x11c BGRA pixels — which is
// exactly D3DFMT_A8R8G8B8 byte order, so we colour-key magenta -> transparent and
// hand the buffer to D3D9_CreateTextureARGB. Item icons: 512=Apple (ball),
// 502=Orange Potion (orange pegs), 505=Blue Potion (blue pegs).
constexpr uintptr_t kEngBuildPath = 0x00d5a720;  // __fastcall(session, 0, idstr, outbuf, 0)
using BuildPath_t = void* (__fastcall*)(void*, void*, const char*, char*, int);
using TexMgr_t    = void* (__cdecl*)();
using MakeKey_t   = void* (__cdecl*)(const char*);
using LoadTex_t   = void* (__fastcall*)(void*, void*, void*);

// "orange"/"blue" are the Peggle terms for target/bonus pegs — we dress them as
// coloured GEMS (Bejeweled-Peggle look): Red Gemstone = must-clear, Blue
// Gemstone = bonus. The ball is a hand-drawn Poring (see DrawPoring), so no ball
// item icon is needed.
constexpr uint32_t kItemOrange = 716;  // Red Gemstone (the must-clear "orange" pegs)
constexpr uint32_t kItemBlue   = 717;  // Blue Gemstone (bonus pegs)

// RO's positional sound player (FUN_00600770) — internally honours the player's
// SFX on/off setting. this = the sound-mgr instance at *0x01253d0c. Play at the
// listener position (0,0,0) => centred, full volume, 2D-ish.
//
// __thiscall emulated as __fastcall (this=ECX, EDX ignored) — same trick chat.cc
// uses for engine __thiscalls. CRUCIAL: the function is `RET 0x20`, i.e. it pops
// 8 stack dwords, so besides name/x/y/z/maxDist/minDist/volume it takes an 8th
// trailing dword (unused, always 0 in the native caller). Passing only 7 args
// under-pops the stack by 4 bytes and crashes the CALLER right after the call —
// which is exactly the "crash on peg hit inside StepPhysics" this caused.
using PlaySoundFn = void(__fastcall*)(void*, void*, const char*, float, float,
                                      float, int, int, float, int);
void PlayRoSound(const char* name) {
  __try {
    void* mgr = *reinterpret_cast<void**>(0x01253d0c);
    if (mgr)
      reinterpret_cast<PlaySoundFn>(0x00600770)(
          mgr, nullptr, name, 0.0f, 0.0f, 0.0f, 250, 40, 1.0f, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

struct IconTex { void* tex = nullptr; int w = 0, h = 0; int attempts = 0; bool gave_up = false; };
IconTex g_tex_orange, g_tex_blue;

// SEH-guarded (no C++ objects in scope — static scratch buffer, like chat.cc).
// Returns true and fills *out on success; false if the item/device isn't ready.
bool LoadItemIcon(uint32_t id, IconTex* out) {
  static uint32_t buf[256 * 256];
  __try {
    char idbuf[16];
    std::snprintf(idbuf, sizeof(idbuf), "%u", id);
    char path[260] = {0};
    reinterpret_cast<BuildPath_t>(kEngBuildPath)(
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr, idbuf, path, 0);
    void* mgr  = reinterpret_cast<TexMgr_t>(ro::texmgr::kGet)();
    void* key  = reinterpret_cast<MakeKey_t>(ro::texmgr::kMakeKey)(path);
    void* texv = reinterpret_cast<LoadTex_t>(ro::texmgr::kLoad)(mgr, nullptr, key);
    if (!texv) return false;
    auto* t = reinterpret_cast<uint8_t*>(texv);
    const int sw = *reinterpret_cast<int*>(t + 0x114);
    const int sh = *reinterpret_cast<int*>(t + 0x118);
    const uint32_t* spx = *reinterpret_cast<uint32_t**>(t + 0x11c);
    if (!spx || sw <= 0 || sh <= 0 || sw > 256 || sh > 256) return false;
    for (int i = 0; i < sw * sh; ++i) {
      const uint32_t s = spx[i];
      // Magenta (0xFF00FF) = the classic RO color-key -> fully transparent;
      // everything else forced opaque (item BMPs carry no real alpha).
      buf[i] = ((s & 0x00FFFFFFu) == 0x00FF00FFu) ? 0u : (s | 0xFF000000u);
    }
    void* tex = D3D9_CreateTextureARGB(buf, sw, sh);
    if (!tex) return false;
    out->tex = tex; out->w = sw; out->h = sh;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Try to load each icon once it's needed; retry a few seconds (device/session
// may not be ready the first frames) then give up and fall back to plain shapes.
void EnsureTextures() {
  // Textures D3DPOOL_DEFAULT : mortes après reset/recréation du device -> on les
  // réinitialise pour forcer un rechargement (sinon draw d'un handle mort = crash).
  static unsigned s_epoch = 0;
  const unsigned e = Overlay_DeviceEpoch();
  if (e != s_epoch) { g_tex_orange = IconTex{}; g_tex_blue = IconTex{}; s_epoch = e; }
  auto attempt = [](IconTex& t, uint32_t id) {
    if (t.tex || t.gave_up) return;
    if (!LoadItemIcon(id, &t) && ++t.attempts >= 240) t.gave_up = true;
  };
  attempt(g_tex_orange, kItemOrange);
  attempt(g_tex_blue, kItemBlue);
}

// A hand-drawn Poring for the ball — pink blob, two eyes with glints, a little
// smile. Pure ImDrawList, donc toujours disponible : c'est le repli quand le
// vrai sprite ne se charge pas (fichier absent, GRF inhabituel).
void DrawPoring(ImDrawList* dl, ImVec2 c, float r) {
  dl->AddCircleFilled(c, r, IM_COL32(255, 160, 200, 255));                 // body
  dl->AddCircleFilled(ImVec2(c.x - r * 0.30f, c.y - r * 0.35f),           // highlight
                      r * 0.28f, IM_COL32(255, 225, 238, 200));
  dl->AddCircle(c, r, IM_COL32(205, 85, 135, 255), 0, 1.6f);              // outline
  const float ex = r * 0.40f, ey = -r * 0.02f, er = r * 0.17f;
  dl->AddCircleFilled(ImVec2(c.x - ex, c.y + ey), er, IM_COL32(45, 30, 45, 255));   // eyes
  dl->AddCircleFilled(ImVec2(c.x + ex, c.y + ey), er, IM_COL32(45, 30, 45, 255));
  dl->AddCircleFilled(ImVec2(c.x - ex + er * 0.3f, c.y + ey - er * 0.3f), er * 0.4f, // glints
                      IM_COL32(255, 255, 255, 235));
  dl->AddCircleFilled(ImVec2(c.x + ex + er * 0.3f, c.y + ey - er * 0.3f), er * 0.4f,
                      IM_COL32(255, 255, 255, 235));
  dl->PathArcTo(ImVec2(c.x, c.y + r * 0.12f), r * 0.34f,                  // smile
                0.18f * 3.14159265f, 0.82f * 3.14159265f, 12);
  dl->PathStroke(IM_COL32(125, 45, 75, 255), 0, 2.0f);
}

// ── REAL Poring monster sprite ───────────────────────────────────────────────
// Le sprite de monstre passe par ui/mob_sprite.h, donc par notre propre parseur
// .spr/.act. Ce qui a disparu ici : l'atlas de sprites et sa page, la cellule
// lue à spr+0x510, le pas de calque 0x24, Act_GetFrame, et le résolveur de nom
// Mob_ClassIdToResName — un switch en dur dont le `default:` renvoyait
// « poring », ce qui masquait ses propres échecs.
//
// 🔴 Le chemin « vrai Poring » n'est plus réservé au DX9. Il l'était parce que
// le handle GPU d'une CTexture native n'est pas au même offset sous DX7 ; nos
// textures sont les nôtres, donc les deux moteurs affichent le même Poring.
constexpr int   kPoringClassId  = 1002;
constexpr float kPoringFrameMs  = 130.0f;  // repli si le .act ne déclare rien

ro::MobSpriteRes g_poring;

}  // namespace

void Roggle::OnModeSwitch(ModeMgr::ModeType, const char*) {
  // The D3D device may have been reset/recreated across the switch — our cached
  // D3DPOOL_DEFAULT icon textures could now be dangling pointers. Drop them
  // (don't Release — the underlying object may already be gone); EnsureTextures
  // rebuilds them on the new device, and drawing falls back to shapes meanwhile.
  g_tex_orange = IconTex{};
  g_tex_blue   = IconTex{};
}

void Roggle::OnRenderUI() {
  if (!enabled_) return;
  if (!g.inited) { std::srand(GetTickCount()); NewGame(); }
  EnsureTextures();       // load the RO item icons (retries until in-world)
  ro::LoadMobSprite(kPoringClassId, &g_poring);  // idempotent, cache derrière

  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(kBoardW + 16.0f, kBoardH + 78.0f),
                           ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Roggle", &open,
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    bourgeon::CloseWindowOnEscape(open);
    // ── HUD ──
    ImGui::Text(i18n::Tr("Score : %d"), g.score);
    ImGui::SameLine(0, 22); ImGui::Text(i18n::Tr("Billes : %d"), g.balls_left);
    ImGui::SameLine(0, 22);
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.12f, 1.0f), "Oranges : %d", g.orange_left);
    ImGui::SameLine(0, 22);
    if (ImGui::SmallButton(i18n::Tr("Nouvelle partie"))) NewGame();

    // ── Canvas: an InvisibleButton gives us a clickable, hoverable region ──
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("board", ImVec2(kBoardW, kBoardH));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) { return ImVec2(origin.x + x, origin.y + y); };

    // ── Physics on a fixed timestep (framerate-independent) ──
    ImGuiIO& io = ImGui::GetIO();
    static float accum = 0.0f;
    accum += io.DeltaTime;
    if (accum > 0.25f) accum = 0.25f;  // clamp after a stall (no spiral of death)
    const float fdt = 1.0f / 240.0f;
    for (int it = 0; accum >= fdt && it < 64; ++it) { StepPhysics(fdt); accum -= fdt; }

    // ── Aim + launch ──
    if (g.state == kAiming && hovered) {
      g.aim_x = io.MousePos.x - origin.x;
      g.aim_y = io.MousePos.y - origin.y;
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) LaunchBall(g.aim_x, g.aim_y);
    }

    // ── Draw ──
    dl->AddRectFilled(P(0, 0), P(kBoardW, kBoardH), IM_COL32(18, 20, 34, 255), 6.0f);
    dl->AddRect(P(0, 0), P(kBoardW, kBoardH), IM_COL32(80, 90, 140, 255), 6.0f);

    // Aim guide: a short predicted arc (gravity + walls only, ignores pegs).
    if (g.state == kAiming) {
      float dx = g.aim_x - kCannonX, dy = g.aim_y - kCannonY;
      float len = std::sqrt(dx * dx + dy * dy);
      if (len < 1.0f) { dx = 0.0f; dy = 1.0f; len = 1.0f; }
      dx /= len; dy /= len;
      if (dy < 0.15f) {
        dy = 0.15f;
        const float nx = std::sqrt(std::max(0.0f, 1.0f - dy * dy));
        dx = (dx < 0.0f) ? -nx : nx;
      }
      float vx = dx * kLaunchSpeed, vy = dy * kLaunchSpeed, px = kCannonX, py = kCannonY;
      for (int i = 0; i < 96; ++i) {
        vy += kGravity * fdt; px += vx * fdt; py += vy * fdt;
        if (px < kBallR || px > kBoardW - kBallR) vx = -vx;
        if (py > kBoardH) break;
        if (i % 3 == 0) dl->AddCircleFilled(P(px, py), 2.0f, IM_COL32(255, 255, 255, 85));
      }
    }

    // Pegs — RO item icons if loaded (orange = Orange Potion, blue = Blue
    // Potion), else fall back to plain coloured circles.
    constexpr float kPegDraw = 13.0f;  // icon half-size (visual only; collision = kPegR)
    for (const auto& p : g.pegs) {
      if (p.removed) continue;
      const IconTex& it = p.orange ? g_tex_orange : g_tex_blue;
      if (it.tex) {
        // Dim unhit pegs slightly; a lit peg pops to full brightness + a ring.
        const ImU32 tint = p.hit ? IM_COL32(255, 255, 255, 255)
                                  : IM_COL32(255, 255, 255, 225);
        dl->AddImage((ImTextureID)(uintptr_t)it.tex,
                     P(p.x - kPegDraw, p.y - kPegDraw), P(p.x + kPegDraw, p.y + kPegDraw),
                     ImVec2(0, 0), ImVec2(1, 1), tint);
        if (p.hit)
          dl->AddCircle(P(p.x, p.y), kPegDraw + 1.0f,
                        p.orange ? IM_COL32(255, 220, 120, 230)
                                 : IM_COL32(190, 225, 255, 230),
                        0, 2.0f);
      } else {
        ImU32 c;
        if (p.orange) c = p.hit ? IM_COL32(255, 232, 150, 255) : IM_COL32(255, 140, 20, 255);
        else          c = p.hit ? IM_COL32(210, 240, 255, 255) : IM_COL32(70, 130, 235, 255);
        dl->AddCircleFilled(P(p.x, p.y), kPegR, c);
        dl->AddCircle(P(p.x, p.y), kPegR, IM_COL32(0, 0, 0, 90), 0, 1.5f);
      }
    }

    // Bucket.
    dl->AddRectFilled(P(g.bucket_x - kBucketW * 0.5f, kBucketY - kBucketH * 0.5f),
                      P(g.bucket_x + kBucketW * 0.5f, kBucketY + kBucketH * 0.5f),
                      IM_COL32(90, 220, 120, 255), 3.0f);
    dl->AddRect(P(g.bucket_x - kBucketW * 0.5f, kBucketY - kBucketH * 0.5f),
                P(g.bucket_x + kBucketW * 0.5f, kBucketY + kBucketH * 0.5f),
                IM_COL32(30, 90, 45, 255), 3.0f, 0, 1.5f);

    // Cannon + aim barrel.
    dl->AddCircleFilled(P(kCannonX, kCannonY), 9.0f, IM_COL32(200, 200, 210, 255));
    if (g.state == kAiming) {
      float dx = g.aim_x - kCannonX, dy = g.aim_y - kCannonY;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > 1.0f) {
        dx /= len; dy /= len;
        dl->AddLine(P(kCannonX, kCannonY), P(kCannonX + dx * 24.0f, kCannonY + dy * 24.0f),
                    IM_COL32(235, 235, 245, 255), 3.0f);
      }
    }

    // Ball = a Poring: the REAL monster sprite if we could load+resolve it,
    // else the hand-drawn one. (Visual size a touch bigger than the collision.)
    if (g.state == kFlying) {
      constexpr float kBallDraw = 13.0f;
      // allow_upscale : la boîte de la balle fait quelques dizaines de pixels,
      // le Poring doit la remplir quelle que soit l'échelle du plateau.
      if (!ro::DrawMobSprite(dl, g_poring,
                             P(g.bx - kBallDraw, g.by - kBallDraw),
                             P(g.bx + kBallDraw, g.by + kBallDraw),
                             static_cast<float>(ImGui::GetTime()), /*action=*/0,
                             kPoringFrameMs, /*allow_upscale=*/true))
        DrawPoring(dl, P(g.bx, g.by), 11.0f);
    }

    // Win/lose overlay (the "Nouvelle partie" HUD button restarts).
    if (g.state == kWon || g.state == kLost) {
      dl->AddRectFilled(P(0, 0), P(kBoardW, kBoardH), IM_COL32(0, 0, 0, 150), 6.0f);
      const char* msg = (g.state == kWon) ? i18n::Tr("GAGNÉ !") : "Perdu...";
      const ImU32 mc  = (g.state == kWon) ? IM_COL32(120, 255, 150, 255)
                                          : IM_COL32(255, 120, 120, 255);
      const ImVec2 ts = ImGui::CalcTextSize(msg);
      dl->AddText(P(kBoardW * 0.5f - ts.x * 0.5f, kBoardH * 0.5f - 30.0f), mc, msg);
      const char* sub = i18n::Tr("Clique \"Nouvelle partie\" en haut pour rejouer.");
      const ImVec2 ss = ImGui::CalcTextSize(sub);
      dl->AddText(P(kBoardW * 0.5f - ss.x * 0.5f, kBoardH * 0.5f - 6.0f),
                  IM_COL32(230, 230, 230, 255), sub);
    }
  }
  ImGui::End();
  if (!open) enabled_ = false;  // window closed = hide (game state persists)
}
