#include "bourgeon.h"

#include <Windows.h>

#include "imgui.h"
#include "plugins/auto_login.h"
#include "plugins/chat.h"
#include "plugins/cheat_detector.h"
#include "plugins/discord_relay.h"
#include "plugins/basic_info.h"
#include "plugins/dps_meter.h"
#include "plugins/menu_icons.h"
#include "plugins/integrity_check.h"
#include "plugins/moonlight_ui.h"
#include "plugins/status_tweaks.h"
#include "plugins/inventory_tweaks.h"
#include "plugins/equip_tweaks.h"
#include "plugins/window_pos_tweaks.h"
#include "plugins/status_icon_tweaks.h"
#include "plugins/quest_tracker_tweaks.h"
#include "plugins/settings_tweaks.h"
#include "plugins/weapon_layer.h"
#include "plugins/fps_view.h"
#include "plugins/doom_tweaks.h"
#include "plugins/roggle_tweaks.h"
#include "plugins/rojeweled_tweaks.h"
#include "plugins/skill_bar_tweaks.h"
#include "plugins/item_desc_tweaks.h"
#include "utils/log_console.h"

Bourgeon::Bourgeon()
    : plugins_(), last_tick_count_(), log_lines_(), client_() {}

RagnarokClient& Bourgeon::client() { return client_; }
DiscordRelay* Bourgeon::discord_relay() { return discord_relay_; }
DpsMeter*     Bourgeon::dps_meter()     { return dps_meter_; }
BasicInfoTweaks* Bourgeon::basic_info() { return basic_info_; }
MenuIconTweaks* Bourgeon::menu_icons()  { return menu_icons_; }
StatusIconTweaks* Bourgeon::status_icons() { return status_icons_; }
QuestTrackerTweaks* Bourgeon::quest_tracker() { return quest_tracker_; }
SettingsTweaks* Bourgeon::settings_tweaks() { return settings_tweaks_; }
FpsViewTweaks* Bourgeon::fps_view() { return fps_view_; }
DoomTweaks* Bourgeon::doom() { return doom_; }
RoggleTweaks* Bourgeon::roggle() { return roggle_; }
RojeweledTweaks* Bourgeon::rojeweled() { return rojeweled_; }
MoonlightUi* Bourgeon::moonlight_ui() { return moonlight_ui_; }
SkillBarTweaks* Bourgeon::skill_bar() { return skill_bar_; }
ItemDescTweaks* Bourgeon::item_desc() { return item_desc_; }

bool Bourgeon::Initialize() {
  LogInfo("Bourgeon {}\n", BOURGEON_VERSION);

  if (!client_.Initialize()) {
    LogError("Bourgeon failed to initialize");
    return false;
  }

  LogInfo("Bourgeon initialized successfully!");
  LoadPlugins();

  return true;
}

void Bourgeon::OnTick() {
  // Only run once every 100ms (6 frames at 60fps)
  const auto current_tick_count = GetTickCount();
  if (current_tick_count >= last_tick_count_ &&
      current_tick_count <= last_tick_count_ + 100) {
    return;
  }
  last_tick_count_ = current_tick_count;

  for (auto& plugin : plugins_) {
    try {
      plugin->OnTick();
    } catch (const std::exception& error) {
      LogError("[{}] OnTick: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::OnProcessInput() {
  // Runs every frame in the game's input phase (NOT throttled like OnTick) so a
  // menu-icon click dispatches with the same timing/context as a native click —
  // OnTick's ~100ms throttle delayed it to a random frame, which made heavy
  // windows (world map) crash intermittently.
  if (auto* mi = menu_icons()) mi->FlushPending();
  // Cache les fenêtres de description natives DANS LA PHASE INPUT (par frame, non
  // throttlé) -> flicker ~nul à l'ouverture d'un skill (dont l'OnMsg n'est PAS
  // hookée, contrairement à l'item). Idempotent/sûr (re-cache chaque frame).
  if (auto* idt = item_desc()) {
    idt->HideNativeDescWindows();  // item 0xc + comparaison 0xea
    idt->HideNativeSkillWindow();  // skill 0x2e
  }
}

void Bourgeon::AddLogLine(std::string log_line) {
  LogInfo("[plugin] {}", log_line);
  log_lines_.emplace_back(std::move(log_line));
}

void Bourgeon::RenderUI() {
  // if (strstr(GetCommandLineA(), "--console") != nullptr) { // Render Bourgeon's main window
  // ShowBourgeonWindow();
  // }
  if (strstr(GetCommandLineA(), "--demo") != nullptr) {
    ImGui::ShowDemoWindow();
  }
  // Render windows drawn by plugins
  for (auto& plugin : plugins_) {
    try {
      plugin->OnRenderUI();
    } catch (const std::exception& error) {
      LogError("[{}] OnRenderUI: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::FireModeSwitch(ModeMgr::ModeType mode_type,
                              const char* map_name) {
  for (auto& plugin : plugins_) {
    try {
      plugin->OnModeSwitch(mode_type, map_name);
    } catch (const std::exception& error) {
      LogError("[{}] OnModeSwitch: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::FireTalkType(const char* chat_buffer) {
  for (auto& plugin : plugins_) {
    try {
      plugin->OnTalkType(chat_buffer);
    } catch (const std::exception& error) {
      LogError("[{}] OnTalkType: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::FireChatMessage(const char* chat_buffer) {
  for (auto& plugin : plugins_) {
    try {
      plugin->OnChatMessage(chat_buffer);
    } catch (const std::exception& error) {
      LogError("[{}] OnChatMessage: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::FireKeyDown(unsigned long vkey, int new_key, int accurate_key) {
  for (auto& plugin : plugins_) {
    try {
      plugin->OnKeyDown(vkey, new_key, accurate_key);
    } catch (const std::exception& error) {
      LogError("[{}] OnKeyDown: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::FireRecvPacket(uint16_t opcode, const uint8_t* data,
                              uint16_t len) {
  for (auto& plugin : plugins_) {
    try {
      plugin->OnRecvPacket(opcode, data, len);
    } catch (const std::exception& error) {
      LogError("[{}] OnRecvPacket: {}", plugin->name(), error.what());
    }
  }
}

bool Bourgeon::SendPacket(const uint8_t* buf, size_t len) {
  return client_.rag_connection().SendPacket(
      static_cast<int>(len), reinterpret_cast<char*>(const_cast<uint8_t*>(buf)));
}

void Bourgeon::RegisterRecvOpcode(uint16_t opcode) {
  client_.rag_connection().RegisterRecvOpcode(opcode);
}

void Bourgeon::RegisterObserveOpcode(uint16_t opcode, uint16_t forward_len) {
  client_.rag_connection().RegisterObserveOpcode(opcode, forward_len);
}

void Bourgeon::LoadPlugins() {
  plugins_.emplace_back(std::make_unique<AutoLogin>());
  plugins_.emplace_back(std::make_unique<IntegrityCheck>());
  plugins_.emplace_back(std::make_unique<CheatDetector>());
  {
    auto moonlight_ui = std::make_unique<MoonlightUi>();
    moonlight_ui_ = moonlight_ui.get();
    plugins_.emplace_back(std::move(moonlight_ui));
  }
  plugins_.emplace_back(std::make_unique<ChatTweaks>());
  plugins_.emplace_back(std::make_unique<StatusTweaks>());
  plugins_.emplace_back(std::make_unique<InventoryTweaks>());
  plugins_.emplace_back(std::make_unique<EquipTweaks>());
  plugins_.emplace_back(std::make_unique<WindowPosTweaks>());
  plugins_.emplace_back(std::make_unique<WeaponLayerTweaks>());
  {
    auto fps_view = std::make_unique<FpsViewTweaks>();
    fps_view_ = fps_view.get();
    plugins_.emplace_back(std::move(fps_view));
  }
  {
    auto doom = std::make_unique<DoomTweaks>();
    doom_ = doom.get();
    plugins_.emplace_back(std::move(doom));
  }
  {
    auto roggle = std::make_unique<RoggleTweaks>();
    roggle_ = roggle.get();
    plugins_.emplace_back(std::move(roggle));
  }
  {
    auto rojeweled = std::make_unique<RojeweledTweaks>();
    rojeweled_ = rojeweled.get();
    plugins_.emplace_back(std::move(rojeweled));
  }
  {
    auto item_desc = std::make_unique<ItemDescTweaks>();
    item_desc_ = item_desc.get();
    plugins_.emplace_back(std::move(item_desc));
  }
  {
    auto skill_bar = std::make_unique<SkillBarTweaks>();
    skill_bar_ = skill_bar.get();
    plugins_.emplace_back(std::move(skill_bar));
  }
  {
    auto status_icons = std::make_unique<StatusIconTweaks>();
    status_icons_ = status_icons.get();
    plugins_.emplace_back(std::move(status_icons));
  }
  {
    auto quest_tracker = std::make_unique<QuestTrackerTweaks>();
    quest_tracker_ = quest_tracker.get();
    plugins_.emplace_back(std::move(quest_tracker));
  }
  {
    auto settings_tweaks = std::make_unique<SettingsTweaks>();
    settings_tweaks_ = settings_tweaks.get();
    plugins_.emplace_back(std::move(settings_tweaks));
  }
  {
    auto dps = std::make_unique<DpsMeter>();
    dps_meter_ = dps.get();
    plugins_.emplace_back(std::move(dps));
  }
  {
    auto basic_info = std::make_unique<BasicInfoTweaks>();
    basic_info_ = basic_info.get();
    plugins_.emplace_back(std::move(basic_info));
  }
  {
    auto menu_icons = std::make_unique<MenuIconTweaks>();
    menu_icons_ = menu_icons.get();
    plugins_.emplace_back(std::move(menu_icons));
  }
  {
    auto relay = std::make_unique<DiscordRelay>();
    discord_relay_ = relay.get();
    plugins_.emplace_back(std::move(relay));
  }

  for (const auto& plugin : plugins_) {
    LogInfo("Loaded plugin: {}", plugin->name());
  }
}

void Bourgeon::ShowBourgeonWindow() const {
  ImGui::Begin("Bourgeon");

  // List of loaded plugins
  if (ImGui::CollapsingHeader("Loaded plugins")) {
    for (const auto& plugin : plugins_) {
      ImGui::BulletText("%s", plugin->name());
    }
  }

  // Logs: a live mirror of every LogInfo/LogDiag/LogError (fed by the in-memory
  // spdlog sink), not just the plugin lines pushed via AddLogLine.  Snapshotted
  // each frame so it stays thread-safe against sinks running on other threads.
  if (ImGui::CollapsingHeader("Logs")) {
    std::vector<std::string> log_lines;
    LogLineBuffer::instance().Snapshot(&log_lines);
    ImGui::BeginChild("scrolling", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    // Clipped lines
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(log_lines.size()));
    while (clipper.Step()) {
      for (int line_no = clipper.DisplayStart; line_no < clipper.DisplayEnd;
           line_no++) {
        ImGui::TextUnformatted(log_lines[line_no].c_str());
      }
    }
    clipper.End();
    ImGui::PopStyleVar();

    // Auto-scroll when at the bottom
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
      ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
  }

  ImGui::End();
}
