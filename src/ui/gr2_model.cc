#include "ui/gr2_model.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>

namespace ro {
namespace {

// ── L'API Granny, liée à la main ─────────────────────────────────────────────
//
// Les exports sont décorés `__stdcall`. Deux d'entre eux sont des DONNÉES, et
// c'est le piège de ce module :
//
// 🔴 `GrannyPNT332VertexType` et `GrannyRGBA8888PixelFormat` sont des variables
// qui CONTIENNENT un pointeur — il faut déréférencer une fois de plus que pour
// un symbole ordinaire. Le client ne laisse aucun doute là-dessus :
//     mov eax, ds:GrannyPNT332VertexType   ; eax = &variable
//     push dword ptr [eax]                 ; on pousse son CONTENU
// (0x007212D0, et 0x0072145E pour le format de pixel). Passer l'adresse de la
// variable fait crasher `granny2.dll` DANS sa propre lecture du descripteur :
// une violation d'accès qui ne ressemble en rien à une erreur d'appelant.
using PFN_GetVersionString  = const char*(__stdcall*)();
using PFN_ReadEntireFile    = void*(__stdcall*)(int32_t, void*);
using PFN_FreeFile          = void(__stdcall*)(void*);
using PFN_GetFileInfo       = void*(__stdcall*)(void*);
using PFN_GetMeshIndexCount = int32_t(__stdcall*)(void*);
using PFN_CopyMeshIndices   = void(__stdcall*)(void*, int32_t, void*);
using PFN_GetMeshVertexCount= int32_t(__stdcall*)(void*);
using PFN_GetMeshVertices   = void*(__stdcall*)(void*);
using PFN_CopyMeshVertices  = void(__stdcall*)(void*, const void*, void*);
using PFN_GetMeshVertexType = void*(__stdcall*)(void*);
using PFN_MeshIsRigid       = int32_t(__stdcall*)(void*);
using PFN_GetMeshTriGroupCount = int32_t(__stdcall*)(void*);
using PFN_GetMeshTriGroups  = void*(__stdcall*)(void*);
using PFN_GetMaterialTextureByType = void*(__stdcall*)(void*, int32_t);
using PFN_TextureHasAlpha   = int32_t(__stdcall*)(void*);
using PFN_CopyTextureImage  = int32_t(__stdcall*)(void*, int32_t, int32_t, const void*,
                                                 int32_t, int32_t, int32_t, void*);
using PFN_InstantiateModel  = void*(__stdcall*)(void*);
using PFN_GetSourceSkeleton = void*(__stdcall*)(void*);
using PFN_NewLocalPose      = void*(__stdcall*)(int32_t);
using PFN_NewWorldPose      = void*(__stdcall*)(int32_t);
using PFN_FreeLocalPose     = void(__stdcall*)(void*);
using PFN_FreeWorldPose     = void(__stdcall*)(void*);
using PFN_SetModelClock     = void(__stdcall*)(void*, float);
using PFN_SampleModelAnims  = void(__stdcall*)(void*, int32_t, int32_t, void*);
using PFN_BuildWorldPose    = void(__stdcall*)(void*, int32_t, int32_t, void*,
                                              const float*, void*);
using PFN_FreeCompletedControls = void(__stdcall*)(void*);
using PFN_GetWorldPoseComposite4x4Array = const float*(__stdcall*)(void*);
using PFN_GetWorldPoseComposite4x4 = const float*(__stdcall*)(void*, int32_t);
using PFN_NewMeshBinding    = void*(__stdcall*)(void*, void*, void*);
using PFN_FreeMeshBinding   = void(__stdcall*)(void*);
using PFN_GetMeshBindingToBoneIndices = const int32_t*(__stdcall*)(void*);
using PFN_NewMeshDeformer   = void*(__stdcall*)(void*, const void*, int32_t);
using PFN_FreeMeshDeformer  = void(__stdcall*)(void*);
using PFN_DeformVertices    = void(__stdcall*)(void*, const int32_t*, const float*,
                                              int32_t, const void*, void*);
using PFN_PlayControlledAnim = void*(__stdcall*)(float, void*, void*);
using PFN_SetControlLoopCount = void(__stdcall*)(void*, int32_t);

struct Api {
  bool    ready = false;
  HMODULE dll   = nullptr;
  PFN_GetVersionString  GetVersionString = nullptr;
  PFN_ReadEntireFile    ReadEntireFile   = nullptr;
  PFN_FreeFile          FreeFile         = nullptr;
  PFN_GetFileInfo       GetFileInfo      = nullptr;
  PFN_GetMeshIndexCount GetMeshIndexCount= nullptr;
  PFN_CopyMeshIndices   CopyMeshIndices  = nullptr;
  PFN_GetMeshVertexCount GetMeshVertexCount = nullptr;
  PFN_GetMeshVertices   GetMeshVertices  = nullptr;
  PFN_CopyMeshVertices  CopyMeshVertices = nullptr;
  PFN_GetMeshVertexType GetMeshVertexType= nullptr;
  PFN_MeshIsRigid       MeshIsRigid      = nullptr;
  PFN_GetMeshTriGroupCount GetMeshTriGroupCount = nullptr;
  PFN_GetMeshTriGroups  GetMeshTriGroups = nullptr;
  PFN_GetMaterialTextureByType GetMaterialTextureByType = nullptr;
  PFN_TextureHasAlpha   TextureHasAlpha  = nullptr;
  PFN_CopyTextureImage  CopyTextureImage = nullptr;
  PFN_InstantiateModel  InstantiateModel = nullptr;
  PFN_GetSourceSkeleton GetSourceSkeleton= nullptr;
  PFN_NewLocalPose      NewLocalPose     = nullptr;
  PFN_NewWorldPose      NewWorldPose     = nullptr;
  PFN_FreeLocalPose     FreeLocalPose    = nullptr;
  PFN_FreeWorldPose     FreeWorldPose    = nullptr;
  PFN_SetModelClock     SetModelClock    = nullptr;
  PFN_SampleModelAnims  SampleModelAnimations = nullptr;
  PFN_BuildWorldPose    BuildWorldPose   = nullptr;
  PFN_FreeCompletedControls FreeCompletedModelControls = nullptr;
  PFN_GetWorldPoseComposite4x4Array GetWorldPoseComposite4x4Array = nullptr;
  PFN_GetWorldPoseComposite4x4 GetWorldPoseComposite4x4 = nullptr;
  PFN_NewMeshBinding    NewMeshBinding   = nullptr;
  PFN_FreeMeshBinding   FreeMeshBinding  = nullptr;
  PFN_GetMeshBindingToBoneIndices GetMeshBindingToBoneIndices = nullptr;
  PFN_NewMeshDeformer   NewMeshDeformer  = nullptr;
  PFN_FreeMeshDeformer  FreeMeshDeformer = nullptr;
  PFN_DeformVertices    DeformVertices   = nullptr;
  PFN_PlayControlledAnim PlayControlledAnimation = nullptr;
  PFN_SetControlLoopCount SetControlLoopCount = nullptr;
  const void* PNT332VertexType    = nullptr;
  const void* RGBA8888PixelFormat = nullptr;
};

Api g_api;

template <typename T>
bool Bind(HMODULE dll, const char* name, T* out) {
  *out = reinterpret_cast<T>(GetProcAddress(dll, name));
  return *out != nullptr;
}

// ── Le layout, tel que le CLIENT le lit ──────────────────────────────────────
//
// Ces offsets ne viennent pas d'un en-tête Granny (nous n'en avons pas) mais du
// client décompilé, et ils ont été revérifiés sur les six modèles livrés par
// tools/gr2_dump.cc — compteurs cohérents, noms lisibles, index dans les bornes.
template <typename T>
T At(const void* base, int offset) {
  T v;
  std::memcpy(&v, static_cast<const char*>(base) + offset, sizeof(T));
  return v;
}

// granny_file_info : les compteurs et leurs tableaux, deux dwords par famille.
int32_t FiTextureCount(const void* fi) { return At<int32_t>(fi, 16); }
void**  FiTextures(const void* fi)     { return At<void**>(fi, 20); }
int32_t FiModelCount(const void* fi)   { return At<int32_t>(fi, 64); }
void**  FiModels(const void* fi)       { return At<void**>(fi, 68); }
int32_t FiAnimCount(const void* fi)    { return At<int32_t>(fi, 80); }
void**  FiAnims(const void* fi)        { return At<void**>(fi, 84); }

// granny_model : Name, Skeleton, InitialPlacement (granny_transform = 68 o),
// MeshBindingCount, MeshBindings.
const char* ModelName(const void* m)     { return At<const char*>(m, 0); }
void*       ModelSkeleton(const void* m) { return At<void*>(m, 4); }
int32_t     ModelBindingCount(const void* m) { return At<int32_t>(m, 76); }
void**      ModelBindings(const void* m) { return At<void**>(m, 80); }

int32_t SkeletonBoneCount(const void* s) { return At<int32_t>(s, 4); }

// granny_mesh : Name, …, MaterialBindingCount, MaterialBindings.
const char* MeshName(const void* m)      { return At<const char*>(m, 0); }
int32_t     MeshMatBindCount(const void* m) { return At<int32_t>(m, 20); }
void**      MeshMatBindings(const void* m)  { return At<void**>(m, 24); }

// granny_texture : FromFileName, TextureType, Width, Height.
const char* TexName(const void* t)   { return At<const char*>(t, 0); }
int32_t     TexWidth(const void* t)  { return At<int32_t>(t, 8); }
int32_t     TexHeight(const void* t) { return At<int32_t>(t, 12); }

// granny_animation : Name, Duration.
float AnimDuration(const void* a) { return At<float>(a, 4); }

// granny_tri_material_group.
struct TriGroup {
  int32_t material_index;
  int32_t tri_first;
  int32_t tri_count;
};

std::string SafeName(const char* s) {
  if (!s || IsBadReadPtr(s, 1)) return std::string();
  for (int i = 0; i < 256; ++i) {
    if (s[i] == '\0') return std::string(s, s + i);
  }
  return std::string();
}

}  // namespace

bool GrannyReady(const char* dll_path) {
  if (g_api.ready) return true;

  // Dans le jeu, la DLL est DÉJÀ chargée — le client l'importe statiquement.
  // On prend celle-là : en charger une seconde copie donnerait deux
  // allocateurs Granny distincts sur les mêmes fichiers.
  g_api.dll = GetModuleHandleA("granny2.dll");
  if (!g_api.dll) g_api.dll = LoadLibraryA(dll_path ? dll_path : "granny2.dll");
  if (!g_api.dll) return false;

  HMODULE d = g_api.dll;
  bool ok = true;
  ok &= Bind(d, "_GrannyGetVersionString@0", &g_api.GetVersionString);
  ok &= Bind(d, "_GrannyReadEntireFileFromMemory@8", &g_api.ReadEntireFile);
  ok &= Bind(d, "_GrannyFreeFile@4", &g_api.FreeFile);
  ok &= Bind(d, "_GrannyGetFileInfo@4", &g_api.GetFileInfo);
  ok &= Bind(d, "_GrannyGetMeshIndexCount@4", &g_api.GetMeshIndexCount);
  ok &= Bind(d, "_GrannyCopyMeshIndices@12", &g_api.CopyMeshIndices);
  ok &= Bind(d, "_GrannyGetMeshVertexCount@4", &g_api.GetMeshVertexCount);
  ok &= Bind(d, "_GrannyGetMeshVertices@4", &g_api.GetMeshVertices);
  ok &= Bind(d, "_GrannyCopyMeshVertices@12", &g_api.CopyMeshVertices);
  ok &= Bind(d, "_GrannyGetMeshVertexType@4", &g_api.GetMeshVertexType);
  ok &= Bind(d, "_GrannyMeshIsRigid@4", &g_api.MeshIsRigid);
  ok &= Bind(d, "_GrannyGetMeshTriangleGroupCount@4", &g_api.GetMeshTriGroupCount);
  ok &= Bind(d, "_GrannyGetMeshTriangleGroups@4", &g_api.GetMeshTriGroups);
  ok &= Bind(d, "_GrannyGetMaterialTextureByType@8", &g_api.GetMaterialTextureByType);
  ok &= Bind(d, "_GrannyTextureHasAlpha@4", &g_api.TextureHasAlpha);
  ok &= Bind(d, "_GrannyCopyTextureImage@32", &g_api.CopyTextureImage);
  ok &= Bind(d, "_GrannyInstantiateModel@4", &g_api.InstantiateModel);
  ok &= Bind(d, "_GrannyGetSourceSkeleton@4", &g_api.GetSourceSkeleton);
  ok &= Bind(d, "_GrannyNewLocalPose@4", &g_api.NewLocalPose);
  ok &= Bind(d, "_GrannyNewWorldPose@4", &g_api.NewWorldPose);
  ok &= Bind(d, "_GrannyFreeLocalPose@4", &g_api.FreeLocalPose);
  ok &= Bind(d, "_GrannyFreeWorldPose@4", &g_api.FreeWorldPose);
  ok &= Bind(d, "_GrannySetModelClock@8", &g_api.SetModelClock);
  ok &= Bind(d, "_GrannySampleModelAnimations@16", &g_api.SampleModelAnimations);
  ok &= Bind(d, "_GrannyBuildWorldPose@24", &g_api.BuildWorldPose);
  ok &= Bind(d, "_GrannyFreeCompletedModelControls@4", &g_api.FreeCompletedModelControls);
  // 🔴 COMPOSITE, pas `GrannyGetWorldPose4x4Array`. Les deux existent, les deux
  // rendent un tableau de matrices de la même taille, et l'API ne refuse pas la
  // mauvaise : le skinning « marche » simplement de travers — les meshes d'un
  // même monstre partent chacun de leur côté, ce qui ressemble à un problème de
  // repère et n'en est pas un. Seules les composites annulent la pose de
  // liaison, et c'est bien la variante composite que le client emploie
  // (`GrannyGetWorldPoseComposite4x4` pour ses meshes rigides, 0x00724EF0).
  ok &= Bind(d, "_GrannyGetWorldPoseComposite4x4Array@4",
             &g_api.GetWorldPoseComposite4x4Array);
  ok &= Bind(d, "_GrannyGetWorldPoseComposite4x4@8", &g_api.GetWorldPoseComposite4x4);
  ok &= Bind(d, "_GrannyNewMeshBinding@12", &g_api.NewMeshBinding);
  ok &= Bind(d, "_GrannyFreeMeshBinding@4", &g_api.FreeMeshBinding);
  ok &= Bind(d, "_GrannyGetMeshBindingToBoneIndices@4", &g_api.GetMeshBindingToBoneIndices);
  ok &= Bind(d, "_GrannyNewMeshDeformer@12", &g_api.NewMeshDeformer);
  ok &= Bind(d, "_GrannyFreeMeshDeformer@4", &g_api.FreeMeshDeformer);
  ok &= Bind(d, "_GrannyDeformVertices@24", &g_api.DeformVertices);
  ok &= Bind(d, "_GrannyPlayControlledAnimation@12", &g_api.PlayControlledAnimation);
  ok &= Bind(d, "_GrannySetControlLoopCount@8", &g_api.SetControlLoopCount);

  void** pnt  = reinterpret_cast<void**>(GetProcAddress(d, "GrannyPNT332VertexType"));
  void** rgba = reinterpret_cast<void**>(GetProcAddress(d, "GrannyRGBA8888PixelFormat"));
  if (!pnt || !rgba) ok = false;
  else {
    g_api.PNT332VertexType    = *pnt;   // 🔴 le CONTENU, cf. l'en-tête
    g_api.RGBA8888PixelFormat = *rgba;
  }

  g_api.ready = ok;
  return ok;
}

const char* GrannyVersion() {
  if (!g_api.ready || !g_api.GetVersionString) return "";
  const char* v = g_api.GetVersionString();
  return v ? v : "";
}

bool LoadModel(std::vector<uint8_t> bytes, Model* out) {
  if (!out || bytes.empty() || !GrannyReady()) return false;
  *out = Model{};
  out->bytes = std::move(bytes);

  out->file = g_api.ReadEntireFile(static_cast<int32_t>(out->bytes.size()),
                                   out->bytes.data());
  if (!out->file) return false;
  void* fi = g_api.GetFileInfo(out->file);
  if (!fi || FiModelCount(fi) <= 0) { FreeModel(out); return false; }

  // ── Textures ───────────────────────────────────────────────────────────────
  // Toutes embarquées, décodées en RGBA8888 comme le fait `sub_721420`. Le
  // client, lui, réordonne ensuite en BGRA pour DirectX ; ici on garde du RGBA
  // et c'est l'affichage qui décidera — ce module ne connaît aucune API 3D.
  void** textures = FiTextures(fi);
  out->textures.resize(static_cast<size_t>(FiTextureCount(fi)));
  for (int i = 0; i < FiTextureCount(fi); ++i) {
    ModelTexture& t = out->textures[static_cast<size_t>(i)];
    void* gt = textures[i];
    t.name = SafeName(TexName(gt));
    t.w = TexWidth(gt);
    t.h = TexHeight(gt);
    t.has_alpha = g_api.TextureHasAlpha(gt) != 0;
    if (t.w <= 0 || t.h <= 0 || t.w > 4096 || t.h > 4096) { t.w = t.h = 0; continue; }
    t.rgba.resize(static_cast<size_t>(t.w) * t.h * 4);
    g_api.CopyTextureImage(gt, 0, 0, g_api.RGBA8888PixelFormat, t.w, t.h,
                           t.w * 4, t.rgba.data());
  }

  // ── Le modèle, son squelette, ses poses ────────────────────────────────────
  out->model    = FiModels(fi)[0];
  out->skeleton = ModelSkeleton(out->model);
  if (!out->skeleton) { FreeModel(out); return false; }
  out->bone_count = SkeletonBoneCount(out->skeleton);
  out->instance   = g_api.InstantiateModel(out->model);
  if (!out->instance || out->bone_count <= 0) { FreeModel(out); return false; }
  out->local_pose = g_api.NewLocalPose(out->bone_count);
  out->world_pose = g_api.NewWorldPose(out->bone_count);
  if (!out->local_pose || !out->world_pose) { FreeModel(out); return false; }

  // L'animation embarquée, jouée en boucle. Les six modèles livrés en ont
  // exactement une ; `model\3dmob_bone\<n>_<nom>.gr2` n'entre en jeu que pour
  // les poses SUPPLÉMENTAIRES (0x0071F600), dont la fiche n'a pas besoin.
  if (FiAnimCount(fi) > 0) {
    void* anim = FiAnims(fi)[0];
    out->animation_seconds = AnimDuration(anim);
    void* ctrl = g_api.PlayControlledAnimation(0.0f, anim, out->instance);
    if (ctrl) g_api.SetControlLoopCount(ctrl, 0);  // 0 = sans fin
  }

  // ── Meshes : indices, liaison au squelette, déformeur ──────────────────────
  const int32_t nmesh = ModelBindingCount(out->model);
  void** bindings = ModelBindings(out->model);
  out->meshes.reserve(static_cast<size_t>(nmesh > 0 ? nmesh : 0));
  for (int32_t i = 0; i < nmesh; ++i) {
    void* mesh = At<void*>(&bindings[i], 0);  // granny_model_mesh_binding = {Mesh}
    if (!mesh) continue;

    ModelMesh mm;
    mm.mesh = mesh;
    mm.name = SafeName(MeshName(mesh));
    mm.vertex_count = g_api.GetMeshVertexCount(mesh);
    const int32_t icount = g_api.GetMeshIndexCount(mesh);
    if (mm.vertex_count <= 0 || icount <= 0 || icount % 3 != 0) continue;

    // 2 octets par index, comme le client (`GrannyCopyMeshIndices(mesh, 2, …)`).
    mm.indices.resize(static_cast<size_t>(icount));
    g_api.CopyMeshIndices(mesh, 2, mm.indices.data());
    mm.posed.resize(static_cast<size_t>(mm.vertex_count));

    void* src_skel = g_api.GetSourceSkeleton(out->instance);
    mm.binding = g_api.NewMeshBinding(mesh, src_skel, src_skel);

    if (g_api.MeshIsRigid(mesh)) {
      // Mesh rigide : pas de déformeur, les sommets se lisent une fois pour
      // toutes et c'est la matrice de son os qui les place. Aucun des six
      // modèles livrés n'est dans ce cas, mais le client gère les deux et un
      // modèle ajouté demain pourrait l'être.
      g_api.CopyMeshVertices(mesh, g_api.PNT332VertexType, mm.posed.data());
    } else {
      mm.deformer = g_api.NewMeshDeformer(g_api.GetMeshVertexType(mesh),
                                          g_api.PNT332VertexType, 2);
      if (!mm.deformer) continue;  // sans déformeur, ce mesh ne se pose pas
    }

    // Groupes de triangles -> texture. Le client résout la texture diffuse par
    // `GrannyGetMaterialTextureByType(binding, 2)` puis compare le NOM DE
    // FICHIER (premier champ) à ceux du fichier : c'est ce qu'on refait, faute
    // d'index direct.
    const int32_t ngroup = g_api.GetMeshTriGroupCount(mesh);
    const TriGroup* groups = static_cast<const TriGroup*>(g_api.GetMeshTriGroups(mesh));
    const int32_t nbind = MeshMatBindCount(mesh);
    void** mbind = MeshMatBindings(mesh);
    for (int32_t gi = 0; gi < ngroup && groups; ++gi) {
      ModelPart part;
      part.tri_first = groups[gi].tri_first;
      part.tri_count = groups[gi].tri_count;
      const int32_t mi = groups[gi].material_index;
      if (mi >= 0 && mi < nbind && mbind) {
        void* tex = g_api.GetMaterialTextureByType(mbind[mi], 2);
        const std::string want = tex ? SafeName(TexName(tex)) : std::string();
        for (size_t k = 0; k < out->textures.size(); ++k) {
          if (!want.empty() && out->textures[k].name == want) {
            part.texture = static_cast<int>(k);
            break;
          }
        }
      }
      mm.parts.push_back(part);
    }
    if (mm.parts.empty()) {
      ModelPart part;
      part.tri_count = icount / 3;
      mm.parts.push_back(part);
    }

    out->meshes.push_back(std::move(mm));
  }
  if (out->meshes.empty()) { FreeModel(out); return false; }

  // Pose de repos, à la fois pour avoir des sommets exploitables tout de suite
  // et pour mesurer la boîte englobante qui servira au cadrage.
  PoseModel(out, 0.0f);
  bool first = true;
  for (const ModelMesh& mm : out->meshes) {
    for (const ModelVertex& v : mm.posed) {
      for (int k = 0; k < 3; ++k) {
        if (first || v.p[k] < out->bb_min[k]) out->bb_min[k] = v.p[k];
        if (first || v.p[k] > out->bb_max[k]) out->bb_max[k] = v.p[k];
      }
      first = false;
    }
  }
  return true;
}

void PoseModel(Model* m, float seconds) {
  if (!m || !m->instance || !g_api.ready) return;

  // Exactement la séquence du client (`sub_725350`, 0x00725350) : horloge,
  // échantillonnage dans la pose locale, construction de la pose monde. Le
  // `nullptr` est l'offset 4x4 — le client n'en pose pas non plus.
  void* src_skel = g_api.GetSourceSkeleton(m->instance);
  const int32_t bones = SkeletonBoneCount(src_skel);
  g_api.SetModelClock(m->instance, seconds);
  g_api.SampleModelAnimations(m->instance, 0, bones, m->local_pose);
  g_api.BuildWorldPose(src_skel, 0, bones, m->local_pose, nullptr, m->world_pose);
  g_api.FreeCompletedModelControls(m->instance);

  const float* mats = g_api.GetWorldPoseComposite4x4Array(m->world_pose);
  for (ModelMesh& mm : m->meshes) {
    if (!mm.deformer) {
      // Rigide : les sommets sont déjà lus ; il resterait à les multiplier par
      // `GrannyGetWorldPoseComposite4x4(worldPose, toBoneIndices[0])`, ce que
      // fait `sub_724EF0`. Rien à faire tant qu'aucun modèle livré n'est rigide,
      // et le jour où ça arrive, c'est ICI que ça se voit.
      continue;
    }
    const int32_t* to_bones = g_api.GetMeshBindingToBoneIndices(mm.binding);
    const void* src = g_api.GetMeshVertices(mm.mesh);
    if (!to_bones || !src || !mats) continue;
    g_api.DeformVertices(mm.deformer, to_bones, mats, mm.vertex_count, src,
                         mm.posed.data());
  }
}

void FreeModel(Model* m) {
  if (!m) return;
  if (g_api.ready) {
    for (ModelMesh& mm : m->meshes) {
      if (mm.deformer) g_api.FreeMeshDeformer(mm.deformer);
      if (mm.binding)  g_api.FreeMeshBinding(mm.binding);
    }
    if (m->world_pose) g_api.FreeWorldPose(m->world_pose);
    if (m->local_pose) g_api.FreeLocalPose(m->local_pose);
    if (m->file)       g_api.FreeFile(m->file);
  }
  *m = Model{};
}

}  // namespace ro
