#include "ui/head_icon.h"

#include <Windows.h>

#include <cstdio>

// Rendu DirectX 7 actif : le handle GPU natif n'est pas au même offset dans CTexture.
// Défini par le hook de rendu (comme dans basic_info / char_select).
extern bool g_imgui_dx7_active;

namespace ro {
namespace {

// Format-strings NATIVES (CP949) : « %d » = id de coiffure brut. Mêmes adresses que
// les icônes de coiffure du char-select (chaînes vérifiées dans l'exécutable).
constexpr uintptr_t kFmtActFemale = 0x0108F9A0;  // 인간족\머리통\여\%d_여.act
constexpr uintptr_t kFmtSprFemale = 0x0108F9BC;  // …\여\%d_여.spr
constexpr uintptr_t kFmtActMale   = 0x0108F9D8;  // 인간족\머리통\남\%d_남.act
constexpr uintptr_t kFmtSprMale   = 0x0108F9F4;  // …\남\%d_남.spr

constexpr uintptr_t kTexMgrGet      = 0x00a90350;  // __cdecl() -> gestionnaire
constexpr uintptr_t kMakeKey        = 0x00a9f030;  // __cdecl(path) -> clé de ressource
constexpr uintptr_t kLoadRes        = 0x00a8d4a0;  // __fastcall(mgr, edx, key) -> CSprite*/CAction*
constexpr uintptr_t kActGetFrame    = 0x0070f4b0;  // __fastcall(act, edx, action, frame)
constexpr uintptr_t kAtlasGetCached = 0x00566b70;  // __fastcall(atlas, edx, cell, pal, geom)
constexpr uintptr_t kAtlasBuild     = 0x005663d0;  // idem, construit la page si absente
constexpr uintptr_t kSceneCtxPtr    = 0x012515f8;  // *ptr + 0xc0 = atlas de sprites
constexpr int       kCTexDX9Handle  = 0x12c;       // CTexture -> IDirect3DTexture9*
constexpr int       kCTexDX7Handle  = 0x128;
constexpr int       kSprCells       = 0x510;  // CSprite -> tableaux de cellules
constexpr int       kSprPalette     = 0x110;  // CSprite -> palette convertie embarquée
constexpr int       kPalConverted   = 0x510;  // CPaletteRes -> palette CONVERTIE (pas +0x110)

using TexMgrGetFn   = void* (__cdecl*)();
using MakeKeyFn     = void* (__cdecl*)(const char*);
using LoadResFn     = void* (__fastcall*)(void*, void*, void*);
using ActGetFrameFn = void* (__fastcall*)(void*, void*, unsigned, unsigned);
using AtlasFn       = void* (__fastcall*)(void*, void*, void*, int, int*);

// Cache (coiffure, genre) -> ressources. Les ressources appartiennent au
// gestionnaire natif : on ne garde que le pointeur, jamais de libération.
struct SpriteEntry { int hair = -1, sex = -1; void* spr = nullptr; void* act = nullptr; };
constexpr int kSpriteCacheN = 64;
SpriteEntry g_sprite_cache[kSpriteCacheN];
int         g_sprite_next = 0;

// Remap historique des 12 premières coiffures : id de coiffure -> numéro de FICHIER
// .spr. Le client ne nomme pas le fichier d'après l'id brut, il passe par une table
// de noms (celle qu'indexe Job_BuildBodyOrHeadSpritePath_impl) ; sans ce remap la
// ligne de guilde affiche une coupe voisine. Table identique à celle du char-select
// (vérifiée à l'œil : 1↔2, 3→7/6→3/7→6, 4↔5, 11↔12 ; 8/9/10 et 13+ = identité), et
// préservée par le patch WARP Allow65kHairs pour les index < 43.
int HairFile(int hair) {
  static const int kHairFile[13] = {0, 2, 1, 7, 5, 4, 3, 6, 8, 9, 10, 12, 11};
  return (hair >= 1 && hair <= 12) ? kHairFile[hair] : hair;
}

SpriteEntry* EnsureSprite(int hair, int sex) {
  for (int i = 0; i < kSpriteCacheN; ++i)
    if (g_sprite_cache[i].hair == hair && g_sprite_cache[i].sex == sex)
      return &g_sprite_cache[i];

  char spr_path[160], act_path[160];
  std::snprintf(spr_path, sizeof(spr_path),
                reinterpret_cast<const char*>(sex ? kFmtSprMale : kFmtSprFemale), hair);
  std::snprintf(act_path, sizeof(act_path),
                reinterpret_cast<const char*>(sex ? kFmtActMale : kFmtActFemale), hair);
  void* spr = nullptr;
  void* act = nullptr;
  __try {
    void* mgr = reinterpret_cast<TexMgrGetFn>(kTexMgrGet)();
    spr = reinterpret_cast<LoadResFn>(kLoadRes)(
        mgr, nullptr, reinterpret_cast<MakeKeyFn>(kMakeKey)(spr_path));
    act = reinterpret_cast<LoadResFn>(kLoadRes)(
        mgr, nullptr, reinterpret_cast<MakeKeyFn>(kMakeKey)(act_path));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    spr = nullptr;
    act = nullptr;
  }
  SpriteEntry& entry = g_sprite_cache[g_sprite_next];
  g_sprite_next = (g_sprite_next + 1) % kSpriteCacheN;
  entry.hair = hair;
  entry.sex = sex;
  entry.spr = spr;
  entry.act = act;
  return &entry;
}

// Palette de couleur de cheveux : palette\머리\head_<N>.pal, chargée par le même
// gestionnaire. ⚠ CPaletteRes garde le brut RGBA à +0x110 et la palette CONVERTIE
// (format du moteur) à +0x510 : c'est celle-ci que veut l'atlas.
struct PaletteEntry { int color = -1; void* data = nullptr; };
constexpr int kPaletteCacheN = 32;
PaletteEntry g_palette_cache[kPaletteCacheN];
int          g_palette_next = 0;

void* EnsurePalette(int color) {
  if (color < 0) return nullptr;
  for (int i = 0; i < kPaletteCacheN; ++i)
    if (g_palette_cache[i].color == color) return g_palette_cache[i].data;

  void* data = nullptr;
  __try {
    char path[64];
    // "palette\" + 머리 (CP949 B8 D3 B8 AE) + "\head_<N>.pal"
    std::snprintf(path, sizeof(path), "palette\\\xB8\xD3\xB8\xAE\\head_%d.pal", color);
    void* mgr = reinterpret_cast<TexMgrGetFn>(kTexMgrGet)();
    void* res = reinterpret_cast<LoadResFn>(kLoadRes)(
        mgr, nullptr, reinterpret_cast<MakeKeyFn>(kMakeKey)(path));
    if (res) data = reinterpret_cast<char*>(res) + kPalConverted;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    data = nullptr;
  }
  PaletteEntry& entry = g_palette_cache[g_palette_next];
  g_palette_next = (g_palette_next + 1) % kPaletteCacheN;
  entry.color = color;
  entry.data = data;
  return data;
}

// Plus grande cellule de la frame 0 (action 0 = de face) -> texture d'atlas + UV.
// Ré-résolu à chaque appel : la page d'atlas est un cache LRU, et le re-Get remet la
// cellule en tête (cf. la même recette dans char_select / login_parade).
bool ResolveQuad(SpriteEntry* entry, void* palette_override, void** out_tex,
                 ImVec2* uv0, ImVec2* uv1, float* cell_w, float* cell_h) {
  if (!entry || !entry->spr || !entry->act) return false;
  const int handle_off = g_imgui_dx7_active ? kCTexDX7Handle : kCTexDX9Handle;
  bool found = false;
  __try {
    void* frame =
        reinterpret_cast<ActGetFrameFn>(kActGetFrame)(entry->act, nullptr, 0, 0);
    if (!frame) return false;
    char* frame_bytes = reinterpret_cast<char*>(frame);
    char* layers_begin = *reinterpret_cast<char**>(frame_bytes + 0x20);
    char* layers_end   = *reinterpret_cast<char**>(frame_bytes + 0x24);
    const int layer_count = static_cast<int>((layers_end - layers_begin) / 0x24);
    if (layer_count <= 0 || layer_count > 64) return false;
    char* sprite = reinterpret_cast<char*>(entry->spr);
    void* atlas = reinterpret_cast<void*>(
        *reinterpret_cast<uintptr_t*>(kSceneCtxPtr) + 0xc0);
    long best_area = -1;
    for (int i = 0; i < layer_count; ++i) {
      int* layer = reinterpret_cast<int*>(layers_begin + i * 0x24);
      const int cell_index = layer[2], cell_kind = layer[8];
      if (cell_index < 0 || cell_kind >= 2) continue;
      const int cells_begin = *reinterpret_cast<int*>(sprite + kSprCells + cell_kind * 0xc);
      const int cells_end   = *reinterpret_cast<int*>(sprite + kSprCells + 4 + cell_kind * 0xc);
      if (static_cast<unsigned>(cell_index) >=
          static_cast<unsigned>((cells_end - cells_begin) >> 2))
        continue;
      short* cell = *reinterpret_cast<short**>(cells_begin + cell_index * 4);
      if (!cell) continue;
      const int palette = palette_override
          ? static_cast<int>(reinterpret_cast<uintptr_t>(palette_override))
          : static_cast<int>(reinterpret_cast<uintptr_t>(sprite + kSprPalette));
      int geom[12] = {0};
      void* ctex = reinterpret_cast<AtlasFn>(kAtlasGetCached)(atlas, nullptr, cell,
                                                             palette, geom);
      if (!ctex)
        ctex = reinterpret_cast<AtlasFn>(kAtlasBuild)(atlas, nullptr, cell, palette, geom);
      if (!ctex) continue;
      const long area = static_cast<long>(cell[0]) * static_cast<long>(cell[1]);
      if (area <= best_area) continue;
      best_area = area;
      *out_tex = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctex) + handle_off);
      *cell_w = static_cast<float>(cell[0]);
      *cell_h = static_cast<float>(cell[1]);
      *uv0 = ImVec2(*reinterpret_cast<float*>(&geom[3]), *reinterpret_cast<float*>(&geom[4]));
      *uv1 = ImVec2(*reinterpret_cast<float*>(&geom[5]), *reinterpret_cast<float*>(&geom[6]));
      found = (*out_tex != nullptr);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return found;
}

}  // namespace

bool DrawHeadIcon(ImDrawList* draw_list, float x, float y, float box, int hair,
                  int sex, int hair_color) {
  if (!draw_list || box <= 1.0f || hair < 0) return false;
  // L'id de coiffure n'est PAS le numéro de fichier pour les 12 premières (cf. HairFile).
  SpriteEntry* entry = EnsureSprite(HairFile(hair), sex ? 1 : 0);
  void* tex = nullptr;
  ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
  float cell_w = 0.0f, cell_h = 0.0f;
  // Couleur demandée d'abord ; si la palette manque, la palette d'origine du sprite.
  void* palette = EnsurePalette(hair_color);
  if (!ResolveQuad(entry, palette, &tex, &uv0, &uv1, &cell_w, &cell_h)) {
    if (!palette) return false;
    if (!ResolveQuad(entry, nullptr, &tex, &uv0, &uv1, &cell_w, &cell_h)) return false;
  }
  if (!tex || cell_w <= 0.0f || cell_h <= 0.0f) return false;

  // Proportions conservées, centrage dans le carré (jamais d'agrandissement : une
  // tête RO fait ~30 px, l'étirer la rendrait floue).
  const float scale = (cell_w > box || cell_h > box)
                          ? ((box / cell_w < box / cell_h) ? box / cell_w : box / cell_h)
                          : 1.0f;
  const float draw_w = cell_w * scale, draw_h = cell_h * scale;
  const ImVec2 p0(x + (box - draw_w) * 0.5f, y + (box - draw_h) * 0.5f);
  const ImVec2 p1(p0.x + draw_w, p0.y + draw_h);
  draw_list->AddImage(reinterpret_cast<ImTextureID>(tex), p0, p1, uv0, uv1);
  return true;
}

}  // namespace ro
