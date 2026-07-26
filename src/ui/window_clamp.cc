#include "ui/window_clamp.h"

#include "imgui.h"
#include "imgui_internal.h"  // ImGuiWindow, context->Windows, ImClamp/ImTrunc, MarkIniSettingsDirty

namespace ro {

ImVec2 ClampWindowPosToScreen(ImVec2 window_pos, ImVec2 window_size) {
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  // Écran de taille nulle = fenêtre du jeu minimisée (GetClientRect renvoie 0x0).
  // On ne touche à RIEN : clamper dans un écran nul collerait tout en (0,0) et
  // cette position pourrie serait mémorisée (même piège que le saut de frame dans
  // RenderImGuiDX9).
  if (screen.x <= 0.0f || screen.y <= 0.0f) return window_pos;

  ImVec2 clamped = window_pos;
  clamped.x = window_size.x >= screen.x
                  ? 0.0f
                  : ImClamp(window_pos.x, 0.0f, screen.x - window_size.x);
  clamped.y = window_size.y >= screen.y
                  ? 0.0f
                  : ImClamp(window_pos.y, 0.0f, screen.y - window_size.y);
  return clamped;
}

void KeepWindowsOnScreen() {
  ImGuiContext* context = ImGui::GetCurrentContext();
  if (!context) return;
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  if (screen.x <= 0.0f || screen.y <= 0.0f) return;

  constexpr ImGuiWindowFlags kOwnedByImGui =
      ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup |
      ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildMenu;

  for (ImGuiWindow* window : context->Windows) {
    if (!window) continue;
    if (window->Flags & kOwnedByImGui) continue;
    // Fenêtre encore jamais dessinée : sa taille n'est pas mesurée, la clamper
    // reviendrait à la coller en (0,0). Elle le sera à la frame d'après.
    if (window->Size.x <= 0.0f || window->Size.y <= 0.0f) continue;
    // Centrage à pivot en attente (SetNextWindowPos(..., pivot) d'une fenêtre qui
    // n'a pas encore sa taille définitive) : c'est Begin qui la placera cette frame,
    // à partir de SetWindowPosVal. Rien à corriger, et surtout rien à toucher.
    if (window->SetWindowPosVal.x != FLT_MAX) continue;

    // window->Size est la taille EFFECTIVEMENT occupée à la dernière frame : pour
    // une fenêtre repliée c'est la hauteur de barre de titre, donc replier une
    // fenêtre posée en bas d'écran ne la fait pas remonter.
    const ImVec2 clamped = ClampWindowPosToScreen(window->Pos, window->Size);
    if (clamped.x == window->Pos.x && clamped.y == window->Pos.y) continue;

    // Écriture directe de window->Pos, PAS ImGui::SetWindowPos(window, …) : cette
    // surcharge interne efface au passage SetWindowPosVal et les bits
    // Once/FirstUseEver/Appearing de SetWindowPosAllowFlags — de quoi faire rater le
    // centrage d'une fenêtre en cours d'apparition. Le décalage de curseurs de
    // layout qu'elle fait en plus ne nous sert à rien : ici, aucune fenêtre n'est en
    // cours d'append (on tourne entre NewFrame et le premier Begin de la frame).
    window->Pos = ImTrunc(clamped);
    // La position corrigée doit être celle écrite dans imgui.ini, sinon une fenêtre
    // laissée hors écran y revient au relancement. (Testé sur NoSavedSettings par
    // MarkIniSettingsDirty lui-même.)
    ImGui::MarkIniSettingsDirty(window);
  }
}

}  // namespace ro
