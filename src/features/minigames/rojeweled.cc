#include "ui/game_texture.h"
#include "features/minigames/rojeweled.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "imgui.h"
#include "ui/imgui_escape.h"

#include "d3d9/d3d9_hook.h"  // D3D9_AdditiveBlendCallback

extern bool g_imgui_dx7_active;  // sprite path is DX9 only (coloured-tile fallback under DX7)

// ── Board / tunables ──────────────────────────────────────────────────────────
namespace {
constexpr int   kN        = 8;      // 8x8 board
constexpr float kCell     = 44.0f;  // px per cell
constexpr float kPad      = 6.0f;    // board inset
constexpr int   kTypes    = 6;      // number of monster gem types
constexpr int   kBaseScore = 50;    // per gem cleared

// ── Monster gem types (Poring family — distinct colours) ──────────────────────
struct Mon {
  const char* resname;  // 몬스터\<resname>.spr/.act
  ImU32       fallback; // coloured-tile colour if the sprite can't load
  void*       spr;      // CSprite* (cells @+0x510, palette @+0x110)
  void*       act;      // CAction*
  int         fails;
  bool        gaveup;
  // resolved this frame:
  void*  tex;
  ImVec2 uv0, uv1;
  bool   ok;
};
// Colours span the full spectrum (pink/orange/yellow/green/blue/purple) so the
// per-cell pad makes every type unmistakable — several of these monsters share a
// near-identical pale Poring sprite (Poring vs Angeling especially), so the PAD
// colour, not the face, is the gem identity.
Mon g_mon[kTypes] = {
  {"poring",   IM_COL32(255,  95, 165, 255), nullptr, nullptr, 0, false, nullptr, {}, {}, false},  // pink
  {"drops",    IM_COL32(255, 140,  20, 255), nullptr, nullptr, 0, false, nullptr, {}, {}, false},  // orange
  {"metaling", IM_COL32(255, 215,  55, 255), nullptr, nullptr, 0, false, nullptr, {}, {}, false},  // gold (metal sprite)
  {"poporing", IM_COL32( 70, 205,  85, 255), nullptr, nullptr, 0, false, nullptr, {}, {}, false},  // green
  {"marin",    IM_COL32( 55, 150, 240, 255), nullptr, nullptr, 0, false, nullptr, {}, {}, false},  // blue
  {"deviling", IM_COL32(180,  85, 225, 255), nullptr, nullptr, 0, false, nullptr, {}, {}, false},  // purple
};

// ── Engine glue (20250716) — same monster-sprite pipeline as Roggle ──────
constexpr uintptr_t kFmtSpr         = 0x0103181c;  // "몬스터\\%s.spr" (CP949)
constexpr uintptr_t kFmtAct         = 0x0103182c;  // "몬스터\\%s.act" (CP949)
constexpr uintptr_t kActGetFrame    = 0x0070f4b0;  // __thiscall(act, action, frameIdx) -> frame*
constexpr uintptr_t kActFrameCount  = 0x0070f6b0;  // __thiscall(act, action) -> int (#frames)
constexpr uintptr_t kAtlasGetCached = 0x00566b70;  // __thiscall(atlas, cell, pal, geom) -> ctex|0
constexpr uintptr_t kAtlasBuild     = 0x005663d0;  // __thiscall(atlas, cell, pal, geom) -> ctex
constexpr uintptr_t kSceneCtxPtr    = 0x012515f8;  // *ptr + 0xc0 = sprite atlas
constexpr int       kCTexDX9Handle  = 0x12c;

using TexMgrGetFn   = void* (__cdecl*)();
using MakeKeyFn     = void* (__cdecl*)(const char*);
using LoadResFn     = void* (__fastcall*)(void*, void*, void*);
using ActGetFrameFn = void* (__fastcall*)(void*, void*, unsigned, unsigned);
using ActFrameCntFn = int   (__fastcall*)(void*, void*, unsigned);
using AtlasFn       = void* (__fastcall*)(void*, void*, void*, int, int*);

// Time-driven frame index for the idle action (0), so monsters animate (Porings
// bob). Each type cycles independently over ~130 ms/frame.
unsigned IdleFrame(void* act) {
  int n = reinterpret_cast<ActFrameCntFn>(kActFrameCount)(act, nullptr, 0);
  if (n < 1) n = 1;
  if (n == 1) return 0;
  const DWORD cyc = static_cast<DWORD>(n) * 130u;
  return static_cast<unsigned>((GetTickCount() % cyc) * static_cast<DWORD>(n) / cyc);
}

// RO's positional sound player (FUN_00600770, __thiscall emulated as __fastcall;
// RET 0x20 => needs the trailing 8th arg — same signature/gotcha as Roggle).
// Honours the player's SFX setting internally. Play centred (0,0,0).
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

void LoadMon(Mon& m) {
  if (m.act || m.gaveup) return;
  bool ok = false;
  __try {
    char sp[160], ap[160];
    std::snprintf(sp, sizeof(sp), reinterpret_cast<const char*>(kFmtSpr), m.resname);
    std::snprintf(ap, sizeof(ap), reinterpret_cast<const char*>(kFmtAct), m.resname);
    void* mgr = reinterpret_cast<TexMgrGetFn>(ro::texmgr::kGet)();
    m.spr = reinterpret_cast<LoadResFn>(ro::texmgr::kLoad)(
        mgr, nullptr, reinterpret_cast<MakeKeyFn>(ro::texmgr::kMakeKey)(sp));
    m.act = reinterpret_cast<LoadResFn>(ro::texmgr::kLoad)(
        mgr, nullptr, reinterpret_cast<MakeKeyFn>(ro::texmgr::kMakeKey)(ap));
    ok = (m.spr && m.act);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    m.spr = m.act = nullptr;
  }
  if (!ok && ++m.fails >= 300) m.gaveup = true;
}

// Resolve a monster's idle body cell to a native texture + atlas UVs. Same logic
// as Roggle::ResolvePoringQuad (largest-area layer = body).
bool ResolveMonQuad(Mon& m, void** out_tex, ImVec2* uv0, ImVec2* uv1) {
  if (!m.spr || !m.act) return false;
  bool ok = false;
  __try {
    void* frame = reinterpret_cast<ActGetFrameFn>(kActGetFrame)(
        m.act, nullptr, 0, IdleFrame(m.act));
    if (!frame) return false;
    char* fr = reinterpret_cast<char*>(frame);
    char* lbegin = *reinterpret_cast<char**>(fr + 0x20);
    char* lend   = *reinterpret_cast<char**>(fr + 0x24);
    const int nlayers = static_cast<int>((lend - lbegin) / 0x24);
    if (nlayers <= 0 || nlayers > 64) return false;
    char* spr = reinterpret_cast<char*>(m.spr);
    void* atlas = reinterpret_cast<void*>(
        *reinterpret_cast<uintptr_t*>(kSceneCtxPtr) + 0xc0);
    void*  best = nullptr;
    ImVec2 b0, b1;
    long   best_area = -1;
    for (int i = 0; i < nlayers; ++i) {
      int* L = reinterpret_cast<int*>(lbegin + i * 0x24);
      const int sprNo = L[2], sprType = L[8];
      if (sprNo < 0 || sprType >= 2) continue;
      const int cellBase = *reinterpret_cast<int*>(spr + 0x510 + sprType * 0xc);
      const int cellEnd  = *reinterpret_cast<int*>(spr + 0x514 + sprType * 0xc);
      if (static_cast<unsigned>(sprNo) >=
          static_cast<unsigned>((cellEnd - cellBase) >> 2)) continue;
      short* cell = *reinterpret_cast<short**>(cellBase + sprNo * 4);
      if (!cell) continue;
      const int palette = static_cast<int>(reinterpret_cast<uintptr_t>(spr + 0x110));
      int geom[12] = {0};
      void* ctex = reinterpret_cast<AtlasFn>(kAtlasGetCached)(atlas, nullptr, cell, palette, geom);
      if (!ctex) ctex = reinterpret_cast<AtlasFn>(kAtlasBuild)(atlas, nullptr, cell, palette, geom);
      if (!ctex) continue;
      const long area = static_cast<long>(cell[0]) * static_cast<long>(cell[1]);
      if (area > best_area) {
        best_area = area;
        best = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctex) + kCTexDX9Handle);
        b0 = ImVec2(*reinterpret_cast<float*>(&geom[3]), *reinterpret_cast<float*>(&geom[4]));
        b1 = ImVec2(*reinterpret_cast<float*>(&geom[5]), *reinterpret_cast<float*>(&geom[6]));
      }
    }
    if (best) { *out_tex = best; *uv0 = b0; *uv1 = b1; ok = true; }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return ok;
}

// ── Match-3 game state ────────────────────────────────────────────────────────
// A short-lived clear "explosion" spawned at a matched cell (rendered as an
// expanding fading ring + sparks in ImGui, in the monster's colour).
struct Boom { int r, c; DWORD t0; ImU32 col; };

// Animation timings.
constexpr DWORD kSwapMs = 140;   // slide duration
constexpr DWORD kBoomMs = 380;   // explosion lifetime

enum Phase { kIdle, kSwapping, kReverting };

struct Game {
  bool inited = false;
  int  cell[kN][kN];     // gem type 0..kTypes-1
  int  score = 0;
  int  sel_r = -1, sel_c = -1;  // currently selected cell (-1 = none)
  // Swap slide animation (the board already holds the swapped result; the two
  // animating cells are drawn sliding from each other's old spot).
  int   phase = kIdle;
  int   ar, ac, br, bc;         // the two animating cells
  DWORD anim_t0 = 0;
  DWORD last_action = 0;        // for the idle "hint" timer
  std::vector<Boom> booms;      // active explosions
};
Game g;

bool FindValidMove(int& mr, int& mc, int& mnr, int& mnc);  // fwd
void Reshuffle();                                          // fwd

int RandType() { return std::rand() % kTypes; }

// True if placing `t` at (r,c) would immediately complete a 3-in-a-row with the
// two cells already filled to its left / above (used to seed a match-free board).
bool WouldMatchAt(int r, int c, int t) {
  if (c >= 2 && g.cell[r][c - 1] == t && g.cell[r][c - 2] == t) return true;
  if (r >= 2 && g.cell[r - 1][c] == t && g.cell[r - 2][c] == t) return true;
  return false;
}

void NewGame() {
  for (int r = 0; r < kN; ++r)
    for (int c = 0; c < kN; ++c) {
      int t;
      do { t = RandType(); } while (WouldMatchAt(r, c, t));
      g.cell[r][c] = t;
    }
  int a, b, c, d;
  if (!FindValidMove(a, b, c, d)) Reshuffle();  // guarantee at least one move
  g.score = 0;
  g.sel_r = g.sel_c = -1;
  g.phase = kIdle;
  g.last_action = GetTickCount();
  g.inited = true;
}

// Mark all cells belonging to a horizontal/vertical run of >=3. Returns #marked.
int FindMatches(bool matched[kN][kN]) {
  for (int r = 0; r < kN; ++r)
    for (int c = 0; c < kN; ++c) matched[r][c] = false;
  int count = 0;
  // horizontal runs
  for (int r = 0; r < kN; ++r) {
    int run = 1;
    for (int c = 1; c <= kN; ++c) {
      if (c < kN && g.cell[r][c] == g.cell[r][c - 1]) {
        ++run;
      } else {
        if (run >= 3)
          for (int k = c - run; k < c; ++k)
            if (!matched[r][k]) { matched[r][k] = true; ++count; }
        run = 1;
      }
    }
  }
  // vertical runs
  for (int c = 0; c < kN; ++c) {
    int run = 1;
    for (int r = 1; r <= kN; ++r) {
      if (r < kN && g.cell[r][c] == g.cell[r - 1][c]) {
        ++run;
      } else {
        if (run >= 3)
          for (int k = r - run; k < r; ++k)
            if (!matched[k][c]) { matched[k][c] = true; ++count; }
        run = 1;
      }
    }
  }
  return count;
}

// Finds one swap (with the right/down neighbour) that would create a match.
// Returns it in (mr,mc)-(mnr,mnc); false if the board is deadlocked.
bool FindValidMove(int& mr, int& mc, int& mnr, int& mnc) {
  bool matched[kN][kN];
  for (int r = 0; r < kN; ++r)
    for (int c = 0; c < kN; ++c) {
      if (c + 1 < kN) {
        std::swap(g.cell[r][c], g.cell[r][c + 1]);
        const bool ok = FindMatches(matched) > 0;
        std::swap(g.cell[r][c], g.cell[r][c + 1]);
        if (ok) { mr = r; mc = c; mnr = r; mnc = c + 1; return true; }
      }
      if (r + 1 < kN) {
        std::swap(g.cell[r][c], g.cell[r + 1][c]);
        const bool ok = FindMatches(matched) > 0;
        std::swap(g.cell[r][c], g.cell[r + 1][c]);
        if (ok) { mr = r; mc = c; mnr = r + 1; mnc = c; return true; }
      }
    }
  return false;
}

// Shuffle the existing gems until the board has no immediate match but at least
// one valid move (anti-deadlock). Falls back to a fresh fill if unlucky.
void Reshuffle() {
  bool matched[kN][kN];
  int a, b, cc, d;
  for (int tries = 0; tries < 400; ++tries) {
    for (int i = kN * kN - 1; i > 0; --i) {
      const int j = std::rand() % (i + 1);
      std::swap(g.cell[i / kN][i % kN], g.cell[j / kN][j % kN]);
    }
    if (FindMatches(matched) == 0 && FindValidMove(a, b, cc, d)) return;
  }
}

// Clear matched cells, collapse each column downward, refill the top with new
// random gems. Cascades until the board is stable. Returns total cleared.
int ResolveCascades() {
  bool matched[kN][kN];
  int total = 0, combo = 0;
  while (true) {
    const int m = FindMatches(matched);
    if (m == 0) break;
    ++combo;
    g.score += m * kBaseScore * combo;  // chained cascades score more
    total += m;
    PlayRoSound("_stone_explosion.wav");  // one boom per cascade step
    // clear (spawn an explosion in each cleared monster's colour)
    for (int r = 0; r < kN; ++r)
      for (int c = 0; c < kN; ++c)
        if (matched[r][c]) {
          const int t = g.cell[r][c];
          if (t >= 0 && t < kTypes)
            g.booms.push_back(Boom{r, c, GetTickCount(), g_mon[t].fallback});
          g.cell[r][c] = -1;
        }
    // collapse columns (gems fall to the bottom)
    for (int c = 0; c < kN; ++c) {
      int write = kN - 1;
      for (int r = kN - 1; r >= 0; --r)
        if (g.cell[r][c] != -1) g.cell[write--][c] = g.cell[r][c];
      for (int r = write; r >= 0; --r) g.cell[r][c] = RandType();  // refill top
    }
  }
  int a, b, c2, d;
  if (!FindValidMove(a, b, c2, d)) Reshuffle();  // anti-deadlock
  return total;
}

// Start a swap: apply it to the board immediately and play the slide animation.
// The board→match decision happens in UpdateGame once the slide finishes.
void InitiateSwap(int r, int c, int nr, int nc) {
  if (nr < 0 || nr >= kN || nc < 0 || nc >= kN) return;
  std::swap(g.cell[r][c], g.cell[nr][nc]);
  g.phase = kSwapping;
  g.ar = r; g.ac = c; g.br = nr; g.bc = nc;
  g.anim_t0 = GetTickCount();
}

// Drives the animation state machine + ages out explosions. Called every frame.
//  kSwapping: slide plays, then -> resolve cascades (match) or revert-slide.
//  kReverting: slide-back plays, then -> idle.
void UpdateGame() {
  const DWORD now = GetTickCount();
  g.booms.erase(std::remove_if(g.booms.begin(), g.booms.end(),
                    [now](const Boom& b) { return now - b.t0 > kBoomMs; }),
                g.booms.end());

  if (g.phase == kIdle) return;
  if (now - g.anim_t0 < kSwapMs) return;  // slide still playing

  if (g.phase == kSwapping) {
    bool matched[kN][kN];
    const int m = FindMatches(matched);
    if (m > 0) {
      ResolveCascades();
      g.phase = kIdle;
    } else {
      std::swap(g.cell[g.ar][g.ac], g.cell[g.br][g.bc]);  // invalid -> revert
      g.phase = kReverting;
      g.anim_t0 = now;
    }
  } else {  // kReverting slide finished
    g.phase = kIdle;
  }
}

}  // namespace

// ── Plugin ────────────────────────────────────────────────────────────────────
void Rojeweled::OnModeSwitch(ModeMgr::ModeType, const char*) {
  for (auto& m : g_mon) { m.spr = m.act = nullptr; m.fails = 0; m.gaveup = false; }
}

void Rojeweled::OnRenderUI() {
  if (!enabled_) return;
  if (!g.inited) { std::srand(GetTickCount()); NewGame(); }
  for (auto& m : g_mon) LoadMon(m);  // load monster sprites (retry until in-world)

  const float board = kN * kCell + kPad * 2.0f;
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(board + 16.0f, board + 70.0f), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Rojeweled", &open,
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    bourgeon::CloseWindowOnEscape(open);
    ImGui::Text("Score : %d", g.score);
    ImGui::SameLine(0, 24);
    if (ImGui::SmallButton("Nouvelle partie")) NewGame();
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("Clique 2 monstres voisins pour les échanger");

    // Resolve each gem type's sprite once this frame (DX9 only).
    for (auto& m : g_mon) {
      m.ok = false;
      if (!g_imgui_dx7_active) m.ok = ResolveMonQuad(m, &m.tex, &m.uv0, &m.uv1);
    }

    UpdateGame();  // advance the slide/revert state machine + age explosions

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // Canonical ImGui click: the InvisibleButton's own return (fires on a press+
    // release over it) — more reliable than hovered + IsMouseClicked.
    const bool clicked = ImGui::InvisibleButton("board", ImVec2(board, board));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto CellMin = [&](int r, int c) {
      return ImVec2(origin.x + kPad + c * kCell, origin.y + kPad + r * kCell);
    };

    // Input: click to select, click an adjacent cell to swap. Only while idle
    // (no animation in flight).
    if (clicked && g.phase == kIdle) {
      const ImVec2 mp = ImGui::GetIO().MousePos;
      const int c = static_cast<int>((mp.x - origin.x - kPad) / kCell);
      const int r = static_cast<int>((mp.y - origin.y - kPad) / kCell);
      g.last_action = GetTickCount();
      if (r >= 0 && r < kN && c >= 0 && c < kN) {
        if (g.sel_r < 0) {
          g.sel_r = r; g.sel_c = c;
        } else {
          const int dr = r - g.sel_r, dc = c - g.sel_c;
          if ((std::abs(dr) == 1 && dc == 0) || (std::abs(dc) == 1 && dr == 0)) {
            InitiateSwap(g.sel_r, g.sel_c, r, c);
            g.sel_r = g.sel_c = -1;
          } else if (r == g.sel_r && c == g.sel_c) {
            g.sel_r = g.sel_c = -1;  // deselect
          } else {
            g.sel_r = r; g.sel_c = c;  // reselect
          }
        }
      }
    }

    // Slide offset for the two animating cells (they visually travel from each
    // other's old spot into their own). t: 0 = at the other cell, 1 = settled.
    const float st = (g.phase == kIdle) ? 1.0f
        : std::min(1.0f, (GetTickCount() - g.anim_t0) / static_cast<float>(kSwapMs));
    auto SlideOff = [&](int r, int c) -> ImVec2 {
      if (g.phase == kIdle) return ImVec2(0, 0);
      int or_ = -1, oc = -1;
      if (r == g.ar && c == g.ac) { or_ = g.br; oc = g.bc; }
      else if (r == g.br && c == g.bc) { or_ = g.ar; oc = g.ac; }
      if (or_ < 0) return ImVec2(0, 0);
      return ImVec2((oc - c) * kCell * (1.0f - st), (or_ - r) * kCell * (1.0f - st));
    };

    // Board background.
    dl->AddRectFilled(origin, ImVec2(origin.x + board, origin.y + board),
                      IM_COL32(24, 26, 42, 255), 6.0f);
    dl->AddRect(origin, ImVec2(origin.x + board, origin.y + board),
                IM_COL32(80, 90, 140, 255), 6.0f);

    // Cell backings (static checker).
    for (int r = 0; r < kN; ++r)
      for (int c = 0; c < kN; ++c) {
        const ImVec2 mn = CellMin(r, c);
        dl->AddRectFilled(ImVec2(mn.x + 1, mn.y + 1),
                          ImVec2(mn.x + kCell - 1, mn.y + kCell - 1),
                          ((r + c) & 1) ? IM_COL32(34, 37, 58, 255)
                                        : IM_COL32(28, 31, 50, 255), 3.0f);
      }

    // Gems (with slide offset + a coloured ring per type so look-alikes read).
    for (int r = 0; r < kN; ++r)
      for (int c = 0; c < kN; ++c) {
        const int t = g.cell[r][c];
        if (t < 0 || t >= kTypes) continue;
        const ImVec2 off = SlideOff(r, c);
        const ImVec2 mn(CellMin(r, c).x + off.x, CellMin(r, c).y + off.y);
        Mon& m = g_mon[t];
        const float inset = 4.0f;
        const ImVec2 gm(mn.x + inset, mn.y + inset),
                     gx(mn.x + kCell - inset, mn.y + kCell - inset);
        // STRONG type-colour pad behind the sprite — the gem's real identity.
        // Same-type cells now read as a same-colour line even though several
        // Poring-family monsters share a near-identical sprite (different ids).
        const ImU32 tc = m.fallback & 0x00ffffffu;
        const ImVec2 pmn(mn.x + 2, mn.y + 2), pmx(mn.x + kCell - 2, mn.y + kCell - 2);
        dl->AddRectFilled(pmn, pmx, tc | 0x6e000000u, 6.0f);          // fill
        dl->AddRect(pmn, pmx, tc | 0xff000000u, 6.0f, 0, 2.5f);        // bright ring
        if (m.ok && m.tex) {
          dl->AddImage((ImTextureID)(uintptr_t)m.tex, gm, gx, m.uv0, m.uv1);
        } else {
          const ImVec2 ctr((gm.x + gx.x) * 0.5f, (gm.y + gx.y) * 0.5f);
          dl->AddCircleFilled(ctr, (gx.x - gm.x) * 0.42f, m.fallback);
          dl->AddCircle(ctr, (gx.x - gm.x) * 0.42f, IM_COL32(0, 0, 0, 80), 0, 1.5f);
        }
      }

    // Explosions: RO-style ADDITIVE glow bursts — a soft coloured glow, a
    // white-hot core, an expanding shockwave ring and radial sparks, all
    // wrapped in additive blend so they add light like real RO effect sprites.
    if (!g.booms.empty() && !g_imgui_dx7_active) {
      const DWORD now = GetTickCount();
      dl->AddCallback(reinterpret_cast<ImDrawCallback>(D3D9_AdditiveBlendCallback()),
                      nullptr);
      for (const Boom& b : g.booms) {
        const float bt = std::min(1.0f, (now - b.t0) / static_cast<float>(kBoomMs));
        const float inv = 1.0f - bt;
        const ImVec2 mn = CellMin(b.r, b.c);
        const ImVec2 ctr(mn.x + kCell * 0.5f, mn.y + kCell * 0.5f);
        const ImU32 tc = b.col & 0x00ffffffu;
        // soft coloured glow (large, fades quadratically)
        dl->AddCircleFilled(ctr, kCell * (0.20f + bt * 0.45f),
                            tc | (static_cast<ImU32>(inv * inv * 150.0f) << 24), 20);
        // white-hot core (fast fade)
        dl->AddCircleFilled(ctr, kCell * 0.24f * inv,
                            IM_COL32(255, 255, 255, static_cast<int>(inv * inv * 220.0f)), 16);
        // expanding shockwave ring
        dl->AddCircle(ctr, kCell * (0.20f + bt * 0.85f),
                      tc | (static_cast<ImU32>(inv * 200.0f) << 24), 24, 3.0f * inv + 0.6f);
        // radial sparks flying outward
        for (int k = 0; k < 8; ++k) {
          const float ang = k * (6.2831853f / 8.0f) + b.r * 0.7f + b.c * 0.4f;
          const float rad = kCell * (0.10f + bt * 0.95f);
          const ImVec2 pp(ctr.x + std::cos(ang) * rad, ctr.y + std::sin(ang) * rad);
          dl->AddCircleFilled(pp, inv * 3.5f + 0.5f,
                              tc | (static_cast<ImU32>(inv * 230.0f) << 24));
        }
      }
      dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    // Selection highlight.
    if (g.sel_r >= 0) {
      const ImVec2 mn = CellMin(g.sel_r, g.sel_c);
      dl->AddRect(mn, ImVec2(mn.x + kCell, mn.y + kCell),
                  IM_COL32(255, 240, 120, 255), 3.0f, 0, 2.5f);
    }

    // Idle hint: after a few seconds with no move, gently pulse a valid swap so
    // the player can always see what to play (the monsters look alike).
    if (g.phase == kIdle && GetTickCount() - g.last_action > 3500) {
      int hr, hc, hnr, hnc;
      if (FindValidMove(hr, hc, hnr, hnc)) {
        const float pulse = 0.5f + 0.5f * std::sin(GetTickCount() * 0.006f);
        const ImU32 hcol = IM_COL32(120, 255, 160, 110 + static_cast<int>(pulse * 130));
        const int cells[2][2] = {{hr, hc}, {hnr, hnc}};
        for (int i = 0; i < 2; ++i) {
          const ImVec2 mn = CellMin(cells[i][0], cells[i][1]);
          dl->AddRect(mn, ImVec2(mn.x + kCell, mn.y + kCell), hcol, 4.0f, 0, 3.0f);
        }
      }
    }
  }
  ImGui::End();
  if (!open) enabled_ = false;
}
