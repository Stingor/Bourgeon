#include "ragnarok/render.h"
#include "ragnarok/ragnarok_client.h"

#include <Windows.h>

#include <array>
#include <iomanip>
#include <sstream>

#include "backends/imgui_impl_win32.h"
#include "bourgeon.h"
#include "imgui/imgui_impl_dx7.h"
#include "imgui_internal.h"
#include "features/overlays/skill_bar.h"
#include "features/windows/storage_window.h"
#include "features/windows/inventory_viewer.h"
#include "features/systems/moonlight_auth.h"        // WantsKeyboard (écrans de login)
#include "features/windows/char_select.h"           // NativeScreenHasKeyboard
#include "features/windows/make_item_window.h"      // WantsEnterKey (avale VK_RETURN)
#include "features/windows/npc_dialog_window.h"     // EatsKey (touches du dialogue NPC)
#include "features/windows/item_desc_window.h"      // EatsBookKey (flèches du livre)
#include "features/windows/weapon_refine_window.h"  // WantsEnterKey (avale VK_RETURN)
#include "features/windows/chat_window.h"           // WantsEscapeKey (avale VK_ESCAPE)
#include "features/minigames/doom.h"
#include "ragnarok/configuration.h"
#include "ragnarok/object_factory.h"
#include "ragnarok/packets.h"
#include "ui/ro_imgui.h"
#include "utils/byte_pattern.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

using CreateWindowExAFunc = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int,
                                          int, int, int, HWND, HMENU, HINSTANCE,
                                          LPVOID);
using WindowProcFunc = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                      WPARAM wParam,
                                                      LPARAM lParam);
static HWND WINAPI CreateWindowExAHook(DWORD, LPCSTR, LPCSTR, DWORD, int, int,
                                       int, int, HWND, HMENU, HINSTANCE,
                                       LPVOID);
static LRESULT CALLBACK WindowProcHook(HWND hwnd, UINT uMsg, WPARAM wParam,
                                       LPARAM lParam);

static CreateWindowExAFunc CreateWindowExARef;
static WindowProcFunc WndProcRef;

// Handle of the first (game) window created via CreateWindowExA. Exposed to
// plugins through RagnarokClient::GameWindow() (e.g. for synthesizing input).
static HWND g_game_hwnd = nullptr;

// Marque des frappes que NOUS postons au client natif (RagnarokClient::
// PostGameKey). Le `repeat count` (bits 0-15) y est NUL, ce qu'un WM_KEYDOWN du
// pilote clavier ne produit jamais (il vaut toujours au moins 1) : aucune frappe
// réelle ne peut porter cette valeur, la reconnaissance est donc sans ambiguïté.
constexpr LPARAM kSyntheticKeyLParam = 0x00420000;

static bool IsMouseOverAnyImGuiWindow(float mx, float my);
// Same, but ALSO counts click-through (locked) windows — used to decide whether
// to draw the RO cursor on top of them (they still render above the game cursor).
static bool IsMouseOverAnyVisibleImGuiWindow(float mx, float my);

// ── Real RO cursor capture (DX9) ─────────────────────────────────────────────
// The game draws its cursor as a SOFTWARE sprite batched into the scene render
// queue, BEFORE our Present-hook ImGui — so it's always hidden under ImGui
// windows (see project_ro_cursor). Instead of re-drawing the game's batched
// cursor (impossible after the flush), we CAPTURE the cursor's atlas texture +
// UV each frame and re-draw it ourselves via ImGui's foreground list, but only
// where the mouse is over an ImGui window (so there's no double cursor).
//
// Capture mechanism: CursorMgr_RenderSprite (0x00a74410) renders the cursor; it
// resolves the current frame's GPU texture through SpriteAtlas_GetCachedTexture
// (0x00566b70). We hook the former to scope a re-entrancy flag, and the latter
// to grab, while that flag is set, the returned CTexture's IDirect3DTexture9*
// (CTexture+0x12c, confirmed live) + the atlas UV rect (geom[3..6]) + the sprite
// pixel size. Everything runs on the single render thread, so no locking.
extern bool g_imgui_dx7_active;

namespace {
constexpr uintptr_t kCursorRenderFn = 0x00a74410;  // CursorMgr_RenderSprite
// CTexture -> native GPU handle. The concrete CTexture class is renderer-
// specific (different vtable + field offset): DX9 stores an IDirect3DTexture9*
// at +0x12c, DX7 an IDirectDrawSurface7* at +0x128 (both confirmed live). ImGui's
// ImTextureID is opaque — the active backend (imgui_impl_dx9 / imgui_impl_dx7)
// interprets it, so the same AddImage works for both.
constexpr int kCTexNativeOffDX9 = 0x12c;
constexpr int kCTexNativeOffDX7 = 0x128;

struct CursorCapture {
  void*  tex = nullptr;             // IDirect3DTexture9* (atlas page)
  ImVec2 uv0{0.f, 0.f}, uv1{1.f, 1.f};
  int    w = 0, h = 0;              // sprite frame pixel size
  DWORD  tick = 0;                  // GetTickCount() of last capture (freshness)
};
CursorCapture g_cursor_cap;
bool g_capturing_cursor   = false;  // set only inside CursorMgr_RenderSprite
bool g_captured_this_pass = false;  // grab just the first (main) layer

using CursorRenderFn = void(__fastcall*)(void* thisptr);
using AtlasGetFn = void*(__fastcall*)(void* thisptr, void* edx, int spr_frame,
                                      int palette, int* geom);
CursorRenderFn g_orig_cursor_render = nullptr;
AtlasGetFn     g_orig_atlas_get     = nullptr;

// Hook on SpriteAtlas_GetCachedTexture: while the cursor is rendering, grab the
// first non-null atlas texture it resolves.
void* __fastcall Hooked_AtlasGet(void* thisptr, void* edx, int spr_frame,
                                 int palette, int* geom) {
  void* ctex = g_orig_atlas_get(thisptr, edx, spr_frame, palette, geom);
  if (g_capturing_cursor && !g_captured_this_pass && ctex && geom && spr_frame) {
    const int off = g_imgui_dx7_active ? kCTexNativeOffDX7 : kCTexNativeOffDX9;
    void* d3dtex = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(ctex) + off);
    if (d3dtex) {
      const short* sf = reinterpret_cast<const short*>(
          static_cast<uintptr_t>(static_cast<unsigned>(spr_frame)));
      g_cursor_cap.tex = d3dtex;
      g_cursor_cap.uv0 = ImVec2(*reinterpret_cast<float*>(&geom[3]),
                                *reinterpret_cast<float*>(&geom[4]));
      g_cursor_cap.uv1 = ImVec2(*reinterpret_cast<float*>(&geom[5]),
                                *reinterpret_cast<float*>(&geom[6]));
      g_cursor_cap.w   = sf[0];
      g_cursor_cap.h   = sf[1];
      g_cursor_cap.tick = GetTickCount();
      g_captured_this_pass = true;
    }
  }
  return ctex;
}

// Hook on CursorMgr_RenderSprite: scope the capture flag to the cursor render,
// and while the mouse is over an ImGui window force the cursor type (this+0x50)
// to 0 (arrow). Without this, the game's hover state machine keeps switching the
// type (NPC / attack / no-walk) based on the game entity or cell sitting *under*
// the ImGui window, so the captured cursor flickered between arrow and those.
// The write is transient — the game re-derives the type every frame via its own
// hover update, so there's no lasting state change.
void __fastcall Hooked_CursorRender(void* thisptr) {
  g_capturing_cursor = true;
  g_captured_this_pass = false;
  if (thisptr && ImGui::GetCurrentContext()) {
    const ImVec2 mp = ImGui::GetIO().MousePos;
    // Consommé chaque frame (remis à 0) : type de curseur RO demandé par un widget
    // du toolkit au survol (main sur scrollbar/resize/checkbox/bouton), 0 = flèche.
    const int req = ro::TakeHoverCursor();
    if (IsMouseOverAnyImGuiWindow(mp.x, mp.y))
      *reinterpret_cast<int*>(reinterpret_cast<char*>(thisptr) + 0x50) = req;
  }
  // Curseur plein écran (login Moonlight / char-select) : on laisse le rendu natif
  // s'exécuter (il alimente la capture d'atlas via Hooked_AtlasGet — rien à
  // répliquer), mais on POUSSE son quad hors écran. Le sprite natif est calculé à
  // base = g_MouseScreen{X,Y} + *(float*)(mode+0x30/+0x34) (RE 0x00a74554) ; on
  // force ces deux offsets à ~ -(souris) - 4096 -> le quad tombe hors viewport
  // (sommets XYZRHW, clippés), puis on RESTAURE avant de rendre la main : invisible
  // pour tout le reste du client (anim, hit-test natif, notif UIWindowMgr intacts).
  bool fs_suppress = false;
  float saved_ox = 0.0f, saved_oy = 0.0f;
  if (thisptr && ro::FullscreenCursorActive()) {
    __try {
      auto* ox = reinterpret_cast<float*>(reinterpret_cast<char*>(thisptr) + 0x30);
      auto* oy = reinterpret_cast<float*>(reinterpret_cast<char*>(thisptr) + 0x34);
      saved_ox = *ox;
      saved_oy = *oy;
      const float mx = static_cast<float>(*reinterpret_cast<int*>(0x011e40d4));
      const float my = static_cast<float>(*reinterpret_cast<int*>(0x011e40d8));
      *ox = -mx - 4096.0f;
      *oy = -my - 4096.0f;
      fs_suppress = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      fs_suppress = false;
    }
  }
  g_orig_cursor_render(thisptr);
  if (fs_suppress) {  // restauré AVANT de rendre la main (aucun return entre-temps)
    *reinterpret_cast<float*>(reinterpret_cast<char*>(thisptr) + 0x30) = saved_ox;
    *reinterpret_cast<float*>(reinterpret_cast<char*>(thisptr) + 0x34) = saved_oy;
  }
  g_capturing_cursor = false;
}

// Installed lazily from the render thread (the hooked functions run on the same
// thread, so they can't be mid-execution while we patch them). Works in both
// renderers — the native-handle offset is chosen per renderer in the capture.
void InstallCursorCapture() {
  static bool done = false;
  if (done) return;
  done = true;
  using namespace hooking;
  g_orig_cursor_render = reinterpret_cast<CursorRenderFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kCursorRenderFn),
          reinterpret_cast<uint8_t*>(&Hooked_CursorRender)));
  g_orig_atlas_get = reinterpret_cast<AtlasGetFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(render::kAtlasGetCachedAddr),
          reinterpret_cast<uint8_t*>(&Hooked_AtlasGet)));
  // LogInfo("[Cursor] capture hooks installed (render_orig={:x} atlas_orig={:x})",
          // reinterpret_cast<uintptr_t>(g_orig_cursor_render),
          // reinterpret_cast<uintptr_t>(g_orig_atlas_get));
}

// ── Char-select paging crash fix (native UINewSelectCharWnd off-by-one) ───────
// Native bug (predates Bourgeon): UINewSelectCharWnd's selected-slot render
// (0x0079d590) reads slots[idx] from the per-page slot array at this+0xe8, where
// idx = this+0x120, guarded ONLY by a non-null check — there is NO
// `idx < slots_per_page (this+0x128)` bounds check. While paging through the
// char list, idx is left at slots_per_page (15) as a "no current selection"
// sentinel, so the render reads slots[15] — one element past the 15-entry array,
// i.e. the heap guard bytes 0xABABABAB — and dereferences it as a vtable. Crash
// (very often around the 3rd page). Confirmed live with x32dbg; see
// project_charselect_paging_crash.
//
// Fix: re-insert the missing bounds check at the read site. The native code
// already has a clean "no selection" path (je 0x0079d60d -> pop esi; ret, which
// also skips the child tree-walk). We detour the `mov ecx,[esi+0x120]` at
// 0x0079d5e0 (the je at 0x0079d5d3 targets exactly 0x0079d5e0, so the 5-byte jmp
// patch lands on no other instruction) and, when idx >= slots_per_page, branch
// to that same skip path; otherwise resume the original render at 0x0079d5e6
// (right after the stolen mov, with ecx = idx already loaded). esi (this) is
// callee-saved across the function's intervening calls, and eax is immediately
// overwritten on both paths, so this is register-safe. 20250716-specific.
constexpr uintptr_t kSelCharRenderPatch = 0x0079d5e0;  // mov ecx,[esi+0x120]

__declspec(naked) void SelCharPagingFixStub() {
  __asm {
    mov  ecx, [esi+120h]   ; selected slot index (this+0x120)
    cmp  ecx, [esi+128h]   ; vs slots-per-page (this+0x128)
    jae  no_selection      ; idx >= per_page -> out of range, skip safely
    mov  eax, 79d5e6h      ; in range: resume original render (ecx already set)
    jmp  eax
  no_selection:
    mov  eax, 79d60dh      ; native "no selection" path: pop esi; ret
    jmp  eax
  }
}

void InstallCharSelectPagingFix() {
  static bool done = false;
  if (done) return;
  done = true;
  using namespace hooking;
  HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kSelCharRenderPatch),
      reinterpret_cast<uint8_t*>(&SelCharPagingFixStub));
  // LogInfo("[CharSelect] paging-crash bounds-check installed @ {:x}",
          // kSelCharRenderPatch);
}
}  // namespace

// ── RO cursor overlay via ImGui foreground draw list ─────────────────────────
// Called between ImGui::NewFrame() and ImGui::EndFrame() so the draw commands
// are part of the current frame's render data, guaranteed above all windows.
void DrawROCursorImGui() {
  if (!ImGui::GetCurrentContext()) return;
  InstallCursorCapture();  // lazy, once (no-op in DX7)

  const ImVec2 mp = ImGui::GetIO().MousePos;
  if (mp.x < 0.f || mp.y < 0.f) return;
  // Draw over locked (click-through) bars/portrait too, not just interactive
  // windows — otherwise the game's batched cursor stays hidden behind them.
  // Mode « plein écran » (login Moonlight / char-select) : le curseur natif est
  // rendu hors écran (cf. Hooked_CursorRender), donc on redessine le nôtre PARTOUT
  // -> pas de double curseur, et un seul curseur sur tout l'écran.
  if (!ro::FullscreenCursorActive() &&
      !IsMouseOverAnyVisibleImGuiWindow(mp.x, mp.y))
    return;

  ImDrawList* dl = ImGui::GetForegroundDrawList();

  // Real captured RO cursor (DX9): the atlas sub-rect the game resolved for the
  // current cursor frame. Freshness-gated so a stale texture isn't drawn after
  // the game stops rendering a cursor.
  const CursorCapture cap = g_cursor_cap;
  if (cap.tex && cap.w > 0 && cap.h > 0 &&
      (GetTickCount() - cap.tick) < 500) {
    dl->AddImage((ImTextureID)(uintptr_t)cap.tex,
                 mp, ImVec2(mp.x + cap.w, mp.y + cap.h), cap.uv0, cap.uv1);
    return;
  }

  // Fallback: placeholder triangle (cursor not captured yet).
  const float mx = mp.x, my = mp.y;
  ImVec2 pts[3] = {{mx, my}, {mx + 10.f, my + 10.f}, {mx, my + 13.f}};
  dl->AddTriangleFilled(pts[0], pts[1], pts[2], IM_COL32(255, 255, 255, 230));
  dl->AddTriangle(pts[0], pts[1], pts[2], IM_COL32(0, 0, 0, 200), 1.2f);
}

RagnarokClient::RagnarokClient()
    : timestamp_(),
      session_(),
      rag_connection_(),
      window_mgr_(),
      login_mode_(),
      game_mode_() {}

RagnarokClient::~RagnarokClient() { ImGui_ImplWin32_Shutdown(); }

bool RagnarokClient::Initialize() {
  // Hook CreateWindowExA unconditionally so ImGui gets initialized when the
  // game window is created, even for unsupported clients.
  SetupImgui();

  timestamp_ = GetClientTimeStamp();
  if (timestamp_ == kUnknownTimeStamp) {
    LogError("Failed to determine client date");
    return false;
  }
  const auto timestamp_as_str = std::to_string(timestamp_);
  // LogInfo("Detected client: {}", timestamp_as_str);

  const YAML::Node configuration = LoadConfiguration();
  const auto client_configuration = configuration[timestamp_as_str];
  if (!client_configuration.IsDefined()) {
    LogError("This client isn't supported");
    return false;
  }

  // Native client patch: fix the char-select paging crash (UINewSelectCharWnd
  // off-by-one OOB on the selected slot). Addresses are 20250716-specific.
  if (timestamp_as_str == "20250716") InstallCharSelectPagingFix();

  ObjectFactory factory;
  session_ = factory.CreateSession(client_configuration["CSession"]);
  if (!session_) {
    return false;
  }

  rag_connection_ =
      factory.CreateRagConnection(client_configuration["CRagConnection"]);
  if (!rag_connection_) {
    return false;
  }

  window_mgr_ = factory.CreateUIWindowMgr(client_configuration["UIWindowMgr"]);
  if (!window_mgr_) {
    return false;
  }

  mode_mgr_ = factory.CreateModeMgr(client_configuration["CModeMgr"]);
  if (!mode_mgr_) {
    return false;
  }

  login_mode_ = factory.CreateLoginMode(client_configuration["CLoginMode"]);
  if (!login_mode_) {
    return false;
  }

  game_mode_ = factory.CreateGameMode(client_configuration["CGameMode"]);
  if (!game_mode_) {
    return false;
  }

  return true;
}

YAML::Node RagnarokClient::LoadConfiguration() {
  return YAML::Load(kYamlConfiguration);
}

void* RagnarokClient::GameWindow() { return g_game_hwnd; }

void RagnarokClient::PostGameKey(int vkey) {
  if (!g_game_hwnd) return;
  PostMessageW(g_game_hwnd, WM_KEYDOWN, static_cast<WPARAM>(vkey),
               kSyntheticKeyLParam);
  PostMessageW(g_game_hwnd, WM_KEYUP, static_cast<WPARAM>(vkey),
               kSyntheticKeyLParam);
}

uint32_t RagnarokClient::timestamp() const { return timestamp_; }

Session& RagnarokClient::session() const { return *session_; }

RagConnection& RagnarokClient::rag_connection() const {
  return *rag_connection_;
}

UIWindowMgr& RagnarokClient::window_mgr() const { return *window_mgr_; }

bool RagnarokClient::UseItemById(int item_id) const {
  // 🔴 NE PAS APPELER sur le client 20250716 : GetItemInfoById parcourt
  // `item_list()`, dont l'offset est FAUX (cf. l'en-tête de
  // object_layouts/session/20250716.h) — tête de liste nulle, crash immédiat.
  // Cette fonction n'a jamais eu d'appelant, c'est pourquoi le défaut a survécu.
  PACKET_CZ_USE_ITEM packet;
  ItemInfo iinfo;

  if (!session_->GetItemInfoById(item_id, iinfo)) {
    return false;
  }

  packet.header = static_cast<int16_t>(PacketHeader::CZ_USE_ITEM);
  packet.index = static_cast<uint16_t>(iinfo.item_index_);
  packet.aid = session_->aid();

  return rag_connection_->SendPacket(sizeof(packet),
                                     reinterpret_cast<char*>(&packet));
}

uint32_t RagnarokClient::GetClientTimeStamp() {
  const auto* const p_client_base =
      static_cast<const uint8_t*>(GetClientBase());
  if (p_client_base == nullptr) {
    return RagnarokClient::kUnknownTimeStamp;
  }

  const auto* p_dos_header =
      reinterpret_cast<const IMAGE_DOS_HEADER*>(p_client_base);
  const auto* p_nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      p_client_base + p_dos_header->e_lfanew);

  // Check PE timestamp
  if (p_nt_headers->FileHeader.TimeDateStamp != 0) {
    return ConvertClientTimestamp(p_nt_headers->FileHeader.TimeDateStamp);
  }

  const IMAGE_DATA_DIRECTORY& dir =
      p_nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  if (dir.Size == 0 || dir.VirtualAddress == 0) {
    return kUnknownTimeStamp;
  }

  // Check the debug data directory timestamp
  const auto* p_dbg_dir = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
      p_client_base + dir.VirtualAddress);
  if (p_dbg_dir->TimeDateStamp != 0) {
    return ConvertClientTimestamp(p_dbg_dir->TimeDateStamp);
  }

  return kUnknownTimeStamp;
}

void* RagnarokClient::GetClientBase() {
  return static_cast<void*>(GetModuleHandleW(nullptr));
}

uint32_t RagnarokClient::ConvertClientTimestamp(uint32_t timestamp) {
  const std::time_t temp = timestamp;
  std::tm time{};
  gmtime_s(&time, &temp);
  return (time.tm_year + 1900) * 10000 + (time.tm_mon + 1) * 100 + time.tm_mday;
}

bool RagnarokClient::SetupImgui() {
  using namespace hooking;

  const HMODULE h_user32 = GetModuleHandleA("user32.dll");
  if (h_user32 == nullptr) {
    LogError("Failed to get user32.dll's handle");
    return false;
  }

  auto* api_addr =
      reinterpret_cast<uint8_t*>(GetProcAddress(h_user32, "CreateWindowExA"));
  if (api_addr == nullptr) {
    LogError("Failed to resolve CreateWindowExA's address");
    return false;
  }

  CreateWindowExARef =
      reinterpret_cast<CreateWindowExAFunc>(HookManager::Instance().SetHook(
          HookType::kJmpHook, api_addr,
          reinterpret_cast<uint8_t*>(CreateWindowExAHook)));
  return CreateWindowExARef != nullptr;
}

static HWND WINAPI CreateWindowExAHook(DWORD dwExStyle, LPCSTR lpClassName,
                                       LPCSTR lpWindowName, DWORD dwStyle,
                                       int X, int Y, int nWidth, int nHeight,
                                       HWND hWndParent, HMENU hMenu,
                                       HINSTANCE hInstance, LPVOID lpParam) {
  using namespace hooking;

  const auto hwnd = CreateWindowExARef(dwExStyle, lpClassName, lpWindowName,
                                       dwStyle, X, Y, nWidth, nHeight,
                                       hWndParent, hMenu, hInstance, lpParam);
  if (hwnd == nullptr) {
    return hwnd;
  }

  // Only hook the FIRST game window. d3d9.dll internally calls CreateWindowExA
  // for its "Direct3DWindowClass" window during device creation (triggered by
  // our WatchThread). If we re-run this setup, WndProcRef gets overwritten to
  // d3d9's DefWindowProc trampoline, so the game's WndProc is never reached,
  // leaving the connection coroutine's task pointer uninitialised → crash.
  if (WndProcRef != nullptr) {
    // LogInfo("CreateWindowExAHook: skipping re-init for class='{}' (already set up)",
            // lpClassName ? lpClassName : "(null)");
    return hwnd;
  }

  // LogInfo("CreateWindowExAHook: class='{}' hwnd={:x}",
          // lpClassName ? lpClassName : "(null)",
          // reinterpret_cast<uintptr_t>(hwnd));

  // Remember the main game window so plugins can target it (e.g. AutoLogin
  // posts input messages here).
  g_game_hwnd = hwnd;

  // Hook WndProc
  WNDCLASSEXA wnd_class;
  wnd_class.cbSize = sizeof(wnd_class);
  if (!GetClassInfoExA(hInstance, lpClassName, &wnd_class)) {
    return hwnd;
  }
  if (wnd_class.lpfnWndProc == nullptr) {
    LogError("WndProc was nullptr, cannot hook");
    return hwnd;
  }

  WndProcRef = reinterpret_cast<WindowProcFunc>(HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(wnd_class.lpfnWndProc),
      reinterpret_cast<uint8_t*>(WindowProcHook)));
  // LogInfo("CreateWindowExAHook: WndProc hooked, proc={:x} trampoline={:x}",
          // reinterpret_cast<uintptr_t>(wnd_class.lpfnWndProc),
          // reinterpret_cast<uintptr_t>(WndProcRef));

  // Start initializing imgui
  ImGui::CreateContext();
  // Charge la police coréenne (glyphes hangul pré-bakés) AVANT la 1ère frame, pour
  // que l'atlas statique construit par le backend (DX7/DX9) contienne le coréen.
  // Sans ça, toute chaîne CP949 s'affiche en carrés. Voir ui/ro_imgui.h.
  ro::LoadKoreanFont();
  ImGui::StyleColorsDark();
  ImGui_ImplWin32_Init(hwnd);
  ImGuiIO& io = ImGui::GetIO();
  io.MouseDrawCursor = false;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

  return hwnd;
}

// Check if client-space coordinates (mx, my) fall inside any ImGui window that
// was active in the last rendered frame. This uses last-frame window rects but
// current-frame mouse coords, which is correct: windows don't move between a
// WM_MOUSEMOVE and the previous EndScene, so there's no meaningful lag.
// This handles the one-frame gap in io.WantCaptureMouse when the cursor first
// enters an ImGui window (WantCaptureMouse is still false that first frame).
static bool IsMouseOverAnyImGuiWindow(float mx, float my) {
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (!ctx) return false;
  ImVec2 p(mx, my);
  for (ImGuiWindow* w : ctx->Windows) {
    // Skip click-through windows (NoMouseInputs, e.g. locked HUD bars): they
    // never capture the mouse, so they must not block clicks to the game.
    if (w->WasActive && !(w->Flags & ImGuiWindowFlags_NoMouseInputs) &&
        w->OuterRectClipped.Contains(p))
      return true;
  }
  return false;
}

// Like IsMouseOverAnyImGuiWindow but INCLUDES click-through (NoMouseInputs)
// windows: a locked HUD bar/portrait still draws ON TOP of the game's batched
// cursor, so we must redraw the RO cursor above it (while still letting the click
// pass to the game — that's handled separately by the input-routing path which
// uses the stricter check above).
static bool IsMouseOverAnyVisibleImGuiWindow(float mx, float my) {
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (!ctx) return false;
  ImVec2 p(mx, my);
  for (ImGuiWindow* w : ctx->Windows) {
    if (w->WasActive && w->OuterRectClipped.Contains(p))
      return true;
  }
  return false;
}

// Track which mouse buttons were pressed while NOT over ImGui, so their
// corresponding button-up events are always forwarded to the game even if the
// cursor has drifted over an ImGui window in the meantime.
static uint8_t g_mouse_captured_by_game = 0;  // bitmask: bit0=L, bit1=R, bit2=M

static LRESULT CALLBACK WindowProcHook(HWND hwnd, UINT uMsg, WPARAM wParam,
                                       LPARAM lParam) {
  // Frappe synthétique de Bourgeon : elle vise le client NATIF (auto-confirm du
  // char-server, service-select). Court-circuit AVANT ImGui — sans quoi notre
  // propre UI la traiterait comme une frappe du joueur : l'Entrée postée pour
  // valider « Select Service » atterrissait dans le char-select ImGui et
  // déclenchait une entrée en jeu non demandée.
  if ((uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) && lParam == kSyntheticKeyLParam)
    return WndProcRef(hwnd, uMsg, wParam, lParam);

  // Only process ImGui events after at least one frame has been rendered.
  // Before that (e.g. D3D9 login screen where EndScene hasn't fired yet),
  // ImGui side-effects like SetCapture() on WM_LBUTTONDOWN interfere with
  // the game's connection coroutine and cause a crash.
  if (ImGui::GetCurrentContext() && ImGui::GetFrameCount() > 0) {
    ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam);

    // Échap avalé pour le JEU tant qu'une fenêtre RO fermable est ouverte (ImGui
    // vient de le recevoir via le handler ; ProcessEscapeStack ferme la fenêtre du
    // dessus). Évite l'ouverture intempestive du menu natif.
    // 🔴 La barre de chat dépliée (battle mode) la confisque elle aussi, et elle
    // n'est PAS une fenêtre RO : elle ne s'enregistre donc pas dans la pile
    // ci-dessus. Sans ce test, la frappe qui la referme ouvrirait en même temps le
    // menu du client — deux effets pour un geste, exactement ce qu'on venait de
    // corriger côté fenêtres RO.
    if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE &&
        (ro::AnyEscapeWindowOpen() ||
         (Bourgeon::Instance().chat_window() != nullptr &&
          Bourgeon::Instance().chat_window()->WantsEscapeKey()))) {
      // 🔴 REMISE DIRECTE. La chatbox apprend normalement les touches par
      // `ProcessPushButton`, c'est-à-dire par LE JEU — à qui l'on vient
      // précisément de retirer celle-ci. Sans cette ligne, Échap est avalé pour
      // tout le monde et ne referme plus rien.
      if (Bourgeon::Instance().chat_window() != nullptr)
        Bourgeon::Instance().chat_window()->OnRawKey(VK_ESCAPE);
      return 0;
    }

    // Même principe pour Entrée, que la fenêtre de refine utilise comme raccourci
    // de validation : sans ça le jeu ouvrirait AUSSI sa saisie de chat par-dessus.
    // La touche est confisquée tant que cette fenêtre est OUVERTE, pas seulement
    // quand l'action est possible — sinon, en enchaînant les refines à coups
    // d'Entrée, chaque creux du cycle (tentative en vol, liste consommée) laissait
    // passer la frappe et faisait clignoter le chat. Fenêtre fermée, la touche
    // revient intégralement au jeu.
    //
    // 🔴 ESPACE AUSSI, et ce n'est pas un raffinement : les deux touches sont le
    // MÊME évènement pour le client.
    //   UIWindowMgr_OnKeyDown  @0x00A471E0 :  if (key == 13 || key == 32)
    //                                            -> UIWindowMgr_ActivateDefault @0x00A2E270
    //   celui-ci appelle OnMsg(msg = 0) sur la fenêtre prioritaire, et
    //   UIWindow_OnMsg_Default @0x008841D0 traduit msg 0 en
    //   OnMsg(6, this+0x8C) — c'est-à-dire un CLIC RÉEL sur le bouton par défaut
    //   (msg 1 fait de même avec +0x90, le bouton Annuler).
    // Aucune de ces étapes ne regarde la visibilité (le prédicat vt+8 des fenêtres
    // est un `return 1` en dur, @0x005A5D90) : une fenêtre masquée par son +0x28
    // reçoit la frappe et agit. Confisquer Entrée sans confisquer Espace laisserait
    // donc la moitié du trou ouvert.
    if (uMsg == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_SPACE)) {
      if (auto* refine = Bourgeon::Instance().weapon_refine_window())
        if (refine->WantsEnterKey()) return 0;
      // Même règle pour la fenêtre de fabrication (94 « LIST » / 79).
      if (auto* make_item = Bourgeon::Instance().make_item_window())
        if (make_item->WantsEnterKey()) return 0;
    }

    // ── Parcours de login moderne : le clavier appartient à ImGui ─────────────
    // Du formulaire web jusqu'au char-select, AUCUNE touche du joueur ne doit
    // atteindre le client natif : ses écrans (login masqué, « Select Service »,
    // char-select natif fugitivement découvert) réagissent tous au clavier sans
    // jamais consulter leur visibilité. Un joueur qui martelait Entrée entrait en
    // jeu sur un personnage qu'il n'avait pas choisi, ou ouvrait la fenêtre
    // native de création — dont notre scène ne reprend la main qu'après annulation.
    //
    // On ne relâche qu'en SORTANT du parcours (repli « Login classique », écran
    // natif assumé) ou une fois en jeu : c'est ce que portent les deux prédicats.
    // ImGui, lui, a déjà reçu le message plus haut — nos champs de saisie et la
    // navigation continuent de fonctionner normalement.
    //
    // ⚠ Les combinaisons Alt restent au système et au jeu (Alt+F4, Alt+Entrée) :
    // aucune n'agit sur les fenêtres RO, et les confisquer empêcherait de fermer
    // le client depuis l'écran de connexion.
    if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_CHAR ||
        uMsg == WM_UNICHAR) {
      const bool alt_combo = (GetKeyState(VK_MENU) & 0x8000) &&
                             !(GetKeyState(VK_CONTROL) & 0x8000);
      if (!alt_combo) {
        if (auto* auth = Bourgeon::Instance().moonlight_auth()) {
          if (auth->WantsKeyboard()) {
            auto* char_select = Bourgeon::Instance().char_select();
            if (char_select == nullptr || !char_select->NativeScreenHasKeyboard())
              return 0;
          }
        }
      }
    }

    ImGuiIO& io = ImGui::GetIO();

    // lParam for client-space mouse messages encodes X in low word, Y in high
    // word as signed 16-bit values.
    float mx = static_cast<float>(static_cast<short>(LOWORD(lParam)));
    float my = static_cast<float>(static_cast<short>(HIWORD(lParam)));
    bool over_imgui = io.WantCaptureMouse || IsMouseOverAnyImGuiWindow(mx, my);

    // (Plus aucun relais de glisser NATIF ici : les fenêtres qui pouvaient en
    // émettre — inventaire, cart, storage, équipement, grimoire — sont toutes des
    // viewers ImGui du groupe « Interface moderne », et leurs natives ne naissent
    // plus. Tout se joue désormais en glisser ImGui.)

    // Let the game's Windows cursor (SetCursor) show through on top of ImGui.
    // ImGui's own software cursor is never drawn — the game controls the cursor
    // appearance (arrow/hand/NPC) via SetCursor() which the OS renders on top.
    io.MouseDrawCursor = false;

    if (over_imgui) {
      switch (uMsg) {
        // Track button-down events so their up-events reach the game even if
        // the cursor drifts over ImGui before the button is released.
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: g_mouse_captured_by_game &= ~1; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: g_mouse_captured_by_game &= ~2; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: g_mouse_captured_by_game &= ~4; break;
        // Forward button-up to the game if the press started outside ImGui.
        case WM_LBUTTONUP:
          if (g_mouse_captured_by_game & 1) { g_mouse_captured_by_game &= ~1; break; }
          return 0;
        case WM_RBUTTONUP:
          if (g_mouse_captured_by_game & 2) { g_mouse_captured_by_game &= ~2; break; }
          return 0;
        case WM_MBUTTONUP:
          if (g_mouse_captured_by_game & 4) { g_mouse_captured_by_game &= ~4; break; }
          return 0;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
          return 0;
      }
      // Block down/dblclk events from reaching the game when over ImGui.
      switch (uMsg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
          return 0;
      }
    } else {
      // Press started outside ImGui — mark it so the up-event reaches the game.
      switch (uMsg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: g_mouse_captured_by_game |= 1; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: g_mouse_captured_by_game |= 2; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: g_mouse_captured_by_game |= 4; break;
        case WM_LBUTTONUP: g_mouse_captured_by_game &= ~1; break;
        case WM_RBUTTONUP: g_mouse_captured_by_game &= ~2; break;
        case WM_MBUTTONUP: g_mouse_captured_by_game &= ~4; break;
      }
    }

    // Doom::WantsKeyboard() backstops io.WantCaptureKeyboard for the one
    // frame between a click that focuses the DOOM window and ImGui's own
    // capture flag taking effect (SetNextFrameWantCaptureKeyboard only applies
    // at the NEXT NewFrame) — without it, a key pressed in that gap reaches
    // the game too (e.g. Escape opening both DOOM's and the RO menu).
    // 🔴 La barre de chat DÉPLIÉE mais pas encore focalisée en veut aussi : le
    // joueur tape, ça doit s'écrire dans le chat et surtout PAS déplacer son
    // personnage. ImGui, lui, ne demande rien — aucun champ n'est actif — d'où ce
    // troisième prédicat. La chatbox récupère les caractères dans la file d'entrée
    // et les rend au champ une frame plus tard (`WantsTypedKeys`).
    ChatWindow* const chat_typing = Bourgeon::Instance().chat_window();
    const bool chat_wants_typed =
        (chat_typing != nullptr) && chat_typing->WantsTypedKeys();
    if (io.WantCaptureKeyboard || Doom::WantsKeyboard() || chat_wants_typed) {
      // 🔴 Une SAISIE DE TEXTE ImGui n'a besoin que des touches qui écrivent. Sans
      // la nuance ci-dessous, cliquer dans un champ — le filtre d'une boutique, la
      // quantité d'un panier — éteignait TOUT le clavier du jeu tant que le champ
      // gardait le focus : plus de F1-F9 (skillbar), plus d'Alt+F. Le joueur le vit
      // comme « la fenêtre bloque le clavier », et ne devine pas qu'il doit cliquer
      // ailleurs pour le récupérer. Même principe que l'avalage ciblé du dialogue
      // NPC juste en dessous, appliqué cette fois à la règle générale.
      //
      // On ne relâche QUE ce qu'aucun champ de saisie ne peut utiliser :
      //   - F1 a F12 : hotkeys de barre d'action, inertes pour ImGui ;
      //   - INSERT : s'asseoir / se lever. ImGui n'a pas de mode « refrappe »,
      //     cette touche ne lui sert donc À RIEN — et l'avaler faisait qu'on ne
      //     pouvait plus s'asseoir tant que la barre de chat avait le focus,
      //     c'est-à-dire précisément quand on discute assis ;
      //   - les combinaisons avec Alt : raccourcis du jeu (Alt+F...), qu'ImGui
      //     n'utilise pas non plus. Ctrl reste avalé — c'est copier/coller/tout
      //     sélectionner du champ.
      //
      // 🔴 LE CRITÈRE, pour la prochaine touche qu'on voudra ajouter : est-ce
      // qu'une SAISIE DE TEXTE peut s'en servir ? Suppr, Origine, Fin, les
      // flèches, Page haut/bas et Tab servent tous à l'édition — ils doivent
      // rester avalés, sans quoi le champ deviendrait inutilisable.
      //
      // Et seulement quand la capture vient bien d'une saisie (`WantTextInput`) :
      // les prises volontaires du clavier — char-select, DOOM, qui la demandent par
      // SetNextFrameWantCaptureKeyboard — doivent rester TOTALES, elles remplacent
      // un écran natif qu'on ne veut pas voir réagir derrière.
      // 🔴 `chat_wants_typed` compte comme une SAISIE, pas comme une prise totale :
      // la barre de chat attend des lettres, pas les F1-F9 de la barre de skills
      // ni Insert. Sans lui ici, ouvrir la barre éteignait toute la barre d'action
      // — alors même que le joueur n'a pas encore commencé à écrire.
      const bool typing =
          (io.WantTextInput || chat_wants_typed) && !Doom::WantsKeyboard();
      // 🔴 `wParam` ne désigne PAS la même chose selon le message : code VIRTUEL
      // pour WM_KEYDOWN/UP, mais CARACTÈRE pour WM_CHAR. Or VK_F1..VK_F12, ce
      // sont les codes 0x70..0x7B — c'est-à-dire 'p'..'z' en ASCII. La nuance
      // ci-dessus relâchait donc vers le jeu le WM_CHAR de ces lettres-là : tapé
      // dans un champ ImGui (le filtre du storage…), « proxy » s'écrivait AUSSI
      // dans la barre de chat native, mais pas « abcde ». Un caractère n'est
      // jamais une hotkey : la nuance ne vaut que pour les messages de TOUCHE.
      const bool char_msg = (uMsg == WM_CHAR || uMsg == WM_UNICHAR);
      // AltGr (clavier français) = Ctrl+Alt : ce sont des caractères de SAISIE
      // (@, #, [, ], {, }, \, |, €), pas des raccourcis du jeu. Sans le test de
      // Ctrl, taper @ dans un champ ImGui envoyait la frappe au jeu en plus.
      const bool alt_shortcut = (GetKeyState(VK_MENU) & 0x8000) &&
                                !(GetKeyState(VK_CONTROL) & 0x8000);
      const bool game_only_key =
          !char_msg && ((wParam >= VK_F1 && wParam <= VK_F12) ||
                        wParam == VK_INSERT || alt_shortcut);
      if (!(typing && game_only_key)) {
        // 🔴 REMISE DIRECTE, même raison qu'Échap plus haut : quand c'est LA BARRE
        // qui motive l'avalage, le jeu ne verra pas la touche, donc
        // `ProcessPushButton` ne tournera pas et `OnKeyDown` non plus. Entrée
        // n'ouvrait, ne fermait et n'envoyait plus rien dès que la barre avait
        // perdu le clavier. Les autres motifs d'avalage (un champ ImGui a le
        // focus, DOOM) ne nous concernent pas : la touche appartient alors à ce
        // champ-là.
        if (chat_wants_typed && uMsg == WM_KEYDOWN &&
            (wParam == VK_RETURN || wParam == VK_ESCAPE))
          chat_typing->OnRawKey(static_cast<unsigned long>(wParam));
        switch (uMsg) {
          case WM_KEYDOWN: case WM_KEYUP:
          case WM_SYSKEYDOWN: case WM_SYSKEYUP:
          case WM_CHAR: case WM_UNICHAR:
            return 0;
        }
      }
    } else if (NpcDialogWindow::EatsKey(uMsg, wParam)) {
      // Dialogue NPC ImGui : avalage CIBLÉ (Entrée/Espace/Échap, flèches + 1-9 si
      // menu) — le reste du clavier (F1-F9, hotkeys skillbar…) atteint le jeu,
      // comme pendant un dialogue natif.
      return 0;
    } else if (ItemDescWindow::EatsBookKey(uMsg, wParam)) {
      // Livre moderne ouvert : ← et → tournent SA page. Le jeu ne doit pas les
      // voir — il les diffuse à toutes ses fenêtres, y compris la fenêtre livre
      // MASQUÉE qui se ré-affichait en changeant de page (clignotement en fond) et
      // la barre de chat, dont elles remontaient l'historique. Le pas de page est
      // armé par le prédicat et joué au rendu, hors de ce message.
      return 0;
    }
  }

  return WndProcRef(hwnd, uMsg, wParam, lParam);
}