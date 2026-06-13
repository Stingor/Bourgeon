#include "ragnarok/ragnarok_client.h"

#include <Windows.h>

#include <array>
#include <iomanip>
#include <sstream>

#include "backends/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx7.h"
#include "imgui_internal.h"
#include "ragnarok/configuration.h"
#include "ragnarok/object_factory.h"
#include "ragnarok/packets.h"
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
  LogInfo("Detected client: {}", timestamp_as_str);

  const YAML::Node configuration = LoadConfiguration();
  const auto client_configuration = configuration[timestamp_as_str];
  if (!client_configuration.IsDefined()) {
    LogError("This client isn't supported");
    return false;
  }

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

uint32_t RagnarokClient::timestamp() const { return timestamp_; }

Session& RagnarokClient::session() const { return *session_; }

RagConnection& RagnarokClient::rag_connection() const {
  return *rag_connection_;
}

UIWindowMgr& RagnarokClient::window_mgr() const { return *window_mgr_; }

bool RagnarokClient::UseItemById(int item_id) const {
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
    LogInfo("CreateWindowExAHook: skipping re-init for class='{}' (already set up)",
            lpClassName ? lpClassName : "(null)");
    return hwnd;
  }

  LogInfo("CreateWindowExAHook: class='{}' hwnd={:x}",
          lpClassName ? lpClassName : "(null)",
          reinterpret_cast<uintptr_t>(hwnd));

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
  LogInfo("CreateWindowExAHook: WndProc hooked, proc={:x} trampoline={:x}",
          reinterpret_cast<uintptr_t>(wnd_class.lpfnWndProc),
          reinterpret_cast<uintptr_t>(WndProcRef));

  // Start initializing imgui
  ImGui::CreateContext();
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
    if (w->WasActive && w->OuterRectClipped.Contains(p))
      return true;
  }
  return false;
}

static LRESULT CALLBACK WindowProcHook(HWND hwnd, UINT uMsg, WPARAM wParam,
                                       LPARAM lParam) {
  // Only process ImGui events after at least one frame has been rendered.
  // Before that (e.g. D3D9 login screen where EndScene hasn't fired yet),
  // ImGui side-effects like SetCapture() on WM_LBUTTONDOWN interfere with
  // the game's connection coroutine and cause a crash.
  if (ImGui::GetCurrentContext() && ImGui::GetFrameCount() > 0) {
    ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam);

    ImGuiIO& io = ImGui::GetIO();

    // lParam for client-space mouse messages encodes X in low word, Y in high
    // word as signed 16-bit values.
    float mx = static_cast<float>(static_cast<short>(LOWORD(lParam)));
    float my = static_cast<float>(static_cast<short>(HIWORD(lParam)));
    bool over_imgui = io.WantCaptureMouse || IsMouseOverAnyImGuiWindow(mx, my);

    // Use over_imgui (not IsWindowHovered) so the software cursor stays visible
    // while dragging outside a popup/window boundary — WantCaptureMouse remains
    // true for active drag items even when the pointer leaves the window rect.
    io.MouseDrawCursor = over_imgui;

    if (over_imgui) {
      switch (uMsg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
          return 0;
      }
    }

    if (io.WantCaptureKeyboard) {
      switch (uMsg) {
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR: case WM_UNICHAR:
          return 0;
      }
    }
  }

  return WndProcRef(hwnd, uMsg, wParam, lParam);
}