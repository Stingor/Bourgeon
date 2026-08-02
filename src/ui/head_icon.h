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
// Sprites humains uniquement (인간족\머리통\<genre>\<id>_<genre>.spr). Une race
// sans ce sprite (Doram…) renvoie simplement false, à l'appelant de ne rien
// dessiner.
namespace ro {

// `hair` est l'id de coiffure BRUT (celui du serveur / des paquets) : le remap
// historique vers le numéro de fichier .spr des 12 premières coupes est appliqué
// à l'intérieur, comme le fait le rendu du jeu.
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
// Renvoie false si la ressource manque (rien n'est dessiné).
bool DrawHeadIcon(ImDrawList* draw_list, float x, float y, float box, int hair,
                  int sex, int hair_color, bool allow_upscale = false);

}  // namespace ro
