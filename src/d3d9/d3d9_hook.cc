#include "d3d9/d3d9_hook.h"

#include <Windows.h>
#include <d3d9.h>
#include <atomic>
#include <thread>

#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "bourgeon.h"
#include "imgui.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

extern bool g_imgui_dx7_active;

// ── vtable indices ─────────────────────────────────────────────────────────────
// IDirect3DDevice9 / IDirect3DDevice9Ex
static constexpr int kResetIdx    = 16;
static constexpr int kPresentIdx  = 17;
static constexpr int kEndSceneIdx = 42;
static constexpr int kResetExIdx  = 132;

// IDirect3D9 / IDirect3D9Ex factory
static constexpr int kFactoryCreateDeviceIdx   = 16;  // IDirect3D9::CreateDevice
static constexpr int kFactoryCreateDeviceExIdx = 20;  // IDirect3D9Ex::CreateDeviceEx

// ── function pointer types ────────────────────────────────────────────────────
// Device vtable calling convention for this client:
//   ecx  = *device (vtable pointer)
//   stack[0] = device ("this" as explicit first stack arg)
//   stack[1..N] = remaining method args
// See disassembly at 0x00542f71-0x00542f77 for confirmation.
using Reset_t    = HRESULT(__fastcall*)(void*, void*, IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ResetEx_t  = HRESULT(__fastcall*)(void*, void*, IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using EndScene_t = HRESULT(__fastcall*)(void*, void*, IDirect3DDevice9*);
using Present_t  = HRESULT(__fastcall*)(void*, void*, IDirect3DDevice9*,
                                         const RECT*, const RECT*, HWND, const RGNDATA*);

using FactoryCreateDevice_t = HRESULT(__fastcall*)(IDirect3D9*, void*,
                                                    UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS*,
                                                    IDirect3DDevice9**);
// This client's vtable[20] calling convention differs from standard __thiscall:
//   ecx  = pFullscreenDisplayMode (0 for windowed)
//   stack: [pD3D, adapter, devType, hwnd, flags, pp, pFullscreen, ppDev]  (8 args)
// The factory "this" is the FIRST STACK arg, not ecx.
using FactoryCreateDeviceEx_t = HRESULT(__fastcall*)(D3DDISPLAYMODEEX*, void*,
                                                      IDirect3D9Ex*,
                                                      UINT, D3DDEVTYPE, HWND, DWORD,
                                                      D3DPRESENT_PARAMETERS*,
                                                      D3DDISPLAYMODEEX*,
                                                      IDirect3DDevice9Ex**);

using Direct3DCreate9Ex_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using Direct3DCreate9_t   = IDirect3D9*(WINAPI*)(UINT);

// ── device hook originals ─────────────────────────────────────────────────────
static Reset_t    g_orig_reset     = nullptr;
static ResetEx_t  g_orig_reset_ex  = nullptr;
static EndScene_t g_orig_end_scene = nullptr;
static Present_t  g_orig_present   = nullptr;

// ── factory hook originals ────────────────────────────────────────────────────
static FactoryCreateDevice_t   g_orig_factory_create_device    = nullptr;
static FactoryCreateDeviceEx_t g_orig_factory_create_device_ex = nullptr;

// ── JMP hook originals (Direct3DCreate9 / Direct3DCreate9Ex code bytes) ───────
static Direct3DCreate9Ex_t g_orig_create9ex_fn = nullptr;
static Direct3DCreate9_t   g_orig_create9_fn   = nullptr;

// Cached factory pointer — used to temporarily restore vtable[20] before each
// g_orig_create9ex_fn call so d3d9.dll's internal null-self vtable dispatch
// goes to the original and never enters our hook.
static IDirect3D9Ex* g_factory_ptr = nullptr;

// Nesting depth of Hooked_Direct3DCreate9Ex calls.
// d3d9.dll internally calls Direct3DCreate9Ex recursively.  We patch vtable[20]
// only when depth returns to 0 — i.e. after ALL factory-init code is done.
static int g_create9ex_depth = 0;

static std::atomic<bool> g_dx9_initialized{false};
static std::atomic<bool> g_rendered_this_frame{false};

// ── vtable slot patcher ───────────────────────────────────────────────────────
static void PatchSlot(void** vtable, int idx, void* hook, void** out_orig) {
    void** slot = &vtable[idx];
    if (out_orig) *out_orig = *slot;
    LogInfo("D3D9: vtable[{}] = {:x}", idx, reinterpret_cast<uintptr_t>(*slot));
    DWORD old;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = hook;
        VirtualProtect(slot, sizeof(void*), old, &old);
        LogInfo("D3D9: vtable[{}] patched", idx);
    } else {
        LogError("D3D9: VirtualProtect failed for vtable[{}]", idx);
    }
}

// ── ImGui render helper ───────────────────────────────────────────────────────
static void RenderImGuiDX9(IDirect3DDevice9* self) {
    if (!g_dx9_initialized.load()) {
        LogInfo("D3D9: ImGui_ImplDX9_Init device={:x}", reinterpret_cast<uintptr_t>(self));
        ImGui_ImplDX9_Init(self);
        g_dx9_initialized.store(true);
    }
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    Bourgeon::Instance().RenderUI();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    g_rendered_this_frame.store(true);
}

// ── EndScene hook ─────────────────────────────────────────────────────────────
// ecx = vtable (*device); self = device (first explicit stack arg).
static HRESULT __fastcall Hooked_EndScene(void* vtable_ecx, void* /*edx*/,
                                           IDirect3DDevice9* self) {
    if (!g_imgui_dx7_active && ImGui::GetCurrentContext() && !g_rendered_this_frame.load()) {
        RenderImGuiDX9(self);
    }
    return g_orig_end_scene(vtable_ecx, nullptr, self);
}

// ── Present hook ──────────────────────────────────────────────────────────────
// ecx = vtable; self = device (first explicit stack arg).
// BeginScene/EndScene cannot be called safely here (would re-enter our hooks
// via standard C++ dispatch using the wrong calling convention).
static HRESULT __fastcall Hooked_Present(void* vtable_ecx, void* /*edx*/,
                                          IDirect3DDevice9* self,
                                          const RECT* pSrcRect, const RECT* pDestRect,
                                          HWND hDestWindowOverride,
                                          const RGNDATA* pDirtyRegion) {
    g_rendered_this_frame.store(false);
    return g_orig_present(vtable_ecx, nullptr, self, pSrcRect, pDestRect,
                          hDestWindowOverride, pDirtyRegion);
}

// ── Reset hook ────────────────────────────────────────────────────────────────
static HRESULT __fastcall Hooked_Reset(void* vtable_ecx, void* /*edx*/,
                                        IDirect3DDevice9* self,
                                        D3DPRESENT_PARAMETERS* pPP) {
    LogInfo("D3D9 Reset");
    if (g_dx9_initialized.load()) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_orig_reset(vtable_ecx, nullptr, self, pPP);
    if (SUCCEEDED(hr) && g_dx9_initialized.load()) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

// ── ResetEx hook ──────────────────────────────────────────────────────────────
static HRESULT __fastcall Hooked_ResetEx(void* vtable_ecx, void* /*edx*/,
                                          IDirect3DDevice9* self,
                                          D3DPRESENT_PARAMETERS* pPP,
                                          D3DDISPLAYMODEEX* pFullscreen) {
    LogInfo("D3D9 ResetEx");
    if (g_dx9_initialized.load()) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_orig_reset_ex(vtable_ecx, nullptr, self, pPP, pFullscreen);
    if (SUCCEEDED(hr) && g_dx9_initialized.load()) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

// ── patch device vtable at creation time ─────────────────────────────────────
// Called from Hooked_D3D9_CreateDevice and Hooked_D3D9Ex_CreateDeviceEx with
// the *actual* device the game is about to use — no more guessing vtables.
static void PatchDeviceVtable(void** vtbl, bool is_ex) {
    LogInfo("D3D9 PatchDeviceVtable: vtable={:x}", reinterpret_cast<uintptr_t>(vtbl));
    if (vtbl[kPresentIdx] != static_cast<void*>(&Hooked_Present))
        PatchSlot(vtbl, kPresentIdx, &Hooked_Present,
                  g_orig_present ? nullptr : reinterpret_cast<void**>(&g_orig_present));
    if (vtbl[kEndSceneIdx] != static_cast<void*>(&Hooked_EndScene))
        PatchSlot(vtbl, kEndSceneIdx, &Hooked_EndScene,
                  g_orig_end_scene ? nullptr : reinterpret_cast<void**>(&g_orig_end_scene));
    if (vtbl[kResetIdx] != static_cast<void*>(&Hooked_Reset))
        PatchSlot(vtbl, kResetIdx, &Hooked_Reset,
                  g_orig_reset ? nullptr : reinterpret_cast<void**>(&g_orig_reset));
    if (is_ex && vtbl[kResetExIdx] != static_cast<void*>(&Hooked_ResetEx))
        PatchSlot(vtbl, kResetExIdx, &Hooked_ResetEx,
                  g_orig_reset_ex ? nullptr : reinterpret_cast<void**>(&g_orig_reset_ex));
}

// ── factory CreateDevice hook ─────────────────────────────────────────────────
static HRESULT __fastcall Hooked_D3D9_CreateDevice(IDirect3D9* self, void* /*edx*/,
                                                    UINT adapter, D3DDEVTYPE devtype,
                                                    HWND hwnd, DWORD flags,
                                                    D3DPRESENT_PARAMETERS* pp,
                                                    IDirect3DDevice9** ppDev) {
    LogInfo("D3D9 Hooked_CreateDevice factory={:x}", reinterpret_cast<uintptr_t>(self));
    if (!self) {
        LogError("D3D9 Hooked_CreateDevice: null factory — skipping");
        return D3DERR_INVALIDCALL;
    }
    HRESULT hr = g_orig_factory_create_device(self, nullptr, adapter, devtype,
                                               hwnd, flags, pp, ppDev);
    if (SUCCEEDED(hr) && ppDev && *ppDev) {
        void** vtbl = *reinterpret_cast<void***>(*ppDev);
        LogInfo("D3D9 CreateDevice: device={:x} vtable={:x}",
                reinterpret_cast<uintptr_t>(*ppDev), reinterpret_cast<uintptr_t>(vtbl));
        PatchDeviceVtable(vtbl, false);
    }
    LogInfo("D3D9 Hooked_CreateDevice: hr={:x}", static_cast<unsigned>(hr));
    return hr;
}

// ── factory CreateDeviceEx hook ───────────────────────────────────────────────
// Calling convention: ecx = pFullscreenDisplayMode (0 for windowed); the factory
// "this" is the first STACK arg, not ecx.  See disassembly at 0x00542df5-0x00542e1f.
static HRESULT __fastcall Hooked_D3D9Ex_CreateDeviceEx(D3DDISPLAYMODEEX* pFullEcx, void* /*edx*/,
                                                         IDirect3D9Ex* self,
                                                         UINT adapter, D3DDEVTYPE devtype,
                                                         HWND hwnd, DWORD flags,
                                                         D3DPRESENT_PARAMETERS* pp,
                                                         D3DDISPLAYMODEEX* pFullscreen,
                                                         IDirect3DDevice9Ex** ppDev) {
    LogInfo("D3D9 Hooked_CreateDeviceEx factory={:x}", reinterpret_cast<uintptr_t>(self));
    HRESULT hr = g_orig_factory_create_device_ex(pFullEcx, nullptr, self, adapter, devtype,
                                                  hwnd, flags, pp, pFullscreen, ppDev);
    if (SUCCEEDED(hr) && ppDev && *ppDev) {
        void** vtbl = *reinterpret_cast<void***>(*ppDev);
        LogInfo("D3D9 CreateDeviceEx: device={:x} vtable={:x}",
                reinterpret_cast<uintptr_t>(*ppDev), reinterpret_cast<uintptr_t>(vtbl));
        PatchDeviceVtable(vtbl, true);
    }
    LogInfo("D3D9 Hooked_CreateDeviceEx: hr={:x}", static_cast<unsigned>(hr));
    return hr;
}

// ── patch factory vtable[16] (CreateDevice, non-Ex path) ─────────────────────
// Safe to call at any recursion depth — d3d9.dll does not call vtable[16]
// internally during Direct3DCreate9Ex.
static void PatchCreateDevice(IDirect3D9Ex* d3d9ex) {
    void** vtbl = *reinterpret_cast<void***>(d3d9ex);
    LogInfo("D3D9 factory vtable={:x}", reinterpret_cast<uintptr_t>(vtbl));
    if (vtbl[kFactoryCreateDeviceIdx] != static_cast<void*>(&Hooked_D3D9_CreateDevice)) {
        PatchSlot(vtbl, kFactoryCreateDeviceIdx, &Hooked_D3D9_CreateDevice,
                  reinterpret_cast<void**>(&g_orig_factory_create_device));
    }
}

// ── patch factory vtable[20] (CreateDeviceEx) ────────────────────────────────
// Must only be called once all factory-init recursion is complete (depth == 0).
// d3d9.dll calls vtable[20] internally with null ecx during factory init — if
// vtable[20] already points to our hook at that point the call stack is corrupt.
static void PatchCreateDeviceEx(IDirect3D9Ex* d3d9ex) {
    void** vtbl = *reinterpret_cast<void***>(d3d9ex);
    if (vtbl[kFactoryCreateDeviceExIdx] != static_cast<void*>(&Hooked_D3D9Ex_CreateDeviceEx)) {
        PatchSlot(vtbl, kFactoryCreateDeviceExIdx, &Hooked_D3D9Ex_CreateDeviceEx,
                  reinterpret_cast<void**>(&g_orig_factory_create_device_ex));
    }
}

// ── vtable[20] guard: restore original before calling original Direct3DCreate9Ex,
// re-patch after. Prevents d3d9.dll's internal null-self vtable[20] dispatch from
// entering our hook with a mismatched stack ABI.
static void RestoreCreateDeviceEx() {
    if (!g_factory_ptr || !g_orig_factory_create_device_ex) return;
    void** vtbl = *reinterpret_cast<void***>(g_factory_ptr);
    if (vtbl[kFactoryCreateDeviceExIdx] != static_cast<void*>(&Hooked_D3D9Ex_CreateDeviceEx)) return;
    DWORD old;
    VirtualProtect(&vtbl[kFactoryCreateDeviceExIdx], sizeof(void*), PAGE_READWRITE, &old);
    vtbl[kFactoryCreateDeviceExIdx] = reinterpret_cast<void*>(g_orig_factory_create_device_ex);
    VirtualProtect(&vtbl[kFactoryCreateDeviceExIdx], sizeof(void*), old, &old);
    LogInfo("D3D9: vtable[20] temporarily restored");
}

// ── JMP hooks on Direct3DCreate9 / Direct3DCreate9Ex ──────────────────────────
static HRESULT WINAPI Hooked_Direct3DCreate9Ex(UINT sdkVer, IDirect3D9Ex** ppD3D) {
    LogInfo("D3D9 Direct3DCreate9Ex called depth={}", g_create9ex_depth);
    ++g_create9ex_depth;
    // Always restore vtable[20] to original before entering d3d9.dll code.
    // d3d9.dll makes a recursive Direct3DCreate9Ex call internally, and after
    // the nested call returns it calls vtable[20] with null ecx.  If our hook
    // is in vtable[20] at that point the call-convention mismatch corrupts the
    // stack and crashes.  We re-patch vtable[20] only at depth==0 (outermost).
    RestoreCreateDeviceEx();
    HRESULT hr = g_orig_create9ex_fn(sdkVer, ppD3D);
    --g_create9ex_depth;
    if (SUCCEEDED(hr) && ppD3D && *ppD3D) {
        g_factory_ptr = *ppD3D;
        LogInfo("D3D9 factory={:x} depth={}", reinterpret_cast<uintptr_t>(*ppD3D), g_create9ex_depth);
        // vtable[16]: safe at any depth, d3d9 never calls it internally.
        PatchCreateDevice(*ppD3D);
        // vtable[20]: only once all factory-init recursion is done.
        if (g_create9ex_depth == 0) PatchCreateDeviceEx(*ppD3D);
    } else {
        LogInfo("D3D9 Direct3DCreate9Ex: hr={:x} ppD3D={} *ppD3D={}",
                static_cast<unsigned>(hr),
                ppD3D != nullptr,
                (ppD3D && *ppD3D) ? "non-null" : "null");
    }
    return hr;
}

static IDirect3D9* WINAPI Hooked_Direct3DCreate9(UINT sdkVer) {
    LogInfo("D3D9 Direct3DCreate9 called");
    IDirect3D9* d3d9 = g_orig_create9_fn(sdkVer);
    LogInfo("D3D9 Direct3DCreate9: factory={:x}", reinterpret_cast<uintptr_t>(d3d9));
    return d3d9;
}

// ── background thread ─────────────────────────────────────────────────────────
static void WatchThread() {
    LogInfo("D3D9 WatchThread: started");

    int ticks = 0;
    while (GetModuleHandleA("d3d9.dll") == nullptr) {
        Sleep(50);
        if (++ticks % 100 == 0) LogInfo("D3D9 WatchThread: waiting ({} ticks)", ticks);
    }
    LogInfo("D3D9 WatchThread: d3d9.dll detected");

    HMODULE mod = GetModuleHandleA("d3d9.dll");
    using namespace hooking;

    // JMP-hook Direct3DCreate9Ex
    auto* fn_ex = reinterpret_cast<uint8_t*>(GetProcAddress(mod, "Direct3DCreate9Ex"));
    if (fn_ex) {
        g_orig_create9ex_fn = reinterpret_cast<Direct3DCreate9Ex_t>(
            HookManager::Instance().SetHook(HookType::kJmpHook, fn_ex,
                                            reinterpret_cast<uint8_t*>(Hooked_Direct3DCreate9Ex)));
        LogInfo("D3D9 WatchThread: Direct3DCreate9Ex JMP-hooked ok={}", g_orig_create9ex_fn != nullptr);
    }

    // JMP-hook Direct3DCreate9
    auto* fn9 = reinterpret_cast<uint8_t*>(GetProcAddress(mod, "Direct3DCreate9"));
    if (fn9) {
        g_orig_create9_fn = reinterpret_cast<Direct3DCreate9_t>(
            HookManager::Instance().SetHook(HookType::kJmpHook, fn9,
                                            reinterpret_cast<uint8_t*>(Hooked_Direct3DCreate9)));
        LogInfo("D3D9 WatchThread: Direct3DCreate9 JMP-hooked ok={}", g_orig_create9_fn != nullptr);
    }

    // No temp factory creation here — creating a factory before the game has a window
    // leaves d3d9.dll in a state that corrupts subsequent factory calls.
    // The JMP hooks above are sufficient; Hooked_Direct3DCreate9Ex will patch the
    // factory vtable and code-hook CreateDeviceEx on the first real game call.

    LogInfo("D3D9 WatchThread: done");
}

void InitD3D9Hook() {
    std::thread(WatchThread).detach();
}
