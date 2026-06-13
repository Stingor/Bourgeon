#include "plugins/moonlight_ui.h"

#include "bourgeon.h"
#include "imgui.h"
#include "plugins/discord_relay.h"
#include "spdlog/fmt/fmt.h"

namespace {

const char* kAlootTypes[] = {"Healing", "Usable", "Etc",      "Armors",
                             "Weapons", "Card",   "PetEgg",   "PetArmor",
                             "Ammo"};
const char* kListBoxLines[] = {"Line1", "Line2", "Line3"};

}  // namespace

void MoonlightUi::OnRenderUI() {
  if (!window_open_) {
    return;
  }

  if (ImGui::Begin("Moonlight-Destiny", &window_open_)) {
    ImGui::TextUnformatted("Settings");
    ImGui::Separator();

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::Combo("Alootype", &aloot_type_, kAlootTypes,
                     IM_ARRAYSIZE(kAlootTypes))) {
      Bourgeon::Instance().AddLogLine(
          fmt::format("ALootType value selected: {}", aloot_type_));
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
      aloot_type_ = 0;
      Bourgeon::Instance().AddLogLine("ALootType has been reset");
    }

    if (ImGui::Checkbox("Discord chat Gonryun", &discord_chat_)) {
      if (auto* relay = Bourgeon::Instance().discord_relay()) {
        relay->set_chat_active(discord_chat_);
      }
      Bourgeon::Instance().AddLogLine(
          fmt::format("Discord chat: {}", discord_chat_ ? "enabled" : "disabled"));
    }

    if (ImGui::InputText("TextInput", text_input_, sizeof(text_input_))) {
      Bourgeon::Instance().AddLogLine(
          fmt::format("TextInput has been edited: {}", text_input_));
    }

    if (ImGui::ListBox("ListBox", &list_box_selection_, kListBoxLines,
                       IM_ARRAYSIZE(kListBoxLines), 3)) {
      Bourgeon::Instance().AddLogLine(
          fmt::format("ListBox item selected: {}", list_box_selection_));
    }
  }
  ImGui::End();
}
