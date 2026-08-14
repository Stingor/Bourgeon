#pragma once

// ── Le modèle 3D d'un monstre, dessiné comme un sprite ───────────────────────
//
// Les six modèles Granny du bestiaire (Emperium, les trois gardiens, le drapeau
// de guilde, le coffre au trésor) n'ont pas de `.spr` : c'est le moteur 3D du
// client qui les rend. Nos fenêtres, elles, dessinent des images.
//
// 🔴 D'où le parti pris : on PRÉ-REND l'animation en images, une fois, et
// l'affichage redevient exactement celui d'un sprite. Ce n'est pas un
// contournement mais le bon compromis :
//   * aucune texture n'est créée à chaque frame — la libération d'une texture
//     encore référencée par une draw-list corrompt le tas (0xC0000374), et le
//     projet a déjà payé ce prix ailleurs ;
//   * ça marche à l'identique en DirectDraw7 et en Direct3D9, sans écrire deux
//     chemins graphiques pour six modèles ;
//   * le coût est payé une fois à l'ouverture de la fiche, pas 60 fois par
//     seconde.
//
// La rotation à la molette re-rend la série : c'est 40 rasterisations de
// quelques centaines de triangles, soit un cran de molette qui coûte quelques
// dizaines de millisecondes — visible seulement si on cherche à le voir.

#include "imgui.h"

#include <cstdint>
#include <string>
#include <vector>

#include "ui/gr2_model.h"

namespace ro {

struct MobModelRes {
  // Nom de fichier tel que `jobName.lub` le donne, extension comprise
  // (« Empelium90_0.gr2 »). Sert de clé : recharger le même ne fait rien.
  std::string name;
  bool  loaded = false;
  bool  failed = false;

  Model model;

  // Une texture par image de l'animation, dans l'orientation `yaw`.
  std::vector<void*> frames;
  int   width  = 0;
  int   height = 0;
  float ms_per_frame = 100.0f;
  float yaw = 0.0f;

  // Textures de la série précédente, à relâcher UNE FOIS que la frame qui a pu
  // les dessiner est passée. Cf. la règle du projet sur les libérations
  // différées.
  std::vector<void*> retiring;
  unsigned device_epoch = 0;
};

// Charge le modèle `model_name` (ex. « Empelium90_0.gr2 ») depuis
// `data\model\3dmob\` via le VFS du client, et pré-rend son animation à la
// taille demandée. Idempotent : rappeler avec les mêmes paramètres ne refait
// rien. Rend true quand il y a des images à dessiner.
bool LoadMobModel(const char* model_name, int width, int height,
                  MobModelRes* res);

// Change l'orientation et re-rend la série. Sans effet si l'angle est le même.
void SetMobModelYaw(MobModelRes* res, float yaw);

// Dessine l'image correspondant à `anim_seconds` (0 = première image, pas
// d'animation). Le modèle est cadré DANS le rectangle, en conservant ses
// proportions. Rend false s'il n'y a rien à dessiner.
bool DrawMobModel(ImDrawList* draw_list, MobModelRes* res, ImVec2 rect_min,
                  ImVec2 rect_max, float anim_seconds, float alpha = 1.0f);

void FreeMobModel(MobModelRes* res);

}  // namespace ro
