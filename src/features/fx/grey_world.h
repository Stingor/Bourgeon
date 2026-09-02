#pragma once

// ── GreyWorld : le monde dépouillé ───────────────────────────────────────────
//
// Le pendant, réglable et réversible, des GRF « greyworld » / « darkside » que la
// communauté RO se passe depuis vingt ans : on ôte le décor qui cache les
// entités, on rend le sol lisible, et on montre où l'on peut MARCHER.
//
// Un GRF le fait en remplaçant les fichiers de carte (.rsw vidé de ses modèles,
// .gnd retexturé en damier) : c'est irréversible, ça pèse 75 Mo, ça se périme à
// chaque nouvelle carte et il faut le distribuer. Ici tout se fait à l'exécution,
// sur les données que le client a déjà en mémoire — donc sur TOUTES les cartes,
// y compris celles ajoutées demain, et ça s'éteint d'un clic.
//
// Quatre leviers, indépendants, sous un interrupteur commun :
//
//   1. DÉCORS 3D — les maillages .rsm du .rsw (arbres, murs, ponts, props) ne
//      sont plus soumis au rendu. Les mobs et PNJ 3D ne bougent pas : eux
//      passent par Granny (.gr2), pas par le chemin .rsm.
//   2. QUADRILLAGE — un quad par cellule de la .gat, coloré selon qu'on peut y
//      marcher, que c'est un mur, ou que c'est infranchissable mais tirable.
//      C'est le client lui-même qui dessine (la fonction de son curseur de
//      destination), donc le relief est épousé et le tri de la scène respecté.
//   3. SOL UNI — le terrain repeint d'une couleur unie, par le mécanisme déjà
//      écrit pour le fond de capture des Staff Tools (cf. ground_paint.h).
//   4. APLATIR — les hauteurs du terrain réécrites à leur chargement.
//
// ⚠ PAS LE BROUILLARD, et c'est délibéré. Le client a `/fog`, que la fenêtre des
// réglages de Bourgeon expose déjà comme toutes les bascules de `CmdOnOffList` —
// même effet, appliqué par le handler natif et persisté dans `OptionInfo.lua`.
// Un levier de plus ici n'aurait été qu'une SECONDE case pour un seul état, que
// rien ne tenait d'accord avec la première.
//
// 🔴 CE QUI EST DX9 SEULEMENT : le sol uni (levier 3), parce qu'il encadre une
// passe du renderer DX9. Les autres travaillent EN AMONT du renderer — sur la
// file de scène, commune aux deux back-ends — et valent donc aussi en DX7.
//
// ⚠ CE N'EST PAS ground_paint. Le « Sol uni » des Staff Tools reste un fond de
// capture : une couleur, rien d'autre, et il ne dit rien des cellules. GreyWorld
// s'en sert comme d'une brique (levier 3) sans lui prendre son réglage : les deux
// peuvent être actifs, et c'est alors la couleur du staff qui gagne.

namespace grey_world {

// ── État persistable ─────────────────────────────────────────────────────────
// Un seul agrégat, exposé par référence : MoonlightUi::LoadSettings/SaveSettings
// le sérialise champ par champ dans bourgeon_settings.yaml (clés « greyworld_* »).
// Les couleurs sont au format du picker ImGui — 4 floats RGBA, à convertir en
// ARGB pour le client.
struct Config {
  bool enabled     = false;  // l'interrupteur commun
  bool hide_models = true;   // levier 1 : décors .rsm, au RENDU
  bool grid        = true;   // levier 2 : quadrillage des cellules
  bool flat_ground = true;   // levier 3 : sol uni (DX9 seulement)
  // Levier 4 : APLATIR le terrain. Le seul qui ne se voit pas tout de suite —
  // il réécrit les hauteurs à leur chargement, donc il prend effet au prochain
  // changement de carte. Ce n'est pas une gêne pour un réglage qu'on coche une
  // fois, pour essayer ou pour adopter.
  //
  // 🔴 IL EMPORTE LE DÉCHARGEMENT DES DÉCORS, et ce n'est PAS un choix offert au
  // joueur. Un décor garde sa hauteur d'origine : sur un sol aplati il flotte ou
  // s'enfonce, et — masqué ou non — il reste dans le quadrillage de sélection,
  // où le clic au sol bute dessus. Aplatir sans décharger est un état CASSÉ ;
  // le rendre atteignable ne produirait que des rapports de bug.
  bool flatten     = false;

  // Demi-côté, en cellules, du carré dessiné autour du joueur. C'est le seul
  // réglage qui coûte : le nombre de quads croît au CARRÉ (24 -> 2401 cellules).
  int radius = 24;

  // ── Le dessin d'une case, en deux axes indépendants ────────────────────────
  //
  // 1. AVEC QUOI on la peint : une texture du client, la même que celle qu'il
  //    emploie déjà pour ses propres marqueurs au sol. `grid.tga` est la croix
  //    de ton curseur de destination ; `SquareRange.tga` est le cadre carré des
  //    sorts de zone — c'est celle qui donne un carrelage. Le carré PLEIN n'a
  //    aucune texture dédiée dans le client : on l'obtient en n'échantillonnant
  //    qu'un point opaque d'une de ces deux-là (cf. kCellStyles dans le .cc).
  // 2. LESQUELLES on peint : toutes, ou seulement celles qui bordent un
  //    changement de terrain — la silhouette des obstacles.
  //
  // 🔴 PAS de « une case sur deux » : un filtre qui saute des cases sans
  // regarder leur type peut sauter un MUR, donc mentir sur le terrain. Le
  // contour, lui, garde toujours les bords : il retire de la matière sans jamais
  // perdre un obstacle. C'est la raison pour laquelle il est le seul filtre.
  enum Pattern { kPatternCross = 0, kPatternTile, kPatternSolid, kPatternCount };
  enum Fill     { kFillAll = 0, kFillOutline, kFillCount };

  int pattern = kPatternSolid;  // le carreau plein et son joint : le vrai greyworld
  int fill    = kFillAll;

  // Le JOINT, en pourcentage du côté de la case, pour le carreau plein : de
  // combien on rétrécit le carreau pour laisser voir le sol tout autour. C'est
  // ce vide qui dessine la bordure — le client n'a pas de texture de cadre fiable
  // et n'en a pas besoin.
  int gap = 12;

  // Ordre RGBA du picker. Les défauts vivent dans la table de réglages
  // (moonlight_ui.cc) ; ceux-ci ne servent qu'avant la première lecture du YAML.
  // ⚠ Ils doivent donc DIRE LA MÊME CHOSE qu'elle : deux valeurs voisines mais
  // différentes ne se voient pas à l'œil et se lisent comme un réglage déjà
  // modifié. `col_ground` = 0xFF2A2A2E, à l'octet près (42/255, 46/255).
  float col_ground[4] = {0.16471f, 0.16471f, 0.18039f, 1.00f};  // ardoise
  float col_walk[4]   = {1.00f, 1.00f, 1.00f, 0.16f};  // marchable, trait fin
  float col_block[4]  = {1.00f, 0.31f, 0.25f, 0.55f};  // mur
  float col_snipe[4]  = {1.00f, 0.75f, 0.25f, 0.55f};  // infranchissable, tirable
};

Config& cfg();

// Installe les hooks (idempotent, sûr à appeler chaque frame).
void EnsureInstalled();

// ── Lu au démarrage de la DLL, pas à l'entrée en jeu ─────────────────────────
// 🔴 `bourgeon_settings.yaml` n'est relu qu'à la transition vers le mode jeu,
// donc APRÈS le chargement de la première carte. Or l'aplatissement travaille au
// CHARGEMENT : sans cette lecture précoce, ses détours n'existent pas encore
// quand la carte d'entrée se charge, et le réglage ne prend qu'au changement de
// carte suivant — alors qu'entrer en jeu EST un chargement de carte.
//
// Elle relit les deux seuls champs qui doivent valoir dès cet instant, par
// `startup::Section` (qui retombe sur bourgeon_settings.yaml), et pose les
// détours. `LoadSettings` réécrira ensuite le même état, sans conséquence.
void LoadStartupState();

// À appeler quand `cfg()` vient de changer : propage ce qui ne peut pas être
// relu chaque frame — la demande de sol uni, qui se pose chez ground_paint.
void Apply();

// Contrôles ImGui — section « GreyWorld » du panneau Gameplay.
bool DrawSettings();

}  // namespace grey_world
