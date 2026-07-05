#include "plugins/basic_info.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "imgui.h"
#include "plugins/moonlight_ui.h"  // shared AlignGrid (snap + draw)
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// DX9 vs DX7 active backend (defined in ragnarok_client.cc) — picks the
// CTexture native-handle offset, like the RO cursor capture.
extern bool g_imgui_dx7_active;

// ── Live value sources (20250716 client) ─────────────────────────────────────
// EXP confirmed by RE of UIBasicInfoWnd::OnCreate (UIINT64BarGraph_SetCurMax) —
// INT64.  HP/SP confirmed by UIBasicInfoWnd::DrawContent, which reads them as
// (int) — INT32.  `max` may sit below `cur` (job exp); that's fine.
namespace {
constexpr float kSnapThreshold = 10.0f;  // px magnetism radius for sticky snap

struct Src {
  uintptr_t cur, max;
  bool      wide;       // true = INT64, false = INT32
  const char* label;
  const char* win_id;   // ImGui window id (### keeps it stable)
};
const Src kSrc[BasicInfoTweaks::kBarCount] = {
  {0x015fb9d0, 0x015fb9d8, true,  "Base", "###BIBaseExp"},
  {0x015fb9e8, 0x015fb9e0, true,  "Job",  "###BIJobExp"},
  {0x015ff908, 0x015ff90c, false, "HP",   "###BIHp"},
  {0x015ff910, 0x015ff914, false, "SP",   "###BISp"},
};

inline long long RDval(uintptr_t addr, bool wide) {
  return wide ? *reinterpret_cast<const volatile int64_t*>(addr)
              : static_cast<long long>(
                    *reinterpret_cast<const volatile int32_t*>(addr));
}

inline float ExpFrac(long long cur, long long max) {
  if (max <= 0) return 0.0f;
  const double f = static_cast<double>(cur) / static_cast<double>(max);
  if (f < 0.0) return 0.0f;
  if (f > 1.0) return 1.0f;
  return static_cast<float>(f);
}

// A full-screen UI (world map = window id 0x8c) replaces the in-game HUD; hide
// the bars + alignment grid while it is open. FindWindow @0x00a47b90 on the
// window manager @0x0131f4e8; the world-map window is destroyed on close, so
// this tracks its open-state exactly. (Mirrors MenuIconTweaks::HudReplaced.)
bool HudReplaced() {
  using FindWindowFn = void* (__thiscall*)(void*, int);
  return reinterpret_cast<FindWindowFn>(0x00a47b90)(
             reinterpret_cast<void*>(0x0131f4e8), 0x8c) != nullptr;
}

// ── Status-portrait value sources (20250716 client) ──────────────────────────
constexpr uintptr_t kBaseLevel = 0x015fb9f0;  // DAT_015fb9f0 (UIBasicInfoWnd "Base Lv.")
constexpr uintptr_t kJobLevel  = 0x015fb9f8;  // DAT_015fb9f8 (UIBasicInfoWnd "Job Lv.")
constexpr uintptr_t kSession   = 0x015fa3c0;  // g_session

inline int RDi(uintptr_t a) { return *reinterpret_cast<volatile int*>(a); }

// Class-name lookup, exactly as UIBasicInfoWnd::DrawContent does it:
//   jobid = FUN_00d5b580(session);  name = FUN_00d5bb40(session, jobid, -1)
// Both are __thiscall (this=session); called via __fastcall with a dummy edx so
// the real args land on the stack (standard thiscall->fastcall shim).
const char* ClassName() {
  using GetJobId_t     = int (__fastcall*)(void* ecx, void* edx);
  using GetClassName_t = const char* (__fastcall*)(void* ecx, void* edx,
                                                   unsigned jobid, int sex);
  const int jobid = reinterpret_cast<GetJobId_t>(0x00d5b580)(
      reinterpret_cast<void*>(kSession), nullptr);
  const char* n = reinterpret_cast<GetClassName_t>(0x00d5bb40)(
      reinterpret_cast<void*>(kSession), nullptr, static_cast<unsigned>(jobid), -1);
  return n ? n : "";
}

// ── Head-sprite capture (the "regenerated" portrait) ─────────────────────────
// We render the player's character with the game's own actor renderer
// (FUN_007ac210 ctor + FUN_007ac820 draw, dir/action 0/0 = front idle) and
// capture each sprite layer through a hook on the quad-submit FUN_00a1b7c0
// (same idea as the RO-cursor capture). The captured atlas textures + UVs are
// re-composited via ImGui into the head element. Appearance is read live from
// the same globals UIBasicInfoWnd's equip-doll uses (FUN_008cf970), so it tracks
// hair/colour changes; head-gear + weapon are omitted in v1 (head focus).
constexpr uintptr_t kActorCtor   = 0x007ac210;  // actor-object ctor (19 params)
constexpr uintptr_t kActorDraw   = 0x007ac820;  // actor draw (param 1 = quad path)
constexpr uintptr_t kActorDtor   = 0x0079a6a0;  // actor-object destructor
constexpr uintptr_t kGetSex      = 0x00d84760;  // GetSex(session)
constexpr uintptr_t kActorQuadFn = 0x00a1b7c0;  // textured-quad submit (hooked)
constexpr uintptr_t kAtlasGetFn  = 0x00566b70;  // SpriteAtlas_GetCachedTexture
constexpr uintptr_t kSceneCtxPtr = 0x012515f8;  // -> scene render ctx (atlas @+0xc0)
constexpr uintptr_t kRenderCtxPtr = 0x0131f6c4;  // BasicInfo window = a valid renderCtx
constexpr uintptr_t kHair        = 0x015fb278;  // DAT_015fb278 hair style
constexpr uintptr_t kClothesCol  = 0x015fb28c;  // DAT_015fb28c clothes palette
constexpr uintptr_t kHairCol     = 0x015fb290;  // DAT_015fb290 hair palette
constexpr uintptr_t kGarmentView = 0x015fb2a0;  // g_OwnLook_GarmentRobeViewId
constexpr int kCTexOffDX9 = 0x12c, kCTexOffDX7 = 0x128;  // CTexture -> native GPU handle

struct CapLayer {
  void*  tex;                 // IDirect3DTexture9* (atlas page)
  ImVec2 uv0, uv1;            // atlas sub-rect
  float  cx, cy, w, h;        // sprite centre + scaled size (actor space)
  bool   mirror;
  bool   head_region;         // true = face/hair/head-gear (RGBA), false = body/garment
};
CapLayer g_caps[48];
int      g_cap_count   = 0;
bool     g_cap_active  = false;  // set only during our actor render
// Buffer SÉPARÉ pour l'aperçu d'équipement (ne doit PAS écraser g_caps du portrait
// — les 2 rendus cohabitent). Le hook écrit vers la cible active g_cap_buf/g_cap_num.
CapLayer  g_pv_caps[48];
int       g_pv_count = 0;
CapLayer* g_cap_buf = g_caps;
int*      g_cap_num = &g_cap_count;
bool     g_portrait_debug = false;  // when set, LogPortraitDiag logs each pass

// ── Diagnostics ──────────────────────────────────────────────────────────────
// Record EVERY layer that reaches the hook during our capture (type/size/branch/
// tex-resolved) so the log shows exactly what the actor emits and why a layer is
// or isn't captured. Drained + logged (throttled) by CapturePortraitActor.
struct DiagLayer {
  int type; int w; int h; int branch; bool tex;
  float cx; float cy;      // computed sprite centre (actor space)
  int   ox; int oy;        // act_layer[0]/[1] offsets
  float rawx; float rawy;  // game's x/y at the quad submit (carries attach)
};
DiagLayer g_diag[48];
int       g_diag_count = 0;

// NB: x and y (param_1/param_2) are SIGNED INTEGER pixel offsets, NOT floats —
// the caller pushes them as ints (the body-anchor sync is baked in, often
// negative). Reading y as a float reinterprets e.g. -1 (0xFFFFFFFF) as a NaN,
// which is the bug that mis-placed the head/head-gears. `palette` is likewise a
// raw pointer the decompiler mis-typed as float.
using ActorQuadFn = void(__fastcall*)(void* self, void* edx, int x, int y,
                                      void* p3, void* p4, short* spr_frame,
                                      int* act_layer, float scale, float angle,
                                      unsigned color, void* palette, float p11);
using AtlasGetFn  = void*(__fastcall*)(void* self, void* edx, int spr_frame,
                                       int palette, int* geom);
ActorQuadFn g_orig_actor_quad = nullptr;

// ── Animation (idle-combat / standby) ────────────────────────────────────────
// We DON'T re-derive the head/head-gear attach: the game folds it into the quad
// x/y itself (Actor_DrawSprites sums each layer's per-frame Pos[0] anchor diff,
// via Actor_ComputeHeadAttach, into the submit position). So the hook just replays
// Actor_SubmitSpriteQuad's rect math from x/y. We DO drive the pose: this+0x38 =
// animType*8 + dir, this+0x3c = frame index. Standby (combat idle) = animType 4
// => pose 32 (dir 0, front). The frame cycles over ~1.3 s, wrapping on the body
// action's frame count (captured live from the first/body layer's ACT via
// Act_GetActionFrames). All RE'd + adversarially verified (workflow 2026-06-28).
using ActFramesFn = int*(__fastcall*)(void* act, void* edx, unsigned action);
constexpr uintptr_t kActFramesFn = 0x0070f2c0;  // Act_GetActionFrames(act,action)
int   g_portrait_anim = 4;          // chosen animType (0=idle..4=standby..8=dead)
int   g_portrait_dir  = 0;          // facing direction 0..7 (low 3 bits of pose)
bool  g_portrait_animate = true;    // cycle the action frames (vs freeze frame 0)
bool  g_portrait_garment = false;   // feed the equipped garment/cape (full body)
void* g_cur_actor = nullptr;        // our actor (read pose @+0x38 in the hook)
int   g_body_frame_count = 1;       // frames in the chosen action (for wrap)
int   g_pv_frame_count = 1;         // idem pour l'aperçu (anim marche)
int*  g_frame_dst = &g_body_frame_count;  // cible du comptage (portrait par défaut)
bool  g_first_layer = false;        // first layer of a pass = body (capture count)

// Hook on FUN_00a1b7c0: while WE are rendering the portrait actor, capture each
// layer's atlas texture + UV + geometry and SKIP the native submit (so nothing
// draws into the game scene). All other (game) rendering passes straight through.
void __fastcall Hooked_ActorQuad(void* self, void* edx, int x, int y,
                                 void* p3, void* p4, short* spr_frame,
                                 int* act_layer, float scale, float angle,
                                 unsigned color, void* palette, float p11) {
  if (!g_cap_active) {
    g_orig_actor_quad(self, edx, x, y, p3, p4, spr_frame, act_layer, scale,
                      angle, color, palette, p11);
    return;
  }
  __try {
    if (spr_frame && act_layer) {
      const int off = g_imgui_dx7_active ? kCTexOffDX7 : kCTexOffDX9;
      void*  native = nullptr;
      ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);

      if (act_layer[8] == 0) {
        // Palette/atlas sprite (e.g. the BODY): resolve via the sprite atlas;
        // UVs are the atlas sub-rect (geom[3..6]).
        int geom[12] = {0};
        void* atlas = reinterpret_cast<void*>(
            *reinterpret_cast<uintptr_t*>(kSceneCtxPtr) + 0xc0);
        void* ctex = reinterpret_cast<AtlasGetFn>(kAtlasGetFn)(
            atlas, nullptr, static_cast<int>(reinterpret_cast<intptr_t>(spr_frame)),
            static_cast<int>(reinterpret_cast<intptr_t>(palette)), geom);
        if (ctex) {
          native = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctex) + off);
          uv0 = ImVec2(*reinterpret_cast<float*>(&geom[3]),
                       *reinterpret_cast<float*>(&geom[4]));
          uv1 = ImVec2(*reinterpret_cast<float*>(&geom[5]),
                       *reinterpret_cast<float*>(&geom[6]));
        }
      } else {
        // RGBA sprite (e.g. the HEAD/face + hair): the frame references its own
        // CTexture directly at byte +8 (spr_frame is short* → +4 shorts). UVs =
        // sprite size / texture size (mirrors FUN_00a1b7c0's else branch). This
        // is why head/hair used to vanish — our capture only handled type 0.
        void* ctex = *reinterpret_cast<void**>(spr_frame + 4);
        if (ctex) {
          native = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctex) + off);
          const int tw = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctex) + 0xc);
          const int th = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctex) + 0x10);
          if (tw > 0 && th > 0)
            uv1 = ImVec2(static_cast<float>(spr_frame[0]) / static_cast<float>(tw),
                         static_cast<float>(spr_frame[1]) / static_cast<float>(th));
        }
      }

      // Quad rect — replay Actor_SubmitSpriteQuad's own math (0x00a1b7c0). The
      // x/y passed here ALREADY carry the head/head-gear attach (the game sums the
      // per-frame Pos[0] anchor diffs into the submit position), so we just place
      // each layer at (x,y) + its sprite offsets. act_layer[5]/[6] are per-layer
      // scale FLOATS stored in int slots (reinterpret, don't int-cast). spr_frame
      // [0..3] = tileW, tileH, tilesX-1, tilesY-1; +0.5 = half-texel bias.
      const float sX = *reinterpret_cast<float*>(&act_layer[5]);
      const float sY = *reinterpret_cast<float*>(&act_layer[6]);
      const int fullW = (spr_frame[2] + 1) * static_cast<int>(spr_frame[0]);
      const int fullH = (spr_frame[3] + 1) * static_cast<int>(spr_frame[1]);
      const float left   = static_cast<float>(x) +
                           static_cast<float>(act_layer[0]) * scale * sX;
      const float right  = static_cast<float>(x) +
                           static_cast<float>(fullW + act_layer[0] - 1) * scale * sX;
      const float top    = static_cast<float>(y) +
                           static_cast<float>(act_layer[1]) * scale * sY;
      const float bottom = static_cast<float>(y) +
                           static_cast<float>(fullH - 1 + act_layer[1]) * scale * sY;
      const float cx = (left + right) * 0.5f + 0.5f;
      const float cy = (top + bottom) * 0.5f + 0.5f;
      const float cw = right - left;
      const float ch = bottom - top;

      // First layer of the pass = body: capture its standby frame count so the
      // next pass can wrap the frame index. p3 = this layer's ACT object.
      if (g_first_layer && p3 && g_cur_actor) {
        g_first_layer = false;
        const unsigned pose = *reinterpret_cast<unsigned*>(
            reinterpret_cast<char*>(g_cur_actor) + 0x38);
        int* fr = reinterpret_cast<ActFramesFn>(kActFramesFn)(p3, nullptr, pose);
        if (fr) {
          const int n = static_cast<int>((fr[1] - fr[0]) / 0x44);
          if (n > 0) *g_frame_dst = n;
        }
      }

      // Diagnostics: record every layer reaching the hook.
      if (g_diag_count < 48) {
        DiagLayer& d = g_diag[g_diag_count++];
        d.type   = act_layer[8];
        d.w      = spr_frame[0];
        d.h      = spr_frame[1];
        d.branch = (act_layer[8] == 0) ? 0 : 1;
        d.tex    = (native != nullptr);
        d.cx     = cx;
        d.cy     = cy;
        d.ox     = act_layer[0];
        d.oy     = act_layer[1];
        d.rawx   = static_cast<float>(x);  // game x/y (carries attach)
        d.rawy   = static_cast<float>(y);
      }

      if (native && *g_cap_num < 48) {
        CapLayer& L = g_cap_buf[(*g_cap_num)++];
        L.tex    = native;
        L.uv0    = uv0;
        L.uv1    = uv1;
        L.w      = cw;
        L.h      = ch;
        L.cx     = cx;
        L.cy     = cy;
        L.mirror = (act_layer[3] & 1) != 0;
        // RGBA sprites (act_layer[8]!=0) are the face/hair/head-gears; palette
        // sprites (==0) are the body/garment. Used to frame head vs head+body.
        L.head_region = (act_layer[8] != 0);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  // suppress the native submit while capturing
}

void InstallActorCapture() {
  static bool done = false;
  if (done) return;
  done = true;
  using namespace hooking;
  g_orig_actor_quad = reinterpret_cast<ActorQuadFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kActorQuadFn),
          reinterpret_cast<uint8_t*>(&Hooked_ActorQuad)));
}

// Equipped-headgear view id for an equip slot, with COSTUME precedence: the
// costume item array (session+0x2b30) overrides the general one (session+0x17d0)
// when a costume piece occupies the slot. Each item is 0xf8 bytes; view id @
// +0x70, a unique slot/item tag @ +4 (0 = empty), used by the caller to de-dup.
int EquipHeadgearView(int slot, int* out_tag) {
  const uintptr_t cos = kSession + 0x2b30 + static_cast<uintptr_t>(slot) * 0xf8;
  if (*reinterpret_cast<int*>(cos + 4) != 0) {  // costume present -> precedence
    *out_tag = *reinterpret_cast<int*>(cos + 4);
    return *reinterpret_cast<int*>(cos + 0x70);
  }
  const uintptr_t gen = kSession + 0x17d0 + static_cast<uintptr_t>(slot) * 0xf8;
  *out_tag = *reinterpret_cast<int*>(gen + 4);
  return *reinterpret_cast<int*>(gen + 0x70);
}

// Logs the last capture pass (throttled ~2s). Separate from CapturePortraitActor
// because that fn uses __try/__except (no C++ object unwinding allowed there).
void LogPortraitDiag() {
  if (!g_portrait_debug) return;  // opt-in (off by default — no log spam)
  static DWORD last = 0;
  const DWORD now = GetTickCount();
  if (now - last < 2000) return;
  last = now;
  LogDiag("[Portrait] pass: {} layers hit hook, {} captured (hair={})",
          g_diag_count, g_cap_count, *reinterpret_cast<int*>(kHair));
  LogDiag("[Portrait] pose=standby(32) frames={}", g_body_frame_count);
  for (int i = 0; i < g_diag_count; ++i) {
    const DiagLayer& d = g_diag[i];
    LogDiag("[Portrait]   layer[{}] type={} {}x{} tex={} off=({},{}) xy=({},{}) -> centre=({},{})",
            i, d.type, d.w, d.h, d.tex ? "ok" : "FAIL", d.ox, d.oy,
            static_cast<int>(d.rawx), static_cast<int>(d.rawy),
            static_cast<int>(d.cx), static_cast<int>(d.cy));
  }
}

// Renders the player's character once with the capture hook active, filling
// g_caps[]. SEH-guarded — a failure leaves g_cap_count at 0 (placeholder shown).
void CapturePortraitActor() {
  InstallActorCapture();
  if (!g_orig_actor_quad) return;
  void* render_ctx = *reinterpret_cast<void**>(kRenderCtxPtr);
  if (!render_ctx) return;  // need a valid UIWindow as render context

  g_cap_count = 0;
  g_diag_count = 0;
  g_first_layer = true;  // first layer this pass = body (capture frame count)
  __try {
    using GetSexFn = int(__fastcall*)(void*, void*);
    using GetJobFn = int(__fastcall*)(void*, void*);
    const int sex = reinterpret_cast<GetSexFn>(kGetSex)(
        reinterpret_cast<void*>(kSession), nullptr);
    const int job = reinterpret_cast<GetJobFn>(0x00d5b580)(
        reinterpret_cast<void*>(kSession), nullptr);
    const int hair = *reinterpret_cast<int*>(kHair);
    const int clo  = *reinterpret_cast<int*>(kClothesCol);
    const int hc   = *reinterpret_cast<int*>(kHairCol);

    // Equipped-headgear view ids (costume overrides general; see EquipHeadgearView).
    // Slots: 8 = head-mid, 9 = head-low, 0 = head-top (per UIBasicInfoWnd doll
    // FUN_008cf970). De-dup by the +4 tag so an item spanning slots draws once.
    int has8, has9, has0;
    const int view8 = EquipHeadgearView(8, &has8);
    const int view9 = EquipHeadgearView(9, &has9);
    const int view0 = EquipHeadgearView(0, &has0);
    const int hg_mid = (has8 != 0) ? view8 : 0;
    const int hg_low = (has9 != 0 && has9 != has8) ? view9 : 0;
    const int hg_top = (has0 != 0 && has0 != has8 && has0 != has9) ? view0 : 0;

    // __thiscall Actor_Init(this, p1..p19) via __fastcall(this, dummy_edx, ..).
    // Field map RE'd from Actor_DrawFromCharInfo (0x0079ab80): the actor stores a
    // SECOND job copy at this+0x18 (game reads char+0x58). The body layer feeds it
    // to Job_ResolveBodyClass(job, job_body, 1) which returns (job_body - 0xf6e)
    // for 3rd jobs => the body class. If job_body is 0 the body collapses to
    // Novice. So job_body MUST equal the job (NOT a weapon — there is no weapon
    // param in this ctor; weapons are a separate layer system).
    using CtorFn = void*(__fastcall*)(void* self, void* edx, void* render_ctx,
        int x, int y, int sex, int job_short, int job_full, int job_body, int hair,
        int hg_top, int hg_mid, int hg_low, int garment, int p13, int p14,
        int clothes_col, int hair_col, int pose, int frame, int p19);
    using DrawFn = void(__fastcall*)(void* self, void* edx, char param);
    using DtorFn = void(__fastcall*)(void* self, void* edx);

    alignas(8) unsigned char actor[0x200];
    std::memset(actor, 0, sizeof(actor));
    // Layer map (RE'd via Actor_BuildSpriteLayers 0x007ae4e0): case 0/7 = garment
    // (skipped, garment id 0), case 1 = body (job + job_body), case 2 = hair/head
    // (머리, uses hair @+0x1a), cases 3-6 = head-gears top/mid/low (from the +0x80
    // priority map we feed via the hg params here). Body class is resolved from
    // job_body (this+0x18) — see Actor_Init's field map. +0x24/garment stays 0.
    // Chosen animation: pose = animType*8 + dir; when animating, cycle the frame
    // over ~1.3 s, wrapping on the action's frame count (captured by the hook on
    // the previous pass); otherwise freeze on frame 0. this+0x38 = pose (its low
    // 3 bits are the facing dir), +0x3c = frame index.
    const int pose = g_portrait_anim * 8 + (g_portrait_dir & 7);
    const int nframes = g_body_frame_count > 0 ? g_body_frame_count : 1;
    const DWORD kCycleMs = 1300;
    const int frame = (g_portrait_animate && nframes > 1)
        ? static_cast<int>(
              static_cast<unsigned long long>(GetTickCount() % kCycleMs) *
              static_cast<unsigned>(nframes) / kCycleMs)
        : 0;
    // Garment/cape (own-player look global, like hair). Only fed in full-body
    // mode — it's a body-region layer that would clutter the head-only view.
    const int garment = g_portrait_garment
        ? *reinterpret_cast<int*>(kGarmentView) : 0;
    reinterpret_cast<CtorFn>(kActorCtor)(
        actor, nullptr, render_ctx, /*x*/ 0, /*y*/ 0, sex, job & 0xffff, job,
        /*job_body*/ job & 0xffff, hair, hg_top, hg_mid, hg_low, garment,
        /*p13*/ 0, /*p14*/ 0, clo, hc, /*pose*/ pose, /*frame*/ frame,
        /*p19*/ 0);

    g_cur_actor = actor;  // hook reads the pose @+0x38 to size the frame loop
    g_cap_active = true;
    reinterpret_cast<DrawFn>(kActorDraw)(actor, nullptr, 1);  // 1 => quad path
    g_cap_active = false;
    g_cur_actor = nullptr;

    reinterpret_cast<DtorFn>(kActorDtor)(actor, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_cap_active = false;
  }
  LogPortraitDiag();  // throttled diagnostics (outside __try — uses fmt/strings)
}

// ── Aperçu d'équipement (mouseover) ──────────────────────────────────────────
// Réutilise le moteur de capture ci-dessus, mais en injectant le viewID de l'item
// prévisualisé dans le bon slot de tête (mapping de UICostumePreviewWnd OnMsg 0x17)
// au lieu de l'équipement porté. Rend le perso portant SEULEMENT l'item (base +
// item), comme la fenêtre d'aperçu native. Slot ARME absent d'Actor_Init (le natif
// non plus ne preview que tête/garment).
enum PvSlot { PV_NONE, PV_TOP, PV_MID, PV_LOW, PV_GARMENT };
PvSlot MapEmplacementToSlot(int emp) {
  switch (emp) {
    case 0x100: case 0x400: case 0x1400: case 0x300: case 0xc00: return PV_TOP;
    case 0x200: case 0x800: case 0x201: case 0x1800:            return PV_MID;
    case 0x1:   case 0x1000: case 0x301: case 0x1c00:           return PV_LOW;
    case 0x4:   case 0x2000:                                    return PV_GARMENT;
    default:                                                    return PV_NONE;
  }
}

// Capture le perso (apparence live) portant l'item (viewID dans le slot), pose de
// face statique, dans g_caps[]. SEH-gardé (g_cap_count=0 si échec).
void CaptureItemPreviewActor(int view_id, PvSlot slot, int dir) {
  InstallActorCapture();
  if (!g_orig_actor_quad) return;
  void* render_ctx = *reinterpret_cast<void**>(kRenderCtxPtr);
  if (!render_ctx) return;
  g_pv_count = 0;
  g_cap_buf = g_pv_caps;   // rediriger le hook vers le buffer aperçu (pas g_caps)
  g_cap_num = &g_pv_count;
  g_frame_dst = &g_pv_frame_count;  // comptage de frames -> buffer aperçu
  g_diag_count = 0;
  g_first_layer = true;   // capturer le nb de frames de l'anim marche
  __try {
    using GetSexFn = int(__fastcall*)(void*, void*);
    using GetJobFn = int(__fastcall*)(void*, void*);
    const int sex = reinterpret_cast<GetSexFn>(kGetSex)(
        reinterpret_cast<void*>(kSession), nullptr);
    const int job = reinterpret_cast<GetJobFn>(0x00d5b580)(
        reinterpret_cast<void*>(kSession), nullptr);
    const int hair = *reinterpret_cast<int*>(kHair);
    const int clo  = *reinterpret_cast<int*>(kClothesCol);
    const int hc   = *reinterpret_cast<int*>(kHairCol);
    const int hg_top  = (slot == PV_TOP) ? view_id : 0;
    const int hg_mid  = (slot == PV_MID) ? view_id : 0;
    const int hg_low  = (slot == PV_LOW) ? view_id : 0;
    const int garment = (slot == PV_GARMENT) ? view_id : 0;
    using CtorFn = void*(__fastcall*)(void* self, void* edx, void* render_ctx,
        int x, int y, int sex, int job_short, int job_full, int job_body, int hair,
        int hg_top, int hg_mid, int hg_low, int garment, int p13, int p14,
        int clothes_col, int hair_col, int pose, int frame, int p19);
    using DrawFn = void(__fastcall*)(void* self, void* edx, char param);
    using DtorFn = void(__fastcall*)(void* self, void* edx);
    alignas(8) unsigned char actor[0x200];
    std::memset(actor, 0, sizeof(actor));
    // Carrousel d'animations (~2.5s chacune) : marche -> idle -> assis. dir via
    // molette. (anim : 0=idle, 1=marche, 2=assis — ajustable si l'index diffère.)
    static const int kAnims[3] = {1, 0, 2};
    const int anim = kAnims[(GetTickCount() / 2500u) % 3u];
    const int pose = anim * 8 + (dir & 7);
    // Cyclage des frames de l'anim courante (~600ms/cycle). g_pv_frame_count est
    // capturé par le hook au 1er passage (0 au tout premier -> frame 0, 1 fr lag).
    const int nf = g_pv_frame_count > 0 ? g_pv_frame_count : 1;
    const int frame = static_cast<int>(
        static_cast<unsigned long long>(GetTickCount() % 600u) *
        static_cast<unsigned>(nf) / 600u);
    reinterpret_cast<CtorFn>(kActorCtor)(
        actor, nullptr, render_ctx, 0, 0, sex, job & 0xffff, job,
        job & 0xffff, hair, hg_top, hg_mid, hg_low, garment, 0, 0, clo, hc,
        pose, frame, 0);
    g_cur_actor = actor;
    g_cap_active = true;
    reinterpret_cast<DrawFn>(kActorDraw)(actor, nullptr, 1);
    g_cap_active = false;
    g_cur_actor = nullptr;
    reinterpret_cast<DtorFn>(kActorDtor)(actor, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { g_cap_active = false; }
  g_cap_buf = g_caps;      // restaurer la cible portrait (toujours)
  g_cap_num = &g_cap_count;
  g_frame_dst = &g_body_frame_count;
}
}  // namespace

void BasicInfoTweaks::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
}

// Tooltip d'aperçu d'un équipement porté par le perso (appelé par item_desc au
// survol de « ViewID : N »). Capture le perso + l'item (viewID dans le slot) via
// le moteur du portrait, puis composite les sprites dans un tooltip ImGui. Ne
// fait rien si l'item n'est pas un headgear/garment (slot PV_NONE).
bool BasicInfoTweaks::CanPreview(int emplacement) const {
  return MapEmplacementToSlot(emplacement) != PV_NONE;
}

void BasicInfoTweaks::RenderItemPreviewTooltip(int view_id, int emplacement) {
  if (view_id == 0) return;
  const PvSlot slot = MapEmplacementToSlot(emplacement);
  if (slot == PV_NONE) return;
  // Molette (pendant le survol) = rotation du perso (dir 0..7).
  static int s_dir = 0;
  const float wheel = ImGui::GetIO().MouseWheel;
  if (wheel != 0.0f) {
    s_dir = (s_dir + (wheel > 0.0f ? 1 : 7)) & 7;  // +1 / -1 avec wrap
    ImGui::GetIO().MouseWheel = 0.0f;  // consommer -> pas de scroll de fenêtre
  }
  CaptureItemPreviewActor(view_id, slot, s_dir);
  if (g_pv_count <= 0) return;
  // Régions TÊTE (visage/cheveux/hat, RGBA) et CORPS (palette), + bbox totale
  // (pour l'échelle). head_region posé par le hook (act_layer[8]!=0).
  float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
  float hx0 = 1e9f, hy0 = 1e9f, hx1 = -1e9f, hy1 = -1e9f;
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  bool has_head = false, has_body = false;
  for (int i = 0; i < g_pv_count; ++i) {
    const CapLayer& L = g_pv_caps[i];
    const float lx0 = L.cx - L.w * 0.5f, lx1 = L.cx + L.w * 0.5f;
    const float ly0 = L.cy - L.h * 0.5f, ly1 = L.cy + L.h * 0.5f;
    if (lx0 < minx) minx = lx0;  if (lx1 > maxx) maxx = lx1;
    if (ly0 < miny) miny = ly0;  if (ly1 > maxy) maxy = ly1;
    if (L.head_region) {
      has_head = true;
      if (lx0 < hx0) hx0 = lx0;  if (lx1 > hx1) hx1 = lx1;
      if (ly0 < hy0) hy0 = ly0;  if (ly1 > hy1) hy1 = ly1;
    } else {
      has_body = true;
      if (lx0 < bx0) bx0 = lx0;  if (lx1 > bx1) bx1 = lx1;
      if (ly0 < by0) by0 = ly0;  if (ly1 > by1) by1 = ly1;
    }
  }
  const float bw = maxx - minx, bh = maxy - miny;
  if (bw <= 1.0f || bh <= 1.0f) return;
  // Échelle + ancrage FIGÉS (calculés 1x, gardés) : le focal ne suit PLUS la bbox
  // par frame (qui saute avec les membres ET les accessoires animés/volants comme
  // l'oiseau, qui sont dans la « région tête »). Ancrage sur le CORPS seul
  // (torse/jambes = noyau stable) : x = centre corps, y = pieds près du bas.
  // -> perso stable, seule l'anim bouge autour.
  static float s_scale = 0.0f, s_fx = 0.0f, s_feet = 0.0f;
  if (s_scale <= 0.0f) {
    // Échelle basée sur la HAUTEUR DU CORPS (pas la bbox totale, qui inclut les
    // costumes larges comme le cat) -> perso à taille CONSTANTE, jamais rétréci.
    const float body_h = has_body ? (by1 - by0) : (maxy - miny);
    s_scale = (body_h > 1.0f) ? (120.0f / body_h) : 1.0f;
    if (has_body)      { s_fx = (bx0 + bx1) * 0.5f; s_feet = by1; }
    else if (has_head) { s_fx = (hx0 + hx1) * 0.5f; s_feet = hy1; }
    else               { s_fx = (minx + maxx) * 0.5f; s_feet = maxy; }
  }
  const float s = s_scale;
  // Boîte large + haute : costumes larges (cat à côté) / hauts (hats) ne sont pas
  // cropés. Fond transparent -> l'espace vide est invisible.
  const float box_w = 260.0f, box_h = 240.0f;
  // Fond + bordure transparents : seul le sprite du perso s'affiche (pas de boîte).
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
  // Ancre le preview PRÈS du curseur (sinon l'offset de tooltip par défaut + la
  // grande boîte l'éloignent). Le sprite est ancré en bas de la boîte (pieds à
  // top+box_h-14) -> on place la boîte au-dessus du curseur pour que le perso
  // apparaisse juste à côté. Régler kPreviewDX (horizontal) / kPreviewDY (vertical,
  // + = plus bas) pour la distance.
  constexpr float kPreviewDX = -16.0f, kPreviewDY = -10.0f;
  const ImVec2 mouse = ImGui::GetMousePos();
  ImGui::SetNextWindowPos(
      ImVec2(mouse.x + kPreviewDX, mouse.y + kPreviewDY - box_h),
      ImGuiCond_Always);
  ImGui::BeginTooltip();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(box_w, box_h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float ox = p0.x + box_w * 0.5f - s_fx * s;      // corps centré horizontalement
  const float oy = p0.y + box_h - 14.0f - s_feet * s;   // pieds près du bas (figé)
  for (int i = 0; i < g_pv_count; ++i) {
    const CapLayer& L = g_pv_caps[i];
    const ImVec2 q0(ox + (L.cx - L.w * 0.5f) * s, oy + (L.cy - L.h * 0.5f) * s);
    const ImVec2 q1(ox + (L.cx + L.w * 0.5f) * s, oy + (L.cy + L.h * 0.5f) * s);
    const ImVec2 u0 = L.mirror ? ImVec2(L.uv1.x, L.uv0.y) : L.uv0;
    const ImVec2 u1 = L.mirror ? ImVec2(L.uv0.x, L.uv1.y) : L.uv1;
    dl->AddImage((ImTextureID)(uintptr_t)L.tex, q0, q1, u0, u1);
  }
  ImGui::EndTooltip();
  ImGui::PopStyleColor(2);
}

float BasicInfoTweaks::SnapValue(float v, float ext, int self_id,
                                 bool y_axis) const {
  float best = v, best_dist = kSnapThreshold;
  for (int j = 0; j < kBarCount; ++j) {
    if (j == self_id || !bars_[j].show) continue;
    const float opos = y_axis ? static_cast<float>(bars_[j].y)
                              : static_cast<float>(bars_[j].x);
    const float oext = y_axis ? static_cast<float>(bars_[j].h)
                              : static_cast<float>(bars_[j].w);
    // align-near, align-far, just-after, just-before
    const float cands[4] = {opos, opos + oext - ext, opos + oext, opos - ext};
    for (int c = 0; c < 4; ++c) {
      float d = cands[c] - v;
      if (d < 0.0f) d = -d;
      if (d < best_dist) { best_dist = d; best = cands[c]; }
    }
  }
  return best;
}

bool BasicInfoTweaks::DrawBar(BarId id, long long cur, long long max) {
  Bar& bar = bars_[id];
  const bool frozen = locked_;

  // Shared alignment grid (owned by MoonlightUi). SnapAxis is a no-op when grid
  // snapping is off, so we can call it unconditionally when present.
  const AlignGrid* grid = nullptr;
  if (auto* mui = Bourgeon::Instance().moonlight_ui()) grid = &mui->grid_;

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBackground |  // we draw our own bg
                           ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoFocusOnAppearing;
  if (frozen) {
    flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoInputs;  // freeze + click-through
  }

  // We drive move/resize ourselves (NoMove|NoResize) and pin the window exactly
  // to the stored geometry every frame, so the drawn bar, the hit-test rect and
  // the drag handle never desync (no fighting with ImGui's internal move).
  flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
  ImGui::SetNextWindowPos(ImVec2(static_cast<float>(bar.x),
                                 static_cast<float>(bar.y)), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(bar.w),
                                  static_cast<float>(bar.h)), ImGuiCond_Always);

  // Allow very small bars: ImGui's default 32x32 window minimum AND the corner
  // rounding both impose a height floor, so drop them for these windows.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(8.0f, 8.0f));

  bool changed = false;
  if (ImGui::Begin(kSrc[id].win_id, nullptr, flags)) {
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    int hl_edges = 0;  // edge(s) to highlight this frame (hover/drag feedback)

    // Custom move/resize via one full-window invisible button.  The grab spot
    // decides the mode: a window edge (within kEdge, or the generous bottom-right
    // corner grip) = resize that edge/corner, anywhere else = move.  We update the
    // stored geometry (which pins the window next frame), and snap the moved
    // position to other bars — graphics + hit-test stay in lockstep.
    if (!frozen) {
      const float kGrip = 12.0f;  // generous bottom-right corner grab zone
      const float kEdge = 5.0f;   // edge grab thickness
      ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
      ImGui::InvisibleButton("##bihandle", sz);
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float rx = p0.x + sz.x, by = p0.y + sz.y;  // right / bottom edges

      // Edge hit-test under the cursor, shared by activation and the hover
      // highlight (no OS resize cursor here — the game draws its own).
      int hov = 0;
      if (m.x <= p0.x + kEdge) hov |= kEdgeL;
      if (m.x >= rx   - kEdge) hov |= kEdgeR;
      if (m.y <= p0.y + kEdge) hov |= kEdgeT;
      if (m.y >= by   - kEdge) hov |= kEdgeB;
      // Keep the original bottom-right corner grip generous.
      if (m.x >= rx - kGrip && m.y >= by - kGrip) hov |= kEdgeR | kEdgeB;

      if (ImGui::IsItemActivated()) {
        drag_edges_ = hov;
        drag_mode_  = hov ? 2 : 1;
        if (hov) {  // resize: offset from the grabbed edge so it tracks the cursor
          drag_off_x_ = (hov & kEdgeL) ? m.x - p0.x : (hov & kEdgeR) ? m.x - rx : 0.0f;
          drag_off_y_ = (hov & kEdgeT) ? m.y - p0.y : (hov & kEdgeB) ? m.y - by : 0.0f;
        } else {  // move: offset from the top-left
          drag_off_x_ = m.x - p0.x;
          drag_off_y_ = m.y - p0.y;
        }
      }
      // Highlight the dragged edges while resizing, else the hovered edge(s).
      if (ImGui::IsItemActive() && drag_mode_ == 2) hl_edges = drag_edges_;
      else if (ImGui::IsItemHovered())              hl_edges = hov;

      if (ImGui::IsItemActive()) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        if (drag_mode_ == 1) {  // move
          float nx = m.x - drag_off_x_, ny = m.y - drag_off_y_;
          if (sticky_) {
            nx = SnapValue(nx, static_cast<float>(bar.w), id, false);
            ny = SnapValue(ny, static_cast<float>(bar.h), id, true);
          }
          // Snap the top-left corner to the visible grid lines.
          if (grid) { nx = grid->SnapAxis(nx, ds.x); ny = grid->SnapAxis(ny, ds.y); }
          const int ix = static_cast<int>(nx + 0.5f);
          const int iy = static_cast<int>(ny + 0.5f);
          if (ix != bar.x || iy != bar.y) {
            bar.x = ix; bar.y = iy; changed = true;
          }
        } else if (drag_mode_ == 2) {  // resize the grabbed edge(s)/corner
          // Move only the grabbed edges; the opposite ones stay pinned. Each
          // dragged edge snaps to the visible grid (lands on a line), then the
          // x/y/w/h fall out of the four edge positions.
          float left = p0.x, right = rx, top = p0.y, bottom = by;
          const float kMin = 5.0f;  // minimum bar width/height (px)
          if (drag_edges_ & kEdgeL) {
            left = m.x - drag_off_x_;
            if (grid) left = grid->SnapAxis(left, ds.x);
            if (left > right - kMin) left = right - kMin;
          } else if (drag_edges_ & kEdgeR) {
            right = m.x - drag_off_x_;
            if (grid) right = grid->SnapAxis(right, ds.x);
            if (right < left + kMin) right = left + kMin;
          }
          if (drag_edges_ & kEdgeT) {
            top = m.y - drag_off_y_;
            if (grid) top = grid->SnapAxis(top, ds.y);
            if (top > bottom - kMin) top = bottom - kMin;
          } else if (drag_edges_ & kEdgeB) {
            bottom = m.y - drag_off_y_;
            if (grid) bottom = grid->SnapAxis(bottom, ds.y);
            if (bottom < top + kMin) bottom = top + kMin;
          }
          const int ix = static_cast<int>(left   + 0.5f);
          const int iy = static_cast<int>(top    + 0.5f);
          const int nw = static_cast<int>(right  - left + 0.5f);
          const int nh = static_cast<int>(bottom - top  + 0.5f);
          if (ix != bar.x || iy != bar.y || nw != bar.w || nh != bar.h) {
            bar.x = ix; bar.y = iy; bar.w = nw; bar.h = nh; changed = true;
          }
        }
      }
    }

    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = rounding_;  // AddRect* clamps to half-dimension

    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(
        ImVec4(bg_color_[0], bg_color_[1], bg_color_[2], bg_color_[3]));
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(
        ImVec4(bar.fill[0], bar.fill[1], bar.fill[2], bar.fill[3]));
    const ImU32 border = IM_COL32(0, 0, 0, 160);

    const float f = ExpFrac(cur, max);

    dl->AddRectFilled(p0, p1, bg, rounding);
    // Fill: skip when essentially empty (avoids a rounded sliver at 0%), and
    // round only the trailing corners so the progress front stays a clean edge.
    if (vertical_) {
      const float fillpx = (p1.y - p0.y) * f;  // fill upward from bottom
      if (fillpx >= 1.0f) {
        const ImDrawFlags fl = (f >= 0.999f) ? ImDrawFlags_RoundCornersAll
                                             : ImDrawFlags_RoundCornersBottom;
        dl->AddRectFilled(ImVec2(p0.x, p1.y - fillpx), p1, fill, rounding, fl);
      }
    } else {
      const float fillpx = (p1.x - p0.x) * f;  // fill rightward from left
      if (fillpx >= 1.0f) {
        const ImDrawFlags fl = (f >= 0.999f) ? ImDrawFlags_RoundCornersAll
                                             : ImDrawFlags_RoundCornersLeft;
        dl->AddRectFilled(p0, ImVec2(p0.x + fillpx, p1.y), fill, rounding, fl);
      }
    }
    if (border_) dl->AddRect(p0, p1, border, rounding);

    if (text_mode_ != 0) {
      const char* label = kSrc[id].label;
      char buf[96];
      if (text_mode_ == 1)
        std::snprintf(buf, sizeof(buf), "%s %.2f%%", label, f * 100.0f);
      else if (text_mode_ == 2)
        std::snprintf(buf, sizeof(buf), "%s %lld / %lld", label, cur, max);
      else
        std::snprintf(buf, sizeof(buf), "%s %lld / %lld (%.2f%%)", label, cur,
                      max, f * 100.0f);
      const ImVec2 ts = ImGui::CalcTextSize(buf);
      const ImVec2 tp((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f);
      dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, 200), buf);
      dl->AddText(tp, IM_COL32(255, 255, 255, 255), buf);
    }

    // Faint resize hint in the bottom-right corner (unlocked only).
    if (!frozen) {
      const float kGrip = 12.0f;
      dl->AddTriangleFilled(ImVec2(p1.x, p1.y - kGrip), ImVec2(p1.x, p1.y),
                            ImVec2(p1.x - kGrip, p1.y),
                            IM_COL32(255, 255, 255, 70));
    }

    // Edge-resize feedback: glow the hovered/dragged edge(s) (a corner lights
    // two). Replaces the OS resize cursor, which the game's own cursor hides.
    if (hl_edges) {
      const ImU32 hc = IM_COL32(255, 220, 80, 210);
      const float t  = 2.0f;
      if (hl_edges & kEdgeL) dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p1.y), hc, t);
      if (hl_edges & kEdgeR) dl->AddLine(ImVec2(p1.x, p0.y), ImVec2(p1.x, p1.y), hc, t);
      if (hl_edges & kEdgeT) dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y), hc, t);
      if (hl_edges & kEdgeB) dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), hc, t);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(4);
  return changed;
}

namespace {
// Returns the current text content for a portrait element (empty string for the
// head element, which draws a sprite placeholder instead of text).
void PortraitText(int id, char* out, size_t n) {
  switch (id) {
    case BasicInfoTweaks::kPortName: {
      const std::string nm = Bourgeon::Instance().client().session().GetCharName();
      std::snprintf(out, n, "%s", nm.empty() ? "?" : nm.c_str());
      break;
    }
    case BasicInfoTweaks::kPortClass: {
      const char* cls = ClassName();
      std::snprintf(out, n, "%s", (cls && cls[0]) ? cls : "");
      break;
    }
    case BasicInfoTweaks::kPortLevel:
      // base/job merged, simply "%d/%d" as requested.
      std::snprintf(out, n, "%d/%d", RDi(kBaseLevel), RDi(kJobLevel));
      break;
    default:
      out[0] = '\0';
      break;
  }
}
}  // namespace

// Connected group of SHOWN portrait elements whose frames touch/overlap `seed`
// (transitive closure, 2px tolerance so edge-adjacent frames count). Returns a
// PortId bitmask. Used by CTRL block-move to drag a cluster of frames as one.
int BasicInfoTweaks::PortraitTouchGroup(int seed) const {
  const int T = 2;  // touch tolerance (px)
  auto touch = [&](const PortraitElem& a, const PortraitElem& b) {
    return a.x - T < b.x + b.w && b.x - T < a.x + a.w &&
           a.y - T < b.y + b.h && b.y - T < a.y + a.h;
  };
  int mask = 1 << seed;
  for (bool added = true; added;) {
    added = false;
    for (int a = 0; a < kPortCount; ++a) {
      if (!(mask & (1 << a))) continue;
      for (int b = 0; b < kPortCount; ++b) {
        if ((mask & (1 << b)) || !ports_[b].show) continue;
        if (touch(ports_[a], ports_[b])) { mask |= (1 << b); added = true; }
      }
    }
  }
  return mask;
}

// Draws one portrait element as a standalone movable/resizable frame: own bg
// colour+opacity, own corner rounding, own text colour.  ImGui owns the move
// (drag anywhere) and resize (bottom-right grip); we pin the window to the
// stored geometry, snap to the shared alignment grid, and read the result back.
// Returns true if the geometry changed this frame.
bool BasicInfoTweaks::DrawPortraitElem(PortId id) {
  PortraitElem& e = ports_[id];
  const bool frozen = portrait_locked_;

  const AlignGrid* grid = nullptr;
  if (auto* mui = Bourgeon::Instance().moonlight_ui()) grid = &mui->grid_;

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoFocusOnAppearing;
  if (frozen) flags |= ImGuiWindowFlags_NoInputs;  // click-through when locked

  ImGui::SetNextWindowPos(ImVec2(static_cast<float>(e.x), static_cast<float>(e.y)),
                          ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(e.w), static_cast<float>(e.h)),
                           ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(8.0f, 8.0f));

  char id_buf[24];
  std::snprintf(id_buf, sizeof(id_buf), "###Port%d", static_cast<int>(id));

  bool changed = false;
  if (ImGui::Begin(id_buf, nullptr, flags)) {
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int hl_edges = 0;

    // Custom move/edge-resize via one full-window invisible button (mirrors the
    // EXP-bar interaction): grab an edge/corner to resize, anywhere else to move;
    // moved/resized edges snap to the alignment grid.
    if (!frozen) {
      const float kGrip = 12.0f, kEdge = 5.0f;
      ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
      ImGui::InvisibleButton("##ph", sz);
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float rx = p0.x + sz.x, by = p0.y + sz.y;
      int hov = 0;
      if (m.x <= p0.x + kEdge) hov |= kEdgeL;
      if (m.x >= rx   - kEdge) hov |= kEdgeR;
      if (m.y <= p0.y + kEdge) hov |= kEdgeT;
      if (m.y >= by   - kEdge) hov |= kEdgeB;
      if (m.x >= rx - kGrip && m.y >= by - kGrip) hov |= kEdgeR | kEdgeB;

      if (ImGui::IsItemActivated()) {
        drag_edges_ = hov;
        drag_mode_  = hov ? 2 : 1;
        if (hov) {
          drag_off_x_ = (hov & kEdgeL) ? m.x - p0.x : (hov & kEdgeR) ? m.x - rx : 0.0f;
          drag_off_y_ = (hov & kEdgeT) ? m.y - p0.y : (hov & kEdgeB) ? m.y - by : 0.0f;
        } else {
          drag_off_x_ = m.x - p0.x;
          drag_off_y_ = m.y - p0.y;
        }
        // CTRL + move = drag every frame touching this one as a rigid block.
        drag_group_mask_ = (!hov && ImGui::GetIO().KeyCtrl)
            ? PortraitTouchGroup(static_cast<int>(id)) : 0;
      }
      if (ImGui::IsItemActive() && drag_mode_ == 2) hl_edges = drag_edges_;
      else if (ImGui::IsItemHovered())              hl_edges = hov;

      if (ImGui::IsItemActive()) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        if (drag_mode_ == 1) {  // move
          float nx = m.x - drag_off_x_, ny = m.y - drag_off_y_;
          if (grid) { nx = grid->SnapAxis(nx, ds.x); ny = grid->SnapAxis(ny, ds.y); }
          const int ix = static_cast<int>(nx + 0.5f), iy = static_cast<int>(ny + 0.5f);
          if (ix != e.x || iy != e.y) {
            const int dx = ix - e.x, dy = iy - e.y;  // per-frame delta
            e.x = ix; e.y = iy; changed = true;
            // CTRL block-move: shift the rest of the touching group by the same
            // delta (snap is applied to the grabbed frame; others follow rigidly).
            const int self_bit = 1 << static_cast<int>(id);
            if (drag_group_mask_ & ~self_bit) {
              for (int j = 0; j < kPortCount; ++j) {
                if (j == static_cast<int>(id) || !(drag_group_mask_ & (1 << j)))
                  continue;
                ports_[j].x += dx; ports_[j].y += dy;
              }
            }
          }
        } else if (drag_mode_ == 2) {  // resize grabbed edge(s)/corner
          float left = p0.x, right = rx, top = p0.y, bottom = by;
          const float kMin = 8.0f;
          if (drag_edges_ & kEdgeL) {
            left = m.x - drag_off_x_;
            if (grid) left = grid->SnapAxis(left, ds.x);
            if (left > right - kMin) left = right - kMin;
          } else if (drag_edges_ & kEdgeR) {
            right = m.x - drag_off_x_;
            if (grid) right = grid->SnapAxis(right, ds.x);
            if (right < left + kMin) right = left + kMin;
          }
          if (drag_edges_ & kEdgeT) {
            top = m.y - drag_off_y_;
            if (grid) top = grid->SnapAxis(top, ds.y);
            if (top > bottom - kMin) top = bottom - kMin;
          } else if (drag_edges_ & kEdgeB) {
            bottom = m.y - drag_off_y_;
            if (grid) bottom = grid->SnapAxis(bottom, ds.y);
            if (bottom < top + kMin) bottom = top + kMin;
          }
          const int ix = static_cast<int>(left + 0.5f), iy = static_cast<int>(top + 0.5f);
          const int nw = static_cast<int>(right - left + 0.5f);
          const int nh = static_cast<int>(bottom - top + 0.5f);
          if (ix != e.x || iy != e.y || nw != e.w || nh != e.h) {
            e.x = ix; e.y = iy; e.w = nw; e.h = nh; changed = true;
          }
        }
      }
    }

    const float rounding = e.rounding;
    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(
        ImVec4(e.bg[0], e.bg[1], e.bg[2], e.bg[3]));
    const ImU32 fg = ImGui::ColorConvertFloat4ToU32(
        ImVec4(e.fg[0], e.fg[1], e.fg[2], e.fg[3]));
    dl->AddRectFilled(p0, p1, bg, rounding);
    if (portrait_border_) dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 160), rounding);

    if (id == kPortHead) {
      // Regenerated head sprite: composite the captured actor layers, fitted to
      // the frame width and top-anchored (so the head fills the top) + clipped.
      if (portrait_head_sprite_ && g_cap_count > 0) {
        // Head-only: skip the body, which is the first (back-most) captured
        // layer. The head/hair/head-gears (the rest) all anchor to the head and
        // compose correctly together — so no body means nothing to misalign to.
        const int start =
            (portrait_head_only_ && g_cap_count > 1) ? 1 : 0;
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;     // drawn
        float hx0 = 1e9f, hy0 = 1e9f, hx1 = -1e9f, hy1 = -1e9f;          // head region
        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;          // body region
        bool has_head = false, has_body = false;
        for (int i = start; i < g_cap_count; ++i) {
          const CapLayer& L = g_caps[i];
          const float lx0 = L.cx - L.w * 0.5f, lx1 = L.cx + L.w * 0.5f;
          const float ly0 = L.cy - L.h * 0.5f, ly1 = L.cy + L.h * 0.5f;
          if (lx0 < minx) minx = lx0;  if (lx1 > maxx) maxx = lx1;
          if (ly0 < miny) miny = ly0;  if (ly1 > maxy) maxy = ly1;
          if (L.head_region) {
            has_head = true;
            if (lx0 < hx0) hx0 = lx0;  if (lx1 > hx1) hx1 = lx1;
            if (ly0 < hy0) hy0 = ly0;  if (ly1 > hy1) hy1 = ly1;
          } else {
            has_body = true;
            if (lx0 < bx0) bx0 = lx0;  if (lx1 > bx1) bx1 = lx1;
            if (ly0 < by0) by0 = ly0;  if (ly1 > by1) by1 = ly1;
          }
        }
        const float bw = maxx - minx, bh = maxy - miny;
        if (bw > 1.0f && bh > 1.0f) {
          // Anchor the zoom on the bbox CENTRE so zooming in/out stays centred in
          // the frame (offx/offy = where that centre lands, 0.5/0.5 = middle) and
          // clip. Zoom + focus offsets are live-tunable sliders.
          const float availW = sz.x - 4.0f;
          const float s = (availW / bw) * portrait_head_zoom_;
          // Focus point: head-only -> the head centre; head+body -> the MEDIAN of
          // the head centre and the body centre (a flattering upper-body framing).
          float focusX = (minx + maxx) * 0.5f;
          float focusY = (miny + maxy) * 0.5f;
          if (!portrait_head_only_ && has_head && has_body) {
            focusX = (hx0 + hx1 + bx0 + bx1) * 0.25f;  // (headCx + bodyCx)/2
            focusY = (hy0 + hy1 + by0 + by1) * 0.25f;  // (headCy + bodyCy)/2
          } else if (has_head) {
            focusX = (hx0 + hx1) * 0.5f;               // head centre
            focusY = (hy0 + hy1) * 0.5f;
          }
          // Map the focus to the FRAME centre so the zoom always pivots centred;
          // offx/offy are a delta FROM centre (0 = centred, ±0.5 = ±half a frame).
          // This keeps zoom in/out stable regardless of the offsets.
          const float ox = p0.x + sz.x * (0.5f + portrait_head_offx_) - focusX * s;
          const float oy = p0.y + sz.y * (0.5f + portrait_head_offy_) - focusY * s;
          dl->PushClipRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                           ImVec2(p1.x - 1.0f, p1.y - 1.0f), true);
          for (int i = start; i < g_cap_count; ++i) {
            const CapLayer& L = g_caps[i];
            const ImVec2 q0(ox + (L.cx - L.w * 0.5f) * s,
                            oy + (L.cy - L.h * 0.5f) * s);
            const ImVec2 q1(ox + (L.cx + L.w * 0.5f) * s,
                            oy + (L.cy + L.h * 0.5f) * s);
            const ImVec2 u0 = L.mirror ? ImVec2(L.uv1.x, L.uv0.y) : L.uv0;
            const ImVec2 u1 = L.mirror ? ImVec2(L.uv0.x, L.uv1.y) : L.uv1;
            dl->AddImage((ImTextureID)(uintptr_t)L.tex, q0, q1, u0, u1);
          }
          dl->PopClipRect();
        }
      } else {
        const char* ph = " ";
        const ImVec2 ts = ImGui::CalcTextSize(ph);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f),
                    IM_COL32(255, 255, 255, 70), ph);
      }
    } else {
      char buf[96];
      PortraitText(id, buf, sizeof(buf));
      const ImVec2 ts = ImGui::CalcTextSize(buf);
      const ImVec2 tp((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f);
      dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, 200), buf);
      dl->AddText(tp, fg, buf);
    }

    if (!frozen) {  // faint resize hint in the bottom-right corner
      const float kGrip = 12.0f;
      dl->AddTriangleFilled(ImVec2(p1.x, p1.y - kGrip), ImVec2(p1.x, p1.y),
                            ImVec2(p1.x - kGrip, p1.y), IM_COL32(255, 255, 255, 70));
    }
    if (hl_edges) {  // edge-resize feedback (the game hides the OS resize cursor)
      const ImU32 hc = IM_COL32(255, 220, 80, 210);
      const float t = 2.0f;
      if (hl_edges & kEdgeL) dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p1.y), hc, t);
      if (hl_edges & kEdgeR) dl->AddLine(ImVec2(p1.x, p0.y), ImVec2(p1.x, p1.y), hc, t);
      if (hl_edges & kEdgeT) dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y), hc, t);
      if (hl_edges & kEdgeB) dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), hc, t);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(4);
  return changed;
}

// Draws every shown portrait element (each its own movable frame), persisting
// the layout once on drag-end.
void BasicInfoTweaks::DrawPortrait() {
  if (!portrait_visible_) return;
  // Session globals are only populated once a character is in the world.
  if (Bourgeon::Instance().client().session().aid() == 0) return;

  // The head sprite is (re)captured in OnTick (game update phase — a safer
  // context to call the actor renderer than the Present hook); here we just draw
  // the latest g_caps.
  bool changed = false;
  for (int i = 0; i < kPortCount; ++i) {
    if (!ports_[i].show) continue;
    changed |= DrawPortraitElem(static_cast<PortId>(i));
  }
  if (changed) portrait_drag_pending_ = true;
  if (portrait_drag_pending_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    portrait_drag_pending_ = false;
    geometry_dirty_ = true;  // drained by MoonlightUi (saves the yaml)
  }
}

void BasicInfoTweaks::OnRenderUI() {
  if (!in_game_) return;
  if (HudReplaced()) return;  // world map / full-screen UI replaces the HUD
  // The alignment grid is drawn by MoonlightUi (shared overlay), not here.

  DrawPortrait();  // independent of the EXP-bar master toggle below

  if (!visible_) return;
  // Globals are only populated once a character is in the world.
  if (Bourgeon::Instance().client().session().aid() == 0) return;

  bool changed = false;
  for (int i = 0; i < kBarCount; ++i) {
    if (!bars_[i].show) continue;
    const long long cur = RDval(kSrc[i].cur, kSrc[i].wide);
    const long long max = RDval(kSrc[i].max, kSrc[i].wide);
    changed |= DrawBar(static_cast<BarId>(i), cur, max);
  }

  force_apply_ = false;  // one-shot: consumed after all bars drew

  if (changed) drag_pending_ = true;
  // Persist exactly once, when the user releases the mouse after moving/resizing.
  if (drag_pending_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    drag_pending_   = false;
    geometry_dirty_ = true;  // drained by MoonlightUi (saves the YAML)
  }
}

namespace {
// ── Hide native Basic Info PRE-RENDER via its msg-0x22 handler ───────────────
// UIBasicInfoWnd (id 0) vtable 0x0103e35c. Its OnMsg is vtable+0x94; MakeWindow's
// id-0 case calls it with msg 0x22 (layout-restore) DURING creation, before the
// first frame — so hiding there avoids the login flicker of the OnTick approach.
//
// We hide by moving it OFF-SCREEN via a raw +0x1c/+0x20 write. This is what kills
// the native dock/snap "ghost" (a hidden-in-place window is still a snap target;
// an off-screen one is not — live-verified). Crucially it does NOT corrupt the
// saved position: BASICINFOWNDINFO.X/Y persist from a SEPARATE store in the window
// manager (mgr+0x514), which a raw field write never touches — LIVE-VERIFIED via
// x32dbg: live pos -> -10000 while mgr+0x514 stayed (0,0). The real pos is captured
// before the override so it can be restored if the option is turned off.
// Same handler ABI as Equip/Status (ret 0x18, this in ECX). Vtable-slot patch = safe.
constexpr uintptr_t kBIMsgSlot    = 0x0103e35c + 0x94;  // vtable+0x94 OnMsg slot
constexpr int       kBIWinX       = 0x1c;
constexpr int       kBIWinY       = 0x20;
constexpr int       kBIOffScreen  = -10000;
constexpr int       kBIMsgRestore = 0x22;
constexpr int       kBIUnset      = static_cast<int>(0x80000000);  // "no saved pos yet"
using BIMsg_t = int (__fastcall*)(void*, void*, int, int, int, int, int, int);  // ret 0x18
BIMsg_t g_bi_orig_msg = nullptr;
bool    g_bi_hide     = false;               // synced from portrait_hide_basic_info_
int     g_bi_saved_x  = kBIUnset, g_bi_saved_y = kBIUnset;  // real pos, captured once

// Move Basic Info off-screen via a raw field write (NOT SetPos — a raw write does
// not sync back to the persisted mgr+0x514, so the save stays intact). Captures the
// real on-screen position the first time so it can be restored.
inline void BIPinOffscreen(void* w) {
  int* px = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(w) + kBIWinX);
  int* py = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(w) + kBIWinY);
  if (*px > kBIOffScreen + 5000) { g_bi_saved_x = *px; g_bi_saved_y = *py; }
  *px = kBIOffScreen;
  *py = kBIOffScreen;
}

int __fastcall BIMsgHook(void* self, void* edx, int p1, int msg, int p3, int p4,
                         int p5, int p6) {
  __try {
    const int r = g_bi_orig_msg(self, edx, p1, msg, p3, p4, p5, p6);
    if (msg == kBIMsgRestore && g_bi_hide) BIPinOffscreen(self);  // pre-render off-screen
    return r;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

template <typename T>
void BIPatchPtr(uintptr_t addr, T val) {
  DWORD old;
  if (VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), PAGE_EXECUTE_READWRITE, &old)) {
    *reinterpret_cast<T*>(addr) = val;
    VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), sizeof(T));
  }
}
}  // namespace

BasicInfoTweaks::BasicInfoTweaks() {
  // Install the msg-0x22 hide hook at DLL load (before any Basic Info is created),
  // so the very first HUD creation at login is caught pre-render (no flicker).
  void* cur = *reinterpret_cast<void**>(kBIMsgSlot);
  if (cur && cur != reinterpret_cast<void*>(&BIMsgHook)) {
    g_bi_orig_msg = reinterpret_cast<BIMsg_t>(cur);
    BIPatchPtr<void*>(kBIMsgSlot, reinterpret_cast<void*>(&BIMsgHook));
  }
}

// Enforces the "Masquer la fenêtre Basic Info d'origine" option using the native
// VISIBILITY (SetVisible), NOT an off-screen move — so nothing corrupts a saved
// position. The msg-0x22 hook above hides it pre-render at creation; here we
// re-hide if the game re-shows it, and restore visibility when the option is off.
//   BasicInfo singleton ptr = *(g_UIWindowMgr 0x0131f4e8 + 0x1dc) (vtable
//   0x0103e35c); null until the HUD is created.
void BasicInfoTweaks::OnTick() {
  // Regenerate the head sprite from the game's actor renderer (update phase is a
  // safer place to call it than the Present hook). DrawPortrait reads g_caps.
  if (portrait_visible_ && portrait_head_sprite_ && ports_[kPortHead].show &&
      Bourgeon::Instance().client().session().aid() != 0) {
    g_portrait_debug = portrait_debug_log_;
    g_portrait_anim = portrait_anim_;
    g_portrait_dir = portrait_dir_;
    g_portrait_animate = portrait_animate_;
    g_portrait_garment = portrait_show_garment_ && !portrait_head_only_;
    CapturePortraitActor();
  } else {
    g_cap_count = 0;
  }

  constexpr uintptr_t kBasicInfoPtr = 0x0131f6c4;  // 0x0131f4e8 + 0x1dc

  g_bi_hide = portrait_hide_basic_info_;  // seen by the pre-render msg-0x22 hook
  void* bi = *reinterpret_cast<void**>(kBasicInfoPtr);
  if (!bi) return;  // HUD not created yet

  if (portrait_hide_basic_info_) {
    BIPinOffscreen(bi);       // keep off-screen (raw write; persist mgr+0x514 untouched)
    bi_pinned_off_ = true;
  } else if (bi_pinned_off_) {
    if (g_bi_saved_x != kBIUnset) {  // restore real pos when the option is turned off
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(bi) + kBIWinX) = g_bi_saved_x;
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(bi) + kBIWinY) = g_bi_saved_y;
    }
    bi_pinned_off_ = false;
  }
}
