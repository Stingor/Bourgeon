#include "ui/game_texture.h"

#include <cstdio>
#include <cstring>

#include <Windows.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / Overlay_DeviceEpoch
#include "utils/str_key_map.h"  // util::StrKeyMap : cache interrogé par const char*

namespace {

// Pixels bruts d'une texture du client, tels que le TexMgr les expose.
struct RawTex { const uint8_t* bgra; int w; int h; };

// SEH, donc POD UNIQUEMENT dans cette portée : aucun objet C++ à dérouler, sinon
// MSVC refuse (C2712). La conversion vers std::vector est faite par l'appelant,
// hors du __try.
bool GetRawTex(const char* path, RawTex* out) {
  __try {
    void* tex = ro::texmgr::LoadResource(path);
    if (!tex) return false;
    auto* bytes = static_cast<char*>(tex);
    const int w = *reinterpret_cast<int*>(bytes + ro::texmgr::kWidth);
    const int h = *reinterpret_cast<int*>(bytes + ro::texmgr::kHeight);
    const uint8_t* bgra =
        *reinterpret_cast<const uint8_t**>(bytes + ro::texmgr::kPixels);
    // Garde-fou : une taille absurde signale qu'on lit un objet qui n'est pas une
    // texture (pointeur bancal). La borne était à 256, taillée pour les icônes et
    // les petits boutons — elle rejetait donc EN SILENCE tout fond de fenêtre
    // (bg_bank.bmp fait 345x187, bg_styling.bmp et w_statwin_bg sont du même ordre),
    // et l'appelant ne voyait qu'une texture nulle sans savoir pourquoi.
    // 4096 = la même borne que LoadClientBmp (ui/ro_imgui.cc), l'autre chargeur de
    // bmp d'interface du toolkit : les deux attrapent le même genre de valeur folle.
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !bgra) return false;
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

namespace {

// Le cache et son epoch de device, sortis de la fonction pour qu'une seconde —
// l'invalidation explicite — puisse les atteindre.
//
// 🔴 `StrKeyMap` et non `unordered_map<std::string, …>` : ce cache est
// interrogé par `const char*` à chaque frame, pour chaque texture d'interface
// dessinée. Une table à clé `std::string` y construisait une chaîne temporaire
// À CHAQUE APPEL, succès compris — nos chemins dépassent tous la SSO, donc une
// allocation et une libération par interrogation. Voir utils/str_key_map.h pour
// le pourquoi du `string_view` et du stockage à adresses stables.
util::StrKeyMap<GameTexture>& TexCache() {
  static util::StrKeyMap<GameTexture> s_cache;
  return s_cache;
}
Overlay_DeviceEpochWatch g_cache_watch;

}  // namespace

GameTexture CachedTextureFromGameFile(const char* path) {
  auto& cache = TexCache();
  // Textures en D3DPOOL_DEFAULT : elles meurent au reset du device, et un handle
  // libéré passé à AddImage plante dans ddraw. Même garde que ui/icon_cache.cc.
  if (g_cache_watch.Changed()) {
    // ⚠ On JETTE sans relâcher : ces handles appartiennent à un device qui
    // n'existe plus, et les relâcher planterait. C'est l'inverse de
    // InvalidateGameTextures, où le device est toujours là.
    cache.Clear();
  }
  if (!path || !path[0]) return {};
  // ⚠ Une seule recherche : `find` puis `cache[path]` en hachait deux fois le
  // chemin sur un défaut. `Emplace` rend l'entrée en place et n'écrase rien.
  if (const GameTexture* found = cache.Find(path)) return *found;
  return cache.Emplace(path, TextureFromGameFile(path));
}

void InvalidateGameTextures() {
  auto& cache = TexCache();
  // Le device est VIVANT ici : les handles sont à nous, et les abandonner serait
  // une fuite à chaque changement de skin.
  for (auto& entry : cache) {
    if (entry.second.tex) Overlay_ReleaseTexture(entry.second.tex);
  }
  cache.Clear();
}

namespace uipath {

void WithFileName(uintptr_t exe_path, const char* file, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  const char* base  = reinterpret_cast<const char*>(exe_path);
  const char* slash = std::strrchr(base, '\\');
  const size_t n = slash ? static_cast<size_t>(slash - base + 1) : 0;
  if (n && n < out_size) std::memcpy(out, base, n);
  std::snprintf(out + n, out_size - n, "%s", file);
}

}  // namespace uipath

}  // namespace ro
