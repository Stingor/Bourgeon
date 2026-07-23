#pragma once

#include <cstdint>

#include "plugins/plugin.h"

// Saut cosmétique du joueur à la barre espace.
//
// RO est un vrai moteur 3D (terrain 3D + sprites billboardés). La hauteur du
// sprite au sol est RECALCULÉE chaque frame depuis la heightmap (.gnd) dans
// CActorSprite_UpdateMotionAndPosition (0x00c47700) via Terrain_GetHeightAt —
// écrire directement la position Z de l'entité serait donc écrasé aussitôt.
// MAIS le jeu ajoute juste après, chaque frame, un vec3 d'offset de position
// PROPRE à l'acteur :
//     pos.x += *(actor+0x3f0)   // offset X
//     pos.y += *(actor+0x3f4)   // offset HAUTEUR (Y)   <-- notre levier
//     pos.z += *(actor+0x3f8)   // offset Z
// (initialisé à 0 dans ActorAiClass_ctor 0x00c3f900, jamais touché en jeu
// normal). Écrire +0x3f4 soulève donc le sprite — même en marchant, puisque
// l'offset est ré-ajouté après le recalcul terrain — sans aucun impact gameplay :
// le serveur ne voit rien, c'est purement visuel.
//
// ⚠ PIÈGE : ce recalcul de position n'a lieu QUE par intermittence. Dans
// 0x00c47700 le bloc final (LAB_00c47c9c) est gardé par `if (local_10 != 0)`, et
// local_10 ne vaut 1 qu'en MARCHE (case 1) et KNOCKBACK (case 4). À l'ARRÊT
// (case 0/STAND) la fonction sort par LAB_00c47892 sans jamais retoucher
// +0x10/+0x14/+0x18 : la position garde sa dernière valeur. Un saut terminé
// immobile laissait donc le perso figé en l'air (offset remis à 0 mais jamais
// relu). D'où la 2e écriture : on refait le calcul du moteur nous-mêmes chaque
// frame — pos.y(+0x14) = Terrain_GetHeightAt(monde, x, z) + offset — ce qui donne
// exactement la même valeur que lui, donc zéro conflit dans les deux régimes.
// (Terrain_GetHeightAt 0x007110c0, monde = *(actorMgr+0x30).)
//
// L'axe Y de RO pointe vers le BAS (cf FpsViewTweaks : « monter = soustraire »),
// donc une hauteur de saut positive s'applique comme un offset NÉGATIF.
//
// Espace lance un arc parabolique (montée puis descente) sur ~600 ms. La touche
// n'est PAS consommée : elle traverse normalement. Le natif ne route espace vers
// ProcessPushButton (d'où vient OnKeyDown) que HORS focus chat — taper une espace
// dans le chat ne déclenche donc aucun saut, même garantie que les hotkeys de
// skill. Adresses spécifiques au client 20250716.
class PlayerJumpTweaks : public Plugin {
 public:
  const char* name() const override { return "PlayerJump"; }

  void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Toggle + réglages exposés au menu moonlight_ui.
  bool enabled() const { return enabled_; }
  void SetEnabled(bool on);
  float* p_height()      { return &jump_height_; }   // amplitude (unités monde)
  int*   p_duration_ms() { return &jump_ms_; }        // durée de l'arc (ms)

 private:
  void ResetOffset();  // remet +0x3f4 à 0 sur l'acteur joueur et coupe le saut

  bool     enabled_       = true;
  bool     jumping_       = false;
  uint32_t jump_start_ms_ = 0;
  float    jump_height_   = 10.0f;  // hauteur de crête, en unités monde
  int      jump_ms_       = 600;    // durée montée+descente
};
