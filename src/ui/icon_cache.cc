#include "ragnarok/item_db.h"
#include "ui/icon_cache.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "d3d9/d3d9_hook.h"  // Overlay_DeviceEpoch
#include "ui/game_texture.h"  // ro::texmgr::kBuildItemIconPath

namespace {

// __stdcall(id_str, out[128], identified) -> « 유저인터페이스\item\<res>.bmp »
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
  // ⚠ Un MÉLANGE, pas un « ou exclusif » sur les bits de poids faible. C'était
  // `hash(nameid) ^ (identified << 1)` : sur MSVC `hash<uint32_t>` est proche de
  // l'identité, si bien que la clé (id, 1) tombait dans le même seau que
  // (id ^ 2, 0) — deux items voisins et leurs deux états se rangeaient à quatre
  // dans le même seau, pour rien.
  size_t operator()(const IconKey& k) const {
    const uint32_t mixed = k.nameid * 2654435761u +
                           static_cast<uint32_t>(k.identified);
    return std::hash<uint32_t>{}(mixed);
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
    reinterpret_cast<BuildIconPath_t>(ro::texmgr::kBuildItemIconPath)(idstr, out, identified);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── Image de collection ──────────────────────────────────────────────────────
// Le client la nomme par le RESNAME de l'item, pas par son id : il faut donc
// monter un ItemSkillInfo autonome, lui poser l'id, et lui demander son resname.
using InfoCtor_t   = void(__fastcall*)(void*);
using InfoSetId_t  = void(__thiscall*)(void*, int);
using GetResName_t = char*(__fastcall*)(void*);

// « 유저인터페이스\collection\<resname>.bmp », composé au format sur la racine
// partagée : le chemin n'est plus un littéral collé, donc le piège de l'octet
// `\xba` avalant le `\` suivant ne se pose plus ici.
constexpr char kCollectionFmt[] = "%s\\collection\\%s.bmp";

// SEH isolé (POD only).
void ResolveResName(uint32_t nameid, char* out, size_t capacity) {
  out[0] = '\0';
  __try {
    uint8_t info[0x100];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(itemdb::kInfoCtorAddr)(info);
    reinterpret_cast<InfoSetId_t>(itemdb::kInfoSetIdAddr)(info, static_cast<int>(nameid));
    info[0x5c] = 1;  // « identifié » : le resname est alors lu dans rec+8
    const char* resname = reinterpret_cast<GetResName_t>(itemdb::kGetResNameAddr)(info);
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
  static Overlay_DeviceEpochWatch s_watch;
  if (!s_watch.Changed()) return;
  g_icon_cache.clear();
  g_collection_cache.clear();
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
  // ⚠ `emplace` et non `cache[key] = icon` : la clé vient d'être cherchée en
  // vain, la réécrire avec `operator[]` la hachait une seconde fois.
  return g_icon_cache.emplace(key, icon).first->second;
}

bool ItemIconPixels(uint32_t nameid, std::vector<uint8_t>* argb, int* w, int* h) {
  char path[192];
  if (!BuildIconPathSafe(nameid, path, /*identified=*/1) || !path[0]) return false;
  return GameFilePixels(path, argb, w, h);
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
    std::snprintf(path, sizeof(path), kCollectionFmt, ro::uipath::kUiRoot, resname);
    icon = TextureFromGameFile(path);
  }
  // Pas d'art de collection : la petite icône d'inventaire vaut mieux que rien.
  if (!icon.tex) icon = ItemIcon(nameid);
  // ⚠ Même remarque que dans `ItemIcon` : une seule recherche, pas deux.
  return g_collection_cache.emplace(nameid, icon).first->second;
}

}  // namespace ro
