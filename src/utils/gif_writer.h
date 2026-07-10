#pragma once

#include <cstdint>

// Minimal animated-GIF encoder (RO avatar export).
//
// Encodes `nframes` RGBA frames of size w*h into an animated, looping GIF at
// `path`. Each frame pixel is 0xAARRGGBB (A8R8G8B8, matching a D3D9 LockRect of a
// D3DFMT_A8R8G8B8 surface). Pixels with alpha < 128 become the GIF's transparent
// colour (so the avatar keeps its cut-out silhouette over any background).
//
// Per frame: the opaque colours are reduced to <=255 entries via median-cut, index
// 0 is reserved as the transparent colour, and the index stream is LZW-compressed
// (standard GIF variable-width LZW). `delay_cs` = per-frame delay in centiseconds
// (1/100 s; e.g. 10 = 100 ms). Loops forever (NETSCAPE2.0 extension).
//
// Returns true on success. Self-contained (no d3dx / WIC / stb dependency).
bool GifWrite(const char* path, const uint32_t* const* frames, int w, int h,
              int nframes, int delay_cs);
