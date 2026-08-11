#include "ui/qty_prompt.h"

#include "imgui.h"
#include "ui/ro_imgui.h"
#include "utils/i18n.h"

namespace ro {
namespace {

// « Quantité » est le titre AFFICHÉ ; l'id ImGui (après ###) est fixe et propre au
// module, pour qu'il ne dépende pas du libellé et ne collisionne avec rien.
const char* const kPopupId = i18n::Tr("Quantité###ro_qty_prompt");

const void* g_owner = nullptr;  // fenêtre à qui appartient le prompt en cours
bool g_open_requested = false;  // OpenQuantityPrompt en attente
bool g_popup_open = false;      // la modale était ouverte à la frame précédente
int  g_amount = 1;              // quantité en cours de saisie

}  // namespace

void OpenQuantityPrompt(const void* owner) {
  g_owner = owner;
  g_open_requested = true;
}

int QuantityPrompt(const void* owner, const char* action_label, int max_amount,
                   bool* cancelled) {
  if (cancelled) *cancelled = false;
  if (max_amount < 1) max_amount = 1;

  if (g_open_requested && g_owner == owner) {
    g_open_requested = false;
    // Pile d'un seul objet : rien à choisir, on répond sans ouvrir de dialogue.
    if (max_amount <= 1) return 1;
    ImGui::OpenPopup(kPopupId);
  }
  // Prompt d'une autre fenêtre (ou aucun) : ne rien rendre ici. Sans ça, une
  // fenêtre voisine verrait son BeginPopupModal renvoyer false et le lirait comme
  // un abandon — le popup ImGui est scopé à la pile d'ID de son propriétaire.
  if (g_owner != owner) return 0;

  // Apparition SOUS LE CURSEUR (le prompt suit le clic qui l'a déclenché) et sans
  // voile : assombrir tout l'écran pour saisir un nombre serait disproportionné.
  const ImVec2 mouse = ImGui::GetMousePos();
  SetNextRoModalPos(mouse.x, mouse.y, false);
  if (!BeginRoPopupModal(kPopupId)) {
    // Fermeture qui ne vient pas de nos boutons (Échap) = abandon.
    if (g_popup_open) {
      g_popup_open = false;
      if (cancelled) *cancelled = true;
    }
    return 0;
  }
  g_popup_open = true;
  // Échap doit fermer LE PROMPT, pas la fenêtre RO qui est derrière (la pile Échap
  // les fermerait toutes les deux d'un coup).
  SuppressEscapeStack();

  ImGui::Text(i18n::Tr("%s combien ? (max %d)"), action_label ? action_label : i18n::Tr("Déplacer"),
              max_amount);

  // À l'ouverture : défaut = pile ENTIÈRE (le cas courant) et focus sur le champ,
  // texte sélectionné -> taper un nombre le remplace pour une quantité partielle.
  const bool appearing = ImGui::IsWindowAppearing();
  if (appearing) g_amount = max_amount;

  // [-] [champ] [+] : InputInt en step 0 (ses +/- natifs ne sont pas skinnés),
  // petits boutons RO carrés à la place, comme le panier du cash shop.
  if (RoButton("-")) --g_amount;
  ImGui::SameLine(0.0f, 2.0f);
  if (appearing) ImGui::SetKeyboardFocusHere();  // cible l'InputInt qui suit
  ImGui::SetNextItemWidth(ro::Px(90.0f));
  ImGui::InputInt("##ro_qty", &g_amount, 0, 0);
  ImGui::SameLine(0.0f, 2.0f);
  if (RoButton("+")) ++g_amount;
  if (g_amount < 1) g_amount = 1;
  if (g_amount > max_amount) g_amount = max_amount;

  // Entrée (ou pavé numérique) vaut OK — le focus est dans le champ de saisie.
  const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
  int accepted = 0;
  if (RoButton(i18n::Tr("OK")) || enter) accepted = g_amount;
  ImGui::SameLine();
  if (RoButton(i18n::Tr("Tout"))) accepted = max_amount;
  ImGui::SameLine();
  const bool abandoned = RoButton(i18n::Tr("Annuler"));

  if (accepted > 0 || abandoned) {
    g_amount = 1;
    g_popup_open = false;  // fermeture voulue : ne pas la relire comme un abandon
    ImGui::CloseCurrentPopup();
    if (abandoned && cancelled) *cancelled = true;
  }
  EndRoPopupModal();
  return accepted;
}

}  // namespace ro
