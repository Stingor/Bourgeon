#include "ui/icon_cache.h"

#include <Windows.h>

#include <cstdio>
#include <unordered_map>
#include <vector>

#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / Overlay_DeviceEpoch

namespace {

// ── Adresses natives (client 20250716) ───────────────────────────────────────
constexpr uintptr_t kBuildIconPath = 0x00d5a720;  // __stdcall(id_str, out[128], identified)
constexpr uintptr_t kTexMgr        = 0x00a90350;
constexpr uintptr_t kMakeKey       = 0x00a9f030;
constexpr uintptr_t kLoadTex       = 0x00a8d4a0;
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;

using BuildIconPath_t = void(__stdcall*)(const char*, char*, int);
using TexMgr_t        = void*(__cdecl*)();
using MakeKey_t       = void*(__cdecl*)(const char*);
using LoadTex_t       = void*(__fastcall*)(void*, void*, void*);

// Une icône est identifiée par (nameid, identified) : le client bâtit deux
// chemins différents selon l'état.
struct IconKey {
  uint32_t nameid;
  int      identified;
  bool operator==(const IconKey& other) const {
    return nameid == other.nameid && identified == other.identified;
  }
};
struct IconKeyHash {
  size_t operator()(const IconKey& k) const {
    return std::hash<uint32_t>{}(k.nameid) ^ (static_cast<size_t>(k.identified) << 1);
  }
};

// tex nul = échec MÉMORISÉ : sans ça, une icône absente serait rechargée à
// chaque frame, avec son SEH et son parcours du TexMgr.
std::unordered_map<IconKey, ro::IconTex, IconKeyHash> g_icon_cache;

// Résout le .bmp en pixels bruts BGRA via le TexMgr natif.
// SEH, donc POD UNIQUEMENT : aucun objet C++ à dérouler ici, sinon C2712. La
// conversion vers std::vector est faite hors du __try, par l'appelant.
struct RawTex { const uint8_t* bgra; int w; int h; };

bool GetRawTex(const char* path, RawTex* out) {
  __try {
    void* mgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
    if (!mgr) return false;
    void* key = reinterpret_cast<MakeKey_t>(kMakeKey)(path);
    if (!key) return false;
    void* tex = reinterpret_cast<LoadTex_t>(kLoadTex)(mgr, nullptr, key);
    if (!tex) return false;
    const int w = *reinterpret_cast<int*>(static_cast<char*>(tex) + kTexW);
    const int h = *reinterpret_cast<int*>(static_cast<char*>(tex) + kTexH);
    const uint8_t* bgra =
        *reinterpret_cast<const uint8_t**>(static_cast<char*>(tex) + kTexPix);
    // Garde-fou de taille : au-delà, c'est un objet qui n'est pas une icône.
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || !bgra) return false;
    out->bgra = bgra; out->w = w; out->h = h;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH isolé (POD only) : construit `유저인터페이스\item\<res>.bmp` depuis l'id.
bool BuildIconPathSafe(uint32_t nameid, char* out, int identified) {
  char idstr[16];
  std::snprintf(idstr, sizeof(idstr), "%u", nameid);
  out[0] = '\0';
  __try {
    reinterpret_cast<BuildIconPath_t>(kBuildIconPath)(idstr, out, identified);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

ro::IconTex LoadItemIcon(uint32_t nameid, int identified) {
  char path[160];
  if (!BuildIconPathSafe(nameid, path, identified)) return {};
  RawTex raw{};
  if (!GetRawTex(path, &raw)) return {};

  std::vector<uint8_t> argb(static_cast<size_t>(raw.w) * raw.h * 4);
  for (int i = 0; i < raw.w * raw.h; ++i) {
    const uint8_t b = raw.bgra[i * 4], g = raw.bgra[i * 4 + 1], r = raw.bgra[i * 4 + 2];
    const bool color_key = (r == 0xFF && g == 0 && b == 0xFF);  // magenta -> transparent
    argb[i * 4]     = b;
    argb[i * 4 + 1] = g;
    argb[i * 4 + 2] = r;
    argb[i * 4 + 3] = color_key ? 0 : 0xFF;
  }
  return {Overlay_CreateTextureARGB(argb.data(), raw.w, raw.h), raw.w, raw.h};
}

}  // namespace

namespace ro {

IconTex ItemIcon(uint32_t nameid, int identified) {
  // Textures D3DPOOL_DEFAULT : mortes après un reset/recréation du device. Sans
  // ce garde, AddImage plante dans ddraw sur un handle libéré.
  static unsigned s_device_epoch = 0;
  const unsigned epoch = Overlay_DeviceEpoch();
  if (epoch != s_device_epoch) {
    g_icon_cache.clear();
    s_device_epoch = epoch;
  }

  const IconKey key{nameid, identified};
  auto found = g_icon_cache.find(key);
  if (found != g_icon_cache.end()) return found->second;
  return g_icon_cache[key] = LoadItemIcon(nameid, identified);
}

}  // namespace ro
