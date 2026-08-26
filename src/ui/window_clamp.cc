#include "ui/window_clamp.h"

#include <cfloat>
#include <vector>

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

// La part d'écran qu'une fenêtre ne dépasse pas. 90 % : assez pour qu'une
// fenêtre pleine reste utilisable, assez peu pour qu'il reste toujours de quoi
// l'attraper et voir le jeu derrière.
constexpr float kMaxScreenFraction = 0.90f;

void KeepWindowsSizedToScreen() {
  ImGuiContext* context = ImGui::GetCurrentContext();
  if (!context) return;
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  if (screen.x <= 0.0f || screen.y <= 0.0f) return;

  const ImVec2 largest(screen.x * kMaxScreenFraction,
                       screen.y * kMaxScreenFraction);

  constexpr ImGuiWindowFlags kOwnedByImGui =
      ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup |
      ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildMenu;
  // Taille dictée par le contenu ou par l'appelant : ce n'est pas le joueur qui
  // l'a tirée, ce n'est pas à nous de la couper.
  constexpr ImGuiWindowFlags kSizeNotOurs =
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize;

  for (ImGuiWindow* window : context->Windows) {
    if (!window) continue;
    if (window->Flags & (kOwnedByImGui | kSizeNotOurs)) continue;
    // Jamais dessinée : sa taille n'est pas encore mesurée (même raison que le
    // clamp de position ci-dessous).
    if (window->SizeFull.x <= 0.0f || window->SizeFull.y <= 0.0f) continue;

    // `SizeFull` et NON `Size` : `Size` est la taille EFFECTIVE de la dernière
    // frame — la seule hauteur de barre de titre pour une fenêtre repliée. La
    // borner déplierait la fenêtre à la taille d'un titre.
    ImVec2 bounded = window->SizeFull;
    bounded.x = ImMin(bounded.x, largest.x);
    bounded.y = ImMin(bounded.y, largest.y);
    if (bounded.x == window->SizeFull.x && bounded.y == window->SizeFull.y)
      continue;

    window->SizeFull = ImTrunc(bounded);
    // La taille corrigée doit partir dans imgui.ini, sinon une fenêtre trop
    // grande revient telle quelle au relancement.
    ImGui::MarkIniSettingsDirty(window);
  }
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

// ── Aimantation ──────────────────────────────────────────────────────────────
namespace {

// Rayon d'attraction, en pixels d'écran. Assez large pour qu'on n'ait pas à viser
// le pixel, assez court pour qu'on puisse encore poser délibérément une fenêtre à
// une dizaine de pixels d'une autre.
constexpr float kMagnetRadius = 12.0f;

// Deux fenêtres qui ne se croisent sur AUCUN axe n'ont rien à s'aimanter : coller
// le bord gauche de l'une au bord droit d'une autre posée trois cents pixels plus
// bas ne range rien du tout. On tolère un petit débord, sinon deux fenêtres
// rangées coin contre coin ne s'accrochent qu'au pixel près.
constexpr float kMagnetOverlap = 24.0f;

bool g_magnet_on = true;

// 🔴 Des ImGuiID, PAS des ImGuiWindow* : une fenêtre peut disparaître entre deux
// frames (une conversation qu'on ferme), et le pointeur gardé d'une frame à
// l'autre serait pendant.
//
// La passe tourne AVANT le premier Begin de la frame : elle ne dispose donc que
// des marques posées à la frame PRÉCÉDENTE. Sans conséquence — une fenêtre ne
// change pas de nature d'une frame à l'autre, et sa géométrie d'avant est
// justement celle qu'on voit à l'écran au moment où le joueur vise.
std::vector<ImGuiID> g_magnet_marks;    // en cours de constitution (frame N)
std::vector<ImGuiID> g_magnet_windows;  // celles de la frame N-1, les seules cibles

bool IsMagnetWindow(ImGuiID id) {
  for (const ImGuiID marked : g_magnet_windows)
    if (marked == id) return true;
  return false;
}

// Retient le plus COURT des écarts proposés, et seulement s'il tient dans le
// rayon. `best` reste à FLT_MAX quand rien n'a mordu : c'est ce qui distingue
// « aucun voisin » d'un écart nul (fenêtre déjà collée).
struct MagnetAxis {
  float best  = FLT_MAX;
  float delta = 0.0f;

  void Consider(float candidate) {
    const float distance = ImFabs(candidate);
    if (distance > kMagnetRadius || distance >= best) return;
    best  = distance;
    delta = candidate;
  }
  float Apply(float value) const { return (best == FLT_MAX) ? value : value + delta; }
};

}  // namespace

void SetWindowMagnet(bool on) { g_magnet_on = on; }

void MagnetMarkWindow() {
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window == nullptr) return;
  // La racine : c'est elle qui porte la position, et c'est elle qu'ImGui déplace.
  ImGuiWindow* root = (window->RootWindow != nullptr) ? window->RootWindow : window;
  g_magnet_marks.push_back(root->ID);
}

void SnapMovingWindowToPeers() {
  // L'échange se fait à CHAQUE passe, même quand l'aimant est éteint ou qu'aucune
  // fenêtre ne bouge : sans lui, les marques de la frame s'empileraient sans fin.
  g_magnet_windows.swap(g_magnet_marks);
  g_magnet_marks.clear();
  if (!g_magnet_on || g_magnet_windows.empty()) return;

  ImGuiContext* context = ImGui::GetCurrentContext();
  if (context == nullptr || context->MovingWindow == nullptr) return;
  // 🔴 Le SIMPLE CLIC ne doit rien aimanter. ImGui arme `MovingWindow` dès que le
  // bouton s'enfonce, bien avant le moindre déplacement : sans ce test, cliquer
  // sur une chatbox déjà posée à trois pixels d'une autre la ferait sauter contre
  // elle — un geste qui ne demandait rien, et qui déplace quand même.
  if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left)) return;
  ImGuiWindow* moving = (context->MovingWindow->RootWindow != nullptr)
                            ? context->MovingWindow->RootWindow
                            : context->MovingWindow;
  if (moving->Size.x <= 0.0f || moving->Size.y <= 0.0f) return;
  if (!IsMagnetWindow(moving->ID)) return;

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  if (display.x <= 0.0f || display.y <= 0.0f) return;  // fenêtre du jeu minimisée

  const float x0 = moving->Pos.x, x1 = moving->Pos.x + moving->Size.x;
  const float y0 = moving->Pos.y, y1 = moving->Pos.y + moving->Size.y;

  MagnetAxis ax, ay;
  // Les bords de l'écran, d'abord : ce sont les voisins que toute fenêtre a.
  ax.Consider(0.0f - x0);
  ax.Consider(display.x - x1);
  ay.Consider(0.0f - y0);
  ay.Consider(display.y - y1);

  for (ImGuiWindow* peer : context->Windows) {
    if (peer == nullptr || peer == moving) continue;
    // `WasActive` et pas `Active` : à ce point de la frame, aucune fenêtre n'a
    // encore été soumise — c'est l'état de la frame précédente qui décrit ce que
    // le joueur a sous les yeux.
    if (!peer->WasActive) continue;
    if (peer->Size.x <= 0.0f || peer->Size.y <= 0.0f) continue;
    if (!IsMagnetWindow(peer->ID)) continue;

    const float px0 = peer->Pos.x, px1 = peer->Pos.x + peer->Size.x;
    const float py0 = peer->Pos.y, py1 = peer->Pos.y + peer->Size.y;

    // Quatre écarts par axe : les deux qui font se TOUCHER les fenêtres (bord
    // contre bord), et les deux qui les ALIGNENT (bords du même côté à la même
    // abscisse). Les seconds servent à empiler proprement deux fenêtres l'une
    // sous l'autre — c'est ce qu'on cherche en rangeant des chatbox.
    if (y0 <= py1 + kMagnetOverlap && py0 <= y1 + kMagnetOverlap) {
      ax.Consider(px1 - x0);  // se coller à sa droite
      ax.Consider(px0 - x1);  // se coller à sa gauche
      ax.Consider(px0 - x0);  // bords gauches alignés
      ax.Consider(px1 - x1);  // bords droits alignés
    }
    if (x0 <= px1 + kMagnetOverlap && px0 <= x1 + kMagnetOverlap) {
      ay.Consider(py1 - y0);  // se poser sous elle
      ay.Consider(py0 - y1);  // se poser au-dessus
      ay.Consider(py0 - y0);  // bords hauts alignés
      ay.Consider(py1 - y1);  // bords bas alignés
    }
  }

  if (ax.best == FLT_MAX && ay.best == FLT_MAX) return;
  const ImVec2 snapped(ax.Apply(moving->Pos.x), ay.Apply(moving->Pos.y));
  if (snapped.x == moving->Pos.x && snapped.y == moving->Pos.y) return;

  // Écriture directe de Pos, pour les raisons détaillées dans KeepWindowsOnScreen.
  // 🔴 Et rien à mémoriser : ImGui RECALCULE cette position à partir de la souris
  // à chaque frame du glisser (UpdateMouseMovingWindowNewFrame). C'est ce qui
  // donne à l'aimant son comportement attendu — la fenêtre décolle dès que la
  // souris s'éloigne, sans dérive accumulée.
  moving->Pos = ImTrunc(snapped);
  ImGui::MarkIniSettingsDirty(moving);
}

}  // namespace ro
