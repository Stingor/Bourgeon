#pragma once

void InitD3D9Hook();

// Creates a D3D9 texture from a 32-bit A8R8G8B8 pixel buffer (w*h, tightly
// packed) using the game's render device, for use as an ImGui ImTextureID.
// Returns nullptr until the device has been captured (first rendered frame).
// Caller owns the returned IDirect3DTexture9* (Release on shutdown / device loss).
void* D3D9_CreateTextureARGB(const void* argb, int w, int h);

// Renderer-agnostic ARGB→texture upload: routes to the DX7 (DirectDraw surface)
// or D3D9 helper depending on which renderer the client is running. Plugins that
// draw textured ImGui content (e.g. menu icons) MUST use this so they work in
// both modes. Returns an ImTextureID-compatible void* (or nullptr).
void* Overlay_CreateTextureARGB(const void* argb, int w, int h);
