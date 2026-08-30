#pragma once

#include "features/plugin.h"

// LoginParade — fait flâner une petite bande de monstres de la famille Poring
// sur l'écran de login. Purement cosmétique : les .spr/.act passent par
// ui/mob_sprite.h (donc notre propre parseur, ui/spr_act.h) et sont dessinés en
// ImGui par-dessus le fond de login natif — aucun acteur en scène, aucun
// monde/terrain requis, et plus rien qui dépende de l'atlas de sprites du
// client ni de la disposition mémoire de ses objets.
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
  LoginParade();

  void OnRenderLoginUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Tient à jour le miroir du réglage dans le fichier de DÉMARRAGE (cf. ctor).
  void OnTick() override;

  // Réglage opt-in persisté par [[moonlight_ui]] (défaut ON : c'est tout l'intérêt).
  // Accès direct par MoonlightUi::Load/SaveSettings, comme les autres plugins.
  // Le son suit ce toggle + la config son globale du jeu (pas de réglage séparé).
  bool enabled_ = true;

 private:
  // 🔴 Le réglage ci-dessus vit dans `bourgeon_settings.yaml`, qui n'est relu
  // QU'À L'ENTRÉE EN JEU — c'est-à-dire jamais, au moment où cette parade
  // s'affiche. Elle tournait donc sur son défaut (ON) chez des joueurs qui
  // l'avaient décochée, et ne redevenait obéissante qu'après une première
  // connexion.
  //
  // On en garde donc un MIROIR dans le fichier de démarrage, lu au constructeur
  // et réécrit dès que la valeur change — d'où qu'elle vienne, du panneau comme
  // de la table de réglages. Ce n'est pas le réglage : c'en est l'écho, à la
  // seule fin d'être lisible assez tôt.
  bool startup_mirror_ = true;
};
