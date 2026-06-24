#include "plugins/moonlight_ui.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "bourgeon.h"
#include "imgui.h"
#include "plugins/discord_relay.h"
#include "plugins/dps_meter.h"
#include "ragnarok/ui_window_mgr.h"
#include "spdlog/fmt/fmt.h"
#include "utils/byte_pattern.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

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

void __fastcall AppendLineHook(void* ecx, void* edx, char* text, uint32_t p2,
                               void* p3) {
  if (text && std::strstr(text, "<ITEML>")) {
    std::string modified = InjectItemIcons(text);
    g_append_line_orig(ecx, edx, const_cast<char*>(modified.c_str()), p2, p3);
    return;
  }
  g_append_line_orig(ecx, edx, text, p2, p3);
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
}  // namespace

// ── Item description window hook ──────────────────────────────────────────
// Hooks FUN_008c18b0 (__thiscall, 20250716 client) to capture the nameid of
// whichever item the player right-clicks to inspect.
//
// The function is a UI window message handler.  Message 0x18 means "set item":
//   param_3 is the item data struct; the item's nameid is stored as a
//   MSVC std::string (SSO layout) at byte offset 0x2C (= param_3[11]):
//     [0..3]  char* ptr  (or inline buf when small)
//     [4..7]  inline buf continued
//     [8..11] inline buf continued
//     [12]    _Mysize (string length)
//     [16]    _Myres  (capacity - 1)  <- checked against 15 to detect heap
//   If capacity-1 > 15 the string is heap-allocated and param_3[11] is a ptr;
//   otherwise the 16-byte inline buffer starts at param_3+0x2C.
//   atoi() on that text gives the numeric nameid.

using ItemDescWndFn = int (__fastcall*)(void*, void*, uint32_t, int, int*, int, int, int);
static ItemDescWndFn g_item_desc_wnd_orig  = nullptr;
static uint32_t      g_last_viewed_item    = 0;
static POINT         g_item_desc_cursor    = {0, 0};
static bool          g_item_desc_visible   = false;  // set on 0x18, cleared on close msg
static void*         g_item_desc_wnd_ptr   = nullptr; // ecx of the desc window object

static int __fastcall ItemDescWndHook(void* ecx, void* /*edx*/,
                                       uint32_t p1, int p2, int* p3,
                                       int p4, int p5, int p6) {
  if (p2 == 0x18 && p3 != nullptr) {
    // Ignore 0x18 from secondary windows (e.g. equipment comparison window).
    // Lock onto the first ecx that sends 0x18 while no tooltip is open.
    if (g_item_desc_visible && ecx != g_item_desc_wnd_ptr)
      return g_item_desc_wnd_orig(ecx, nullptr, p1, p2, p3, p4, p5, p6);

    const int* sso = p3 + 11;  // std::string at byte offset 0x2C
    const char* str;
    if (static_cast<uint32_t>(sso[5]) > 15u)
      str = *reinterpret_cast<const char* const*>(sso);
    else
      str = reinterpret_cast<const char*>(sso);
    if (str) {
      const long id = std::atol(str);
      if (id > 0) {
        if (g_item_desc_visible && static_cast<uint32_t>(id) == g_last_viewed_item) {
          LogInfo("[ItemDescWnd] same-item 0x18: id={} ecx=0x{:08X} -> toggle-close",
                  id, reinterpret_cast<uint32_t>(ecx));
          g_item_desc_visible = false;
          g_item_desc_wnd_ptr = nullptr;
        } else {
          g_last_viewed_item  = static_cast<uint32_t>(id);
          g_item_desc_visible = true;
          g_item_desc_wnd_ptr = ecx;
          GetCursorPos(&g_item_desc_cursor);
        }
      }
    }
  } else if (p2 == 0x06 && ecx == g_item_desc_wnd_ptr) {
    // X button close — also reset ptr for the same reason.
    g_item_desc_visible = false;
    g_item_desc_wnd_ptr = nullptr;
  }
  return g_item_desc_wnd_orig(ecx, nullptr, p1, p2, p3, p4, p5, p6);
}

// Returns the path to bourgeon_settings.yaml next to the game executable.
static std::string GetSettingsPath() {
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string path(buf);
  const auto sep = path.find_last_of("\\/");
  if (sep != std::string::npos) path.resize(sep + 1);
  return path + "bourgeon_settings.yaml";
}

void MoonlightUi::LoadItemNames() {
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string base(buf);
  const auto sep = base.find_last_of("\\/");
  if (sep != std::string::npos) base.resize(sep + 1);

  // Try common RO client layouts in order.
  static const char* kCandidates[] = {
    "System\\itemInfoMerged.lua",
    "SystemEN\\itemInfoMerged.lua",
    "System\\itemInfo.lua",
    "SystemEN\\itemInfo.lua",
  };
  std::ifstream f;
  std::string path;
  for (const char* cand : kCandidates) {
    path = base + cand;
    f.open(path);
    if (f) break;
    f.clear();
  }
  if (!f) {
    LogError("[MoonlightUi] itemInfoMerged.lua not found (tried System\\ and SystemEN\\)");
    return;
  }
  LogInfo("[MoonlightUi] loading item names from {}", path);

  uint32_t current_id = 0;
  std::string line;
  while (std::getline(f, line)) {
    // Match item ID line: \t[501] = {
    // Only treat as item ID when the bracket content is purely numeric.
    // Lines like  identifiedDisplayName = "Horn Card [Shield]",  contain
    // non-numeric brackets and must fall through to the Name check below.
    const auto lb = line.find('[');
    if (lb != std::string::npos) {
      const auto rb = line.find(']', lb + 1);
      if (rb != std::string::npos) {
        const auto between = line.substr(lb + 1, rb - lb - 1);
        const bool all_digits = !between.empty() &&
            std::all_of(between.begin(), between.end(),
                        [](unsigned char c){ return std::isdigit(c) != 0; });
        if (all_digits) {
          try {
            current_id = static_cast<uint32_t>(std::stoul(between));
          } catch (...) { current_id = 0; }
          continue;  // item ID line fully consumed
        }
        // Non-numeric bracket (e.g. "[Shield]"): fall through to Name check.
      }
    }
    // Match: Name = "Red Potion"  (compact format used by this client)
    // Also handles legacy: identifiedDisplayName = "Sleipnir",
    if (current_id > 0 &&
        (line.find("Name =") != std::string::npos)) {
      const auto q1 = line.find('"');
      const auto q2 = line.rfind('"');  // rfind: last quote handles names with "
      if (q1 != std::string::npos && q2 != std::string::npos && q1 < q2)
        item_names_[current_id] = line.substr(q1 + 1, q2 - q1 - 1);
    }
  }
  LogInfo("[MoonlightUi] loaded {} item names", item_names_.size());
}

MoonlightUi::MoonlightUi() {
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeFromServer);
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodePresetList);
  // Observe the standard map-move packet to learn the current map name.
  Bourgeon::Instance().RegisterObserveOpcode(kOpcodeMapMove, kMapNameLen);
  FindChatBgSites();
  LoadItemNames();

  // Hook the item description window to capture the nameid when right-clicking.
  g_item_desc_wnd_orig = reinterpret_cast<ItemDescWndFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kItemDescWndAddr),
          reinterpret_cast<uint8_t*>(ItemDescWndHook)));
  if (!g_item_desc_wnd_orig) {
    LogError("[MoonlightUi] failed to hook item desc wnd at 0x{:08X}",
             kItemDescWndAddr);
  }

  // Hook the chat tab's "append drawn line" virtual (FUN_0083d840) to inject
  // ^i[id] before <ITEML> into the text that becomes a drawn chat line.
  g_append_line_orig = reinterpret_cast<AppendLineFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(0x0083d840),
          reinterpret_cast<uint8_t*>(AppendLineHook)));
  if (!g_append_line_orig) {
    LogError("[MoonlightUi] failed to hook chat AppendLine at 0x0083d840");
  }

  // Hook the low-level GDI text-out (FUN_005471a0) to render ^i[id] tokens in
  // drawn text as item icons (blitting into the ctx's render node).
  g_textout_low_orig = reinterpret_cast<TextOutLowFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(0x005471a0),
          reinterpret_cast<uint8_t*>(TextOutLowHook)));
  if (!g_textout_low_orig) {
    LogError("[MoonlightUi] failed to hook GDI text-out at 0x005471a0");
  }

  // Hook the layout text-measure so ^i tokens count as the icon width (keeps the
  // icon flush against the name and the link click-target aligned).
  g_layout_measure_orig = reinterpret_cast<LayoutMeasureFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(0x00a21c90),
          reinterpret_cast<uint8_t*>(MeasureHook)));
  if (!g_layout_measure_orig) {
    LogError("[MoonlightUi] failed to hook layout measure at 0x00a21c90");
  }
}

// ── Chat background colours ───────────────────────────────────────────────

namespace {
// Static description of every chat-background site we patch.  Wildcards cover
// the 4-byte ARGB immediate so the search works whether or not a WARP binary
// patch was already applied to the exe.
struct ChatBgSiteDesc {
  int                  group;        // ChatBgGroupId
  std::vector<uint8_t> bytes;
  const char*          mask;
  size_t               imm_off;      // offset of the 4-byte ARGB inside the match
  uint32_t             heap_vtable;  // 0 = no heap recolour for this site
  uint32_t             heap_field;
};

const ChatBgSiteDesc kChatBgSites[] = {
  // ── Main chat panel ──────────────────────────────────────────────────────
  // Site 1: UINewChatWnd ctor  MOV [reg+0xD8], imm32   (colour stored in obj+0xD8)
  {0, {0xC7, 0x00, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      "x?xxxx????", 6, 0x01037F80, 0xD8},
  // Site 2: selected-tab colour  CMP / MOV ECX,imm32 / MOV EAX,0x99000000 / CMOVZ
  {0, {0x3B, 0x9E, 0x14, 0x01, 0x00, 0x00, 0xB9, 0x00, 0x00, 0x00, 0x00,
       0xB8, 0x00, 0x00, 0x00, 0x99, 0x0F, 0x44, 0xC1},
      "xxxxxxx????xxxxxxxx", 7, 0, 0},
  // ── Detached chat windows ────────────────────────────────────────────────
  // Site 3: outer border  MOV EAX,[ESI+0xEC] / PUSH imm32 / PUSH [ESI+0xE8]
  {1, {0x8B, 0x86, 0xEC, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xB6, 0xE8, 0x00, 0x00, 0x00},
      "xxxxxxx????xxxxxx", 7, 0, 0},
  // Site 8: UISubChatHisWnd ctor  MOV [ESI+0xD4], imm32 / MOV [ESI+0xC4], EAX
  {1, {0xC7, 0x86, 0xD4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x89, 0x86, 0xC4, 0x00, 0x00, 0x00},
      "xxxxxx????xxxxxx", 6, 0x01037EA8, 0xD4},
  // ── Whisper (1:1) window ──────────────────────────────────────────────────
  // Site 6: MOV EAX,[ESI+0x14] / PUSH imm32 / PUSH [ESI+0x18] / SUB EAX,2
  {2, {0x8B, 0x46, 0x14, 0x68, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x76, 0x18,
       0x83, 0xE8, 0x02},
      "xxxx????xxxxxx", 4, 0, 0},
};
}  // namespace

void MoonlightUi::FindChatBgSites() {
  chat_bg_[kChatBgMain].label        = "Main chat";
  chat_bg_[kChatBgMain].yaml_key     = "chat_bg";          // kept for back-compat
  chat_bg_[kChatBgDetached].label    = "Detached windows";
  chat_bg_[kChatBgDetached].yaml_key = "chat_bg_detached";
  chat_bg_[kChatBgWhisper].label     = "Whisper (1:1)";
  chat_bg_[kChatBgWhisper].yaml_key  = "chat_bg_whisper";

  // Locate the .text section via the PE header of the main module.
  const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandle(nullptr));
  const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto* sec  = IMAGE_FIRST_SECTION(nt);

  uint8_t* text_start = nullptr;
  size_t   text_size  = 0;
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (std::memcmp(sec->Name, ".text", 5) == 0) {
      text_start = const_cast<uint8_t*>(base) + sec->VirtualAddress;
      text_size  = sec->Misc.VirtualSize;
      break;
    }
  }
  if (!text_start) {
    LogError("[MoonlightUi] chat_bg: .text section not found");
    return;
  }

  LogInfo("[MoonlightUi] chat_bg: scanning .text [0x{:08X} .. +0x{:X}] for {} site(s)",
          reinterpret_cast<uint32_t>(text_start), text_size,
          static_cast<int>(sizeof(kChatBgSites) / sizeof(kChatBgSites[0])));

  int site_idx = 0;
  for (const ChatBgSiteDesc& d : kChatBgSites) {
    const char* gname = chat_bg_[d.group].label;
    BytePattern pat(d.bytes, d.mask);
    auto* found = static_cast<uint8_t*>(pat.Search(text_start, text_size));
    if (!found) {
      LogError("[MoonlightUi] chat_bg: site #{} (group {} '{}') NOT FOUND",
               site_idx, d.group, gname);
      ++site_idx;
      continue;
    }
    auto* imm = reinterpret_cast<uint32_t*>(found + d.imm_off);

    // Make the immediate field writable (one VirtualProtect, never restored).
    DWORD old_protect;
    VirtualProtect(imm, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &old_protect);

    ChatBgGroup& g = chat_bg_[d.group];
    g.instrs.push_back(imm);
    if (d.heap_vtable) g.heap.push_back({d.heap_vtable, d.heap_field});
    chat_bg_found_ = true;

    LogInfo("[MoonlightUi] chat_bg: site #{} (group {} '{}') found @ VA 0x{:08X}, "
            "imm @ +{} = 0x{:08X}{}",
            site_idx, d.group, gname, reinterpret_cast<uint32_t>(found),
            d.imm_off, *imm,
            d.heap_vtable ? " [+heap recolour]" : "");
    ++site_idx;
  }

  // Seed each picker from the colour currently in its first immediate, and log a
  // per-group summary so missing sites are obvious in the log.
  for (int i = 0; i < kChatBgCount; ++i) {
    ChatBgGroup& g = chat_bg_[i];
    if (!g.instrs.empty()) PickerFromArgb(g.color, *g.instrs.front());
    LogInfo("[MoonlightUi] chat_bg group {} '{}': {} instr site(s), {} heap target(s)",
            i, g.label, static_cast<int>(g.instrs.size()),
            static_cast<int>(g.heap.size()));
  }
}

void MoonlightUi::ApplyChatBg(ChatBgGroup& g, uint32_t argb, bool walk_heap) {
  for (uint32_t* p : g.instrs) {
    *p = argb;
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(uint32_t));
  }
  if (walk_heap && !g.heap.empty()) PatchChatBgObjects(g, argb);
}

void MoonlightUi::PatchChatBgObjects(const ChatBgGroup& g, uint32_t argb) {
  HANDLE heap = GetProcessHeap();
  if (!heap || !HeapLock(heap)) return;

  PROCESS_HEAP_ENTRY entry = {};
  int count = 0;
  while (HeapWalk(heap, &entry)) {
    if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
    // Window objects start with their vtable pointer.
    const auto* vtable_ptr = static_cast<const uint32_t*>(entry.lpData);
    for (const ChatBgHeapTarget& t : g.heap) {
      if (entry.cbData < t.field_off + sizeof(uint32_t)) continue;
      if (*vtable_ptr != t.vtable) continue;
      *reinterpret_cast<uint32_t*>(
          static_cast<uint8_t*>(entry.lpData) + t.field_off) = argb;
      ++count;
    }
  }

  HeapUnlock(heap);
  LogInfo("[MoonlightUi] chat_bg[{}]: recoloured {} live object(s)", g.yaml_key, count);
}

uint32_t MoonlightUi::ArgbFromPicker(const float c[4]) {
  const uint32_t r = static_cast<uint32_t>(c[0] * 255.0f + 0.5f) & 0xFF;
  const uint32_t g = static_cast<uint32_t>(c[1] * 255.0f + 0.5f) & 0xFF;
  const uint32_t b = static_cast<uint32_t>(c[2] * 255.0f + 0.5f) & 0xFF;
  const uint32_t a = static_cast<uint32_t>(c[3] * 255.0f + 0.5f) & 0xFF;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

void MoonlightUi::PickerFromArgb(float c[4], uint32_t argb) {
  c[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f; // R
  c[1] = static_cast<float>((argb >>  8) & 0xFF) / 255.0f; // G
  c[2] = static_cast<float>( argb        & 0xFF) / 255.0f; // B
  c[3] = static_cast<float>((argb >> 24) & 0xFF) / 255.0f; // A
}

// ── Settings persistence ──────────────────────────────────────────────────

void MoonlightUi::LoadSettings() {
  const std::string path = GetSettingsPath();
  std::ifstream f(path);
  if (!f) return;  // first run — no file yet

  try {
    const YAML::Node root = YAML::Load(f);
    const YAML::Node ui = root["moonlight_ui"];
    if (!ui) return;

    for (ChatBgGroup& g : chat_bg_) {
      const std::string hex = ui[g.yaml_key].as<std::string>("");
      if (hex.size() != 8) continue;
      const uint32_t argb = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
      PickerFromArgb(g.color, argb);
      if (!g.instrs.empty()) ApplyChatBg(g, argb, true);
      LogInfo("[MoonlightUi] loaded {} 0x{:08X}", g.yaml_key, argb);
    }

    ui_collapsed_         = ui["ui_collapsed"].as<bool>(false);
    show_alootid_overlay_ = ui["alootid_overlay"].as<bool>(false);
    mainchat_preset_bar_  = ui["mainchat_preset_bar"].as<bool>(false);
    log_level_            = ui["log_level"].as<std::string>("info");
    LogConsole::instance().SetLevel(log_level_);
    apply_collapse_ = true;

    if (auto* dps = Bourgeon::Instance().dps_meter())
      dps->show_ground_dmg_in_chat_ = ui["dps_ground_dmg_chat"].as<bool>(true);

    chat_bg_presets_.clear();
    if (const YAML::Node presets = ui["chat_bg_presets"]) {
      for (const YAML::Node& p : presets) {
        const std::string name  = p["name"].as<std::string>("");
        const std::string color = p["color"].as<std::string>("");
        if (name.empty() || color.size() != 8) continue;
        const uint32_t argb = static_cast<uint32_t>(std::stoul(color, nullptr, 16));
        chat_bg_presets_.push_back({name, argb});
      }
    }
  } catch (const std::exception& e) {
    LogError("[MoonlightUi] failed to parse {}: {}", path, e.what());
  }
}

void MoonlightUi::SaveSettings() {
  YAML::Emitter out;
  out << YAML::BeginMap
      << YAML::Key << "moonlight_ui"
      << YAML::Value << YAML::BeginMap;
  for (const ChatBgGroup& g : chat_bg_) {
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08X", ArgbFromPicker(g.color));
    out   << YAML::Key << g.yaml_key << YAML::Value << hex;
  }
  out     << YAML::Key << "ui_collapsed"          << YAML::Value << ui_collapsed_
        << YAML::Key << "log_level"            << YAML::Value << log_level_
        << YAML::Key << "alootid_overlay"      << YAML::Value << show_alootid_overlay_
        << YAML::Key << "mainchat_preset_bar"  << YAML::Value << mainchat_preset_bar_
        << YAML::Key << "dps_ground_dmg_chat"  << YAML::Value
            << (Bourgeon::Instance().dps_meter()
                    ? Bourgeon::Instance().dps_meter()->show_ground_dmg_in_chat_
                    : true)
        << YAML::Key << "chat_bg_presets" << YAML::Value << YAML::BeginSeq;
  for (const auto& p : chat_bg_presets_) {
    char pbuf[9];
    std::snprintf(pbuf, sizeof(pbuf), "%08X", p.argb);
    out << YAML::BeginMap
        << YAML::Key << "name"  << YAML::Value << p.name
        << YAML::Key << "color" << YAML::Value << pbuf
        << YAML::EndMap;
  }
  out       << YAML::EndSeq
      << YAML::EndMap
      << YAML::EndMap;

  const std::string path = GetSettingsPath();
  std::ofstream f(path);
  if (!f) {
    LogError("[MoonlightUi] failed to write {}", path);
    return;
  }
  f << out.c_str();
  LogInfo("[MoonlightUi] saved chat backgrounds to {}", path);
}

// ── Server settings sync ──────────────────────────────────────────────────

void MoonlightUi::UpdateRelay() {
  if (auto* relay = Bourgeon::Instance().discord_relay()) {
    relay->set_chat_active(discord_chat_ && in_gonryun_);
  }
}

void MoonlightUi::OnModeSwitch(ModeMgr::ModeType mode_type,
                               const char* map_name) {
  const bool was_in_game = in_game_;
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);

  // Only update in_gonryun_ when we have a real map name. OnUpdateHook fires
  // FireModeSwitch(kGame, "") on every tick for in_game_ tracking; that empty
  // call must not override the map we learned from a real CModeMgr::Switch.
  if (map_name && map_name[0] != '\0') {
    in_gonryun_ = in_game_ && (strncmp(map_name, kDiscordMap, sizeof(kDiscordMap) - 1) == 0);
  } else if (!in_game_) {
    in_gonryun_ = false;
  }

  if (in_game_ && !was_in_game)
    LoadSettings();

  if (!in_game_ && was_in_game)
    aloot_ids_.clear();

  UpdateRelay();
}

// ZC packet layout (data points past [opcode:2][total_len:2]):
//   [char_id:4][count:2][{id:2, value:4} * count]
void MoonlightUi::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  if (opcode == kOpcodeMapMove) {
    // 0x0091 ZC_NPCACK_MAPMOVE: data points at mapname[16] (e.g. "gonryun.gat").
    const char* map_name = reinterpret_cast<const char*>(data);
    in_gonryun_ = in_game_ &&
                  (strncmp(map_name, kDiscordMap, sizeof(kDiscordMap) - 1) == 0);
    LogInfo("[MoonlightUi] map move -> '{}' in_gonryun={}",
            std::string(map_name, strnlen(map_name, len)), in_gonryun_);
    UpdateRelay();
    return;
  }

  if (opcode == kOpcodePresetList) {
    // ZC_BOURGEON_PRESET_LIST: [active_no:1][count:1][{no:1,autoload:1,namelen:1,name:var}...]
    // data points past [opcode:2][len:2], so data[0]=active_no, data[1]=count.
    if (len < 2) return;
    alootid_active_preset_   = data[0];
    alootid_selected_preset_ = data[0];  // select active preset in combo by default
    const uint8_t count = data[1];
    alootid_presets_.clear();
    uint16_t off = 2;
    for (uint8_t i = 0; i < count && off + 3 <= len; ++i) {
      AlootPreset p;
      p.no       = data[off];
      p.autoload = data[off + 1] != 0;
      const uint8_t namelen = data[off + 2];
      off += 3;
      if (off + namelen > len) break;
      p.name.assign(reinterpret_cast<const char*>(data + off), namelen);
      off += namelen;
      alootid_presets_.push_back(std::move(p));
    }
    // Auto-fill the save input with the active preset's name so "Sauvegarder"
    // updates it directly instead of creating a new slot.
    if (alootid_active_preset_ != 0) {
      for (const auto& p : alootid_presets_) {
        if (p.no == alootid_active_preset_) {
          std::strncpy(alootid_preset_input_, p.name.c_str(),
                       sizeof(alootid_preset_input_) - 1);
          alootid_preset_input_[sizeof(alootid_preset_input_) - 1] = '\0';
          break;
        }
      }
    }
    // Snapshot the current list as "saved state" — the server sends this packet
    // after every save/load/delete, so the snapshot stays in sync automatically.
    alootid_saved_ids_ = aloot_ids_;
    return;
  }

  if (opcode != kOpcodeFromServer) return;
  // Layout after the [opcode:2][len:2] header: [char_id:4][count:2][{id,value}*].
  if (len < 6) return;

  const uint16_t count = *reinterpret_cast<const uint16_t*>(data + 4);
  const uint16_t expected_len = static_cast<uint16_t>(6 + count * 6);
  if (len < expected_len) {
    LogError("[MoonlightUi] ZC_BOURGEON_SETTINGS truncated: len={} count={}", len, count);
    return;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const uint16_t id    = *reinterpret_cast<const uint16_t*>(data + 6 + i * 6);
    const uint32_t value = *reinterpret_cast<const uint32_t*>(data + 6 + i * 6 + 2);
    switch (id) {
      case kSettingShowExp:
        show_exp_ = (value != 0);
        LogInfo("[MoonlightUi] show_exp={}", show_exp_);
        break;
      case kSettingShowZeny:
        show_zeny_ = (value != 0);
        LogInfo("[MoonlightUi] show_zeny={}", show_zeny_);
        break;
      case kSettingShowMobInfo:
        show_mob_info_ = (value != 0);
        LogInfo("[MoonlightUi] show_mob_info={}", show_mob_info_);
        break;
      case kSettingSeparate:
        separate_ = (value != 0);
        LogInfo("[MoonlightUi] separate={}", separate_);
        break;
      case kSettingBlockExp:
        block_exp_ = (value != 0);
        LogInfo("[MoonlightUi] block_exp={}", block_exp_);
        break;
      case kSettingAlootRare:
        aloot_rare_ = (value != 0);
        LogInfo("[MoonlightUi] aloot_rare={}", aloot_rare_);
        break;
      case kSettingAlootRate:
        aloot_rate_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] aloot_rate={}", aloot_rate_);
        break;
      case kSettingAlootPognon:
        aloot_pognon_ = static_cast<int>(value) * 100;
        LogInfo("[MoonlightUi] aloot_pognon={}", aloot_pognon_);
        break;
      case kSettingAlootType:
        aloot_type_mask_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] aloot_type_mask=0x{:04X}", aloot_type_mask_);
        break;
      case kSettingDiscordChat:
        discord_chat_ = (value != 0);
        LogInfo("[MoonlightUi] discord_chat={}", discord_chat_);
        UpdateRelay();
        break;
      case kSettingShowDelay:
        show_delay_ = (value != 0);
        LogInfo("[MoonlightUi] show_delay={}", show_delay_);
        break;
      case kSettingShowSpeed:
        show_speed_ = (value != 0);
        LogInfo("[MoonlightUi] show_speed={}", show_speed_);
        break;
      case kSettingSellStuff:
        sell_stuff_ = (value != 0);
        LogInfo("[MoonlightUi] sell_stuff={}", sell_stuff_);
        break;
      case kSettingSellItem:
        sell_item_ = (value != 0);
        LogInfo("[MoonlightUi] sell_item={}", sell_item_);
        break;
      case kSettingNoAsk:
        no_ask_ = (value != 0);
        LogInfo("[MoonlightUi] no_ask={}", no_ask_);
        break;
      case kSettingNoks:
        noks_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] noks={}", noks_);
        break;
      case kSettingWings:
        wings_ = (value != 0);
        LogInfo("[MoonlightUi] wings={}", wings_);
        break;
      case kSettingAlootMvp:
        aloot_mvp_ = (value != 0);
        LogInfo("[MoonlightUi] aloot_mvp={}", aloot_mvp_);
        break;
      case kSettingAlootMvpRwd:
        aloot_mvp_rwd_ = (value != 0);
        LogInfo("[MoonlightUi] aloot_mvp_rwd={}", aloot_mvp_rwd_);
        break;
      case kSettingTriInv:
        tri_inv_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] tri_inv={}", tri_inv_);
        break;
      case kSettingTriCart:
        tri_cart_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] tri_cart={}", tri_cart_);
        break;
      case kSettingTriStorage:
        tri_storage_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] tri_storage={}", tri_storage_);
        break;
      case kSettingTriGstorage:
        tri_gstorage_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] tri_gstorage={}", tri_gstorage_);
        break;
      case kSettingAlootId:
        if (value == 0) {
          aloot_ids_.clear();
          LogInfo("[MoonlightUi] aloot_ids cleared");
        } else {
          bool found = false;
          for (uint32_t x : aloot_ids_) if (x == value) { found = true; break; }
          if (!found) aloot_ids_.push_back(value);
          LogInfo("[MoonlightUi] aloot_id added={}", value);
        }
        break;
      case kSettingAlootIdRemove:
        break;
      default:
        LogInfo("[MoonlightUi] unknown setting id={} value={}", id, value);
        break;
    }
  }
}

void MoonlightUi::SendSetting(uint16_t id, uint32_t value) {
  uint8_t buf[10];
  *reinterpret_cast<uint16_t*>(buf)     = kOpcodeToServer;
  *reinterpret_cast<uint16_t*>(buf + 2) = 10;
  *reinterpret_cast<uint16_t*>(buf + 4) = id;
  *reinterpret_cast<uint32_t*>(buf + 6) = value;
  Bourgeon::Instance().SendPacket(buf, sizeof(buf));
}

void MoonlightUi::SendPresetCmd(uint8_t cmd, uint8_t no, const char* name) {
  const uint16_t namelen = name ? static_cast<uint16_t>(strnlen(name, 50)) : 0;
  const uint16_t total   = static_cast<uint16_t>(6 + namelen);
  std::vector<uint8_t> buf(total);
  *reinterpret_cast<uint16_t*>(buf.data())     = kOpcodePresetCmd;
  *reinterpret_cast<uint16_t*>(buf.data() + 2) = total;
  buf[4] = cmd;
  buf[5] = no;
  if (namelen > 0) std::memcpy(buf.data() + 6, name, namelen);
  Bourgeon::Instance().SendPacket(buf.data(), total);
}

// Helper to display a little (?) mark which shows a tooltip when hovered.
// In your own code you may want to display an actual icon if you are using a merged icon fonts (see docs/FONTS.md)
static void HelpMarker(const char* desc)
{
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip())
  {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
    ImGui::TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// Make the UI compact because there are so many fields
static void PushStyleCompact()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, (float)(int)(style.FramePadding.y * 0.60f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, (float)(int)(style.ItemSpacing.y * 0.60f)));
}

static void PopStyleCompact()
{
    ImGui::PopStyleVar(2);
}

// ── ImGui panel ───────────────────────────────────────────────────────────

void MoonlightUi::OnRenderUI() {
  if (!in_game_) return;

  if (apply_collapse_) {
    ImGui::SetNextWindowCollapsed(ui_collapsed_, ImGuiCond_Always);
    apply_collapse_ = false;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::Begin("Moonlight-Destiny");

  const bool is_collapsed = ImGui::IsWindowCollapsed();
  if (is_collapsed != ui_collapsed_) {
    ui_collapsed_ = is_collapsed;
    SaveSettings();
  }
  ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (!is_collapsed) {

    if (ImGui::CollapsingHeader("Règles du serveur"))
    {
      auto BulletWrapped = [](const char* text) {
        ImGui::Bullet(); ImGui::SameLine(); ImGui::TextWrapped("%s", text);
      };
      ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "CES RÈGLEMENTS S'APPLIQUENT PARTOUT SUR MOONLIGHT-DESTINY !");
      if (ImGui::TreeNode("Règlements généraux"))
      {
        ImGui::TextWrapped("Les règles du serveur doivent être appliquées à la lettre.\nToute personne ne respectant pas la charte sera sanctionnée dans les plus brefs délais.");
        ImGui::Spacing();
        BulletWrapped("Les joueurs doivent se respecter et garder un langage propre et courtois.");
        BulletWrapped("Les propos visant à rejeter un nouveau joueur sont interdits.");
        BulletWrapped("L'utilisation de programmes tels que bots ou hacks = ban définitif sans hésitation.");
        BulletWrapped("Le flood est strictement interdit.");
        BulletWrapped("Vous êtes entièrement responsable de votre compte.");
        BulletWrapped("Le staff ne rend pas les items perdus (vente NPC, deslotage raté, refine raté).");
        BulletWrapped("Le staff peut exceptionnellement rendre un item perdu si les logs prouvent un bug serveur.");
        BulletWrapped("Ne partagez jamais votre compte ou votre mot de passe.");
        BulletWrapped("La demande de support pour créer un serveur privé est non recommandée.");
        BulletWrapped("Le plagiat volontaire d'un membre du staff est puni.");
        BulletWrapped("Tout ce qui se rapporte au serveur est la propriété exclusive des administrateurs.");
        BulletWrapped("Le langage SMS est à proscrire.");
        BulletWrapped("L'exploitation d'un bug ou abus = sanction. Prévenez immédiatement un administrateur.");
        BulletWrapped("Si vous abusez du cashshop en votant avec plusieurs comptes forum… \ngare à vous c'est comme avec les impôts, \ntant qu'on est pas contrôlé c'est la fête, mais quand ils vous tombent dessus...");
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Sur le serveur de jeu"))
      {
        BulletWrapped("Insultes et vols de drop (Looting) = INTERDITS.");
        BulletWrapped("Heal ou buff un monstre qui ne vous appartient pas sans accord = puni.");
        BulletWrapped("Si vous êtes banni définitivement, tous les comptes liés à votre IP/PC le seront aussi.");
        BulletWrapped("Les sanctions (mute, jail, kick, ban) sont à la discrétion du staff.");
        BulletWrapped("Le Kill Steal est strictement interdit (voir définition). Utilisez @noks pour vous protéger.");
        ImGui::Spacing();
        BulletWrapped("Les MVPs sont FFA :");
        ImGui::Indent();
        ImGui::TextWrapped("Vous pouvez les attaquer même si quelqu'un est dessus.");
        ImGui::TextWrapped("(À vous de voir si vous voulez passer pour un gros connard selfish en KSant le MVP)");
        ImGui::Text("Si vous ne voulez pas vous faire KS, faites @noks <3");
        ImGui::Unindent();
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Le staff"))
      {
        BulletWrapped("Si vous cassez les couilles du staff ban/delete non temporaire.");
        BulletWrapped("Aucun membre du staff ne vous demandera votre mot de passe.");
        BulletWrapped("Aucun membre du staff ne vous demandera votre login.");
        BulletWrapped("Aucun membre du staff ne vous demandera votre email.");
        BulletWrapped("Seuls les admins peuvent rendre des items perdus suite à un bug serveur.");
        BulletWrapped("Le staff ne rend pas les items prêtés à un joueur disparu/banni.");
        BulletWrapped("Le staff ne donne pas d'items (hors events).");
        BulletWrapped("Les membres du staff ne sont pas des robots. Soyez courtois, cherchez avant de demander.");
        BulletWrapped("Les questions dont la réponse est sur une database = évitez.");
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Règlements dans les endroits spécifiques"))
      {
        if (ImGui::TreeNode("Salle de duel"))
        {
          BulletWrapped("Ce n'est pas un salon de thé");
          BulletWrapped("Si vous regardez, ok. Sinon, laissez la place.");
          BulletWrapped("Utilisez : @duel, @invite, @accept, @reject, @leave.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("Carnage Room"))
        {
          BulletWrapped("Loi du plus fort.");
          BulletWrapped("Amusez‑vous dans le respect.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("PVP Room"))
        {
          BulletWrapped("Free Kill interdit.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("DB Room"))
        {
          BulletWrapped("Kill Steal STRICTEMENT interdit.");
          BulletWrapped("Si la personne meurt ou se hide les mobs sont à vous.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("Guild Dungeon"))
        {
          BulletWrapped("Libre de tuer les guildiens adverses.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("WoE Castles"))
        {
          BulletWrapped("Interdiction d'apporter de l'aide via un perso non participant (multi-account/perso).");
          BulletWrapped("Les ententes entre guildes sont informelles, non officielles, non sanctionnables.");
          BulletWrapped("Elles doivent être discutées entre guildes dominantes, dans le respect.");
          ImGui::TreePop();
        }
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Logiciels tiers"))
      {
        ImGui::Text("Autorisations :");
        ImGui::Indent();
          BulletWrapped("Je vais être clair : oui, j'autorise les scripts AHK, les macros clavier/souris, les trucs qui bouclent un sort… tant que ça reste :");
          BulletWrapped("SIMPLE");
          BulletWrapped("BASIQUE");
          BulletWrapped("Pas un tableau de bord de la NASA");
          BulletWrapped("Vous bouclez le spell, éventuellement un clic en plus pour les AOE type Storm Gust, et basta.");
        ImGui::Unindent();
        ImGui::Text("Quality of Life :");
        ImGui::Indent();
          BulletWrapped("Le but, c'est du Q.O.L");
          BulletWrapped("Vous préservez votre clavier, votre souris, vos doigts, vos poignets, vos oreilles, et celles de vos voisins qui n'ont rien demandé.");
          BulletWrapped("Bref : du confort, pas du cheat.");
        ImGui::Unindent();
        ImGui::Text("Les trucs interdits (et je rigole zéro) :");
        ImGui::Indent();
          ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Ne me prenez pas pour un jambon.");
          ImGui::Text("Si vous me sortez :");
          ImGui::Indent();
            BulletWrapped("un auto-buffer");
            BulletWrapped("un auto-pot");
            BulletWrapped("un super TP/SG de physicien quantique");
            BulletWrapped("un script qui ferait rougir Tony Stark");
          ImGui::Unindent();
          ImGui::Text("Alors là :");
          ImGui::Indent();
            BulletWrapped("Je vous fais le fion.");
            BulletWrapped("Je m'en bats les couilles.");
            BulletWrapped("Je vous dégage plus vite que Thanos avec son finger snap. *Snap*");
          ImGui::Unindent();
          ImGui::Text("Les excuses bidon :");
          ImGui::Indent();
            BulletWrapped("\"Mais les autres serveurs le font...\"");
            BulletWrapped("\"Mais j'étais pas AFK, je regardais Naruto à côté...\"");
          ImGui::Unindent();
          ImGui::Text("Résultat :");
          ImGui::Indent();
            BulletWrapped("Pouf.");
            BulletWrapped("Vous étiez sur Moon.");
            BulletWrapped("Vous ne l'êtes plus.");
            BulletWrapped("Et il ne restera de vous que des ruines numériques sur Wayback Machine.");
          ImGui::Unindent();
        ImGui::Unindent();
        ImGui::TreePop();
      }
    }

    // ── Chat Box Settings ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Chat Settings"))
    {
      PushStyleCompact();
      if (ImGui::Checkbox("Chat Discord (Gonryun only)", &discord_chat_)) {
        UpdateRelay();
        SendSetting(kSettingDiscordChat, discord_chat_ ? 1 : 0);
      }

      // ── Chat Background Colours (Main / Detached / Whisper) ───────────────
      // One independent colour+opacity picker per group, persisted locally.
      auto render_chatbg = [&](ChatBgGroup& g) {
        if (g.instrs.empty()) return;
        ImGui::PushID(g.yaml_key);
        const ImVec4 swatch(g.color[0], g.color[1], g.color[2], g.color[3]);
        if (ImGui::ColorButton("##btn", swatch,
                               ImGuiColorEditFlags_AlphaPreview, ImVec2(20, 20)))
          ImGui::OpenPopup("picker");
        ImGui::SameLine();
        ImGui::TextUnformatted(g.label);

        if (ImGui::BeginPopup("picker")) {
          // ── Shared user presets ─────────────────────────────────────────
          if (!chat_bg_presets_.empty()) {
            ImGui::TextUnformatted("Presets:");
            int delete_idx = -1;
            for (int i = 0; i < static_cast<int>(chat_bg_presets_.size()); ++i) {
              const auto& p = chat_bg_presets_[i];
              const ImVec4 col(((p.argb >> 16) & 0xFF) / 255.0f,
                               ((p.argb >>  8) & 0xFF) / 255.0f,
                               ( p.argb        & 0xFF) / 255.0f,
                               ((p.argb >> 24) & 0xFF) / 255.0f);
              ImGui::PushID(i);
              if (ImGui::ColorButton("##swatch", col,
                                     ImGuiColorEditFlags_AlphaPreview |
                                     ImGuiColorEditFlags_NoTooltip,
                                     ImVec2(18, 18))) {
                PickerFromArgb(g.color, p.argb);
                ApplyChatBg(g, p.argb, true);
                SaveSettings();
              }
              if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", p.name.c_str());
              ImGui::SameLine();
              ImGui::TextUnformatted(p.name.c_str());
              ImGui::SameLine();
              if (ImGui::SmallButton("x"))
                delete_idx = i;
              ImGui::PopID();
            }
            if (delete_idx >= 0) {
              chat_bg_presets_.erase(chat_bg_presets_.begin() + delete_idx);
              SaveSettings();
            }
            ImGui::Separator();
          }
          // ── Save current colour as a preset ─────────────────────────────
          ImGui::SetNextItemWidth(120.0f);
          ImGui::InputText("##preset_name", preset_name_buf_, sizeof(preset_name_buf_));
          ImGui::SameLine();
          if (ImGui::Button("Save preset") && preset_name_buf_[0] != '\0') {
            chat_bg_presets_.push_back({preset_name_buf_, ArgbFromPicker(g.color)});
            preset_name_buf_[0] = '\0';
            SaveSettings();
          }
          ImGui::Separator();
          if (ImGui::ColorPicker4("##pick", g.color,
                                  ImGuiColorEditFlags_AlphaBar |
                                  ImGuiColorEditFlags_NoSidePreview)) {
            ApplyChatBg(g, ArgbFromPicker(g.color), false);
            g.editing = true;
          }
          if (g.editing && ImGui::IsMouseReleased(0)) {
            ApplyChatBg(g, ArgbFromPicker(g.color), true);
            SaveSettings();
            g.editing = false;
          }
          ImGui::Separator();
          if (ImGui::Button("Close", ImVec2(-1.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
        ImGui::PopID();
      };

      if (chat_bg_found_) {
        render_chatbg(chat_bg_[kChatBgMain]);
        // Quick preset switcher toggle, on the same line as the main chat picker.
        ImGui::SameLine();
        if (ImGui::Checkbox("Preset bar", &mainchat_preset_bar_))
          SaveSettings();
        render_chatbg(chat_bg_[kChatBgDetached]);
        render_chatbg(chat_bg_[kChatBgWhisper]);
      } else {
        ImGui::TextDisabled("(chat background patch unavailable)");
      }
      PopStyleCompact();
    }
    // ── DPS Meter ────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("DPS Meter")) {
      if (auto* dps = Bourgeon::Instance().dps_meter()) {
        ImGui::Checkbox("Afficher", &dps->visible_);

        ImGui::SetNextItemWidth(160.0f);
        int slot_ms = dps->slot_ms_;
        if (ImGui::SliderInt("Résolution (ms/slot)", &slot_ms, 50, 2000)) {
          dps->slot_ms_ = slot_ms;
          dps->ResetHistory();
        }
        ImGui::SameLine(); HelpMarker("Largeur de chaque colonne du graphique en millisecondes.\nValeur plus basse = graphique plus précis mais moins smooth.");

        ImGui::SetNextItemWidth(160.0f);
        int win = dps->dps_window_secs_;
        if (ImGui::SliderInt("Fenêtre DPS (s)", &win, 1, 30))
          dps->dps_window_secs_ = win;
        ImGui::SameLine(); HelpMarker("Fenêtre de temps pour calculer le DPS courant affiché.");

        ImGui::SetNextItemWidth(160.0f);
        int timeout = dps->combat_timeout_secs_;
        if (ImGui::SliderInt("Timeout combat (s)", &timeout, 1, 15))
          dps->combat_timeout_secs_ = timeout;
        ImGui::SameLine(); HelpMarker("Secondes sans dégâts avant de quitter le mode combat.");

        if (ImGui::Button("Reset graphique"))
          dps->ResetHistory();

        ImGui::Separator();
        if (ImGui::Checkbox("Afficher dommages de sorts de zone dans le chat", &dps->show_ground_dmg_in_chat_))
          SaveSettings();
        ImGui::SameLine(); HelpMarker(
            "Affiche chaque coup de Storm Gust / Meteor Storm / LoV etc. dans le chat.\n"
            "Message custom Bourgeon — le serveur ne montre pas ces dégâts dans le chat habituel.");
      }
    }

    // ── Commands Settings ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Commands Settings"))
    {
      PushStyleCompact();
      ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
      if (ImGui::BeginTabBar("CommandsSettingsTabs", tab_bar_flags))
      {
        if (ImGui::BeginTabItem("Général"))
        {
          if (ImGui::BeginTable("split", 2)) // Toggles settings
          {
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show EXP gain", &show_exp_)) SendSetting(kSettingShowExp, show_exp_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche le gain d'EXP dans le chat log. (@showexp)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show Zeny gain", &show_zeny_)) SendSetting(kSettingShowZeny, show_zeny_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche le gain de Zeny dans le chat log. (@showzeny)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show mob info", &show_mob_info_)) SendSetting(kSettingShowMobInfo, show_mob_info_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche la RACE et l'ELEMENT des monstres,\nsous leur nom. (Thx Doo - @showmobinfo)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Separate Kills", &separate_)) SendSetting(kSettingSeparate, separate_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche un séparateur dans le chat log entre chaque kill de mobs. (Demandez à Spider - @separate)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Block EXP Gain", &block_exp_)) SendSetting(kSettingBlockExp, block_exp_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Bloque le gain d'EXP. (@blockexp)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show Skill Delay", &show_delay_)) SendSetting(kSettingShowDelay, show_delay_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche un message dans le chat quand un skill\néchoue à cause du cooldown. (@showdelay)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show Speed", &show_speed_)) SendSetting(kSettingShowSpeed, show_speed_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche la valeur de vitesse de déplacement et d'attaque\ndans le chat lors d'un changement comme après\nun buff style AgiUP ou Card. (@showspeed)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Sell Stuff", &sell_stuff_)) SendSetting(kSettingSellStuff, sell_stuff_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Permet la vente d'items améliorés (refine > 0),\ncartes, munitions et items slotés chez les PNJ marchands.\nDésactiver pour protéger ces items. (@sellstuff)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Sell Item", &sell_item_)) SendSetting(kSettingSellItem, sell_item_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker(
              "Permet la vente des items du groupe IG_SELLITEM chez les PNJ marchands.\nDésactiver pour les protéger. (@sellitem)\n\n"
              "Groupe SELLITEM :\nGreen Potion (506)\nWhite Slim Potion (547)\nLucky Candy (570)\n"
              "Old Blue Box (603)\nYggdrasil Berry (607)\nYggdrasil Seed (608)\nOld Card Album (616)\n"
              "Old Violet Box (617)\nGift Box (644)\nPoison Bottle (678)\nGold (969)\n"
              "Temporal Crystal (6607)\nCoagulated Spell (6608)\nJitterbug's Tooth (6719)\n"
              "Fire Bottle (7135)\nAcid Bottle (7136)\nCoating Bottle (7139)\n"
              "Fragment of Agony (7436)\nFragment of Misery (7437)\nFragment of Hatred (7438)\nPiece of Memory Red (7439)\n"
              "Ice Scale (7562)\nCursed Water (12020)\nElemental Converter Fire (12114)\nElemental Converter Water (12115)\n"
              "Elemental Converter Earth (12116)\nElemental Converter Wind (12117)\nMystical Card Album (12246)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("No Ask", &no_ask_)) SendSetting(kSettingNoAsk, no_ask_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Refuse automatiquement les invitations\nde trade, de guilde et d'alliance. (@noask)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Wings", &wings_)) SendSetting(kSettingWings, wings_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Active ou désactive le sprite alternatif des Angel wings et Devil wings (Moonlight 2005 vibe - @wings)");
            ImGui::EndTable();
          }
          // @noks — combo 4 options (off / self / party / guild)
          {
            static const char* kNoksLabels[] = { "Off", "Self", "Party", "Guild" };
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::BeginCombo("@noks", kNoksLabels[noks_ < 4 ? noks_ : 0])) {
              for (int i = 0; i < 4; ++i) {
                const bool selected = (noks_ == i);
                if (ImGui::Selectable(kNoksLabels[i], selected)) {
                  noks_ = i;
                  SendSetting(kSettingNoks, static_cast<uint16_t>(i));
                }
                if (selected) ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }
            ImGui::SameLine(); HelpMarker("Kill Steal Protection — empêche d'autres joueurs de voler vos kills MVP.\nSelf = toi seulement, Party = ta party, Guild = ta guilde. (@noks)");
          }
          // Tri inventaires — combo 7 options (0=Par ID … 6=Aucun)
          {
            static const char* kTriLabels[] = { "Par ID", "Par type", "Par quantité", "Par poids", "Par prix", "Par nom", "Aucun" };
            auto TriCombo = [&](const char* label, int& value, uint16_t setting_id) {
              ImGui::SetNextItemWidth(130.0f);
              if (ImGui::BeginCombo(label, kTriLabels[value >= 0 && value < 7 ? value : 0])) {
                for (int i = 0; i < 7; ++i) {
                  const bool selected = (value == i);
                  if (ImGui::Selectable(kTriLabels[i], selected)) {
                    value = i;
                    SendSetting(setting_id, static_cast<uint16_t>(i));
                  }
                  if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
              }
            };
            TriCombo("Tri Inventaire", tri_inv_, kSettingTriInv);
            ImGui::SameLine(); HelpMarker("Tri automatique de l'inventaire.");
            TriCombo("Tri Chariot",    tri_cart_, kSettingTriCart);
            ImGui::SameLine(); HelpMarker("Tri automatique du chariot.");
            TriCombo("Tri Storages",     tri_storage_, kSettingTriStorage);
            ImGui::SameLine(); HelpMarker("Tri automatique des Storages personnel à la prochaine ouverture.");
            TriCombo("Tri Storage Guilde", tri_gstorage_, kSettingTriGstorage);
            ImGui::SameLine(); HelpMarker("Tri automatique du Storage de guilde à la prochaine ouverture.");
          }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Autoloots"))
        {
          ImGui::Spacing();
          {// @autoloot
            int rate = aloot_rate_;
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::SliderInt("@autoloot", &rate, 0, 100, "%d%%")) {
              aloot_rate_ = rate;
              SendSetting(kSettingAlootRate, static_cast<uint16_t>(rate));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##rate")) {
              aloot_rate_ = 0;
              SendSetting(kSettingAlootRate, 0);
            }
          }
          ImGui::Separator();
          { // @autolootpognon
            int pognon = aloot_pognon_;
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::InputInt("@autolootpognon (z)", &pognon, 100, 10000)) {
              if (pognon < 0) pognon = 0;
              if (pognon > 1000000) pognon = 1000000;
              pognon = (pognon / 100) * 100;
              aloot_pognon_ = pognon;
              SendSetting(kSettingAlootPognon, static_cast<uint16_t>(pognon / 100));
            }
          ImGui::SameLine(); HelpMarker("Autoloot des items ayant au minimum le prix de revente configuré.");
            auto apply_pognon_delta = [this](int delta) {
              int v = aloot_pognon_ + delta;
              if (v < 0) v = 0;
              if (v > 1000000) v = 1000000;
              v = (v / 100) * 100;
              aloot_pognon_ = v;
              SendSetting(kSettingAlootPognon, static_cast<uint16_t>(v / 100));
            };
            if (ImGui::Button("-10kz"))  apply_pognon_delta(-10000);
            ImGui::SameLine();
            if (ImGui::Button("-1kz"))   apply_pognon_delta(-1000);
            ImGui::SameLine();
            if (ImGui::Button("+1kz"))   apply_pognon_delta(1000);
            ImGui::SameLine();
            if (ImGui::Button("+10kz"))  apply_pognon_delta(10000);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##pognon")) {
              aloot_pognon_ = 0;
              SendSetting(kSettingAlootPognon, 0);
            }
          }
          ImGui::Separator();
          if (ImGui::TreeNode("@autoloottype")) {// @autoloottype
            ImGui::TextUnformatted("@autoloottype :");
            ImGui::SameLine(); HelpMarker("Cochez les types d'items à lootter automatiquement.\nHealing=0 Usable=2 Etc=3 Armor=4 Weapon=5\nCard=6 PetEgg=7 PetArmor=8 Ammo=10 Cash=11");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##type")) {
              aloot_type_mask_ = 0;
              SendSetting(kSettingAlootType, 0);
            }
            static const struct { const char* label; int bit; } kAlootTypes[] = {
              {"Healing",   1 << 0},  {"Usable",    1 << 2},
              {"Etc",       1 << 3},  {"Armor",     1 << 4},
              {"Weapon",    1 << 5},  {"Card",      1 << 6},
              {"Pet Egg",   1 << 7},  {"Pet Armor", 1 << 8},
              {"Ammo",      1 << 10}, {"Cash",     1 << 11},
            };
            if (ImGui::BeginTable("aloottype", 2)) {
              for (const auto& t : kAlootTypes) {
                ImGui::TableNextColumn();
                bool checked = (aloot_type_mask_ & t.bit) != 0;
                if (ImGui::Checkbox(t.label, &checked)) {
                  if (checked) aloot_type_mask_ |=  t.bit;
                  else         aloot_type_mask_ &= ~t.bit;
                  SendSetting(kSettingAlootType, static_cast<uint16_t>(aloot_type_mask_));
                }
              }
              ImGui::EndTable();
            }
            ImGui::TreePop();
          }
          ImGui::Separator();
          {// @autolootrare
          if (ImGui::Checkbox("Autoloot rares", &aloot_rare_)) SendSetting(kSettingAlootRare, aloot_rare_ ? 1 : 0);
          ImGui::SameLine(); HelpMarker(
            "Autolooting: Toutes les Cards\nOld Blue Box (603)\nYggdrasil Berry (607)\nYggdrasil Seed (608)\nOld Card Album (616)\nOld Purple Box (617)\nGift Box (644)\nGold (969)\n"
            "Temporal Crystal (6607)\nCoagulated Spell (6608)\nJitterbug's Tooth (6719)\nFragment of Agony (7436)\nFragment of Misery (7437)\nFragment of Hatred (7438)\n"
            "Piece_Of_Memory_Red (7439)\nTreasure Box (7444)\nCursed Water (12020)\nElemental Converter Fire (12114)\nElemental Converter Water (12115)\n"
            "Elemental Converter Earth (12116)\nElemental Converter Wind (12117)\nMystical Card Album (12246)\nSentimental Fragment (22687)\nCursed Fragment (23016)");
          }
          ImGui::Separator();
          {// @autolootmvp / @autolootmvpreward
          if (ImGui::Checkbox("Autoloot MVP cards", &aloot_mvp_)) SendSetting(kSettingAlootMvp, aloot_mvp_ ? 1 : 0);
          ImGui::SameLine(); HelpMarker("Loot automatiquement les cartes MVP\nquelque soit leur taux de drop. (@autolootmvp)");
          if (ImGui::Checkbox("Autoloot MVP rewards (actif par défaut)", &aloot_mvp_rwd_)) SendSetting(kSettingAlootMvpRwd, aloot_mvp_rwd_ ? 1 : 0);
          ImGui::SameLine(); HelpMarker("Les drops de récompense des MVP sont lootés\nautomatiquement par défaut.\nDécocher pour désactiver. (@autolootmvpreward)");
          }
          ImGui::Separator();
          if (ImGui::TreeNode("@autolootid")) {// @autolootid
            ImGui::TextUnformatted("@autolootid :");
            ImGui::SameLine(); HelpMarker("Loot automatiquement les items par ID.\nMax 50 IDs. (@autolootid <id>)");
            ImGui::SameLine();
            if (ImGui::Checkbox("Overlay", &show_alootid_overlay_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker("Affiche un bouton Add/Remove Alootid\nprès du curseur au clic droit sur un item.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##alootid")) {
              aloot_ids_.clear();
              SendSetting(kSettingAlootId, 0);
            }
            ImGui::SetNextItemWidth(100.0f);
            ImGui::InputInt("##alootid_input", &aloot_id_input_, 0, 0);
            if (aloot_id_input_ < 0) aloot_id_input_ = 0;
            ImGui::SameLine();
            const bool can_add = (aloot_id_input_ > 0 && aloot_ids_.size() < 50);
            if (!can_add) ImGui::BeginDisabled();
            if (ImGui::Button("Add##alootid")) {
              const uint32_t id = static_cast<uint32_t>(aloot_id_input_);
              bool found = false;
              for (uint32_t x : aloot_ids_) if (x == id) { found = true; break; }
              if (!found) {
                aloot_ids_.push_back(id);
                SendSetting(kSettingAlootId, id);
              }
            }
            if (!can_add) ImGui::EndDisabled();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
            {
              // Detect unsaved changes: compare current list vs snapshot (order-independent).
              auto sorted_copy = [](const std::vector<uint32_t>& v) {
                auto s = v; std::sort(s.begin(), s.end()); return s;
              };
              const bool dirty = (alootid_active_preset_ != 0) &&
                                 (sorted_copy(aloot_ids_) != sorted_copy(alootid_saved_ids_));

              static char hdr[96];
              if (alootid_active_preset_ != 0) {
                const char* preset_name = nullptr;
                char no_buf[8];
                for (const auto& p : alootid_presets_) {
                  if (p.no == alootid_active_preset_) {
                    preset_name = p.name.empty()
                      ? (std::snprintf(no_buf, sizeof(no_buf), "#%u", p.no), no_buf)
                      : p.name.c_str();
                    break;
                  }
                }
                if (dirty)
                  std::snprintf(hdr, sizeof(hdr), "%s (non sauvegardé)",
                                preset_name ? preset_name : "?");
                else
                  std::snprintf(hdr, sizeof(hdr), "%s",
                                preset_name ? preset_name : "?");
              } else {
                std::strncpy(hdr, "Liste courante", sizeof(hdr));
              }
              ImGui::TextUnformatted(hdr);
            }
            ImGui::BeginChild("##alootid_list", ImVec2(0, 160), true);
            if (ImGui::BeginTable("##alootid_tbl", 2, ImGuiTableFlags_SizingStretchSame)) {
              for (int i = 0; i < static_cast<int>(aloot_ids_.size()); ++i) {
                ImGui::TableNextColumn();
                const uint32_t id = aloot_ids_[i];
                char lbl[32];
                std::snprintf(lbl, sizeof(lbl), "x##alootid_%d", i);
                if (ImGui::SmallButton(lbl)) {
                  SendSetting(kSettingAlootIdRemove, aloot_ids_[i]);
                  aloot_ids_.erase(aloot_ids_.begin() + i);
                  --i;
                  continue;
                }
                ImGui::SameLine();
                const auto it = item_names_.find(id);
                if (it != item_names_.end())
                  ImGui::Text("%s [%u]", it->second.c_str(), id);
                else
                  ImGui::Text("[%u]", id);
              }
              ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            // Quick-add/remove from the last right-clicked item description window.
            if (g_last_viewed_item != 0) {
              ImGui::Separator();
              const auto itv = item_names_.find(g_last_viewed_item);
              if (itv != item_names_.end())
                ImGui::Text("Vu: [%u] %s", g_last_viewed_item, itv->second.c_str());
              else
                ImGui::Text("Vu: [%u]", g_last_viewed_item);
              ImGui::SameLine();
              int vu_idx = -1;
              for (int k = 0; k < static_cast<int>(aloot_ids_.size()); ++k)
                if (aloot_ids_[k] == g_last_viewed_item) { vu_idx = k; break; }
              if (vu_idx >= 0) {
                if (ImGui::SmallButton("Remove##alootid_vu")) {
                  SendSetting(kSettingAlootIdRemove, aloot_ids_[vu_idx]);
                  aloot_ids_.erase(aloot_ids_.begin() + vu_idx);
                }
              } else {
                const bool can_add_vu = (aloot_ids_.size() < 50);
                if (!can_add_vu) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Add##alootid_vu")) {
                  aloot_ids_.push_back(g_last_viewed_item);
                  SendSetting(kSettingAlootId, g_last_viewed_item);
                }
                if (!can_add_vu) ImGui::EndDisabled();
              }
            }
            // ── Presets (server-backed, DB table `alootid`) ──
            ImGui::Separator();
            ImGui::TextUnformatted("Presets :");
            // Autoload indicator + toggle, on the same line as the label.
            {
              const AlootPreset* autoload_preset = nullptr;
              for (const auto& p : alootid_presets_)
                if (p.autoload) { autoload_preset = &p; break; }
              ImGui::SameLine();
              // Find the selected preset to know what toggling autoload does.
              const AlootPreset* sel_for_al = nullptr;
              for (const auto& p : alootid_presets_)
                if (p.no == alootid_selected_preset_) { sel_for_al = &p; break; }
              bool al = sel_for_al && sel_for_al->autoload;
              if (!sel_for_al) ImGui::BeginDisabled();
              if (ImGui::Checkbox("Autoload##preset", &al))
                SendPresetCmd(5, al ? alootid_selected_preset_ : 0);
              if (!sel_for_al) ImGui::EndDisabled();
              ImGui::SameLine();
              if (autoload_preset) {
                if (autoload_preset->name.empty())
                  ImGui::TextDisabled("(#%u)", autoload_preset->no);
                else
                  ImGui::TextDisabled("(%s)", autoload_preset->name.c_str());
              } else {
                ImGui::TextDisabled("(aucun)");
              }
            }
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputText("##preset_name", alootid_preset_input_,
                             sizeof(alootid_preset_input_));
            ImGui::SameLine();
            ImGui::Checkbox("Renommer##preset_toggle", &alootid_rename_open_);
            if (alootid_rename_open_) {
              ImGui::SameLine();
              ImGui::SetNextItemWidth(120.0f);
              ImGui::InputText("##preset_rename", alootid_rename_input_,
                               sizeof(alootid_rename_input_));
            }
            ImGui::SameLine();
            {
              const AlootPreset* sel_for_rename = nullptr;
              for (const auto& p : alootid_presets_)
                if (p.no == alootid_selected_preset_) { sel_for_rename = &p; break; }

              // Find existing preset matching the typed name (for delete-on-empty).
              const AlootPreset* named_preset = nullptr;
              for (const auto& p : alootid_presets_)
                if (p.name == alootid_preset_input_) { named_preset = &p; break; }

              const bool is_rename = alootid_rename_open_;
              const bool list_empty = aloot_ids_.empty();
              // Rename: need a new name + a selected preset.
              // Save: need a non-empty name; list empty → delete the named preset.
              const bool can_act = is_rename
                ? (alootid_rename_input_[0] != '\0' && sel_for_rename != nullptr)
                : (alootid_preset_input_[0] != '\0' &&
                   (!list_empty || named_preset != nullptr));

              if (!can_act) ImGui::BeginDisabled();
              const char* btn_label = (!is_rename && list_empty && named_preset)
                ? "Supprimer##preset_save" : "Sauvegarder##preset";
              if (ImGui::SmallButton(btn_label)) {
                if (is_rename && sel_for_rename) {
                  SendPresetCmd(6, alootid_selected_preset_, alootid_rename_input_);
                  alootid_rename_open_ = false;
                } else if (!is_rename && list_empty && named_preset) {
                  SendPresetCmd(4, named_preset->no);
                } else {
                  uint8_t save_no = 0;
                  bool used[11] = {};
                  for (const auto& p : alootid_presets_) {
                    if (p.name == alootid_preset_input_) { save_no = p.no; break; }
                    if (p.no <= 10) used[p.no] = true;
                  }
                  if (save_no == 0)
                    for (uint8_t n = 1; n <= 10; ++n) if (!used[n]) { save_no = n; break; }
                  if (save_no > 0)
                    SendPresetCmd(2, save_no, alootid_preset_input_);
                }
              }
              if (!can_act) ImGui::EndDisabled();
            }

            // Combo: select preset by name
            {
              const AlootPreset* sel_preset = nullptr;
              for (const auto& p : alootid_presets_)
                if (p.no == alootid_selected_preset_) { sel_preset = &p; break; }

              auto preset_label = [](const AlootPreset& p, char* buf, size_t sz, bool mark_active) {
                if (p.name.empty())
                  std::snprintf(buf, sz, "#%u%s", p.no, mark_active ? " *" : "");
                else
                  std::snprintf(buf, sz, "%s%s", p.name.c_str(), mark_active ? " *" : "");
              };
              char preview_buf[66];
              const char* preview;
              if (sel_preset) {
                preset_label(*sel_preset, preview_buf, sizeof(preview_buf), false);
                preview = preview_buf;
              } else {
                preview = "-- choisir --";
              }
              ImGui::SetNextItemWidth(120.0f);
              if (ImGui::BeginCombo("##preset_select", preview)) {
                for (const auto& p : alootid_presets_) {
                  const bool sel = (p.no == alootid_selected_preset_);
                  char label[66];
                  preset_label(p, label, sizeof(label), p.no == alootid_active_preset_);
                  if (ImGui::Selectable(label, sel))
                    alootid_selected_preset_ = p.no;
                  if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
              }
              ImGui::SameLine();
              const bool has_sel = sel_preset != nullptr;
              if (!has_sel) ImGui::BeginDisabled();
              if (ImGui::SmallButton("Charger##preset"))
                SendPresetCmd(3, alootid_selected_preset_);
              ImGui::SameLine();
              if (ImGui::SmallButton("Supprimer##preset"))
                SendPresetCmd(4, alootid_selected_preset_);
              if (!has_sel) ImGui::EndDisabled();

            }
            ImGui::TreePop();
          }
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
      PopStyleCompact();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(4);

  // ── Alootid floating overlay ──────────────────────────────────────────────
  // Detect silent tooltip close (e.g. comparison→non-comparison): the game
  // zeroes kItemDescWndGlobalPtr without sending our hook a close message.
  if (g_item_desc_visible &&
      *reinterpret_cast<const uintptr_t*>(kItemDescWndGlobalPtr) == 0) {
    g_item_desc_visible = false;
    g_item_desc_wnd_ptr = nullptr;
  }

  if (show_alootid_overlay_ && g_last_viewed_item != 0 && g_item_desc_visible) {
    // Try to read the tooltip window position from the stored object pointer.
    // Offsets found via CheatEngine: [ptr+0x18]=Y, [ptr+0x20]=X.
    // If the pointer doesn't match the right object the values will be garbage
    // and we fall back to the cursor position captured at open time.
    float overlay_x = static_cast<float>(g_item_desc_cursor.x) + 12.0f;
    float overlay_y = static_cast<float>(g_item_desc_cursor.y) + 12.0f;
    if (g_item_desc_wnd_ptr != nullptr) {
      const auto* base = static_cast<const uint8_t*>(g_item_desc_wnd_ptr);
      const int wx = *reinterpret_cast<const int*>(base + 0x1C);  // X (confirmed)
      const int wy = *reinterpret_cast<const int*>(base + 0x20);  // Y (confirmed)
      if (wx > 0 && wx < 4096 && wy > 0 && wy < 4096) {
        overlay_x = static_cast<float>(wx);
        overlay_y = static_cast<float>(wy) - 24.0f;
      }
    }
    ImGui::SetNextWindowPos(ImVec2(overlay_x, overlay_y), ImGuiCond_Always);  // live-track
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    constexpr ImGuiWindowFlags kOverlayFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##alootid_overlay", nullptr, kOverlayFlags)) {
      const auto itv = item_names_.find(g_last_viewed_item);
      if (itv != item_names_.end())
        ImGui::TextUnformatted(itv->second.c_str());
      else
        ImGui::Text("[%u]", g_last_viewed_item);

      int ov_idx = -1;
      for (int k = 0; k < static_cast<int>(aloot_ids_.size()); ++k)
        if (aloot_ids_[k] == g_last_viewed_item) { ov_idx = k; break; }

      ImGui::SameLine();
      if (ov_idx >= 0) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.18f, 0.18f, 1.0f));
        if (ImGui::SmallButton("- alootid")) {
          SendSetting(kSettingAlootIdRemove, aloot_ids_[ov_idx]);
          aloot_ids_.erase(aloot_ids_.begin() + ov_idx);
        }
        ImGui::PopStyleColor();
      } else {
        const bool can_add = (aloot_ids_.size() < 50);
        if (!can_add) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.18f, 1.0f));
        if (ImGui::SmallButton("+ alootid")) {
          aloot_ids_.push_back(g_last_viewed_item);
          SendSetting(kSettingAlootId, g_last_viewed_item);
        }
        ImGui::PopStyleColor();
        if (!can_add) ImGui::EndDisabled();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("x"))
        g_item_desc_visible = false;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

  // ── Main-chat quick preset switcher (compact, draggable, resizable) ───────
  if (mainchat_preset_bar_ && chat_bg_found_) {
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize(ImVec2(80.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(40.0f, 1.0f), ImVec2(8000.0f, 8000.0f));
    // Match the main Moonlight-Destiny window's frame/grab/window rounding.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    // Lower the per-window minimum size (default 32x32) so this bar can be made
    // as thin as a single preset row.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(40.0f, 1.0f));
    // No title bar (minimalist). Still draggable from the body and resizable.
    if (ImGui::Begin("Chat presets", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoNav)) {
      PushStyleCompact();
      ChatBgGroup& g = chat_bg_[kChatBgMain];
      if (chat_bg_presets_.empty()) {
        ImGui::TextDisabled("No presets yet.");
        ImGui::TextDisabled("Add some in the");
        ImGui::TextDisabled("Main chat picker.");
      } else {
        for (int i = 0; i < static_cast<int>(chat_bg_presets_.size()); ++i) {
          const auto& p = chat_bg_presets_[i];
          const ImVec4 col(((p.argb >> 16) & 0xFF) / 255.0f,
                           ((p.argb >>  8) & 0xFF) / 255.0f,
                           ( p.argb        & 0xFF) / 255.0f,
                           ((p.argb >> 24) & 0xFF) / 255.0f);
          ImGui::PushID(i);
          if (ImGui::ColorButton("##sw", col,
                                 ImGuiColorEditFlags_AlphaPreview |
                                 ImGuiColorEditFlags_NoTooltip,
                                 ImVec2(14, 14))) {
            PickerFromArgb(g.color, p.argb);
            ApplyChatBg(g, p.argb, true);
            SaveSettings();
          }
          ImGui::SameLine();
          ImGui::TextUnformatted(p.name.c_str());
          ImGui::PopID();
          ImGui::SameLine();
        }
      }
      PopStyleCompact();
    }
    ImGui::End();
    ImGui::PopStyleVar(6);
    // Closing is done by un-ticking the "Preset bar" checkbox (no title-bar [x]).
  }
}
