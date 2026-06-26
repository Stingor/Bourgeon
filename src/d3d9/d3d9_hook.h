#pragma once

void InitD3D9Hook();

// Creates a D3D9 texture from a 32-bit A8R8G8B8 pixel buffer (w*h, tightly
// packed) using the game's render device, for use as an ImGui ImTextureID.
// Returns nullptr until the device has been captured (first rendered frame).
// Caller owns the returned IDirect3DTexture9* (Release on shutdown / device loss).
void* D3D9_CreateTextureARGB(const void* argb, int w, int h);
