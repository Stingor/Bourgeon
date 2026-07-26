#include "ui/icon_cache.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
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

// ── Image de collection ──────────────────────────────────────────────────────
// Le client la nomme par le RESNAME de l'item, pas par son id : il faut donc
// monter un ItemSkillInfo autonome, lui poser l'id, et lui demander son resname.
constexpr uintptr_t kInfoCtor   = 0x006a1b20;  // ItemSkillInfo_ctor(this) __fastcall
constexpr uintptr_t kInfoSetId  = 0x006a6570;  // ItemSkillInfo_SetId(this,id) __thiscall
constexpr uintptr_t kGetResName = 0x006a4bc0;  // ItemSkillDB_GetResName(info) -> C-str
using InfoCtor_t   = void(__fastcall*)(void*);
using InfoSetId_t  = void(__thiscall*)(void*, int);
using GetResName_t = char*(__fastcall*)(void*);

// Préfixe CP949 « 유저인터페이스\collection\ », en DEUX littéraux concaténés : sans
// la coupure, le \xba avalerait le \ suivant.
constexpr char kCollectionPrefix[] =
    "\xc0\xaf\xc0\xfa\xc0\xce\xc5\xcd\xc6\xe4\xc0\xcc\xbd\xba" "\\collection\\";

// SEH isolé (POD only).
void ResolveResName(uint32_t nameid, char* out, size_t capacity) {
  out[0] = '\0';
  __try {
    uint8_t info[0x100];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(kInfoCtor)(info);
    reinterpret_cast<InfoSetId_t>(kInfoSetId)(info, static_cast<int>(nameid));
    info[0x5c] = 1;  // « identifié » : le resname est alors lu dans rec+8
    const char* resname = reinterpret_cast<GetResName_t>(kGetResName)(info);
    if (resname && resname[0]) {
      std::strncpy(out, resname, capacity - 1);
      out[capacity - 1] = '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

std::unordered_map<uint32_t, ro::IconTex> g_collection_cache;

}  // namespace

namespace {

// Textures D3DPOOL_DEFAULT : mortes après un reset/recréation du device. Sans ce
// garde, AddImage plante dans ddraw sur un handle libéré. Les DEUX caches sont
// vidés ensemble — celui de collection retombe sur l'autre, ils ne peuvent pas
// vivre à des époques différentes.
void DropCachesOnDeviceReset() {
  static unsigned s_device_epoch = 0;
  const unsigned epoch = Overlay_DeviceEpoch();
  if (epoch == s_device_epoch) return;
  g_icon_cache.clear();
  g_collection_cache.clear();
  s_device_epoch = epoch;
}

}  // namespace

namespace ro {

IconTex ItemIcon(uint32_t nameid, int identified) {
  DropCachesOnDeviceReset();

  const IconKey key{nameid, identified};
  auto found = g_icon_cache.find(key);
  if (found != g_icon_cache.end()) return found->second;

  char path[192];
  IconTex icon{};
  if (BuildIconPathSafe(nameid, path, identified)) icon = TextureFromGameFile(path);
  return g_icon_cache[key] = icon;
}

IconTex ItemCollectionIcon(uint32_t nameid) {
  DropCachesOnDeviceReset();

  auto found = g_collection_cache.find(nameid);
  if (found != g_collection_cache.end()) return found->second;

  IconTex icon{};
  char resname[64];
  ResolveResName(nameid, resname, sizeof(resname));
  if (resname[0]) {
    char path[192];
    std::snprintf(path, sizeof(path), "%s%s.bmp", kCollectionPrefix, resname);
    icon = TextureFromGameFile(path);
  }
  // Pas d'art de collection : la petite icône d'inventaire vaut mieux que rien.
  if (!icon.tex) icon = ItemIcon(nameid);
  return g_collection_cache[nameid] = icon;
}

}  // namespace ro
