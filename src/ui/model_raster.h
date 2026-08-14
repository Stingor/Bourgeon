#pragma once

// ── Rendu logiciel d'un modèle Granny vers une image RGBA ────────────────────
//
// Pourquoi en logiciel plutôt qu'en DX : le jeu tourne en DirectDraw7 OU en
// Direct3D9 selon le réglage du Setup, nos fenêtres doivent rendre pareil dans
// les deux, et un modèle de mob pèse 6 à 300 triangles — le coût CPU est du
// bruit. Un rasteriseur maison évite d'avoir à écrire, débugger et maintenir
// DEUX chemins graphiques pour six modèles.
//
// Le résultat est une image RGBA ordinaire : elle se téléverse ensuite comme
// n'importe quelle texture d'interface, ce que le projet sait déjà faire pour
// les sprites. Rendre APRÈS coup, dans une image, a un autre avantage : une
// animation peut être pré-rendue une fois en N images, et l'affichage
// redevient exactement celui d'un sprite — donc aucune texture créée pendant
// une frame ImGui (cf. feedback_texture_release_defer_frame).

#include <cstdint>
#include <vector>

#include "ui/gr2_model.h"

namespace ro {

struct ModelViewParams {
  int   width  = 128;
  int   height = 160;

  // Rotation autour de l'axe VERTICAL du modèle, en radians. Les modèles RO ont
  // Z pour hauteur (boîtes mesurées : z de 0 à ~25 pour un gardien, 41 pour le
  // drapeau) — ce n'est pas Y, et s'y tromper couche le monstre sur le flanc.
  float yaw = 0.6f;
  // Inclinaison de la caméra, en radians. 🔴 45°, et ce n'est pas un goût :
  // c'est l'angle de la caméra du jeu. `Camera_DragControl` (0x00C79F90) borne
  // la plongée (`camera+0x44`) autour de −45° à −60° — un modèle rendu à 20°
  // paraît « pas comme en jeu » sans qu'on sache dire pourquoi, parce que l'œil
  // compare à ce qu'il voit sur la carte juste à côté.
  float pitch = 0.785398f;

  // Marge autour du modèle, en fraction du cadre. Le cadrage est AUTOMATIQUE
  // (boîte englobante projetée) : un gardien et un coffre n'ont pas du tout la
  // même taille, et figer une échelle rendrait l'un minuscule ou l'autre coupé.
  float margin = 0.06f;

  // Rendu à 2× puis réduction. Sur des modèles de 300 triangles c'est gratuit,
  // et ça enlève l'escalier des silhouettes — très visible sur les arêtes d'un
  // cristal comme l'Emperium.
  bool supersample = true;

  // Éclairage, dans l'esprit de ce que le client passe à son propre rendu
  // (0x00C5BB10 : diffus 200,200,200, ambiant 128).
  float ambient = 0.50f;
  float diffuse = 0.78f;
};

// Rend le modèle DANS SA POSE COURANTE (appeler `PoseModel` avant). L'image
// sort en RGBA, 4 octets par pixel, ligne du haut d'abord, fond transparent.
// Rend false si le modèle n'a rien de dessinable.
bool RenderModelImage(const Model& model, const ModelViewParams& params,
                      std::vector<uint8_t>* out_rgba);

}  // namespace ro
