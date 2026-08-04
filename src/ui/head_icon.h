#pragma once

#include "imgui.h"

// Miniature de TÊTE d'un personnage — ce que la fenêtre « Member » de la guilde
// affiche sur chaque ligne (UIGuildMemberManageWnd, rendu de ligne 0x0091c7e0).
//
// Le sprite passe par ui/sprite_view.h, donc par notre propre parseur : plus
// d'atlas, plus de cellule, plus de palette lue à un offset deviné dans
// CPaletteRes. La couleur de cheveux est un simple .pal externe appliqué aux
// index de l'image, exactement comme le fait le jeu.
//
// Toutes races : le dossier racine (인간족 / 도람족) et la table de numéros de
// coiffure découlent de la CLASSE, comme chez le client. D'où le `job` exigé
// partout ci-dessous — sans lui, un Doram (Summoner) irait chercher son sprite
// dans l'arborescence humaine et ne trouverait rien.
namespace ro {

// `hair` est l'id de coiffure BRUT (celui du serveur / des paquets) : le remap
// vers le numéro de fichier .spr passe par la table native, comme le fait le
// rendu du jeu.
//
// Dessine la tête (coiffure `hair`, genre `sex` 0=femme/1=homme, couleur de
// cheveux `hair_color`, -1 = palette d'origine du sprite) tenant dans un carré de
// `box` pixels dont le coin haut-gauche est (x, y) en coordonnées ÉCRAN. Le
// sprite garde ses proportions et est centré dans le carré.
//
// `allow_upscale` autorise l'AGRANDISSEMENT quand la tête est plus petite que la
// case. Les deux réglages ont leur emploi : sur une ligne de liste (hauteur de
// texte) on ne fait que RÉDUIRE, tandis qu'une grille de sélection de coiffure
// veut des vignettes qui remplissent leur case.
//
// `job` = la classe du personnage. Elle choisit la RACE, donc le dossier et la
// table de coiffures ; 0 (Novice) pour une vignette qui n'en a pas d'autre.
// Sans défaut, exprès : une valeur oubliée passerait inaperçue sur un humain et
// ne se verrait que sur un Doram.
//
// Renvoie false si la ressource manque (rien n'est dessiné).
bool DrawHeadIcon(ImDrawList* draw_list, float x, float y, float box, int hair,
                  int sex, int hair_color, bool allow_upscale, int job);

// ── Chemins, pour qui compose lui-même (ui/doll.cc) ──────────────────────────
// Exposés pour que le pantin n'ait pas à recopier ni les gabarits CP949 ni la
// résolution du numéro de coiffure : deux copies finiraient par diverger.

// Chemin VFS du sprite de tête, SANS extension. `hair` est l'id BRUT (le remap
// vers le numéro de fichier est fait ici). false si l'écriture ne tient pas.
bool HeadSpriteBasePath(int hair, int sex, int job, char* out, size_t out_size);

// Chemin VFS de la palette de couleur de cheveux. false si `color` < 0.
//
// ⚠ `job` n'est pas décoratif : chaque RACE a son dossier de palettes et elles
// ne sont PAS interchangeables (indexations différentes — voir le .cc). Le nom
// est unisexe des deux côtés grâce aux patchs WARP `HeadPalUnisex` et
// `DoramHeadPalShared`.
bool HairPalettePath(int color, int job, char* out, size_t out_size);

}  // namespace ro
