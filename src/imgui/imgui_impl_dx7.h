#pragma once

// dear imgui: Renderer Backend for DirectX7
// This needs to be used along with a Platform Backend (e.g. Win32)
//
// You can copy and use unmodified imgui_impl_* files in your project. See
// examples/ folder for examples of using this. If you are new to Dear ImGui,
// read documentation from the docs/ folder + read the top of imgui.cpp. Read
// online: https://github.com/ocornut/imgui/tree/master/docs

#include "imgui.h"  // IMGUI_IMPL_API

struct IDirect3DDevice7;

IMGUI_IMPL_API bool ImGui_ImplDX7_Init(IDirect3DDevice7* device);
IMGUI_IMPL_API void ImGui_ImplDX7_Shutdown();
IMGUI_IMPL_API void ImGui_ImplDX7_NewFrame();
IMGUI_IMPL_API void ImGui_ImplDX7_RenderDrawData(ImDrawData* draw_data);

// Creates a DX7 texture (LPDIRECTDRAWSURFACE7, returned as void*) from a 32-bit
// A8R8G8B8 pixel buffer (w*h, tightly packed, top-down) for use as an ImGui
// ImTextureID in DX7 mode. Returns nullptr if the device isn't ready yet.
// Caller owns the surface (Release on teardown / device loss).
IMGUI_IMPL_API void* DX7_CreateTextureARGB(const void* argb, int w, int h);

// Use if you want to reset your rendering device without losing Dear ImGui
// state.
IMGUI_IMPL_API bool ImGui_ImplDX7_CreateDeviceObjects();
IMGUI_IMPL_API void ImGui_ImplDX7_InvalidateDeviceObjects();
