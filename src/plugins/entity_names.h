#pragma once

#include "plugins/plugin.h"

// EntityNamesTweaks — affiche EN PERMANENCE le nom flottant au-dessus des
// entités (joueurs / monstres / NPC), pas seulement au survol de la souris.
//
// Le client natif ne dessine le label de nom que pour l'entité SURVOLÉE (un
// widget unique réutilisé, cf. docs/entity_nameplate_re.md). Ce plugin
// reproduit l'affichage pour toutes les entités visibles via un overlay ImGui :
// chaque frame il parcourt la liste d'acteurs du GameMode, lit la position
// écran déjà projetée par le moteur (+0xac/+0xb0), récupère le nom dans le
// dictionnaire natif (CNameDict, qui demande aussi au serveur les noms encore
// inconnus) et dessine le texte aux pieds de l'entité — comme le fait le jeu.
//
// Lecture seule côté moteur : aucun hook, aucun état natif modifié. Tout est
// gardé par __try et par le timestamp du client (20250716).
class EntityNamesTweaks : public Plugin {
 public:
  const char* name() const override { return "Entity Names"; }

  void OnRenderUI() override;

  // Panneau de configuration (rendu par MoonlightUi).
  void DrawSettings();

  // Accesseurs pour la persistance (bourgeon_settings.yaml via MoonlightUi).
  bool&  enabled()       { return enabled_; }
  bool&  show_players()  { return show_players_; }
  bool&  show_monsters() { return show_monsters_; }
  bool&  show_npcs()     { return show_npcs_; }
  bool&  show_self()     { return show_self_; }
  bool&  outline()       { return outline_; }
  int&   y_offset()      { return y_offset_; }
  float& font_scale()    { return font_scale_; }

 private:
  void DrawNames();  // corps réel, appelé sous __try depuis OnRenderUI

  bool  enabled_       = false;  // désactivé par défaut (opt-in)
  bool  show_players_  = true;
  bool  show_monsters_ = false;  // peut encombrer sur les maps à mobs
  bool  show_npcs_     = false;
  bool  show_self_     = false;  // ton propre nom
  bool  outline_       = true;   // contour noir pour la lisibilité
  int   y_offset_      = 2;      // décalage vertical (px) sous les pieds
  float font_scale_    = 1.0f;   // 0.7 .. 1.6
};
