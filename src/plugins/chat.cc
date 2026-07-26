#include "plugins/chat.h"

#include <Windows.h>
#include <intrin.h>  // _ReturnAddress
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "ui/ro_imgui.h"    // widgets « façon RO » du panneau de réglages
#include "ui/ro_widgets.h"  // HelpMarker, SameLine, WheelSliderInt
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Item link icon injection ──────────────────────────────────────────────
// Item icons are drawn next to each <ITEML> equipment link in chat via THREE
// hooks: inject a short ^i{<base62 id>} token before the tag (AppendLineHook),
// render it at the GDI leaf (TextOutLowHook), and report the token as the icon's
// width to the layout so links sit flush against the icon (MeasureHook).
//
// Item link format (PACKETVER >= 20200724, moonlight/rAthena):
//   <ITEML>[5ch_equip_b62][1ch_slot][nameid_b62][optional fields]</ITEML>
//   nameid starts at byte offset 13 (7+5+1) and runs until non-base62 char.
//
// Base62 alphabet (rAthena utilities.cpp): 0-9=0-9, a-z=10-35, A-Z=36-61

static int B62Digit(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 36;
  return -1;
}

static uint32_t B62Decode(const char* s, size_t len) {
  uint32_t v = 0;
  for (size_t i = 0; i < len; i++)
    v = v * 62u + static_cast<uint32_t>(B62Digit(static_cast<unsigned char>(s[i])));
  return v;
}

// Returns a copy of |text| with ^i[itemid] prepended before each <ITEML> tag.
static std::string InjectItemIcons(const char* text) {
  std::string result;
  const char* p = text;
  while (const char* tag = strstr(p, "<ITEML>")) {
    result.append(p, tag - p);
    // Parse nameid: <ITEML>(7) + equip(5) + slot_flag(1) = offset 13
    const char* data = tag + 13;
    size_t id_len = 0;
    while (B62Digit(static_cast<unsigned char>(data[id_len])) >= 0) id_len++;
    if (id_len > 0 && B62Decode(data, id_len) > 0) {
      // ^i{<base62 id>} — reuse the tag's own base62 nameid verbatim; the leaf
      // hook decodes it back, so no decode→re-encode round-trip is needed.  Uses
      // {} (not the engine's native ^i[ token) so it can't collide with it.
      result += "^i{";
      result.append(data, id_len);
      result += '}';
    }
    result += '<';  // re-emit the '<'; loop advances past it
    p = tag + 1;
  }
  result += p;
  return result;
}

// ── PROTOTYPE: native item-icon draw on chat links ─────────────────────────
// Reuses the engine's own `^i` ItemIconWnd machinery (vtable 0x010276dc) to
// draw a 25x25 item icon next to each chat <ITEML> link, the same way the NPC
// dialog / ChatWindow (TextLayout) renders item icons.  All calls below are to
// the game's own functions, so a working sequence is directly portable to a
// WARP exe code-cave (no DLL/ImGui dependency).
//
// Construction sequence reverse-engineered from TextLayout_EmitItemIconToken
// (FUN_00800f60) construction site at 0x00801043-0x0080112c:
//   obj = operator_new(0x94, &nothrow)            ; 0x00dbbeae, &DAT_010a4bc0
//   UIWindow_base_ctor(obj, 0)                    ; 0x00a1b190 (__thiscall)
//   *obj = 0x010276dc                             ; ItemIconWnd vtable
//   <init SSO std::string at obj+0x7c>            ; size=0 cap=0xF buf[0]=0; obj+0x79=1
//   ItemIconWnd_SetSize(obj, 0x19, 0x19)          ; 0x00a1cb70 (__thiscall)
//   std::string::assign(obj+0x7c, "501", 3)       ; 0x004f1940 (__thiscall)
//   (*[vtable+0x98])(obj)                          ; paint dispatch
// To actually draw at a screen position we additionally drive the standard
// UIWindow draw chain ourselves (the parent TextLayout normally does this):
//   UIWindow_OnDraw_Base(obj, w, h)  -> builds render node at obj+0x24 (0x00a245c0)
//   ItemIconWnd_LoadBitmap(obj)      -> loads tex into node     (0x00803850)
//   UIWindow_SetPos(obj, x, y)       -> obj+0x1c/0x20 = x/y     (0x00a23450)
//   UIWindow_PaintDispatch(obj)      -> blit                    (0x00a23340)

namespace {
// Engine glue (20250716).  All calls match dis-assembled conventions from
// ItemBtn_LoadIconByResName (0x00857350): a render-node-backed image blit that
// the engine composites as part of the owning window's draw pass.
constexpr uintptr_t kEngBuildPath = 0x00d5a720;  // __thiscall(session, idstr, outbuf, byte)
constexpr uintptr_t kEngSession   = 0x015fa3c0;  // session object (ecx for BuildPath)
constexpr uintptr_t kEngTexMgr    = 0x00a90350;  // __cdecl() -> tex mgr
constexpr uintptr_t kEngMakeKey   = 0x00a9f030;  // __cdecl(path) -> key
constexpr uintptr_t kEngLoadTex   = 0x00a8d4a0;  // __thiscall(mgr, key) -> tex

using BuildPath_t = void* (__fastcall*)(void*, void*, const char*, char*, int);
using TexMgr_t    = void* (__cdecl*)();
using MakeKey_t   = void* (__cdecl*)(const char*);
using LoadTex_t   = void* (__fastcall*)(void*, void*, void*);

// FUN_0083d840: the chat tab's "append drawn line" virtual (vtable +0xe4).
// __thiscall(this, char* text, color, sender-ish).  Called for EVERY drawn line
// — by WrapAndDispatch (per wrapped chunk) and by the direct link path — with
// |text| still containing the raw <ITEML> tag.  Injecting ^i[id] before each
// <ITEML> here puts the token into the drawn text, which UIText_DrawColored
// then renders as an icon via ChatTextHook.  This is THE chokepoint AddLine and
// the dispatcher hooks missed.
using AppendLineFn = void (__fastcall*)(void*, void*, char*, uint32_t, void*);
AppendLineFn g_append_line_orig = nullptr;

// Width-enforcement state (defined near the top so the icon/append hooks can
// sample it too).  Driven by chat::SetCustomWidth / the WndProc hook.
int   g_chat_width      = 0;        // >0 = enforce this width; 0 = leave engine default
int   g_chat_orig_width = 0;        // captured engine default width (for restore)
bool  g_in_apply        = false;    // re-entrancy guard for the relayout
void* g_chat_wnd        = nullptr;  // live UINewChatWnd (cached from WndProc ecx)
bool  g_chat_timestamps = false;    // true = prefix new chat lines with [HH:MM:SS]
bool  g_chat_item_icons = true;     // true = inject ^i item icons on <ITEML> chat links

// Writes the current local time as "[HH:MM:SS] " into |out| (needs >= 12 bytes).
// Shared by the timestamp injection sites: the raw-history writer (AddLine, so it
// survives resize/RebuildFromHistory), the live-display splitter (CharWrap, for
// plain lines), and the drawn-line virtual (AppendLine, for live item-link lines).
static void FormatTimestamp(char* out, size_t cap) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::snprintf(out, cap, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
}

// True if |s| already begins with a "[HH:MM:SS] " stamp (as FormatTimestamp
// writes it).  Used to keep AppendLine stamping idempotent: a live item-link line
// arrives unstamped, but on a rebuild the raw-history copy is already stamped.
static bool HasTimestamp(const char* s) {
  if (!s) return false;
  auto dig = [](char c) { return c >= '0' && c <= '9'; };
  return s[0] == '[' && dig(s[1]) && dig(s[2]) && s[3] == ':' && dig(s[4]) &&
         dig(s[5]) && s[6] == ':' && dig(s[7]) && dig(s[8]) && s[9] == ']' &&
         s[10] == ' ';
}
// ── Chat new-line word-wrap width fix ──────────────────────────────────────
// The new-line dispatch (WndProc msg 0x25) pre-splits each incoming line at a
// fixed 94 CHARACTERS (~709px) via FUN_00979f10 BEFORE the pixel word-wrap, so a
// widened chat never wraps past ~709px — only newlines and that 94-char cap break
// a line.  We hook that splitter and, only when it is the chat call (return addr
// 0x008fd12a) with a custom width active, disable the char cap so the engine's
// pixel wrap (at tab+0x14) governs — an exact wrap at any width, with NO per-line
// rebuild.  FUN_00979f10 has 17 callers; the return-addr check confines the change
// to the chat dispatch.
constexpr uintptr_t kCharWrap       = 0x00979f10;   // __cdecl(text, outlist, maxchars)
constexpr uintptr_t kChatWrapCaller = 0x008fd12a;   // return addr of the chat call site
using CharWrapFn = void (__cdecl*)(char*, int*, int);
CharWrapFn g_charwrap_orig = nullptr;

void __cdecl CharWrapHook(char* text, int* outlist, int maxchars) {
  const bool chat_call =
      _ReturnAddress() == reinterpret_cast<void*>(kChatWrapCaller);
  if (g_chat_width > 0 && chat_call)
    maxchars = 0x10000;  // effectively no char cap → pixel wrap (tab+0x14) wins
  // Live-display timestamp: this splitter feeds the on-screen wrapped lines for a
  // freshly arrived message (it is NOT used by RebuildFromHistory, which re-wraps
  // the already-stamped raw history) — so stamping here makes the [HH:MM:SS] show
  // immediately, while the AddLine hook keeps it in raw history for later rebuilds.
  if (g_chat_timestamps && chat_call && text && text[0]) {
    char stamp[16];
    FormatTimestamp(stamp, sizeof(stamp));
    std::string stamped;
    stamped.reserve(11 + std::strlen(text));
    stamped.append(stamp);
    stamped.append(text);
    g_charwrap_orig(const_cast<char*>(stamped.c_str()), outlist, maxchars);
    return;
  }
  g_charwrap_orig(text, outlist, maxchars);
}

// ── Hide the fixed-width input frame (dialog_bg.bmp) on a widened chat ───────
// UINewChatWnd_Paint blits dialog_bg.bmp (a fixed 600px image) at its NATURAL size
// via UIWindow_BlitImageToNode — so the input frame never widens.  The input-row
// BORDER LINES in the same Paint use this+0x14 (chat width) and already scale, so
// skipping just this one blit (when a custom width is active) leaves a clean
// scaling border instead of a stuck 600px bar.  Return-addr gated to that single
// call site (0x008f3498); the blit has many other callers, all untouched.
constexpr uintptr_t kBlitImageToNode = 0x00a1d260;  // __thiscall(this,x,y,tex,flag)
constexpr uintptr_t kDialogBgBlitRet = 0x008f3498;  // return addr of the dialog_bg blit
using BlitFn = void (__fastcall*)(void*, void*, int, int, int, int);
BlitFn g_blit_orig = nullptr;

void __fastcall BlitHook(void* ecx, void* edx, int x, int y, int tex, int flag) {
  if (g_chat_width > 0 &&
      _ReturnAddress() == reinterpret_cast<void*>(kDialogBgBlitRet))
    return;  // skip the fixed-width input-frame image
  g_blit_orig(ecx, edx, x, y, tex, flag);
}

void __fastcall AppendLineHook(void* ecx, void* edx, char* text, uint32_t p2,
                               void* p3) {
  if (text && std::strstr(text, "<ITEML>")) {
    // Item-link lines skip the CharWrap splitter (the engine routes them straight
    // here), so the live copy arrives unstamped — prepend the timestamp here, but
    // only if it is not already present (on a rebuild the raw-history copy already
    // carries it, so this stays idempotent and never double-stamps).
    std::string modified;
    if (g_chat_timestamps && !HasTimestamp(text)) {
      char stamp[16];
      FormatTimestamp(stamp, sizeof(stamp));
      modified = stamp;
    }
    if (g_chat_item_icons) modified += InjectItemIcons(text);
    else                   modified += text;
    g_append_line_orig(ecx, edx, const_cast<char*>(modified.c_str()), p2, p3);
    return;
  }
  g_append_line_orig(ecx, edx, text, p2, p3);
}

// ── Optional [HH:MM:SS] timestamp on every chat line ────────────────────────
// UISubChatWnd_AddLine(this, text, color, sender) is the per-message RAW-history
// writer (text -> tab+0x100, color -> +0x10c, sender -> +0x118).  Prepending the
// stamp to |text| here stores it IN the raw history, so it survives every
// re-wrap (resize / RebuildFromHistory) and renders flush as the start of the
// line — exactly once per message per tab.  (The drawn-line path AppendLine runs
// per wrapped chunk and on every rebuild, so it is the wrong place for this.)
// The stamp inherits the line's colour; empty lines are left untouched.  Toggle
// via chat::SetTimestamps (driven by the MoonlightUi settings checkbox).
constexpr uintptr_t kSubChatAddLine = 0x0083f070;  // __thiscall(this,text,color,sender)
using AddLineFn = void (__fastcall*)(void*, void*, char*, uint32_t, char*);
AddLineFn g_addline_orig = nullptr;

void __fastcall AddLineHook(void* ecx, void* edx, char* text, uint32_t color,
                            char* sender) {
  if (g_chat_timestamps && text && text[0]) {
    char stamp[16];
    FormatTimestamp(stamp, sizeof(stamp));
    std::string stamped;
    stamped.reserve(11 + std::strlen(text));
    stamped.append(stamp);
    stamped.append(text);
    g_addline_orig(ecx, edx, const_cast<char*>(stamped.c_str()), color, sender);
    return;
  }
  g_addline_orig(ecx, edx, text, color, sender);
}

// ── Timestamp on DETACHED chat-window live lines ────────────────────────────
// Detached chat windows (UIChatWnd) display new lines via UISubChatWnd_WrapAndDispatch
// with the ORIGINAL (unstamped) text, so the live [HH:MM:SS] never showed there.
// (Raw history IS stamped by AddLineHook; the MAIN chat's live display by CharWrapHook;
// item-link lines by AppendLineHook — detached PLAIN lines fell through every one.)
// WrapAndDispatch is shared (main @0x008fd269, RebuildFromHistory @0x008643b9, detached)
// and word-wraps the FULL line then dispatches each chunk, so a blanket stamp would
// double-stamp the main and wrongly stamp wrapped continuation chunks. We gate on the
// DETACHED call site's return address (0x0090243f in UIChatWnd_HandleMsg case 0x25) and
// stamp the FULL line BEFORE it wraps -> the stamp lands on the first chunk only.
// Idempotent via HasTimestamp (a rebuild from already-stamped raw history won't double).
// Signature mirrors the per-line dispatch: __thiscall(this, text, p2, p3) (3 stack args).
constexpr uintptr_t kWrapAndDispatch    = 0x0083d3f0;  // UISubChatWnd_WrapAndDispatch
constexpr uintptr_t kDetachedWrapCaller = 0x0090243f;  // ret addr of the detached call site
using WrapDispatchFn = void (__fastcall*)(void*, void*, char*, uint32_t, void*);
WrapDispatchFn g_wrap_dispatch_orig = nullptr;

void __fastcall WrapAndDispatchHook(void* ecx, void* edx, char* text, uint32_t p2,
                                    void* p3) {
  if (g_chat_timestamps && text && text[0] && !HasTimestamp(text) &&
      _ReturnAddress() == reinterpret_cast<void*>(kDetachedWrapCaller)) {
    char stamp[16];
    FormatTimestamp(stamp, sizeof(stamp));
    std::string stamped;
    stamped.reserve(11 + std::strlen(text));
    stamped.append(stamp);
    stamped.append(text);
    g_wrap_dispatch_orig(ecx, edx, const_cast<char*>(stamped.c_str()), p2, p3);
    return;
  }
  g_wrap_dispatch_orig(ecx, edx, text, p2, p3);
}

// ── Leaf renderer hook: draw the icon where ^i[id] appears in a text segment ──
// FUN_005471a0 is the engine's low-level GDI text-out: __thiscall(ctx, x, y,
// str, len).  ctx+8 = the render node, ctx+0x14 = font.  The chat line text
// (carrying our injected ^i[id], which the chat renderer leaves literal) passes
// through here.  We detect ^i[id], measure the preceding text to get the icon x,
// blit the item icon into the same render node (reusing FUN_00a1d260 via a tiny
// fake-window whose +0x24 points at the node), and blank the token so it isn't
// drawn.  Reuses only engine calls -> portable to a WARP code-cave.
constexpr uintptr_t kEngTextOutLow  = 0x005471a0;  // __thiscall(ctx, x, y, str, len)
constexpr uintptr_t kEngTextMeasure = 0x005474a0;  // __thiscall(ctx, SIZE*, str, len, font)

using MeasureTextFn = void* (__fastcall*)(void*, void*, void*, const char*, int, int);
static MeasureTextFn g_measure_text_orig = nullptr;

// ── Cache de mesure de texte (fix du freeze de chat en combat) ────────────────
// Le word-wrap du moteur cherche le point de coupure d'une ligne en mesurant des
// préfixes de plus en plus longs de la MÊME chaîne : [0..1], [0..2], [0..3]...
// Mesuré sur un rebuild d'historique : 8640 appels GDI pour 120 lignes (72/ligne),
// 94 % du temps total — chacun étant un GetTextExtentPoint32W (syscall). En combat,
// TrimHistoryHalf relance ce rebuild toutes les ~1,4 s ; avec 2 fenêtres de chat ça
// donnait un freeze de ~500 ms. Le coût unitaire du syscall varie d'un facteur ~30
// selon la session (cause user-mode non trouvée, voir mémoire projet) — le cache le
// rend sans effet.
//
// Ces mesures sont pures : même (texte, police) => même largeur. Mémoïsation en
// table à adressage direct, clé FNV-1a 64 bits sur le texte + les paramètres de
// police (ctx+0xc, ctx+0x10, id police, ctx+0x1c qui porte gras/italique), donc pas
// de résultat périmé sur changement de style. Collision 64 bits hors de portée.
// --nomeasurecache restaure le comportement d'origine.
static bool g_measure_cache = true;
struct MCacheEntry { uint64_t key; long cx, cy; };
// 65536 entrees (~1,5 Mo) : un seul rebuild produit jusqu'a 8640 mesures DISTINCTES,
// une table plus petite se piétinerait elle-meme.
static constexpr size_t kMCacheSize = 65536;
static constexpr size_t kMCacheMask = kMCacheSize - 1;
static MCacheEntry g_mcache[kMCacheSize] = {};

static uint64_t Fnv1a(const void* p, size_t n, uint64_t h) {
  const unsigned char* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}

// UIRenderCtx_MeasureText (0x005474a0) est LE point chaud du rebuild d'historique :
// le word-wrap du moteur y mesure des prefixes croissants de la meme ligne (~72
// par ligne, 8640 par rebuild), chacun etant un GetTextExtentPoint32W = un syscall
// GDI. On memoise le resultat : meme (texte, police) => meme largeur, donc pur.
// Detail complet dans la memoire projet "chat-trim-freeze".
void* __fastcall MeasureTextHook(void* ecx, void* edx, void* out_size,
                                 const char* str, int len, int font) {
  if (!ecx) return g_measure_text_orig(ecx, edx, out_size, str, len, font);

  SIZE* out = static_cast<SIZE*>(out_size);
  uint64_t key = 0;
  MCacheEntry* slot = nullptr;
  if (g_measure_cache && out && str && len > 0 && len < 8192) {
    const int* c = static_cast<const int*>(ecx);
    key = Fnv1a(str, static_cast<size_t>(len), 1469598103934665603ULL);
    // ctx+0xc et ctx+0x10 : parametres de police ; ctx+0x1c : mot portant les
    // octets gras (0x1d) et italique (0x1e) — les inclure evite de servir une
    // largeur mise en cache pour un style different.
    const int meta[4] = { c[3], c[4], font, c[7] };
    key = Fnv1a(meta, sizeof(meta), key);
    slot = &g_mcache[key & kMCacheMask];
    if (slot->key == key) {
      out->cx = slot->cx;
      out->cy = slot->cy;
      return out_size;
    }
  }

  void* r = g_measure_text_orig(ecx, edx, out_size, str, len, font);

  if (slot) {  // memorise le resultat frais
    slot->key = key;
    slot->cx  = out->cx;
    slot->cy  = out->cy;
  }
  return r;
}

constexpr uintptr_t kEngNodeBlit    = 0x0053f140;  // __thiscall(node, x, y, w, h, ARGB*, colorkey)
using TextOutLowFn = void  (__fastcall*)(void*, void*, int, int, char*, unsigned);
using MeasureFn    = void* (__fastcall*)(void*, void*, void*, const char*, int, int);
using NodeBlitFn   = void  (__fastcall*)(void*, void*, int, int, int, int, const uint32_t*, char);
TextOutLowFn g_textout_low_orig = nullptr;

// Target on-screen icon size in chat (px).  Item BMPs are ~24px; chat lines are
// ~14px, so we downsample to this.
constexpr int kChatIconSize = 16;
// Horizontal space a token occupies (icon + small pad).  Used both as the
// layout width an ^i token reports (MeasureHook) and the leaf draw advance, so
// the icon sits flush and the following name/link-click-target stay aligned.
constexpr int kIconAdvance = kChatIconSize + 2;

// FUN_00a21c90: the layout text-width used to position chat link buttons + their
// click targets (see FUN_0084a780).  __thiscall(this, text, len, p3, p4, p5, p6)
// -> width.  We make each ^i{..} token report kIconAdvance instead of its
// (wider) literal text width, so links sit flush against the icon.
using LayoutMeasureFn = int (__fastcall*)(void*, void*, char*, unsigned, int, int, char, char);
LayoutMeasureFn g_layout_measure_orig = nullptr;

// Scans str[from..len) for the next item-icon token ^i{<base62 id>} — the form
// we inject before each <ITEML> link.  On a match returns true and sets
// *tok = index of the '^', *end = one past the '}', *id = decoded nameid (0 if
// the body is empty/invalid).  An unclosed ^i{ is skipped (not a token).  Single
// source of truth for token detection, used by both hooks below.  (The engine's
// own ^i[<decimal>] token is rendered natively by TextLayout windows, not here.)
static bool NextIconToken(const char* str, unsigned len, unsigned from,
                          unsigned* tok, unsigned* end, uint32_t* id) {
  for (unsigned i = from; i + 3 <= len; ++i) {
    if (str[i] != '^' || str[i + 1] != 'i' || str[i + 2] != '{') continue;
    unsigned e = i + 3;
    while (e < len && str[e] != '}') ++e;
    if (e >= len) continue;  // no closing brace in this run — not a token
    *id = B62Decode(str + i + 3, e - (i + 3));
    *tok = i;
    *end = e + 1;
    return true;
  }
  return false;
}

int __fastcall MeasureHook(void* ecx, void* edx, char* text, unsigned len,
                           int p3, int p4, char p5, char p6) {
  if (text) {
    const unsigned L = len ? len : static_cast<unsigned>(std::strlen(text));
    unsigned tok, end;
    uint32_t id;
    if (NextIconToken(text, L, 0, &tok, &end, &id)) {
      // Each token's literal text would measure wider than the icon; replace its
      // width with kIconAdvance so link buttons sit flush against the icon.
      int w = g_layout_measure_orig(ecx, edx, text, len, p3, p4, p5, p6);
      do {
        const int tw = g_layout_measure_orig(ecx, edx, text + tok,
                                             end - tok, p3, p4, p5, p6);
        w += kIconAdvance - tw;
      } while (NextIconToken(text, L, end, &tok, &end, &id));
      return w;
    }
  }
  return g_layout_measure_orig(ecx, edx, text, len, p3, p4, p5, p6);
}

// SEH-guarded width of |s[0..n)| in the ctx font.  0 on fault.
static int MeasureSEH(void* ctx, const char* s, int n) {
  int w = 0;
  __try {
    char buf[512];
    int k = (n < 0) ? 0 : (n < 511 ? n : 511);
    std::memcpy(buf, s, k);
    buf[k] = '\0';
    const int font = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ctx) + 0x14);
    SIZE sz = {0, 0};
    reinterpret_cast<MeasureFn>(kEngTextMeasure)(ctx, nullptr, &sz, buf, k, font);
    w = sz.cx;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    w = 0;
  }
  return w;
}

// SEH-guarded: load item |id|'s icon, downsample to kChatIconSize, blit into the
// ctx's render node at (x, y).  No C++ objects in scope (legal __try).
static void BlitIconAtSEH(void* ctx, int x, int y, uint32_t id) {
  static uint32_t dbuf[kChatIconSize * kChatIconSize];
  __try {
    char idbuf[16];
    std::snprintf(idbuf, sizeof(idbuf), "%u", id);
    char path[260] = {0};
    reinterpret_cast<BuildPath_t>(kEngBuildPath)(
        reinterpret_cast<void*>(kEngSession), nullptr, idbuf, path, 0);
    void* mgr = reinterpret_cast<TexMgr_t>(kEngTexMgr)();
    void* key = reinterpret_cast<MakeKey_t>(kEngMakeKey)(path);
    void* texv = reinterpret_cast<LoadTex_t>(kEngLoadTex)(mgr, nullptr, key);
    if (!texv) return;
    auto* t = reinterpret_cast<uint8_t*>(texv);
    const int sw = *reinterpret_cast<int*>(t + 0x114);
    const int sh = *reinterpret_cast<int*>(t + 0x118);
    const uint32_t* spx = *reinterpret_cast<uint32_t**>(t + 0x11c);
    if (!spx || sw <= 0 || sh <= 0) return;
    for (int dy = 0; dy < kChatIconSize; ++dy) {
      const int syy = dy * sh / kChatIconSize;
      for (int dx = 0; dx < kChatIconSize; ++dx)
        dbuf[dy * kChatIconSize + dx] = spx[syy * sw + (dx * sw / kChatIconSize)];
    }
    void* node = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ctx) + 8);
    reinterpret_cast<NodeBlitFn>(kEngNodeBlit)(node, nullptr, x, y,
                                               kChatIconSize, kChatIconSize, dbuf, 1);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void __fastcall TextOutLowHook(void* ctx, void* edx, int x, int y, char* str,
                               unsigned len) {
  // Nothing to do unless the run carries an injected ^i{<base62>} icon token.
  unsigned tok, end;
  uint32_t id;
  if (!str || len < 4 || !NextIconToken(str, len, 0, &tok, &end, &id)) {
    g_textout_low_orig(ctx, edx, x, y, str, len);
    return;
  }

  // Draw the run in pieces at explicit x: plain segments via the original, and
  // for each token blit the icon into a kIconAdvance-wide gap (matching what
  // MeasureHook told the layout) so the following name/link click-target stays
  // exactly where it expects — no shift, no doubling.
  int cur_x = x;
  unsigned seg = 0;
  do {
    if (tok > seg) {
      g_textout_low_orig(ctx, edx, cur_x, y, str + seg, tok - seg);
      cur_x += MeasureSEH(ctx, str + seg, static_cast<int>(tok - seg));
    }
    if (id > 0) BlitIconAtSEH(ctx, cur_x, y, id);
    cur_x += kIconAdvance;
    seg = end;
  } while (NextIconToken(str, len, seg, &tok, &end, &id));
  // draw the trailing segment
  if (seg < len)
    g_textout_low_orig(ctx, edx, cur_x, y, str + seg, len - seg);
}

// NOTE: chat font height/weight was investigated and deliberately DROPPED — see the
// project memory "Chat font size (dropped)".  The size lever is real (PaintLines @
// 0x008539c0 passes size 0xc to UIText_DrawColored -> FUN_00546f60 -> CreateFontA
// nHeight) but a clean chat-only implementation needs a PaintLines hook plus pitch
// scaling and is not worth the surface area, so no code for it lives here.

// ── Chat width resize (v1 — needs in-game testing) ─────────────────────────
// The main chat (UINewChatWnd) ships height-only: its WndProc (0x008fc220)
// msg 0xE handles resize mode 2 (height, discrete tab-bar rows) and DROPS mode
// 0/1 (width).  Worse, the layout hard-codes a ~600px width — input box
// (this+0xbc, width 0x1cc, x 0x6e) and filter buttons (this+0xe0, x 0x24a/0x23c)
// use fixed coords.  Here we re-add the width branch and reflow the width-
// dependent controls.  Resize comes from UIResizeButton_OnDrag -> parent
// vtable+0x94: mode 0/1 = width (newW = param_4), 2/3 = height.
//
// WndProc is __thiscall(this, param_1, msg, mode, newW, newH, p6); hooked as
// __fastcall(ecx=this, edx=dummy, param_1, msg, mode, newW, newH, p6).
// Fields: this+0x14 width, this+0x18 height, this+0x108 = width-0x14 (right-
// align base read by LayoutButtons), this+0x30 tab rows, this+0xbc input box.
//
// KNOWN-INCOMPLETE (v1): filter buttons (this+0xe0) keep fixed x; the resize
// relies on SetTabBarHeight for relayout, which has a drag-guard — verify in
// game whether controls reflow during the drag.
constexpr uintptr_t kChatWndProc      = 0x008fc220;  // __thiscall WndProc (vtable+0x94)
constexpr uintptr_t kSetTabBarHeight  = 0x00903160;  // __thiscall(this, rows) — relayout
constexpr uintptr_t kInputRowLayout   = 0x008f9840;  // __thiscall(this) — input-row controls
constexpr uintptr_t kUISetSize        = 0x00a1cb70;   // __thiscall(obj, w, h)
constexpr uintptr_t kRebuildFromHist  = 0x008642d0;   // __thiscall(tab): clear+re-wrap history
constexpr uintptr_t kResizeBtnVtable  = 0x0102a260;   // UIResizeButton vtable (PTR_FUN_0102a260)
constexpr int kResizeBtnModeOff       = 0x9c;         // UIResizeButton mode field (2 = height bar)
constexpr int kChatMinWidth = 0x140;  // 320 px floor
// Gap (px) between the right edge of the rightmost input-row filter button and the
// window's right edge.  The frame border occupies the last ~6px (this+0xcc = width-6),
// so this must exceed 6 to keep the button fully visible.  Tunable: bigger = more
// margin from the border, smaller = closer to the edge.
constexpr int kFilterRightInset = 5;

using ChatWndProcFn     = uint32_t (__fastcall*)(void*, void*, void*, int, int, int, int, int);
using SetTabBarHeightFn = void (__fastcall*)(void*, void*, int);
using InputRowLayoutFn  = void (__fastcall*)(void*, void*);
using UISetSizeFn       = void (__fastcall*)(void*, void*, int, int);
using RebuildFn         = void (__fastcall*)(void*);
ChatWndProcFn g_chat_wndproc_orig = nullptr;
InputRowLayoutFn g_inputrow_orig = nullptr;

// SEH-guarded: apply |new_w| to the chat window, resize the input box to follow,
// and re-run the engine relayout.  No C++ objects in scope (legal __try).
static void ApplyChatWidthSEH(void* wnd, int new_w) {
  __try {
    auto* w = reinterpret_cast<uint8_t*>(wnd);
    // Remember the engine default width on the very first apply (any path), so
    // disabling can restore it exactly.
    if (g_chat_orig_width == 0)
      g_chat_orig_width = *reinterpret_cast<int*>(w + 0x14);
    if (new_w < kChatMinWidth) new_w = kChatMinWidth;
    *reinterpret_cast<int*>(w + 0x14)  = new_w;         // window width
    *reinterpret_cast<int*>(w + 0x108) = new_w - 0x14;  // right-align base
    // Input box spans x=0x6e..(width - right margin 0x1e); keep height 0x10.
    // Resize the input's logical width (text region follows this).  We do NOT
    // touch input+0x88 — that's the WARP-patched max-char count, left fixed.
    void* input = *reinterpret_cast<void**>(w + 0xbc);
    const int input_w = new_w - 0x8c;
    if (input && input_w > 0)
      reinterpret_cast<UISetSizeFn>(kUISetSize)(input, nullptr, input_w, 0x10);
    // Re-run the engine relayout (frame + buttons + tab POSITIONS and the tab
    // WIDTHS) with the current tab-row count.
    const int rows = *reinterpret_cast<int*>(w + 0x30);
    // SetTabBarHeight relays out the chat (frame + tab positions/widths) and, at its
    // tail, calls the input-row layout (kInputRowLayout) which we hook to re-align the
    // filter buttons.  Calling it here covers the resize path; the hook also fires on
    // input show/hide (OnLButtonDown calls kInputRowLayout directly), so the buttons
    // stay put across every relayout, not just our apply.
    reinterpret_cast<SetTabBarHeightFn>(kSetTabBarHeight)(wnd, nullptr, rows);
    // Re-wrap the chat LOG to the new width: each channel tab now has the new
    // width (tab+0x14), so UISubChatWnd_RebuildFromHistory clears the drawn
    // lines and re-wraps the whole raw history at that width.  It CLEARS first,
    // so calling it every apply (slider drag) is idempotent — no leak, unlike
    // recreating the scroll controls via UIWindow_SetSize (UISubChatWnd_OnResize
    // duplicates them).  Tab vector is [this+0xf4, this+0xf8).
    int* it  = *reinterpret_cast<int**>(w + 0xf4);
    int* end = *reinterpret_cast<int**>(w + 0xf8);
    for (; it != end; ++it) {
      void* tab = reinterpret_cast<void*>(*it);
      if (tab) reinterpret_cast<RebuildFn>(kRebuildFromHist)(tab);
    }
    // Stretch the TOP height-resize bar to follow the width.  It's an anonymous
    // mode-2 UIResizeButton (created at width-0x1a, NOT kept in a this+ field and
    // never re-widened), so past the default width its grab strip ends mid-chat and
    // the handle can't be grabbed on the right.  Walk the child list
    // (this+0x50 std::list; node+0=next, node+8=child) for the mode-2 resize button
    // and set its hit width to new_w-0x1a (its x=3 is kept by create/SetTabBarHeight).
    uint8_t* sentinel = *reinterpret_cast<uint8_t**>(w + 0x50);
    if (sentinel) {
      for (uint8_t* n = *reinterpret_cast<uint8_t**>(sentinel);
           n && n != sentinel; n = *reinterpret_cast<uint8_t**>(n)) {
        auto* child = *reinterpret_cast<uint8_t**>(n + 8);
        if (child && *reinterpret_cast<uintptr_t*>(child) == kResizeBtnVtable &&
            *reinterpret_cast<int*>(child + kResizeBtnModeOff) == 2) {
          // Resize via UIWindow_SetSize (rebuilds the render node) — NOT a direct
          // +0x14 write: the grip's DrawContent paints its line across +0x14 into
          // the node, so widening +0x14 alone leaves the extra span as a black
          // (un-colorkeyed) strip.  Only when the width actually changed, to avoid
          // rebuilding the node on every relayout.
          const int want = new_w - 0x1a;
          if (*reinterpret_cast<int*>(child + 0x14) != want)
            reinterpret_cast<UISetSizeFn>(kUISetSize)(
                child, nullptr, want, *reinterpret_cast<int*>(child + 0x18));
          break;
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Hooked input-row layout (FUN_008f9840): the engine lays out the input-row controls
// here — including the two right-hand filter buttons (this+0xe0[1] and [2]) which it
// HARD-CODES at x=0x24a/0x23c (586/572, i.e. assuming the 600px creation width).  On a
// widened chat that leaves them mid-input.  This is the SINGLE chokepoint for those
// buttons: it runs on input show/hide (OnLButtonDown calls it directly) AND at the
// tail of SetTabBarHeight (resize), so re-aligning here makes them stick across every
// path (this fixes the reset-on-input-toggle — SetTabBarHeight is NOT on that path).
// We anchor the rightmost button (btn1) by its RIGHT edge at (width - kFilterRightInset)
// — just inside the frame border — using its live width (+0x14), and sit btn2 FLUSH to
// its left.  This is border-relative (no creation-width magic) so it is correct at any
// width.  Buttons keep their y (+0x20); skipped while hidden (orig parks them at x<0).
void __fastcall InputRowLayoutHook(void* ecx, void* edx) {
  g_inputrow_orig(ecx, edx);
  if (g_chat_width <= 0) return;  // custom width active? (any value, narrow or wide)
  __try {
    auto* w = reinterpret_cast<uint8_t*>(ecx);
    const int cur_w = *reinterpret_cast<int*>(w + 0x14);
    if (cur_w < kChatMinWidth) return;  // sanity; never below our 320px floor
    // Always re-anchor to the CURRENT right edge — for ANY custom width, not just
    // when wider than the default.  The engine hard-codes these buttons at x=586/572
    // (the 600px creation width), which lands off-screen on a narrow chat; previously
    // bailing below the default width handed them back to that, so a sub-600 resize
    // flip-flopped between our anchor and 586/572 (the "spread then vanish" glitch).
    const int right = cur_w - kFilterRightInset;  // target right edge, inside the border
    auto* btn1 = *reinterpret_cast<uint8_t**>(w + 0xe4);  // this+0xe0[1] (rightmost)
    auto* btn2 = *reinterpret_cast<uint8_t**>(w + 0xe8);  // this+0xe0[2] (left of btn1)
    int x1 = right;
    if (btn1 && *reinterpret_cast<int*>(btn1 + 0x1c) >= 0) {  // shown, not parked off-screen
      x1 = right - *reinterpret_cast<int*>(btn1 + 0x14);  // btn1 right edge at |right|
      reinterpret_cast<void (__fastcall*)(void*, void*, int, int)>(
          *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(btn1) + 0x10))(
          btn1, nullptr, x1, *reinterpret_cast<int*>(btn1 + 0x20));
    }
    if (btn2 && *reinterpret_cast<int*>(btn2 + 0x1c) >= 0)
      reinterpret_cast<void (__fastcall*)(void*, void*, int, int)>(
          *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(btn2) + 0x10))(
          btn2, nullptr, x1 - *reinterpret_cast<int*>(btn2 + 0x14),  // flush left of btn1
          *reinterpret_cast<int*>(btn2 + 0x20));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// SEH-guarded: cheaply re-assert the custom WRAP width on every channel tab
// (tab+0x14), WITHOUT a full relayout/rebuild.  The stock new-line handler
// (WndProc msg 0x25) word-wraps each incoming line to tab+0x14 (and gates
// wrapping on tab+0x14 - 5), but some engine relayout silently reverts tab+0x14
// back to the ~600px default between our applies — so NEW lines wrapped narrow
// even though RebuildFromHistory (run at apply time, when tab+0x14 was still
// custom) had re-wrapped the existing history wide.  Writing tab+0x14 right
// before the stock handler runs makes its wrap use the custom width with no
// rebuild.  Guarded on the main window already being custom (this+0x14 ==
// g_chat_width) so it never runs before the first full apply has captured the
// engine default — that path is left to ApplyChatWidthSEH.
static void ReassertTabWidthsSEH(void* wnd) {
  __try {
    if (g_chat_width < kChatMinWidth) return;
    auto* w = reinterpret_cast<uint8_t*>(wnd);
    if (*reinterpret_cast<int*>(w + 0x14) != g_chat_width) return;
    const int tab_w = g_chat_width - 8;  // matches SetTabBarHeight's mainW-8
    int* it  = *reinterpret_cast<int**>(w + 0xf4);
    int* end = *reinterpret_cast<int**>(w + 0xf8);
    for (; it != end; ++it) {
      void* tab = reinterpret_cast<void*>(*it);
      if (tab)
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(tab) + 0x14) = tab_w;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

uint32_t __fastcall ChatWndProcHook(void* ecx, void* edx, void* p1, int msg,
                                    int mode, int new_w, int new_h, int p6) {
  g_chat_wnd = ecx;  // the WndProc 'this' is always the main chat window
  // BEFORE the stock handler: re-assert the per-tab wrap width so an incoming new
  // line (msg 0x25 pixel-wraps to tab+0x14 inside this call) uses the custom width.
  if (g_chat_width > 0 && !g_in_apply)
    ReassertTabWidthsSEH(ecx);
  uint32_t r = g_chat_wndproc_orig(ecx, edx, p1, msg, mode, new_w, new_h, p6);
  // Keep the chat at the user-chosen width: the stock layout only reflows on a
  // relayout (resize / tab switch / new line), so whenever one runs and the width
  // has drifted from g_chat_width, re-apply it.  Also widens the chat from the
  // first relayout instead of only after a height drag.
  if (g_chat_width > 0 && !g_in_apply) {
    const int cur = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ecx) + 0x14);
    if (cur != g_chat_width) {
      g_in_apply = true;
      ApplyChatWidthSEH(ecx, g_chat_width);
      g_in_apply = false;
    }
  }
  return r;
}

// Apply |target| to the live chat immediately (slider feedback).  No-op until
// the chat exists (g_chat_wnd is cached the first time its WndProc runs).
void EnforceChatWidthNow(int target) {
  if (!g_chat_wnd || target <= 0) return;
  g_in_apply = true;
  ApplyChatWidthSEH(g_chat_wnd, target);
  g_in_apply = false;
}

// ── Clear chat history ──────────────────────────────────────────────────────
// Empty a chat window's per-channel raw-history vectors (text @tab+0x100, colors
// @tab+0x10c, senders @tab+0x118) via std::vector::resize(0), then
// UISubChatWnd_RebuildFromHistory (clears the drawn lines + re-wraps the now-empty
// history -> blank). Iterates the window's tab vector [wnd+0xf4, wnd+0xf8).
// SEH-guarded with no C++ objects in scope (legal __try). Runs from OnRenderUI,
// the same context the width re-wrap already calls RebuildFromHistory from.
constexpr uintptr_t kVecStrResize = 0x007103d0;  // std::vector<std::string>::resize
constexpr uintptr_t kVecU32Resize = 0x0053faa0;  // std::vector<uint32_t>::resize
using VecResizeFn = void (__fastcall*)(void*, void*, unsigned);

// Clear one channel tab (UISubChatWnd).  Calls game funcs — invoke only inside SEH.
static void ClearOneTab(char* tab) {
  if (!tab) return;
  reinterpret_cast<VecResizeFn>(kVecStrResize)(tab + 0x100, nullptr, 0);  // text
  reinterpret_cast<VecResizeFn>(kVecU32Resize)(tab + 0x10c, nullptr, 0);  // colors
  reinterpret_cast<VecResizeFn>(kVecStrResize)(tab + 0x118, nullptr, 0);  // senders
  reinterpret_cast<RebuildFn>(kRebuildFromHist)(tab);  // clear drawn + re-wrap
}

// Main chat (UINewChatWnd): its channel tabs are a std::vector [wnd+0xf4, wnd+0xf8).
static void ClearChatWindowHistorySEH(void* wnd) {
  __try {
    int* it  = *reinterpret_cast<int**>(reinterpret_cast<char*>(wnd) + 0xf4);
    int* end = *reinterpret_cast<int**>(reinterpret_cast<char*>(wnd) + 0xf8);
    for (; it != end; ++it) ClearOneTab(reinterpret_cast<char*>(*it));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Detached chat windows (UIChatWnd — a SEPARATE class from the main UINewChatWnd)
// keep their OWN history, so clearing the main window leaves them untouched. They
// live in a std::set whose head node is the global at 0x0131f510 (MSVC _Tree node:
// +0=_Left, +4=_Parent, +8=_Right, +0xd=_Isnil byte, value @+0x14 = the UIChatWnd*);
// each window's active tab is at wnd+0xb4. Walk the tree (iterative DFS, nil-skip +
// stack/iteration caps for safety) and clear each window's tab. Mirrors the game's
// own per-window enumeration in ChatLog_SaveAllToFiles (0x00907030).
constexpr uintptr_t kDetachedChatTree  = 0x0131f510;  // std::set<UIChatWnd*> head node
constexpr int       kDetachedActiveTab = 0xb4;        // UIChatWnd -> active UISubChatWnd

static bool TreeNodeIsNil(int* n) {
  return !n || *(reinterpret_cast<char*>(n) + 0xd) != 0;  // _Isnil byte
}

static void ClearDetachedChatsSEH() {
  __try {
    // 0x0131f510 is the std::set OBJECT: [+0] = _Myhead (sentinel node ptr),
    // [+4] = _Mysize. The tree root is _Myhead._Parent (verified live: object ->
    // _Myhead 0x03b41da0 -> root node -> value@+0x14 = UIChatWnd -> tab@+0xb4).
    int* setobj   = reinterpret_cast<int*>(kDetachedChatTree);
    int* sentinel = reinterpret_cast<int*>(setobj[0]);  // _Myhead
    if (!sentinel) return;
    int* st[64];
    int  sp = 0;
    int* root = reinterpret_cast<int*>(sentinel[1]);    // _Myhead._Parent = root
    if (!TreeNodeIsNil(root)) st[sp++] = root;
    for (int guard = 0; sp > 0 && guard < 512; ++guard) {
      int* node = st[--sp];
      if (void* wnd = reinterpret_cast<void*>(node[5]))  // value @node+0x14
        ClearOneTab(*reinterpret_cast<char**>(
            reinterpret_cast<char*>(wnd) + kDetachedActiveTab));
      int* l = reinterpret_cast<int*>(node[0]);  // _Left
      int* r = reinterpret_cast<int*>(node[2]);  // _Right
      if (sp < 62) {
        if (!TreeNodeIsNil(l)) st[sp++] = l;
        if (!TreeNodeIsNil(r)) st[sp++] = r;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
}  // namespace

ChatTweaks::ChatTweaks() {
  // Scan du .text pour les couleurs de fond (chat_bg.cc). Fait dans le ctor, donc
  // AVANT toute lecture de configuration : les pickers sont amorcés avec ce que
  // porte le binaire, et le yaml n'a plus qu'à écraser cette valeur.
  FindBackgroundSites();

  // Cache de mesure de texte : supprime le freeze de chat en combat (word-wrap
  // quadratique en appels GDI dans les rebuilds d'historique). --nomeasurecache
  // le désactive pour reproduire le comportement d'origine.
  g_measure_cache = (strstr(GetCommandLineA(), "--nomeasurecache") == nullptr);
  g_measure_text_orig = reinterpret_cast<MeasureTextFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kEngTextMeasure),
          reinterpret_cast<uint8_t*>(MeasureTextHook)));
  if (!g_measure_text_orig)
    LogError("[Chat] failed to hook text measure at 0x005474a0");

  // Hook the chat tab's "append drawn line" virtual (FUN_0083d840) to inject
  // ^i{id} before <ITEML> into the text that becomes a drawn chat line.
  g_append_line_orig = reinterpret_cast<AppendLineFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(0x0083d840),
          reinterpret_cast<uint8_t*>(AppendLineHook)));
  if (!g_append_line_orig) {
    LogError("[Chat] failed to hook chat AppendLine at 0x0083d840");
  }

  // Hook the per-message raw-history writer (UISubChatWnd_AddLine) to optionally
  // prepend a [HH:MM:SS] timestamp (toggled via chat::SetTimestamps).
  g_addline_orig = reinterpret_cast<AddLineFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kSubChatAddLine),
          reinterpret_cast<uint8_t*>(AddLineHook)));
  if (!g_addline_orig) {
    LogError("[Chat] failed to hook UISubChatWnd_AddLine at 0x0083f070");
  }

  // Hook the per-line dispatch so DETACHED chat windows' live lines get the
  // timestamp too (gated to the detached call site — see WrapAndDispatchHook).
  g_wrap_dispatch_orig = reinterpret_cast<WrapDispatchFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kWrapAndDispatch),
          reinterpret_cast<uint8_t*>(WrapAndDispatchHook)));
  if (!g_wrap_dispatch_orig) {
    LogError("[Chat] failed to hook UISubChatWnd_WrapAndDispatch at 0x0083d3f0");
  }

  // Hook the low-level GDI text-out (FUN_005471a0) to render ^i{id} tokens in
  // drawn text as item icons (blitting into the ctx's render node).
  g_textout_low_orig = reinterpret_cast<TextOutLowFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(0x005471a0),
          reinterpret_cast<uint8_t*>(TextOutLowHook)));
  if (!g_textout_low_orig) {
    LogError("[Chat] failed to hook GDI text-out at 0x005471a0");
  }

  // Hook the layout text-measure so ^i tokens count as the icon width (keeps the
  // icon flush against the name and the link click-target aligned).
  g_layout_measure_orig = reinterpret_cast<LayoutMeasureFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(0x00a21c90),
          reinterpret_cast<uint8_t*>(MeasureHook)));
  if (!g_layout_measure_orig) {
    LogError("[Chat] failed to hook layout measure at 0x00a21c90");
  }

  // Hook the chat WndProc (0x008fc220) to add a WIDTH resize branch to its msg
  // 0xE handler (the stock handler only does height).  v1 — needs in-game test.
  g_chat_wndproc_orig = reinterpret_cast<ChatWndProcFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kChatWndProc),
          reinterpret_cast<uint8_t*>(ChatWndProcHook)));
  if (!g_chat_wndproc_orig) {
    LogError("[Chat] failed to hook UINewChatWnd_WndProc at 0x008fc220");
  }

  // Hook the new-line char-wrap splitter (FUN_00979f10) so a widened chat wraps
  // new lines at the pixel width (tab+0x14) instead of the fixed 94-char cap.
  g_charwrap_orig = reinterpret_cast<CharWrapFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kCharWrap),
          reinterpret_cast<uint8_t*>(CharWrapHook)));
  if (!g_charwrap_orig) {
    LogError("[Chat] failed to hook line splitter at 0x00979f10");
  }

  // Hook the image blit so the fixed-width input frame (dialog_bg.bmp) can be
  // skipped on a widened chat (return-addr gated to its one call site in Paint).
  g_blit_orig = reinterpret_cast<BlitFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kBlitImageToNode),
          reinterpret_cast<uint8_t*>(BlitHook)));
  if (!g_blit_orig) {
    LogError("[Chat] failed to hook UIWindow_BlitImageToNode at 0x00a1d260");
  }

  // Hook the input-row layout (FUN_008f9840) so the two right-hand filter buttons are
  // re-aligned to the widened right edge after every relayout — the engine resets them
  // on input show/hide (OnLButtonDown) and at the tail of SetTabBarHeight (resize).
  g_inputrow_orig = reinterpret_cast<InputRowLayoutFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kInputRowLayout),
          reinterpret_cast<uint8_t*>(InputRowLayoutHook)));
  if (!g_inputrow_orig) {
    LogError("[Chat] failed to hook UINewChatWnd input-row layout at 0x008f9840");
  }
}

void ChatTweaks::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  // Chat window is destroyed on logout; drop the cached ptr + captured default
  // so they re-acquire next game session (avoids touching a stale pointer).
  if (mode_type != ModeMgr::ModeType::kGame) {
    g_chat_wnd = nullptr;
    g_chat_orig_width = 0;
  }
}

namespace chat {
void SetCustomWidth(bool enabled, int px) {
  if (enabled) {
    if (px < 320)  px = 320;
    if (px > 1200) px = 1200;
    g_chat_width = px;
    EnforceChatWidthNow(px);
  } else {
    const int orig = g_chat_orig_width;
    g_chat_width = 0;                          // stop enforcing
    if (orig > 0) EnforceChatWidthNow(orig);   // restore engine default
  }
}

void SetTimestamps(bool enabled) { g_chat_timestamps = enabled; }
void SetItemIcons(bool enabled)  { g_chat_item_icons = enabled; }

void ClearHistory() {
  if (g_chat_wnd) ClearChatWindowHistorySEH(g_chat_wnd);  // main window's tab vector
  ClearDetachedChatsSEH();  // every detached UIChatWnd keeps its own history
}
}  // namespace chat

// ── Réglages du plugin ───────────────────────────────────────────────────────

void ChatTweaks::ApplySettings() {
  // Le bornage vit ici, et non chez l'appelant : un yaml édité à la main ne doit
  // pas pouvoir rendre le chat inutilisable, quel que soit le chemin d'arrivée.
  if (custom_width_px_ < 320)  custom_width_px_ = 320;
  if (custom_width_px_ > 1200) custom_width_px_ = 1200;
  chat::SetCustomWidth(custom_width_, custom_width_px_);
  chat::SetTimestamps(timestamps_);
  chat::SetItemIcons(item_icons_);
  ApplyAllBackgrounds();
}

bool ChatTweaks::DrawSettings() {
  bool changed = false;

  if (ro::RoCheckbox("Largeur du chat", &custom_width_)) {
    chat::SetCustomWidth(custom_width_, custom_width_px_);
    changed = true;
  }

  if (custom_width_) {
    // Le slider bouge à 60 Hz, mais chat::SetCustomWidth relance le relayout
    // natif ET RebuildFromHistory sur TOUS les onglets — le chemin exact du
    // freeze de word-wrap déjà corrigé côté mesure. On ne l'applique donc qu'au
    // RELÂCHEMENT du slider, pas à chaque frame de glissement.
    // `moved && !IsItemActive()` = ajustement à la MOLETTE (WheelSliderInt la
    // traite hors du slider, sans jamais « désactiver » l'item) : on applique
    // tout de suite. `IsItemDeactivatedAfterEdit()` = fin de drag ou fin de
    // saisie Ctrl+clic. Pendant le drag l'item est actif : on ne fait rien.
    const bool moved = WheelSliderInt("Largeur (px)", &custom_width_px_, 320, 1200);
    if ((moved && !ImGui::IsItemActive()) || ImGui::IsItemDeactivatedAfterEdit()) {
      chat::SetCustomWidth(true, custom_width_px_);
      changed = true;
    }
  }

  if (ro::RoCheckbox("Horodatage du chat", &timestamps_)) {
    chat::SetTimestamps(timestamps_);
    changed = true;
  }

  if (ro::RoCheckbox("Icônes d'objets", &item_icons_)) {
    chat::SetItemIcons(item_icons_);
    changed = true;
  }

  if (ro::RoButton("Effacer l'historique du chat")) chat::ClearHistory();
  SameLine();
  HelpMarker(
      "Vide l'historique de tous les canaux de la fenêtre de chat principale "
      "(historique brut effacé + affichage vidé). Les nouveaux messages "
      "réapparaissent normalement ensuite.");

  return changed;
}
