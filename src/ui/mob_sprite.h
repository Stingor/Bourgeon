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
  SpriteRes sprite;
};

// Nom de RESSOURCE d'une classe, en CP949 — la brique commune à tout ce qui
// nomme un fichier de sprite. `sex` vaut -1 pour un monstre (le client résout
// lui-même, sa branche sort avant), 0 femme / 1 homme pour un personnage.
//
// Rend false si la classe n'a pas d'entrée `jobName` : aucun repli n'est
// inventé. Retomber sur « poring » est exactement le défaut que ce module
// corrige — l'appelant affiche son placeholder.
bool JobResName(int class_id, int sex, char* out, size_t out_size);

// Charge (ou retrouve en cache) les ressources du monstre `class_id`.
// Rend true quand le .spr et le .act sont exploitables.
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
