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

// Animated GIF for OPAQUE SCREEN captures (the zone recorder). Same input format
// as GifWrite (0xAARRGGBB, alpha ignored here), but two different choices, both
// forced by what a screen capture is:
//
//  · GLOBAL palette, median-cut over ALL frames at once. GifWrite quantises each
//    frame on its own by dropping colour bits — fine for a palettised RO sprite,
//    but on a rendered scene it posterises the sky into bands, and a per-frame
//    palette makes flat areas SHIMMER (the same pixel lands on a different colour
//    from one frame to the next). One palette for the whole clip fixes both.
//
//  · DIFFERENTIAL frames. Only the pixels that changed since the previous frame
//    are written; the rest are the transparent index over disposal method 1 (do
//    not dispose), so the canvas keeps showing them. Each frame is also clipped
//    to the bounding box of its changes. A 10 s clip of a mostly-static game
//    screen shrinks by roughly an order of magnitude — which is the difference
//    between a GIF one can embed in a tutorial and one nobody will load.
//
// No dithering, deliberately: it would scatter noise over every flat area, and
// every dithered pixel counts as "changed", which defeats the differencing.
//
// `delay_cs` = per-frame delay in centiseconds. Loops forever. Returns true on
// success. The file is streamed out frame by frame (a long clip never doubles
// its memory footprint in this 32-bit process).
bool GifWriteScreen(const char* path, const uint32_t* const* frames, int w, int h,
                    int nframes, int delay_cs);
