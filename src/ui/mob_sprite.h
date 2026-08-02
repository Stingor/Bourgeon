#pragma once

// ── Sprite de monstre (.spr/.act) rendu en ImGui ─────────────────────────────
// Charge les ressources d'un monstre par son id de classe et dessine son
// animation, calques compris, dans un rectangle ImGui.
//
// 🔴 Les fichiers sont lus et interprétés PAR NOUS (ui/spr_act.h, port de GRF
// Editor). Ce module n'appelle plus l'atlas de sprites du client, ni ses
// accesseurs de .act, ni ses structures CSprite/CAction. Il ne reste du natif
// que deux choses, chacune pour une raison :
//
//  1. `Job_GetDisplayNameOrResName` — la table `jobName.lub`, qui traduit un id
//     de classe en NOM DE DOSSIER. Elle doit rester native : le client charge
//     jobName_F.lub puis jobName.lub, et un override posé dans `data\` prime sur
//     le GRF ; seule sa table dit ce qui a RÉELLEMENT été chargé. (Et surtout
//     PAS `Monster_GetResNameById`, un switch en dur d'environ 130 monstres dont
//     le `default:` renvoie « poring » — c'est ce qui affichait un Poring sur
//     tout le bestiaire.)
//  2. `FileMgr_LoadToMemory` — le VFS, indispensable parce que `data.grf` est
//     CHIFFRÉ. Voir ui/spr_act.h.
//
// Différence avec login_parade : celui-ci ne dessine QUE le plus grand calque
// (heuristique « le corps, pas l'ombre »), ce qui suffit à un Poring. Ici on
// dessine TOUS les calques à leur position/échelle/miroir/teinte propres, ce
// qui est indispensable dès qu'un monstre est composé (aile + corps, monture,
// arme, flammes…).
//
// ⚠ Géométrie : le calque est CENTRÉ sur (OffsetX, OffsetY), et l'offset n'est
// PAS multiplié par l'échelle du calque. C'est `Plane.FromLayer` de GRF Editor
// à la lettre, et c'est aussi ce que fait le rendu d'acteur du client
// (`Actor_DrawSprites` 0x007AC820 : `acteur.x + layer.OffsetX`). Le détail de la
// composition — et le pourquoi de l'erreur inverse commise avant — est dans le
// .cc, au-dessus du code.

#include "imgui.h"

namespace ro {

// Poignée vers les ressources d'un monstre. Le contenu appartient à un cache
// global (ui/mob_sprite.cc) : cette structure est copiable, jetable, et sa
// destruction ne libère rien.
struct MobSpriteRes {
  int   class_id = -1;
  bool  failed   = false;   // chargement tenté et raté (ne pas réessayer en boucle)
  void* res      = nullptr; // entrée du cache, nullptr tant que rien n'est chargé
};

// Charge (ou retrouve en cache) les ressources du monstre `class_id`.
// Idempotent : si `res` porte déjà ce class_id, rien n'est fait.
// Rend true quand le .spr ET le .act sont exploitables.
bool LoadMobSprite(int class_id, MobSpriteRes* res);

// Nombre d'images de l'action `action` (0 = idle, orientation sud).
// 0 si la ressource n'est pas chargée ou si l'action n'existe pas.
int MobActionFrameCount(const MobSpriteRes& res, unsigned action);

// Dessine le monstre dans le rectangle [rect_min, rect_max].
//
//   anim_seconds  horloge continue de l'appelant (ex. ImGui::GetTime()) ; c'est
//                 elle qui fait tourner l'animation. Passer une constante fige
//                 l'image, comme le fait le natif.
//   action        0 = idle sud. Les .act de mob rangent les actions par
//                 motion*8 + direction.
//   ms_per_frame  REPLI seulement, et interrupteur : 0 fige l'animation sur la
//                 première image (comportement du natif). Dès que le .act
//                 déclare une cadence pour l'action — `Action.speed`, en ticks
//                 de 25 ms — c'est ELLE qui s'applique. Une constante en dur
//                 rendait l'animation trop lente sur la plupart des monstres.
//   allow_upscale autorise l'AGRANDISSEMENT quand le sprite est plus petit que
//                 la boîte. Défaut **false**, et c'est le bon défaut : un mob
//                 doit garder sa taille réelle, sinon un Poring et un Baphomet
//                 s'affichent à la même échelle et on perd l'information de
//                 gabarit. La mise à l'échelle ne sert qu'à faire RENTRER ce qui
//                 dépasse — c'est aussi ce que fait le natif.
//
// Rend false si rien n'a pu être dessiné : l'appelant pose alors son repli.
bool DrawMobSprite(ImDrawList* draw_list, const MobSpriteRes& res,
                   ImVec2 rect_min, ImVec2 rect_max, float anim_seconds,
                   unsigned action = 0, float ms_per_frame = 130.0f,
                   bool allow_upscale = false, float alpha = 1.0f);

}  // namespace ro
