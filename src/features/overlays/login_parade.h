#pragma once

#include "features/plugin.h"

// LoginParade — fait flâner une petite bande de monstres de la famille Poring
// sur l'écran de login. Purement cosmétique : les sprites .spr/.act sont chargés
// via le système de ressources natif (même chemin éprouvé que le Poring de
// [[roggle_tweaks]]) et dessinés en ImGui par-dessus le fond de login natif —
// aucun acteur en scène, aucun monde/terrain requis (l'atlas de sprites est
// valide dès le boot, pas seulement en jeu).
//
// Comportement : chaque Poring sautille d'un bord à l'autre à sa hauteur, fait
// des pauses, se retourne aux bords ; un clic de souris dessus le fait sursauter
// et détaler. Les sprites s'estompent au-dessus du panneau de login central pour
// ne jamais gêner la saisie ID/mot de passe.
//
// Ne s'affiche qu'au mode login (le client démarre au login -> in_login_ = true).
// Rendu ImGui : marche en DX9 comme en DX7 (avec repli forme si l'atlas échoue).
class LoginParade : public Plugin {
 public:
  const char* name() const override { return "LoginParade"; }

  // Hook DÉDIÉ login/char-select : OnRenderUI est court-circuité hors jeu par
  // Bourgeon::RenderUI (choke point), donc on dessine ici (appelé uniquement quand
  // le monde n'est PAS actif, dans une frame ImGui vivante).
  void OnRenderLoginUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Réglage opt-in persisté par [[moonlight_ui]] (défaut ON : c'est tout l'intérêt).
  // Accès direct par MoonlightUi::Load/SaveSettings, comme les autres plugins.
  // Le son suit ce toggle + la config son globale du jeu (pas de réglage séparé).
  bool enabled_ = true;
};
