#pragma once

// ── HUD caméra (outil du staff) ──────────────────────────────────────────────
// Affiche EN VALEURS tout ce qui règle la caméra du monde : la pose courante et
// sa cible (tilt, rotation, zoom, point visé, œil), les bornes que le moteur
// applique au glisser et à la molette, la mémoire du zoom par genre de carte,
// la ligne de `viewpointtable` de la carte, et les deux commandes qui agissent
// sur le rig (/camera, /zoom).
//
// LECTURE SEULE : le HUD n'écrit rien, ni dans la caméra ni dans les globaux.
// Les modules qui la pilotent (dézoom étendu, écran de veille, vue FPS) restent
// seuls maîtres — le HUD sert justement à voir ce qu'ils font.
//
// Gaté comme le reste de Staff Tools : IsStaff() revérifié à chaque frame par
// l'appelant, et en jeu seulement.

namespace camera_hud {

// « staff_camera_hud », persisté par MoonlightUi. Indépendant de l'ouverture de
// la fenêtre Staff Tools : on referme l'établi et le HUD reste à l'écran.
bool& enabled();

// Case à cocher + aide, pour la fenêtre Staff Tools. Vrai si à persister.
bool DrawSettings();

// Le HUD lui-même. Ne dessine rien si désactivé ou hors du monde.
void Draw();

}  // namespace camera_hud
