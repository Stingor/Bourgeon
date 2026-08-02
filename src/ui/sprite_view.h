#pragma once

// ── Afficher N'IMPORTE QUEL .spr/.act dans ImGui ─────────────────────────────
//
// La brique générique posée sur ui/spr_act.h : chargement par CHEMIN, cache,
// téléversement paresseux des textures, composition des calques, cadrage et
// animation. Elle ne connaît ni les monstres, ni les jobs, ni les coiffes —
// juste un couple de fichiers.
//
// C'est le seul chemin à utiliser pour dessiner un sprite RO en ImGui. Huit
// modules portaient chacun leur propre copie de la chaîne native (résolution de
// nom -> TexMgr -> atlas de sprites, avec des offsets de structure devinés) ;
// tous peuvent passer par ici, et n'auront alors plus rien à recaler au prochain
// Ragexe.
//
// ⚠ Les chemins sont ceux du VFS, préfixe `data\` compris, en CP949 — par
// exemple `data\sprite\몬스터\Chocho`. Voir ui/spr_act.h : le client applique
// DEUX préfixes successifs (`sprite\` selon l'extension, puis `data\`), et on
// court-circuite les deux.
//
// ⚠ Nos sources sont en UTF-8 : un littéral coréen écrit ici ne désignerait
// aucun dossier du GRF. Les appelants construisent leurs chemins à partir des
// gabarits CP949 du binaire du client (cf. ui/mob_sprite.cc).

#include "imgui.h"

namespace ro {

// Poignée vers une paire chargée. Le contenu appartient à un cache global :
// copiable, jetable, et sa destruction ne libère rien.
struct SpriteRes {
  void* res    = nullptr;  // entrée du cache, nullptr tant que rien n'est chargé
  bool  failed = false;    // chargement tenté et raté (ne pas réessayer en boucle)
};

// Charge (ou retrouve en cache) la paire `<base_path>.spr` / `<base_path>.act`.
// Idempotent tant que la poignée porte déjà ce chemin.
// Rend true quand les deux fichiers sont exploitables.
bool LoadSprite(const char* base_path, SpriteRes* res);

// Nombre d'images de `action`. Les .act rangent les actions par
// motion * 8 + direction ; 0 = première pose, orientation sud.
// 0 si la ressource n'est pas chargée ou si l'action n'existe pas.
int SpriteActionFrameCount(const SpriteRes& res, unsigned action);

// Cadence déclarée par le .act pour cette action, en millisecondes par image.
// 0 si indisponible. C'est elle qui donne la bonne vitesse d'animation — une
// constante en dur rend la plupart des sprites trop lents.
float SpriteFrameIntervalMs(const SpriteRes& res, unsigned action);

// Index de l'image affichée à l'instant `anim_seconds`, pour les MÊMES
// paramètres que `DrawSprite` — et par le même calcul, donc la même image.
//
// C'est ce qu'il faut pour caler autre chose que le dessin sur l'animation : un
// son d'image, par exemple, ne doit se déclencher qu'au CHANGEMENT d'image, et
// sur celle qui est vraiment à l'écran. Recalculer la cadence de son côté ferait
// dériver le son du dessin dès que le .act déclare la sienne.
unsigned SpriteFrameIndex(const SpriteRes& res, unsigned action,
                          float anim_seconds, float ms_per_frame = 130.0f);

// Nom du fichier son attaché à (action, frame), ou nullptr s'il n'y en a pas.
//
// ⚠ La table de sons d'un .act contient AUSSI des marqueurs d'animation, pas
// seulement des .wav : seules les entrées se terminant par « .wav » sont rendues.
// Les jouer sans ce filtre fait passer des marqueurs pour des sons.
const char* SpriteFrameSound(const SpriteRes& res, unsigned action,
                             unsigned frame);

// Premier son déclaré par le .act, toutes actions confondues — la « voix » du
// sprite, celle qu'on joue sur une interaction ponctuelle. nullptr s'il n'y en
// a aucun, ce qui est le cas de la plupart des .act.
const char* SpriteMainSound(const SpriteRes& res);

// Dessine le sprite dans le rectangle [rect_min, rect_max].
//
//   anim_seconds  horloge continue de l'appelant (ex. ImGui::GetTime()) ; c'est
//                 elle qui fait tourner l'animation. Une constante fige l'image.
//   ms_per_frame  REPLI et interrupteur : 0 fige sur la première image. Dès que
//                 le .act déclare une cadence pour l'action, c'est ELLE qui
//                 s'applique.
//   allow_upscale autorise l'AGRANDISSEMENT quand le sprite est plus petit que
//                 la boîte. Défaut false : la mise à l'échelle ne sert qu'à
//                 faire RENTRER ce qui dépasse, sinon on perd l'information de
//                 gabarit (un Poring et un Baphomet à la même taille).
//
// Rend false si rien n'a pu être dessiné : l'appelant pose alors son repli.
bool DrawSprite(ImDrawList* draw_list, const SpriteRes& res, ImVec2 rect_min,
                ImVec2 rect_max, float anim_seconds, unsigned action = 0,
                float ms_per_frame = 130.0f, bool allow_upscale = false,
                float alpha = 1.0f);

}  // namespace ro
