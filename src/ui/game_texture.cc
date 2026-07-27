#include "ui/game_texture.h"

#include <Windows.h>

#include <vector>

#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB

namespace {

using TexMgr_t  = void*(__cdecl*)();
using MakeKey_t = void*(__cdecl*)(const char*);
using LoadTex_t = void*(__fastcall*)(void*, void*, void*);

// Pixels bruts d'une texture du client, tels que le TexMgr les expose.
struct RawTex { const uint8_t* bgra; int w; int h; };

// SEH, donc POD UNIQUEMENT dans cette portée : aucun objet C++ à dérouler, sinon
// MSVC refuse (C2712). La conversion vers std::vector est faite par l'appelant,
// hors du __try.
bool GetRawTex(const char* path, RawTex* out) {
  __try {
    void* mgr = reinterpret_cast<TexMgr_t>(ro::texmgr::kGet)();
    if (!mgr) return false;
    void* key = reinterpret_cast<MakeKey_t>(ro::texmgr::kMakeKey)(path);
    if (!key) return false;
    void* tex = reinterpret_cast<LoadTex_t>(ro::texmgr::kLoad)(mgr, nullptr, key);
    if (!tex) return false;
    auto* bytes = static_cast<char*>(tex);
    const int w = *reinterpret_cast<int*>(bytes + ro::texmgr::kWidth);
    const int h = *reinterpret_cast<int*>(bytes + ro::texmgr::kHeight);
    const uint8_t* bgra =
        *reinterpret_cast<const uint8_t**>(bytes + ro::texmgr::kPixels);
    // Garde-fou : au-delà de 256×256 ce n'est pas une ressource d'interface, et
    // une taille absurde signale surtout qu'on lit un objet qui n'en est pas une.
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || !bgra) return false;
    out->bgra = bgra; out->w = w; out->h = h;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}  // namespace

namespace ro {

bool GameFilePixels(const char* path, std::vector<uint8_t>* argb, int* w, int* h) {
  if (!path || !path[0] || !argb) return false;
  RawTex raw{};
  if (!GetRawTex(path, &raw)) return false;
  argb->resize(static_cast<size_t>(raw.w) * raw.h * 4);
  uint8_t* out = argb->data();
  for (int i = 0; i < raw.w * raw.h; ++i) {
    const uint8_t b = raw.bgra[i * 4], g = raw.bgra[i * 4 + 1], r = raw.bgra[i * 4 + 2];
    const bool color_key = (r == 0xFF && g == 0 && b == 0xFF);  // magenta -> transparent
    out[i * 4]     = b;
    out[i * 4 + 1] = g;
    out[i * 4 + 2] = r;
    out[i * 4 + 3] = color_key ? 0 : 0xFF;
  }
  if (w) *w = raw.w;
  if (h) *h = raw.h;
  return true;
}

GameTexture TextureFromGameFile(const char* path) {
  std::vector<uint8_t> argb;
  int w = 0, h = 0;
  if (!GameFilePixels(path, &argb, &w, &h)) return {};
  return {Overlay_CreateTextureARGB(argb.data(), w, h), w, h};
}

}  // namespace ro
