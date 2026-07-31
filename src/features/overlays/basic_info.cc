#include "ragnarok/lua.h"
#include "ragnarok/render.h"
#include "ragnarok/globals.h"
#include "ui/game_texture.h"
#include "features/overlays/basic_info.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "ui/window_clamp.h"  // ClampWindowPosToScreen (barres/portrait déplacés à la main)
#include "ui/window_zorder.h"  // HUD maintenu sous les vraies fenêtres

#include "ragnarok/uiwnd.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"  // D3D9_CompositeQuadsRGBA (export GIF avatar)
#include "features/systems/bourgeon_opcodes.h"  // kHatEffectMap (ZC 0x0F17)
#include "features/fx/ez_effect_capture.h"  // capture PARTAGÉE des effets EZ (doll + aperçu)
#include "features/moonlight_ui/moonlight_ui.h"  // shared AlignGrid (snap + draw)
#include "utils/gif_writer.h"       // GifWrite
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

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
  uintptr_t   cur, max;
  long long   max_const;  // if != 0, literal max overriding *max (zeny = INT32 cap)
  bool        wide;       // true = INT64, false = INT32
  bool        grouped;    // thousands-group the amount in the label (zeny)
  const char* label;
  const char* win_id;     // ImGui window id (### keeps it stable)
};
// Zeny (g_PlayerZeny @0x015fba90) and Weight (cur @0x015fbaa0 / max @0x015fba9c)
// confirmed by RE of UIBasicInfoWnd::DrawContent @0x0095e620 — both INT32. Zeny
// has no in-memory max, but the client stores it signed 32-bit, so its bar fills
// relative to the INT32 hard cap (kZenyMax) and shows the amount thousands-grouped.
constexpr long long kZenyMax = 2147483647LL;  // INT32_MAX = client hard zeny cap
const Src kSrc[BasicInfo::kBarCount] = {
  {0x015fb9d0, 0x015fb9d8, 0,        true,  false, "Base",  "###BIBaseExp"},
  {0x015fb9e8, 0x015fb9e0, 0,        true,  false, "Job",   "###BIJobExp"},
  {0x015ff908, 0x015ff90c, 0,        false, false, "HP",    "###BIHp"},
  {0x015ff910, 0x015ff914, 0,        false, false, "SP",    "###BISp"},
  {rag::kZenyAddr, rag::kZenyAddr, kZenyMax, false, true,  "Zeny",  "###BIZeny"},
  {0x015fbaa0, 0x015fba9c, 0,        false, false, "Poids", "###BIWeight"},
};

// Formats a signed integer with thousands separators (1234567 -> "1,234,567")
// into `out` — used for the grouped zeny bar. Bounded; always NUL-terminated.
void GroupInt(long long v, char* out, size_t n) {
  if (n == 0) return;
  char tmp[24];
  std::snprintf(tmp, sizeof(tmp), "%lld", v < 0 ? -v : v);
  const int len = static_cast<int>(std::strlen(tmp));
  const int cap = static_cast<int>(n) - 1;  // room for the NUL
  int oi = 0;
  if (v < 0 && oi < cap) out[oi++] = '-';
  for (int i = 0; i < len; ++i) {
    if (i > 0 && (len - i) % 3 == 0 && oi < cap) out[oi++] = ',';
    if (oi < cap) out[oi++] = tmp[i];
  }
  out[oi] = '\0';
}

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

// ── Status-portrait value sources (20250716 client) ──────────────────────────
constexpr uintptr_t kBaseLevel = 0x015fb9f0;  // DAT_015fb9f0 (UIBasicInfoWnd "Base Lv.")
constexpr uintptr_t kJobLevel  = 0x015fb9f8;  // DAT_015fb9f8 (UIBasicInfoWnd "Job Lv.")

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
      reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
  const char* n = reinterpret_cast<GetClassName_t>(0x00d5bb40)(
      reinterpret_cast<void*>(rag::kSessionAddr), nullptr, static_cast<unsigned>(jobid), -1);
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
  bool   companion;           // true = cart/faucon (sous-acteur) : rendu MAIS exclu de
                              // l'ancrage (ne doit PAS tirer le centre/pieds du corps)
};
CapLayer g_caps[48];
int      g_cap_count   = 0;
bool     g_cap_active  = false;  // set only during our actor render
// Buffer SÉPARÉ pour l'aperçu d'équipement (ne doit PAS écraser g_caps du portrait
// — les 2 rendus cohabitent). Le hook écrit vers la cible active g_cap_buf/g_cap_num.
CapLayer  g_pv_caps[48];
int       g_pv_count = 0;
// Buffer DÉDIÉ à l'avatar plein-corps (character sheet). Comme g_pv_caps il
// cohabite avec g_caps : la capture redirige g_cap_buf/g_cap_num/g_frame_dst
// vers lui puis restaure. g_av_frame_count = nb d'images de l'action (wrap anim,
// 1 frame de lag comme l'aperçu au tout premier passage).
CapLayer  g_av_caps[48];
int       g_av_count = 0;
int       g_av_frame_count = 1;
// Délai natif par image de l'action (float .act delays[0], tableau @actObj+0x12c),
// capturé au 1er layer du corps. ms/image = delay * 25 (base AniTick RO ; delay~4.0
// => ~100 ms). Sert à animer le doll à la VITESSE native (délai constant par image).
float     g_av_frame_delay = 4.0f;
// Scratch pour réordonner arme+bouclier DEVANT/DERRIÈRE le corps selon la direction,
// comme le natif (cf. project_weapon_zorder / WeaponLayer) : FRONT {0,1,6,7} =
// devant (ordre coiffes<arme<bouclier) ; BACK {2,3,4,5} = derrière le corps.
// g_av_wpn_start = index de début du bloc arme/bouclier dans g_av_caps (= body_count),
// posé par EmitWeaponShieldLayers. File-scope (static local + __try => C2712).
CapLayer  g_av_reorder[48];
int       g_av_wpn_start = -1;
// Zoom du corps (FUN_00d7fd30 -> param `scale` du submit) capturé au 1er layer,
// réutilisé pour dessiner l'arme/bouclier à la MÊME échelle que le corps.
float     g_av_body_scale = 1.0f;
// Signature d'apparence (viewids équipés + corps), INVARIANTE par frame d'animation :
// posée par CaptureAvatarActor, lue par RenderPlayerAvatar pour re-figer le cadrage
// quand l'équipement/apparence change (sinon cadrage périmé à équipement changé).
unsigned  g_av_sig = 0;
CapLayer* g_cap_buf = g_caps;
int*      g_cap_num = &g_cap_count;
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

// EmitCapLayer — per-layer capture, factored out of Hooked_ActorQuad so the
// weapon/shield injector (EmitWeaponShieldLayers) can reuse the EXACT same
// texture resolution + quad math. Resolves the layer's DX9 texture (act_layer[8]
// ==0 => sprite-atlas page via SpriteAtlas_GetCachedTexture + geom UVs; else the
// frame's own CTexture at +8), replays Actor_SubmitSpriteQuad's rect math from
// (x,y)+sprite offsets, and pushes ONE CapLayer into the ACTIVE buffer (g_cap_buf/
// g_cap_num, bounded < 48). x/y are the submit position (already carrying any
// body/head attach). p3/angle/color are unused by the capture (kept so the call
// sites read like the native submit). Math is byte-for-byte the old inline code.
void EmitCapLayer(void* p3, short* spr_frame, int* act_layer, float x, float y,
                  float scale, float angle, unsigned color, void* palette) {
  (void)p3; (void)angle; (void)color;
  const int off = g_imgui_dx7_active ? kCTexOffDX7 : kCTexOffDX9;
  void*  native = nullptr;
  ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);

  if (act_layer[8] == 0) {
    // Palette/atlas sprite (BODY, GARMENT, WEAPON, SHIELD): resolve via the
    // sprite atlas; UVs are the atlas sub-rect (geom[3..6]).
    int geom[12] = {0};
    void* atlas = reinterpret_cast<void*>(
        *reinterpret_cast<uintptr_t*>(render::kContextPtr) + 0xc0);
    void* ctex = reinterpret_cast<AtlasGetFn>(render::kAtlasGetCachedAddr)(
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
    // RGBA sprite (HEAD/face + hair): the frame references its own CTexture
    // directly at byte +8 (spr_frame is short* → +4 shorts). UVs = sprite size /
    // texture size (mirrors FUN_00a1b7c0's else branch).
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

  // Quad rect — replay Actor_SubmitSpriteQuad's own math (0x00a1b7c0). x/y already
  // carry the attach; act_layer[5]/[6] are per-layer scale FLOATS stored in int
  // slots (reinterpret, don't int-cast). spr_frame[0..3] = tileW, tileH, tilesX-1,
  // tilesY-1; +0.5 = half-texel bias.
  const float sX = *reinterpret_cast<float*>(&act_layer[5]);
  const float sY = *reinterpret_cast<float*>(&act_layer[6]);
  const int fullW = (spr_frame[2] + 1) * static_cast<int>(spr_frame[0]);
  const int fullH = (spr_frame[3] + 1) * static_cast<int>(spr_frame[1]);
  const float left   = x + static_cast<float>(act_layer[0]) * scale * sX;
  const float right  = x + static_cast<float>(fullW + act_layer[0] - 1) * scale * sX;
  const float top    = y + static_cast<float>(act_layer[1]) * scale * sY;
  const float bottom = y + static_cast<float>(fullH - 1 + act_layer[1]) * scale * sY;
  const float cx = (left + right) * 0.5f + 0.5f;
  const float cy = (top + bottom) * 0.5f + 0.5f;
  const float cw = right - left;
  const float ch = bottom - top;

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
    // RGBA sprites (act_layer[8]!=0) are the face/hair/head-gears; palette sprites
    // (==0) are the body/garment/weapon. Used to frame head vs head+body.
    L.head_region = (act_layer[8] != 0);
    // Par défaut PAS un compagnon (le slot est réutilisé d'une frame à l'autre : il faut
    // effacer un éventuel `true` laissé par un cart/faucon). EmitCompanionLayers le
    // repasse à true APRÈS coup pour ses propres couches.
    L.companion = false;
  }
}

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
      // First layer of the pass = body: capture its standby frame count (so the
      // next pass can wrap the frame index) AND the actor zoom (so injected
      // weapon/shield layers use the SAME scale). p3 = this layer's ACT object.
      if (g_first_layer && p3 && g_cur_actor) {
        g_first_layer = false;
        g_av_body_scale = scale;
        const unsigned pose = *reinterpret_cast<unsigned*>(
            reinterpret_cast<char*>(g_cur_actor) + 0x38);
        int* fr = reinterpret_cast<ActFramesFn>(kActFramesFn)(p3, nullptr, pose);
        if (fr) {
          const int n = static_cast<int>((fr[1] - fr[0]) / 0x44);
          if (n > 0) *g_frame_dst = n;
        }
        // Délai natif par image (tableau floats @act+0x12c..0x130) -> vitesse d'anim.
        int* db = *reinterpret_cast<int**>(reinterpret_cast<char*>(p3) + 0x12c);
        int* de = *reinterpret_cast<int**>(reinterpret_cast<char*>(p3) + 0x130);
        if (db && de != db) {
          const float d = *reinterpret_cast<float*>(db);  // delays[0]
          if (d > 0.0f) g_av_frame_delay = d;
        }
      }
      EmitCapLayer(p3, spr_frame, act_layer, static_cast<float>(x),
                   static_cast<float>(y), scale, angle, color, palette);
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

// Résout les 3 view ids de coiffe (top/mid/low) à afficher, avec :
//  (1) précédence costume par slot (tableau costume session+0x2b30) quand `show_costume` ;
//  (2) suppression NATIVE d'un chapeau RÉEL multi-slot dès qu'un costume occupe l'un de
//      ses slots : un hat qui prend head top+mid+low (même invIndex/tag +4 dans les 3
//      entrées equip) est masqué EN ENTIER par le natif quand un costume prend le head-top
//      — il ne doit pas ressurgir via ses slots mid/low (que le costume, lui, ne couvre
//      pas). Règle : un item réel est masqué si son tag == le tag réel d'un slot
//      actuellement couvert par un costume.
//  (3) de-dup par tag (item multi-slot / costume multi-slot -> une seule couche), en
//      GARDANT la priorité de couche identique au rendu natif capturé (correct hors costume).
// Slots equip (vérifié en jeu) : 0 = head-bot/low, 8 = head-top, 9 = head-mid. Les
// paramètres hg_top/hg_mid/hg_low ne sont que des noms de COUCHE de l'actor ctor : on
// conserve le câblage slot->couche de l'origine (slot8->hg_mid, slot9->hg_low, slot0->hg_top),
// empiriquement correct — la suppression ci-dessus, elle, est symétrique et ne dépend PAS de
// quel slot est « top » (elle opère sur les tags des 3 slots tête).
void ResolveHeadgearViews(bool show_costume, int* hg_top, int* hg_mid, int* hg_low) {
  const int slots[3] = {0, 8, 9};  // ordre de lecture ; [k] -> couche assignée plus bas
  int rtag[3], rview[3], ctag[3], cview[3], etag[3], eview[3];
  for (int k = 0; k < 3; ++k) {
    const uintptr_t gen = rag::kSessionAddr + 0x17d0 + static_cast<uintptr_t>(slots[k]) * 0xf8;
    const uintptr_t cos = rag::kSessionAddr + 0x2b30 + static_cast<uintptr_t>(slots[k]) * 0xf8;
    rtag[k]  = *reinterpret_cast<int*>(gen + 4);
    rview[k] = *reinterpret_cast<int*>(gen + 0x70);
    ctag[k]  = show_costume ? *reinterpret_cast<int*>(cos + 4) : 0;
    cview[k] = *reinterpret_cast<int*>(cos + 0x70);
  }
  // Vue EFFECTIVE par slot : costume prioritaire ; sinon réel s'il n'est pas « couvert »
  // (aucun slot du même item réel n'est pris par un costume).
  for (int k = 0; k < 3; ++k) {
    if (ctag[k] != 0) { etag[k] = ctag[k]; eview[k] = cview[k]; continue; }
    bool covered = false;
    if (rtag[k] != 0)
      for (int j = 0; j < 3; ++j)
        if (ctag[j] != 0 && rtag[j] == rtag[k]) { covered = true; break; }
    if (rtag[k] != 0 && !covered) { etag[k] = rtag[k]; eview[k] = rview[k]; }
    else                          { etag[k] = 0;       eview[k] = 0; }
  }
  // De-dup couche (priorité de l'origine, préservée) : slot 8 d'abord, puis slot 9, puis
  // slot 0 — un item multi-slot à view identique ne rend donc que dans une seule couche.
  *hg_mid = etag[1] ? eview[1] : 0;                                        // slot 8
  *hg_low = (etag[2] && etag[2] != etag[1]) ? eview[2] : 0;                // slot 9
  *hg_top = (etag[0] && etag[0] != etag[1] && etag[0] != etag[2]) ? eview[0] : 0;  // slot 0
}

// Nameid de l'item équipé dans `slot` (arme=1, bouclier=5), lu in-place dans le
// tableau equip (session+0x17d0+slot*0xf8 ; resname std::string SSO @+0x2c ->
// atoi). Renvoie 0 si le slot est vide. SOURCE FIABLE (toujours à jour) pour
// arme/bouclier : les globals g_OwnLook_WeaponViewId (0x280)/ShieldViewId (0x284)
// sont MIS À ZÉRO au char-select (MOVLPD 0x00d2a8aa) — l'arme du login atterrit
// dans WeaponViewId2 (0x288, +0x40ca) et le bouclier n'est JAMAIS seedé ; ils ne
// se peuplent que via un ZC_SPRITE_CHANGE en jeu (cf. World_Spawn... / seed).
int EquipSlotNameId(int slot) {
  __try {
    const uintptr_t e = rag::kSessionAddr + 0x17d0 + static_cast<uintptr_t>(slot) * 0xf8;
    if (*reinterpret_cast<int*>(e + 0x04) == 0 ||   // invIndex 0 = vide
        *reinterpret_cast<int*>(e + 0x10) != 1)     // present != 1
      return 0;
    const unsigned cap = *reinterpret_cast<unsigned*>(e + 0x40);
    const char* rn = (cap > 15) ? *reinterpret_cast<char**>(e + 0x2c)
                                : reinterpret_cast<char*>(e + 0x2c);
    return (rn && rn[0]) ? std::atoi(rn) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Renders the player's character once with the capture hook active, filling
// g_caps[]. SEH-guarded — a failure leaves g_cap_count at 0 (placeholder shown).
void CapturePortraitActor() {
  InstallActorCapture();
  if (!g_orig_actor_quad) return;
  void* render_ctx = *reinterpret_cast<void**>(kRenderCtxPtr);
  if (!render_ctx) return;  // need a valid UIWindow as render context

  g_cap_count = 0;
  g_first_layer = true;  // first layer this pass = body (capture frame count)
  __try {
    using GetSexFn = int(__fastcall*)(void*, void*);
    using GetJobFn = int(__fastcall*)(void*, void*);
    const int sex = reinterpret_cast<GetSexFn>(kGetSex)(
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
    const int job = reinterpret_cast<GetJobFn>(0x00d5b580)(
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
    const int hair = *reinterpret_cast<int*>(kHair);
    const int clo  = *reinterpret_cast<int*>(kClothesCol);
    const int hc   = *reinterpret_cast<int*>(kHairCol);

    // Equipped-headgear view ids (costume precedence + native suppression of a
    // multi-slot real hat covered by a costume; see ResolveHeadgearViews).
    int hg_top, hg_mid, hg_low;
    ResolveHeadgearViews(/*show_costume=*/true, &hg_top, &hg_mid, &hg_low);

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
  g_first_layer = true;   // capturer le nb de frames de l'anim marche
  __try {
    using GetSexFn = int(__fastcall*)(void*, void*);
    using GetJobFn = int(__fastcall*)(void*, void*);
    const int sex = reinterpret_cast<GetSexFn>(kGetSex)(
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
    const int job = reinterpret_cast<GetJobFn>(0x00d5b580)(
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
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

// ── Arme + bouclier sur l'avatar (approche A) ────────────────────────────────
// Le corps bas-niveau (Actor_Init/Actor_DrawSprites) n'a PAS de slot arme : arme
// et bouclier ne vivent que sur le CActorSprite haut-niveau (slots 5/6, cf.
// CActorSprite_BuildWeaponLayers 0x00d403a0). On réplique donc la résolution
// native : view id -> weaponClass -> chemins .spr/.act -> charge -> frame de NOTRE
// pose -> boucle les layers -> EmitCapLayer à l'origine du corps (0,0). Toutes les
// adresses/offsets/signatures ont été vérifiés dans Ghidra (Actor_DrawSprites,
// Job_BuildWeaponSpritePath, Job_ResolveBodyClass, Act_GetFrame/Layer).
// ── Slot-live : lire l'arme/bouclier DÉJÀ résolus par le jeu sur l'acteur joueur ─
// (Validé x32 en live.) Chaîne : GameMode_GetActive(0x1213338) renvoie le CMode
// (*(mgr+4) si *(mgr+0x58)==1) ; actorMgr = *(CMode+0xcc) ; **acteur joueur** =
// *(actorMgr+0x2c) (vtable 0x01094810, gid @+0x110 = AID own). Tableau des objets
// sprite par slot = *(actor+0x4ac) : slot i = base[i] (objet COMBINÉ = cellules
// @+0x510 ET frames via Act_GetFrame). Slots (CONFIRMÉ x32 en changeant l'équip) :
// 0-4 corps/tête/coiffes, **5 = arme**, 6 = trail arme (souvent null), **7 = bouclier**
// (SEUL le slot 7 change quand on change de bouclier ; le slot 6 reste null). On
// dessine ce que le jeu a mis -> le doll identique au monde, zéro résolution.
constexpr int kOffActorMgr = 0xcc;   // CMode -> actorMgr
constexpr int kOffOwnActor = 0x2c;   // actorMgr -> acteur joueur
constexpr int kOffSlotArr    = 0x4ac;  // acteur -> vector CELLULES (base @+0x4ac, end @+0x4b0)
constexpr int kOffSlotActArr = 0x4b8;  // acteur -> vector FRAMES (Act_GetFrame). Par slot :
constexpr int kSlotWeapon  = 5, kSlotShield = 7;  // cells=*(0x4ac)[s], act=*(0x4b8)[s]

constexpr uintptr_t kWeaponView   = 0x015fb280;  // g_OwnLook_WeaponViewId (0 = rien)
constexpr uintptr_t kShieldView   = 0x015fb284;  // g_OwnLook_ShieldViewId (0 = rien)
constexpr uintptr_t kWeaponView2  = 0x015fb288;  // g_OwnLook_WeaponViewId2 (arme seed login)
constexpr uintptr_t kTexExists    = 0x00a8e500;  // UITextureMgr_ResourceExists(mgr, path)->bool
constexpr uintptr_t kWpnItemClass = 0x00d8a1d0;  // Weapon_ItemIdToWeaponClass(viewId)
// ⚠ Le sélecteur (2e arg du builder) est INVERSÉ vs le nom Ghidra — PROUVÉ live
// (LogWpnDiag) : le wrapper qui pousse 0 (0x..010/080, nommé "SpritePath") renvoie
// le chemin **.act**, celui qui pousse 1 (0x..160/0f0, "ActPath") renvoie le **.spr**.
// On assigne donc l'adresse par l'EXTENSION réelle, pas par le nom.
constexpr uintptr_t kJobWpnSpr    = 0x00d8a160;  // -> .spr (pousse 1)
constexpr uintptr_t kJobWpnAct    = 0x00d8a010;  // -> .act (pousse 0)
constexpr uintptr_t kJobShieldSpr = 0x00d8a0f0;  // -> .spr (pousse 1)
constexpr uintptr_t kJobShieldAct = 0x00d8a080;  // -> .act (pousse 0)
constexpr uintptr_t kActFrameLayer= 0x0070f390;  // Act_GetFrameLayer(frame, idx)
constexpr uintptr_t kStrDtor      = 0x004e78c0;  // std::string::~string (game alloc)
// ── Réglages LIVE (à ajuster à l'œil, sans re-RE) ────────────────────────────
// Nudge d'alignement (repli si décalage constant observé : cf. anchor +0x4dc/+0x4e0
// de SetSlotSprite, non consommé au draw -> auto-align par offsets layer supposé).
constexpr float kWeaponNudgeX = 0.0f;
constexpr float kWeaponNudgeY = 0.0f;
// palVariant = 2e arg de Job_ResolveBodyClass ; DOIT être le job COMPLET (sinon
// fallback Novice). On le dérive du getter natif (pas de valeur figée).

// ── Diagnostics arme/bouclier slot-live (POD, remplis sous __try, loggés HORS __try
// par LogWpnDiag ; throttlé). À RETIRER une fois validé. ─────────────────────────
struct PartDiag {
  int   bail;    // -1=non tenté (item non équipé), 0=ok, 1=obj null, 2=frame null, 3=nL<=0
  int   spr_ok, act_ok, nL, emitted;
  float lx, ly, lw, lh;  // géométrie (cx,cy,w,h) du DERNIER layer émis
  bool  has_tex;         // dernier layer émis a bien une texture native
  void* spr_obj;         // objet slot combiné (= act_obj)
  void* act_obj;
};
struct WpnDiag {
  bool active;
  int  wview, sview, pose;  // wview/sview = nameid équipé (gate)
  PartDiag shield, weapon;
};
WpnDiag g_wpn_diag = {};

// Injecte les layers d'UNE pièce (slot arme/bouclier/trail) dans le buffer actif, à
// partir des DEUX objets sprite DÉJÀ chargés par le jeu (slot-live) : `sprObj` =
// cellules @+0x510 + palette @+0x110 (tableau 0x4ac) ; `actObj` = frames via
// Act_GetFrame (tableau 0x4b8). Prend la frame de NOTRE pose, boucle les layers,
// pré-remplit l'atlas (Get puis Build si miss, CACHE-ONLY), EmitCapLayer à (0,0). pd=diag.
void EmitSlotLayers(void* sprObj, void* actObj, int pose, int frameIdx, float scale,
                    PartDiag* pd) {
  if (pd) { pd->bail = 0; pd->spr_obj = sprObj; pd->act_obj = actObj;
            pd->spr_ok = (sprObj != nullptr); pd->act_ok = (actObj != nullptr); }
  if (!sprObj || !actObj) { if (pd) pd->bail = 1; return; }
  const int emit_start = g_cap_num ? *g_cap_num : 0;
  using ActFrameFn = void*(__fastcall*)(void*, void*, unsigned, unsigned);
  using ActLayerFn = int* (__fastcall*)(void*, void*, unsigned);
  // __thiscall CONFIRMÉ (this=atlas en ECX, RET 0xc) : passer l'atlas + edx=nullptr.
  using AtlasBuildFn = void*(__fastcall*)(void*, void*, short*, int, int*);
  // sprObj = *(actor+0x4ac)[slot] (cellules @+0x510) ; actObj = *(actor+0x4b8)[slot]
  // (frames). Séparés — cf. CActorSprite_BuildPartMap (noms Ghidra SPR/ACT inversés).
  void* frame = reinterpret_cast<ActFrameFn>(render::kActionGetFrameAddr)(
      actObj, nullptr, static_cast<unsigned>(pose), static_cast<unsigned>(frameIdx));
  if (!frame) { if (pd) pd->bail = 2; return; }
  const int lbeg = *reinterpret_cast<int*>(reinterpret_cast<char*>(frame) + 0x20);
  const int lend = *reinterpret_cast<int*>(reinterpret_cast<char*>(frame) + 0x24);
  const int nL = (lend - lbeg) / 0x24;
  if (pd) { pd->nL = nL; if (nL <= 0) pd->bail = 3; }
  for (int i = 0; i < nL; ++i) {
    int* L = reinterpret_cast<ActLayerFn>(kActFrameLayer)(
        frame, nullptr, static_cast<unsigned>(i));
    if (!L || L[2] == -1) continue;  // sprNo -1 = layer invisible (comme le draw)
    // spr_frame = *(SPR+0x510 + sprType*0xc)[sprNo]  (cf. Actor_DrawSprites)
    int* base = *reinterpret_cast<int**>(
        reinterpret_cast<char*>(sprObj) + 0x510 + L[8] * 0xc);
    if (!base) continue;
    short* spr_frame = *reinterpret_cast<short**>(
        reinterpret_cast<char*>(base) + L[2] * 4);
    if (!spr_frame) continue;
    void* palette = reinterpret_cast<char*>(sprObj) + 0x110;  // palette embarquée
    if (L[8] == 0) {  // pré-remplir l'atlas pour que le Get dans EmitCapLayer hit
      int g2[12] = {0};
      void* atlas = reinterpret_cast<void*>(
          *reinterpret_cast<uintptr_t*>(render::kContextPtr) + 0xc0);
      void* c = reinterpret_cast<AtlasGetFn>(render::kAtlasGetCachedAddr)(
          atlas, nullptr, static_cast<int>(reinterpret_cast<intptr_t>(spr_frame)),
          static_cast<int>(reinterpret_cast<intptr_t>(palette)), g2);
      if (!c)
        reinterpret_cast<AtlasBuildFn>(render::kAtlasBuildAddr)(
            atlas, nullptr, spr_frame,
            static_cast<int>(reinterpret_cast<intptr_t>(palette)), g2);
    }
    EmitCapLayer(actObj, spr_frame, L, kWeaponNudgeX, kWeaponNudgeY, scale,
                 *reinterpret_cast<float*>(&L[7]), 0xFFFFFFFFu, palette);
  }
  if (pd) {
    pd->emitted = (g_cap_num ? *g_cap_num : 0) - emit_start;
    if (pd->emitted > 0 && g_cap_buf && g_cap_num) {
      const CapLayer& last = g_cap_buf[*g_cap_num - 1];  // dernier layer poussé
      pd->lx = last.cx; pd->ly = last.cy; pd->lw = last.w; pd->lh = last.h;
      pd->has_tex = (last.tex != nullptr);
    }
  }
}

// Injecte arme + bouclier + trail (slots 5/6/7) dans le buffer actif en SLOT-LIVE :
// on lit les sprites DÉJÀ résolus par le jeu sur l'acteur joueur et on les dessine à
// NOTRE pose. Zéro résolution de chemin (fini le Lua / le viewid item-vs-type). Par
// slot : SPR(cellules @+0x510) = *(actor+0x4ac)[slot], ACT(frames) = *(actor+0x4b8)
// [slot] — DEUX objets distincts (cf. CActorSprite_BuildPartMap ; noms Ghidra SPR/ACT
// inversés). GATE NATIF : un slot n'est dessiné que si les DEUX sont non-null (un slot
// arme stale sans arme n'a qu'UN des deux -> sauté, pas de fantôme). SEH-gardé.
void EmitWeaponShieldLayers(int anim, int dir, int frameIdx, float body_scale) {
  __try {
    using GameModeFn = void*(__fastcall*)(int);
    void* gameMode = reinterpret_cast<GameModeFn>(rag::kModeMgrGetActiveAddr)(
        static_cast<int>(rag::kModeMgrAddr));
    if (!gameMode) return;
    void* actorMgr = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(gameMode) + kOffActorMgr);
    if (!actorMgr) return;
    void* actor = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(actorMgr) + kOffOwnActor);  // acteur joueur (vtbl 0x01094810)
    if (!actor) return;
    int* sprArr = *reinterpret_cast<int**>(
        reinterpret_cast<char*>(actor) + kOffSlotArr);        // cellules (0x4ac)
    int* sprEnd = *reinterpret_cast<int**>(
        reinterpret_cast<char*>(actor) + kOffSlotArr + 4);
    int* actArr = *reinterpret_cast<int**>(
        reinterpret_cast<char*>(actor) + kOffSlotActArr);     // frames (0x4b8)
    if (!sprArr || !sprEnd || !actArr) return;
    const int count = static_cast<int>(sprEnd - sprArr);
    const int pose = anim * 8 + (dir & 7);

    std::memset(&g_wpn_diag, 0, sizeof(g_wpn_diag));
    g_wpn_diag.active = true;
    g_wpn_diag.pose = pose;
    g_wpn_diag.weapon.bail = -1;
    g_wpn_diag.shield.bail = -1;
    g_wpn_diag.wview = EquipSlotNameId(1);  // nameid arme équipée (info log)
    g_wpn_diag.sview = EquipSlotNameId(5);  // nameid bouclier équipé (info log)

    // Ordre painter (vue FACE) : trail(6), arme(5), BOUCLIER(7) en dernier = au-dessus
    // -> coiffes < arme < bouclier (comme le natif corrigé). g_av_wpn_start = début du
    // bloc entier (arme+bouclier+trail) pour que le z-order directionnel le déplace en
    // BLOC derrière le corps en vue de dos. EmitSlotLayers saute si un objet est null.
    g_av_wpn_start = *g_cap_num;  // = body_count (juste après le corps)
    const int order[3] = {6, kSlotWeapon, kSlotShield};
    for (int i = 0; i < 3; ++i) {
      const int slot = order[i];
      if (slot >= count) continue;
      PartDiag* pd = (slot == kSlotWeapon) ? &g_wpn_diag.weapon
                   : (slot == kSlotShield) ? &g_wpn_diag.shield : nullptr;
      EmitSlotLayers(reinterpret_cast<void*>(sprArr[slot]),
                     reinterpret_cast<void*>(actArr[slot]), pose, frameIdx,
                     body_scale, pd);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Compagnons (cart / faucon) sur l'avatar ───────────────────────────────
// Le CHARIOT et le FAUCON ne sont PAS des slots du composite 9-parts : ce sont des
// SOUS-ACTEURS enfants du joueur (std::list @actor+0x3a8, count @+0x3ac ; nœud MSVC
// {+0 next, +4 prev, +8 subActor}). RE live 2026-07-12 (x32 sur un perso en cart).
// Chaque enfant a la MÊME base CActorSprite que les slots -> EmitSlotLayers le dessine
// tel quel : SPR(cellules @+0x510) = child+0x104, ACT(frames) = child+0x108, pose @+0x38,
// frame @+0x3c. Le SPR stocke son chemin GRF à spr+0x14 -> on filtre cart (손수레) / faucon
// (매) par sous-chaîne (le KIND @+0x1a8 est générique = pas discriminant). Le peco, lui,
// s'affiche déjà via le sprite de CORPS monté (rien à faire).
constexpr int kOffChildPrimary = 0x380;  // acteur -> pointeur enfant PERSISTANT (stable tout le frame)
constexpr int kOffChildHead    = 0x3a8;  // acteur -> std::list enfants de RENDU (souvent VIDE en EndScene)
constexpr int kOffChildCount   = 0x3ac;  // acteur -> _Mysize de la liste ci-dessus
constexpr int kOffChildPose = 0x38, kOffChildFrame = 0x3c, kOffChildActive = 0xa0;
constexpr int kOffChildSpr = 0x104, kOffChildAct = 0x108, kOffChildVisible = 0x158, kOffChildParent = 0x15c;
constexpr int kOffChildSprPath = 0x14;  // ressource SPR -> chemin GRF (char buffer null-terminé)
const char kCartMark[]   = "\xBC\xD5\xBC\xF6\xB7\xB9";  // 손수레 (CP949) = cart
const char kFalconMark[] = "\xB8\xC5";                   // 매 (CP949) = faucon
// Nudge d'ancrage (espace acteur, scalé par body_scale) — à régler à l'œil si le cart/faucon
// est décalé. +X = droite, +Y = bas.
constexpr float kCartNudgeX = 0.0f, kCartNudgeY = 0.0f;
constexpr float kFalconNudgeX = 0.0f, kFalconNudgeY = 0.0f;
// Décalage de direction (0..7) si la rotation du compagnon a une base constante-fausse vs le corps.
// Devraient rester 0 : le jeu donne à l'enfant la MÊME direction 0-7 que le corps (RE ScreenDir).
constexpr int kCartDirOffset = 0, kFalconDirOffset = 0;

// Le chemin GRF du SPR (spr+0x14) contient-il `needle` (bytes CP949) ? SEH (POD).
bool ChildSprPathHas(void* spr, const char* needle, int nlen) {
  __try {
    const char* p = reinterpret_cast<const char*>(spr) + kOffChildSprPath;
    for (int i = 0; i < 256 && p[i]; ++i) {
      int j = 0;
      for (; j < nlen && p[i + j]; ++j)
        if (p[i + j] != needle[j]) break;
      if (j == nlen) return true;
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int g_av_companion_present = 0;  // bit0=cart, bit1=faucon (posé ici, lu par la sig d'apparence)

// Placement du cart dans le doll À PLAT (aucune projection perspective live). RE de la
// matrice de vue (`cam+0x98`, lue live, caméra standard camYaw=0) : la carte monde→écran est
// DÉCOUPLÉE (termes croisés = 0) → world +X ⇒ écran X (×1), world +Z ⇒ écran Y (×0.766 =
// sin~50°, l'inclinaison caméra), world +Y(hauteur) ⇒ écran Y (×0.643). Le hat-effect RE
// notait déjà : « pour ImGui standalone, PAS de projection ; depthScale=1, offsets pixel
// directs ». C'est ce qu'on fait : la perspective (invW/profondeur) + la position ÉCRAN live du
// joueur (caméra qui SUIT avec du lag) étaient la cause du « décalage Y+ en marchant ». Ici :
// 100% statique, zéro lecture live. kCartTilePx = 1 tuile en px NATIFS (×body_scale) — RÉGLABLE
// (l'user disait « trop près » : monter la valeur éloigne). kCamPitch = foreshortening vertical.
// Magnitude = taille écran d'1 tuile en px natifs (×body_scale). MESURÉE live : la valeur
// GÉOMÉTRIQUE exacte ≈ 20 px (live tile ~82 px à W=230 ÷ échelle sprite), mais l'user la
// trouvait « trop près » ; 46 était « trop loin ». C'est donc un MULTIPLICATEUR stylistique
// (seul knob de distance) — X et Y en dérivent tous deux (Y = ×kCamPitch), d'où « faux en X ou
// en Y sur les cardinales, trop loin dans les 2 en diagonale » quand il est mal réglé.
constexpr float kCartTilePx = 32.0f;   // entre 20 (fidèle, trop près) et 46 (trop loin) — régler
constexpr float kCamPitch   = 0.766f;  // écrasement vertical axe Z (vm[7], mesuré) — NE PAS toucher
// En pose COMBAT (anim 4) le sprite d'attaque LUNGE en diagonale (45°) ; le cart doit suivre
// ce visuel → direction de placement décalée d'1 pas (±45°). +1 par défaut ; passer à -1 si le
// cart part du mauvais côté diagonal en combat.
constexpr int   kCartCombatShift = 1;
// Faucon MASQUÉ sur l'avatar (choix utilisateur 2026-07-13). Mettre à true pour le réafficher :
// la détection le peuple alors et le reste du pipeline (présence, émission) le prend en charge.
constexpr bool  kShowFalcon = false;

// Dessine le cart (derrière le corps) et le faucon (devant) de l'acteur joueur dans
// le buffer avatar actif, à la DIRECTION de l'avatar. On lit les sprites DÉJÀ résolus par
// le jeu sur les sous-acteurs enfants (aucune résolution de chemin). SEH-gardé.
void EmitCompanionLayers(int pose, bool animate, float body_scale) {
  g_av_companion_present = 0;
  __try {
    using GameModeFn = void*(__fastcall*)(int);
    void* gameMode = reinterpret_cast<GameModeFn>(rag::kModeMgrGetActiveAddr)(static_cast<int>(rag::kModeMgrAddr));
    if (!gameMode) return;
    void* actorMgr = *reinterpret_cast<void**>(reinterpret_cast<char*>(gameMode) + kOffActorMgr);
    if (!actorMgr) return;
    void* actor = *reinterpret_cast<void**>(reinterpret_cast<char*>(actorMgr) + kOffOwnActor);
    if (!actor) return;
    // Sources d'enfants : (1) le POINTEUR PERSISTANT actor+0x380 (stable tout le frame —
    // c'est LUI qui compte : la fiche capture l'avatar en EndScene, où la liste de rendu
    // 0x3a8 est VIDE) ; (2) la std::list 0x3a8 en BONUS (rarement peuplée ici). Dédup.
    void* kids[8]; int nk = 0;
    {
      void* prim = *reinterpret_cast<void**>(reinterpret_cast<char*>(actor) + kOffChildPrimary);
      if (prim) kids[nk++] = prim;
      void* head = *reinterpret_cast<void**>(reinterpret_cast<char*>(actor) + kOffChildHead);
      const int count = *reinterpret_cast<int*>(reinterpret_cast<char*>(actor) + kOffChildCount);
      if (head && count > 0 && count <= 64) {
        void* node = *reinterpret_cast<void**>(head);  // head->next = 1er nœud
        for (int i = 0; i < count && node && node != head && nk < 8; ++i) {
          void* c = *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 8);  // subActor
          node = *reinterpret_cast<void**>(node);                                  // ->next
          bool dup = (c == nullptr);
          for (int k = 0; k < nk && !dup; ++k) if (kids[k] == c) dup = true;
          if (!dup) kids[nk++] = c;
        }
      }
    }
    if (nk == 0) return;

    // Retient le cart / le faucon appartenant à CE joueur (parent==actor, actif, visible),
    // avec leur ACTION+frame LIVE (valides pour LEUR .act).
    void* cartSpr = nullptr; void* cartAct = nullptr;
    void* falcSpr = nullptr; void* falcAct = nullptr;
    for (int i = 0; i < nk; ++i) {
      void* c = kids[i];
      if (*reinterpret_cast<void**>(reinterpret_cast<char*>(c) + kOffChildParent) != actor) continue;
      if (*reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(c) + kOffChildActive) == 0) continue;
      if (*reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(c) + kOffChildVisible) == 0) continue;
      void* spr = *reinterpret_cast<void**>(reinterpret_cast<char*>(c) + kOffChildSpr);
      void* act = *reinterpret_cast<void**>(reinterpret_cast<char*>(c) + kOffChildAct);
      if (!spr || !act) continue;
      // On NE lit PAS pose/frame live de l'enfant (ils défilent quand le joueur bouge → doll
      // qui vibre) : la fiche est FIGÉE, on reconstruit pose (base idle + dir affichée) et frame
      // (0, ou horloge propre en Marche animée) nous-mêmes. On ne garde que les objets SPR/ACT.
      if (ChildSprPathHas(spr, kCartMark, 6))                       { cartSpr = spr; cartAct = act; }
      else if (kShowFalcon && ChildSprPathHas(spr, kFalconMark, 2)) { falcSpr = spr; falcAct = act; }
    }
    if (!cartSpr && !falcSpr) return;
    g_av_companion_present = (cartSpr ? 1 : 0) | (falcSpr ? 2 : 0);

    // POSITION : voir kCartTilePx/kCamPitch plus haut. Le cart traîne ~1 tuile derrière le
    // joueur dans le MONDE (RE `ChildSprite_UpdatePoseAndPos` case 3 : rattrape si dist>seuil
    // DAT_0100ec3c=5.0, sinon FIGE — il ne converge PAS et reflète le DERNIER déplacement, pas le
    // cap courant). On NE LIT donc PAS sa position live ; on place « 1 tuile derrière la dir
    // AFFICHÉE » à plat (bloc cart). `CActorSprite_SetFacingTowardXZ 0x00c40ac0` (ex-mal nommée
    // "SetWorldPosXZ") ne pose que le CAP, pas la position.
    const int d = pose & 7;
    const int anim = pose >> 3;
    // COMBAT (anim 4) : le sprite d'attaque lunge en DIAGONALE → direction EFFECTIVE du cart
    // décalée de kCartCombatShift (45°). Hors combat, dEff == d.
    const int dEff = (anim == 4) ? ((d + kCartCombatShift) & 7) : d;
    // Z-order : le cart passe DEVANT le corps UNIQUEMENT pour les 3 directions « de dos »
    // {3,4,5} (perso face à l'opposé → cart PLUS PRÈS de la caméra, bz=cos(dEff·45)<0) ; il est
    // DERRIÈRE pour les 5 autres {0,1,2,6,7}. (Ancien test asymétrique {0,1,6,7} oubliait d=2.)
    const bool behindBody = (dEff < 3 || dEff > 5);
    // Le CHARIOT est en action idle (base 0) + direction ; le corps garde sa dir `d`, mais pour la
    // FICHE on aligne le cart sur `dEff` (= d hors combat, d±1 en combat pour suivre la diagonale
    // d'attaque perçue — l'utilisateur l'exige). Frame = 0 (ou défilé si Marche). Position à plat
    // ~1 tuile derrière `dEff` (kCartTilePx) ; z-order = behindBody. Réglage fin : kCart*Nudge.
    // FAUCON : dir `d` non décalée, toujours dessiné en DERNIER = devant.
    const int cartDir = (dEff + kCartDirOffset) & 7;  // sprite + placement suivent la diagonale combat
    const int falcDir = (d + kFalconDirOffset) & 7;
    if (cartSpr) {
      // Image du cart : au REPOS = image 0 (figée) ; quand l'avatar S'ANIME on fait
      // DÉFILER les frames du .act — c'est ÇA la « vibration » qui simule le déplacement.
      // RE .act (GRF editor) : le châssis (layer 0) reste fixe, le layer 1 oscille offY
      // −18 → −20 → −18 d'une frame à l'autre ; cycler l'image rejoue ce rebond tel quel,
      // sans autre sprite ni autre action. Cadence = même horloge que le corps
      // (g_av_frame_delay × 25 ms, clampé). Nb d'images du cart via kActFramesFn (stride 0x44).
      // Gate = Marche (anim 1) SEULE : la RE (case 3, frame avancé si dist>seuil) prouve que le
      // cart ne défile QUE quand le joueur se DÉPLACE ; en Combat sur place il reste figé.
      // ⚠ DÉFAUT = 0 (image figée), PAS `cartFrame` (frame LIVE) : le jeu fait défiler le frame
      // live du cart quand le PERSO se déplace sur la map → lire `cartFrame` faisait VIBRER le
      // doll figé (bug signalé). La fiche ne défile QUE via son horloge propre (Marche animée).
      int cartFrameUse = 0;
      if (animate && (pose >> 3) == 1) {
        int cartNframes = 1;
        int* fr = reinterpret_cast<ActFramesFn>(kActFramesFn)(
            cartAct, nullptr, static_cast<unsigned>(cartDir));  // base 0 (idle) + dir affichée
        if (fr) { const int n = static_cast<int>((fr[1] - fr[0]) / 0x44); if (n > 0) cartNframes = n; }
        if (cartNframes > 1) {
          float ims = g_av_frame_delay * 25.0f;
          if (ims < 40.0f) ims = 40.0f;
          if (ims > 600.0f) ims = 600.0f;
          cartFrameUse = static_cast<int>(
              (GetTickCount() / static_cast<DWORD>(ims)) % static_cast<unsigned>(cartNframes));
        }
      }
      const int s = *g_cap_num;
      EmitSlotLayers(cartSpr, cartAct, cartDir, cartFrameUse, body_scale, nullptr);  // base 0 idle
      const int e = *g_cap_num;
      // Marque AVANT le reorder (le flag voyage avec la couche copiée) : le cart est
      // rendu mais NE tire PAS l'ancrage corps (sinon il décentre/rapetisse l'avatar).
      for (int i = s; i < e; ++i) g_av_caps[i].companion = true;
      // POSITION À PLAT = 1 tuile derrière la dir AFFICHÉE `d` — 100% STATIQUE, zéro lecture live
      // (ni position ni caméra) → aucun décalage quand le perso marche. « Derrière » unité monde
      // (X,Z) = (-sin facing_d, cos facing_d), facing_d = -d*45 (camYaw=0, conv. vérifiée live).
      // Carte monde→écran (matrice de vue mesurée, découplée) : X_monde→X_écran, Z_monde→Y_écran
      // ×kCamPitch, Y_écran vers le BAS → +Z (derrière une vue de face) = VERS LE HAUT (−Y). ×
      // kCartTilePx × body_scale (mêmes unités que les couches capturées).
      {
        const float rad = static_cast<float>(-dEff) * 45.0f * 0.01745329252f;  // facing_dEff en rad
        const float bx = -std::sin(rad), bz = std::cos(rad);   // « derrière » unité monde (X, Z)
        const float dx = bx * kCartTilePx * body_scale;
        const float dy = -bz * kCamPitch * kCartTilePx * body_scale;  // Z→Y écrasé, écran vers bas
        for (int i = s; i < e; ++i) { g_av_caps[i].cx += dx; g_av_caps[i].cy += dy; }
      }
      const float nx = kCartNudgeX * body_scale, ny = kCartNudgeY * body_scale;
      if (nx != 0.0f || ny != 0.0f)
        for (int i = s; i < e; ++i) { g_av_caps[i].cx += nx; g_av_caps[i].cy += ny; }
      if (behindBody && e > s && s > 0) {
        const int cn = e - s;
        if (cn > 0 && e <= 48) {
          for (int i = 0; i < cn; ++i) g_av_reorder[i] = g_av_caps[s + i];
          for (int i = s - 1; i >= 0; --i) g_av_caps[i + cn] = g_av_caps[i];
          for (int i = 0; i < cn; ++i) g_av_caps[i] = g_av_reorder[i];
        }
      }
    }
    if (falcSpr) {
      const int s = *g_cap_num;
      // FIGÉ : action de base 0 (repos) + frame 0. Le jeu change la RANGÉE de base du faucon
      // selon l'état de mouvement du joueur (parent+0x70 : repos/marche/…) ET fait défiler son
      // frame → lire `falcPose`/`falcFrame` live ferait bouger le doll figé. On force l'idle.
      EmitSlotLayers(falcSpr, falcAct, falcDir, 0, body_scale, nullptr);
      const int e = *g_cap_num;
      for (int i = s; i < e; ++i) g_av_caps[i].companion = true;  // idem : hors ancrage
      // Faucon perché SUR le joueur (offset monde ~0) : pas de synthèse de position, réglage
      // fin uniquement via kFalconNudge* (aucune lecture live → aucun désync au déplacement).
      const float nx = kFalconNudgeX * body_scale, ny = kFalconNudgeY * body_scale;
      if (nx != 0.0f || ny != 0.0f)
        for (int i = s; i < e; ++i) { g_av_caps[i].cx += nx; g_av_caps[i].cy += ny; }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Construit l'acteur (ctor 19 params) à la pose/image données et le dessine
// (chemin quad) : chaque layer passe par le hook -> buffer ACTIF (g_cap_buf).
// Fonction plate (aucun objet C++ à dérouler) pour être appelable sous __try, et
// réutilisable (corps animé + 2e passe tête figée). L'appelant doit avoir positionné
// g_cap_buf/g_cap_num et g_first_layer avant l'appel.
void AvatarBuildCaptureOnce(void* render_ctx, int sex, int job, int hair, int clo,
                            int hc, int hg_top, int hg_mid, int hg_low, int garment,
                            int pose, int frame) {
  using CtorFn = void*(__fastcall*)(void* self, void* edx, void* render_ctx,
      int x, int y, int sex, int job_short, int job_full, int job_body, int hair,
      int hg_top, int hg_mid, int hg_low, int garment, int p13, int p14,
      int clothes_col, int hair_col, int pose, int frame, int p19);
  using DrawFn = void(__fastcall*)(void* self, void* edx, char param);
  using DtorFn = void(__fastcall*)(void* self, void* edx);
  alignas(8) unsigned char actor[0x200];
  std::memset(actor, 0, sizeof(actor));
  reinterpret_cast<CtorFn>(kActorCtor)(
      actor, nullptr, render_ctx, 0, 0, sex, job & 0xffff, job,
      job & 0xffff, hair, hg_top, hg_mid, hg_low, garment, 0, 0, clo, hc,
      pose, frame, 0);
  g_cur_actor = actor;
  g_cap_active = true;
  reinterpret_cast<DrawFn>(kActorDraw)(actor, nullptr, 1);  // 1 => quad path
  g_cap_active = false;
  g_cur_actor = nullptr;
  reinterpret_cast<DtorFn>(kActorDtor)(actor, nullptr);
}

// ── Avatar plein-corps (character sheet) ─────────────────────────────────────
// Clone de CapturePortraitActor, mais : (a) buffer dédié g_av_caps, (b) garment
// TOUJOURS nourri (corps entier, jamais gaté par head_only), (c) coiffes live via
// ResolveHeadgearViews(top/mid/low) exactement comme le portrait. anim/dir/animate = pose
// choisie (combo sous l'avatar). SEH-gardé ; restaure g_cap_buf/g_cap_num/
// g_frame_dst vers le portrait à la sortie (même contrat que l'aperçu).
// force_frame >= 0 : capture CETTE image précise (export GIF, ignore le temps/le gel
// des poses statiques) ; < 0 : image auto (temps) selon la pose.
void CaptureAvatarActor(int anim, int dir, bool animate, int force_frame = -1,
                        bool show_costume = true) {
  InstallActorCapture();
  if (!g_orig_actor_quad) return;
  void* render_ctx = *reinterpret_cast<void**>(kRenderCtxPtr);
  if (!render_ctx) return;
  g_av_count = 0;
  g_cap_buf = g_av_caps;            // rediriger le hook vers le buffer avatar
  g_cap_num = &g_av_count;
  g_frame_dst = &g_av_frame_count;  // comptage de frames -> buffer avatar
  g_first_layer = true;             // capturer le nb de frames du corps (wrap)
  __try {
    using GetSexFn = int(__fastcall*)(void*, void*);
    using GetJobFn = int(__fastcall*)(void*, void*);
    const int sex = reinterpret_cast<GetSexFn>(kGetSex)(
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
    const int job = reinterpret_cast<GetJobFn>(0x00d5b580)(
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
    const int hair = *reinterpret_cast<int*>(kHair);
    const int clo  = *reinterpret_cast<int*>(kClothesCol);
    const int hc   = *reinterpret_cast<int*>(kHairCol);
    // Coiffes portées : précédence costume + suppression native d'un hat réel multi-slot
    // couvert par un costume (cf. ResolveHeadgearViews). show_costume gate la précédence
    // costume : false (vue Équipement + « Voir les costumes » décoché) -> coiffes RÉELLES.
    int hg_top, hg_mid, hg_low;
    ResolveHeadgearViews(show_costume, &hg_top, &hg_mid, &hg_low);
    // Garment : quand show_costume, le costume de cape (tableau costume slot 2) prime ;
    // sinon le garment EFFECTIF (kGarmentView, déjà config-aware -> réel si costumes off).
    int garment;
    if (show_costume) {
      const uintptr_t cosG = rag::kSessionAddr + 0x2b30 + 2 * 0xf8;  // slot cape/garment costume
      garment = (*reinterpret_cast<int*>(cosG + 4) != 0) ? *reinterpret_cast<int*>(cosG + 0x70)
                                                         : *reinterpret_cast<int*>(kGarmentView);
    } else {
      garment = *reinterpret_cast<int*>(kGarmentView);
    }
    // Signature d'apparence (FNV-1a des viewids/apparence, + nameids arme/bouclier) :
    // stable d'une frame d'anim à l'autre (aucun terme dépendant de l'image), sert à
    // RenderPlayerAvatar pour re-figer le cadrage quand l'équipement change.
    {
      const unsigned parts[] = {
          static_cast<unsigned>(sex),    static_cast<unsigned>(job),
          static_cast<unsigned>(hair),   static_cast<unsigned>(clo),
          static_cast<unsigned>(hc),     static_cast<unsigned>(hg_top),
          static_cast<unsigned>(hg_mid), static_cast<unsigned>(hg_low),
          static_cast<unsigned>(garment),
          static_cast<unsigned>(EquipSlotNameId(1)),   // nameid arme
          static_cast<unsigned>(EquipSlotNameId(5)),   // nameid bouclier
          static_cast<unsigned>(g_av_companion_present),  // cart/faucon (frame précédente)
          static_cast<unsigned>(show_costume)};        // costumes affichés ou non
      unsigned sig = 2166136261u;
      for (unsigned p : parts) sig = (sig ^ p) * 16777619u;
      g_av_sig = sig;
    }
    const int pose = anim * 8 + (dir & 7);
    const int nframes = g_av_frame_count > 0 ? g_av_frame_count : 1;
    // Vitesse NATIVE : délai constant par image = .act delay * 25 ms (AniTick RO ;
    // delay~4.0 => ~100 ms). L'ancien cycle fixe 1.3 s ignorait le nb d'images et
    // était ~4x trop lent. Clamp sain [40, 600] ms.
    float ims = g_av_frame_delay * 25.0f;
    if (ims < 40.0f) ims = 40.0f;
    if (ims > 600.0f) ims = 600.0f;
    // Seuls Marche(1) et Combat(4) s'animent. Repos(0)/Assis(2) sont FIGÉS à l'image 0
    // (sinon la tête + les coiffes « regardent autour » en défilant, gênant). Côté
    // character_sheet, la case « Animer » n'est d'ailleurs proposée que pour Marche/Combat.
    const bool anim_pose = (anim == 1 || anim == 4);
    const int frame =
        (force_frame >= 0)
            ? (force_frame < nframes ? force_frame : (nframes - 1))
            : ((animate && anim_pose && nframes > 1)
                   ? static_cast<int>((GetTickCount() / static_cast<DWORD>(ims)) %
                                      static_cast<unsigned>(nframes))
                   : 0);
    // Corps + tête + coiffes + garment -> g_av_caps (buffer actif).
    AvatarBuildCaptureOnce(render_ctx, sex, job, hair, clo, hc, hg_top, hg_mid,
                           hg_low, garment, pose, frame);
    // Arme + bouclier : absents du corps bas-niveau -> on les injecte NOUS-MÊMES
    // dans g_av_caps (buffer toujours actif ici), à la même pose/frame/échelle que
    // le corps, à l'origine (0,0). `frame` = index d'image du corps ; g_av_body_scale
    // = zoom capturé par le hook au 1er layer du corps.
    g_av_wpn_start = -1;  // évite une valeur périmée si EmitWeaponShieldLayers sort tôt
    EmitWeaponShieldLayers(anim, dir, frame, g_av_body_scale);
    // Z-order directionnel (reproduit le natif corrigé, cf. project_weapon_zorder) :
    // en vue de DOS (dir 2/3/4/5) l'arme ET le bouclier passent DERRIÈRE le corps ;
    // en vue de FACE (dir 0/1/6/7) ils restent devant. On déplace le bloc entier
    // arme+bouclier ([g_av_wpn_start .. count]) avant le corps quand on est de dos.
    {
      const int d = dir & 7;
      const bool back = (d >= 2 && d <= 5);  // groupe DOS natif {2,3,4,5}
      if (back && g_av_wpn_start > 0 && g_av_count > g_av_wpn_start) {
        int m = 0;
        for (int i = g_av_wpn_start; i < g_av_count && m < 48; ++i)
          g_av_reorder[m++] = g_av_caps[i];               // arme + bouclier (derrière)
        for (int i = 0; i < g_av_wpn_start && m < 48; ++i)
          g_av_reorder[m++] = g_av_caps[i];               // corps par-dessus
        std::memcpy(g_av_caps, g_av_reorder, static_cast<size_t>(m) * sizeof(CapLayer));
        g_av_count = m;
      }
    }
    // Compagnons (cart / faucon) : sous-acteurs enfants du joueur, à la MÊME direction que
    // le corps (child+0x38 = action propre + ScreenDir, cf. RE). Le cart a sa PROPRE horloge
    // d'image : figé au repos, il « vibre » (frames .act) en Marche pour simuler le déplacement.
    // Cart derrière le corps en vue de face, faucon devant. APRÈS le reorder arme/bouclier.
    EmitCompanionLayers(pose, animate, g_av_body_scale);
  } __except (EXCEPTION_EXECUTE_HANDLER) { g_cap_active = false; }
  g_cap_buf = g_caps;              // restaurer la cible portrait (toujours)
  g_cap_num = &g_cap_count;
  g_frame_dst = &g_body_frame_count;
}

// ── Capture d'effet STR (hat effect .str) ─────────────────────────────────────
// Miroir du moteur de capture SPRITE ci-dessus, mais pour les effets .str : les hat
// effects (costumes SANS viewid, cf. docs/hat_effect_re.md). Un effet .str est un
// billboard 2D animé (glow additif) rendu par un pipeline SÉPARÉ des sprites :
// Effect_DrawStrFrameQuads (vtable+0xc) boucle les couches actives et appelle
// Effect_SubmitStrQuad (0x00bcfb10) qui projette monde->écran + insère en file
// différée. On hooke Effect_SubmitStrQuad : hors capture -> natif (les effets du
// monde rendent normalement) ; en capture (g_str_cap_active) -> on ASSEMBLE les 4
// sommets 2D EXACTEMENT comme le natif (mêmes branches min/max = winding + UV
// corrects), SANS la projection monde/écran ni l'échelle caméra, puis on SUPPRIME
// l'insert natif. La rotation + l'ancrage tête + l'échelle se font au composite.
constexpr uintptr_t kStrSubmitQuad = 0x00bcfb10;  // Effect_SubmitStrQuad (hooké)
constexpr uintptr_t kStrLayerTex   = 0x00715be0;  // Str_GetLayerTexture(layer, idx)

// Une couche capturée d'un effet .str : 4 sommets locaux (pré-rotation, relatifs à
// (cx,cy)) + UV normalisés + couleur + blend. Le composite applique rotation(angle)
// puis (cx,cy), puis son propre ancrage/échelle, et dessine via AddImageQuad (tint).
struct StrCapLayer {
  void*    tex;                       // IDirect3DTexture9* (page de la couche)
  float    cx, cy;                    // centre 2D de la couche (unités canvas .str)
  float    vx[4], vy[4];              // 4 coins locaux (pré-rotation)
  float    vu[4], vv[4];              // 4 UV (déjà normalisés [0..1])
  float    angle;                     // angle brut workBuf[0x15] (deg ; conv. au composite)
  unsigned rgba;                      // IM_COL32(r,g,b,a) (tint de la couche)
  int      sblend, dblend;            // facteurs de blend .str (additif quasi toujours)
};
StrCapLayer g_str_caps[64];
int         g_str_count = 0;
bool        g_str_cap_active = false;  // vrai seulement pendant NOTRE rendu d'effet

using StrLayerTexFn = void* (__fastcall*)(void* layer, void* edx, int idx);
using StrSubmitFn = void (__fastcall*)(void* self, void* edx, void* layer,
                                       float* workBuf, int depthIdx, float* camera);
StrSubmitFn g_orig_str_quad = nullptr;

// Résolution texture des .str en SOUS-DOSSIER = mécanisme NATIF (RE 2026-07-12). Le nœud
// in-world (CActorSprite host, dir posé par _splitpath dans CActorSprite_LoadStrEffect) charge ses
// textures via Str_GetLayerTextureWithDir 0x00715d30 : construit "effect\<dir><basename>" à partir
// d'un std::string (dir) et met en cache au MÊME slot que Str_GetLayerTexture (layer+idx*4+0x1c8).
// On appelle CETTE fonction (au lieu d'un loader maison) -> même chemin, même cache, MÊME type
// d'objet écrit dans +0x1c8 que le rendu natif -> cohérent (pas de confusion de type = plus de
// crash), pas de cache maison (FPS natif). dir = partie dossier de GetHatEfResName, CASSE
// D'ORIGINE (le natif ne minusculise pas ; l'effet rend in-world donc la casse résout bien).
char g_str_cap_subdir[80] = "";                    // dir du .str (ex. "efst_Gold_Shower\") ou ""
constexpr uintptr_t kStrLayerTexDir = 0x00715d30;  // Str_GetLayerTextureWithDir(layer, idx, &dir)
using StrLayerTexDirFn = void* (__fastcall*)(void* layer, void* edx, int idx, const void* dir);

// std::string MSVC (release, 32-bit) reconstruite en POD : [union buf16/ptr][_Mysize][_Myres].
// Le natif lit _Mysize @+0x10, et si _Myres @+0x14 > 15 déréférence *ptr (mode tas), sinon lit les
// caractères en place (SSO). Remplie depuis le dir à chaque capture ; passée par & au natif. POD
// -> utilisable même si le hook est sous SEH (pas d'unwind C++).
struct MsvcStdString { char bx[16]; uint32_t mysize; uint32_t myres; };
MsvcStdString g_str_dir_str = { {0}, 0, 15 };
char          g_str_dir_chars[80] = "";
void SetStrCapDir(const char* dir) {
  size_t n = dir ? std::strlen(dir) : 0;
  if (n >= sizeof(g_str_dir_chars)) n = sizeof(g_str_dir_chars) - 1;
  std::memcpy(g_str_cap_subdir, dir ? dir : "", n); g_str_cap_subdir[n] = '\0';
  std::memcpy(g_str_dir_chars,  dir ? dir : "", n); g_str_dir_chars[n]  = '\0';
  if (n < 16) {                                          // SSO : caractères en place
    std::memcpy(g_str_dir_str.bx, g_str_dir_chars, n + 1);
    g_str_dir_str.mysize = static_cast<uint32_t>(n);
    g_str_dir_str.myres  = 15;                           // <=15 -> natif lit bx en place
  } else {                                               // tas : ptr dans les 4 premiers octets
    *reinterpret_cast<char**>(g_str_dir_str.bx) = g_str_dir_chars;
    g_str_dir_str.mysize = static_cast<uint32_t>(n);
    g_str_dir_str.myres  = static_cast<uint32_t>(n);     // >15 -> natif déréférence *ptr
  }
}

// Hook sur Effect_SubmitStrQuad. Assemble les 4 sommets comme le natif (0x00bcfb10) :
// bloc X/U gardé par workBuf[0xb]<=workBuf[0xa], bloc Y/V par workBuf[0x10]<=workBuf[0xf].
// Coins X = workBuf[0xa..0xd], Y = workBuf[0xe..0x11] ; src rect (px) = workBuf[2..5] ;
// UV normalisés par tex+0x14/tex+0xc (U) & tex+0x18/tex+0x10 (V). SEH-gardé.
void __fastcall Hooked_StrQuad(void* self, void* edx, void* layer, float* workBuf,
                               int depthIdx, float* camera) {
  if (!g_str_cap_active) {
    g_orig_str_quad(self, edx, layer, workBuf, depthIdx, camera);
    return;
  }
  __try {
    if (layer && workBuf && g_str_count < 64) {
      const int off = g_imgui_dx7_active ? kCTexOffDX7 : kCTexOffDX9;
      int texIdx = static_cast<int>(workBuf[0x12]);
      if (texIdx < 0) texIdx = 0;
      // Résolution texture = résolveurs NATIFS (mêmes chemin/cache/type d'objet que le rendu
      // in-world -> cohérent, pas de crash de confusion de type). Sous-dossier ->
      // Str_GetLayerTextureWithDir(layer, idx, &dir) : "effect\<dir><basename>". Racine ->
      // Str_GetLayerTexture(layer, idx) : "effect\<basename>". Les deux mettent en cache au slot
      // natif layer+idx*4+0x1c8 (pas de cache maison = FPS natif).
      void* ctex = g_str_cap_subdir[0]
          ? reinterpret_cast<StrLayerTexDirFn>(kStrLayerTexDir)(layer, nullptr, texIdx, &g_str_dir_str)
          : reinterpret_cast<StrLayerTexFn>(kStrLayerTex)(layer, nullptr, texIdx);
      void* native = ctex ? *reinterpret_cast<void**>(reinterpret_cast<char*>(ctex) + off)
                          : nullptr;
      if (native) {
        StrCapLayer& L = g_str_caps[g_str_count++];
        L.tex = native;
        L.cx = workBuf[0]; L.cy = workBuf[1];
        const float u0 = workBuf[2], v0 = workBuf[3];       // srcU0, srcV0 (px)
        const float u1 = workBuf[4] + workBuf[2];           // srcU0+srcW
        const float v1 = workBuf[5] + workBuf[3];           // srcV0+srcH
        // Bloc X/U (vx/vu des 4 sommets) — réplique EXACTE de Effect_SubmitStrQuad.
        if (workBuf[0xb] <= workBuf[0xa]) {
          L.vx[0] = workBuf[0xc]; L.vu[0] = u1;
          L.vx[1] = workBuf[0xb]; L.vu[1] = u1;
          L.vx[2] = workBuf[0xd]; L.vu[2] = u0;
          L.vx[3] = workBuf[0xa]; L.vu[3] = u0;
        } else {
          L.vx[0] = workBuf[0xd]; L.vu[0] = u0;
          L.vx[1] = workBuf[0xa]; L.vu[1] = u0;
          L.vx[2] = workBuf[0xc]; L.vu[2] = u1;
          L.vx[3] = workBuf[0xb]; L.vu[3] = u1;
        }
        // Bloc Y/V (vy/vv des 4 sommets) — réplique EXACTE.
        if (workBuf[0x10] <= workBuf[0xf]) {
          L.vy[0] = workBuf[0xe];  L.vv[0] = v0;
          L.vy[1] = workBuf[0x11]; L.vv[1] = v1;
          L.vy[2] = workBuf[0xf];  L.vv[2] = v0;
          L.vy[3] = workBuf[0x10]; L.vv[3] = v1;
        } else {
          L.vy[0] = workBuf[0x11]; L.vv[0] = v1;
          L.vy[1] = workBuf[0xe];  L.vv[1] = v0;
          L.vy[2] = workBuf[0x10]; L.vv[2] = v1;
          L.vy[3] = workBuf[0xf];  L.vv[3] = v0;
        }
        // Normalisation UV : *= tex[0x14]/tex[0xc] (U), tex[0x18]/tex[0x10] (V).
        const int tW  = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctex) + 0xc);
        const int tH  = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctex) + 0x10);
        const int t14 = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctex) + 0x14);
        const int t18 = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctex) + 0x18);
        const float us = (tW != 0) ? static_cast<float>(t14) / static_cast<float>(tW) : 0.0f;
        const float vs = (tH != 0) ? static_cast<float>(t18) / static_cast<float>(tH) : 0.0f;
        for (int k = 0; k < 4; ++k) { L.vu[k] *= us; L.vv[k] *= vs; }
        L.angle = workBuf[0x15];
        const unsigned r = static_cast<unsigned>(static_cast<int>(workBuf[0x16])) & 0xffu;
        const unsigned g = static_cast<unsigned>(static_cast<int>(workBuf[0x17])) & 0xffu;
        const unsigned b = static_cast<unsigned>(static_cast<int>(workBuf[0x18])) & 0xffu;
        const unsigned a = static_cast<unsigned>(static_cast<int>(workBuf[0x19])) & 0xffu;
        // Couleur de la couche (tint) = workBuf[0x16..0x19] (r,g,b,a) = IM_COL32(r,g,b,a).
        // Pour gold_shower : blanc × alpha ANIMÉ (le fondu du glow vient de a, pas de la texture).
        L.rgba = (a << 24) | (b << 16) | (g << 8) | r;
        // Blend PAR COUCHE = facteurs D3DBLEND ENTIERS (RE : workBuf[0x1a]/[0x1b] copiés en MOV
        // brut depuis le keyframe +0x70/+0x74, PAS des floats). Les lire en INT brut, sinon le
        // cast float->int donne 0 (les bits entiers 5/7 = floats dénormaux ≈ 0).
        L.sblend = reinterpret_cast<const int*>(workBuf)[0x1a];  // gold_shower : 5 = D3DBLEND_SRCALPHA
        L.dblend = reinterpret_cast<const int*>(workBuf)[0x1b];  // gold_shower : 7 = D3DBLEND_DESTALPHA
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  // insert natif SUPPRIMÉ pendant la capture (rien ne rend dans la scène)
}

void InstallStrCapture() {
  static bool done = false;
  if (done) return;
  done = true;
  using namespace hooking;
  g_orig_str_quad = reinterpret_cast<StrSubmitFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kStrSubmitQuad),
          reinterpret_cast<uint8_t*>(&Hooked_StrQuad)));
}

// Origine du canvas .str : soustraite du centre de couche par Effect_SubmitStrQuad
// (recentre l'effet sur le point d'ancrage = tête de l'acteur). LUES à l'exécution
// (pas de valeur figée). L'échelle caméra + la position écran du nœud sont ignorées :
// on ancre nous-mêmes (ox,oy) à l'échelle `scale` de notre choix.
constexpr uintptr_t kStrCanvasCx = 0x01022f5c;  // DAT_01022f5c (soustrait de workBuf[0])
constexpr uintptr_t kStrCanvasCy = 0x01013e88;  // DAT_01013e88 (soustrait de workBuf[1])

// Composite les couches STR capturées (g_str_caps) par-dessus le dessin courant, EXACTEMENT
// comme le natif (RE Effect_SubmitStrQuad) : (ox,oy) = point d'ancrage écran (canvas 320/240),
// `scale` = échelle canvas.str -> px écran. Par couche : rotation (unité native 1024/tour) autour
// du centre, translation (centre - centreCanvas), ancrage + échelle, blend NATIF par couche,
// AddImageQuad (tint = couleur). No-op si vide. Aucun paramètre de calibrage manuel : tout est
// dérivé du natif (cf. constantes d'ancre/échelle dans RenderPlayerAvatar).
void DrawStrCapLayers(ImDrawList* dl, float ox, float oy, float scale) {
  if (!dl || g_str_count <= 0) return;
  float canvas_cx = 0.0f, canvas_cy = 0.0f;
  __try {
    canvas_cx = *reinterpret_cast<const float*>(kStrCanvasCx);
    canvas_cy = *reinterpret_cast<const float*>(kStrCanvasCy);
  } __except (EXCEPTION_EXECUTE_HANDLER) { canvas_cx = canvas_cy = 0.0f; }

  // Blend PAR COUCHE = facteurs NATIFS capturés (plus de mode alpha forcé qui peignait le fond
  // noir des textures -> bord noir). Callback DX9 explicite par couche (DX7 : repli alpha).
  for (int i = 0; i < g_str_count; ++i) {
    const StrCapLayer& L = g_str_caps[i];
    if (!L.tex) continue;
    // Rotation : unité native = 1024 par tour (RE Effect_SubmitStrQuad), rad = angle * 2π/1024.
    const float rad = L.angle * 0.006135923f;  // 2π/1024 (et NON deg->rad)
    const float ca = std::cos(rad), sa = std::sin(rad);
    ImVec2 p[4];
    for (int k = 0; k < 4; ++k) {
      const float rx = L.vx[k] * ca - L.vy[k] * sa;
      const float ry = L.vx[k] * sa + L.vy[k] * ca;
      p[k].x = (rx + (L.cx - canvas_cx)) * scale + ox;
      p[k].y = (ry + (L.cy - canvas_cy)) * scale + oy;
    }
    // SRCBLEND/DESTBLEND natifs. DESTALPHA(7)->ONE(2), INVDESTALPHA(8)->ZERO(1) car le backbuffer
    // RO n'a pas d'alpha destination (le HW traite DESTALPHA comme 1.0) ; autres facteurs tels
    // quels. gold_shower = SRCALPHA(5)/ONE(2) = additif modulé par alpha -> fond noir invisible.
    int dst = L.dblend;
    if (dst == 7) dst = 2; else if (dst == 8) dst = 1;
    if (L.sblend > 0 && dst > 0) {
      void* cb = reinterpret_cast<void*>(
          static_cast<uintptr_t>((L.sblend & 0xff) | ((dst & 0xff) << 8)));
      dl->AddCallback(reinterpret_cast<ImDrawCallback>(D3D9_ExplicitBlendCallback()), cb);
    } else {
      dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);  // repli alpha (blend inconnu)
    }
    // Winding : strip natif (0,1,2,3) -> périmètre AddImageQuad (0,2,3,1).
    const ImTextureID tex = (ImTextureID)(uintptr_t)L.tex;
    dl->AddImageQuad(tex, p[0], p[2], p[3], p[1],
                     ImVec2(L.vu[0], L.vv[0]), ImVec2(L.vu[2], L.vv[2]),
                     ImVec2(L.vu[3], L.vv[3]), ImVec2(L.vu[1], L.vv[1]), L.rgba);
  }
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);  // restaure l'alpha ImGui
}

// ── Capture d'effet EZ-PARTICULES (hat effects `hatEffectID`) ─────────────────
// 2E FAMILLE de hat effects (docs/hat_effect_re.md, RE 2026-07-13). Les entrées
// HatEffectInfo avec `hatEffectID` (ex. HAT_EF_Digital_Space ordinal 87 -> id 1240)
// NE sont PAS des .str : GetHatEfResName renvoie vide (fiche perso NUE, validé). Elles
// rendent via le système PARTICULES procédural, plus la famille CEffectMgr (auras/statuts
// type Perm_Frost) qui passe par un draw par-effet en phase render.
// ⚠ TOUTE cette capture (hooks, appartenance, projection d'ancre, blend et couleurs par
// sommet) vit désormais dans le module PARTAGÉ `ez_capture` (features/fx/ez_effect_capture.h).
// On ne garde ici que la CONSOMMATION : ciblage de l'acteur, filtre de dessin par id, FIT
// et composite sur le doll. Ne pas ré-implémenter de capture locale : les trois pièges
// (nombre de sommets variable, couleur par sommet, blend par primitive) sont documentés
// dans l'en-tête du module.
// Aperçu cashshop « try before you buy » : spawn temporaire d'un effet EZ/CEffectMgr NON équipé sur le
// JOUEUR (il rend en jeu -> la capture live le récupère). Actor_ToggleEffectId(this=acteur, id, addFlag) :
// addFlag=1 insère l'id dans l'ENSEMBLE actor+0x3fc et RÉ-APPLIQUE TOUT l'ensemble (donc les effets
// ÉQUIPÉS du joueur restent/sont restaurés) ; addFlag=0 retire l'id de l'ensemble et n'enlève QUE cet
// effet. -> les effets équipés sont préservés par l'ensemble (⚠ NE PAS utiliser Effect_ApplyEffectIdToActor
// direct 0xc41ba0 : il casse l'état équipé -> @refresh nécessaire). id unifié = ordinal + 0x98a.
constexpr uintptr_t kToggleEffectId = 0x00c44940;  // Actor_ToggleEffectId(this, effectId, addFlag) __thiscall
constexpr int       kHatOrdinalBase = 0x98a;       // id unifié d'un hat effect = ordinal + 0x98a

// Ids concrets CIBLES = GetHatEffectID(ordinal) des hat effects équipés (rempli chaque OnTick). Ils ne
// filtrent PLUS la capture (le module capture tout ce qui appartient à l'acteur ciblé) mais le DESSIN :
// passés en `DrawOpts::ids`, ils isolent le(s) hat effect(s) des autres effets du joueur (auras
// skill/statut ids 164/230, footprint). 0 cible = pas de filtre -> tout ce qui a été capturé.
int   g_ez_target_ids[8] = {0};
int   g_ez_target_count  = 0;                        // 0 = pas de filtre id au dessin

void*    g_ez_owner_actor= nullptr;  // acteur dont on capture l'effet (nullptr = capture off)
void*    g_ez_last_actor = nullptr;  // dernier ownActor NON-null connu (repli si mode transitoirement inactif)
int      g_ez_preview_ord    = 0;    // ordinal EZ/CEffectMgr demandé pour l'aperçu cashshop (0 = aucun)
DWORD    g_ez_preview_tick   = 0;    // dernier RequestEzPreview (keep-alive : despawn si non rafraîchi)
int      g_ez_preview_active = 0;    // ordinal réellement spawné sur le joueur (0 = aucun)
// Id CONCRET de l'effet d'aperçu (résolu par GetHatEffectID). Indispensable DEUX fois : pour le
// supprimer du rendu du monde, et pour l'AUTORISER au dessin — g_ez_target_ids ne liste que les
// effets ÉQUIPÉS, donc sans ça un aperçu d'effet non équipé est filtré et n'apparaît nulle part.
int      g_ez_preview_cid    = 0;


// Installe la capture PARTAGÉE (module ez_capture) : hooks chaînés, idempotents. On active en plus la
// famille CEffectMgr (auras/statuts type Perm_Frost), que le doll doit composer comme les effets EZ.
// ⚠ La suppression in-world ne vise QUE l'effet d'APERÇU (cf. ReconcileEzPreview) : les hat effects
// ÉQUIPÉS restent visibles sur le personnage, ils sont légitimement portés.
void InstallEzCapture() {
  static bool done = false;
  if (done) return;
  done = true;
  // La famille CEffectMgr (auras/statuts, ex. Perm_Frost) est capturée d'office par le module ; le
  // doll l'inclut au dessin via DrawOpts::include_effmgr.
  ez_capture::EnsureInstalled();
}

// Acteur joueur live : GameMode_GetActive(0x1213338) -> CMode -> *(+0xcc)=actorMgr ->
// *(+0x2c)=ownActor. nullptr hors-jeu. POD/SEH. (Même chaîne que EmitCompanionLayers.)
void* GetOwnActorLive() {
  void* actor = nullptr;
  __try {
    void* gm = reinterpret_cast<void*(__fastcall*)(int)>(rag::kModeMgrGetActiveAddr)(
        static_cast<int>(rag::kModeMgrAddr));
    if (gm) {
      void* mgr = *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) + kOffActorMgr);
      if (mgr) actor = *reinterpret_cast<void**>(reinterpret_cast<char*>(mgr) + kOffOwnActor);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { actor = nullptr; }
  return actor;
}

// ── Aperçu cashshop d'un effet EZ/CEffectMgr NON équipé (« try before you buy ») ──────────────
// Appelé CHAQUE frame de survol d'un item-effet EZ (keep-alive). ReconcileEzPreview (OnTick) applique la
// différence : spawn/despawn temporaire de l'effet SUR LE JOUEUR via Actor_ToggleEffectId -> il rend en
// jeu -> la capture live du module ez_capture le récupère -> composité sur l'aperçu. L'effet
// apparaît sur le perso en jeu pendant le survol (voulu). Robuste au survol qui change / s'arrête (keep-alive 300 ms).
void RequestEzPreview(int ordinal) {
  g_ez_preview_ord = ordinal;
  g_ez_preview_tick = GetTickCount();
}
// Défini plus bas : appel brut d'un global Lua getter(ord). Sert ici à résoudre l'id CONCRET de
// l'effet d'aperçu (GetHatEffectID), seul moyen de le supprimer nominativement du rendu du monde.
float HatLuaNum(int ordinal, const char* fn, float def);

void ReconcileEzPreview() {
  int wanted = g_ez_preview_ord;
  if (wanted != 0 && GetTickCount() - g_ez_preview_tick > 300) wanted = 0;  // survol terminé -> despawn
  if (wanted == g_ez_preview_active) return;                                 // rien à faire
  void* actor = GetOwnActorLive();
  if (!actor) return;                          // hors-jeu : on retentera (l'état actif reste, resync au retour)
  using ToggleFn = void(__thiscall*)(void*, int, char);
  const ToggleFn toggle = reinterpret_cast<ToggleFn>(kToggleEffectId);
  __try {
    if (g_ez_preview_active != 0)
      toggle(actor, g_ez_preview_active + kHatOrdinalBase, 0);   // despawn l'ancien aperçu (retire de l'ensemble)
    if (wanted != 0)
      toggle(actor, wanted + kHatOrdinalBase, 1);                // spawn le nouvel aperçu (insère + ré-applique tout)
    g_ez_preview_active = wanted;
  } __except (EXCEPTION_EXECUTE_HANDLER) { g_ez_preview_active = wanted; }

  // L'aperçu doit se voir SUR LE DOLL UNIQUEMENT : on demande au module de ne pas laisser passer ses
  // primitives vers le rendu du monde. La suppression est NOMINATIVE et ne vise QUE l'effet d'aperçu,
  // surtout pas les hat effects ÉQUIPÉS — ceux-là sont légitimement portés par le personnage et
  // doivent rester visibles en jeu.
  // ⚠ Un aperçu de la famille CEffectMgr n'a pas d'id résolu (effect_id = -1) : il n'est pas
  // supprimable par id et restera visible sur le personnage.
  g_ez_preview_cid =
      (g_ez_preview_active != 0)
          ? static_cast<int>(HatLuaNum(g_ez_preview_active, "GetHatEffectID", 0.0f))
          : 0;
  // On masque sur le personnage : l'effet d'aperçu par son id, PLUS la famille « hôte particule »
  // tant qu'un aperçu est en cours — celle-ci est anonyme, on ne peut la viser que globalement.
  // ⚠ Conséquence assumée : pendant un survol, un costume PORTÉ de cette même famille disparaît lui
  // aussi du personnage. C'est bref, et ça évite l'effet parasite qui restait collé au sprite.
  int sup[2];
  int nsup = 0;
  if (g_ez_preview_cid > 0) sup[nsup++] = g_ez_preview_cid;
  if (g_ez_preview_active != 0) sup[nsup++] = ez_capture::kStrParticleId;
  ez_capture::SetSuppressedIds(ez_capture::kSlotDoll, nsup ? sup : nullptr, nsup);
}

// Dessin du composite EZ sur le doll. Les sommets capturés sont en écran (XYZRHW, même FVF/chemin de
// draw que les STR -> RenderPrim_DrawRecord partagé) ; l'ancre écran et l'échelle de profondeur sont
// fournies par le module (ez_capture::ProjectAnchor). Repasser à false si régression.
bool g_ez_enabled = true;  // verts = écran (XYZRHW confirmé) ; outliers filtrés par proximité (g_ez_max_r)
// Calibrage manuel de l'échelle EZ sur le doll (défaut 1.0 = ratio physique s/S_live).
float g_ez_cal = 1.0f;
// Plancher du FIT : le doll ne rétrécit pas en-dessous de ce ratio de son échelle de base, même si
// l'effet est plus grand que le cadre (au-delà, l'effet déborde et est rogné par le clip du rect).
// Évite le « doll minuscule » quand un effet (aura) s'étale large. Réglable (0.5-0.8 raisonnable).
float g_ez_fit_floor = 0.6f;
// Rayon écran (px) autour de l'ancre au-delà duquel un triangle est rejeté (outlier hors-effet).
float g_ez_max_r = 700.0f;
// Blend : chaque primitive porte SON PROPRE SRCBLEND/DESTBLEND, lu dans l'enregistrement par le module
// et rejoué tel quel (DrawOpts::blend_mode = 0). C'est le seul mode correct : un même effet mélange
// additif et alpha dans la même frame. g_ez_additive=true force l'additif GLOBAL (outil de comparaison
// des glows uniquement — il crame les primitives alpha).
bool g_ez_additive = false;

// FIT : les quads sont dessinés LIVE (animés) chaque frame ; seule la BBOX de l'effet est figée une
// fois (stabilité du scale doll), invalidée au changement de hat effect (0x0A3B) et nettoyée après
// quelques frames sans capture (effet retiré).
bool     g_ez_frozen_valid = false;  // bbox FIT calculée pour l'effet courant
int      g_ez_empty_frames = 0;      // frames consécutives SANS effet capturé (Count()==0) -> nettoie la bbox
// BBox de l'effet figé, en unités « doll par unité d'échelle s » (= (v-ancre)/S). Sert au FIT :
// RenderPlayerAvatar réduit s (perso ET effet ensemble) pour que l'effet tienne dans le cadre fixe.
float    g_ez_fz_dx0 = 0.0f, g_ez_fz_dx1 = 0.0f, g_ez_fz_dy0 = 0.0f, g_ez_fz_dy1 = 0.0f;

// Composite les primitives EZ capturées par le module sur le doll. Les sommets sont en écran ABSOLU
// (projetés monde par le natif ce frame) ; ez_capture::ProjectAnchor donne l'ancre écran (ax,ay) de
// l'effet et S = px écran par unité canvas à cette profondeur. Doll = (ox + (vx-ax)·R, oy + (vy-ay)·R)
// avec R = (s/S)·cal (1 unité canvas monde -> s px doll, comme le sprite). No-op si rien n'est capturé
// ou si la projection échoue.
// `before` : ne dessine que la phase demandée. Z-ORDER par primitive = bit 0x8 des FLAGS natifs (RE
// render-order, workflow 2026-07-16) : le jeu range chaque primitive dans un BUCKET, et le bit 0x8 =
// "render before character" (bucket dessiné AVANT le perso, donc DERRIÈRE lui). Le module l'applique
// via DrawOpts::use_zorder/draw_behind. Un effet est TOUJOURS tout-devant OU tout-derrière (le moteur
// ne l'intercale jamais entre les couches perso).
// PAS de reset ici (appelé 2×/frame) : la capture est vidée sur frontière de frame par le module.
// Construit le filtre d'effets d'une surface : les hat effects ÉQUIPÉS, plus l'effet d'APERÇU si
// cette surface le montre. Factorisé pour que le DESSIN, la MESURE (bbox du FIT) et la DÉCISION
// d'appliquer le FIT voient rigoureusement le même sous-ensemble.
// `ids` doit pouvoir accueillir 9 entrées. Renvoie le nombre d'ids écrits.
int BuildEzFilter(int* ids, bool with_preview) {
  int n = 0;
  for (int i = 0; i < g_ez_target_count && n < 8; ++i) ids[n++] = g_ez_target_ids[i];
  if (with_preview && g_ez_preview_cid > 0) {
    bool dup = false;
    for (int i = 0; i < n; ++i) if (ids[i] == g_ez_preview_cid) { dup = true; break; }
    if (!dup) ids[n++] = g_ez_preview_cid;
  }
  return n;
}

// Nombre de primitives capturées qui concernent LE DOLL (équipés seulement). ⚠ Ne pas utiliser
// ez_capture::Count() pour cela : il compte TOUS les effets du joueur, donc l'aperçu survolé
// ailleurs — le doll croirait alors avoir quelque chose à cadrer.
int EzPrimCountForDoll() {
  int ids[9];
  ez_capture::DrawOpts o;
  o.id_count = BuildEzFilter(ids, /*with_preview=*/false);
  o.ids = ids;
  o.include_effmgr = false;  // les CEffectMgr portent l'id concret : filtrés par `ids` (cf. DrawEzCapTris)
  o.include_str_particle = (g_ez_preview_active == 0);  // même règle que DrawEzCapTris
  const ez_capture::Prim* p = ez_capture::Prims();
  const int total = ez_capture::Count();
  int n = 0;
  for (int i = 0; i < total; ++i)
    if (ez_capture::Matches(p[i], o)) ++n;
  return n;
}

// `with_preview` : inclure l'effet d'APERÇU en cours (survol cash-shop / description d'item).
// ⚠ Réservé aux surfaces d'aperçu. Le doll de la fiche perso doit montrer ce que le personnage PORTE,
// pas ce qu'on est en train de survoler ailleurs — sinon l'effet survolé apparaît aussi sur le doll.
void DrawEzCapTris(ImDrawList* dl, float ox, float oy, float s, bool before, bool with_preview) {
  if (!dl) return;
  // ANCRE ÉCRAN LIVE : reprojetée CHAQUE frame. Comme elle suit l'acteur, (vx-ax) annule la
  // translation quand le perso marche, tout en gardant les primitives LIVE -> l'ANIMATION joue
  // (l'ancien freeze figeait UN instantané -> position stable mais anim gelée).
  // Filtre = effets ÉQUIPÉS + l'effet d'APERÇU en cours. Construit AVANT l'ancre : celle-ci doit être
  // celle d'un effet QU'ON DESSINE — la demander sans filtre projetait la position du premier effet
  // capturé, souvent un autre (aura, statut), d'où une ancre sans rapport et un doll vide.
  int ids[9];
  const int id_count = BuildEzFilter(ids, with_preview);
  ez_capture::DrawOpts o;
  o.ox = ox; o.oy = oy;
  o.blend_mode = g_ez_additive ? 2 : 0;   // 0 = blend natif par primitive (correct)
  o.use_zorder = true; o.draw_behind = before;
  o.max_r = g_ez_max_r;                   // filtre de proximité : rejette les outliers « nuages »
  o.ids = ids; o.id_count = id_count;
  // Famille CEffectMgr : filtrage PRÉCIS par id d'instance (`ordinal + 0x98a`), et non plus « tout
  // ou rien ». C'est ce qui permet enfin de ne montrer sur le doll que les costumes PORTÉS, et de
  // réserver le costume SURVOLÉ à la surface d'aperçu.
  // ⚠ L'essai en jeu a montré que ces effets passent bel et bien par ce chemin (les exclure vidait
  // le doll ET l'aperçu), contrairement à ce qu'une analyse statique avait conclu.
  // ⚠ Plus besoin d'inclusion en bloc : MESURÉ en jeu, les instances CEffectMgr portent l'id CONCRET
  // (ordinal 248 -> 2429), donc le module les expose comme des effect_id normaux et elles passent par
  // le filtre `ids` ci-dessus, exactement comme le chemin EZ. C'est ce qui permet enfin de réserver
  // le costume SURVOLÉ à la surface d'aperçu, et de le MASQUER sur le personnage.
  o.include_effmgr = false;
  // Famille « hôte particule » (CEZ2STREffect → CEZ2STRParticle) : ANONYME, impossible à rattacher à
  // un effet précis. On tranche donc par le CONTEXTE plutôt que par l'id :
  //  - surface d'aperçu -> toujours (elle ne montre qu'un effet à la fois) ;
  //  - doll -> seulement si AUCUN aperçu n'est en cours, car alors ces primitives ne peuvent venir
  //    que d'un effet PORTÉ. Pendant un survol l'origine est ambiguë, on s'abstient.
  // ⚠ Conséquence assumée : pendant un survol, un effet PORTÉ de cette famille disparaît du doll.
  o.include_str_particle = with_preview || (g_ez_preview_active == 0);
  // ⚠ AUCUN id à montrer = ne rien dessiner. La règle du module est « pas de filtre -> tout le
  // capturé » : sans ce garde, un personnage sans hat effect équipé faisait dessiner TOUT le tampon
  // sur le doll (constaté côté lab : les nuages d'ambiance de gonryun redessinés par-dessus l'écran).
  // On ne sort PAS si la famille « hôte particule » est demandée : elle est anonyme, donc légitimement
  // sans id — c'est le cas de la surface d'aperçu.
  if (id_count == 0 && !o.include_str_particle) return;

  float ax, ay, S;
  if (!ez_capture::ProjectAnchor(o, &ax, &ay, &S) || S <= 1e-4f) return;
  const int count = ez_capture::Count();
  const float R = (s / S) * g_ez_cal;
  o.scale = R;
  // FIT : bbox figée UNE fois (l'effet garde ~la même étendue -> évite que le scale du doll jitter à
  // chaque frame d'anim). RenderPlayerAvatar lit g_ez_fz_dx0.. AVANT ce dessin. Invalidée au changement
  // de hat effect (handler 0x0A3B). ax/ay/S restent LIVE (l'ancre s'annule dans (vx-ax), cf. supra).
  // ⚠ On itère sur p.n sommets RÉELS (3 ou 4) : lire un 4e sommet inexistant ramasserait le pointeur
  // de texture et les blends réinterprétés en float -> bbox aberrante.
  // ⚠ SEUL LE DOLL MESURE. La bbox figée ne sert qu'au cadrage du doll (lue par RenderPlayerAvatar),
  // mais cette fonction est appelée AUSSI par le tooltip d'aperçu : le laisser écrire figeait une
  // bbox incluant l'effet SURVOLÉ, et le cadre du doll se redimensionnait pour un effet qu'il ne
  // dessine même pas (constaté 2026-07-19). Un état partagé de plus, avec deux écrivains.
  if (!with_preview && !g_ez_frozen_valid && count > 0) {
    const ez_capture::Prim* caps = ez_capture::Prims();  // LIVE (animé)
    float dx0 = 1e9f, dx1 = -1e9f, dy0 = 1e9f, dy1 = -1e9f;
    const float mr2 = g_ez_max_r * g_ez_max_r, invS = 1.0f / S;
    for (int i = 0; i < count; ++i) {
      const ez_capture::Prim& p = caps[i];
      // ⚠ MÊME filtre que le dessin : sans ça, un effet capturé mais NON dessiné (typiquement
      // l'aperçu survolé ailleurs) élargit la bbox et fait rétrécir le cadre du doll pour « faire
      // tenir » quelque chose qui n'y est pas affiché.
      if (!ez_capture::Matches(p, o)) continue;
      for (int k = 0; k < p.n; ++k) {
        const float ddx = p.x[k] - ax, ddy = p.y[k] - ay;
        if (ddx * ddx + ddy * ddy > mr2) continue;
        if (ddx < dx0) dx0 = ddx; if (ddx > dx1) dx1 = ddx;
        if (ddy < dy0) dy0 = ddy; if (ddy > dy1) dy1 = ddy;
      }
    }
    if (dx1 >= dx0) {
      g_ez_fz_dx0 = dx0 * invS; g_ez_fz_dx1 = dx1 * invS;
      g_ez_fz_dy0 = dy0 * invS; g_ez_fz_dy1 = dy1 * invS;
      g_ez_frozen_valid = true;
    }
  }
  if (!g_ez_enabled) return;  // dessin désactivé (calibrage) — doll propre
  // Dessin délégué : le module gère le z-order (bit 0x8), le blend PAR PRIMITIVE (celui de
  // l'enregistrement) et les couleurs PAR SOMMET (dégradés d'alpha des traînées préservés).
  // `o` est rempli plus haut (l'ancre et l'échelle en dépendent).
  ez_capture::Draw(dl, o);
}

// ── Spawn / pilotage d'un nœud d'effet STR autonome (hat effect) ──────────────
// Recette RE (docs/hat_effect_re.md, agent 2026-07-12) : alloc(0x11ca8)+ctor,
// Effect_LoadStrByEffectId(id CONCRET), horloge d'anim = node+0x178 (ms relatif),
// Effect_UpdateStrKeyframes remplit les workBuf, puis on ITÈRE les couches nous-mêmes
// (SANS les gardes visibilité de Effect_DrawStrFrameQuads) en appelant le SubmitStrQuad
// HOOKÉ (capture). Nœuds MIS EN CACHE par id concret (persistants : pas d'alloc/dtor
// par frame, pas de gestion de durée de vie fragile ; ~72 Ko × nb d'effets distincts).
constexpr uintptr_t kEffCtor     = 0x00b90780;  // EffectInst_Ctor_StrNode(node)
constexpr uintptr_t kEffLoadStr  = 0x00bb4170;  // Effect_LoadStrByEffectId(node,src,id,x,y,z)
constexpr uintptr_t kEffUpdateKF = 0x00bced10;  // Effect_UpdateStrKeyframes(node,offXYZ,f,f)
constexpr int       kEffNodeSize = 0x11ca8;     // taille du nœud (alloc)
constexpr int kEffCStr       = 0x7ec;    // CStr* chargé (0 si échec de chargement)
constexpr int kEffLayerBase  = 0x7f0;    // valeur = CStr+0x110 (base couches/keyframes)
constexpr int kEffClockMs    = 0x178;    // horloge d'anim (ms) — pilotée par nous
constexpr int kEffWorkBuf0   = 0x7f4;    // workBuf couche L = node+0x7f4+L*0x74
constexpr int kEffActiveFlag = 0x119fc;  // flag actif couche L = node+0x119fc+L (octet)
constexpr int kEffLoopMax    = 0x11c80;  // boucles max (compteur node+0x11c7c ; Effect_ResetStrLoop rembobine +0x178=0)
// Cadence native EXACTE (RE : GameLogic_ComputeTimestepCount 0x00c16a90) : le pas fixe est
// DAT_015e5a9c = 0x10 = 16 ms/frame (flag DAT_0122b3dc=1) -> 62.5 Hz, INDÉPENDANT du FPS de rendu.
// node+0x178 (index de frame ENTIER) avance de +1 toutes les 16 ms. f(T) = floor(T_ms / 16).
constexpr unsigned kStrFrameMs = 16;  // ms par frame .str (pas fixe natif, non deviné)

// Bridge Lua brut (Lua 5.1) — même mécanique que StatusName (character_sheet) : évite
// le wrapper varargs (string BYVAL). Adresses partagées avec les autres plugins.

// Un nœud d'effet en cache : le nœud lui-même + son tick de départ (horloge relative :
// node+0x178 = GetTickCount()-start, sinon un tick absolu dépasserait toutes les
// keyframes -> effet « fini » = rien). failed = chargement échoué (ne pas re-tenter).
struct StrEffNode {
  void*    node    = nullptr;
  DWORD    last    = 0;      // GetTickCount du dernier tick appliqué (cadence delta temps réel)
  unsigned acc     = 0;      // reste de ms (< 16) pas encore converti en frame native
  bool     primed  = false;  // frame 0 rendue au moins une fois
  bool     failed  = false;
};
std::unordered_map<int, StrEffNode> g_str_nodes;  // id concret -> nœud
float g_str_srcpos[0x20] = {0};                   // struct srcPos (XYZ @+0x10), zéro

// Table itemId(client) -> ordinal de hat effect (e_hat_effects), poussée par le serveur
// au login (ZC 0x0F17, cf. clif_bourgeon_hateffect_map). AUTORITATIVE (scan des scripts
// item_db) : le client ne mappe pas item->ordinal nativement (effectHatItemTable = simple
// appartenance). Sert à ItemToHatOrdinal pour prévisualiser les costumes sans viewid.
std::unordered_map<uint32_t, uint16_t> g_hat_item_ord;

// ── Diagnostic (POD) du dernier pilotage d'effet — rempli par les fonctions SEH, loggé
// throttlé (fmt) par l'appelant hors __try (RenderPlayerAvatar). Sert à localiser où la
// chaîne casse : résolution ordinal->concret, charge du .str, comptage de couches, capture.
int g_hatfx_dg_concrete = 0;   // id concret pilotté (0 = résolution ratée)
int g_hatfx_dg_nodeok   = -1;  // -1 non tenté, 0 échec charge/.str absent, 1 nœud chargé
int g_hatfx_dg_layers   = -1;  // layerCount du .str (nb de couches)
int g_hatfx_dg_captured = -1;  // g_str_count après capture (nb de couches capturées)

// Résolution NATIVE ordinal hat -> nom/chemin du .str, via le global Lua GetHatEfResName
// (cf. Effect_ResolveResourceName 0x00af0900). C'est LE résolveur des hat effects par NOM
// (GetHatEffectID renvoie -1 pour eux). Ex : ordinal 48 -> "efst_Gold_Shower\coin2.str".
// POD ONLY (SEH). out vidé si échec.
void HatOrdinalToResNameRaw(int ordinal, char* out, int cap) {
  if (cap <= 0) return;
  out[0] = '\0';
  __try {
    void* L = lua::State();
    if (L) {
      lua::GetField(
          L, lua::kGlobalsIndex, "GetHatEfResName");
      lua::PushNumber(
          L, static_cast<double>(ordinal));
      if (lua::PCall(L, 1, 1, 0) == 0) {
        const char* s = lua::ToLString(L, -1, nullptr);
        if (s && s[0]) { std::strncpy(out, s, cap - 1); out[cap - 1] = '\0'; }
      }
      lua::SetTop(L, -2);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// Wrapper caché (map = op C++, hors __try). Renvoie le nom .str de l'ordinal, "" si échec
// (non figé : Lua peut ne pas être prêt au 1er appel).
const char* HatOrdinalToResName(int ordinal) {
  static std::unordered_map<int, std::string> cache;
  auto it = cache.find(ordinal);
  if (it != cache.end()) return it->second.c_str();
  char buf[96];
  HatOrdinalToResNameRaw(ordinal, buf, sizeof(buf));
  if (buf[0]) return (cache[ordinal] = buf).c_str();
  return "";
}

// Paramètres d'un hat effect (Lua, AUCUN hardcode) :
//   pos_x  = GetHatEfPosX(ord)           : décalage horizontal (HatEffectInfo.lub)
//   before= IsRenderBeforeCharacter(ord): effet DERRIÈRE le perso (true) ou devant (false)
// (Le décalage VERTICAL des effets "tête/scène" vient d'un nudge ÉCRAN -80 natif
//  (CActorSprite_ComputeStrScreenAnchor), NON reproductible à plat sans l'échelle de rendu en jeu S.
//  On ancre donc tous les effets sur l'ORIGINE de l'acteur (pieds) et le .str se place lui-même :
//  cercle magique au sol, pièces au-dessus, etc. -> correct pour les effets sol, un peu bas pour
//  les effets tête/scène qui voudraient le -80.)
struct HatEffectParams { float pos_x = 0.0f; bool before = false; };

// Appel brut d'un global Lua getter(ord). Nombre : lua_tolstring+atof (le natif convertit les
// nombres en place, cf. GetHatEfResName). Booléen : lua_toboolean. POD/SEH. `def` si Lua KO.
float HatLuaNum(int ordinal, const char* fn, float def) {
  float r = def;
  __try {
    void* L = lua::State();
    if (L) {
      lua::CheckStack(L, 3);  // comme le natif : garantit la place
      lua::GetField(L, lua::kGlobalsIndex, fn);
      lua::PushNumber(L, static_cast<double>(ordinal));
      if (lua::PCall(L, 1, 1, 0) == 0) {
        // Résultat NUMÉRIQUE lu via lua_tonumber (comme le natif Lua_CallGlobal_va pour 'd'/'l').
        // lua_tolstring convertissait aussi mais on évite le round-trip string+atof (fragile).
        const double d = lua::ToNumber(L, -1);
        r = static_cast<float>(d);
      }
      lua::SetTop(L, -2);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { r = def; }
  return r;
}
bool HatLuaBool(int ordinal, const char* fn, bool def) {
  bool r = def;
  __try {
    void* L = lua::State();
    if (L) {
      lua::GetField(L, lua::kGlobalsIndex, fn);
      lua::PushNumber(L, static_cast<double>(ordinal));
      if (lua::PCall(L, 1, 1, 0) == 0)
        r = lua::ToBoolean(L, -1) != 0;
      lua::SetTop(L, -2);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { r = def; }
  return r;
}
// Caché par ordinal. NE met en cache QUE si Lua est prêt (sinon repli non figé -> réessai).
const HatEffectParams& HatOrdinalParams(int ordinal) {
  static std::unordered_map<int, HatEffectParams> cache;
  static const HatEffectParams s_fallback;
  auto it = cache.find(ordinal);
  if (it != cache.end()) return it->second;
  if (!lua::State()) return s_fallback;  // Lua pas prêt -> pas de cache, on réessaiera
  HatEffectParams p;
  p.pos_x   = HatLuaNum(ordinal, "GetHatEfPosX", 0.0f);
  p.before = HatLuaBool(ordinal, "IsRenderBeforeCharacter", false);
  return cache[ordinal] = p;
}

// Adresses de chargement de ressource (comme cashshop_window) + AddRef (0x00a8e800).
constexpr uintptr_t kTexAddRef = 0x00a8e800;  // UITexture_AddRef(obj) (__fastcall, ecx=obj)
constexpr int       kEffDummyId = 0x59;       // StormGust : id concret bidon (setup natif complet)

// Crée un nœud d'effet STR chargé du .str `strName` (par NOM). POD ONLY (SEH -> pas de
// C2712). Stratégie : ctor + Effect_LoadStrByEffectId avec un id concret BIDON (StormGust)
// pour le setup COMPLET du nœud (préambule + init vtable), puis on REMPLACE la ressource
// .str par la nôtre (queue de Effect_LoadStrByEffectId : UITextureMgr_Load + AddRef + champs
// +0x7ec/+0x7f0/+0x119f8). +0x11c80=9999 -> boucle (aperçu permanent). nullptr si échec.
void* StrNode_CreateByName(const char* strName) {
  if (!strName || !strName[0]) return nullptr;
  void* node = std::calloc(1, static_cast<size_t>(kEffNodeSize));
  if (!node) return nullptr;
  bool ok = false;
  __try {
    reinterpret_cast<void(__fastcall*)(void*)>(kEffCtor)(node);
    reinterpret_cast<void(__fastcall*)(void*, void*, void*, int, float, float, float)>(
        kEffLoadStr)(node, nullptr, g_str_srcpos, kEffDummyId, 0.0f, 0.0f, 0.0f);
    void* mgr = reinterpret_cast<void*(__cdecl*)()>(ro::texmgr::kGet)();
    void* key = reinterpret_cast<void*(__cdecl*)(const char*)>(ro::texmgr::kMakeKey)(strName);
    void* strObj = (mgr && key)
        ? reinterpret_cast<void*(__fastcall*)(void*, void*, void*)>(ro::texmgr::kLoad)(mgr, nullptr, key)
        : nullptr;
    if (strObj) {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + kEffCStr) = strObj;
      reinterpret_cast<void(__fastcall*)(void*)>(kTexAddRef)(strObj);  // garde le .str vivant
      *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + kEffLayerBase) =
          reinterpret_cast<char*>(strObj) + 0x110;
      const int fc = *reinterpret_cast<int*>(reinterpret_cast<char*>(strObj) + 0x118);
      *reinterpret_cast<int*>(reinterpret_cast<char*>(node) + 0x119f8) =
          (*reinterpret_cast<int*>(reinterpret_cast<char*>(strObj) + 0x128) == 0) ? fc - 1 : fc;
      // Boucle "à l'infini" = VALEUR NATIVE EXACTE : les auras qui bouclent (gc_darkcrow, chill…)
      // mettent 9999 dans node+0x11c80 (RE : Effect_LoadStrByEffectId). On la réplique telle quelle.
      *reinterpret_cast<int*>(reinterpret_cast<char*>(node) + kEffLoopMax) = 9999;
      // NB : on NE modifie NI les noms NI le cache du CStr (partagé avec le rendu in-world natif
      // -> corruption/crash). La résolution du BON chemin texture (avec sous-dossier) se fait
      // dans le hook Hooked_StrQuad via le factory (cache par chemin), cf. g_str_cap_subdir.
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  if (!ok) { std::free(node); return nullptr; }
  return node;
}

// Pilote le nœud À LA MANIÈRE DU NATIF puis ITÈRE les couches -> capture via le hook.
// RE (Effect_UpdateStrKeyframes 0x00bced10 + Effect_ResetStrLoop 0x00bb5b10) : node+0x178 est un
// INDEX DE FRAME ENTIER que le driver natif incrémente de +1 par tick. L'avance d'un keyframe
// n'a lieu QUE si node+0x178 == kf.endFrame EXACTEMENT (égalité entière) -> il FAUT visiter chaque
// entier (donc +1 à la fois, jamais un saut = elapsed_ms). Quand toutes les couches finissent,
// UpdateKF appelle Effect_ResetStrLoop qui rembobine node+0x178=0 -> la BOUCLE est INTERNE au
// nœud (plafond node+0x11c80). Donc : on n'écrit JAMAIS une horloge absolue ; on incrémente de
// `steps` (relatif), 1 UpdateKF par incrément. `prime` = 1er tick -> rend la frame 0 sans avancer.
// POD ONLY (SEH, aucun objet C++ -> évite C2712).
void StrNode_DriveCapture(void* node, int steps, bool prime) {
  __try {
    int* clock = reinterpret_cast<int*>(reinterpret_cast<char*>(node) + kEffClockMs);
    float off3[3] = {0.0f, 0.0f, 0.0f};
    const auto updkf =
        reinterpret_cast<unsigned(__fastcall*)(void*, void*, void*, float, float)>(kEffUpdateKF);
    if (prime) { *clock = 0; updkf(node, nullptr, off3, 0.0f, 0.0f); }  // frame 0
    for (int i = 0; i < steps; ++i) {
      *clock += 1;                              // +1 frame native (relatif ; boucle interne rembobine)
      updkf(node, nullptr, off3, 0.0f, 0.0f);   // avance keyframes (match exact) + remplit workBuf
    }
    void* layerBase =
        *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + kEffLayerBase);
    if (layerBase) {
      int layerCount = *reinterpret_cast<int*>(reinterpret_cast<char*>(layerBase) + 8);
      if (layerCount < 1) layerCount = 1;
      if (layerCount > 48) layerCount = 48;  // borne sûre (évite d'itérer des couches fantômes)
      g_hatfx_dg_layers = layerCount;  // diag
      g_str_cap_active = true;
      int depthIdx = 0;
      // Réplique la boucle de Effect_DrawStrFrameQuads mais SANS les gardes (FUN_00d9d020
      // effets-activés / FUN_00c0afa0 visible) qui recaleraient un nœud autonome.
      for (int Lyr = 1; Lyr < layerCount && g_str_count < 64; ++Lyr) {
        if (*(reinterpret_cast<char*>(node) + kEffActiveFlag + Lyr) != 0) {
          void* layerStruct = reinterpret_cast<char*>(layerBase) + 0x10 + Lyr * 0x380;
          float* workBuf = reinterpret_cast<float*>(
              reinterpret_cast<char*>(node) + kEffWorkBuf0 + Lyr * 0x74);
          // Appel du SubmitStrQuad HOOKÉ -> Hooked_StrQuad capture (camera ignoré).
          reinterpret_cast<StrSubmitFn>(kStrSubmitQuad)(
              node, nullptr, layerStruct, workBuf, depthIdx, nullptr);
          depthIdx += 2;
        }
      }
      g_str_cap_active = false;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { g_str_cap_active = false; }
}

// Pilote le nœud du .str de `concreteId` (crée/charge au 1er appel, cache ensuite) et
// CAPTURE ses couches 2D dans g_str_caps via le hook. Réinitialise g_str_count. No-op si
// l'effet ne charge pas. La MAP (C++) et le SEH sont dans des fonctions SÉPARÉES (C2712 :
// MSVC interdit __try dans une fonction qui déroule aussi des objets C++).
void CaptureHatEffectOrdinal(int ordinal) {
  InstallStrCapture();
  g_str_count = 0;
  g_hatfx_dg_concrete = ordinal;
  g_hatfx_dg_nodeok = -1; g_hatfx_dg_layers = -1; g_hatfx_dg_captured = -1;  // reset diag
  if (!g_orig_str_quad || ordinal <= 0) return;
  StrEffNode& slot = g_str_nodes[ordinal];  // clé = ordinal (crée l'entrée si absente)
  if (slot.failed) { g_hatfx_dg_nodeok = 0; return; }
  if (!slot.node) {
    const char* name = HatOrdinalToResName(ordinal);  // GetHatEfResName -> .str
    if (!name || !name[0]) { g_hatfx_dg_nodeok = 0; return; }  // Lua pas prêt : retenter (pas failed)
    void* node = StrNode_CreateByName(name);
    if (!node) { slot.failed = true; g_hatfx_dg_nodeok = 0; return; }
    slot.node = node;
    slot.last = GetTickCount();
    slot.acc = 0;
  }
  g_hatfx_dg_nodeok = 1;
  // dir du .str (partie dossier de GetHatEfResName, ex. "efst_Gold_Shower\") pour le résolveur
  // NATIF subdir-aware du hook — CASSE D'ORIGINE (comme _splitpath natif ; rend in-world donc OK).
  // "" -> effet racine. Remplit aussi la std::string POD passée à Str_GetLayerTextureWithDir.
  {
    const char* name = HatOrdinalToResName(ordinal);  // caché
    const char* bs = name ? std::strrchr(name, '\\') : nullptr;
    if (bs) {
      char dir[80];
      int n = static_cast<int>(bs - name) + 1;         // inclut le '\' final (comme _splitpath)
      if (n > 0 && n < static_cast<int>(sizeof(dir))) {
        std::memcpy(dir, name, static_cast<size_t>(n));
        dir[n] = '\0';
        SetStrCapDir(dir);
      } else {
        SetStrCapDir("");
      }
    } else {
      SetStrCapDir("");
    }
  }
  // Cadence temps réel EXACTE (16 ms/frame natif) par ACCUMULATEUR DE DELTA : on ne compte que le
  // temps écoulé entre deux captures RÉELLES (la fiche ne rend/tick que visible), donc pas de
  // fast-forward à la réouverture. steps = frames natives dues ce tick ; le reste <16 ms est gardé
  // dans slot.acc. La BOUCLE est interne au nœud (Effect_ResetStrLoop rembobine node+0x178) ->
  // aucun reset d'horloge ici. Après un gros trou (fiche masquée), on borne + on vide le backlog.
  const DWORD now = GetTickCount();
  slot.acc += now - slot.last;   // ms écoulées depuis le dernier tick (delta, non absolu)
  slot.last = now;
  int steps = static_cast<int>(slot.acc / kStrFrameMs);
  slot.acc -= static_cast<unsigned>(steps) * kStrFrameMs;
  if (steps > 8) { steps = 8; slot.acc = 0; }  // trou (fiche masquée) : reprendre, pas rattraper
  StrNode_DriveCapture(slot.node, steps, !slot.primed);
  slot.primed = true;
  g_hatfx_dg_captured = g_str_count;
}

// ── Paperdoll d'un personnage ARBITRAIRE (char-select) ───────────────────────
// Les trois captures ci-dessus (portrait / aperçu / avatar) rendent TOUJOURS le
// personnage CONNECTÉ : elles lisent l'apparence dans les globals de session
// (rag::kSessionAddr, g_OwnLook_*) et prennent la fenêtre BasicInfo (0x0131f6c4) comme
// contexte de rendu. Au char-select, ni l'un ni l'autre n'existe -> capture vide.
// Ce chemin-ci est donc AUTONOME :
//   (a) apparence passée en paramètre (DollLook) au lieu des globals ;
//   (b) contexte de rendu FACTICE (DollRenderCtx) au lieu d'une UIWindow ;
//   (c) buffer de capture dédié (g_doll_caps) + cache (g_doll_cache).
// Il réutilise en revanche le SEUL moteur de capture (hook sur
// Actor_SubmitSpriteQuad 0x00a1b7c0 + EmitCapLayer) : ce hook est global, il ne
// peut pas y en avoir un second.
// Séquence par personnage = celle du char-select NATIF
// (UINewSelectCharWnd_RenderSlots 0x0079d170) : Actor_Init -> Actor_DrawSprites(1)
// -> Actor_Dtor, SANS arme (+0x5a) ni bouclier (+0x62) — le natif ne les rend pas.

constexpr uintptr_t kUiWndFadeColor = 0x00a1edf0;  // UIWindow_GetFadeColor (vtbl+0xa0)

// Contexte de rendu FACTICE. RE (Actor_Init 0x007ac210 + Actor_DrawSprites
// 0x007ac820, chemin quad param=1) : le « render ctx » est seulement STOCKÉ dans
// actor+0x04 par le ctor (aucun déréférencement), et sur le chemin quad il ne sert
// qu'à deux choses :
//   1. un appel virtuel vtbl+0xa0 == UIWindow_GetFadeColor, dont la couleur part
//      dans le submit — et que notre capture IGNORE ;
//   2. le `this` d'Actor_SubmitSpriteQuad — que le hook SUPPRIME pendant la capture.
// (Le déréférencement ctx+0x24 du décompilé n'est QUE sur la branche NON-quad, qu'on
// ne prend jamais.) Un objet de 0x100 octets dont seule l'entrée 40 de vtable est
// renseignée suffit donc — et lui, il existe hors jeu.
// UIWindow_GetFadeColor ne lit que +0x38 (alpha cible), +0x3c (tick du fondu) et
// +0x40 (alpha courant, qu'il RÉÉCRIT) : cible == courant == 0xff -> il renvoie
// 0xffffffff sans toucher à rien d'autre. Le reste à zéro = état vide légal.
struct FakeRenderCtx {
  void*         vtbl;
  unsigned char pad[0xFC];
};

void* DollRenderCtx() {
  static void*         s_vtbl[48] = {};  // 48 entrées = 0xc0 o > slot 40 (= +0xa0)
  static FakeRenderCtx s_ctx = {};
  s_vtbl[40] = reinterpret_cast<void*>(kUiWndFadeColor);
  s_ctx.vtbl = s_vtbl;
  unsigned char* p = reinterpret_cast<unsigned char*>(&s_ctx);
  *reinterpret_cast<unsigned*>(p + 0x38) = 0xff;            // alpha cible
  *reinterpret_cast<unsigned*>(p + 0x3c) = GetTickCount();  // tick du fondu
  *reinterpret_cast<unsigned*>(p + 0x40) = 0xff;            // alpha courant
  return &s_ctx;
}

// ── Cache de dolls ───────────────────────────────────────────────────────────
// Une capture = un aller-retour NATIF complet (Actor_Init + résolution/chargement
// des .spr/.act + Actor_DrawSprites) ; la grille du char-select peut afficher 45
// slots (bientôt 60). On mémorise donc les couches par APPARENCE (clé = signature
// des champs de DollLook + direction : deux persos identiques partagent l'entrée,
// et tout changement d'apparence en crée une nouvelle) et on borne le nombre de
// captures par frame.
//
// Pourquoi ré-capturer, puisque la pose est figée ? Parce qu'une couche capturée
// référence une PAGE d'atlas + des UV, et l'atlas est un cache LRU (cellule,palette)
// -> CTexture (cf. docs/sprite_rendering_re.md) : si la cellule est évincée puis
// réallouée, les UV mémorisées pointeraient sur les mauvais pixels. Ré-capturer
// ré-appelle SpriteAtlas_GetCachedTexture, ce qui remet la cellule en tête de LRU
// et rafraîchit le handle de page — même raison que le « ré-résolu chaque frame »
// de login_parade.
// Et le device D3D9 ? Un reset détruit les textures : chaque entrée retient
// l'Overlay_DeviceEpoch de sa capture et n'est JAMAIS dessinée si l'epoch a changé
// (sinon on dessinerait un pointeur mort -> crash ddraw).
constexpr int   kDollCacheSize  = 64;   // > 60 slots serveur : pas de thrash
constexpr int   kDollMaxLayers  = 24;   // corps+tête+3 coiffes+garment ≈ 7 en pratique
constexpr DWORD kDollRefreshMs  = 500;  // rafraîchissement souhaité (re-pin LRU atlas)
// Péremption DURE : au-delà, une entrée n'est PLUS DESSINÉE tant qu'elle n'a pas été
// re-capturée. Filet contre le seul scénario où un handle de texture peut réellement
// mourir sous nous : le cache est statique, il survit à une session de jeu, et au
// retour au char-select les pages d'atlas de la session précédente peuvent avoir été
// libérées. 2 s >> le temps qu'il faut au budget (2/frame) pour rafraîchir tout
// l'écran, donc en régime normal on ne refuse jamais de dessiner.
constexpr DWORD kDollExpireMs   = 2000;
constexpr int   kDollPerFrame   = 2;    // budget de captures NATIVES par frame

CapLayer g_doll_caps[48];
int      g_doll_count = 0;

struct DollCacheEntry {
  unsigned key   = 0;  // signature apparence+direction (0 = entrée libre)
  unsigned epoch = 0;  // Overlay_DeviceEpoch() au moment de la capture
  DWORD    stamp = 0;  // GetTickCount() de la capture (péremption)
  DWORD    used  = 0;  // GetTickCount() du dernier usage (éviction LRU)
  int      count = 0;  // couches valides (0 = capture vide : on retentera)
  // Cadrage figé, en unités acteur : centre X + pieds du CORPS (ancrage stable,
  // insensible aux coiffes larges), sommet de la bbox TOTALE et demi-largeur
  // maximale depuis le centre (aucun côté ne sort du cadre).
  float    cx = 0.0f, feet = 0.0f, top = 0.0f, half = 0.0f;
  CapLayer layers[kDollMaxLayers];
};
DollCacheEntry g_doll_cache[kDollCacheSize];

// Budget de captures par frame (frontière de frame = ImGui::GetFrameCount).
int g_doll_budget = 0;
int g_doll_budget_frame = -1;
bool DollTakeBudget() {
  const int f = ImGui::GetFrameCount();
  if (f != g_doll_budget_frame) {
    g_doll_budget_frame = f;
    g_doll_budget = kDollPerFrame;
  }
  if (g_doll_budget <= 0) return false;
  --g_doll_budget;
  return true;
}

// Signature d'apparence (FNV-1a) : clé du cache. Jamais 0 (0 = entrée libre).
// `anim` (type d'action) fait partie de la clé : debout et assis d'un MÊME perso
// sont deux vignettes distinctes -> deux entrées de cache.
unsigned DollKey(const BasicInfo::DollLook& k, int dir, int anim) {
  const unsigned parts[] = {
      static_cast<unsigned>(k.sex),      static_cast<unsigned>(k.job),
      static_cast<unsigned>(k.body),     static_cast<unsigned>(k.hair),
      static_cast<unsigned>(k.hair_color), static_cast<unsigned>(k.clothes_color),
      static_cast<unsigned>(k.head_low), static_cast<unsigned>(k.head_top),
      static_cast<unsigned>(k.head_mid), static_cast<unsigned>(k.garment),
      static_cast<unsigned>(dir & 7),    static_cast<unsigned>(anim)};
  unsigned h = 2166136261u;
  for (unsigned p : parts) h = (h ^ p) * 16777619u;
  return h ? h : 1u;
}

// Capture UNE image (pose de face, image 0) du paperdoll `k` dans g_doll_caps.
// SEH-gardé : un échec laisse g_doll_count à 0 (l'appelant affichera un
// placeholder). Restaure la cible de capture du portrait à la sortie, comme les
// autres captures.
void CaptureDollActor(const BasicInfo::DollLook& k, int dir, int anim) {
  InstallActorCapture();
  if (!g_orig_actor_quad) return;
  void* render_ctx = DollRenderCtx();
  g_doll_count = 0;
  g_cap_buf = g_doll_caps;  // rediriger le hook vers le buffer doll
  g_cap_num = &g_doll_count;
  // g_first_layer reste FAUX exprès : le bloc « 1re couche » du hook écrit
  // g_av_body_scale / g_av_frame_delay / *g_frame_dst, qui appartiennent au doll de
  // la fiche perso. Notre pose est FIGÉE (une seule image) -> aucun de ces trois
  // états ne nous sert, et on ne les pollue pas. (g_frame_dst n'est donc pas touché.)
  g_first_layer = false;
  __try {
    using CtorFn = void*(__fastcall*)(void* self, void* edx, void* render_ctx,
        int x, int y, int sex, int job_short, int job_full, int job_body, int hair,
        int p9, int p10, int p11, int garment, int p13, int p14,
        int clothes_col, int hair_col, int pose, int frame, int p19);
    using DrawFn = void(__fastcall*)(void* self, void* edx, char param);
    using DtorFn = void(__fastcall*)(void* self, void* edx);
    alignas(8) unsigned char actor[0x200];
    std::memset(actor, 0, sizeof(actor));
    // job_body (actor+0x18) = CHARACTER_INFO+0x58 « body style ». Côté serveur
    // (rAthena) ce champ contient un ID DE CLASSE — pc.cpp fait
    // `status.body = status.class_` quand la valeur n'est pas dans job_db — et
    // Job_ResolveBodyClass 0x00d99150 le consomme comme tel. S'il vaut 0, le corps
    // retombe sur NOVICE : d'où le repli sur le job (garde ; le natif, lui, passe
    // +0x58 brut car le serveur le remplit toujours).
    const int body = (k.body != 0) ? k.body : k.job;
    // ⚠ ORDRE DES COIFFES (vérifié dans RenderSlots 0x0079d170) : les paramètres 9,
    // 10 et 11 reçoivent, DANS CET ORDRE, head LOW (+0x60), head TOP (+0x64) puis
    // head MID (+0x66). Les noms hg_top/hg_mid/hg_low des autres captures sont des
    // noms de COUCHE, pas des slots d'équipement — ne pas s'y fier ici.
    // pose = animType*8 + dir. animType 0 (repos) = vignette debout ; animType 2 =
    // ASSIS (pose 0x10 pour dir 0 = celle que le natif utilise pour un perso en
    // attente de suppression). image 0 = figée (le natif n'anime que via
    // rec[0x53]/[0x54], à 0 en temps normal).
    // p19 (actor+0x40) = tick de base, consommé par Act_ResolveAltAnimFrame (images
    // alternatives type clignement). On passe 0 — comme les autres captures — ce qui
    // rend la capture DÉTERMINISTE : indispensable ici, sinon deux captures de la
    // même apparence différeraient et le cache afficherait un état périmé.
    const int pose = anim * 8 + (dir & 7);
    reinterpret_cast<CtorFn>(kActorCtor)(
        actor, nullptr, render_ctx, /*x*/ 0, /*y*/ 0, k.sex,
        /*job_short*/ k.job & 0xffff, /*job_full*/ k.job, /*job_body*/ body,
        k.hair, /*p9*/ k.head_low, /*p10*/ k.head_top, /*p11*/ k.head_mid,
        k.garment, /*p13*/ 0, /*p14*/ 0, k.clothes_color, k.hair_color,
        /*pose*/ pose, /*frame*/ 0, /*p19*/ 0);
    g_cur_actor = actor;
    g_cap_active = true;
    reinterpret_cast<DrawFn>(kActorDraw)(actor, nullptr, 1);  // 1 => chemin quad
    g_cap_active = false;
    g_cur_actor = nullptr;
    reinterpret_cast<DtorFn>(kActorDtor)(actor, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_cap_active = false;
    g_cur_actor = nullptr;
  }
  g_cap_buf = g_caps;  // restaurer la cible portrait (contrat commun aux captures)
  g_cap_num = &g_cap_count;
}

// Vide le cache de dolls. Appelé à chaque changement de mode : la scène (et donc les
// pages de l'atlas de sprites) est reconstruite, les handles mémorisés n'ont plus
// aucune raison d'être valides.
void DollCacheInvalidateAll() {
  for (int i = 0; i < kDollCacheSize; ++i) {
    g_doll_cache[i].key = 0;
    g_doll_cache[i].count = 0;
    g_doll_cache[i].used = 0;
  }
}

// Entrée du cache pour `key` sous l'epoch device courant, ou nullptr.
DollCacheEntry* DollCacheFind(unsigned key, unsigned epoch) {
  for (int i = 0; i < kDollCacheSize; ++i)
    if (g_doll_cache[i].key == key && g_doll_cache[i].epoch == epoch)
      return &g_doll_cache[i];
  return nullptr;
}

// Entrée libre, sinon la moins récemment utilisée (LRU).
DollCacheEntry* DollCacheEvict() {
  DollCacheEntry* best = &g_doll_cache[0];
  for (int i = 0; i < kDollCacheSize; ++i) {
    if (g_doll_cache[i].key == 0) return &g_doll_cache[i];
    if (g_doll_cache[i].used < best->used) best = &g_doll_cache[i];
  }
  return best;
}

// Range la capture courante (g_doll_caps) dans `e` et fige le cadrage. Une capture
// VIDE est mémorisée telle quelle (count 0) : elle sera retentée à la péremption
// (sprites pas encore chargés), sans re-tenter à chaque frame.
void DollCacheFill(DollCacheEntry* e, unsigned key, unsigned epoch, DWORD now) {
  const bool same = (e->key == key && e->epoch == epoch);
  int n = g_doll_count;
  if (n > kDollMaxLayers) n = kDollMaxLayers;
  if (n < 0) n = 0;
  // Rafraîchissement qui échoue alors qu'on avait déjà les couches : on GARDE le
  // rendu précédent (aucun clignotement) et on retentera à la prochaine péremption.
  // ⚠ Seulement si c'est la MÊME apparence : sur une entrée recyclée (autre clé) il
  // faut écraser, sinon on afficherait le perso précédent.
  if (n == 0 && same && e->count > 0) {
    e->stamp = now;
    e->used = now;
    return;
  }
  e->key = key;
  e->epoch = epoch;
  e->stamp = now;
  e->used = now;
  e->count = n;
  if (n > 0)
    std::memcpy(e->layers, g_doll_caps, static_cast<size_t>(n) * sizeof(CapLayer));
  // Cadrage (mêmes règles que l'avatar de la fiche, mais sur UNE image puisque la
  // pose est figée) : bbox TOTALE pour la taille, bbox CORPS (couches non-tête =
  // torse/jambes) pour centrer en X et poser les pieds. Une coiffe large ne
  // décentre donc pas le perso.
  float ax0 = 1e9f, ay0 = 1e9f, ax1 = -1e9f, ay1 = -1e9f;
  float bx0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  bool has_body = false;
  for (int i = 0; i < n; ++i) {
    const CapLayer& L = e->layers[i];
    const float lx0 = L.cx - L.w * 0.5f, lx1 = L.cx + L.w * 0.5f;
    const float ly0 = L.cy - L.h * 0.5f, ly1 = L.cy + L.h * 0.5f;
    if (lx0 < ax0) ax0 = lx0;  if (lx1 > ax1) ax1 = lx1;
    if (ly0 < ay0) ay0 = ly0;  if (ly1 > ay1) ay1 = ly1;
    if (!L.head_region) {
      has_body = true;
      if (lx0 < bx0) bx0 = lx0;  if (lx1 > bx1) bx1 = lx1;
      if (ly1 > by1) by1 = ly1;
    }
  }
  if (n <= 0) { e->cx = e->feet = e->top = e->half = 0.0f; return; }
  e->cx   = has_body ? (bx0 + bx1) * 0.5f : (ax0 + ax1) * 0.5f;
  e->feet = has_body ? by1 : ay1;
  e->top  = ay0;
  const float hr = ax1 - e->cx, hl = e->cx - ax0;
  e->half = (hr > hl) ? hr : hl;
}
}  // namespace

void BasicInfo::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
  // Dolls du char-select : le cache retient des pages d'atlas de sprites, et la scène
  // est reconstruite à chaque changement de mode -> on repart de zéro (les entrées
  // seront re-capturées à la demande).
  DollCacheInvalidateAll();
  // Repart d'une liste vide à l'entrée en jeu : le serveur re-pousse la liste
  // complète des hat effects au spawn (0x0A3B status=1). Évite qu'un effet d'un
  // perso précédent persiste (aucun status=0 n'est émis à la déconnexion).
  if (in_game_) { own_hat_effects_.clear(); g_ez_frozen_valid = false; }
}

// Tooltip d'aperçu d'un équipement porté par le perso (appelé par item_desc au
// survol de « ViewID : N »). Capture le perso + l'item (viewID dans le slot) via
// le moteur du portrait, puis composite les sprites dans un tooltip ImGui. Ne
// fait rien si l'item n'est pas un headgear/garment (slot PV_NONE).
bool BasicInfo::CanPreview(int emplacement) const {
  return MapEmplacementToSlot(emplacement) != PV_NONE;
}

void BasicInfo::RenderItemPreviewTooltip(int view_id, int emplacement,
                                               int hat_ordinal) {
  // Rien à montrer si ni sprite (viewid) ni effet (hat effect) : sortie.
  if (view_id == 0 && hat_ordinal == 0) return;
  const PvSlot slot = MapEmplacementToSlot(emplacement);
  // Un sprite non-prévisualisable (slot inconnu) SANS effet -> rien. Avec un hat effect,
  // on rend quand même le perso de BASE (view 0) + l'effet.
  if (slot == PV_NONE && hat_ordinal == 0) return;
  // Réserve la molette à l'item survolé (rotation du perso) : ImGui ne scrolle plus
  // AUCUNE fenêtre à la molette pendant l'aperçu (ex. la scrollbar de la description qui
  // se trouve dessous). L'API prévue pour ça : SetItemKeyOwner sur le dernier item survolé.
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
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
  // Hat effect (.str) superposé : MÊME logique que l'avatar (ancre = ORIGINE + hatEffectPos(X) Lua ;
  // ordre derrière/devant = isRenderBeforeCharacter ; cf. RenderPlayerAvatar). Zéro hardcode.
  static const HatEffectParams s_hp_none_pv;
  const HatEffectParams& hp = hat_ordinal ? HatOrdinalParams(hat_ordinal) : s_hp_none_pv;
  // Deux familles : .str name-based (resname NON vide) = chemin piloté off-screen (DrawStrCapLayers) ;
  // sinon (resname vide) = effet EZ/CEffectMgr (hatEffectID, ex. Digital_Space/Perm_Frost) qu'on ne peut
  // PAS piloter hors-scène -> on le SPAWNE temporairement sur le joueur (RequestEzPreview) et on capture
  // le rendu live (ez_capture), composité comme le doll (z-order par primitive via DrawEzCapTris).
  const char* rn_pv = hat_ordinal ? HatOrdinalToResName(hat_ordinal) : "";
  const bool isStr = (rn_pv && rn_pv[0]);
  // Ne PAS spawn l'aperçu si l'effet est DÉJÀ équipé : il rend déjà sur le joueur (capturé/affiché tel
  // quel). Sinon le toggle-remove de l'aperçu retirerait l'instance ÉQUIPÉE (même id) -> script hateffect
  // « overruled » jusqu'à @refresh. On ne spawne QUE les effets NON équipés (le vrai « try before buy »).
  const bool alreadyEquipped =
      std::find(own_hat_effects_.begin(), own_hat_effects_.end(),
                static_cast<uint16_t>(hat_ordinal)) != own_hat_effects_.end();
  if (hat_ordinal && !isStr && !alreadyEquipped) RequestEzPreview(hat_ordinal);  // « try before buy »
  auto drawPreviewHat = [&]() {
    if (hat_ordinal == 0 || !isStr) return;
    CaptureHatEffectOrdinal(hat_ordinal);
    hat_diag_concrete_ = hat_ordinal;
    hat_diag_layers_   = g_str_count;
    // Ancre = ORIGINE de l'acteur (oy) comme l'avatar : le .str se place lui-même (sol/tête/centré).
    DrawStrCapLayers(dl, ox + hp.pos_x * s, oy, s);
  };
  // DERRIÈRE le perso
  if (hp.before) drawPreviewHat();                                             // .str derrière
  if (hat_ordinal && !isStr) DrawEzCapTris(dl, ox, oy, s, /*before=*/true, /*with_preview=*/true);   // EZ/CEffectMgr derrière (bit 0x8)
  for (int i = 0; i < g_pv_count; ++i) {
    const CapLayer& L = g_pv_caps[i];
    const ImVec2 q0(ox + (L.cx - L.w * 0.5f) * s, oy + (L.cy - L.h * 0.5f) * s);
    const ImVec2 q1(ox + (L.cx + L.w * 0.5f) * s, oy + (L.cy + L.h * 0.5f) * s);
    const ImVec2 u0 = L.mirror ? ImVec2(L.uv1.x, L.uv0.y) : L.uv0;
    const ImVec2 u1 = L.mirror ? ImVec2(L.uv0.x, L.uv1.y) : L.uv1;
    dl->AddImage((ImTextureID)(uintptr_t)L.tex, q0, q1, u0, u1);
  }
  // DEVANT le perso
  if (!hp.before) drawPreviewHat();                                            // .str devant
  if (hat_ordinal && !isStr) DrawEzCapTris(dl, ox, oy, s, /*before=*/false, /*with_preview=*/true);  // EZ/CEffectMgr devant
  ImGui::EndTooltip();
  ImGui::PopStyleColor(2);
}

// Résout item id -> ordinal de hat effect via la table poussée par le serveur au login
// (g_hat_item_ord, ZC 0x0F17). Autoritative (scan des scripts item_db). 0 si l'item n'a
// pas de hat effect (ou table pas encore reçue). Le natif ne mappe PAS item->ordinal :
// effectHatItemTable côté client n'est qu'une appartenance, et GetHatEffectID prend
// l'ordinal (pas l'itemId) -> d'où le push serveur.
int BasicInfo::ItemToHatOrdinal(int item_id) {
  if (item_id <= 0) return 0;
  auto it = g_hat_item_ord.find(static_cast<uint32_t>(item_id));
  return (it != g_hat_item_ord.end()) ? static_cast<int>(it->second) : 0;
}

// Avatar plein-corps (perso complet : corps + coiffes + garment + arme/bouclier,
// apparence live) composité dans la fenêtre ImGui COURANTE. Cadrage STABLE : l'échelle
// + l'ancrage sont FIGÉS, recalculés seulement quand (w,h,anim,dir,apparence) changent
// — jamais par frame d'animation (sinon le sprite « respire »). Le figeage se fait sur
// l'UNION de TOUTES les frames de la pose (comme ExportAvatarGif) pour qu'AUCUNE frame
// ne déborde le cadre (pas de croppage d'extrémités), et l'échelle horizontale est
// bornée sur le demi-extent depuis l'ancrage CORPS (une arme large d'un seul côté n'est
// pas rognée). Pieds ancrés en bas façon WoW, CORPS centré en X -> clip -> AddImage (U
// swap si mirror). No-op hors-jeu ou capture vide. À appeler entre Begin/End.
// anim=animType, dir=0..7 (0=face), animate=joue.
void BasicInfo::RenderPlayerAvatar(float x, float y, float w, float h,
                                         int anim, int dir, bool animate, bool show_costume) {
  if (Bourgeon::Instance().client().session().aid() == 0) return;

  const float pad = 4.0f;
  // Échelle + ancrage FIGÉS : recalculés seulement quand le rect / la pose / la
  // direction / l'apparence changent — jamais par frame d'animation.
  static float s_scale = 0.0f, s_cx = 0.0f, s_feet = 0.0f;
  static float s_head_cx = 0.0f, s_head_cy = 0.0f;  // centre TÊTE (réf. ancre hat effect)
  static float s_body_cy = 0.0f;                    // centre CORPS Y (réf. ancre hat effect)
  static float s_w = -1.0f, s_h = -1.0f;
  static int   s_anim = -1, s_dir = -1;
  static unsigned s_sig = 0;
  static bool s_animate = false;  // dans la clé : nf (union) dépend de animate

  // Capture LIVE (image courante) -> g_av_caps + g_av_sig (signature d'apparence).
  CaptureAvatarActor(anim, dir, animate, -1, show_costume);
  if (g_av_count <= 0) return;
  const unsigned sig = g_av_sig;

  if (s_scale <= 0.0f || s_w != w || s_h != h || s_anim != anim || s_dir != dir ||
      s_sig != sig || s_animate != animate) {
    // (Ré)figeage : bbox UNION sur TOUTES les frames de la pose (comme ExportAvatarGif)
    // -> l'enveloppe contient chaque frame, donc rien n'est jamais rogné, et l'échelle
    // reste constante (zéro « respiration »). bbox TOTALE (arme/coiffes incl.) pour la
    // taille ; bbox CORPS (couches non-tête = torse/jambes) pour centrer X + les pieds.
    float ax0 = 1e9f, ay0 = 1e9f, ax1 = -1e9f, ay1 = -1e9f;
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    float hx0 = 1e9f, hy0 = 1e9f, hx1 = -1e9f, hy1 = -1e9f;  // région TÊTE (ancre effet)
    bool has_body = false, has_head = false;
    const bool anim_pose = (anim == 1 || anim == 4);  // seules Marche/Combat animent
    int nf = (animate && anim_pose && g_av_frame_count > 1) ? g_av_frame_count : 1;
    if (nf > 40) nf = 40;  // garde-fou
    for (int f = 0; f < nf; ++f) {
      CaptureAvatarActor(anim, dir, true, f, show_costume);  // force l'image f
      for (int i = 0; i < g_av_count; ++i) {
        const CapLayer& L = g_av_caps[i];
        // Cart / faucon : DESSINÉS (boucle de rendu plus bas) mais EXCLUS de tout le
        // cadrage — ni l'échelle (bbox totale ax), ni l'ancrage corps (bx)/tête (hx). Sinon
        // un cart large tire le centre X / les pieds et décentre/rapetisse l'avatar : le
        // corps reste le seul « noyau », le compagnon peut déborder (clip du rect). C'est la
        // cause du « position fausse en Repos » (direction OK) : la RE prouve que le cart
        // est bien placé RELATIVEMENT au corps ; c'est l'ancrage global qui dérivait.
        if (L.companion) continue;  // cart/faucon dessinés mais hors cadrage/ancrage
        const float lx0 = L.cx - L.w * 0.5f, lx1 = L.cx + L.w * 0.5f;
        const float ly0 = L.cy - L.h * 0.5f, ly1 = L.cy + L.h * 0.5f;
        if (lx0 < ax0) ax0 = lx0;  if (lx1 > ax1) ax1 = lx1;
        if (ly0 < ay0) ay0 = ly0;  if (ly1 > ay1) ay1 = ly1;
        if (!L.head_region) {
          has_body = true;
          if (lx0 < bx0) bx0 = lx0;  if (lx1 > bx1) bx1 = lx1;
          if (ly0 < by0) by0 = ly0;  if (ly1 > by1) by1 = ly1;
        } else {
          has_head = true;
          if (lx0 < hx0) hx0 = lx0;  if (lx1 > hx1) hx1 = lx1;
          if (ly0 < hy0) hy0 = ly0;  if (ly1 > hy1) hy1 = ly1;
        }
      }
    }
    const float cx   = has_body ? (bx0 + bx1) * 0.5f : (ax0 + ax1) * 0.5f;
    const float feet = has_body ? by1 : ay1;             // pieds = bas du corps
    const float vh   = feet - ay0;                       // sommet (coiffe) -> pieds
    const float half = (ax1 - cx > cx - ax0) ? (ax1 - cx) : (cx - ax0);  // demi-largeur
    if (vh > 1.0f && half > 0.5f) {
      // sx borne l'extent horizontal DEPUIS l'ancrage corps (pas la largeur totale) ->
      // aucun côté ne sort du clip ; sy fait tenir toute la hauteur pieds->sommet.
      const float sx = (w - 2.0f * pad) / (2.0f * half);
      const float sy = (h - 2.0f * pad) / vh;
      s_scale = (sx < sy) ? sx : sy;
      s_cx = cx;
      s_feet = feet;
      // Références pour l'ancre hat effect : centre TÊTE (couches tête) + centre CORPS Y (bbox
      // corps). L'ancre du doll = MÉDIANE de ces deux (réglage retenu). Replis si pas de couche.
      s_head_cx = has_head ? (hx0 + hx1) * 0.5f : cx;
      s_head_cy = has_head ? (hy0 + hy1) * 0.5f : ay0;
      s_body_cy = has_body ? (by0 + by1) * 0.5f : (ay0 + ay1) * 0.5f;
      s_w = w; s_h = h; s_anim = anim; s_dir = dir; s_sig = sig; s_animate = animate;
    }
    // Restaure la capture LIVE pour le rendu (la boucle a laissé la dernière image forcée).
    CaptureAvatarActor(anim, dir, animate, -1, show_costume);
    if (g_av_count <= 0) return;
  }
  if (s_scale <= 0.0f) return;  // pas encore de cadrage valide (capture dégénérée)
  float s  = s_scale;
  float ox = x + w * 0.5f - s_cx * s;      // CORPS centré horizontalement (figé)
  float oy = y + h - pad - s_feet * s;     // pieds du corps collés au bas (figé)
  // FIT effet (demande user : le CADRE ne bouge pas ; l'effet rétrécit pour tenir dedans et le doll
  // scale AVEC). Si un effet EZ est figé et sa bbox déborde du cadre, on réduit s (perso ET effet, car
  // l'effet suit s via R=s/S) puis on re-centre la bbox de l'effet dans le cadre. Largeur effet en px
  // doll = (dx1-dx0)*s (g_ez_fz_* sont en unités doll/s). On ne fait que RÉDUIRE (jamais agrandir).
  // FIT appliqué SEULEMENT si un effet est réellement capturé CE frame (ez_capture::Count()>0). Si le
  // costume-effet est retiré (plus aucune primitive -> Count()==0), on n'applique PAS le FIT : le doll
  // reprend sa taille de base IMMÉDIATEMENT (corrige « reste petit après dé-équipement », sans dépendre
  // du paquet 0x0A3B). Après quelques frames vides on invalide la bbox figée (prochain effet).
  // ⚠ Compte FILTRÉ sur les effets du doll : ez_capture::Count() inclurait l'aperçu survolé ailleurs,
  // et le doll croirait avoir un effet à cadrer alors qu'il n'en affiche aucun.
  if (g_ez_frozen_valid && EzPrimCountForDoll() <= 0) {
    if (++g_ez_empty_frames > 4) g_ez_frozen_valid = false;
  } else if (g_ez_frozen_valid) {
    g_ez_empty_frames = 0;
    const float ew = g_ez_fz_dx1 - g_ez_fz_dx0;
    const float eh = g_ez_fz_dy1 - g_ez_fz_dy0;
    if (ew > 1.0f && eh > 1.0f) {
      const float m = 0.94f;                                   // petite marge intérieure
      const float sfx = (w - 2.0f * pad) * m / ew;
      const float sfy = (h - 2.0f * pad) * m / eh;
      const float sfit = (sfx < sfy) ? sfx : sfy;
      if (sfit < s) {
        // Plancher : ne pas réduire le doll sous g_ez_fit_floor × base. Si l'effet est plus grand
        // que ce que ça permet, il déborde (rogné par le clip du rect) au lieu d'écraser le perso.
        const float sMin = s * g_ez_fit_floor;
        s = (sfit < sMin) ? sMin : sfit;
        // On garde le cadrage du CORPS (centré horizontal, pieds en bas) au nouveau scale — PAS un
        // re-centrage sur la bbox de l'effet (qui décalerait le perso sur le côté). L'effet, dessiné
        // relativement à l'origine acteur (ox,oy), reste ainsi centré sur le perso comme en jeu.
        ox = x + w * 0.5f - s_cx * s;
        oy = y + h - pad - s_feet * s;
      }
    }
  }

  // (d) composite (clip au rect). Hat effects (.str, costumes SANS viewid) capturés depuis un nœud
  // STR autonome puis compositéss comme le natif (blend/rotation/échelle RE). AUCUN hardcode : ancre
  // ET ordre de rendu viennent de HatEffectInfo.lub via Lua (HatOrdinalParams) :
  //  - Ancre X/Y = ORIGINE de l'acteur (le corps est capturé à (0,0) -> l'origine se projette en
  //    (ox, oy)) + hatEffectPosX / hatEffectPos (unités canvas .str -> ×s, l'échelle du contenu).
  //    hatEffectPos<0 = vers le haut. (Remplace l'ancien 11 px + tilt 0.6428 hardcodés = data-driven.)
  //  - isRenderBeforeCharacter : l'effet se dessine DERRIÈRE le perso (avant le sprite) ou DEVANT.
  //  - Contenu .str : 1 px canvas -> s à l'écran. Un effet par ordinal actif (suivi 0x0A3B).
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);

  // (0) hat effects EZ/SPRITE + CEffectMgr (hatEffectID, ex. Digital_Space, Perm_Frost) : capturés
  // live depuis la passe monde (ez_capture), re-ancrés sur le doll (origine acteur (ox,oy), échelle
  // s/S_live). Z-ORDER PAR-QUAD via le bit 0x8 du flag natif (param_2 capturé) = ordre EXACT du jeu
  // (RE render-order workflow) : phase DERRIÈRE (bit set) avant le sprite, phase DEVANT après. Gère
  // plusieurs effets d'un coup, sans Lua. Capture vidée sur frontière de frame (module ez_capture).
  // Doll de la fiche : effets ÉQUIPÉS seulement (pas l'effet survolé ailleurs).
  DrawEzCapTris(dl, ox, oy, s, /*before=*/true, /*with_preview=*/false);   // quads DERRIÈRE le perso (bit 0x8)

  hat_diag_active_ = static_cast<int>(own_hat_effects_.size());
  auto drawHatEffects = [&](bool beforePhase) {
    for (uint16_t ordinal : own_hat_effects_) {
      const HatEffectParams& hp = HatOrdinalParams(static_cast<int>(ordinal));
      if (hp.before != beforePhase) continue;
      CaptureHatEffectOrdinal(static_cast<int>(ordinal));  // GetHatEfResName -> .str -> g_str_caps
      hat_diag_concrete_ = static_cast<int>(ordinal);
      hat_diag_layers_   = g_str_count;      // 0 = résolution/charge/capture a échoué (debug)
      // Ancre = ORIGINE de l'acteur (le corps est capturé à (0,0) -> l'origine se projette en (ox,oy),
      // = la cellule sol/pieds). Le .str place SON contenu par rapport à ça : cercle magique au SOL
      // (contenu au centre canvas = origine), pluie de pièces AU-DESSUS, scène CENTRÉE, etc. -> une
      // seule ancre pour tous, pose-suivie (oy = pieds recadrés par pose). + hatEffectPosX horizontal.
      // (Le lift écran -80 natif des effets "tête/scène" n'est PAS reproductible à plat sans l'échelle
      //  de rendu en jeu S ; l'ancre origine reste juste au sol et sous la tête.)
      const float hox = ox + hp.pos_x * s;
      DrawStrCapLayers(dl, hox, oy, s);      // contenu .str -> écran via DepthScale = s
    }
  };

  drawHatEffects(true);   // (a) effets DERRIÈRE le perso (isRenderBeforeCharacter)
  for (int i = 0; i < g_av_count; ++i) {   // (b) sprite (ordre tableau = painter z-order)
    const CapLayer& L = g_av_caps[i];
    const ImVec2 q0(ox + (L.cx - L.w * 0.5f) * s, oy + (L.cy - L.h * 0.5f) * s);
    const ImVec2 q1(ox + (L.cx + L.w * 0.5f) * s, oy + (L.cy + L.h * 0.5f) * s);
    const ImVec2 u0 = L.mirror ? ImVec2(L.uv1.x, L.uv0.y) : L.uv0;
    const ImVec2 u1 = L.mirror ? ImVec2(L.uv0.x, L.uv1.y) : L.uv1;
    dl->AddImage((ImTextureID)(uintptr_t)L.tex, q0, q1, u0, u1);
  }
  drawHatEffects(false);  // (c) effets .str DEVANT le perso
  DrawEzCapTris(dl, ox, oy, s, /*before=*/false, /*with_preview=*/false);  // (c') quads EZ/CEffectMgr DEVANT le perso
  // (reset de la capture = frontière de frame, dans le module ez_capture)
  dl->PopClipRect();
}

// Paperdoll d'un personnage ARBITRAIRE (char-select), pose statique de face.
// Chemin AUTONOME (voir CaptureDollActor) : ne lit AUCUN global de session et
// n'a pas besoin d'une UIWindow -> utilisable hors jeu. Cache par apparence +
// budget de captures par frame : renvoie false quand rien n'a pu être dessiné
// (budget épuisé cette frame, ou capture vide) -> l'appelant met son placeholder.
bool BasicInfo::RenderDoll(const DollLook& look, float x, float y, float w,
                                 float h, int dir, int anim, uint32_t tint) {
  if (w <= 4.0f || h <= 4.0f) return false;
  const unsigned key   = DollKey(look, dir, anim);
  const unsigned epoch = Overlay_DeviceEpoch();
  const DWORD    now   = GetTickCount();

  DollCacheEntry* e = DollCacheFind(key, epoch);
  // Capture si (a) rien en cache, ou (b) l'entrée est périmée (re-pin de la cellule
  // d'atlas + rafraîchissement du handle de page) — dans les deux cas seulement si
  // le budget de la frame le permet. Sans budget et sans entrée : on ne dessine rien
  // cette frame, le slot apparaîtra à la suivante (max kDollPerFrame nouveaux dolls
  // par frame, donc pas de à-coup quand la grille s'ouvre sur 45 slots).
  if (e == nullptr || (now - e->stamp) > kDollRefreshMs) {
    if (DollTakeBudget()) {
      CaptureDollActor(look, dir, anim);
      DollCacheEntry* dst = e ? e : DollCacheEvict();
      DollCacheFill(dst, key, epoch, now);
      e = dst;
    } else if (e == nullptr || (now - e->stamp) > kDollExpireMs) {
      // Rien en cache, ou entrée trop vieille pour être dessinée sans avoir été
      // revalidée (cf. kDollExpireMs) : placeholder, on retentera à la frame suivante.
      return false;
    }
  }
  e->used = now;
  if (e->count <= 0) return false;  // capture vide (sprite pas encore chargé)

  const float pad  = 2.0f;
  const float vh   = e->feet - e->top;  // sommet (coiffe) -> pieds
  const float half = e->half;
  if (vh <= 1.0f || half <= 0.5f) return false;
  // Échelle pilotée par la HAUTEUR du perso (pieds->sommet), corps centré en X,
  // pieds collés au bas du rect. ⚠ On NE borne PAS par la largeur totale `half` :
  // un compagnon/garment large (chat en costume, cape déployée) gonfle la bbox et,
  // avec `min(sx, sy)`, RAPETISSAIT tout le personnage pour le faire tenir. On laisse
  // désormais la largeur DÉBORDER (clip du rect) plutôt que de rétrécir le perso ;
  // on ne bride que les cas pathologiques (débord > ~2,2x la largeur d'ajustement).
  const float sx = (w - 2.0f * pad) / (2.0f * half);
  const float sy = (h - 2.0f * pad) / vh;
  const float wcap = sx * 2.2f;                 // débord latéral toléré avant bridage
  const float s  = (sy < wcap) ? sy : wcap;
  const float ox = x + w * 0.5f - e->cx * s;
  const float oy = y + h - pad - e->feet * s;

  // PAS de clip au rect : un compagnon/garment large (chat en costume) déborde de
  // la case, et on le veut ENTIER (sans clip il peut mordre sur le siège voisin,
  // acceptable pour la scène). Le dessin reste borné par la fenêtre plein écran.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int i = 0; i < e->count; ++i) {  // ordre du tableau = z-order painter
    const CapLayer& L = e->layers[i];
    if (!L.tex) continue;
    const ImVec2 q0(ox + (L.cx - L.w * 0.5f) * s, oy + (L.cy - L.h * 0.5f) * s);
    const ImVec2 q1(ox + (L.cx + L.w * 0.5f) * s, oy + (L.cy + L.h * 0.5f) * s);
    const ImVec2 u0 = L.mirror ? ImVec2(L.uv1.x, L.uv0.y) : L.uv0;
    const ImVec2 u1 = L.mirror ? ImVec2(L.uv0.x, L.uv1.y) : L.uv1;
    dl->AddImage((ImTextureID)(uintptr_t)L.tex, q0, q1, u0, u1, tint);
  }
  return true;
}

// Exporte le pantin (pose `anim` + direction `dir` courantes) en GIF animé à fond
// TRANSPARENT vers `filepath`. Capture CHAQUE image de la pose (force_frame), calcule
// une bbox union (perso stable), composite chaque image dans un render target
// hors-écran (D3D9_CompositeQuadsRGBA) puis encode (GifWrite) au délai natif. Toutes
// les poses sont animées dans le GIF (même Repos/Assis, figés seulement dans la vue).
bool BasicInfo::ExportAvatarGif(int anim, int dir, const char* filepath,
                                     bool show_costume) {
  if (!filepath || Bourgeon::Instance().client().session().aid() == 0) return false;
  const int CW = 256, CH = 340;  // canvas GIF (agrandi pour la qualité ; LZW compressé)
  const float PAD = 14.0f;

  // Passe 1 : capturer toutes les images (renseigne g_av_frame_count) + bbox union.
  CaptureAvatarActor(anim, dir, true, 0, show_costume);
  int nframes = g_av_frame_count > 0 ? g_av_frame_count : 1;
  if (nframes > 40) nframes = 40;  // garde-fou

  std::vector<std::vector<CapLayer>> frame_layers(static_cast<size_t>(nframes));
  float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
  for (int f = 0; f < nframes; ++f) {
    CaptureAvatarActor(anim, dir, true, f, show_costume);
    frame_layers[static_cast<size_t>(f)].assign(g_av_caps, g_av_caps + g_av_count);
    for (const CapLayer& L : frame_layers[static_cast<size_t>(f)]) {
      minx = std::min(minx, L.cx - L.w * 0.5f);
      maxx = std::max(maxx, L.cx + L.w * 0.5f);
      miny = std::min(miny, L.cy - L.h * 0.5f);
      maxy = std::max(maxy, L.cy + L.h * 0.5f);
    }
  }
  const float bw = maxx - minx, bh = maxy - miny;
  if (bw <= 1.0f || bh <= 1.0f) return false;
  const float s  = std::min((CW - 2 * PAD) / bw, (CH - 2 * PAD) / bh);
  const float ox = CW * 0.5f - (minx + maxx) * 0.5f * s;  // corps centré en X
  const float oy = CH - PAD - maxy * s;                   // pieds ancrés en bas

  // Passe 2 : compositer chaque image sur fond transparent -> RGBA.
  std::vector<uint32_t> canvas(static_cast<size_t>(CW) * CH);
  std::vector<std::vector<uint32_t>> gif_frames(static_cast<size_t>(nframes));
  std::vector<const uint32_t*> ptrs(static_cast<size_t>(nframes));
  std::vector<D3D9TexQuad> quads;
  for (int f = 0; f < nframes; ++f) {
    quads.clear();
    for (const CapLayer& L : frame_layers[static_cast<size_t>(f)]) {
      if (!L.tex) continue;
      D3D9TexQuad q;
      q.tex = L.tex;
      q.x0 = ox + (L.cx - L.w * 0.5f) * s;
      q.x1 = ox + (L.cx + L.w * 0.5f) * s;
      q.y0 = oy + (L.cy - L.h * 0.5f) * s;
      q.y1 = oy + (L.cy + L.h * 0.5f) * s;
      q.u0 = L.mirror ? L.uv1.x : L.uv0.x;  // U swap si miroir (comme le draw)
      q.u1 = L.mirror ? L.uv0.x : L.uv1.x;
      q.v0 = L.uv0.y;
      q.v1 = L.uv1.y;
      quads.push_back(q);
    }
    if (!D3D9_CompositeQuadsRGBA(quads.data(), static_cast<int>(quads.size()), CW, CH,
                                 canvas.data()))
      return false;
    gif_frames[static_cast<size_t>(f)] = canvas;  // copie de l'image
    ptrs[static_cast<size_t>(f)] = gif_frames[static_cast<size_t>(f)].data();
  }

  int delay_cs = static_cast<int>(g_av_frame_delay * 2.5f + 0.5f);  // *25ms /10 = cs
  if (delay_cs < 2) delay_cs = 2;
  const bool ok = GifWrite(filepath, ptrs.data(), CW, CH, nframes, delay_cs);
  if (!ok) LogError("Avatar GIF échec : {}", filepath);
  return ok;
}

float BasicInfo::SnapValue(float v, float ext, int self_id,
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

bool BasicInfo::DrawBar(BarId id, long long cur, long long max) {
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
                           ImGuiWindowFlags_NoFocusOnAppearing |
                           ro::kBackgroundWindowFlags;  // décor : jamais devant une vraie fenêtre
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
  ro::MarkBackgroundWindow(kSrc[id].win_id);
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
          // La barre reste entièrement dans l'écran de jeu. APRÈS l'aimantation :
          // le snap (barres voisines ou grille) peut lui-même repousser dehors, donc
          // c'est le clamp qui doit avoir le dernier mot. Le clamp global de
          // ui/window_clamp.h ne peut rien ici : la fenêtre est réépinglée à
          // (bar.x,bar.y) en ImGuiCond_Always à chaque frame.
          const ImVec2 in_screen = ro::ClampWindowPosToScreen(
              ImVec2(nx, ny),
              ImVec2(static_cast<float>(bar.w), static_cast<float>(bar.h)));
          nx = in_screen.x;
          ny = in_screen.y;
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
      if (kSrc[id].grouped) {  // zeny: label + thousands-grouped amount
        char num[24];
        GroupInt(cur, num, sizeof(num));
        std::snprintf(buf, sizeof(buf), "%s %s", label, num);
      } else if (text_mode_ == 1)
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
    case BasicInfo::kPortName: {
      const std::string nm = Bourgeon::Instance().client().session().GetCharName();
      std::snprintf(out, n, "%s", nm.empty() ? "?" : nm.c_str());
      break;
    }
    case BasicInfo::kPortClass: {
      const char* cls = ClassName();
      std::snprintf(out, n, "%s", (cls && cls[0]) ? cls : "");
      break;
    }
    case BasicInfo::kPortLevel:
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
int BasicInfo::PortraitTouchGroup(int seed) const {
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
bool BasicInfo::DrawPortraitElem(PortId id) {
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
                           ImGuiWindowFlags_NoFocusOnAppearing |
                           ro::kBackgroundWindowFlags;  // décor : jamais devant une vraie fenêtre
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
  ro::MarkBackgroundWindow(id_buf);
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
          // Le cadre saisi reste entièrement dans l'écran de jeu (clamp APRÈS le
          // snap, cf. la barre EXP plus haut). En bloc-move CTRL, le delta appliqué
          // aux autres cadres est celui du cadre SAISI, donc borné par ce clamp.
          const ImVec2 in_screen = ro::ClampWindowPosToScreen(
              ImVec2(nx, ny),
              ImVec2(static_cast<float>(e.w), static_cast<float>(e.h)));
          nx = in_screen.x;
          ny = in_screen.y;
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
          // Hat effects : MÊME traitement que le doll. `ox`/`oy` sont ici aussi l'ORIGINE de
          // l'acteur en coordonnées écran et `s` son échelle, donc l'appel est identique — d'où le
          // z-order en deux passes autour des couches du sprite (bit 0x8 des flags natifs).
          // `with_preview` = false : le portrait montre ce que le personnage PORTE, jamais l'effet
          // survolé ailleurs (même règle que le doll).
          DrawEzCapTris(dl, ox, oy, s, /*before=*/true, /*with_preview=*/false);
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
          DrawEzCapTris(dl, ox, oy, s, /*before=*/false, /*with_preview=*/false);
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
void BasicInfo::DrawPortrait() {
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

// ── Section « BasicInfo » du panneau Moonlight ──────────────────────────
// Déplacée depuis moonlight_ui/panel_interface.cc : ces widgets ne pilotent
// que l'état de CE plugin. MoonlightUi ne garde que l'appel et la décision
// de sauvegarder. Rend true si un réglage a changé.
bool BasicInfo::DrawSettings() {
  bool changed = false;
  PushStyleCompact();

  changed |= ro::RoCheckbox("Masquer la fenêtre Basic Info d'origine", &portrait_hide_basic_info_);
  SameLine(); HelpMarker("Masque la fenêtre native \"Basic Info\".");

  SeparatorText("Barres d'info");
  changed |= ro::RoCheckbox("Afficher les barres", &visible_);
  ImGui::BeginDisabled(!visible_);
  Indent();
    for (int i = 0; i < BasicInfo::kBarCount; ++i) {
      if (i) SameLine();
      changed |= ro::RoCheckbox(BasicInfo::kBarLabels[i], &bars_[i].show);
    }
    SameLine(); HelpMarker("Affiche/cache chaque barre indépendamment.");
  Unindent();
  ImGui::EndDisabled();

  changed |= ro::RoCheckbox("Verrouiller les barres", &locked_);
  SameLine(); HelpMarker(
      "Verrouillée : les barres ne bougent plus et laissent passer les clics au jeu.\n"
      "Déverrouillée : glissez-les pour les déplacer, tirez le coin pour redimensionner.");

  changed |= ro::RoCheckbox("Aimanter les barres (snap)", &sticky_);
  SameLine(); HelpMarker(
      "Quand tu glisses une barre près d'une autre, ses bords s'alignent "
      "et se collent automatiquement (~10px).\nÉloigne-la pour la "
      "détacher. Les barres restent indépendantes.");

  changed |= ro::RoCheckbox("Vertical", &vertical_);
  SameLine(); HelpMarker(
      "Remplissage vertical des barres. \n"
      "Décoche pour les barres horizontales.");

  changed |= ro::RoCheckbox("Bordure des barres", &border_);
  SameLine(); HelpMarker(
      "Trait sombre 1px autour de chaque barre (HP/SP/EXP...). \n"
      "Décoche pour des barres sans contour.");

  const char* modes[] = {"Aucun", "Pourcentage", "Valeurs", "Les deux"};
  changed |= ro::RoCombo("Texte des barres", &text_mode_, modes, IM_ARRAYSIZE(modes));
  SameLine(); HelpMarker(
      "Ce qui est écrit sur les barres : rien, le pourcentage, les "
      "valeurs brutes (courant / max) ou les deux.");
  changed |= WheelSliderFloat("Arrondi", &rounding_, 0.0f, 16.0f);
  SameLine(); HelpMarker("Arrondi des coins des barres.");

  for (int i = 0; i < BasicInfo::kBarCount; ++i) {
    char lbl[32];
    std::snprintf(lbl, sizeof(lbl), "Couleur %s", BasicInfo::kBarLabels[i]);
    changed |= ColorEdit4WithAlphaBar(lbl, bars_[i].fill);
  }
  changed |= ColorEdit4WithAlphaBar("Fond / Opacité", bg_color_);

  TextUnformatted("Tailles rapides de barres (toutes) :");
  // Ce n'est PAS un préréglage (le mot désigne déjà trois autres familles dans
  // ce projet) : c'est le bouton qui applique une taille à TOUTES les barres.
  auto bar_size_button = [&](const char* label, int width_px, int height_px) {
    SameLine();
    if (ro::RoButton(label)) {
      for (int j = 0; j < BasicInfo::kBarCount; ++j) {
        bars_[j].w = width_px;
        bars_[j].h = height_px;
      }
      force_apply_ = true;  // re-apply size even while unlocked
      changed = true;
    }
  };
  bar_size_button("XS", 200, 9);
  bar_size_button("S", 400, 16);
  bar_size_button("M", 600, 22);
  bar_size_button("L", 800, 30);

  SeparatorText("Portrait personnage");
  changed |= ro::RoCheckbox("Afficher le portrait et les étiquettes", &portrait_visible_);
  SameLine(); HelpMarker(
      "Portrait de statut : la tête du personnage, le pseudo, la classe "
      "et le niveau sont des éléments INDÉPENDANTS — chacun déplaçable, "
      "redimensionnable, avec sa couleur/opacité de fond et son arrondi.");

  ImGui::BeginDisabled(!portrait_visible_);

  changed |= ro::RoCheckbox("Verrouiller le portrait", &portrait_locked_);
  Tooltip("Si les éléments sont déverrouillés et en contact les uns avec les autres, ils sont déplaçables en maintenant Ctrl.");
  SameLine(); HelpMarker(
      "Verrouillé : les éléments ne bougent plus et laissent passer les clics au jeu.\n"
      "Déverrouillé : glisse pour déplacer, tire un bord/coin pour redimensionner (aimantage à la grille d'alignement).");

  changed |= ro::RoCheckbox("Tête seule (sans le corps)", &portrait_head_only_);
  SameLine(); HelpMarker(
      "Ne génère que la tête (visage/cheveux/coiffes) et retire le corps.\n"
      "Décoche pour le personnage entier.");

  changed |= ro::RoCheckbox("Cape / garment", &portrait_show_garment_);
  SameLine(); HelpMarker(
      "Affiche la cape/garment équipée (seulement en mode corps "
      "entier — décoche \"Tête seule\" pour la voir).");

  changed |= WheelSliderFloat("Zoom", &portrait_head_zoom_, 0.10f, 2.0f);
  SameLine(); HelpMarker("Ajuster avec le zoom.");

  changed |= WheelSliderFloat("Décalage horiz.", &portrait_head_offx_, -1.5f, 1.5f);
  SameLine(); HelpMarker(
      "Décale le portrait horizontalement (0 = centré).\n"
      "Sert à cadrer la tête/le corps ; le zoom reste centré.");

  changed |= WheelSliderFloat("Décalage vert.", &portrait_head_offy_, -1.5f, 1.5f);
  SameLine(); HelpMarker(
      "Décale le portrait verticalement (0 = centré).\n"
      "Optionnel — le zoom reste centré ; laisse à 0 si tu n'en as pas besoin.");

  static const char* kLabelsAnim[] = { "Repos", "Marche", "Assis", "Ramasser", "Combat", "Attaque", "Touché", "Gelé", "Mort" };
  changed |= ro::RoCombo("Animation", &portrait_anim_, kLabelsAnim, IM_ARRAYSIZE(kLabelsAnim));
  SameLine(); HelpMarker(
      "Pose animée du portrait (Combat = posture prête au combat).\n"
      "Le nombre d'images de l'animation s'ajuste automatiquement.");

  static const char* kLabelsDir[] = { "Face", "Profil-Gauche", "Gauche", "Arrière-Gauche", "Dos", "Arrière-Droite", "Droite", "Profil-Droite" };
  changed |= ro::RoCombo("Direction", &portrait_dir_, kLabelsDir, IM_ARRAYSIZE(kLabelsDir));
  SameLine(); HelpMarker(
      "Oriente le portrait. 0 = face. Essaie les valeurs pour trouver "
      "l'angle voulu (le rendu se met à jour en direct).");

  changed |= ro::RoCheckbox("Animer", &portrait_animate_);
  SameLine(); HelpMarker(
      "Joue les images de l'animation (ex. le balayage de la posture "
      "Combat). Décoche pour figer une pose calme (image 0).");

  SeparatorText("Couleurs et arrondis du portrait et des étiquettes");
  changed |= ro::RoCheckbox("Bordure", &portrait_border_);
  SameLine(); HelpMarker("Trait 1px autour du cadre et des étiquettes.");

  // Per-element config: show / background colour+opacity / rounding /
  // text colour.  Each element is independent.
  for (int i = 0; i < BasicInfo::kPortCount; ++i) {
    auto& e = ports_[i];
    ImGui::PushID(i);
    changed |= ro::RoCheckbox(BasicInfo::kPortLabels[i], &e.show);
    Indent();
    changed |= ColorEdit4WithAlphaBar("Fond / Opacité", e.bg);
    if (i != BasicInfo::kPortHead) {
      SameLine();
      changed |= ColorEdit4WithAlphaBar("Texte", e.fg);
    }
    changed |= WheelSliderFloat("Arrondi", &e.rounding, 0.0f, 16.0f, "%.0f", 1.0f);
    Unindent();
    ImGui::PopID();
  }
  PopStyleCompact();

  ImGui::EndDisabled(); // portrait_visible_
  return changed;
}

void BasicInfo::OnRenderUI() {
  if (!in_game_) return;
  // The alignment grid is drawn by MoonlightUi (shared overlay), not here.

  DrawPortrait();  // independent of the EXP-bar master toggle below

  if (!visible_) return;
  // Globals are only populated once a character is in the world.
  if (Bourgeon::Instance().client().session().aid() == 0) return;

  bool changed = false;
  for (int i = 0; i < kBarCount; ++i) {
    if (!bars_[i].show) continue;
    const long long cur = RDval(kSrc[i].cur, kSrc[i].wide);
    const long long max = kSrc[i].max_const ? kSrc[i].max_const
                                            : RDval(kSrc[i].max, kSrc[i].wide);
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

// Noms des paramètres alignés sur uiwnd::OnMsg : le natif prend SIX entiers dont
// `msg` est le DEUXIÈME ; le premier (`arg0`) vaut 0 sur tous les sites d'appel
// connus et son rôle n'est pas établi. On relaie tout tel quel.
int __fastcall BIMsgHook(void* self, void* edx, int arg0, int msg, int p2, int p3,
                         int p4, int p5) {
  __try {
    const int r = g_bi_orig_msg(self, edx, arg0, msg, p2, p3, p4, p5);
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

BasicInfo::BasicInfo() {
  // Install the msg-0x22 hide hook at DLL load (before any Basic Info is created),
  // so the very first HUD creation at login is caught pre-render (no flicker).
  void* cur = *reinterpret_cast<void**>(kBIMsgSlot);
  if (cur && cur != reinterpret_cast<void*>(&BIMsgHook)) {
    g_bi_orig_msg = reinterpret_cast<BIMsg_t>(cur);
    BIPatchPtr<void*>(kBIMsgSlot, reinterpret_cast<void*>(&BIMsgHook));
  }
  // ZC_EQUIPMENT_EFFECT 0x0A3B (VAR) : [len:2][aid:4][status:1]{effectId:2}. On
  // OBSERVE (paquet natif intact) pour suivre les hat effects actifs du joueur ;
  // `data` pointe dans le buffer recv live -> on lit toute la liste via `len`
  // (champ de longueur du paquet). 7 = len(2)+aid(4)+status(1) garantis forwardés.
  Bourgeon::Instance().RegisterObserveOpcode(0x0A3B, 7);
  // ZC_BOURGEON_HATEFFECT_MAP 0x0F17 (opcode CUSTOM > 0x0C35 -> livré par le reader-hook,
  // data = octets APRÈS [type:2][len:2]) : table itemId->ordinal poussée au login. Cf.
  // bourgeon_opcodes.h / clif_bourgeon_hateffect_map (moonlight).
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kHatEffectMap);
}

// Suit les hat effects (.str) actifs sur le JOUEUR à partir de ZC_EQUIPMENT_EFFECT
// (0x0A3B). Le serveur envoie la liste COMPLÈTE (status=1) au spawn/refresh et des
// bascules unitaires (status=1 équip / status=0 déséquip). Modèle incrémental sur un
// ensemble : status=1 => ajoute chaque id, status=0 => retire. On ne garde que le
// propre joueur (aid == propre aid). `data` = buffer juste après l'opcode :
// data[0..1]=packetLength (inclut l'opcode), data[2..5]=aid, data[6]=status,
// data[7..]=liste d'effectId (2 o. chacun).
void BasicInfo::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  // ZC_BOURGEON_HATEFFECT_MAP (custom recv) : data = APRÈS le header [type:2][len:2]
  // -> [count:2] puis count × {itemId:4, ordinal:2}. Remplace la table en cache.
  if (opcode == bopcodes::kHatEffectMap) {
    if (!data || len < 2) return;
    const uint16_t count = *reinterpret_cast<const uint16_t*>(data);
    const uint8_t* p = data + 2;
    g_hat_item_ord.clear();
    g_hat_item_ord.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
      if (2 + (i + 1) * 6 > len) break;  // garde-fou (paquet tronqué)
      const uint32_t id  = *reinterpret_cast<const uint32_t*>(p + i * 6);
      const uint16_t ord = *reinterpret_cast<const uint16_t*>(p + i * 6 + 4);
      if (id != 0 && ord != 0) g_hat_item_ord[id] = ord;
    }
    return;
  }
  if (opcode != 0x0A3B || len < 7 || data == nullptr) return;
  const uint16_t plen   = *reinterpret_cast<const uint16_t*>(data);
  const uint32_t aid    = *reinterpret_cast<const uint32_t*>(data + 2);
  const uint8_t  status = data[6];
  const uint32_t own    = Bourgeon::Instance().client().session().aid();
  // Corps = plen - en-tête(opcode2 + len2 + aid4 + status1 = 9). Borné défensivement.
  int body = static_cast<int>(plen) - 9;
  if (body < 0) body = 0;
  const int nfx = body / 2;
  if (aid != own) return;  // joueur only
  const uint8_t* p = data + 7;
  for (int i = 0; i < nfx; ++i) {
    const uint16_t fx = *reinterpret_cast<const uint16_t*>(p + i * 2);
    auto it = std::find(own_hat_effects_.begin(), own_hat_effects_.end(), fx);
    if (status) {
      if (it == own_hat_effects_.end()) { own_hat_effects_.push_back(fx); g_ez_frozen_valid = false; }
    } else if (it != own_hat_effects_.end()) {
      own_hat_effects_.erase(it);
      g_ez_frozen_valid = false;  // bbox FIT à recalculer pour ce qui reste
    }
  }
}

// Enforces the "Masquer la fenêtre Basic Info d'origine" option using the native
// VISIBILITY (SetVisible), NOT an off-screen move — so nothing corrupts a saved
// position. The msg-0x22 hook above hides it pre-render at creation; here we
// re-hide if the game re-shows it, and restore visibility when the option is off.
//   BasicInfo singleton ptr = *(g_UIWindowMgr 0x0131f4e8 + 0x1dc) (vtable
//   0x0103e35c); null until the HUD is created.
void BasicInfo::OnTick() {
  // Regenerate the head sprite from the game's actor renderer (update phase is a
  // safer place to call it than the Present hook). DrawPortrait reads g_caps.
  if (portrait_visible_ && portrait_head_sprite_ && ports_[kPortHead].show &&
      Bourgeon::Instance().client().session().aid() != 0) {
    g_portrait_anim = portrait_anim_;
    g_portrait_dir = portrait_dir_;
    g_portrait_animate = portrait_animate_;
    g_portrait_garment = portrait_show_garment_ && !portrait_head_only_;
    CapturePortraitActor();
  } else {
    g_cap_count = 0;
  }

  // ── Capture EZ/CEffectMgr (hat effects `hatEffectID`) : composite doll + aperçu cashshop ──
  // Le module ez_capture remplit sa capture pendant la passe de rendu MONDE ; consommée par
  // RenderPlayerAvatar / RenderItemPreviewTooltip. Reset = frontière de frame (dans le module).
  // Ici on ré-arme le propriétaire pour la passe suivante.
  InstallEzCapture();
  // Aperçu cashshop : spawn/despawn l'effet EZ survolé sur le joueur (keep-alive expiré -> despawn).
  ReconcileEzPreview();
  // Arme le propriétaire. GameMode_GetActive renvoie 0 si le mode est transitoirement « inactif »
  // (*(mgr+0x58)!=1) ; on garde alors le DERNIER acteur connu (l'effet est continu, l'acteur stable).
  // Armé aussi si un aperçu cashshop est actif (effet spawné sur le joueur, à capturer).
  // ⚠ Cet acteur est seulement COMPARÉ, JAMAIS déréférencé (au changement de map il est libéré -> UAF).
  if (own_hat_effects_.empty() && g_ez_preview_active == 0) {
    g_ez_owner_actor = nullptr;
  } else {
    void* live = GetOwnActorLive();
    if (live) g_ez_last_actor = live;
    g_ez_owner_actor = live ? live : g_ez_last_actor;
  }
  // (Le module résout lui-même l'acteur joueur : rien à lui transmettre ici.)
  // Ids concrets CIBLES = GetHatEffectID(ordinal) des hat effects équipés -> filtre le DESSIN
  // (DrawOpts::ids) pour isoler le(s) hat effect(s) du footprint (230) / aura (164). Les CEffectMgr
  // (Perm_Frost...) n'ont pas d'id résolu sur leur chemin : ils passent par `include_effmgr`.
  g_ez_target_count = 0;
  for (uint16_t ord : own_hat_effects_) {
    if (g_ez_target_count >= 8) break;
    const int cid = static_cast<int>(HatLuaNum(static_cast<int>(ord), "GetHatEffectID", -1.0f));
    if (cid > 0) g_ez_target_ids[g_ez_target_count++] = cid;
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
