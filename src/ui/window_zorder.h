#pragma once

#include "imgui.h"

// ── Fenêtres ImGui : plan de fond du HUD ─────────────────────────────────────
// Les éléments de HUD (barre de skills, barres HP/SP/XP/zeny/poids, portrait) ne
// sont pas des « fenêtres » du point de vue du joueur : ce sont des décors épinglés
// à l'écran. Ils ne doivent JAMAIS passer devant une vraie fenêtre (inventaire,
// grimoire, entrepôt, dialogue NPC…), même quand ils viennent d'être cliqués ou
// qu'ils sont créés APRÈS elle.
//
// ImGui n'a pas de « toujours en arrière » : l'ordre d'affichage, c'est l'ordre du
// tableau g.Windows, où toute fenêtre nouvellement créée est ajoutée en DERNIER
// (donc au premier plan) et où FocusWindow remonte la fenêtre cliquée.
// ImGuiWindowFlags_NoBringToFrontOnFocus règle le second cas seulement : une barre
// activée en cours de partie, ou recréée après un changement de carte, se
// retrouverait quand même devant l'inventaire déjà ouvert.
//
// D'où les deux moitiés, indissociables :
//   1. kBackgroundWindowFlags au Begin()      -> le clic ne remonte plus la fenêtre ;
//   2. MarkBackgroundWindow() + le passage    -> l'ordre est réimposé chaque frame,
//      SendBackgroundWindowsToBack()             quel que soit l'ordre de création.
//
// Tout est à appeler depuis le thread principal du jeu (comme le reste d'ImGui).

namespace ro {

// À ajouter aux flags du Begin() d'une fenêtre de fond. Empêche FocusWindow de la
// remonter au premier plan quand on clique dedans (le focus lui-même, donc le
// glisser et le drag & drop, continue de fonctionner normalement).
constexpr ImGuiWindowFlags kBackgroundWindowFlags =
    ImGuiWindowFlags_NoBringToFrontOnFocus;

// Déclare `window_name` (le nom EXACT passé à Begin) comme fenêtre de fond.
// Idempotent : à appeler juste avant le Begin, chaque frame — c'est ce qui évite
// de dépendre d'un ordre d'initialisation des plugins, et ça couvre les noms
// construits à l'exécution (une barre par index).
void MarkBackgroundWindow(const char* window_name);

// Repousse toutes les fenêtres déclarées sous toutes les autres. À appeler UNE
// fois par frame, juste APRÈS ImGui::NewFrame() et avant le moindre Begin : les
// fenêtres de la frame précédente sont encore en place dans g.Windows, et l'ordre
// corrigé est celui qui sera dessiné à EndFrame.
void SendBackgroundWindowsToBack();

}  // namespace ro
