#include "d3d9/d3d9_hook.h"

#include <Windows.h>
#include <d3d9.h>
#include <cstring>
#include <thread>

#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "bourgeon.h"
#include "imgui.h"
#include "imgui/imgui_impl_dx7.h"
#include "utils/frame_profiler.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

extern bool g_imgui_dx7_active;
extern void DrawROCursorImGui();


// ── vtable indices ─────────────────────────────────────────────────────────────
// IDirect3DDevice9 / IDirect3DDevice9Ex
static constexpr int kResetIdx    = 16;
static constexpr int kPresentIdx  = 17;
static constexpr int kEndSceneIdx = 42;
static constexpr int kResetExIdx  = 132;
static constexpr int kSetSamplerStateIdx = 69;  // IDirect3DDevice9::SetSamplerState

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
using SetSamplerState_t = HRESULT(__fastcall*)(void*, void*, IDirect3DDevice9*,
                                               DWORD, DWORD, DWORD);

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

// Device generation, bumped on every reset/recreation. Read on the render thread
// and written only from the reset/create hooks (which run on that same thread —
// the device owner), so a plain unsigned is sufficient. See Overlay_DeviceEpoch.
static unsigned g_device_epoch = 0;
unsigned Overlay_DeviceEpoch() { return g_device_epoch; }

// ── device hook originals ─────────────────────────────────────────────────────
static Reset_t    g_orig_reset     = nullptr;
static ResetEx_t  g_orig_reset_ex  = nullptr;
static EndScene_t g_orig_end_scene = nullptr;
static Present_t  g_orig_present   = nullptr;
static SetSamplerState_t g_orig_set_sampler_state = nullptr;

// Global texture-filter override: 0 = off (passthrough), 1 = force POINT
// (pixel-perfect), 2 = force LINEAR (smooth). Applied to the GAME's draws only —
// suppressed while g_in_overlay (our post-fx + ImGui passes set their own filter).
static int  g_tex_filter_mode = 0;
static bool g_in_overlay      = false;

// Captured every frame in RenderImGuiDX9 so plugins can create textures.
static IDirect3DDevice9* g_imgui_device = nullptr;

// ── factory hook originals ────────────────────────────────────────────────────
static FactoryCreateDevice_t   g_orig_factory_create_device    = nullptr;
static FactoryCreateDeviceEx_t g_orig_factory_create_device_ex = nullptr;

// ── JMP hook originals (Direct3DCreate9 / Direct3DCreate9Ex code bytes) ───────
static Direct3DCreate9Ex_t g_orig_create9ex_fn = nullptr;
static Direct3DCreate9_t   g_orig_create9_fn   = nullptr;

// Direct pointer into d3d9.dll's shared vtable array for IDirect3D9Ex.
// We store the vtable pointer (not the factory object) so that RestoreCreateDeviceEx
// stays valid even after the game releases the factory object — the vtable lives
// in d3d9.dll's .rdata section and is never freed.
static void** g_factory_vtable = nullptr;

// Nesting depth of Hooked_Direct3DCreate9Ex calls.
// d3d9.dll internally calls Direct3DCreate9Ex recursively.  We patch vtable[20]
// only when depth returns to 0 — i.e. after ALL factory-init code is done.
static int g_create9ex_depth = 0;

static std::atomic<bool> g_dx9_initialized{false};

// ── vtable slot patcher ───────────────────────────────────────────────────────
static void PatchSlot(void** vtable, int idx, void* hook, void** out_orig) {
    void** slot = &vtable[idx];
    if (out_orig) *out_orig = *slot;
    LogDebug("D3D9: vtable[{}] = {:x}", idx, reinterpret_cast<uintptr_t>(*slot));
    DWORD old;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = hook;
        VirtualProtect(slot, sizeof(void*), old, &old);
        LogDebug("D3D9: vtable[{}] patched", idx);
    } else {
        LogError("D3D9: VirtualProtect failed for vtable[{}]", idx);
    }
}

// ── ImGui render helper ───────────────────────────────────────────────────────
static void RenderImGuiDX9(IDirect3DDevice9* self) {
    g_imgui_device = self;
    if (!g_dx9_initialized.load()) {
        // LogInfo("D3D9: ImGui_ImplDX9_Init device={:x}", reinterpret_cast<uintptr_t>(self));
        ImGui_ImplDX9_Init(self);
        g_dx9_initialized.store(true);
    }
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Fenêtre minimisée dans la barre des tâches : GetClientRect renvoie 0x0 donc
    // io.DisplaySize == (0,0). Si on ouvrait une frame ImGui, chaque Begin()
    // clamperait sa fenêtre dans un viewport nul (coin haut-gauche 0,0) et cette
    // position pourrie serait mémorisée -> au retour, nos fenêtres (ex. desc item)
    // se retrouvent collées en 0,0. On saute la frame AVANT ImGui::NewFrame : aucune
    // frame ouverte => aucun Begin => aucun clamp => positions préservées.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;
    ImGui::NewFrame();
    Bourgeon::Instance().RenderUI();
    DrawROCursorImGui();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

// Sampler 0 min/mag filter for ImGui draws (POINT = crisp pixel icons, LINEAR =
// smooth). Called from an ImDrawList draw callback; restore with
// ImDrawCallback_ResetRenderState. DX9 only (no-op if no device).
void Overlay_SetTextureFilter(bool linear) {
    if (!g_imgui_device) return;
    const DWORD f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    g_imgui_device->SetSamplerState(0, D3DSAMP_MINFILTER, f);
    g_imgui_device->SetSamplerState(0, D3DSAMP_MAGFILTER, f);
}

// ImGui draw callback: switch the DX9 pipeline to ADDITIVE blend (src=ONE,
// dst=ONE) so subsequent ImDrawList primitives GLOW like RO's additive effect
// sprites. Follow the additive draws with ImDrawCallback_ResetRenderState to
// restore ImGui's normal alpha blending.
static void AdditiveBlendDrawCallback(const ImDrawList*, const ImDrawCmd*) {
    if (!g_imgui_device) return;
    g_imgui_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_imgui_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
}
void* D3D9_AdditiveBlendCallback() {
    return reinterpret_cast<void*>(&AdditiveBlendDrawCallback);
}

// ImGui draw callback: set an EXPLICIT SRCBLEND/DESTBLEND from UserCallbackData
// (low byte = src D3DBLEND, next byte = dst D3DBLEND). Replicates a .str layer's
// native per-layer blend factors (RE : record+0x18/+0x1c = SetRenderState 0x13/0x14).
// e.g. gold_shower = SRCALPHA(5)/ONE(2) -> alpha-modulated additive, no black halo.
static void ExplicitBlendDrawCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    if (!g_imgui_device || !cmd) return;
    const uintptr_t v = reinterpret_cast<uintptr_t>(cmd->UserCallbackData);
    const DWORD src = static_cast<DWORD>(v & 0xff);
    const DWORD dst = static_cast<DWORD>((v >> 8) & 0xff);
    if (!src || !dst) return;  // 0 = pas de facteur valide -> on laisse l'état courant
    g_imgui_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_imgui_device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_imgui_device->SetRenderState(D3DRS_SRCBLEND, src);
    g_imgui_device->SetRenderState(D3DRS_DESTBLEND, dst);
}
void* D3D9_ExplicitBlendCallback() {
    return reinterpret_cast<void*>(&ExplicitBlendDrawCallback);
}

// Creates a D3D9 texture from a 32-bit A8R8G8B8 buffer (w*h, tightly packed).
// D3DUSAGE_DYNAMIC + D3DPOOL_DEFAULT so it works on the client's D3D9Ex device
// (which forbids D3DPOOL_MANAGED). Returns an IDirect3DTexture9* as void*.
void* D3D9_CreateTextureARGB(const void* argb, int w, int h) {
    if (!g_imgui_device || !argb || w <= 0 || h <= 0) return nullptr;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(g_imgui_device->CreateTexture(
            static_cast<UINT>(w), static_cast<UINT>(h), 1, D3DUSAGE_DYNAMIC,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
        tex->Release();
        return nullptr;
    }
    const auto* src = static_cast<const unsigned char*>(argb);
    auto* dst = static_cast<unsigned char*>(lr.pBits);
    for (int y = 0; y < h; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * lr.Pitch,
                    src + static_cast<size_t>(y) * w * 4,
                    static_cast<size_t>(w) * 4);
    tex->UnlockRect(0);
    return tex;
}

// Re-uploads a full frame into a texture made by D3D9_CreateTextureARGB. The
// texture is D3DUSAGE_DYNAMIC, so LockRect with D3DLOCK_DISCARD avoids stalling
// the GPU (fresh memory each time — the whole surface must be rewritten).
bool D3D9_UpdateTextureARGB(void* tex, const void* argb, int w, int h) {
    if (!tex || !argb || w <= 0 || h <= 0) return false;
    auto* t = static_cast<IDirect3DTexture9*>(tex);
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) return false;
    const auto* src = static_cast<const unsigned char*>(argb);
    auto* dst = static_cast<unsigned char*>(lr.pBits);
    for (int y = 0; y < h; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * lr.Pitch,
                    src + static_cast<size_t>(y) * w * 4,
                    static_cast<size_t>(w) * 4);
    t->UnlockRect(0);
    return true;
}

// Composites textured quads onto a fresh transparent render target and reads the
// pixels back (avatar GIF export). Called during the overlay pass (inside a scene),
// so it saves/restores the render target + viewport + all render state around the
// draw. Creates the RT + a system-memory readback surface per call (a handful of
// small frames per export — not hot). DX9 only.
bool D3D9_CompositeQuadsRGBA(const D3D9TexQuad* quads, int n, int w, int h,
                             void* out_argb) {
    if (g_imgui_dx7_active) return false;
    IDirect3DDevice9* dev = g_imgui_device;
    if (!dev || !out_argb || w <= 0 || h <= 0) return false;

    IDirect3DTexture9* rt = nullptr;
    if (FAILED(dev->CreateTexture(static_cast<UINT>(w), static_cast<UINT>(h), 1,
                                  D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                  D3DPOOL_DEFAULT, &rt, nullptr)))
        return false;
    IDirect3DSurface9* rtSurf = nullptr;
    IDirect3DSurface9* sysSurf = nullptr;
    rt->GetSurfaceLevel(0, &rtSurf);
    dev->CreateOffscreenPlainSurface(static_cast<UINT>(w), static_cast<UINT>(h),
                                     D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysSurf,
                                     nullptr);

    IDirect3DSurface9* prevRT = nullptr;
    D3DVIEWPORT9 prevVp{};
    dev->GetRenderTarget(0, &prevRT);
    dev->GetViewport(&prevVp);
    IDirect3DStateBlock9* sb = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &sb);  // restored after the draw

    bool ok = false;
    if (rtSurf && sysSurf && SUCCEEDED(dev->SetRenderTarget(0, rtSurf))) {
        D3DVIEWPORT9 vp{0, 0, static_cast<DWORD>(w), static_cast<DWORD>(h), 0.0f, 1.0f};
        dev->SetViewport(&vp);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

        dev->SetPixelShader(nullptr);
        dev->SetVertexShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        // Accumulate coverage into the target alpha (transparent bg -> cut-out).
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        struct V { float x, y, z, rhw; DWORD c; float u, v; };
        for (int i = 0; i < n; ++i) {
            const D3D9TexQuad& q = quads[i];
            if (!q.tex) continue;
            dev->SetTexture(0, static_cast<IDirect3DBaseTexture9*>(q.tex));
            const float o = -0.5f;  // pixel-center rasterisation offset
            const V vtx[4] = {
                {q.x0 + o, q.y0 + o, 0.0f, 1.0f, 0xFFFFFFFFu, q.u0, q.v0},
                {q.x1 + o, q.y0 + o, 0.0f, 1.0f, 0xFFFFFFFFu, q.u1, q.v0},
                {q.x0 + o, q.y1 + o, 0.0f, 1.0f, 0xFFFFFFFFu, q.u0, q.v1},
                {q.x1 + o, q.y1 + o, 0.0f, 1.0f, 0xFFFFFFFFu, q.u1, q.v1},
            };
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vtx, sizeof(V));
        }

        if (SUCCEEDED(dev->GetRenderTargetData(rtSurf, sysSurf))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(sysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                auto* dst = static_cast<unsigned char*>(out_argb);
                const auto* src = static_cast<const unsigned char*>(lr.pBits);
                for (int y = 0; y < h; ++y)
                    std::memcpy(dst + static_cast<size_t>(y) * w * 4,
                                src + static_cast<size_t>(y) * lr.Pitch,
                                static_cast<size_t>(w) * 4);
                sysSurf->UnlockRect();
                ok = true;
            }
        }
    }

    dev->SetRenderTarget(0, prevRT);
    dev->SetViewport(&prevVp);
    if (sb) { sb->Apply(); sb->Release(); }
    if (prevRT) prevRT->Release();
    if (sysSurf) sysSurf->Release();
    if (rtSurf) rtSurf->Release();
    if (rt) rt->Release();
    return ok;
}

// Picks the texture upload path for the renderer the client is actually running:
// the DX7 proxy (Proxy_EndScene) sets g_imgui_dx7_active, in which case ImGui's
// ImTextureID must be a DirectDraw surface, not a D3D9 texture.
void* Overlay_CreateTextureARGB(const void* argb, int w, int h) {
    if (g_imgui_dx7_active) return DX7_CreateTextureARGB(argb, w, h);
    return D3D9_CreateTextureARGB(argb, w, h);
}

// ── Post-processing pipeline ──────────────────────────────────────────────────
// Fullscreen post effects on the game's rendered frame, applied in Present before
// the overlay. The client renders straight to the default backbuffer (X8R8G8B8,
// no MSAA — confirmed via RE of the device init at 0x00542c20 / 0x00542830), so
// each pass StretchRects the backbuffer into a temp render-target texture and
// re-draws it through a ps_3_0 shader. Pass 1 = colour/effects (grade, temp,
// filter, vignette, grain, chromatic aberration, sharpen); pass 2 = optional
// FXAA (needs neighbour taps of the already-graded image, hence a 2nd pass).
//
// SwapEffect is DISCARD, so this MUST run before Present; SetGammaRamp is ignored
// on D3D9Ex windowed, which is why shader passes are used instead.

static D3D9PostFx g_fx;  // current params (set via D3D9_SetPostFx)

static IDirect3DPixelShader9* g_fx_color_ps = nullptr;
static IDirect3DPixelShader9* g_fx_fxaa_ps  = nullptr;
static bool                   g_fx_tried     = false;  // compiled (or failed) once
static IDirect3DTexture9*     g_fx_tex      = nullptr; // scene copy (RT)
static IDirect3DSurface9*     g_fx_tex_surf = nullptr; // its surface 0
static UINT                   g_fx_w = 0, g_fx_h = 0;

// Pending screenshot (captured in Present, before the overlay).
static volatile bool g_shot_pending = false;
static char          g_shot_path[512] = {0};

// Pass 1: colour + screen-space effects. ps_3_0 (uniform branching on params).
// c0=(bright,contrast,invGamma,sat) c1=(temp,filter,vignette,grain)
// c2=(aberration,sharpen,time,_) c3=(texelW,texelH,_,_)
static const char kFxColorHLSL[] =
    "sampler2D s0:register(s0);\n"
    "float4 p0:register(c0);float4 p1:register(c1);float4 p2:register(c2);float4 p3:register(c3);\n"
    "float4 main(float2 uv:TEXCOORD0):COLOR0{\n"
    " float2 tx=p3.xy; float3 c;\n"
    " if(p2.x>0.0){float2 o=(uv-0.5)*p2.x*0.01; c.r=tex2D(s0,uv+o).r; c.g=tex2D(s0,uv).g; c.b=tex2D(s0,uv-o).b;}\n"
    " else { c=tex2D(s0,uv).rgb; }\n"
    " if(p2.y>0.0){float3 b=tex2D(s0,uv+float2(tx.x,0)).rgb+tex2D(s0,uv-float2(tx.x,0)).rgb+tex2D(s0,uv+float2(0,tx.y)).rgb+tex2D(s0,uv-float2(0,tx.y)).rgb; b*=0.25; c+=(c-b)*p2.y*2.0;}\n"
    " c=(c-0.5)*p0.y+0.5; c+=p0.x; c=pow(saturate(c),p0.z);\n"
    " float l=dot(c,float3(0.299,0.587,0.114)); c=lerp(float3(l,l,l),c,p0.w);\n"
    " c.r+=p1.x*0.10; c.b-=p1.x*0.10;\n"
    " if(p1.y>0.5&&p1.y<1.5){float g=dot(c,float3(0.299,0.587,0.114));c=float3(g,g,g);}\n"
    " else if(p1.y>1.5&&p1.y<2.5){float g=dot(c,float3(0.299,0.587,0.114));c=saturate(g*float3(1.07,0.74,0.43));}\n"
    " else if(p1.y>2.5&&p1.y<3.5){c=1.0-c;}\n"
    " else if(p1.y>3.5){c=saturate(c*float3(1.05,0.90,1.10));}\n"
    " if(p1.z>0.0){float d=distance(uv,float2(0.5,0.5)); float v=smoothstep(0.8,0.35,d); c*=lerp(1.0,v,p1.z);}\n"
    " if(p1.w>0.0){float n=frac(sin(dot(uv*(p2.z+1.0),float2(12.9898,78.233)))*43758.5453); c+=(n-0.5)*p1.w*0.15;}\n"
    " return float4(saturate(c),1.0);\n"
    "}\n";

// Pass 2: compact NVIDIA-style FXAA. c0=(texelW,texelH,_,_) c1=(strength,_,_,_).
// An edge-contrast threshold skips flat areas, and the result is blended back by
// `strength` so high-contrast UI text (chat/windows) keeps most of its sharpness.
static const char kFxFxaaHLSL[] =
    "sampler2D s0:register(s0); float4 rc:register(c0); float4 fp:register(c1);\n"
    "float4 main(float2 uv:TEXCOORD0):COLOR0{\n"
    " float2 r=rc.xy; float3 L=float3(0.299,0.587,0.114);\n"
    " float3 nw=tex2D(s0,uv+float2(-1,-1)*r).rgb, ne=tex2D(s0,uv+float2(1,-1)*r).rgb;\n"
    " float3 sw=tex2D(s0,uv+float2(-1,1)*r).rgb, se=tex2D(s0,uv+float2(1,1)*r).rgb, m=tex2D(s0,uv).rgb;\n"
    " float lnw=dot(nw,L),lne=dot(ne,L),lsw=dot(sw,L),lse=dot(se,L),lm=dot(m,L);\n"
    " float lmin=min(lm,min(min(lnw,lne),min(lsw,lse))), lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));\n"
    " if(lmax-lmin < max(1.0/16.0, lmax*0.125)) return float4(m,1.0);\n"  // skip flat areas
    " float2 d; d.x=-((lnw+lne)-(lsw+lse)); d.y=((lnw+lsw)-(lne+lse));\n"
    " float dr=max((lnw+lne+lsw+lse)*0.25*0.125,1.0/128.0);\n"
    " float rm=1.0/(min(abs(d.x),abs(d.y))+dr); d=clamp(d*rm,-8.0,8.0)*r;\n"
    " float3 a=0.5*(tex2D(s0,uv+d*(1.0/3.0-0.5)).rgb+tex2D(s0,uv+d*(2.0/3.0-0.5)).rgb);\n"
    " float3 b=a*0.5+0.25*(tex2D(s0,uv+d*-0.5).rgb+tex2D(s0,uv+d*0.5).rgb);\n"
    " float lb=dot(b,L); float3 res=(lb<lmin||lb>lmax)?a:b;\n"
    " return float4(lerp(m,res,fp.x),1.0);\n"  // blend by strength -> readable text
    "}\n";

// Minimal ID3DXBuffer (avoids a build-time dependency on the d3dx9 SDK headers;
// d3dx9_43.dll itself is shipped with and loaded by the client). __stdcall to
// match the COM ABI of the real interface.
struct ID3DXBufferMin {
    virtual HRESULT __stdcall QueryInterface(const IID&, void**) = 0;
    virtual ULONG   __stdcall AddRef() = 0;
    virtual ULONG   __stdcall Release() = 0;
    virtual void*   __stdcall GetBufferPointer() = 0;
    virtual DWORD   __stdcall GetBufferSize() = 0;
};
using D3DXCompileShader_t = HRESULT(WINAPI*)(
    const char* src, UINT len, const void* defines, void* include,
    const char* entry, const char* profile, DWORD flags,
    ID3DXBufferMin** shader, ID3DXBufferMin** errs, void* constTable);
// D3DXSaveSurfaceToFileA(file, D3DXIFF_PNG=3, surface, palette, rect).
using D3DXSaveSurfaceToFile_t = HRESULT(WINAPI*)(
    const char*, int, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);

static HMODULE D3dx9() {
    HMODULE m = GetModuleHandleA("d3dx9_43.dll");
    if (!m) m = LoadLibraryA("d3dx9_43.dll");
    return m;
}

static void PostFx_ReleaseRT() {
    if (g_fx_tex_surf) { g_fx_tex_surf->Release(); g_fx_tex_surf = nullptr; }
    if (g_fx_tex)      { g_fx_tex->Release();      g_fx_tex = nullptr; }
    g_fx_w = g_fx_h = 0;
}

// Releases D3DPOOL_DEFAULT resources before a device Reset. Pixel shaders are not
// pool-bound and survive a SAME-device reset.
static void PostFx_OnDeviceLost() { PostFx_ReleaseRT(); }

// Full teardown for a device RECREATION (CreateDevice / CreateDeviceEx): unlike a
// same-device Reset, the pixel shaders ALSO belong to the destroyed old device, so
// they must be released and g_fx_tried cleared to force a recompile on the new
// device. Without this, PostFx_EnsureShaders early-returns dangling shaders and
// PostFx_EnsureRT reuses/Release()s a dead surface -> crash at the next Present
// when post-processing is enabled (a TDR/device-removed recreation is exactly the
// path a GPU-contention crash takes). We still hold refs to these objects, so
// Release() here is safe COM refcounting (it lets the old device finally die).
static void PostFx_OnDeviceRecreated() {
    PostFx_ReleaseRT();
    if (g_fx_color_ps) { g_fx_color_ps->Release(); g_fx_color_ps = nullptr; }
    if (g_fx_fxaa_ps)  { g_fx_fxaa_ps->Release();  g_fx_fxaa_ps = nullptr; }
    g_fx_tried = false;
}

static IDirect3DPixelShader9* CompilePs(IDirect3DDevice9* dev, const char* src,
                                        UINT len, D3DXCompileShader_t compile) {
    ID3DXBufferMin* code = nullptr;
    ID3DXBufferMin* errs = nullptr;
    HRESULT hr = compile(src, len, nullptr, nullptr, "main", "ps_3_0", 0,
                         &code, &errs, nullptr);
    if (errs) errs->Release();
    if (FAILED(hr) || !code) {
        LogError("PostFx: shader compile failed hr={:x}", static_cast<unsigned>(hr));
        if (code) code->Release();
        return nullptr;
    }
    IDirect3DPixelShader9* ps = nullptr;
    hr = dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps);
    code->Release();
    if (FAILED(hr)) { LogError("PostFx: CreatePixelShader hr={:x}",
                               static_cast<unsigned>(hr)); return nullptr; }
    return ps;
}

static bool PostFx_EnsureShaders(IDirect3DDevice9* dev) {
    if (g_fx_color_ps && g_fx_fxaa_ps) return true;
    if (g_fx_tried) return g_fx_color_ps != nullptr;
    g_fx_tried = true;
    HMODULE d3dx = D3dx9();
    if (!d3dx) { LogError("PostFx: d3dx9_43.dll not available"); return false; }
    auto compile = reinterpret_cast<D3DXCompileShader_t>(
        GetProcAddress(d3dx, "D3DXCompileShader"));
    if (!compile) { LogError("PostFx: D3DXCompileShader missing"); return false; }
    g_fx_color_ps = CompilePs(dev, kFxColorHLSL, sizeof(kFxColorHLSL) - 1, compile);
    g_fx_fxaa_ps  = CompilePs(dev, kFxFxaaHLSL,  sizeof(kFxFxaaHLSL)  - 1, compile);
    if (g_fx_color_ps) /* LogInfo("PostFx: shaders ready (fxaa={})", g_fx_fxaa_ps != nullptr) */;
    return g_fx_color_ps != nullptr;
}

static bool PostFx_EnsureRT(IDirect3DDevice9* dev, UINT w, UINT h) {
    if (g_fx_tex && g_fx_w == w && g_fx_h == h) return true;
    PostFx_ReleaseRT();
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                  D3DPOOL_DEFAULT, &g_fx_tex, nullptr)))
        return false;
    if (FAILED(g_fx_tex->GetSurfaceLevel(0, &g_fx_tex_surf))) {
        PostFx_ReleaseRT();
        return false;
    }
    g_fx_w = w; g_fx_h = h;
    return true;
}

// Common device state for a fullscreen post-process quad. Wrap calls in a
// D3DSBT_ALL state block (saved before, applied after) so the game/UI are intact.
static void PostFx_SetCommonState(IDirect3DDevice9* dev) {
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

// Copies the current backbuffer into g_fx_tex and draws a fullscreen quad through
// `ps` (constants must already be set). Returns false on a copy failure.
static bool PostFx_RunPass(IDirect3DDevice9* dev, IDirect3DSurface9* back,
                           float w, float h, IDirect3DPixelShader9* ps) {
    if (FAILED(dev->StretchRect(back, nullptr, g_fx_tex_surf, nullptr, D3DTEXF_NONE)))
        return false;
    dev->SetTexture(0, g_fx_tex);
    dev->SetPixelShader(ps);
    struct V { float x, y, z, rhw, u, v; };
    const V quad[4] = {  // half-texel offset for correct texel alignment
        { -0.5f,     -0.5f,     0.0f, 1.0f, 0.0f, 0.0f },
        { w - 0.5f,  -0.5f,     0.0f, 1.0f, 1.0f, 0.0f },
        { -0.5f,     h - 0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
        { w - 0.5f,  h - 0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    return true;
}

// Must be called inside an open scene, with the backbuffer as the render target.
static void PostFx_Apply(IDirect3DDevice9* dev) {
    if (!g_fx.enabled) return;
    IDirect3DSurface9* back = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &back)) || !back) return;
    D3DSURFACE_DESC desc;
    if (FAILED(back->GetDesc(&desc)) || !PostFx_EnsureShaders(dev) ||
        !PostFx_EnsureRT(dev, desc.Width, desc.Height)) {
        back->Release();
        return;
    }
    const float w = static_cast<float>(desc.Width);
    const float h = static_cast<float>(desc.Height);
    const float tw = 1.0f / w, th = 1.0f / h;

    IDirect3DStateBlock9* sb = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &sb);
    PostFx_SetCommonState(dev);

    // Pass 1: colour + effects.
    float gamma = g_fx.gamma < 0.1f ? 0.1f : g_fx.gamma;
    const float c0[4] = {g_fx.brightness, g_fx.contrast, 1.0f / gamma, g_fx.saturation};
    const float c1[4] = {g_fx.temperature, static_cast<float>(g_fx.filter),
                         g_fx.vignette, g_fx.grain};
    const float t = static_cast<float>(GetTickCount() & 0xFFFF) * 0.001f;
    const float c2[4] = {g_fx.aberration, g_fx.sharpen, t, 0.0f};
    const float c3[4] = {tw, th, 0.0f, 0.0f};
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetPixelShaderConstantF(2, c2, 1);
    dev->SetPixelShaderConstantF(3, c3, 1);
    PostFx_RunPass(dev, back, w, h, g_fx_color_ps);

    // Pass 2: FXAA on the composited frame (world + UI). This client's renderer is
    // DEFERRED — at the UI-render hook the backbuffer holds only the flat clear
    // colour (verified by a uniform-blue dump there), so the world rasterises with
    // the UI at the final flush and there is NO "world without UI" moment to AA
    // separately. FXAA therefore runs here, full-screen; the strength slider keeps
    // high-contrast UI text readable.
    if (g_fx.fxaa && g_fx_fxaa_ps) {
        const float fx1[4] = {g_fx.fxaa_strength, 0.0f, 0.0f, 0.0f};
        dev->SetPixelShaderConstantF(0, c3, 1);   // c0 = (texelW, texelH, ...)
        dev->SetPixelShaderConstantF(1, fx1, 1);  // c1 = (strength, ...)
        PostFx_RunPass(dev, back, w, h, g_fx_fxaa_ps);
    }

    if (sb) { sb->Apply(); sb->Release(); }
    back->Release();
}

// Saves the current backbuffer to the pending PNG path (called in Present after
// PostFx_Apply, before the overlay, so the shot has no Bourgeon UI).
static void PostFx_CaptureIfRequested(IDirect3DDevice9* dev) {
    if (!g_shot_pending) return;
    g_shot_pending = false;
    HMODULE d3dx = D3dx9();
    if (!d3dx) return;
    auto save = reinterpret_cast<D3DXSaveSurfaceToFile_t>(
        GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA"));
    if (!save) { LogError("Screenshot: D3DXSaveSurfaceToFileA missing"); return; }
    IDirect3DSurface9* back = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &back)) || !back) return;
    HRESULT hr = save(g_shot_path, /*D3DXIFF_PNG*/3, back, nullptr, nullptr);
    back->Release();
    if (SUCCEEDED(hr)) /* LogInfo("Screenshot saved: {}", g_shot_path) */;
    else LogError("Screenshot failed hr={:x}", static_cast<unsigned>(hr));
}

void D3D9_SetPostFx(const D3D9PostFx& fx) {
    g_fx = fx;
    // Hard cap: full-screen FXAA above 0.5 makes UI text illegible.
    if (g_fx.fxaa_strength > 0.5f) g_fx.fxaa_strength = 0.5f;
    if (g_fx.fxaa_strength < 0.0f) g_fx.fxaa_strength = 0.0f;
}

void D3D9_RequestScreenshot(const char* filepath) {
    if (!filepath) return;
    strncpy_s(g_shot_path, sizeof(g_shot_path), filepath, _TRUNCATE);
    g_shot_pending = true;
}

// ── EndScene hook ─────────────────────────────────────────────────────────────
// ecx = vtable (*device); self = device (first explicit stack arg).
static HRESULT __fastcall Hooked_EndScene(void* vtable_ecx, void* /*edx*/,
                                           IDirect3DDevice9* self) {
    return g_orig_end_scene(vtable_ecx, nullptr, self);
}

// ── SetSamplerState hook (global texture filter override) ──────────────────────
// Forces the game's min/mag texture filter to POINT (crisp pixel-art) or LINEAR
// (smooth). Skipped while g_in_overlay so our post-fx / ImGui keep their own
// filter. ecx = vtable; self = device (first explicit stack arg).
static HRESULT __fastcall Hooked_SetSamplerState(void* vtable_ecx, void* /*edx*/,
                                                  IDirect3DDevice9* self,
                                                  DWORD sampler, DWORD type, DWORD value) {
    if (type == D3DSAMP_MINFILTER || type == D3DSAMP_MAGFILTER) {
        // One-shot diag: confirms the game DOES drive texture filtering through
        // SetSamplerState (so the override path is reachable). If this never logs,
        // the game uses another mechanism and the filter feature can't work as-is.
        static bool seen = false;
        if (!seen) { seen = true; /* LogInfo("D3D9: game SetSamplerState MIN/MAG seen (filter hook reachable)"); */ }
        if (!g_in_overlay && g_tex_filter_mode)
            value = (g_tex_filter_mode == 1) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    }
    return g_orig_set_sampler_state(vtable_ecx, nullptr, self, sampler, type, value);
}

void D3D9_SetTextureFilter(int mode) { g_tex_filter_mode = mode; }

// ── Present hook ──────────────────────────────────────────────────────────────
// ecx = vtable; self = device (first explicit stack arg).
// ImGui is rendered here — after ALL of the game's BeginScene/EndScene passes —
// in a dedicated scene so it is never baked into an intermediate backbuffer that
// a later game pass would read and composite (which caused ghost/double artifacts
// when the equipment window or other overlay windows were open).
static HRESULT __fastcall Hooked_Present(void* vtable_ecx, void* /*edx*/,
                                          IDirect3DDevice9* self,
                                          const RECT* pSrcRect, const RECT* pDestRect,
                                          HWND hDestWindowOverride,
                                          const RGNDATA* pDirtyRegion) {
    // Chronométrage du temps de frame. En mode DX7 c'est Proxy_EndScene qui tient
    // le compteur — ticker ici aussi compterait la frame deux fois.
    if (!g_imgui_dx7_active) FrameProfiler_Tick();
    if (!g_imgui_dx7_active && ImGui::GetCurrentContext()) {
        // BeginScene is not hooked, so calling through the vtable is safe.
        if (SUCCEEDED(self->BeginScene())) {
            // Post-process the game's frame first; capture a clean screenshot (no
            // overlay) here too, then draw the overlay on top. g_in_overlay tells
            // the SetSamplerState hook to leave our passes' filters alone.
            g_in_overlay = true;
            PostFx_Apply(self);
            PostFx_CaptureIfRequested(self);
            RenderImGuiDX9(self);
            g_in_overlay = false;
            // Call the original EndScene directly to avoid re-entering our hook.
            g_orig_end_scene(vtable_ecx, nullptr, self);
        }
    }
    return g_orig_present(vtable_ecx, nullptr, self, pSrcRect, pDestRect,
                          hDestWindowOverride, pDirtyRegion);
}

// ── Reset hook ────────────────────────────────────────────────────────────────
static HRESULT __fastcall Hooked_Reset(void* vtable_ecx, void* /*edx*/,
                                        IDirect3DDevice9* self,
                                        D3DPRESENT_PARAMETERS* pPP) {
    // LogInfo("D3D9 Reset");
    ++g_device_epoch;       // invalidate plugin texture caches (D3DPOOL_DEFAULT dies)
    PostFx_OnDeviceLost();  // free D3DPOOL_DEFAULT scene-copy RT before reset
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
    // LogInfo("D3D9 ResetEx");
    ++g_device_epoch;       // invalidate plugin texture caches (D3DPOOL_DEFAULT dies)
    PostFx_OnDeviceLost();  // free D3DPOOL_DEFAULT scene-copy RT before reset
    if (g_dx9_initialized.load()) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_orig_reset_ex(vtable_ecx, nullptr, self, pPP, pFullscreen);
    if (SUCCEEDED(hr) && g_dx9_initialized.load()) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

// ── patch device vtable at creation time ─────────────────────────────────────
// Called from Hooked_D3D9_CreateDevice and Hooked_D3D9Ex_CreateDeviceEx with
// the *actual* device the game is about to use — no more guessing vtables.
static void PatchDeviceVtable(void** vtbl, bool is_ex) {
    LogDebug("D3D9 PatchDeviceVtable: vtable={:x}", reinterpret_cast<uintptr_t>(vtbl));
    if (vtbl[kPresentIdx] != static_cast<void*>(&Hooked_Present))
        PatchSlot(vtbl, kPresentIdx, &Hooked_Present,
                  g_orig_present ? nullptr : reinterpret_cast<void**>(&g_orig_present));
    if (vtbl[kEndSceneIdx] != static_cast<void*>(&Hooked_EndScene))
        PatchSlot(vtbl, kEndSceneIdx, &Hooked_EndScene,
                  g_orig_end_scene ? nullptr : reinterpret_cast<void**>(&g_orig_end_scene));
    if (vtbl[kResetIdx] != static_cast<void*>(&Hooked_Reset))
        PatchSlot(vtbl, kResetIdx, &Hooked_Reset,
                  g_orig_reset ? nullptr : reinterpret_cast<void**>(&g_orig_reset));
    if (vtbl[kSetSamplerStateIdx] != static_cast<void*>(&Hooked_SetSamplerState))
        PatchSlot(vtbl, kSetSamplerStateIdx, &Hooked_SetSamplerState,
                  g_orig_set_sampler_state ? nullptr
                                           : reinterpret_cast<void**>(&g_orig_set_sampler_state));
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
    LogDebug("D3D9 Hooked_CreateDevice factory={:x}", reinterpret_cast<uintptr_t>(self));
    if (!self) {
        LogError("D3D9 Hooked_CreateDevice: null factory — skipping");
        return D3DERR_INVALIDCALL;
    }
    HRESULT hr = g_orig_factory_create_device(self, nullptr, adapter, devtype,
                                               hwnd, flags, pp, ppDev);
    if (SUCCEEDED(hr) && ppDev && *ppDev) {
        if (g_dx9_initialized.load()) {
            // LogInfo("D3D9: device recreated via CreateDevice — resetting ImGui");
            ++g_device_epoch;  // invalidate plugin texture caches (old device dies)
            PostFx_OnDeviceRecreated();  // RT + shaders belong to the dead device
            ImGui_ImplDX9_InvalidateDeviceObjects();
            g_dx9_initialized.store(false);
        }
        void** vtbl = *reinterpret_cast<void***>(*ppDev);
        LogDebug("D3D9 CreateDevice: device={:x} vtable={:x}",
                reinterpret_cast<uintptr_t>(*ppDev), reinterpret_cast<uintptr_t>(vtbl));
        PatchDeviceVtable(vtbl, false);
    }
    LogDebug("D3D9 Hooked_CreateDevice: hr={:x}", static_cast<unsigned>(hr));
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
    LogDebug("D3D9 Hooked_CreateDeviceEx factory={:x}", reinterpret_cast<uintptr_t>(self));
    HRESULT hr = g_orig_factory_create_device_ex(pFullEcx, nullptr, self, adapter, devtype,
                                                  hwnd, flags, pp, pFullscreen, ppDev);
    if (SUCCEEDED(hr) && ppDev && *ppDev) {
        if (g_dx9_initialized.load()) {
            // LogInfo("D3D9: device recreated via CreateDeviceEx — resetting ImGui");
            ++g_device_epoch;  // invalidate plugin texture caches (old device dies)
            PostFx_OnDeviceRecreated();  // RT + shaders belong to the dead device
            ImGui_ImplDX9_InvalidateDeviceObjects();
            g_dx9_initialized.store(false);
        }
        void** vtbl = *reinterpret_cast<void***>(*ppDev);
        LogDebug("D3D9 CreateDeviceEx: device={:x} vtable={:x}",
                reinterpret_cast<uintptr_t>(*ppDev), reinterpret_cast<uintptr_t>(vtbl));
        PatchDeviceVtable(vtbl, true);
    }
    LogDebug("D3D9 Hooked_CreateDeviceEx: hr={:x}", static_cast<unsigned>(hr));
    return hr;
}

// ── patch factory vtable[16] (CreateDevice, non-Ex path) ─────────────────────
// Safe to call at any recursion depth — d3d9.dll does not call vtable[16]
// internally during Direct3DCreate9Ex.
static void PatchCreateDevice(IDirect3D9Ex* d3d9ex) {
    void** vtbl = *reinterpret_cast<void***>(d3d9ex);
    LogDebug("D3D9 factory vtable={:x}", reinterpret_cast<uintptr_t>(vtbl));
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
    if (!g_factory_vtable || !g_orig_factory_create_device_ex) return;
    if (g_factory_vtable[kFactoryCreateDeviceExIdx] != static_cast<void*>(&Hooked_D3D9Ex_CreateDeviceEx)) return;
    DWORD old;
    VirtualProtect(&g_factory_vtable[kFactoryCreateDeviceExIdx], sizeof(void*), PAGE_READWRITE, &old);
    g_factory_vtable[kFactoryCreateDeviceExIdx] = reinterpret_cast<void*>(g_orig_factory_create_device_ex);
    VirtualProtect(&g_factory_vtable[kFactoryCreateDeviceExIdx], sizeof(void*), old, &old);
    LogDebug("D3D9: vtable[20] temporarily restored");
}

// ── JMP hooks on Direct3DCreate9 / Direct3DCreate9Ex ──────────────────────────
static HRESULT WINAPI Hooked_Direct3DCreate9Ex(UINT sdkVer, IDirect3D9Ex** ppD3D) {
    LogDebug("D3D9 Direct3DCreate9Ex called depth={}", g_create9ex_depth);
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
        // Save the vtable pointer (in d3d9.dll .rdata, always valid) rather than
        // the factory object pointer (which the game may Release before the next call).
        g_factory_vtable = *reinterpret_cast<void***>(*ppD3D);
        LogDebug("D3D9 factory={:x} depth={}", reinterpret_cast<uintptr_t>(*ppD3D), g_create9ex_depth);
        // vtable[16]: safe at any depth, d3d9 never calls it internally.
        PatchCreateDevice(*ppD3D);
        // vtable[20]: only once all factory-init recursion is done.
        if (g_create9ex_depth == 0) PatchCreateDeviceEx(*ppD3D);
    } else {
        LogDebug("D3D9 Direct3DCreate9Ex: hr={:x} ppD3D={} *ppD3D={}",
                static_cast<unsigned>(hr),
                ppD3D != nullptr,
                (ppD3D && *ppD3D) ? "non-null" : "null");
    }
    return hr;
}

static IDirect3D9* WINAPI Hooked_Direct3DCreate9(UINT sdkVer) {
    LogDebug("D3D9 Direct3DCreate9 called");
    IDirect3D9* d3d9 = g_orig_create9_fn(sdkVer);
    LogDebug("D3D9 Direct3DCreate9: factory={:x}", reinterpret_cast<uintptr_t>(d3d9));
    return d3d9;
}

// ── background thread ─────────────────────────────────────────────────────────
static void WatchThread() {
    LogDebug("D3D9 WatchThread: started");

    int ticks = 0;
    while (GetModuleHandleA("d3d9.dll") == nullptr) {
        Sleep(50);
        if (++ticks % 100 == 0) LogDebug("D3D9 WatchThread: waiting ({} ticks)", ticks);
    }
    LogDebug("D3D9 WatchThread: d3d9.dll detected");

    HMODULE mod = GetModuleHandleA("d3d9.dll");
    using namespace hooking;

    // JMP-hook Direct3DCreate9Ex
    auto* fn_ex = reinterpret_cast<uint8_t*>(GetProcAddress(mod, "Direct3DCreate9Ex"));
    if (fn_ex) {
        g_orig_create9ex_fn = reinterpret_cast<Direct3DCreate9Ex_t>(
            HookManager::Instance().SetHook(HookType::kJmpHook, fn_ex,
                                            reinterpret_cast<uint8_t*>(Hooked_Direct3DCreate9Ex)));
        LogDebug("D3D9 WatchThread: Direct3DCreate9Ex JMP-hooked ok={}", g_orig_create9ex_fn != nullptr);
    }

    // JMP-hook Direct3DCreate9
    auto* fn9 = reinterpret_cast<uint8_t*>(GetProcAddress(mod, "Direct3DCreate9"));
    if (fn9) {
        g_orig_create9_fn = reinterpret_cast<Direct3DCreate9_t>(
            HookManager::Instance().SetHook(HookType::kJmpHook, fn9,
                                            reinterpret_cast<uint8_t*>(Hooked_Direct3DCreate9)));
        LogDebug("D3D9 WatchThread: Direct3DCreate9 JMP-hooked ok={}", g_orig_create9_fn != nullptr);
    }

    // No temp factory creation here — creating a factory before the game has a window
    // leaves d3d9.dll in a state that corrupts subsequent factory calls.
    // The JMP hooks above are sufficient; Hooked_Direct3DCreate9Ex will patch the
    // factory vtable and code-hook CreateDeviceEx on the first real game call.

    LogDebug("D3D9 WatchThread: done");
}

void InitD3D9Hook() {
    std::thread(WatchThread).detach();
}
