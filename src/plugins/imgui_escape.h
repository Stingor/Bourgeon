#pragma once

#include "imgui.h"

namespace bourgeon {

// Ferme une fenêtre ImGui quand Échap est pressé et qu'elle a le focus.
// À appeler juste après ImGui::Begin(...). Passe la même variable `open` que
// celle liée à Begin ; la fonction la met à false le cas échéant.
//
// ImGui ne route pas Échap vers la fermeture des fenêtres classiques (il ne
// gère que les popups/menus). On reproduit le comportement attendu : Échap sur
// la fenêtre focus la referme, comme le bouton de titre.
inline void CloseWindowOnEscape(bool& open) {
  if (!open) return;
  // RootAndChildWindows : garde le focus si un enfant (combo, table) est actif.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false)) {
    open = false;
  }
}

}  // namespace bourgeon
