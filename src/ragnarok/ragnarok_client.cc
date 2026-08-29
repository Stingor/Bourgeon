#include "ragnarok/render.h"
#include "ragnarok/ragnarok_client.h"

#include <Windows.h>

#include <array>
#include <cstdlib>  // abs (tolerance de deplacement du double-clic)
#include <iomanip>
#include <sstream>

#include "backends/imgui_impl_win32.h"
#include "bourgeon.h"
#include "features/gameplay/afk_screen.h"      // compteur d'inactivite (ecran de veille)
#include "features/overlays/target_frame.h"  // le vrai double-clic du monde
#include "imgui/imgui_impl_dx7.h"
#include "imgui_internal.h"
#include "features/overlays/skill_bar.h"
#include "features/windows/storage_window.h"
#include "features/windows/inventory_viewer.h"
#include "features/systems/moonlight_auth.h"        // WantsKeyboard (écrans de login)
#include "features/systems/native_login.h"          // CharSelectWindowPresent (Entrée tardive)
#include "features/windows/char_select.h"           // NativeScreenHasKeyboard
#include "features/windows/make_item_window.h"      // WantsEnterKey (avale VK_RETURN)
#include "features/windows/npc_dialog_window.h"     // EatsKey (touches du dialogue NPC)
#include "features/windows/item_desc_window.h"      // EatsBookKey (flèches du livre)
#include "features/windows/weapon_refine_window.h"  // WantsEnterKey (avale VK_RETURN)
#include "features/windows/chat_window.h"           // WantsEscapeKey (avale VK_ESCAPE)
#include "features/minigames/doom.h"
#include "ragnarok/configuration.h"
#include "ragnarok/globals.h"  // rag::kMouseScreenXAddr / kMouseScreenYAddr
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
  //
  // 🔴 L'ÉCRAN DE VEILLE PASSE PAR LÀ, LUI AUSSI, et il n'a pas d'autre choix. Le
  // drapeau natif `g_cursor_hidden` (0x01229448) semblait tout indiqué — c'est
  // bien lui qui décide du curseur — mais il ne garde QUE le chemin de
  // `CScene_RenderCellsAndCursor`. En jeu, le curseur est dessiné par un SECOND
  // chemin, `GameMode_InGame_ProcessFrame+0x6E5` (0x00c75165), dont l'appel à
  // `CursorMgr_RenderSprite` est INCONDITIONNEL : le drapeau y est ignoré, et
  // l'écrire ne produisait donc rien du tout.
  bool fs_suppress = false;
  float saved_ox = 0.0f, saved_oy = 0.0f;
  const AfkScreen* afk_cursor = Bourgeon::Instance().afk_screen();
  const bool afk_hides_cursor = (afk_cursor != nullptr) && afk_cursor->hiding_cursor();
  if (thisptr && (ro::FullscreenCursorActive() || afk_hides_cursor)) {
    __try {
      auto* ox = reinterpret_cast<float*>(reinterpret_cast<char*>(thisptr) + 0x30);
      auto* oy = reinterpret_cast<float*>(reinterpret_cast<char*>(thisptr) + 0x34);
      saved_ox = *ox;
      saved_oy = *oy;
      const float mx = static_cast<float>(*reinterpret_cast<int*>(rag::kMouseScreenXAddr));
      const float my = static_cast<float>(*reinterpret_cast<int*>(rag::kMouseScreenYAddr));
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
  // Écran de veille : le curseur natif est masqué à la source (drapeau
  // `g_cursor_hidden`), mais CELUI-CI est notre propre copie, redessinée par-dessus
  // les fenêtres ImGui — l'horloge de veille en est une, et il suffirait qu'elle
  // passe sous la souris pour qu'une flèche réapparaisse toute seule au milieu
  // d'un écran qu'on venait de vider.
  if (auto* afk = Bourgeon::Instance().afk_screen(); afk && afk->hiding_cursor())
    return;
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
    // Le refus doit nommer la version attendue : sans elle, « pas supporté »
    // envoie chercher un bug là où il n'y a qu'un exécutable de la mauvaise
    // date.
    LogError("Unsupported client {} -- Bourgeon targets 20250716",
             timestamp_as_str);
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
  // 🔴 APRÈS le thème, et pas avant : ce premier appel photographie le style à
  // 100 % pour servir de référence à tous les changements d'échelle ultérieurs.
  // Pris avant StyleColorsDark, il aurait figé les couleurs d'un autre thème —
  // et surtout, l'échelle lue au chargement de la DLL ne s'appliquerait jamais,
  // faute de contexte ImGui à ce moment-là.
  ro::ApplyUiScale();
  // 🔴 AVANT la première frame : ImGui lit imgui.ini au premier NewFrame et jette
  // toute section dont le handler n'est pas encore là. Posé plus tard, on ne
  // relirait aucune épingle — et la première écriture effacerait celles du joueur.
  ro::RegisterPinSettingsHandler();
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
  if ((uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) && lParam == kSyntheticKeyLParam) {
    // ⚠ Sauf une Entrée d'auto-confirm ARRIVÉE TROP TARD. Ces frappes visent la
    // fenêtre « Select Service » ; postées (PostMessage), elles peuvent être
    // dépilées APRÈS que le char-select natif s'est ouvert — et là, Entrée clique
    // son bouton par défaut : entrée en jeu sur un personnage non choisi, ou
    // ouverture de la fenêtre native de création quand le slot est vide (compte
    // neuf). L'auto-confirm cesse d'en poster dès que cette fenêtre existe ; on
    // jette donc celles qui la trouvent déjà là — leur travail est fini.
    if (wParam == VK_RETURN && native_login::CharSelectWindowPresent()) return 0;
    return WndProcRef(hwnd, uMsg, wParam, lParam);
  }

  // ── Écran de veille : le seul endroit d'où l'on voit TOUTE l'activité ──────
  // Ici et pas ailleurs, parce que c'est ici que passe indistinctement ce que le
  // joueur tape, clique et fait rouler — `ProcessPushButton` ne voit que les
  // touches LIÉES à une action, et nos modules ne voient chacun que ce qui les
  // concerne. Une veille bâtie sur l'un d'eux s'endormirait pendant que le
  // joueur joue.
  //
  // Placé APRÈS le court-circuit des frappes synthétiques juste au-dessus : ces
  // frappes-là sont les NÔTRES (auto-confirm du char-server), et les compter
  // comme activité du joueur retarderait la veille sans que personne n'ait rien
  // fait. Placé AVANT ImGui, en revanche, pour que le clic de réveil ne soit vu
  // par personne — ni notre interface, ni le jeu.
  if (afk::FilterMessage(uMsg, lParam)) return 0;

  // ── Le sélecteur de variante du panneau emoji de Windows, jeté ici ─────────
  // Win+. envoie « ❤️ » en DEUX caractères : le cœur, puis U+FE0F qui demande la
  // version en couleur. Un moteur de texte complet l'absorbe pendant le shaping ;
  // ImGui n'en fait pas et dessinerait son glyphe — vide, mais large d'un emoji
  // entier dans Segoe UI Emoji (MESURÉ : 2812 unités sur 2048). Chaque emoji
  // tapé serait donc suivi d'un blanc plus large que lui dans le champ de saisie.
  //
  // Le même filtrage existe côté LECTURE (ro::WireToUtf8) pour ce qui arrive du
  // réseau ; celui-ci couvre la frappe. Rien ne se perd : la couleur vient de la
  // police, pas de ce caractère.
  if (uMsg == WM_CHAR && (wParam >= 0xFE00 && wParam <= 0xFE0F)) return 0;

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
        // ── 🔴 LE DOUBLE-CLIC SE DÉTECTE ICI, ET NULLE PART AILLEURS ───────
        //
        // ⏱ Mesuré au journal, flux de messages brut à l'appui : un double-clic
        // sur une entité donne **DOWN, UP, DOWN, UP** — la fenêtre du jeu n'a pas
        // `CS_DBLCLKS`, donc **aucun `WM_LBUTTONDBLCLK` n'existe** — et le SECOND
        // `DOWN`, pourtant reçu ici sans problème, ne produit **aucun** appel à
        // `GameMode_PostActorClickAction` : la machine à états de souris du client
        // n'en fait pas un appui frais.
        //
        // Conséquence : compter les appels natifs ne peut PAS détecter un
        // double-clic — on n'en voit qu'un appui sur deux, et deux appels
        // rapprochés sont en réalité les PREMIERS appuis de deux double-clics
        // successifs. C'est trait pour trait le « il faut en faire deux »
        // rapporté en jeu, et aucun réglage de seuil n'y pouvait rien.
        //
        // Ici, les deux appuis sont visibles. On applique donc la règle de
        // Windows elle-même : même délai (`GetDoubleClickTime`) et même
        // tolérance de déplacement (`SM_C?DOUBLECLK`) que ce que le système
        // aurait fait s'il avait envoyé le message.
        case WM_LBUTTONDOWN: {
          g_mouse_captured_by_game |= 1;
          static unsigned last_down_ms = 0;
          static int      last_down_x  = 0;
          static int      last_down_y  = 0;
          const unsigned  now = GetTickCount();
          const int       x   = static_cast<int>(mx);
          const int       y   = static_cast<int>(my);
          const bool doubled =
              last_down_ms != 0 && (now - last_down_ms) <= GetDoubleClickTime() &&
              abs(x - last_down_x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
              abs(y - last_down_y) <= GetSystemMetrics(SM_CYDOUBLECLK);
          if (doubled) {
            // On repart de zéro : un troisième appui ne doit pas se lire comme un
            // nouveau double avec le deuxième.
            last_down_ms = 0;
            if (auto* tf = Bourgeon::Instance().target_frame())
              tf->NoteWorldDoubleClick();
          } else {
            last_down_ms = now;
            last_down_x  = x;
            last_down_y  = y;
          }
          break;
        }
        // Gardé par sûreté : ce client-ci n'envoie pas ce message, mais une autre
        // version (ou un autre style de classe) le ferait, et il vaut alors
        // exactement le second appui détecté ci-dessus.
        case WM_LBUTTONDBLCLK:
          g_mouse_captured_by_game |= 1;
          if (auto* tf = Bourgeon::Instance().target_frame())
            tf->NoteWorldDoubleClick();
          break;
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
      //
      // 🔴 ET UN WIDGET ACTIF N'EST PAS UNE PRISE DE CLAVIER. ImGui lève
      // `WantCaptureKeyboard` dès que `ActiveId != 0` — donc pendant un GLISSER
      // d'objet, un bouton tenu, un slider tiré. Aucun de ces gestes n'attend de
      // touche, et pourtant le clavier du jeu s'éteignait ENTIÈREMENT : une émote
      // portant « @storage » en Alt+3 ne partait plus dès qu'on avait attrapé un
      // item — alors que le client natif l'acceptait, et que c'est justement
      // quand on tient quelque chose qu'on veut ouvrir l'entrepôt.
      // On le range donc du côté de la SAISIE et non de la prise totale : même
      // libération ciblée, même critère (ce qu'aucun champ ne peut utiliser).
      const bool widget_held =
          ImGui::IsAnyItemActive() && !io.WantTextInput && !chat_wants_typed;
      const bool typing = (io.WantTextInput || chat_wants_typed || widget_held) &&
                          !Doom::WantsKeyboard();
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
      // 🔴 IMPR. ÉCRAN : la capture NATIVE du client, et le SEUL moyen de la
      // déclencher. Relevé dans `Game_MainWndProc` (0x00DB8100) :
      //     case WM_KEYUP: if (wParam == 0x2C)
      //                      UIWindowMgr_OnKeyDown(mgr, 0x2C, lParam, 1);
      // — c'est un WM_KEY**UP**, Windows n'émettant pas de WM_KEYDOWN pour cette
      // touche. Notre garde avalait les deux : barre de chat ouverte (ou simple
      // champ ImGui focalisé), plus une seule capture ne partait, et le joueur
      // n'avait aucun moyen de deviner pourquoi.
      // Elle est relâchée quel que soit le MOTIF de la capture, pas seulement en
      // saisie : aucun de nos écrans (DOOM, char-select) n'a d'usage d'Impr.
      // écran, et une capture est sans effet de bord sur le jeu. D'où sa place
      // À CÔTÉ de `game_only_key` et non dedans.
      // ⚠ `!char_msg` est vital ici : 0x2C, c'est aussi la VIRGULE en WM_CHAR.
      const bool screenshot_key = !char_msg && wParam == VK_SNAPSHOT;
      if (!(typing && game_only_key) && !screenshot_key) {
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
      //
      // 🔴 REMISE DIRECTE D'ENTRÉE À LA BARRE ARMÉE, même raison qu'Échap plus
      // haut : la touche n'atteint pas le jeu, donc `ProcessPushButton` ne tourne
      // pas et `OnKeyDown` non plus. Un lien relayé d'un Maj+clic depuis le
      // dialogue restait alors PRISONNIER de la saisie — plus moyen de l'envoyer
      // sans fermer le script. Le dialogue, lui, ne réagit pas à cette Entrée-là :
      // il consulte le même prédicat au rendu.
      // (Rien à faire quand la saisie a déjà le clavier : on ne serait pas passé
      // par ici, `WantCaptureKeyboard` ayant pris la branche du dessus.)
      if (uMsg == WM_KEYDOWN && wParam == VK_RETURN && chat_typing != nullptr &&
          chat_typing->OwnsEnterKey())
        chat_typing->OnRawKey(VK_RETURN);
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

  // ── Échap décible, EN DERNIER RECOURS ─────────────────────────────────────
  // Le geste des MMO : Échap lâche d'abord la cible, et n'ouvre le menu du jeu
  // que s'il n'y en avait pas. Opt-in (« Échap efface la cible » dans le HUD de
  // cible), parce que la touche appartient au client depuis toujours.
  //
  // 🔴 SA PLACE EST ICI, tout en bas, et pas dans le bloc Échap du haut : tout
  // ce qui pouvait vouloir cette touche l'a déjà prise — fenêtre RO fermable,
  // barre de chat dépliée, saisie ImGui, dialogue NPC. Un appui ne fait jamais
  // deux choses, et le déciblage est ce qu'on veut EN DERNIER.
  //
  // 🔴 La touche n'est confisquée que si le déciblage a EU LIEU : sans cible,
  // `ClearTarget` renvoie faux et Échap repart intact au client, qui ouvre son
  // menu. C'est le second appui du geste, et il doit marcher du premier coup.
  if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
    auto* target_frame = Bourgeon::Instance().target_frame();
    if (target_frame != nullptr && target_frame->escape_clears_ &&
        target_frame->ClearTarget())
      return 0;
  }

  return WndProcRef(hwnd, uMsg, wParam, lParam);
}