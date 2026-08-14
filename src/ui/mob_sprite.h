#pragma once

// ── Sprite d'un monstre, par son id de classe ────────────────────────────────
//
// Tout le travail est fait par ui/sprite_view.h ; il ne reste ici qu'UNE chose
// propre aux monstres : traduire un id de classe en nom de fichier.
//
// 🔴 Cette traduction passe par `Job_GetDisplayNameOrResName` (table
// `jobName.lub`), et surtout PAS par `Monster_GetResNameById` — un switch en dur
// d'environ 130 monstres dont le `default:` renvoie « poring », ce qui affichait
// un Poring sur tout le bestiaire. Elle doit rester NATIVE : le client charge
// jobName_F.lub puis jobName.lub, et un override posé dans `data\` prime sur le
// GRF ; seule sa table dit ce qui a RÉELLEMENT été chargé.

#include "imgui.h"

#include "ui/sprite_view.h"

namespace ro {

// Poignée : la ressource plus l'id qu'elle représente, pour rester idempotent.
struct MobSpriteRes {
  int       class_id = -1;
  bool      failed   = false;
  // 🔴 La classe est rendue par un MODÈLE 3D, pas par un sprite : elle n'a
  // AUCUN .spr, et en chercher un est une erreur, pas un accident. `model`
  // porte alors le nom du fichier tel que la table le donne, extension
  // comprise (« Empelium90_0.gr2 »), à compléter en `data\model\3dmob\<model>`.
  bool      is_model = false;
  char      model[64] = {0};
  SpriteRes sprite;
};

// Charge (ou retrouve en cache) les ressources du monstre `class_id`.
// Rend true quand le .spr et le .act sont exploitables.
//
// 🔴 false ne veut pas dire « pas d'art » : une poignée de classes (Emperium,
// gardiens de forteresse, drapeau de guilde, coffres au trésor — 85 en tout,
// pour 8 modèles distincts) sont des ACTEURS 3D. Le client le sait à
// l'extension que lui rend `jobName.lub` (« Empelium90_0.gr2 » au lieu de
// « Chocho ») et bifurque vers `model\3dmob\%s` — il n'existe pas de liste d'ids
// à recopier, l'extension EST le marqueur. Ces classes-là ressortent avec
// `is_model = true` : à l'appelant de dire « modèle 3D » plutôt que « pas de
// sprite », qui serait un mensonge.
bool LoadMobSprite(int class_id, MobSpriteRes* res);

// Nombre d'images de l'action (0 = idle, orientation sud).
int MobActionFrameCount(const MobSpriteRes& res, unsigned action);

// Dessine le monstre. Voir `ro::DrawSprite` pour le détail des paramètres ;
// `allow_upscale` est false par défaut pour que les petits monstres gardent
// leur taille réelle — c'est une information de gabarit.
bool DrawMobSprite(ImDrawList* draw_list, const MobSpriteRes& res,
                   ImVec2 rect_min, ImVec2 rect_max, float anim_seconds,
                   unsigned action = 0, float ms_per_frame = 130.0f,
                   bool allow_upscale = false, float alpha = 1.0f);

}  // namespace ro
