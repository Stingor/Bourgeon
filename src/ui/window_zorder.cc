#include "ui/window_zorder.h"

#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // ImHashStr, FindWindowByID, BringWindowToDisplayBack

namespace ro {
namespace {

// Identifiants des fenêtres de fond, dans l'ordre de première déclaration.
// L'ID d'une fenêtre de haut niveau, c'est ImHashStr() de son nom complet (voir
// ImGuiWindow::ImGuiWindow et FindWindowByName) : on peut donc le calculer sans
// que la fenêtre existe encore, et se passer de garder les chaînes.
std::vector<ImGuiID>& BackgroundWindowIds() {
  static std::vector<ImGuiID> ids;
  return ids;
}

}  // namespace

void MarkBackgroundWindow(const char* window_name) {
  if (!window_name || !*window_name) return;
  const ImGuiID window_id = ImHashStr(window_name);
  std::vector<ImGuiID>& ids = BackgroundWindowIds();
  for (const ImGuiID known_id : ids)
    if (known_id == window_id) return;  // déjà déclarée (appel par frame)
  ids.push_back(window_id);
}

void SendBackgroundWindowsToBack() {
  ImGuiContext* context = ImGui::GetCurrentContext();
  if (!context || context->Windows.Size == 0) return;

  const std::vector<ImGuiID>& ids = BackgroundWindowIds();
  // Parcours À REBOURS : BringWindowToDisplayBack insère à l'index 0, donc traiter
  // la liste en sens inverse laisse les fenêtres empilées dans l'ordre de
  // déclaration tout en bas. Le résultat est le même à chaque frame -> aucun
  // clignotement entre deux éléments de HUD qui se chevauchent.
  for (size_t i = ids.size(); i-- > 0;) {
    ImGuiWindow* window = ImGui::FindWindowByID(ids[i]);
    // Pas encore créée : barre désactivée, ou HUD pas redessiné depuis le login.
    // Elle naîtra au premier plan et sera redescendue à la frame suivante.
    if (!window) continue;
    // g.Windows ne contient que des racines pour le tri d'affichage : une fenêtre
    // enfant suit sa parente et n'a rien à faire ici.
    if (window->Flags & ImGuiWindowFlags_ChildWindow) continue;
    ImGui::BringWindowToDisplayBack(window);
  }
}

}  // namespace ro
