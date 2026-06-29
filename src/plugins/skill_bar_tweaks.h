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

  // ── Config (publique : futur câblage MoonlightUi / persistance yaml) ───────
  bool  panel_visible_ = true;   // panneau de réglage affiché
  bool  enabled_       = false;  // remplacement ImGui actif (cache la barre native)
  bool  edit_mode_     = false;  // barre déplaçable + surlignage des cellules
  int   columns_       = 9;      // colonnes de la grille (1..12)
  int   slot_count_    = 9;      // nombre de slots affichés (1..36)
  int   first_slot_    = 0;      // premier slot logique (0..35)
  float icon_size_     = 32.0f;  // px par icône
  float spacing_       = 2.0f;   // px entre icônes
  int   bar_x_         = 300;    // position écran de la barre
  int   bar_y_         = 500;

 private:
  void DrawPanel();          // panneau de configuration ImGui
  void DrawBar(void* wnd);   // la barre d'action elle-même

  bool in_game_       = false;
  bool native_hidden_ = false;   // état courant de la barre native
  int  last_tab_      = -1;      // pour détecter un changement d'onglet
};
