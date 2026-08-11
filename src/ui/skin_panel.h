#pragma once

#include <string>
#include <vector>

#include "ui/ro_imgui.h"  // ro::RoSkinConfig

// ── skin_panel : le panneau de réglage du skin RO, côté TOOLKIT ──────────────
// Cette section ne pilote aucun plugin : elle configure le socle ImGui lui-même
// (ro::SkinConfig et les presets de couleurs — la police et l'échelle, elles,
// ne sont plus DANS la section, seulement dessinées d'ici). Elle vivait dans
// features/moonlight_ui/panel_interface.cc pour la seule raison qu'elle s'affiche
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

// Le combo « police de toute l'interface », seul. Il n'est PLUS dans la section
// ci-dessous : ses deux poseurs sont l'en-tête « Interface de jeu » du panneau
// Moonlight et l'écran de login, tous deux à côté du choix de la langue.
//
// 🔴 Il PERSISTE lui-même son choix (startup::SaveUiFontFamily), au lieu de le
// rendre à l'appelant comme le reste du panneau. C'est un réglage d'AVANT le
// jeu, écrit dans le fichier de démarrage : le login n'a aucun cycle de
// sauvegarde à lui offrir, et un choix perdu à la fermeture du client passerait
// pour un bug.
//
// `label` se passe DÉJÀ TRADUIT, et via `i18n::TrId` : `RoBeginCombo` fait
// `PushID(label)`, donc un libellé traduit changerait l'identité du widget d'une
// langue à l'autre — au login, précisément pendant qu'on en change.
void DrawUiFontCombo(const char* label, float width);

// Le combo « échelle de toute l'interface » (100 à 200 % par paliers). Même
// nature que celui de la police, et posé au même endroit : réglage d'AVANT le
// jeu, qui persiste lui-même dans le fichier de démarrage
// (startup::SaveUiScalePercent) — d'où l'absence de valeur de retour, il n'y a
// rien à remonter à l'appelant.
//
// ⚠ Se grise tout seul sous le proxy DirectX 7, où l'échelle est sans effet
// (pas de re-rastérisation des glyphes). Voir features/systems/dx7_warning.h.
void DrawUiScaleCombo(const char* label, float width);

// Section complète « Skin RO » : réglages du skin et gestion des presets — soit
// l'HABILLAGE des fenêtres, et lui seul. À placer dans un panneau ImGui
// existant. Renvoie true si quelque chose a changé — l'appelant décide alors de
// sauvegarder.
//
// La police et l'échelle n'y sont plus : elles s'appliquent à toute l'interface,
// pas à son habillage, et sont posées à côté de la langue (cf. les deux combos
// ci-dessus). Elles ne vivent d'ailleurs pas dans bourgeon_settings.yaml et
// s'enregistrent toutes seules — rien qu'elles changent n'aurait pu remonter
// dans la valeur de retour.
bool DrawSkinPanel();

}  // namespace ro
