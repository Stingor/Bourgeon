// This file was taken from SimpleROHook and adapted
#include "ddraw/proxy_idirectdraw.h"

#include <Windows.h>

#include "backends/imgui_impl_win32.h"
#include "bourgeon.h"
#include "imgui/imgui_impl_dx7.h"
#include "utils/log_console.h"

extern void DrawROCursorImGui();

// ImGui is rendered in Proxy_EndScene which fires for every frame because the
// game always goes through our DirectDrawCreateEx export (ddraw.dll proxy).
// g_imgui_dx7_active tells the D3D9 hook to stay quiet while DX7 is running.

static bool g_dx7_imgui_ready = false;
bool g_imgui_dx7_active = false;

// Track which EndScene call is the last one before Flip (1-frame delay).
// On frame N, Flip records how many EndScenes happened (g_dx7_target_endscene).
// On frame N+1, ImGui renders only at that index — never in earlier passes —
// so ImGui is never baked into an intermediate backbuffer that a later pass
// reads, which was the root cause of the ghost/duplicate artifact.
static int g_dx7_endscene_index  = 0;
static int g_dx7_target_endscene = INT_MAX;  // initially large = skip first frame

// InitDX7Hook is called from main.cc; nothing to install for the proxy path.
void InitDX7Hook() {}

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
    LogDebug("CProxyIDirectDraw7::QueryInterface(IDirect3D7)");
    HRESULT temp_ret = m_Instance->QueryInterface(riid, ppvObj);
    if (temp_ret == S_OK) {
      *ppvObj = new CProxyIDirect3D7(reinterpret_cast<IDirect3D7*>(*ppvObj));
      LogDebug("CProxyIDirectDraw7::IDirect3D7 wrapped");
      return S_OK;
    }
    LogError("CProxyIDirectDraw7::IDirect3D7 QI failed hr={:x}", (unsigned)temp_ret);
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
  LogDebug("CProxyIDirect3D7::CreateDevice called");
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

HRESULT CProxyIDirect3DDevice7::Proxy_EndScene(void) {
  if (ImGui::GetCurrentContext()) {
    if (!g_dx7_imgui_ready) {
      ImGui_ImplDX7_Init(m_Instance);
      g_imgui_dx7_active = true;
      g_dx7_imgui_ready = true;
    }
    if (++g_dx7_endscene_index == g_dx7_target_endscene) {
      ImGui_ImplDX7_NewFrame();
      ImGui_ImplWin32_NewFrame();
      ImGui::NewFrame();
      Bourgeon::Instance().RenderUI();
      DrawROCursorImGui();
      ImGui::EndFrame();
      ImGui::Render();
      ImGui_ImplDX7_RenderDrawData(ImGui::GetDrawData());
    }
  }
  return m_Instance->EndScene();
}

HRESULT CProxyIDirectDrawSurface7::Proxy_Flip(LPDIRECTDRAWSURFACE7 p1, DWORD p2) {
  // Record how many EndScene calls happened this frame so next frame we render
  // ImGui only on the last one (avoiding ghost artifacts from intermediate passes).
  g_dx7_target_endscene = g_dx7_endscene_index;
  g_dx7_endscene_index  = 0;
  return m_Instance->Flip(p1, p2);
}
