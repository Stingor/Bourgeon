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

// Global texture min/mag filter override for the GAME's rendering (not the
// Bourgeon overlay): 0 = off/default, 1 = POINT (crisp pixel-art), 2 = LINEAR
// (smooth). Hooks IDirect3DDevice9::SetSamplerState. DX9 only.
void D3D9_SetTextureFilter(int mode);
