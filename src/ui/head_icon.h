#pragma once

#include "imgui.h"

// Miniature de TÊTE d'un personnage, rendue directement depuis le .spr natif —
// exactement ce que fait la fenêtre « Member » de la guilde pour chaque ligne
// (UIGuildMemberManageWnd, rendu de ligne 0x0091c7e0 : chemin de la coiffure ->
// SPR + ACT via le gestionnaire de ressources -> cellule -> page d'atlas -> blit).
//
// Sprites humains uniquement (인간족\머리통\<genre>\<id>_<genre>.spr). Une race
// sans ce sprite (Doram…) renvoie simplement false, à l'appelant de ne rien
// dessiner. Même mécanique que les icônes de coiffure du char-select, dont ce
// module est l'extraction réutilisable.
namespace ro {

// `hair` est l'id de coiffure BRUT (celui du serveur / des paquets) : le remap
// historique vers le numéro de fichier .spr des 12 premières coupes est appliqué
// à l'intérieur, comme le fait le rendu du jeu.
//
// Dessine la tête (coiffure `hair`, genre `sex` 0=femme/1=homme, couleur de
// cheveux `hair_color`, -1 = palette d'origine du sprite) tenant dans un carré de
// `box` pixels dont le coin haut-gauche est (x, y) en coordonnées ÉCRAN. Le
// sprite garde ses proportions et est centré dans le carré.
// Renvoie false si la ressource manque (rien n'est dessiné).
bool DrawHeadIcon(ImDrawList* draw_list, float x, float y, float box, int hair,
                  int sex, int hair_color);

}  // namespace ro
