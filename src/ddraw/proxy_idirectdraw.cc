// This file was taken from SimpleROHook and adapted
#include "ddraw/proxy_idirectdraw.h"

#include <Windows.h>
#include <atomic>
#include <thread>

#include "backends/imgui_impl_win32.h"
#include "bourgeon.h"
#include "imgui/imgui_impl_dx7.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// ── DX7 vtable hook ────────────────────────────────────────────────────────────
//
// The game bypasses our DirectDrawCreateEx proxy (uses COM or loads system
// ddraw.dll by full path).  To catch all D3D7 devices regardless of creation
// path we patch the IDirect3D7 *factory* vtable slot for CreateDevice (index 4).
// When the game's device is created our hook fires, we read the real device
// vtable, and patch EndScene (index 6) at that moment.  This avoids the
// "wrong vtable" problem caused by patching a temp RGB device upfront.

typedef HRESULT(WINAPI* DirectDrawCreateExFn)(GUID FAR*, LPVOID*, REFIID,
                                              IUnknown FAR*);
extern DirectDrawCreateExFn m_pDirectDrawCreateEx;

// IDirect3D7 vtable indices
static constexpr int kDX7CreateDeviceIdx = 4;
// IDirect3DDevice7 vtable indices
static constexpr int kDX7EndSceneIdx     = 6;

using DX7CreateDevice_t = HRESULT(__fastcall*)(IDirect3D7*, void*, REFCLSID,
                                               LPDIRECTDRAWSURFACE7,
                                               LPDIRECT3DDEVICE7*);
using DX7EndScene_t     = HRESULT(__fastcall*)(IDirect3DDevice7*, void*);

static DX7CreateDevice_t g_orig_d3d7_create_device = nullptr;
static DX7EndScene_t     g_orig_dx7_end_scene       = nullptr;
static std::atomic<bool> g_dx7_imgui_ready{false};

bool g_imgui_dx7_active = false;

// ── helper: in-place vtable slot patch ───────────────────────────────────────
static void PatchDX7Slot(void** vtable, int idx, void* hook, void** out_orig) {
    void** slot = &vtable[idx];
    *out_orig   = *slot;
    LogInfo("DX7: vtable[{}] = {:x}", idx, reinterpret_cast<uintptr_t>(*slot));
    DWORD old;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = hook;
        VirtualProtect(slot, sizeof(void*), old, &old);
        LogInfo("DX7: vtable[{}] patched", idx);
    } else {
        LogError("DX7: VirtualProtect failed for vtable[{}]", idx);
    }
}

// ── EndScene hook: renders ImGui onto the game's D3D7 device ─────────────────
static HRESULT __fastcall Hooked_DX7_EndScene(IDirect3DDevice7* self, void*) {
    if (!ImGui::GetCurrentContext())
        return g_orig_dx7_end_scene(self, nullptr);

    if (!g_dx7_imgui_ready.load()) {
        LogInfo("DX7 EndScene: ImGui_ImplDX7_Init device={:x}",
                reinterpret_cast<uintptr_t>(self));
        ImGui_ImplDX7_Init(self);
        g_imgui_dx7_active = true;
        g_dx7_imgui_ready.store(true);
    }

    ImGui_ImplDX7_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    Bourgeon::Instance().RenderUI();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX7_RenderDrawData(ImGui::GetDrawData());

    return g_orig_dx7_end_scene(self, nullptr);
}

// ── CreateDevice hook: intercepts game's device creation ─────────────────────
// Fires when ANY code calls IDirect3D7::CreateDevice, regardless of how the
// IDirect3D7 was obtained.  We read the real device vtable and patch EndScene.
static HRESULT __fastcall Hooked_D3D7_CreateDevice(IDirect3D7* self, void* /*edx*/,
                                                    REFCLSID rclsid,
                                                    LPDIRECTDRAWSURFACE7 lpDDS,
                                                    LPDIRECT3DDEVICE7* lplpD3DDevice) {
    LogInfo("D3D7 Hooked_CreateDevice factory={:x}", reinterpret_cast<uintptr_t>(self));
    HRESULT hr = g_orig_d3d7_create_device(self, nullptr, rclsid, lpDDS, lplpD3DDevice);
    if (SUCCEEDED(hr) && lplpD3DDevice && *lplpD3DDevice) {
        void** vtable7 = *reinterpret_cast<void***>(*lplpD3DDevice);
        LogInfo("D3D7 Hooked_CreateDevice: device={:x} vtable={:x}",
                reinterpret_cast<uintptr_t>(*lplpD3DDevice),
                reinterpret_cast<uintptr_t>(vtable7));
        // Patch EndScene on this vtable if not already hooked.
        if (vtable7[kDX7EndSceneIdx] != static_cast<void*>(&Hooked_DX7_EndScene)) {
            PatchDX7Slot(vtable7, kDX7EndSceneIdx, &Hooked_DX7_EndScene,
                         reinterpret_cast<void**>(&g_orig_dx7_end_scene));
        }
    }
    LogInfo("D3D7 Hooked_CreateDevice: hr={:x}", (unsigned)hr);
    return hr;
}

// ── JMP hook on system ddraw.dll's DirectDrawCreateEx ────────────────────────
// Intercepts ALL callers — including GetProcAddress on the system DLL — so we
// can see and re-patch from the IDirect3D7 the game actually uses.

typedef HRESULT(WINAPI* DirectDrawCreateExFn)(GUID FAR*, LPVOID*, REFIID, IUnknown FAR*);
static DirectDrawCreateExFn g_orig_sys_ddraw_create = nullptr;

static void EnsureIDirect3D7Patched(IDirectDraw7* dd7) {
    IDirect3D7* d3d7 = nullptr;
    HRESULT hr = dd7->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d7));
    if (FAILED(hr) || !d3d7) {
        LogError("SysDDCreateEx: QI(IDirect3D7) failed hr={:x}", (unsigned)hr);
        return;
    }
    void** vtbl = *reinterpret_cast<void***>(d3d7);
    LogInfo("SysDDCreateEx: IDirect3D7 vtable={:x}", reinterpret_cast<uintptr_t>(vtbl));
    if (vtbl[kDX7CreateDeviceIdx] != static_cast<void*>(&Hooked_D3D7_CreateDevice)) {
        LogInfo("SysDDCreateEx: patching CreateDevice slot");
        PatchDX7Slot(vtbl, kDX7CreateDeviceIdx, &Hooked_D3D7_CreateDevice,
                     reinterpret_cast<void**>(&g_orig_d3d7_create_device));
    } else {
        LogInfo("SysDDCreateEx: CreateDevice already patched");
    }
    d3d7->Release();
}

static HRESULT WINAPI Hooked_SysDDCreateEx(GUID FAR* lpGuid, LPVOID* lplpDD,
                                             REFIID riid, IUnknown FAR* pUnk) {
    LogInfo("System DirectDrawCreateEx called riid={:x}",
            riid.Data1);  // first DWORD of GUID
    HRESULT hr = g_orig_sys_ddraw_create(lpGuid, lplpDD, riid, pUnk);
    if (SUCCEEDED(hr) && lplpDD && *lplpDD && IsEqualIID(riid, IID_IDirectDraw7)) {
        EnsureIDirect3D7Patched(reinterpret_cast<IDirectDraw7*>(*lplpDD));
    }
    return hr;
}

// ── background thread ─────────────────────────────────────────────────────────
static void DX7WatchThread() {
    LogInfo("DX7 WatchThread: started");

    if (!m_pDirectDrawCreateEx) {
        LogError("DX7 WatchThread: m_pDirectDrawCreateEx not ready");
        return;
    }

    // JMP-hook system ddraw.dll's DirectDrawCreateEx at the code level so ALL
    // callers (our proxy export, GetProcAddress, COM) are intercepted.
    {
        using namespace hooking;
        auto* fn = reinterpret_cast<uint8_t*>(m_pDirectDrawCreateEx);
        g_orig_sys_ddraw_create = reinterpret_cast<DirectDrawCreateExFn>(
            HookManager::Instance().SetHook(HookType::kJmpHook, fn,
                                            reinterpret_cast<uint8_t*>(Hooked_SysDDCreateEx)));
        if (g_orig_sys_ddraw_create)
            LogInfo("DX7 WatchThread: system DirectDrawCreateEx JMP-hooked");
        else
            LogError("DX7 WatchThread: JMP hook on DirectDrawCreateEx failed");
    }

    // Create a temp IDirectDraw7 now so Hooked_SysDDCreateEx immediately patches
    // the IDirect3D7 vtable — covers the case where the game hasn't called it yet.
    // This call goes through Hooked_SysDDCreateEx → EnsureIDirect3D7Patched.
    {
        IDirectDraw7* dd = nullptr;
        HRESULT hr = m_pDirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd),
                                           IID_IDirectDraw7, nullptr);
        if (SUCCEEDED(hr) && dd) {
            dd->Release();
        } else {
            LogError("DX7 WatchThread: temp DirectDrawCreateEx failed hr={:x}", (unsigned)hr);
        }
    }
    LogInfo("DX7 WatchThread: done");
}

// ── diagnostic: log which rendering DLLs are loaded after game starts ────────
static void RenderDiagThread() {
    Sleep(5000);  // wait for game to finish loading everything
    static const char* kDlls[] = {
        "d3d8.dll", "d3d9.dll", "d3d10.dll", "d3d10_1.dll",
        "d3d11.dll", "d3d12.dll", "opengl32.dll", "vulkan-1.dll",
        "dxgi.dll", nullptr
    };
    LogInfo("=== Render DLL scan ===");
    for (int i = 0; kDlls[i]; ++i) {
        HMODULE h = GetModuleHandleA(kDlls[i]);
        if (h) {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(h, path, sizeof(path));
            LogInfo("  {}: LOADED  path={}", kDlls[i], path);
        } else {
            LogInfo("  {}: not loaded", kDlls[i]);
        }
    }
    LogInfo("=== end scan ===");
}

void InitDX7Hook() {
    std::thread(DX7WatchThread).detach();
    std::thread(RenderDiagThread).detach();
}

// ── Proxy class implementations ───────────────────────────────────────────────

CProxyIDirectDraw7* CProxyIDirectDraw7::lpthis;

CProxyIDirectDraw7::CProxyIDirectDraw7(IDirectDraw7* ptr)
    : m_Instance(ptr),
      CooperativeLevel(0),
      PrimarySurfaceFlag(0),
      TargetSurface(nullptr) {
  LogDebug("IDirectDraw7::Create");
}

CProxyIDirectDraw7::~CProxyIDirectDraw7() { LogDebug("IDirectDraw7::Release"); }

ULONG CProxyIDirectDraw7::Proxy_Release(void) {
  ULONG Count = m_Instance->Release();
  LogDebug("CProxyIDirectDraw7::Release(): RefCount = {}", Count);
  delete this;
  return Count;
}

HRESULT CProxyIDirectDraw7::Proxy_RestoreAllSurfaces(void) {
  return m_Instance->RestoreAllSurfaces();
}

HRESULT CProxyIDirectDraw7::Proxy_QueryInterface(THIS_ REFIID riid,
                                                 LPVOID FAR* ppvObj) {
  if (IsEqualGUID(riid, IID_IDirect3D7)) {
    LogInfo("CProxyIDirectDraw7::QueryInterface(IDirect3D7)");
    HRESULT temp_ret = m_Instance->QueryInterface(riid, ppvObj);
    if (temp_ret == S_OK) {
      *ppvObj = new CProxyIDirect3D7(reinterpret_cast<IDirect3D7*>(*ppvObj));
      LogInfo("CProxyIDirectDraw7::IDirect3D7 wrapped");
      return S_OK;
    }
    LogInfo("CProxyIDirectDraw7::IDirect3D7 QI failed hr={:x}", (unsigned)temp_ret);
    return temp_ret;
  }
  return m_Instance->QueryInterface(riid, ppvObj);
}

HRESULT CProxyIDirectDraw7::Proxy_CreateSurface(
    LPDDSURFACEDESC2 SurfaceDesc, LPDIRECTDRAWSURFACE7 FAR* CreatedSurface,
    IUnknown FAR* pUnkOuter) {
  HRESULT Result =
      m_Instance->CreateSurface(SurfaceDesc, CreatedSurface, pUnkOuter);
  if (FAILED(Result)) return Result;

  if (SurfaceDesc->dwFlags & DDSD_CAPS) {
    DDSCAPS2* Caps = &SurfaceDesc->ddsCaps;
    if (Caps->dwCaps & DDSCAPS_PRIMARYSURFACE) {
      *CreatedSurface = new CProxyIDirectDrawSurface7(*CreatedSurface);
      PrimarySurfaceFlag = 1;
    } else if (Caps->dwCaps & DDSCAPS_3DDEVICE) {
      if (CooperativeLevel & DDSCL_FULLSCREEN && !PrimarySurfaceFlag)
        *CreatedSurface = new CProxyIDirectDrawSurface7(*CreatedSurface);
      else
        TargetSurface = *CreatedSurface;
    } else if ((CooperativeLevel & DDSCL_FULLSCREEN) &&
               (Caps->dwCaps & DDSCAPS_BACKBUFFER)) {
      *CreatedSurface = new CProxyIDirectDrawSurface7(*CreatedSurface);
    }
  }
  return Result;
}

HRESULT CProxyIDirectDraw7::Proxy_GetDisplayMode(LPDDSURFACEDESC2 Desc) {
  return m_Instance->GetDisplayMode(Desc);
}

HRESULT CProxyIDirectDraw7::Proxy_SetCooperativeLevel(HWND hWnd, DWORD dwFlags) {
  LogDebug("IDirectDraw7::SetCooperativeLevel(): dwFlags = 0x{:x}", dwFlags);
  CooperativeLevel = dwFlags;
  return m_Instance->SetCooperativeLevel(hWnd, dwFlags);
}

HRESULT CProxyIDirectDraw7::Proxy_SetDisplayMode(DWORD p1, DWORD p2, DWORD p3,
                                                 DWORD p4, DWORD p5) {
  return m_Instance->SetDisplayMode(p1, p2, p3, p4, p5);
}

HRESULT CProxyIDirectDraw7::Proxy_WaitForVerticalBlank(DWORD dwFlags, HANDLE hEvent) {
  return m_Instance->WaitForVerticalBlank(dwFlags, hEvent);
}

// CProxyIDirect3D7 ─────────────────────────────────────────────────────────────

HRESULT CProxyIDirect3D7::Proxy_CreateDevice(REFCLSID rclsid,
                                             LPDIRECTDRAWSURFACE7 lpDDS,
                                             LPDIRECT3DDEVICE7* lplpD3DDevice) {
  LogInfo("CProxyIDirect3D7::CreateDevice called");
  HRESULT temp_ret = m_Instance->CreateDevice(rclsid, lpDDS, lplpD3DDevice);
  if (temp_ret == D3D_OK) {
    *lplpD3DDevice = reinterpret_cast<LPDIRECT3DDEVICE7>(
        new CProxyIDirect3DDevice7(
            reinterpret_cast<IDirect3DDevice7*>(*lplpD3DDevice), lpDDS));
  }
  return temp_ret;
}

// CProxyIDirect3DDevice7 ───────────────────────────────────────────────────────

CProxyIDirect3DDevice7::~CProxyIDirect3DDevice7() {
  LogDebug("CProxyIDirect3DDevice7::dtor");
}

ULONG CProxyIDirect3DDevice7::Proxy_Release() {
  ULONG Count = m_Instance->Release();
  LogDebug("CProxyIDirect3DDevice7::Release(): RefCount = {}", Count);
  if (Count == 0) delete this;
  return Count;
}

HRESULT CProxyIDirect3DDevice7::Proxy_SetRenderState(
    THIS_ D3DRENDERSTATETYPE dwRenderStateType, DWORD dwRenderState) {
  return m_Instance->SetRenderState(dwRenderStateType, dwRenderState);
}

HRESULT CProxyIDirect3DDevice7::Proxy_BeginScene() {
  return m_Instance->BeginScene();
}

// Proxy_EndScene passes through — Hooked_DX7_EndScene (vtable hook) renders ImGui.
HRESULT CProxyIDirect3DDevice7::Proxy_EndScene(void) {
  return m_Instance->EndScene();
}
