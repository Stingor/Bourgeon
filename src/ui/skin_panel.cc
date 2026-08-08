#include "ui/skin_panel.h"

#include "imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace ro {
namespace {

// Raccourci de lisibilité pour les thèmes de départ : couleur 0-255 -> float RGBA.
void SetColor255(float* rgba, int red, int green, int blue) {
  rgba[0] = red / 255.f;
  rgba[1] = green / 255.f;
  rgba[2] = blue / 255.f;
  rgba[3] = 1.f;
}

}  // namespace

std::vector<SkinPreset>& SkinPresets() {
  static std::vector<SkinPreset> presets;
  return presets;
}

int& SkinPresetSelection() {
  static int selection = -1;
  return selection;
}

void EnsureDefaultSkinPresets() {
  auto& presets = SkinPresets();
  if (!presets.empty()) return;

  presets.push_back({i18n::Tr("RO Classique"), RoSkinConfig{}});  // défauts natifs

  {
    RoSkinConfig dark;
    dark.title_brightness = 0.90f;
    SetColor255(dark.body_col, 44, 46, 54);
    SetColor255(dark.border_col, 90, 94, 110);
    SetColor255(dark.body_text, 226, 228, 235);
    SetColor255(dark.title_text, 255, 255, 255);
    SetColor255(dark.tab_col, 90, 120, 190);
    SetColor255(dark.tab_inact, 70, 74, 86);
    SetColor255(dark.input_col, 64, 66, 76);
    SetColor255(dark.header_col, 58, 60, 70);
    SetColor255(dark.slot_col, 64, 66, 76);
    SetColor255(dark.doll_col, 54, 56, 66);
    SetColor255(dark.card_col, 54, 56, 66);
    SetColor255(dark.card_head_col, 34, 36, 44);
    SetColor255(dark.card_head_text, 226, 228, 235);
    presets.push_back({"Sombre", dark});
  }
  {
    RoSkinConfig sepia;
    SetColor255(sepia.body_col, 244, 236, 218);
    SetColor255(sepia.border_col, 176, 150, 110);
    SetColor255(sepia.body_text, 60, 44, 24);
    SetColor255(sepia.title_text, 40, 28, 12);
    SetColor255(sepia.tab_col, 196, 166, 120);
    SetColor255(sepia.tab_inact, 226, 214, 190);
    SetColor255(sepia.input_col, 232, 222, 200);
    SetColor255(sepia.header_col, 224, 210, 184);
    SetColor255(sepia.slot_col, 232, 222, 200);
    SetColor255(sepia.doll_col, 240, 232, 214);
    SetColor255(sepia.card_col, 250, 244, 228);
    SetColor255(sepia.card_head_col, 150, 120, 80);
    SetColor255(sepia.card_head_text, 250, 244, 230);
    presets.push_back({"Sepia", sepia});
  }
}

bool DrawSkinPanel() {
  bool changed = false;

  bool font_on = IsFontEnabled();
  if (RoCheckbox(i18n::Tr("Police Malgun (UI)"), &font_on)) {
    SetFontEnabled(font_on);
    changed = true;
  }
  SameLine();
  HelpMarker(
      i18n::Tr("ON : police Malgun Gothic pour toute l'UI ImGui (latin + coréen).\n"
      "OFF : police intégrée d'ImGui (ProggyClean)."));

  // (Le skin RO n'est plus optionnel : c'est l'habillage standard des fenêtres
  // ImGui Bourgeon. Seuls ses réglages restent configurables.)
  changed |= ShowRoSkinSettings();

  // ── Presets : jeux de couleurs sauvegardés ─────────────────────────────────
  SeparatorText(i18n::Tr("Presets"));
  auto& presets = SkinPresets();
  int& selection = SkinPresetSelection();
  const int preset_count = static_cast<int>(presets.size());
  const bool valid_sel = selection >= 0 && selection < preset_count;
  const char* preview = valid_sel ? presets[selection].name.c_str() : "(choisir)";
  if (RoBeginCombo("##ro_preset", preview)) {
    for (int i = 0; i < preset_count; ++i)
      if (ImGui::Selectable(presets[i].name.c_str(), selection == i)) selection = i;
    RoEndCombo();
  }
  SameLine();
  if (RoButton("Appliquer") && valid_sel) {
    SkinConfig() = presets[selection].cfg;
    changed = true;
  }
  SameLine();
  if (RoButton("Supprimer") && valid_sel) {
    presets.erase(presets.begin() + selection);
    selection = -1;
    changed = true;
  }
  static char preset_name[32] = "";
  ImGui::InputText("##ro_preset_name", preset_name, sizeof(preset_name));
  SameLine();
  if (RoButton("Sauvegarder") && preset_name[0]) {
    bool found = false;
    for (auto& preset : presets)
      if (preset.name == preset_name) {
        preset.cfg = SkinConfig();
        found = true;
        break;
      }
    if (!found) presets.push_back({preset_name, SkinConfig()});
    changed = true;
    preset_name[0] = '\0';
  }
  SameLine();
  HelpMarker(
      i18n::Tr("Sauvegarde les couleurs/luminosité/opacité actuelles sous un nom.\n"
      "« Appliquer » recharge un préréglage ; on peut se faire plusieurs thèmes."));

  return changed;
}

}  // namespace ro
