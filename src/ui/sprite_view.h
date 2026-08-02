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
  int   palette = 0;       // teinte à utiliser ; 0 = celle du .spr
};

// Charge (ou retrouve en cache) la paire `<base_path>.spr` / `<base_path>.act`.
// Idempotent tant que la poignée porte déjà ce chemin.
// Rend true quand les deux fichiers sont exploitables.
bool LoadSprite(const char* base_path, SpriteRes* res);

// Même chose, mais les images PALETTISÉES sont recolorées par le .pal de
// `pal_path` (chemin VFS complet, `data\` compris — ex.
// `data\palette\머리\head_3.pal`). C'est ainsi que RO teinte cheveux et
// vêtements : le sprite ne porte qu'une couleur par défaut.
//
// Le .spr n'est analysé QU'UNE fois par chemin, quelle que soit la teinte : une
// palette n'ajoute qu'un jeu de textures à l'entrée existante. Une palette
// introuvable n'est pas une erreur — on retombe sur celle du .spr, comme le
// fait le jeu.
bool LoadSpriteRecolored(const char* base_path, const char* pal_path,
                         SpriteRes* res);

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

// ── Composition de plusieurs sprites ─────────────────────────────────────────
//
// `DrawSprite` suffit quand on affiche UN sprite. Un personnage RO, lui, est un
// ASSEMBLAGE : corps, tête, coiffes, chacun son .spr/.act, recalés les uns sur
// les autres. Les deux fonctions ci-dessous donnent la matière première.

// Un calque résolu : sa texture et ses quatre coins, en unités .act (repère du
// sprite, origine = son point d'ancrage). C'est à l'appelant de translater,
// mettre à l'échelle et dessiner.
struct SpriteQuad {
  void*  tex = nullptr;
  ImVec2 uv0{0.0f, 0.0f}, uv1{1.0f, 1.0f};
  ImVec2 corner[4];  // TL, TR, BR, BL — rotation déjà appliquée
  ImU32  tint = IM_COL32_WHITE;
};

// Résout les calques de (action, frame) et téléverse leurs textures.
// Rend le nombre de calques écrits dans `out` (0 si rien).
int SpriteResolveFrame(const SpriteRes& res, unsigned action, unsigned frame,
                       SpriteQuad* out, int max_out);

// Ancre de RÉFÉRENCE de (action, frame) — la première de l'image.
//
// 🔴 C'est par elle que RO accroche une pièce à une autre : le décalage à
// appliquer à l'enfant vaut `ancre_parent - ancre_enfant`, et UNIQUEMENT si les
// deux `attr` sont égaux (le natif s'en assure ; sans ce test on attache des
// pièces qui n'ont rien à voir). Rend false s'il n'y a pas d'ancre — l'appelant
// traite alors le décalage comme nul.
//
// ⚠ Pour une action < 8, le natif lit l'ancre sur l'IMAGE 0 et non sur l'image
// courante. Sans ça les pièces vibrent pendant l'animation d'attente.
bool SpriteFrameAnchor(const SpriteRes& res, unsigned action, unsigned frame,
                       int* x, int* y, int* attr);

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
