#include "features/fx/cell_style.h"

#include "imgui.h"
#include "ui/ro_imgui.h"    // ro::Px, ro::RoCombo
#include "ui/ro_widgets.h"  // mui::WheelSliderInt, mui::Tooltip
#include "utils/i18n.h"

namespace cellstyle {

bool DrawPatternSettings(int* pattern, int* gap, const char* gap_tooltip) {
  bool changed = false;

  // 🔴 Libellés NUS : `ro::RoCombo` traduit ses items lui-même, à la lecture.
  static const char* const kPatterns[] = {"Anneau (curseur du jeu)",
                                          "Carrelage (cadre de case)",
                                          "Carreau plein (avec joint)"};
  static_assert(IM_ARRAYSIZE(kPatterns) == kCount,
                "un libellé par texture, dans le même ordre");

  ImGui::SetNextItemWidth(ro::Px(220.0f));
  if (ro::RoCombo(i18n::Tr("Dessin"), pattern, kPatterns,
                  IM_ARRAYSIZE(kPatterns))) {
    changed = true;
  }
  mui::Tooltip(i18n::Tr("L'anneau et le cadre sont des textures du client — celle de "
                        "ton curseur de destination et celle des sorts de zone.\n"
                        "« Carreau plein » emploie la nôtre : la case devient un "
                        "aplat, un peu plus petit qu'elle, et c'est le sol laissé "
                        "visible tout autour qui trace la bordure."));

  if (*pattern == kSolid) {
    ImGui::SetNextItemWidth(ro::Px(220.0f));
    if (mui::WheelSliderInt(i18n::Tr("Joint (%)"), gap, 0, 40, "%d %%")) {
      changed = true;
    }
    mui::Tooltip(gap_tooltip);
  }
  return changed;
}

}  // namespace cellstyle
