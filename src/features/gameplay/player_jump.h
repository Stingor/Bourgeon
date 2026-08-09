#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/plugin.h"

// Saut à la barre espace (touche remappable) — visible par les autres joueurs.
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
// l'offset est ré-ajouté après le recalcul terrain.
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
// ── Multijoueur ─────────────────────────────────────────────────────────────
// Le mécanisme n'a rien de spécifique au joueur local : +0x3f4 est un champ de
// l'acteur, et n'importe quel acteur en a un. On anime donc une LISTE de sauts
// (gid 0 = soi-même, sinon acteur résolu par ActorList_FindByGID 0x00a69eb0).
// Au déclenchement on émet CZ_BOURGEON_JUMP (0x0F1A, sans payload) ; le serveur
// applique un cooldown anti-flood puis rediffuse ZC_BOURGEON_JUMP (0x0F1B, le
// GID du sauteur) aux clients Bourgeon de la zone SAUF l'émetteur (qui s'anime
// déjà localement). Le saut reste purement esthétique : la position logique du
// personnage ne bouge pas (ni case, ni portée, ni collisions).
//
// La touche n'est PAS consommée : elle traverse normalement. Le natif ne route
// espace vers ProcessPushButton (d'où vient OnKeyDown) que HORS focus chat —
// taper une espace dans le chat ne déclenche donc aucun saut, même garantie que
// les hotkeys de skill. Adresses spécifiques au client 20250716.
//
// La touche est REMAPPABLE (panneau Mini-jeux de moonlight_ui, persistée dans le
// yaml). Le combo choisi passe par le contrôle de conflit partagé de
// features/hotkey_util.h : presets d'équipement, raccourcis natifs de la barre de
// skills et Alt+F sont refusés, pour ne pas déclencher deux actions d'un coup.
class PlayerJump : public Plugin {
 public:
  PlayerJump();

  const char* name() const override { return "PlayerJump"; }

  void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Toggle + réglages exposés au menu moonlight_ui.
  bool enabled() const { return enabled_; }
  // Surcharge non-const : la table de persistance décrit un réglage par
  // l'ADRESSE de sa valeur, il lui faut donc une lvalue (cf. jump_enabled).
  bool& enabled() { return enabled_; }
  void SetEnabled(bool on);
  float* p_height()      { return &jump_height_; }   // amplitude (unités monde)
  int*   p_duration_ms() { return &jump_ms_; }        // durée de l'arc (ms)

  // Touche de saut, persistée par MoonlightUi (yaml « jump_key_* »). Accesseurs
  // par référence : la table de persistance décrit chaque réglage par l'ADRESSE
  // de sa valeur, comme pour tous les autres plugins.
  int&  key_vk()    { return key_vk_; }
  bool& key_ctrl()  { return key_ctrl_; }
  bool& key_alt()   { return key_alt_; }
  bool& key_shift() { return key_shift_; }
  // Capture d'un nouveau combo, pilotée par le panneau de réglages : tant
  // qu'elle dure, la touche pressée REMAPPE au lieu de faire sauter.
  bool&        key_capturing()   { return key_capturing_; }
  std::string& key_conflict_msg() { return key_conflict_msg_; }

 private:
  // Un saut en cours. gid == 0 => le joueur local (évite d'avoir à lire son
  // propre GID ; l'acteur local est résolu directement via l'actorMgr).
  struct Jump {
    uint32_t gid;
    uint32_t start_ms;
  };

  void StartJump(uint32_t gid);   // idempotent : ignore si ce gid saute déjà
  void ClearAll();                // repose tous les sprites au sol et vide la liste

  // 🔴 Le fil RÉSEAU ne fait que copier : `jumps_` est écrite par StartJump et
  // parcourue par OnRenderUI, et la résolution d'acteur lit le monde — deux choses
  // qui n'ont rien à faire hors du fil principal. Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // OFF par défaut : le saut prend une touche (Espace) et émet un paquet que
  // les autres joueurs voient. Comme tout ce qui AGIT, il s'active à la main.
  bool  enabled_     = false;
  float jump_height_ = 10.0f;  // hauteur de crête, en unités monde
  int   jump_ms_     = 600;    // durée montée+descente
  // Combo de déclenchement (VK Windows + modificateurs). 0x20 = VK_SPACE ; on
  // évite d'inclure Windows.h ici pour une constante.
  int   key_vk_      = 0x20;
  bool  key_ctrl_ = false, key_alt_ = false, key_shift_ = false;
  bool  key_capturing_ = false;   // remappage en cours (panneau de réglages)
  std::string key_conflict_msg_;  // dernier refus affiché sous la ligne de capture
  std::vector<Jump> jumps_;    // sauts actifs (local + distants)
};
