#include "ui/icon_cache.h"

#include <Windows.h>

#include <cstdio>
#include <unordered_map>

#include "d3d9/d3d9_hook.h"  // Overlay_DeviceEpoch

namespace {

// __stdcall(id_str, out[128], identified) -> « 유저인터페이스\item\<res>.bmp »
constexpr uintptr_t kBuildIconPath = 0x00d5a720;
using BuildIconPath_t = void(__stdcall*)(const char*, char*, int);

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

// SEH isolé (POD only) : le client construit le chemin depuis l'id en décimal.
bool BuildIconPathSafe(uint32_t nameid, char* out, int identified) {
  char idstr[16];
  std::snprintf(idstr, sizeof(idstr), "%u", nameid);
  out[0] = '\0';
  __try {
    reinterpret_cast<BuildIconPath_t>(kBuildIconPath)(idstr, out, identified);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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

  char path[192];
  IconTex icon{};
  if (BuildIconPathSafe(nameid, path, identified)) icon = TextureFromGameFile(path);
  return g_icon_cache[key] = icon;
}

}  // namespace ro
