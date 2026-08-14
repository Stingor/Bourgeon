// Lecture d'un modèle Granny (.gr2) — validation HORS du client.
//
// ── Pourquoi ce programme existe ─────────────────────────────────────────────
// Une poignée de monstres ne sont pas des sprites : l'Emperium, les gardiens de
// forteresse, le drapeau de guilde et les coffres au trésor sont des ACTEURS 3D
// (85 classes, 8 modèles). Le client le sait à l'extension que lui rend
// `jobName.lub` — « Empelium90_0.gr2 » au lieu de « Chocho » — et bifurque vers
// `data\model\3dmob\%s`. Nos fenêtres, elles, cherchaient un `.spr` de ce nom et
// affichaient « pas de sprite ».
//
// Le format `.gr2` est propriétaire et compressé : personne ne le parse (GRF
// Editor lui-même ne le rend pas). Mais `granny2.dll` est livrée AVEC le client
// et exporte tout ce qu'il faut. Ce programme prouve, hors du jeu, qu'on sait en
// tirer géométrie, textures et squelette — exactement la démarche suivie pour
// `.spr`/`.act` (transcription validée avant la moindre ligne compilée dans le
// client).
//
// ── Comment s'en servir ──────────────────────────────────────────────────────
//     "C:\...\vcvarsall.bat" x86
//     cl /std:c++17 /EHsc /O2 tools\gr2_dump.cc
//     gr2_dump.exe "E:\...\Moonlight-Destiny\data\model\3dmob\empelium90_0.gr2"
// Ajouter `--bmp <dossier>` pour écrire les textures décodées et les regarder.
//
// 🔴 Le programme DOIT être compilé en x86 : `granny2.dll` est 32 bits, comme le
// client.
//
// ── Ce qui est mesuré et ce qui est supposé ──────────────────────────────────
// Les offsets ci-dessous ne viennent pas d'un en-tête Granny (nous n'en avons
// pas) mais du client lui-même, décompilé :
//   * `C3dGrannyModelRes_Load` (0x0071B9B0) : exige `GrannyVersionsMatch(2,1,0,5)`,
//     lit TextureCount/Textures en `file_info+16/+20`, Skeletons en `+36`,
//     ModelCount/Models en `+64/+68`, et refuse une texture dont `+4` n'est pas
//     nul ou dont `+60` ne vaut pas 1 (« single-image : ColorMapTextureType »).
//   * `sub_7211D0` (0x007211D0) : `model+4` = Skeleton, `skeleton+4` = BoneCount,
//     `model+76/+80` = MeshBindingCount/MeshBindings, `mesh+20/+24` =
//     MaterialBindingCount/MaterialBindings, et `GrannyGetMaterialTextureByType(b, 2)`
//     pour la texture diffuse — dont le PREMIER champ est le nom de fichier, que
//     le client compare par `strcmp` aux textures du fichier.
//   * `sub_721420` (0x00721420) : `texture+8/+12` = largeur/hauteur, image
//     décodée par `GrannyCopyTextureImage(..., GrannyRGBA8888PixelFormat, ...)`.
// Le programme les REVÉRIFIE au lieu de leur faire confiance : version de la
// DLL, cohérence des compteurs, lisibilité des noms, et surtout le fait que les
// accesseurs de l'API (qui, eux, connaissent le vrai layout) rendent les mêmes
// chiffres que la lecture directe.

#define _CRT_SECURE_NO_WARNINGS  // `fopen` sur un chemin passé en argument
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ── L'API Granny, chargée à la main ──────────────────────────────────────────
//
// Les exports sont décorés `__stdcall` (`_GrannyGetFileInfo@4`) ; deux symboles
// sont des DONNÉES, pas des fonctions (les descripteurs de format), et se
// récupèrent par le même `GetProcAddress`.
using PFN_GetVersionString   = const char*(__stdcall*)();
using PFN_ReadEntireFileFromMemory = void*(__stdcall*)(int32_t size, void* mem);
using PFN_FreeFile           = void(__stdcall*)(void*);
using PFN_GetFileInfo        = void*(__stdcall*)(void*);
using PFN_GetMeshIndexCount  = int32_t(__stdcall*)(void*);
using PFN_CopyMeshIndices    = void(__stdcall*)(void*, int32_t bytes_per_index, void* dst);
using PFN_GetMeshVertexCount = int32_t(__stdcall*)(void*);
using PFN_CopyMeshVertices   = void(__stdcall*)(void*, const void* type, void* dst);
using PFN_GetMeshVertexType  = void*(__stdcall*)(void*);
using PFN_MeshIsRigid        = int32_t(__stdcall*)(void*);
using PFN_GetMeshTriGroupCount = int32_t(__stdcall*)(void*);
using PFN_GetMeshTriGroups   = void*(__stdcall*)(void*);
using PFN_GetMaterialTextureByType = void*(__stdcall*)(void* material, int32_t type);
using PFN_TextureHasAlpha    = int32_t(__stdcall*)(void*);
using PFN_CopyTextureImage   = int32_t(__stdcall*)(void* texture, int32_t image_index,
                                                  int32_t mip_index, const void* pixel_format,
                                                  int32_t width, int32_t height,
                                                  int32_t stride, void* dst);

struct Granny {
  HMODULE dll = nullptr;
  PFN_GetVersionString   GetVersionString   = nullptr;
  PFN_ReadEntireFileFromMemory ReadEntireFileFromMemory = nullptr;
  PFN_FreeFile           FreeFile           = nullptr;
  PFN_GetFileInfo        GetFileInfo        = nullptr;
  PFN_GetMeshIndexCount  GetMeshIndexCount  = nullptr;
  PFN_CopyMeshIndices    CopyMeshIndices    = nullptr;
  PFN_GetMeshVertexCount GetMeshVertexCount = nullptr;
  PFN_CopyMeshVertices   CopyMeshVertices   = nullptr;
  PFN_GetMeshVertexType  GetMeshVertexType  = nullptr;
  PFN_MeshIsRigid        MeshIsRigid        = nullptr;
  PFN_GetMeshTriGroupCount GetMeshTriGroupCount = nullptr;
  PFN_GetMeshTriGroups   GetMeshTriGroups   = nullptr;
  PFN_GetMaterialTextureByType GetMaterialTextureByType = nullptr;
  PFN_TextureHasAlpha    TextureHasAlpha    = nullptr;
  PFN_CopyTextureImage   CopyTextureImage   = nullptr;
  const void* PNT332VertexType   = nullptr;  // données exportées
  const void* RGBA8888PixelFormat = nullptr;
};

template <typename T>
bool Bind(HMODULE dll, const char* name, T* out) {
  *out = reinterpret_cast<T>(GetProcAddress(dll, name));
  if (*out == nullptr) {
    std::printf("!! export absent : %s\n", name);
    return false;
  }
  return true;
}

bool LoadGranny(const char* dll_path, Granny* g) {
  g->dll = LoadLibraryA(dll_path);
  if (!g->dll) {
    std::printf("!! LoadLibrary(%s) a échoué (%lu)\n", dll_path, GetLastError());
    return false;
  }
  bool ok = true;
  ok &= Bind(g->dll, "_GrannyGetVersionString@0", &g->GetVersionString);
  ok &= Bind(g->dll, "_GrannyReadEntireFileFromMemory@8", &g->ReadEntireFileFromMemory);
  ok &= Bind(g->dll, "_GrannyFreeFile@4", &g->FreeFile);
  ok &= Bind(g->dll, "_GrannyGetFileInfo@4", &g->GetFileInfo);
  ok &= Bind(g->dll, "_GrannyGetMeshIndexCount@4", &g->GetMeshIndexCount);
  ok &= Bind(g->dll, "_GrannyCopyMeshIndices@12", &g->CopyMeshIndices);
  ok &= Bind(g->dll, "_GrannyGetMeshVertexCount@4", &g->GetMeshVertexCount);
  ok &= Bind(g->dll, "_GrannyCopyMeshVertices@12", &g->CopyMeshVertices);
  ok &= Bind(g->dll, "_GrannyGetMeshVertexType@4", &g->GetMeshVertexType);
  ok &= Bind(g->dll, "_GrannyMeshIsRigid@4", &g->MeshIsRigid);
  ok &= Bind(g->dll, "_GrannyGetMeshTriangleGroupCount@4", &g->GetMeshTriGroupCount);
  ok &= Bind(g->dll, "_GrannyGetMeshTriangleGroups@4", &g->GetMeshTriGroups);
  ok &= Bind(g->dll, "_GrannyGetMaterialTextureByType@8", &g->GetMaterialTextureByType);
  ok &= Bind(g->dll, "_GrannyTextureHasAlpha@4", &g->TextureHasAlpha);
  ok &= Bind(g->dll, "_GrannyCopyTextureImage@32", &g->CopyTextureImage);
  // 🔴 Ces deux exports sont des VARIABLES qui CONTIENNENT un pointeur, pas les
  // descripteurs eux-mêmes : il faut déréférencer une fois de plus que
  // d'habitude. Le client ne laisse aucun doute là-dessus —
  //     mov eax, ds:GrannyPNT332VertexType   ; eax = &variable
  //     push dword ptr [eax]                 ; on pousse son CONTENU
  // (0x007212D0 et 0x0072145E). Passer l'adresse de la variable, comme on le
  // ferait pour n'importe quel symbole exporté, fait crasher `granny2.dll` dans
  // sa propre lecture du descripteur — c'est exactement ce qui s'est produit au
  // premier essai, et ça ne ressemble PAS à une erreur d'appelant.
  void** pnt = reinterpret_cast<void**>(GetProcAddress(g->dll, "GrannyPNT332VertexType"));
  void** rgba = reinterpret_cast<void**>(GetProcAddress(g->dll, "GrannyRGBA8888PixelFormat"));
  if (!pnt || !rgba) {
    std::printf("!! descripteurs de format absents\n");
    ok = false;
  } else {
    g->PNT332VertexType    = *pnt;
    g->RGBA8888PixelFormat = *rgba;
  }
  return ok;
}

// ── Le layout, tel que le client le lit ──────────────────────────────────────
//
// Un seul endroit pour ces nombres, et chacun porte sa provenance. Tout le reste
// du programme passe par ces accesseurs : si un offset est faux, il l'est
// partout à la fois, ce qui se voit — plutôt qu'à un seul endroit, ce qui ne se
// voit pas.
template <typename T>
T At(const void* base, int offset) {
  T v;
  std::memcpy(&v, static_cast<const char*>(base) + offset, sizeof(T));
  return v;
}

// granny_file_info
int32_t FiTextureCount(const void* fi)  { return At<int32_t>(fi, 16); }
void**  FiTextures(const void* fi)      { return At<void**>(fi, 20); }
int32_t FiMaterialCount(const void* fi) { return At<int32_t>(fi, 24); }
int32_t FiSkeletonCount(const void* fi) { return At<int32_t>(fi, 32); }
void**  FiSkeletons(const void* fi)     { return At<void**>(fi, 36); }
int32_t FiVertexDataCount(const void* fi) { return At<int32_t>(fi, 40); }
int32_t FiTriTopoCount(const void* fi)  { return At<int32_t>(fi, 48); }
int32_t FiMeshCount(const void* fi)     { return At<int32_t>(fi, 56); }
void**  FiMeshes(const void* fi)        { return At<void**>(fi, 60); }
int32_t FiModelCount(const void* fi)    { return At<int32_t>(fi, 64); }
void**  FiModels(const void* fi)        { return At<void**>(fi, 68); }
int32_t FiTrackGroupCount(const void* fi) { return At<int32_t>(fi, 72); }
int32_t FiAnimationCount(const void* fi)  { return At<int32_t>(fi, 80); }
void**  FiAnimations(const void* fi)      { return At<void**>(fi, 84); }

// granny_model : Name, Skeleton, InitialPlacement (granny_transform = 68 o),
// MeshBindingCount, MeshBindings.
const char* ModelName(const void* m)      { return At<const char*>(m, 0); }
const void* ModelSkeleton(const void* m)  { return At<const void*>(m, 4); }
int32_t ModelBindingCount(const void* m)  { return At<int32_t>(m, 76); }
void**  ModelBindings(const void* m)      { return At<void**>(m, 80); }

// granny_skeleton : Name, BoneCount, Bones.
const char* SkeletonName(const void* s) { return At<const char*>(s, 0); }
int32_t SkeletonBoneCount(const void* s) { return At<int32_t>(s, 4); }

// granny_mesh : Name, ..., MaterialBindingCount, MaterialBindings.
const char* MeshName(const void* m)     { return At<const char*>(m, 0); }
int32_t MeshMatBindCount(const void* m) { return At<int32_t>(m, 20); }
void**  MeshMatBindings(const void* m)  { return At<void**>(m, 24); }

// granny_texture : FromFileName, TextureType, Width, Height.
const char* TexName(const void* t)   { return At<const char*>(t, 0); }
int32_t TexType(const void* t)       { return At<int32_t>(t, 4); }
int32_t TexWidth(const void* t)      { return At<int32_t>(t, 8); }
int32_t TexHeight(const void* t)     { return At<int32_t>(t, 12); }
int32_t TexImageCount(const void* t) { return At<int32_t>(t, 60); }

// granny_animation : Name, Duration, TimeStep, Oversampling, TrackGroupCount…
const char* AnimName(const void* a) { return At<const char*>(a, 0); }
float AnimDuration(const void* a)   { return At<float>(a, 4); }

// Le format que le client demande à Granny pour la géométrie : position,
// normale, une coordonnée de texture. 32 octets, et c'est aussi ce que
// `sub_7211D0` alloue (`32 * vertexCount`) — la taille est donc confirmée par le
// client, pas devinée.
struct VertexPNT332 {
  float p[3];
  float n[3];
  float uv[2];
};
static_assert(sizeof(VertexPNT332) == 32, "PNT332 fait 32 octets");

// granny_tri_material_group : quel matériau pour quelle tranche de triangles.
struct TriGroup {
  int32_t material_index;
  int32_t tri_first;
  int32_t tri_count;
};

// Une chaîne du fichier est-elle lisible ? Un offset faux donne, en pratique, un
// pointeur qui pointe n'importe où : ce contrôle attrape le cas bien plus tôt
// qu'un crash, et sans en provoquer.
bool LooksLikeString(const char* s) {
  if (s == nullptr) return false;
  if (IsBadReadPtr(s, 1)) return false;
  for (int i = 0; i < 256; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == 0) return i > 0;
    if (c < 0x20 && c != '\t') return false;
  }
  return false;
}

const char* SafeStr(const char* s) { return LooksLikeString(s) ? s : "<?>"; }

std::vector<uint8_t> ReadFileBytes(const char* path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path, "rb");
  if (!f) return data;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    data.resize(static_cast<size_t>(n));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
  }
  std::fclose(f);
  return data;
}

// BMP 32 bits, non compressé, pour REGARDER ce qu'on a décodé. Le contrôle final
// d'un décodeur d'image est l'œil ; le reste ne fait que rendre ce contrôle
// possible.
bool WriteBmp32(const char* path, int w, int h, const uint8_t* rgba) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const uint32_t pixels = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4u;
  const uint32_t offset = 14 + 108;
  const uint32_t size   = offset + pixels;
  uint8_t hdr[14 + 108] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  std::memcpy(hdr + 2, &size, 4);
  std::memcpy(hdr + 10, &offset, 4);
  const uint32_t dib = 108;                 // BITMAPV4HEADER : porte les masques
  const int32_t  iw = w, ih = -h;           // hauteur négative = première ligne en haut
  const uint16_t planes = 1, bpp = 32;
  const uint32_t compression = 3;           // BI_BITFIELDS
  const uint32_t mask_r = 0x00FF0000, mask_g = 0x0000FF00,
                 mask_b = 0x000000FF, mask_a = 0xFF000000;
  std::memcpy(hdr + 14 +  0, &dib, 4);
  std::memcpy(hdr + 14 +  4, &iw, 4);
  std::memcpy(hdr + 14 +  8, &ih, 4);
  std::memcpy(hdr + 14 + 12, &planes, 2);
  std::memcpy(hdr + 14 + 14, &bpp, 2);
  std::memcpy(hdr + 14 + 16, &compression, 4);
  std::memcpy(hdr + 14 + 20, &pixels, 4);
  std::memcpy(hdr + 14 + 40, &mask_r, 4);
  std::memcpy(hdr + 14 + 44, &mask_g, 4);
  std::memcpy(hdr + 14 + 48, &mask_b, 4);
  std::memcpy(hdr + 14 + 52, &mask_a, 4);
  std::fwrite(hdr, 1, sizeof(hdr), f);
  // RGBA -> BGRA : l'ordre attendu par un BMP à masques.
  std::vector<uint8_t> row(static_cast<size_t>(w) * 4);
  for (int y = 0; y < h; ++y) {
    const uint8_t* src = rgba + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      row[x * 4 + 0] = src[x * 4 + 2];
      row[x * 4 + 1] = src[x * 4 + 1];
      row[x * 4 + 2] = src[x * 4 + 0];
      row[x * 4 + 3] = src[x * 4 + 3];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "usage: gr2_dump <fichier.gr2> [--dll <granny2.dll>] [--bmp <dossier>]\n");
    return 2;
  }
  // 🔴 Sortie NON bufferisée : le premier essai s'est terminé sur une violation
  // d'accès et le tampon de `stdout` est parti avec — donc aucune trace de
  // l'endroit où ça a lâché. Un programme qui sert à sonder une DLL inconnue
  // doit dire ce qu'il a fait AVANT de le faire.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  const char* gr2_path = argv[1];
  std::string dll_path = "granny2.dll";
  std::string bmp_dir;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--dll") == 0) dll_path = argv[i + 1];
    else if (std::strcmp(argv[i], "--bmp") == 0) bmp_dir = argv[i + 1];
  }

  Granny g;
  if (!LoadGranny(dll_path.c_str(), &g)) return 1;
  std::printf("granny2.dll : %s\n", g.GetVersionString());
  std::printf("             (le client exige GrannyVersionsMatch(2,1,0,5))\n\n");

  std::vector<uint8_t> bytes = ReadFileBytes(gr2_path);
  if (bytes.empty()) {
    std::printf("!! fichier illisible ou vide : %s\n", gr2_path);
    return 1;
  }
  std::printf("%s — %zu octets\n", gr2_path, bytes.size());

  // 🔴 Granny lit le fichier EN PLACE : le tampon doit rester vivant aussi
  // longtemps que le `granny_file`, et il est modifié (décompression sur place).
  void* file = g.ReadEntireFileFromMemory(static_cast<int32_t>(bytes.size()), bytes.data());
  if (!file) {
    std::printf("!! GrannyReadEntireFileFromMemory a refusé le fichier\n");
    return 1;
  }
  void* fi = g.GetFileInfo(file);
  if (!fi) {
    std::printf("!! GrannyGetFileInfo a rendu nul\n");
    return 1;
  }

  // Les 24 premiers dwords, bruts. C'est ce qui permet de RELIRE les offsets si
  // un jour un fichier ne se comporte pas comme prévu, au lieu de les redeviner.
  std::printf("\n-- file_info brut ------------------------------------------\n");
  for (int i = 0; i < 24; ++i) {
    const uint32_t v = At<uint32_t>(fi, i * 4);
    std::printf("  +%-3d 0x%08X %11d\n", i * 4, v, static_cast<int32_t>(v));
  }

  std::printf("\n-- sommaire ------------------------------------------------\n");
  std::printf("  textures    %d\n", FiTextureCount(fi));
  std::printf("  matériaux   %d\n", FiMaterialCount(fi));
  std::printf("  squelettes  %d\n", FiSkeletonCount(fi));
  std::printf("  vertex data %d\n", FiVertexDataCount(fi));
  std::printf("  topologies  %d\n", FiTriTopoCount(fi));
  std::printf("  meshes      %d\n", FiMeshCount(fi));
  std::printf("  modèles     %d\n", FiModelCount(fi));
  std::printf("  trackgroups %d\n", FiTrackGroupCount(fi));
  std::printf("  animations  %d\n", FiAnimationCount(fi));

  // ── Textures ───────────────────────────────────────────────────────────────
  std::printf("\n-- textures ------------------------------------------------\n");
  void** textures = FiTextures(fi);
  for (int i = 0; i < FiTextureCount(fi); ++i) {
    void* t = textures[i];
    const int w = TexWidth(t), h = TexHeight(t);
    std::printf("  [%d] %-28s %4dx%-4d type=%d images=%d alpha=%d\n", i,
                SafeStr(TexName(t)), w, h, TexType(t), TexImageCount(t),
                g.TextureHasAlpha(t) ? 1 : 0);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
      std::printf("      !! dimensions invraisemblables — offset douteux\n");
      continue;
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    std::printf("      -> CopyTextureImage(fmt=%p, %d, %d, stride=%d)…\n",
                g.RGBA8888PixelFormat, w, h, w * 4);
    const int32_t copied =
        g.CopyTextureImage(t, 0, 0, g.RGBA8888PixelFormat, w, h, w * 4, rgba.data());
    std::printf("      <- rendu %d\n", copied);
    // Contrôle sans œil : une image entièrement noire ou entièrement identique
    // trahit un décodage raté aussi sûrement qu'un crash.
    size_t distinct = 0;
    uint32_t first = 0;
    std::memcpy(&first, rgba.data(), 4);
    for (size_t p = 0; p + 4 <= rgba.size(); p += 4) {
      uint32_t px = 0;
      std::memcpy(&px, rgba.data() + p, 4);
      if (px != first) { ++distinct; if (distinct > 16) break; }
    }
    std::printf("      %s\n", distinct > 16 ? "image variée (décodage plausible)"
                                            : "!! image uniforme — suspect");
    if (!bmp_dir.empty()) {
      char out[MAX_PATH];
      std::snprintf(out, sizeof(out), "%s\\tex_%02d.bmp", bmp_dir.c_str(), i);
      std::printf("      -> %s %s\n", out,
                  WriteBmp32(out, w, h, rgba.data()) ? "" : "(échec d'écriture)");
    }
  }

  // ── Squelettes ─────────────────────────────────────────────────────────────
  std::printf("\n-- squelettes ----------------------------------------------\n");
  void** skels = FiSkeletons(fi);
  for (int i = 0; i < FiSkeletonCount(fi); ++i) {
    std::printf("  [%d] %-28s os=%d\n", i, SafeStr(SkeletonName(skels[i])),
                SkeletonBoneCount(skels[i]));
  }

  // ── Animations ─────────────────────────────────────────────────────────────
  std::printf("\n-- animations ----------------------------------------------\n");
  if (FiAnimationCount(fi) == 0) {
    std::printf("  aucune dans CE fichier — les poses vivent dans\n"
                "  model\\3dmob_bone\\<n>_<nom>.gr2 (cf. 0x0071F600)\n");
  }
  void** anims = FiAnimations(fi);
  for (int i = 0; i < FiAnimationCount(fi); ++i) {
    std::printf("  [%d] %-28s durée=%.3f s\n", i, SafeStr(AnimName(anims[i])),
                AnimDuration(anims[i]));
  }

  // ── Modèles et géométrie ───────────────────────────────────────────────────
  std::printf("\n-- modèles -------------------------------------------------\n");
  void** models = FiModels(fi);
  for (int i = 0; i < FiModelCount(fi); ++i) {
    void* m = models[i];
    const void* sk = ModelSkeleton(m);
    std::printf("  [%d] %-28s squelette=%s (%d os) meshes=%d\n", i,
                SafeStr(ModelName(m)), sk ? SafeStr(SkeletonName(sk)) : "-",
                sk ? SkeletonBoneCount(sk) : 0, ModelBindingCount(m));

    void** bindings = ModelBindings(m);
    for (int b = 0; b < ModelBindingCount(m); ++b) {
      // granny_model_mesh_binding : un seul champ, le mesh.
      void* mesh = At<void*>(&bindings[b], 0);
      const int32_t vcount = g.GetMeshVertexCount(mesh);
      const int32_t icount = g.GetMeshIndexCount(mesh);
      const bool rigid = g.MeshIsRigid(mesh) != 0;
      std::printf("      mesh[%d] %-24s sommets=%-6d triangles=%-6d %s\n", b,
                  SafeStr(MeshName(mesh)), vcount, icount / 3,
                  rigid ? "rigide" : "SOUPLE (skinning)");

      // 🔴 Le contre-contrôle qui compte : le nombre de meshes vu à travers
      // NOTRE offset (+76/+80) doit tomber sur des objets que les accesseurs de
      // l'API — qui, eux, connaissent le vrai layout — savent lire. Des
      // compteurs cohérents sur un pointeur pris au hasard, ça n'arrive pas.
      if (vcount <= 0 || icount <= 0 || icount % 3 != 0) {
        std::printf("          !! compteurs incohérents — offset douteux\n");
        continue;
      }

      std::vector<VertexPNT332> verts(static_cast<size_t>(vcount));
      g.CopyMeshVertices(mesh, g.PNT332VertexType, verts.data());
      std::vector<uint16_t> idx(static_cast<size_t>(icount));
      g.CopyMeshIndices(mesh, 2, idx.data());

      // Boîte englobante et bornes des UV : de quoi cadrer un rendu, et de quoi
      // repérer une lecture de travers (des coordonnées à 1e30, ça se voit).
      float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
      float uv_mn[2] = {1e30f, 1e30f}, uv_mx[2] = {-1e30f, -1e30f};
      for (const VertexPNT332& v : verts) {
        for (int k = 0; k < 3; ++k) {
          if (v.p[k] < mn[k]) mn[k] = v.p[k];
          if (v.p[k] > mx[k]) mx[k] = v.p[k];
        }
        for (int k = 0; k < 2; ++k) {
          if (v.uv[k] < uv_mn[k]) uv_mn[k] = v.uv[k];
          if (v.uv[k] > uv_mx[k]) uv_mx[k] = v.uv[k];
        }
      }
      std::printf("          boîte  x[%.2f %.2f] y[%.2f %.2f] z[%.2f %.2f]\n",
                  mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
      std::printf("          uv     u[%.3f %.3f] v[%.3f %.3f]\n", uv_mn[0],
                  uv_mx[0], uv_mn[1], uv_mx[1]);

      uint16_t idx_max = 0;
      for (uint16_t v : idx) if (v > idx_max) idx_max = v;
      std::printf("          index max = %u %s\n", idx_max,
                  idx_max < vcount ? "(dans les bornes)"
                                   : "!! HORS BORNES — lecture fausse");

      const int32_t groups = g.GetMeshTriGroupCount(mesh);
      const TriGroup* tg = static_cast<const TriGroup*>(g.GetMeshTriGroups(mesh));
      std::printf("          groupes de matériau = %d\n", groups);
      for (int32_t gi = 0; gi < groups && gi < 8; ++gi) {
        std::printf("            groupe[%d] matériau=%d triangles %d..%d\n", gi,
                    tg[gi].material_index, tg[gi].tri_first,
                    tg[gi].tri_first + tg[gi].tri_count - 1);
      }

      // Texture diffuse de chaque liaison de matériau. `2` = le type que le
      // client demande (`GrannyGetMaterialTextureByType(binding, 2)`), et le
      // premier champ du résultat est le nom de fichier — celui-là même que le
      // client compare aux textures du fichier pour retrouver son index.
      const int32_t mb = MeshMatBindCount(mesh);
      void** mbind = MeshMatBindings(mesh);
      std::printf("          liaisons de matériau = %d\n", mb);
      for (int32_t k = 0; k < mb && k < 8; ++k) {
        void* tex = g.GetMaterialTextureByType(mbind[k], 2);
        std::printf("            [%d] texture diffuse = %s\n", k,
                    tex ? SafeStr(TexName(tex)) : "(aucune)");
      }
    }
  }

  std::printf("\nfini.\n");
  // Pas de FreeFile : le programme s'arrête ici, et libérer le fichier tout en
  // gardant `bytes` vivant n'apporterait qu'un risque de plus.
  return 0;
}
