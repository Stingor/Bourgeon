#include "ui/ro_widgets.h"

#include "ui/ro_imgui.h"  // ro::RoSliderFloat / RoSliderInt (rendu « façon RO »)

namespace mui {

// Helper to display a little (?) mark which shows a tooltip when hovered.
// In your own code you may want to display an actual icon if you are using a merged icon fonts (see docs/FONTS.md)
void HelpMarker(const char* desc) {
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
    TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// Pastille + nuancier en popup, calqué sur `ChatTweaks::DrawBackgroundGroup` —
// mêmes dimensions de pastille (20×20), mêmes drapeaux de nuancier, même bouton
// « Fermer ». Ce qui n'est PAS repris, ce sont les préréglages : ils appartiennent
// aux fonds du chat natif, qu'ils patchent dans le .text.
bool RoColorSwatch(const char* label, float rgba[4]) {
  bool changed = false;
  ImGui::PushID(label);
  const ImVec4 swatch(rgba[0], rgba[1], rgba[2], rgba[3]);
  if (ImGui::ColorButton("##btn", swatch, ImGuiColorEditFlags_AlphaPreview,
                         ImVec2(20, 20)))
    ImGui::OpenPopup("picker");
  ImGui::SameLine();
  // Le libellé peut porter un identifiant caché (`##` ou `###`) : on n'affiche
  // que ce qui précède, comme le fait ImGui pour tous ses widgets. Coupé à la
  // main plutôt qu'avec `FindRenderedTextEnd`, qui vit dans imgui_internal.h —
  // un widget partagé n'a pas à faire dépendre ses appelants de l'API interne.
  const char* visible_end = label;
  while (*visible_end != '\0' && !(visible_end[0] == '#' && visible_end[1] == '#'))
    ++visible_end;
  ImGui::TextUnformatted(label, visible_end);

  if (ImGui::BeginPopup("picker")) {
    changed |= ImGui::ColorPicker4("##pick", rgba,
                                   ImGuiColorEditFlags_AlphaBar |
                                       ImGuiColorEditFlags_NoSidePreview);
    if (ro::RoButton("Fermer")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

// SliderFloat/SliderInt variants that ALSO adjust on mouse-wheel while hovered
// (fine-tuning without grabbing the handle). SetItemKeyOwner(MouseWheelY) claims
// the wheel so the settings window doesn't scroll at the same time. Step defaults
// to 0.01 (float) / 1 (int).
// Shift+wheel uses a larger step (0.10 / 10) for faster adjustment.
bool WheelSliderFloat(const char* label, float* v, float lo, float hi, const char* fmt, float step) {
  // Rendu = scrollbar horizontale RO (cf. ro::RoSliderFloat) ; ses flèches ajustent
  // au même pas que la molette ci-dessous.
  bool changed = ro::RoSliderFloat(label, v, lo, hi, fmt, step,
                                   ImGuiSliderFlags_AlwaysClamp);
  Tooltip("- Arrows / mouse wheel adjust value (Shift = larger step)\n- Ctrl-Click for direct input.");
  // 🔴 `IsItemHovered()` D'ABORD : sans lui, la molette continuait d'ajuster un
  // slider GRISÉ. Elle est traitée hors du widget (c'est ce qui permet de
  // régler sans attraper la poignée), donc `BeginDisabled` ne la voit pas
  // passer — alors que le survol, lui, est faux sur un item désactivé.
  if (ImGui::IsItemHovered() && ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
    const float w = ImGui::GetIO().MouseWheel;
    if (w != 0.0f) {
      if (step <= 0.0f) step = ImGui::GetIO().KeyShift ? 0.10f : 0.01f;
      float nv = *v + w * step;
      if (nv < lo) nv = lo;
      if (nv > hi) nv = hi;
      if (nv != *v) { *v = nv; changed = true; }
    }
  }
  return changed;
}

bool WheelSliderInt(const char* label, int* v, int lo, int hi, const char* fmt, int step) {
  bool changed = ro::RoSliderInt(label, v, lo, hi, fmt, step,
                                 ImGuiSliderFlags_AlwaysClamp);
  Tooltip("- Arrows / mouse wheel adjust value (Shift = larger step)\n- Ctrl-Click for direct input.");
  // 🔴 `IsItemHovered()` D'ABORD : sans lui, la molette continuait d'ajuster un
  // slider GRISÉ. Elle est traitée hors du widget (c'est ce qui permet de
  // régler sans attraper la poignée), donc `BeginDisabled` ne la voit pas
  // passer — alors que le survol, lui, est faux sur un item désactivé.
  if (ImGui::IsItemHovered() && ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
    const float w = ImGui::GetIO().MouseWheel;
    if (w != 0.0f) {
      if (step <= 0) step = ImGui::GetIO().KeyShift ? 10 : 1;  // unit precision by default ("à l'unité près")
      int nv = *v + (w > 0.0f ? step : -step);
      if (nv < lo) nv = lo;
      if (nv > hi) nv = hi;
      if (nv != *v) { *v = nv; changed = true; }
    }
  }
  return changed;
}

// Make the UI compact because there are so many fields
void PushStyleCompact()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, (float)(int)(style.FramePadding.y * 0.60f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, (float)(int)(style.ItemSpacing.y * 0.60f)));
}

// Restore the default UI style after PushStyleCompact()
void PopStyleCompact()
{
    ImGui::PopStyleVar(2);
}

}  // namespace mui
