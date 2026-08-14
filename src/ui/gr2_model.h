#pragma once

// ── Modèle Granny (.gr2) : chargement, pose, déformation ─────────────────────
//
// Une poignée de monstres ne sont pas des sprites. L'Emperium, les trois
// gardiens de forteresse, le drapeau de guilde et les coffres au trésor — 85
// classes pour 8 modèles — sont des ACTEURS 3D. Le client le sait à l'extension
// que lui rend `jobName.lub` (« Empelium90_0.gr2 » au lieu de « Chocho ») et
// bifurque vers `data\model\3dmob\%s` ; nos fenêtres, elles, cherchaient un
// `.spr` de ce nom et affichaient « pas de sprite ».
//
// 🔴 On n'écrit PAS de parseur : le format est propriétaire et compressé (GRF
// Editor lui-même ne le rend pas). `granny2.dll` est livrée avec le client, déjà
// chargée par lui, et fait tout le travail — y compris le skinning, que les six
// modèles exigent tous (aucun n'est rigide). Ce module ne fait que reproduire,
// appel pour appel, ce que le client fait déjà :
//   * `C3dGrannyModelRes_Load` (0x0071B9B0) — lecture, textures, squelette ;
//   * `sub_7211D0`             (0x007211D0) — liaisons de mesh et déformeurs ;
//   * `sub_725350`             (0x00725350) — horloge, échantillonnage, pose ;
//   * `sub_724EF0`             (0x00724EF0) — déformation des sommets.
// Le détail (offsets mesurés, pièges) est dans docs/mob_3d_re.md.
//
// Ce qui SORT d'ici, ce sont des triangles en espace monde et des textures RGBA :
// aucune dépendance à ImGui, à DirectX ni au client. C'est ce qui permet de le
// valider hors du jeu (tools/gr2_dump.cc) avant d'y toucher.

#include <cstdint>
#include <string>
#include <vector>

namespace ro {

// Le format de sommet que le client demande à Granny — position, normale, une
// coordonnée de texture. 32 octets : `sub_7211D0` alloue exactement
// `32 * vertexCount`, la taille est donc confirmée par le client.
struct ModelVertex {
  float p[3];
  float n[3];
  float uv[2];
};
static_assert(sizeof(ModelVertex) == 32, "PNT332 fait 32 octets");

// Une texture EMBARQUÉE dans le .gr2 (elles le sont toutes : le fichier porte
// jusqu'au chemin de la machine de l'artiste, « D:\ChalesWORK_2003\… »). Aucun
// accès au GRF n'est nécessaire pour un mob 3D.
struct ModelTexture {
  std::string          name;
  int                  w = 0;
  int                  h = 0;
  bool                 has_alpha = false;
  std::vector<uint8_t> rgba;  // 4 octets par pixel, ligne du haut d'abord
};

// Une tranche de triangles d'un mesh partageant la même texture.
struct ModelPart {
  int texture   = -1;  // index dans Model::textures, -1 = aucune
  int tri_first = 0;
  int tri_count = 0;
};

struct ModelMesh {
  std::string              name;
  void*                    mesh     = nullptr;  // granny_mesh*
  void*                    binding  = nullptr;  // granny_mesh_binding*
  void*                    deformer = nullptr;  // nul si le mesh est rigide
  int                      vertex_count = 0;
  std::vector<uint16_t>    indices;
  std::vector<ModelPart>   parts;
  // Sommets APRÈS déformation, en espace monde. Rempli par `PoseModel`.
  std::vector<ModelVertex> posed;
};

struct Model {
  // 🔴 Granny lit le fichier EN PLACE et garde des pointeurs dedans : ce tampon
  // doit vivre aussi longtemps que le modèle. Le déplacer (realloc, copie de
  // vecteur) invaliderait tout d'un coup et en silence.
  std::vector<uint8_t> bytes;

  void* file       = nullptr;  // granny_file*
  void* model      = nullptr;  // granny_model*
  void* instance   = nullptr;  // granny_model_instance*
  void* skeleton   = nullptr;  // granny_skeleton*
  void* local_pose = nullptr;
  void* world_pose = nullptr;
  int   bone_count = 0;

  // Durée de l'animation embarquée, en secondes (0 si le fichier n'en porte
  // aucune). Les six modèles livrés en ont une : de 2 s pour un gardien à 9,9 s
  // pour l'Emperium.
  float animation_seconds = 0.0f;

  std::vector<ModelTexture> textures;
  std::vector<ModelMesh>    meshes;

  // Boîte englobante de la pose de repos, pour cadrer sans deviner.
  float bb_min[3] = {0, 0, 0};
  float bb_max[3] = {0, 0, 0};
};

// `granny2.dll` est-elle joignable ? Dans le jeu elle l'est toujours (le client
// l'importe statiquement) ; hors du jeu il faut la lui indiquer.
bool GrannyReady(const char* dll_path = nullptr);

// Version rendue par la DLL (« 2.1.0.5 » attendue — le client refuse le reste,
// cf. `GrannyVersionsMatch(2,1,0,5)` en 0x0071B9C1). Jamais nulle.
const char* GrannyVersion();

// Charge un modèle depuis les octets d'un `.gr2`. `bytes` est CONSOMMÉ (déplacé
// dans le modèle) — voir la note sur la durée de vie ci-dessus.
bool LoadModel(std::vector<uint8_t> bytes, Model* out);

// Place le modèle à l'instant `seconds` de son animation et déforme ses
// sommets. Sans animation, rend la pose de repos. À appeler avant de lire
// `ModelMesh::posed`.
void PoseModel(Model* m, float seconds);

void FreeModel(Model* m);

}  // namespace ro
