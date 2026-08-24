#include "ragnarok/render.h"
#include "ragnarok/globals.h"
#include "features/overlays/status_icon_bar.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>

#include "imgui.h"
#include "bourgeon.h"
#include "features/gameplay/afk_screen.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "ui/ro_imgui.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ===========================================================================
// Status-icon BAR reimplementation (20250716 client / Moonlight-Destiny.exe,
// base 0x400000).  See status_icon_bar.h for the high-level design.
//
// Original build/layout function (replaced via JMP hook):
//   FUN_00bd4230(scene)  __thiscall(ECX = scene)
//     - early-outs on the scene rebuild gate (scene+0x174 force / +0x178 gate)
//     - clears DAT_015ffd80[100] (laid-out id grid, -1 == empty)
//     - loops layer 0..5 (outer) x active-status vector (inner, 0x14 stride)
//     - GetEFSTImgFileName(session, id, layer) -> "effect\*.tga" or NULL
//       (NULL == status not drawn at this layer); id 0x2a remaps to 0xc
//     - dedups against already-placed ids, allocates an effect-sprite node and
//       sets X(+0x394)/Y(+0x398), id(+0x3f8) etc, then records the id in the
//       grid.  Stock X = screenW-0x20+Xacc, Y = Yacc+0x96 (down 0x23, wrap
//       left 0x2d).  We keep everything identical EXCEPT the X/Y formula.
//
// ABI note: FUN_00568760 (sprite-ref by path) is called with FIVE stack args
// (path,0,0,1,0) in the binary — the Ghidra prototype (3 args) is wrong and
// would corrupt the stack.  We match the real push sequence.
// ===========================================================================

namespace {

// ---- engine addresses ------------------------------------------------------
constexpr uintptr_t kBuildFn     = 0x00bd4230;  // status-bar build/layout (hooked)
constexpr uintptr_t kMakeNode    = 0x00bb5d10;  // __thiscall(scene,0,0.0(8),0.0f) -> node
constexpr uintptr_t kGetEFSTImg  = 0x00d87380;  // __thiscall(session,id,layer) -> const char*
constexpr uintptr_t kSpriteRef   = 0x00568760;  // __thiscall(cache,path,0,0,1,0) -> ref (5 args!)

constexpr uintptr_t kVecBegin    = 0x0136e6c8;  // std::vector<StatusIcon>::begin (raw bytes)
constexpr uintptr_t kVecEnd      = 0x0136e6cc;  // ::end
constexpr uintptr_t kGridIds     = 0x015ffd80;  // int[100] laid-out ids (-1 empty)

constexpr int kVpW = 0x28, kVpH = 0x2c;
constexpr int kSceneNodeSlot = 0x11c84;  // scene[0x4721] = current node ptr
constexpr int kSceneForce    = 0x174;    // force-refresh flag
constexpr int kSceneGate     = 0x178;    // gate / sentinel 0x5f5e0fe
constexpr int kClearVtbl     = 0x40;     // scene vtable slot: clear icon nodes
constexpr int kElemStride    = 0x14;     // 20-byte active-status element
constexpr int kElemEndTick   = 0x04;     // element +0x04 = expiry tick (999999=inf)
constexpr uint32_t kInfiniteTick = 999999;  // sentinel for a permanent status
constexpr uint32_t kGateSentinel = 0x5f5e0fe;
constexpr uint32_t kNodeLifetime = 0x5f5e0ff;  // ~"infinite"

constexpr char kWhiteTex[] = "effect\\white02.bmp";

// __thiscall emulated as __fastcall + dummy edx (the project's convention).
using MakeNode_t  = void* (__fastcall*)(void* scene, void* edx, void* p2, double p3, float p4);
using GetImg_t    = const char* (__fastcall*)(void* session, void* edx, int id, int layer);
using SpriteRef_t = void* (__fastcall*)(void* cache, void* edx, const char* path,
                                        int a, int b, int c, int d);
using Alloc_t     = void* (__cdecl*)(size_t);
using ClearFn_t   = void (__fastcall*)(void* scene, void* edx, int, int, int, int, int,
                                       int, int, int, int);
using BuildFn_t   = void (__fastcall*)(void* scene, void* edx);
using RenderFn_t  = void (__fastcall*)(void* node);

// Type-0 effect-sprite render (FUN_00b5ed20).  Reads node+0x1c0 as the quad's
// diffuse color, AFTER the engine's per-frame fade animation has reset it — so
// it's the only point where forcing our alpha actually reaches the vertices.
// Gated on the 0x3e8=='b' marker (set only on status-icon nodes).
constexpr uintptr_t kRenderType0 = 0x00b5ed20;
constexpr int       kNodeMark    = 0x3e8;  // node+0x3e8 == 0x62 ('b') => status icon
constexpr uint8_t   kNodeMarkVal = 0x62;

const auto MakeNode  = reinterpret_cast<MakeNode_t>(kMakeNode);
const auto GetImg    = reinterpret_cast<GetImg_t>(kGetEFSTImg);
const auto SpriteRef = reinterpret_cast<SpriteRef_t>(kSpriteRef);
const auto Alloc     = reinterpret_cast<Alloc_t>(rag::kGameOperatorNewAddr);

// ---- configurable layout ---------------------------------------------------
// Named constants for the int fields of StatusIconConfig (see the header).
enum Corner { kTopLeft = 0, kTopRight = 1, kBottomLeft = 2, kBottomRight = 3 };
enum Dir    { kDown = 0, kUp = 1, kRight = 2, kLeft = 3 };
enum Sort   { kSortNone = 0, kSortLongest = 1, kSortShortest = 2 };

StatusIconConfig g_cfg;
void*      g_scene       = nullptr;
BuildFn_t  g_build_orig  = nullptr;
RenderFn_t g_render_orig = nullptr;  // trampoline to the stock type-0 render
bool      g_dirty       = false;   // settings changed -> force a rebuild
bool      g_in_game     = false;
bool      g_unlocked    = false;   // edit mode: draggable frame over the bar
bool      g_preview     = false;   // edit mode: inject fake statuses to preview
bool      g_needs_save  = false;   // persist the config once editing settles
int       g_placed_count = 0;      // icons placed by the last BuildCustom
uint32_t  g_endtick[100] = {};     // expiry tick per placed icon (for the overlay)

// ---- tooltip hit-test (FUN_00c93cb0) ---------------------------------------
// __thiscall(scene, int mouseX, int mouseY).  Reuses the stock tooltip build by
// feeding it synthesized stock-grid coords; we only swap the hit-test + nudge
// the resulting tooltip window to the cursor.
constexpr uintptr_t kHitTestFn = 0x00c93cb0;
constexpr int kTipWnd  = 0x5d0;   // scene+0x5d0 = tooltip window ptr
constexpr int kTipW    = 0x14;    // tooltip window: +0x14 width, +0x18 height
constexpr int kTipH    = 0x18;
constexpr int kTipSetPosVtbl = 0x10;  // tooltip window vtable slot: set position

using HitTestFn_t = void (__fastcall*)(void* scene, void* edx, int mx, int my);
using SetPosFn_t  = void (__fastcall*)(void* wnd, void* edx, int x, int y);

HitTestFn_t g_hittest_orig = nullptr;

inline void DirVec(int dir, int pitch, int* dx, int* dy) {
  *dx = *dy = 0;
  switch (dir) {
    case kDown:  *dy =  pitch; break;
    case kUp:    *dy = -pitch; break;
    case kRight: *dx =  pitch; break;
    case kLeft:  *dx = -pitch; break;
  }
}

constexpr int kIconSizeMin = 16, kIconSizeMax = 64;  // px (stock 32)

inline int IconSizePx() {
  int s = g_cfg.icon_size;
  if (s < kIconSizeMin) s = kIconSizeMin;
  else if (s > kIconSizeMax) s = kIconSizeMax;
  return s;
}

// Engine sprite scale (node+0x354/+0x360). The engine builds the quad at +/- the
// scale value, so the icon is (2*scale) px on a side: scale = size/2. Stock size
// is 32 px => scale 16.0f.
inline float IconScaleF() { return static_cast<float>(IconSizePx()) * 0.5f; }

// Icon hit/box half-extent in px (== the engine scale value).
inline int IconHalf() { return IconSizePx() / 2; }

void ComputeXY(int placed, int vpW, int vpH, float* ox, float* oy) {
  const bool right  = (g_cfg.corner == kTopRight || g_cfg.corner == kBottomRight);
  const bool bottom = (g_cfg.corner == kBottomLeft || g_cfg.corner == kBottomRight);
  // The engine renders the sprite CENTERED on (baseX,baseY), so shift the origin
  // one half-icon inward: the anchored CORNER of icon 0 (not its center) lands on
  // the margin — margin 0 => the icon sits flush against the screen edge, fully
  // on-screen. The drag/fold-back in DrawDragOverlay mirrors this same offset.
  const int half = IconHalf();
  int baseX = right  ? (vpW - g_cfg.margin_x - half) : (g_cfg.margin_x + half);
  int baseY = bottom ? (vpH - g_cfg.margin_y - half) : (g_cfg.margin_y + half);

  const int per = g_cfg.per_line < 1 ? 1 : g_cfg.per_line;
  const int line = placed / per;
  const int idx  = placed % per;

  // Spacing is an EDGE gap: 0 => icons touch. Center-to-center distance is the
  // icon span (2*half, scales with icon size) plus the configured gap, so the
  // gap stays constant in px regardless of the icon scale.
  const int span = 2 * half;
  int sx, sy, wx, wy;
  DirVec(g_cfg.step_dir, span + g_cfg.icon_pitch, &sx, &sy);
  DirVec(g_cfg.wrap_dir, span + g_cfg.line_pitch, &wx, &wy);

  *ox = static_cast<float>(baseX + line * wx + idx * sx);
  *oy = static_cast<float>(baseY + line * wy + idx * sy);
}

// Diffuse color field (node+0x1c0) the type-0 render (FUN_00b5ed20) copies onto
// every vertex of the icon quad.  The engine re-animates its alpha byte each
// frame (fade-in / expire-blink, steady ~0xfe), so a one-shot write here loses
// the race — our opacity is instead forced inside RenderIconHook, immediately
// before the render reads this field.  RGB stays white.
constexpr int kNodeColor = 0x1c0;

// Alpha byte (0..255) for the configured opacity %.
inline uint8_t IconAlphaByte() {
  int a = g_cfg.icon_alpha;
  if (a < 0) a = 0; else if (a > 100) a = 100;
  return static_cast<uint8_t>(a * 255 / 100);
}

// Faithful re-emit of one status-icon node (mirrors FUN_00bd4230's per-icon
// block), only X/Y come from our layout.  POD-only; caller is SEH-wrapped.
void EmitIcon(void* scene, int id, const char* path, float x, float y) {
  void* node = MakeNode(scene, nullptr, nullptr, 0.0, 0.0f);
  if (!node) return;
  auto* B = reinterpret_cast<uint8_t*>(node);

  // The engine references the node through scene+0x11c84 during setup.
  *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(scene) + kSceneNodeSlot) = node;

  *reinterpret_cast<int*>(B + 0x1c4)      = 0x201;
  *reinterpret_cast<uint8_t*>(B + 0x1c8)  = 1;
  *reinterpret_cast<int*>(B + 0x1dc)      = 4;
  *reinterpret_cast<int*>(B + 0x3c)       = 0;
  *reinterpret_cast<uint32_t*>(B + 0x1e4) = kNodeLifetime;
  *reinterpret_cast<float*>(B + 0x354) = IconScaleF();
  *reinterpret_cast<float*>(B + 0x360) = IconScaleF();
  *reinterpret_cast<float*>(B + 0x394)    = x;
  *reinterpret_cast<float*>(B + 0x398)    = y;
  *reinterpret_cast<uint8_t*>(B + 0x3e8)  = 0x62;
  *reinterpret_cast<int16_t*>(B + 0x3f8)  = static_cast<int16_t>(id);
  *reinterpret_cast<int*>(B + 0x204)      = 2;

  auto** tex = reinterpret_cast<void**>(Alloc(sizeof(void*) * 2));
  *reinterpret_cast<void**>(B + 0x144) = tex;
  if (tex) {
    void* cache = reinterpret_cast<void*>(render::kSpriteRefCacheAddr);
    tex[0] = SpriteRef(cache, nullptr, path,      0, 0, 1, 0);
    tex[1] = SpriteRef(cache, nullptr, kWhiteTex, 0, 0, 1, 0);
  }
}

inline int& SceneI(void* scene, int off) {
  return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(scene) + off);
}

// One drawable icon collected before layout.  endtick = element +0x04 =
// timeGetTime()+duration (absolute expiry), or 999999 (kInfiniteTick) = never.
struct Pending { int id; const char* path; uint32_t endtick; };

// Does status 'a' expire strictly later than 'b'?  999999 = infinite = latest.
// Wrap-safe via signed tick difference (all expiries are within ~24 days).
inline bool ExpiresLater(uint32_t a, uint32_t b) {
  if (a == kInfiniteTick) return b != kInfiniteTick;
  if (b == kInfiniteTick) return false;
  return static_cast<int32_t>(a - b) > 0;
}

// Stable insertion sort of the collected icons by expiry.  Stable => ties keep
// the engine's priority order.  Small n (<= ~40), POD-only (SEH-safe).
void SortByExpiry(Pending* list, int n, int mode) {
  for (int i = 1; i < n; ++i) {
    Pending key = list[i];
    int j = i - 1;
    while (j >= 0) {
      const bool after = (mode == kSortLongest)
                             ? ExpiresLater(key.endtick, list[j].endtick)
                             : ExpiresLater(list[j].endtick, key.endtick);
      if (!after) break;
      list[j + 1] = list[j];
      --j;
    }
    list[j + 1] = key;
  }
}

// Preview/edit mode: a spread of fake statuses (assorted expiries) so the
// player can see the layout / sort / remaining-time without real buffs.  id =
// a real EFST so the icon + tooltip resolve; dur_ms picked to span the time
// formats (sec / min / hours) plus one permanent.
struct Fake { int id; uint32_t dur_ms; };
const Fake kFakes[] = {
    {0x00, 18000},        {0x05, 45000},        {0x07, 95000},
    {0x68, 210000},       {0x73, 360000},       {0x74, 720000},
    {0x75, 1500000},      {0x76, 2700000},      {0x78, 3600000},
    {0x79, 9000000},      {0x96, 21600000},     {0x17, kInfiniteTick},
};

void AppendFakes(void* session, Pending* list, int* n, uint32_t now) {
  for (const Fake& f : kFakes) {
    if (*n >= 99) break;
    const char* path = nullptr;
    for (int layer = 0; layer <= 5 && !path; ++layer)
      path = GetImg(session, nullptr, f.id, layer);
    if (!path) continue;                       // no icon for this id -> skip
    bool dup = false;
    for (int k = 0; k < *n; ++k)
      if (list[k].id == f.id) { dup = true; break; }
    if (dup) continue;
    list[*n].id = f.id;
    list[*n].path = path;
    list[*n].endtick =
        (f.dur_ms == kInfiniteTick) ? kInfiniteTick : (now + f.dur_ms);
    ++*n;
  }
}

void BuildCustom(void* scene, void* vp) {
  const int vpW = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vp) + kVpW);
  const int vpH = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vp) + kVpH);

  // 1) Collect drawable icons in engine priority order (layer 0..5), deduped.
  Pending list[100];
  int n = 0;
  void* session = reinterpret_cast<void*>(rag::kSessionAddr);
  for (int layer = 0; layer <= 5; ++layer) {
    uint8_t* begin = *reinterpret_cast<uint8_t**>(kVecBegin);
    uint8_t* end   = *reinterpret_cast<uint8_t**>(kVecEnd);
    for (uint8_t* e = begin; e != end; e += kElemStride) {
      int id = *reinterpret_cast<int*>(e);
      const char* path = GetImg(session, nullptr, id, layer);
      if (id == 0x2a) id = 0xc;            // engine remap, after the img lookup
      if (!path) continue;

      bool dup = false;                     // skip ids already collected
      for (int k = 0; k < n; ++k)
        if (list[k].id == id) { dup = true; break; }
      if (dup) continue;

      if (n < 99) {
        list[n].id = id;
        list[n].path = path;
        list[n].endtick = *reinterpret_cast<uint32_t*>(e + kElemEndTick);
        ++n;
      }
    }
  }

  // 1b) Edit-mode preview: top up with fake statuses.
  if (g_preview) AppendFakes(session, list, &n, GetTickCount());

  // 2) Optionally re-order by expiry.
  if (g_cfg.sort_mode != kSortNone && n > 1)
    SortByExpiry(list, n, g_cfg.sort_mode);

  // 3) Lay out + emit.  DAT_015ffd80 + g_endtick mirror the final order for the
  //    tooltip hit-test and the remaining-time overlay.
  int* grid = reinterpret_cast<int*>(kGridIds);
  for (int i = 0; i < 100; ++i) grid[i] = -1;
  for (int i = 0; i < n; ++i) {
    float x, y;
    ComputeXY(i, vpW, vpH, &x, &y);
    EmitIcon(scene, list[i].id, list[i].path, x, y);
    grid[i] = list[i].id;
    g_endtick[i] = list[i].endtick;
  }
  g_placed_count = n;  // for the tooltip hit-test inverse lookup
}

// Render hook for the type-0 effect sprite (FUN_00b5ed20).  Runs once per node
// per frame, right before the render copies node+0x1c0 onto the quad vertices.
// For status-icon nodes (0x3e8=='b') with opacity < 100%, we force the alpha
// byte here — the only spot the engine's per-frame fade animation can't undo
// (it has already written its value by now; the render is about to read it).
// Absolute override (not a multiply) so it can never compound toward invisible.
// Works for both the stock and custom layouts.  SEH-guarded; always forwards.
void __fastcall RenderIconHook(void* node) {
  // 🔴 L'écran de veille passe par ICI, et il n'a pas le choix. Son effacement
  // de l'interface du client veto le rendu des FENÊTRES
  // (`UIWindowMgr_RenderWindows`) — or ces icônes n'en sont pas : ce sont des
  // nœuds de sprite de la SCÈNE, qui traversaient donc l'écran de veille intacts.
  // Ce hook est le seul endroit d'où les faire taire sans toucher ni à leur
  // construction ni à leur durée de vie : on écrit 0 dans l'alpha que le rendu
  // s'apprête à lire, et la frame suivante peut les redessiner comme si de rien
  // n'était.
  const AfkScreen* afk = Bourgeon::Instance().afk_screen();
  const bool afk_hides = (afk != nullptr) && afk->hiding_ui();
  if ((afk_hides || g_cfg.icon_alpha < 100) && node) {
    __try {
      auto* B = reinterpret_cast<uint8_t*>(node);
      if (*reinterpret_cast<uint8_t*>(B + kNodeMark) == kNodeMarkVal) {
        auto* col = reinterpret_cast<uint32_t*>(B + kNodeColor);
        const uint32_t a = afk_hides ? 0u : IconAlphaByte();
        *col = (*col & 0x00ffffffu) | (a << 24);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  if (g_render_orig) g_render_orig(node);
}

// Replacement for FUN_00bd4230.  Replicates the original's rebuild gate exactly
// (so the game's state machine + old-node clearing are untouched), then either
// forwards to stock or builds our custom layout.
void __fastcall BuildHook(void* scene, void* edx) {
  g_scene = scene;
  if (!g_cfg.enabled) {
    if (g_build_orig) g_build_orig(scene, edx);
    return;
  }
  __try {
    void* vp = *reinterpret_cast<void**>(render::kContextPtr);
    if (!vp) return;

    int gate = SceneI(scene, kSceneGate);
    if (static_cast<uint32_t>(gate) == kGateSentinel) { SceneI(scene, kSceneGate) = 0; gate = 0; }
    if (SceneI(scene, kSceneForce) != 0) {
      SceneI(scene, kSceneForce) = 0;
      SceneI(scene, kSceneGate)  = 0;
      void** vtbl = *reinterpret_cast<void***>(scene);
      reinterpret_cast<ClearFn_t>(vtbl[kClearVtbl / 4])(
          scene, nullptr, 0, 0x19, 0, 100, 0, 0, 0, 0, 0);
      gate = SceneI(scene, kSceneGate);
    }
    if (gate != 0) return;

    BuildCustom(scene, vp);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Force one rebuild on the cached scene (replicates a status-change refresh).
void ForceRebuild() {
  if (!g_scene) return;
  __try {
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(g_scene) + kSceneForce) = 1;
    BuildHook(g_scene, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Inverse of our layout: which placed icon is the cursor over?  Returns the
// placed index (== DAT_015ffd80 slot) or -1.  Icons are centered on ComputeXY.
int HitTestIcon(int mx, int my, int vpW, int vpH) {
  const int half = IconHalf();
  for (int i = 0; i < g_placed_count; ++i) {
    float cx, cy;
    ComputeXY(i, vpW, vpH, &cx, &cy);
    const int ix = static_cast<int>(cx), iy = static_cast<int>(cy);
    if (mx >= ix - half && mx <= ix + half &&
        my >= iy - half && my <= iy + half)
      return i;
  }
  return -1;
}

// Tooltip hit-test replacement.  When the custom layout is active, find the
// hovered icon ourselves, drive the stock tooltip build via synthesized
// stock-grid coords (so the engine looks up DAT_015ffd80[i] and builds the
// correct tooltip), then move the tooltip window to the cursor.
void __fastcall HitTestHook(void* scene, void* /*edx*/, int mx, int my) {
  if (!g_hittest_orig) return;  // fail safe if the trampoline didn't install
  if (!g_cfg.enabled || g_placed_count <= 0) {
    g_hittest_orig(scene, nullptr, mx, my);
    return;
  }
  __try {
    void* vp = *reinterpret_cast<void**>(render::kContextPtr);
    if (!vp) { g_hittest_orig(scene, nullptr, mx, my); return; }
    const int vpW = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vp) + kVpW);
    const int vpH = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vp) + kVpH);

    const int hit = HitTestIcon(mx, my, vpW, vpH);
    if (hit < 0) {
      // Off all icons: force the stock "miss" path so it hides the tooltip.
      g_hittest_orig(scene, nullptr, 0x7fffffff, 0);
      return;
    }

    // Synthesize the stock-grid coords whose linear index == hit, so the engine
    // resolves DAT_015ffd80[hit] and builds that status' tooltip.  Stock grid:
    // index = col*rowsPerCol + row, anchored right (x=W-0x12) / top (y=0xa8),
    // cell 0x2d x 0x23; +0xf lands inside the icon (clear of the gap).
    int rows = (vpH - 0xa9) / 0x23;
    if (rows < 1) rows = 1;
    const int col = hit / rows;
    const int row = hit % rows;
    const int anchorX = vpW - 0x12;
    // Asymmetry: the engine resolves the column with ceil((anchorX-mouseX)/0x2d)
    // but the row with floor((mouseY-0xa8)/0x23).  So X must land exactly on the
    // column boundary (offset 0) while Y can sit mid-cell (+0x0f) — verified
    // 0/3900 index mismatches across resolutions/counts.
    const int sx = anchorX - col * 0x2d;
    const int sy = 0xa8 + row * 0x23 + 0x0f;
    g_hittest_orig(scene, nullptr, sx, sy);

    // Move the freshly-built/shown tooltip window to the cursor.
    void* tip = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(scene) + kTipWnd);
    if (tip) {
      const int tw = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(tip) + kTipW);
      const int th = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(tip) + kTipH);
      int x = mx + 14, y = my + 8;
      if (x + tw > vpW) x = mx - tw - 6;
      if (y + th > vpH) y = vpH - th;
      if (x < 0) x = 0;
      if (y < 0) y = 0;
      void** vtbl = *reinterpret_cast<void***>(tip);
      reinterpret_cast<SetPosFn_t>(vtbl[kTipSetPosVtbl / 4])(tip, nullptr, x, y);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

const char* kCorners[] = {"Haut-gauche", "Haut-droite", "Bas-gauche", "Bas-droite"};
const char* kDirs[]    = {"Bas", "Haut", "Droite", "Gauche"};
const char* kSortModes[] = {"Aucun", "Plus long d'abord", "Plus court d'abord"};

// Remaining-time text placement relative to the icon, and (when inside) the
// 9-way anchor within the icon box.
const char* kTimePlaces[]  = {"Dessous", "Dessus", "Dans l'icône"};
const char* kTimeAnchors[] = {"Haut-gauche", "Haut", "Haut-droite",
                              "Gauche",      "Centre", "Droite",
                              "Bas-gauche",  "Bas",  "Bas-droite"};

// Compact colour swatch + popup picker (same UX as the skill-bar text colours).
bool ColorSwatch(const char* label, float col[4]) {
  bool changed = false;
  ImGui::PushID(label);
  if (ImGui::ColorButton("##sw", ImVec4(col[0], col[1], col[2], col[3]),
                         ImGuiColorEditFlags_AlphaPreview, ImVec2(20, 20)))
    ImGui::OpenPopup("pick");
  ImGui::SameLine();
  ImGui::TextUnformatted(label);
  if (ImGui::BeginPopup("pick")) {
    changed = ImGui::ColorPicker4("##p", col,
                        ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview);
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

// Unlocked edit mode: a translucent ImGui frame over the bar's bounding box.
// Dragging it updates the corner margins (so Marge X/Y stay in sync) and flags
// a rebuild.  The icons are engine-drawn, so this is an overlay, not the bar.
void DrawDragOverlay() {
  void* vp = *reinterpret_cast<void**>(render::kContextPtr);
  if (!vp) return;
  const int vpW = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vp) + kVpW);
  const int vpH = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vp) + kVpH);

  // Bounding box of the placed icons (each centered on ComputeXY, +/-IconHalf()).
  const int cnt = g_placed_count > 0 ? g_placed_count : 1;
  const int half = IconHalf();
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (int i = 0; i < cnt; ++i) {
    float cx, cy;
    ComputeXY(i, vpW, vpH, &cx, &cy);
    if (cx - half < x0) x0 = cx - half;
    if (cy - half < y0) y0 = cy - half;
    if (cx + half > x1) x1 = cx + half;
    if (cy + half > y1) y1 = cy + half;
  }
  // No padding: the frame is exactly the icon cluster's bounding box, so its
  // anchored corner coincides with the first icon's corner — the snap point
  // then aligns the icon corner (not a padded frame edge) to the grid.
  const ImVec2 pos(x0, y0);
  const ImVec2 size(x1 - x0, y1 - y0);

  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.25f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.85f, 0.20f, 0.9f));
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;

  // Shared alignment grid (owned by MoonlightUi). SnapAxis is a no-op when grid
  // snapping is off, so we can apply it unconditionally when present.
  const AlignGrid* grid = nullptr;
  if (auto* mui = Bourgeon::Instance().moonlight_ui()) grid = &mui->grid_;

  const bool right  = (g_cfg.corner == kTopRight || g_cfg.corner == kBottomRight);
  const bool bottom = (g_cfg.corner == kBottomLeft || g_cfg.corner == kBottomRight);

  // The snap anchor must match the FRAME's anchor: for a right-anchored corner
  // it's the box's right edge, for a bottom-anchored corner its bottom edge — so
  // the corner the user pins to is the one that lands on a grid line. With no
  // padding + the ComputeXY half-icon inset, this box corner equals the margin
  // position directly (right edge = vpW - margin_x; left edge = margin_x), so the
  // new margin folds straight back from the snapped corner — no origin math.
  const float anchorX = right  ? (pos.x + size.x) : pos.x;
  const float anchorY = bottom ? (pos.y + size.y) : pos.y;

  // Grab offset: mouse-to-anchor-corner, captured on press so the drag tracks an
  // absolute target (needed for the anchored corner to land on grid lines).
  static bool  dragging = false;
  static float grab_dx = 0.0f, grab_dy = 0.0f;

  if (ImGui::Begin("##status_drag", nullptr, flags)) {
    ImGui::InvisibleButton("##grab", size);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (ImGui::IsItemActive()) {
      const ImVec2 m = ImGui::GetIO().MousePos;
      if (!dragging) {
        dragging = true;
        grab_dx = m.x - anchorX;
        grab_dy = m.y - anchorY;
      }
      // Desired anchor-corner position, snapped to the visible grid lines.
      float nax = m.x - grab_dx, nay = m.y - grab_dy;
      if (grid) {
        nax = grid->SnapAxis(nax, static_cast<float>(vpW));
        nay = grid->SnapAxis(nay, static_cast<float>(vpH));
      }
      // The box corner IS the margin position, so fold it straight back.
      int nmx = static_cast<int>((right  ? (vpW - nax) : nax) + 0.5f);
      int nmy = static_cast<int>((bottom ? (vpH - nay) : nay) + 0.5f);
      // La BOÎTE d'icônes reste entièrement dans le viewport, pas seulement son coin
      // ancré : borner la marge au viewport laissait pousser le cluster entier dehors
      // (marge = vpW, ancrage à gauche -> icônes hors écran à droite). La marge étant
      // mesurée DEPUIS le coin ancré, le maximum est le même dans les deux sens :
      // vp - taille de la boîte. Cluster plus grand que l'écran -> marge 0 (collé).
      int max_margin_x = vpW - static_cast<int>(size.x + 0.5f);
      int max_margin_y = vpH - static_cast<int>(size.y + 0.5f);
      if (max_margin_x < 0) max_margin_x = 0;
      if (max_margin_y < 0) max_margin_y = 0;
      if (nmx < 0) nmx = 0; else if (nmx > max_margin_x) nmx = max_margin_x;
      if (nmy < 0) nmy = 0; else if (nmy > max_margin_y) nmy = max_margin_y;
      if (nmx != g_cfg.margin_x || nmy != g_cfg.margin_y) {
        g_cfg.margin_x = nmx;
        g_cfg.margin_y = nmy;
        g_dirty = true;       // OnTick rebuilds the bar at the new origin
        g_needs_save = true;  // flushed on release (when no item is active)
      }
    } else {
      dragging = false;
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

// Compact remaining-time text: <60s -> "%ds", <1h -> "%dm", <1d -> "%dh",
// else "%dj".  Returns false (no text) for permanent or already-expired.
bool FormatRemaining(uint32_t endtick, uint32_t now, char* buf, int cap) {
  if (endtick == kInfiniteTick) return false;
  const int32_t ms = static_cast<int32_t>(endtick - now);
  if (ms <= 0) return false;
  const int sec = ms / 1000;
  if (sec < 60)         snprintf(buf, cap, "%ds", sec);
  else if (sec < 3600)  snprintf(buf, cap, "%dm", sec / 60);
  else if (sec < 86400) snprintf(buf, cap, "%dh", sec / 3600);
  else                  snprintf(buf, cap, "%dj", sec / 86400);
  return true;
}

// Text position within/around an icon box (centered on cx,cy; half-extent
// `half`). place: 0=below 1=above 2=inside; anchor (inside only): 0..8 as
// TL,T,TR,L,C,R,BL,B,BR. Returns the top-left draw position for the glyph run.
void TimeTextPos(float cx, float cy, int half, const ImVec2& ts,
                 int place, int anchor, float* tx, float* ty) {
  const float gap = 1.0f;
  if (place == 0) {          // below the icon, horizontally centered
    *tx = cx - ts.x * 0.5f;
    *ty = cy + half + gap;
  } else if (place == 1) {   // above the icon, horizontally centered
    *tx = cx - ts.x * 0.5f;
    *ty = cy - half - ts.y - gap;
  } else {                   // inside the icon: 9-way anchor
    const int hcol = anchor % 3;  // 0=left 1=center 2=right
    const int vrow = anchor / 3;  // 0=top  1=middle 2=bottom
    *tx = (hcol == 0) ? (cx - half)
        : (hcol == 2) ? (cx + half - ts.x)
                      : (cx - ts.x * 0.5f);
    *ty = (vrow == 0) ? (cy - half)
        : (vrow == 2) ? (cy + half - ts.y)
                      : (cy - ts.y * 0.5f);
  }
}

// Remaining-time text at each icon, drawn via the BACKGROUND draw list (stays
// under all ImGui windows). Styling mirrors the skill-bar text: independent
// glyph / shadow colours, an 8-way halo, and optional faux-bold. All colours
// fade with the icon opacity so the text tracks the bar.
void DrawRemainingTime() {
  void* vp = *reinterpret_cast<void**>(render::kContextPtr);
  if (!vp) return;
  const int vpW = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vp) + kVpW);
  const int vpH = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vp) + kVpH);
  const uint32_t now = GetTickCount();
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  const float af = IconAlphaByte() / 255.0f;  // fade text with the icons
  auto fade = [af](const float* c) {
    return IM_COL32(static_cast<int>(c[0] * 255), static_cast<int>(c[1] * 255),
                    static_cast<int>(c[2] * 255), static_cast<int>(c[3] * 255 * af));
  };
  const ImU32 cText   = fade(g_cfg.col_time_text);
  const ImU32 cShadow = fade(g_cfg.col_time_shadow);
  const ImU32 cBg     = IM_COL32(0, 0, 0, static_cast<int>(165 * af));
  const int   half    = IconHalf();
  const bool  bold    = g_cfg.time_bold;

  for (int i = 0; i < g_placed_count; ++i) {
    char buf[16];
    if (!FormatRemaining(g_endtick[i], now, buf, sizeof(buf))) continue;
    float cx, cy;
    ComputeXY(i, vpW, vpH, &cx, &cy);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    float tx, ty;
    TimeTextPos(cx, cy, half, ts, g_cfg.time_place, g_cfg.time_anchor, &tx, &ty);

    if (g_cfg.time_bg)
      dl->AddRectFilled(ImVec2(tx - 3, ty - 1), ImVec2(tx + ts.x + 3, ty + ts.y + 1),
                        cBg, 3.0f);
    // 8-way halo (outline/shadow) for contrast over the game scene.
    for (int oy = -1; oy <= 1; ++oy)
      for (int ox = -1; ox <= 1; ++ox)
        if (ox || oy) dl->AddText(ImVec2(tx + ox, ty + oy), cShadow, buf);
    dl->AddText(ImVec2(tx, ty), cText, buf);
    if (bold) dl->AddText(ImVec2(tx + 1.0f, ty), cText, buf);  // faux-bold
  }
}

}  // namespace

StatusIconBar::StatusIconBar() {
  // Verify the expected prologue before hooking (55 8B EC 83 EC 28 = push ebp;
  // mov ebp,esp; sub esp,0x28) so a client mismatch fails safe.
  const auto* p = reinterpret_cast<const uint8_t*>(kBuildFn);
  if (!(p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x83 &&
        p[4] == 0xEC && p[5] == 0x28)) {
    LogError("[StatusIcons] FUN_00bd4230 prologue mismatch; hook skipped");
    return;
  }
  g_build_orig = reinterpret_cast<BuildFn_t>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kBuildFn),
          reinterpret_cast<uint8_t*>(&BuildHook)));
  if (g_build_orig)
    /* LogInfo("[StatusIcons] status-bar layout hook installed") */;
  else
    LogError("[StatusIcons] failed to install status-bar layout hook");

  // Tooltip hit-test (FUN_00c93cb0): prologue 55 8B EC 6A FF (push ebp; mov
  // ebp,esp; push -1).  Front-end hook to keep tooltips aligned with the moved
  // bar; forwards verbatim when the custom layout is disabled.
  const auto* q = reinterpret_cast<const uint8_t*>(kHitTestFn);
  if (q[0] == 0x55 && q[1] == 0x8B && q[2] == 0xEC && q[3] == 0x6A && q[4] == 0xFF) {
    g_hittest_orig = reinterpret_cast<HitTestFn_t>(
        hooking::HookManager::Instance().SetHook(
            hooking::HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kHitTestFn),
            reinterpret_cast<uint8_t*>(&HitTestHook)));
    if (g_hittest_orig)
      /* LogInfo("[StatusIcons] tooltip hit-test hook installed") */;
    else
      LogError("[StatusIcons] failed to install tooltip hit-test hook");
  } else {
    LogError("[StatusIcons] FUN_00c93cb0 prologue mismatch; tooltip hook skipped");
  }

  // Type-0 effect render (FUN_00b5ed20): prologue 55 8B EC 83 EC 1C.  Hooked to
  // force the icon opacity right before the quad color is read (the engine's
  // per-frame fade animation overwrites node+0x1c0, so this is the only winning
  // spot).  Forwards verbatim; the per-node 'b' gate keeps it icon-only.
  const auto* r = reinterpret_cast<const uint8_t*>(kRenderType0);
  if (r[0] == 0x55 && r[1] == 0x8B && r[2] == 0xEC && r[3] == 0x83 &&
      r[4] == 0xEC && r[5] == 0x1C) {
    g_render_orig = reinterpret_cast<RenderFn_t>(
        hooking::HookManager::Instance().SetHook(
            hooking::HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kRenderType0),
            reinterpret_cast<uint8_t*>(&RenderIconHook)));
    if (g_render_orig)
      /* LogInfo("[StatusIcons] icon-opacity render hook installed") */;
    else
      LogError("[StatusIcons] failed to install icon-opacity render hook");
  } else {
    LogError("[StatusIcons] FUN_00b5ed20 prologue mismatch; opacity hook skipped");
  }
}

void StatusIconBar::OnTick() {
  if (g_dirty && g_in_game) {
    ForceRebuild();
    g_dirty = false;
  }
}

void StatusIconBar::OnModeSwitch(ModeMgr::ModeType mode_type,
                                    const char* /*map_name*/) {
  g_in_game = (mode_type == ModeMgr::ModeType::kGame);
  if (!g_in_game) g_scene = nullptr;  // don't poke a stale scene from login
}

// The settings controls (no window) — hosted by moonlight_ui's settings panel.
void StatusIconBar::DrawSettings() {
  bool changed = false;

  changed |= ro::RoCheckbox(i18n::Tr("Disposition personnalisée"), &g_cfg.enabled);
  SameLine(); HelpMarker(i18n::Tr("Désactivé = disposition d'origine du jeu (inchangée)."));

  ImGui::BeginDisabled(!g_cfg.enabled);

  // --- edit mode --------------------------------------------------------
  ro::RoCheckbox(i18n::Tr("Déverrouiller (glisser pour déplacer)"), &g_unlocked);
  SameLine(); HelpMarker(
    i18n::Tr("Affiche un cadre sur la barre ; glissez-le pour la déplacer.\n"
    "La position met à jour Marge X / Marge Y.\n"
    "Si la grille d'alignement (aimantation) est active, le cadre s'y aligne."));
  changed |= ro::RoCheckbox(i18n::Tr("Aperçu (faux statuts)"), &g_preview);
  SameLine(); HelpMarker(i18n::Tr("Génère de faux statuts pour prévisualiser la disposition."));

  SeparatorText(i18n::Tr("Réglages"));
  // Opacity is independent of the custom layout — it's forced at render time on
  // any status-icon node, so it works with the stock layout too.  The render
  // hook reads g_cfg.icon_alpha live, so no rebuild is needed.
  changed |= WheelSliderInt(i18n::Tr("Opacité des icônes"), &g_cfg.icon_alpha, 10, 100, "%d%%");
  SameLine(); HelpMarker(
    i18n::Tr("Transparence des icônes de statut (100 % = opaque, comme l'origine).\n"
    "Fonctionne même sans la disposition personnalisée."));

  changed |= WheelSliderInt(i18n::Tr("Taille des icônes"), &g_cfg.icon_size,
                            kIconSizeMin, kIconSizeMax, "%d px");
  SameLine(); HelpMarker(
    i18n::Tr("Taille des icônes de statut en pixels (32 = origine).\n"
    "Nécessite la disposition personnalisée.\n"
    "L'espacement (écart entre bords) est indépendant de la taille."));
  changed |= ro::RoCombo(i18n::Tr("Coin d'ancrage"), &g_cfg.corner, kCorners, IM_ARRAYSIZE(kCorners));
  Tooltip(i18n::Tr("Coin de la première icône qui sert d'ancrage."));
  changed |= ro::RoCombo(i18n::Tr("Sens d'empilement"), &g_cfg.step_dir, kDirs, IM_ARRAYSIZE(kDirs));
  Tooltip(i18n::Tr("Direction dans laquelle les icônes s'empilent à partir du coin d'ancrage."));
  changed |= ro::RoCombo(i18n::Tr("Retour à la ligne"), &g_cfg.wrap_dir, kDirs, IM_ARRAYSIZE(kDirs));
  Tooltip(i18n::Tr("Direction dans laquelle les icônes passent à la ligne suivante."));
  changed |= WheelSliderInt(i18n::Tr("Icônes par ligne"), &g_cfg.per_line, 1, 20);
  changed |= WheelSliderInt(i18n::Tr("Espacement icônes"), &g_cfg.icon_pitch, 0, 20, "%d px");
  SameLine(); HelpMarker(
    i18n::Tr("Écart entre les bords de deux icônes voisines, en pixels.\n"
    "0 = icônes collées ; augmentez pour les espacer."));
  changed |= WheelSliderInt(i18n::Tr("Espacement lignes"), &g_cfg.line_pitch, 0, 20, "%d px");
  SameLine(); HelpMarker(i18n::Tr("Écart entre les bords de deux lignes/colonnes, en pixels (0 = collées)."));
  changed |= ro::RoCombo(i18n::Tr("Tri par durée"), &g_cfg.sort_mode, kSortModes, IM_ARRAYSIZE(kSortModes));
  SameLine();
  HelpMarker(i18n::Tr("Trie les icônes selon le temps d'expiration restant."));
  changed |= ro::RoCheckbox(i18n::Tr("Afficher le temps restant"), &g_cfg.show_remaining);
  SameLine(); HelpMarker(i18n::Tr("Affiche le temps restant sous chaque icône."));

  ImGui::BeginDisabled(!g_cfg.show_remaining);
  Indent();
    changed |= ro::RoCombo(i18n::Tr("Placement du texte"), &g_cfg.time_place,
                           kTimePlaces, IM_ARRAYSIZE(kTimePlaces));
    SameLine(); HelpMarker(i18n::Tr("Dessous / dessus l'icône, ou à l'intérieur."));
    ImGui::BeginDisabled(g_cfg.time_place != 2);  // anchor only when "inside"
      changed |= ro::RoCombo(i18n::Tr("Position dans l'icône"), &g_cfg.time_anchor,
                             kTimeAnchors, IM_ARRAYSIZE(kTimeAnchors));
      SameLine(); HelpMarker(i18n::Tr("Ancrage du texte à l'intérieur de l'icône (9 positions)."));
    ImGui::EndDisabled();
    changed |= ro::RoCheckbox(i18n::Tr("Texte \"gras\""), &g_cfg.time_bold);
    SameLine(); HelpMarker(i18n::Tr("Faux-gras (ImGui n'a pas de fonte grasse)."));
    changed |= ColorSwatch(i18n::Tr("Couleur du texte"), g_cfg.col_time_text);
    changed |= ColorSwatch(i18n::Tr("Couleur de l'ombre"), g_cfg.col_time_shadow);
    SameLine(); HelpMarker(i18n::Tr("Contour/ombre 8 directions ; baissez l'alpha pour l'atténuer."));
    changed |= ro::RoCheckbox(i18n::Tr("Fond derrière le texte"), &g_cfg.time_bg);
    SameLine(); HelpMarker(i18n::Tr("Ajoute un fond sombre derrière le temps restant."));
  Unindent();
  ImGui::EndDisabled();

  if (ImGui::Button(i18n::Tr("Réinitialiser (origine)"))) {
    g_cfg.corner = kTopRight; g_cfg.margin_x = 32; g_cfg.margin_y = 185;
    g_cfg.step_dir = kDown; g_cfg.wrap_dir = kLeft; g_cfg.per_line = 17;
    g_cfg.icon_pitch = 3; g_cfg.line_pitch = 13; g_cfg.sort_mode = kSortNone;
    g_cfg.icon_alpha = 100; g_cfg.icon_size = 32;
    g_cfg.time_place = 0; g_cfg.time_anchor = 7; g_cfg.time_bold = false;
    g_cfg.col_time_text[0] = g_cfg.col_time_text[1] =
        g_cfg.col_time_text[2] = g_cfg.col_time_text[3] = 1.0f;
    g_cfg.col_time_shadow[0] = g_cfg.col_time_shadow[1] =
        g_cfg.col_time_shadow[2] = 0.0f; g_cfg.col_time_shadow[3] = 0.9f;
    changed = true;
  }

  ImGui::EndDisabled();

  if (changed) { g_dirty = true; g_needs_save = true; }  // rebuild + persist
}

void StatusIconBar::OnRenderUI() {
  const bool active = g_cfg.enabled && g_in_game;

  // Game-world overlays (independent of the settings panel, which now lives in
  // moonlight_ui and calls DrawSettings()).
  if (active && g_cfg.show_remaining) DrawRemainingTime();
  if (active && g_unlocked) DrawDragOverlay();

  // Persist once an edit settles (no slider/drag actively held).  moonlight_ui's
  // OnRenderUI (and thus DrawSettings) runs earlier in the frame, so g_needs_save
  // set by the controls or the drag is flushed here.
  if (g_needs_save && !ImGui::IsAnyItemActive()) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    g_needs_save = false;
  }
}

StatusIconConfig& StatusIconBar::config() { return g_cfg; }

void StatusIconBar::MarkDirty() { g_dirty = true; }
