#pragma once

#include <string>
#include <vector>

#include "ui/ro_imgui.h"  // ro::RoSkinConfig

// ── skin_panel : le panneau de réglage du skin RO, côté TOOLKIT ──────────────
// Cette section ne pilote aucun plugin : elle configure le socle ImGui lui-même
// (ro::SkinConfig, la police Malgun, les presets de couleurs). Elle vivait dans
// plugins/moonlight_ui/panel_interface.cc pour la seule raison qu'elle s'affiche
// dans le panneau Moonlight — mais un panneau n'appartient pas à la fenêtre qui
// le dessine, il appartient à la couche dont il règle l'état.
//
// La PERSISTANCE reste à l'appelant : moonlight_ui lit/écrit `ro_skin_*` et
// `ro_skin_presets` dans bourgeon_settings.yaml en passant par les accesseurs
// ci-dessous. Le toolkit ne connaît ni yaml-cpp ni le chemin du fichier.

namespace ro {

// Un jeu de couleurs nommé, sauvegardé par le joueur. `cfg.rounding` n'est
// VOLONTAIREMENT pas persisté avec les presets (cf. EmitSkinCfg, with_rounding) :
// appliquer un thème ne doit pas écraser l'arrondi choisi par le joueur.
struct SkinPreset {
  std::string name;
  RoSkinConfig cfg;
};

// État partagé : rempli par le chargement des réglages, lu par la sauvegarde,
// modifié par le panneau. -1 = aucune sélection.
std::vector<SkinPreset>& SkinPresets();
int& SkinPresetSelection();

// Thèmes de départ, posés UNIQUEMENT si la liste est vide (1er lancement, ou
// yaml sans section presets) : « RO Classique », « Sombre », « Sepia ». À
// appeler après avoir chargé les presets du disque.
void EnsureDefaultSkinPresets();

// Section complète « Skin RO » : police, réglages du skin, gestion des presets.
// À placer dans un panneau ImGui existant. Renvoie true si quelque chose a
// changé — l'appelant décide alors de sauvegarder.
bool DrawSkinPanel();

}  // namespace ro
