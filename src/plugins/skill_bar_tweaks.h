#pragma once

#include "plugins/plugin.h"

// Barre d'action ImGui qui REMPLACE visuellement la barre de raccourcis native
// (UIShortCutWnd, window id 0x24) — façon Bartender/Dominos/ElvUI.
//
// Principe (validé par RE adverse, voir memory project_shortcut_bar_re.md) :
//   - La fenêtre native reste VIVANTE mais CACHÉE (vtable+0x38 -> this+0x28=0).
//     On ne la détruit JAMAIS (le destructeur nulle g_UIWindowMgr+0x1e8 et tue
//     le clavier F1-F9, qui passe par ce cache et non par la visibilité).
//   - On dessine nos propres slots en ImGui en PUR DESSIN (rectangles bordés +
//     couleurs de fond + texte), SANS texture du jeu : color-codé skill/item,
//     id + niveau, overlay de cooldown. (Icônes = polish v2 éventuel.)
//   - SOURCE D'INDEX UNIQUE : on lit chaque slot via this+0xc4[i] (pointeur vers
//     le record 7 octets dans les globals -> données live) ; un clic gauche
//     déclenche l'usage par OnMsg(0,0x29,col,row) — exactement la voie des
//     F-keys -> dessin et activation toujours cohérents. OnMsg(0x17) (rebuild de
//     this+0xc4) est renvoyé seulement au hide et au changement d'onglet.
//   - Clic droit = vide le slot (SkillMgr_SetShortCutSlot id=0).
//
// v1 : une barre configurable (colonnes, taille, espacement, position, nb slots).
// NON câblé dans MoonlightUi (persistance yaml + multi-barres = étape suivante).
class SkillBarTweaks : public Plugin {
 public:
  SkillBarTweaks() = default;

  const char* name() const override { return "Skill Bar"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) override;
  void OnRenderUI() override;

  // Appelé par le hook WndProc au WM_LBUTTONUP (PRÉ-input jeu). Si un drag natif
  // (inventaire/grimoire) est relâché sur une case de la barre, assigne la case
  // (écriture directe) + vide la charge du drag -> pas de drop au sol, pas de
  // crash. Renvoie true si le drop a été traité. (mx,my = coords client.)
  bool HandleNativeDrop(int mx, int my);

  // ── Config (publique : futur câblage MoonlightUi / persistance yaml) ───────
  bool  panel_visible_ = true;   // panneau de réglage affiché
  bool  enabled_       = false;  // remplacement ImGui actif (cache la barre native)
  bool  locked_        = true;   // barre verrouillée (fixe) ; décoché = déplaçable. Slots
                                 // toujours utilisables/réarrangeables, lock ou pas.
  bool  bilinear_      = false;  // filtre texture icônes : false=POINT (net), true=LINEAR (flou ImGui)
  bool  clickthrough_  = false;  // clics traversent la barre (vont au jeu) sauf si Shift maintenu
  bool  show_keys_     = true;   // affiche l'étiquette de touche (F1-F9) en haut-gauche des slots
  bool  bold_text_     = false;  // faux-gras des textes (touches + nombres) : re-dessin décalé
  bool  dirty_         = false;  // config modifiée -> MoonlightUi draine et persiste (yaml)
  int   columns_       = 9;      // colonnes de la grille (1..12)
  int   slot_count_    = 9;      // nombre de slots affichés (1..36)
  int   first_slot_    = 0;      // premier slot logique (0..35)
  float icon_size_     = 32.0f;  // px par icône
  float spacing_       = 2.0f;   // px entre icônes
  int   bar_x_         = 300;    // position écran de la barre
  int   bar_y_         = 500;

  // Couleurs (RGBA 0..1) — fond du cadre, fond par type, fond vide, bordures.
  float col_frame_[4]    = {0.050f, 0.050f, 0.070f, 0.600f};  // fond du cadre (derrière les boutons)
  float col_skill_[4]    = {0.157f, 0.510f, 0.275f, 0.863f};  // vert
  float col_item_[4]     = {0.157f, 0.314f, 0.588f, 0.863f};  // bleu
  float col_empty_[4]    = {0.118f, 0.118f, 0.141f, 0.784f};  // vide
  float col_border_[4]   = {0.000f, 0.000f, 0.000f, 0.784f};
  float col_borderhi_[4] = {1.000f, 0.863f, 0.471f, 0.902f};  // survol / édition
  float col_keytext_[4]  = {0.745f, 0.804f, 0.922f, 0.588f};  // texte des touches (F1..) — discret
  float col_count_[4]    = {1.000f, 0.902f, 0.471f, 1.000f};  // texte nombre (count objet / niveau skill)

 private:
  void DrawPanel();          // panneau de configuration ImGui
  void DrawBar(void* wnd);   // la barre d'action elle-même

  bool in_game_       = false;
  bool native_hidden_ = false;   // état courant de la barre native
  int  last_tab_      = -1;      // pour détecter un changement d'onglet
};
