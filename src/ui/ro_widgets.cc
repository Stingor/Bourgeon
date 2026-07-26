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
  if (ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
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
  if (ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
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
