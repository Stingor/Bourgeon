#pragma once

#include "plugins/plugin.h"

// Avertissement « client lancé en DirectX 7 ».
//
// Le client sait rendre en DirectX 7 ou en DirectX 9 (Setup.exe -> onglet
// Graphics -> « Graphics API »), et Bourgeon suit les deux : l'overlay ImGui est
// dessiné soit par le proxy DirectDraw (ddraw/proxy_idirectdraw, chemin DX7),
// soit par le hook Direct3D9 (d3d9/d3d9_hook). Mais le chemin DX7 n'a ni shaders
// ni render target : tout ce qui en dépend est désactivé (post-traitement et
// FXAA de SettingsTweaks, capture d'écran propre, filtrage de textures, sprites
// réels des mini-jeux, SPR Effect Lab, DOOM...). Les joueurs restés en DX7
// remontent donc des « bugs » qui sont en réalité des fonctionnalités hors
// service.
//
// Ce plugin affiche UNE fois par session, dès la première frame en DX7 (donc à
// l'écran de login), une modale RO qui explique la marche à suivre et propose
// d'ouvrir le Setup. Rien n'est persisté : tant que le joueur reste en DX7 il
// revoit le message au lancement suivant — c'est l'incitation.
class Dx7Warning : public Plugin {
 public:
  const char* name() const override { return "Dx7Warning"; }

  // Login/char-select ET en jeu : la modale doit pouvoir apparaître au plus tôt,
  // mais aussi survivre au cas où la première frame ImGui arrive déjà en jeu.
  void OnRenderLoginUI() override { Draw(true); }
  void OnRenderUI() override { Draw(false); }

 private:
  // `at_login` : hors du monde de jeu, on peut proposer de quitter le client pour
  // ouvrir le Setup ; en jeu on ne coupe évidemment pas la session du joueur.
  void Draw(bool at_login);

  bool opened_    = false;  // popup armé côté ImGui (réarmé s'il est avalé)
  bool dismissed_ = false;  // acquitté par le joueur : plus rien de la session
  bool logged_    = false;  // trace « client en DX7 » écrite une seule fois
};
