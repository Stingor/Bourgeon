#pragma once

void InitD3D9Hook();

// Creates a D3D9 texture from a 32-bit A8R8G8B8 pixel buffer (w*h, tightly
// packed) using the game's render device, for use as an ImGui ImTextureID.
// Returns nullptr until the device has been captured (first rendered frame).
// Caller owns the returned IDirect3DTexture9* (Release on shutdown / device loss).
void* D3D9_CreateTextureARGB(const void* argb, int w, int h);

// Re-uploads a full 32-bit A8R8G8B8 pixel buffer (w*h, tightly packed) into a
// texture previously created by D3D9_CreateTextureARGB (D3DUSAGE_DYNAMIC, so a
// D3DLOCK_DISCARD lock is cheap). w/h must match the creation size. Returns
// false on lock failure (caller may Release + recreate). DX9 only.
bool D3D9_UpdateTextureARGB(void* tex, const void* argb, int w, int h);

// Renderer-agnostic ARGB→texture upload: routes to the DX7 (DirectDraw surface)
// or D3D9 helper depending on which renderer the client is running. Plugins that
// draw textured ImGui content (e.g. menu icons) MUST use this so they work in
// both modes. Returns an ImTextureID-compatible void* (or nullptr).
void* Overlay_CreateTextureARGB(const void* argb, int w, int h);

// Releases a texture made by Overlay_CreateTextureARGB. Both backends return an
// IUnknown-derived object (IDirect3DTexture9 / IDirectDrawSurface7), so one entry
// point covers them. Call this when a CACHE drops an entry while the device is
// still alive — a plugin that browses many sprites would otherwise leak a texture
// per image it ever showed. Do NOT call it on entries dropped because
// Overlay_DeviceEpoch() changed: those belong to a device that is already gone.
void Overlay_ReleaseTexture(void* tex);

// Monotonic counter bumped every time the D3D9 device is reset or recreated
// (Reset / ResetEx / CreateDevice / CreateDeviceEx). Textures made via
// Overlay_CreateTextureARGB live in D3DPOOL_DEFAULT and belong to a specific
// device — after a reset/recreation (alt-tab, resolution change, or a TDR from
// GPU contention) the old handles are dead, and drawing them (ImGui AddImage ->
// SetTexture) faults inside the DX backend. Any plugin that CACHES such textures
// across frames MUST compare this value and drop its cache when it changes.
// DX9 path ONLY: under the DX7/DirectDraw proxy this counter never changes — which
// is fine, because DX7_CreateTextureARGB allocates auto-restored managed / system-
// memory surfaces that survive a mode change (they are not D3DPOOL_DEFAULT).
unsigned Overlay_DeviceEpoch();

// Returns an ImGui draw callback (cast to ImDrawCallback) that switches the DX9
// pipeline to ADDITIVE blend for the following draw commands — for glow effects
// like RO-style explosions. Reset afterwards with ImDrawCallback_ResetRenderState.
// DX9 only (returns a no-op-safe callback that early-outs when the device is gone).
void* D3D9_AdditiveBlendCallback();

// Returns an ImGui draw callback that sets an EXPLICIT SRCBLEND/DESTBLEND on the DX9
// pipeline, encoded in the callback's UserCallbackData: low byte = D3DBLEND source
// factor, second byte = D3DBLEND dest factor (D3DBLEND enum values). Used to replicate
// a .str effect layer's native per-layer blend (e.g. SRCALPHA/ONE = alpha-modulated
// additive, so a black-background coin texture composites without a black halo).
// Reset afterwards with ImDrawCallback_ResetRenderState. DX9 only (no-op if device gone).
void* D3D9_ExplicitBlendCallback();

// Sets the ImGui texture sampler filter for subsequent draw commands: false =
// POINT (crisp pixel-art, e.g. RO icons), true = LINEAR (ImGui's default smooth
// filtering). Call from an ImDrawList::AddCallback, then restore with
// ImDrawCallback_ResetRenderState. DX9 only (no-op otherwise).
void Overlay_SetTextureFilter(bool linear);

// Real-time post-processing of the game's rendered frame (engine output: 3D
// world + sprites + native UI), applied as fullscreen passes in Present BEFORE
// the Bourgeon ImGui overlay (so the overlay stays untouched). Two passes: a
// colour/effects pass, then an optional FXAA pass. DX9 only (no-op in the DX7
// proxy path). Needs d3dx9_43.dll (already loaded by the client) for one-time
// shader compilation; if unavailable the pipeline silently disables itself.
struct D3D9PostFx {
  bool  enabled     = false;  // master switch
  // colour grade
  float brightness  = 0.0f;   // additive,   -0.5..0.5 (0 = neutral)
  float contrast    = 1.0f;   // around 0.5,  0.5..2.0 (1 = neutral)
  float gamma       = 1.0f;   // power curve, 0.5..2.0 (1 = neutral)
  float saturation  = 1.0f;   // 0..2 (1 = neutral, 0 = greyscale)
  float temperature = 0.0f;   // -1..1 (cool .. warm)
  int   filter      = 0;      // 0 none,1 grayscale,2 sepia,3 negative,4 colorblind
  // screen-space effects
  float vignette    = 0.0f;   // 0..1 darkened edges
  float grain       = 0.0f;   // 0..1 animated film grain
  float aberration  = 0.0f;   // 0..1 chromatic aberration
  float sharpen     = 0.0f;   // 0..1 unsharp mask
  bool  fxaa        = false;  // edge anti-aliasing (second pass)
  float fxaa_strength = 0.35f; // 0..0.5 blend of the FXAA result (>0.5 wrecks UI text)
};

// Stores the params immediately; they take effect next frame, so it is safe to
// call before the device exists. Pass enabled=false to bypass all passes.
void D3D9_SetPostFx(const D3D9PostFx& fx);

// Saves the current frame (post-processed game image, WITHOUT the Bourgeon
// overlay) to a PNG. Captured next Present. `filepath` is copied. DX9 only.
void D3D9_RequestScreenshot(const char* filepath);

// One textured quad for offscreen avatar compositing: `tex` = IDirect3DTexture9*,
// (x0,y0)-(x1,y1) = destination rect in canvas pixels, (u0,v0)-(u1,v1) = source UVs
// (pass them pre-swapped for a horizontal mirror).
struct D3D9TexQuad {
  void* tex;
  float x0, y0, x1, y1;
  float u0, v0, u1, v1;
};

// Composites `n` textured quads (in array/painter order, alpha-blended) onto a fresh
// w*h TRANSPARENT render target and reads the result back into `out_argb` (w*h,
// A8R8G8B8 = 0xAARRGGBB per pixel). The canvas alpha becomes the cut-out coverage.
// Used by the character-sheet avatar GIF export. Returns false if the D3D9 device
// isn't ready or a resource/lock fails. DX9 only (no-op under the DX7 proxy).
bool D3D9_CompositeQuadsRGBA(const D3D9TexQuad* quads, int quad_count,
                             int width_px, int height_px,
                             void* out_argb);

// Global texture min/mag filter override for the GAME's rendering (not the
// Bourgeon overlay): 0 = off/default, 1 = POINT (crisp pixel-art), 2 = LINEAR
// (smooth). Hooks IDirect3DDevice9::SetSamplerState. DX9 only.
void D3D9_SetTextureFilter(int mode);
