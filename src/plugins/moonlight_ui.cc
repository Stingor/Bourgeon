#include "plugins/moonlight_ui.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "bourgeon.h"
#include "imgui.h"
#include "plugins/discord_relay.h"
#include "ragnarok/ui_window_mgr.h"
#include "spdlog/fmt/fmt.h"
#include "utils/byte_pattern.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

// Returns the path to bourgeon_settings.yaml next to the game executable.
static std::string GetSettingsPath() {
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string path(buf);
  const auto sep = path.find_last_of("\\/");
  if (sep != std::string::npos) path.resize(sep + 1);
  return path + "bourgeon_settings.yaml";
}

MoonlightUi::MoonlightUi() {
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeFromServer);
  // Observe the standard map-move packet to learn the current map name.
  Bourgeon::Instance().RegisterObserveOpcode(kOpcodeMapMove, kMapNameLen);
  FindChatBgInstruction();
}

// ── Chat background color ─────────────────────────────────────────────────

void MoonlightUi::FindChatBgInstruction() {
  // Locate the .text section via the PE header of the main module.
  const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandle(nullptr));
  const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto* sec  = IMAGE_FIRST_SECTION(nt);

  uint8_t* text_start = nullptr;
  size_t   text_size  = 0;
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (std::memcmp(sec->Name, ".text", 5) == 0) {
      text_start = const_cast<uint8_t*>(base) + sec->VirtualAddress;
      text_size  = sec->Misc.VirtualSize;
      break;
    }
  }

  if (!text_start) {
    LogError("[MoonlightUi] chat_bg: .text section not found");
    return;
  }

  // Pattern: C7 ?? D8 00 00 00 ?? ?? ?? ??
  //          ^^^^^^^^^^^^^^^^^^^^  ─ MOV [reg+0xD8], imm32
  //                                ^─────────^ ─ the ARGB value we patch (+6)
  // Wildcards on the ModRM byte (any register) and the 4-byte ARGB immediate
  // so the pattern works whether or not a WARP binary patch was already applied.
  BytePattern pat(
      {0xC7, 0x00, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      "x?xxxx????"
  );

  auto* found = static_cast<uint8_t*>(pat.Search(text_start, text_size));
  if (!found) {
    LogError("[MoonlightUi] chat_bg: init instruction not found in .text");
    return;
  }

  // The 4-byte ARGB immediate starts 6 bytes into the instruction
  // (1 opcode + 1 ModRM + 4 displacement bytes).
  chat_bg_instr_ = reinterpret_cast<uint32_t*>(found + 6);

  // Make the immediate field writable (one VirtualProtect, never restored).
  DWORD old_protect;
  VirtualProtect(chat_bg_instr_, sizeof(uint32_t),
                 PAGE_EXECUTE_READWRITE, &old_protect);

  // Seed the ImGui picker from whatever color is currently in the instruction
  // (works with the original 0x66000000 or a WARP-patched value).
  const uint32_t argb = *chat_bg_instr_;
  chat_bg_color_[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f; // R
  chat_bg_color_[1] = static_cast<float>((argb >>  8) & 0xFF) / 255.0f; // G
  chat_bg_color_[2] = static_cast<float>( argb        & 0xFF) / 255.0f; // B
  chat_bg_color_[3] = static_cast<float>((argb >> 24) & 0xFF) / 255.0f; // A

  LogInfo("[MoonlightUi] chat_bg: instruction at VA 0x{:08X}, initial color 0x{:08X}",
          reinterpret_cast<uint32_t>(found), argb);
}

void MoonlightUi::PatchInstruction(uint32_t argb) {
  if (!chat_bg_instr_) return;
  *chat_bg_instr_ = argb;
  FlushInstructionCache(GetCurrentProcess(), chat_bg_instr_, sizeof(uint32_t));
}

void MoonlightUi::PatchExistingObjects(uint32_t argb) {
  HANDLE heap = GetProcessHeap();
  if (!heap) return;
  if (!HeapLock(heap)) return;

  PROCESS_HEAP_ENTRY entry = {};
  int count = 0;
  while (HeapWalk(heap, &entry)) {
    if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
    if (entry.cbData < kChatBgColorOff + sizeof(uint32_t)) continue;

    // Chat window objects start with their vtable pointer.
    const auto* vtable_ptr = static_cast<const uint32_t*>(entry.lpData);
    if (*vtable_ptr != kChatWinVtable) continue;

    auto* color_field = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(entry.lpData) + kChatBgColorOff);
    *color_field = argb;
    ++count;
  }

  HeapUnlock(heap);
  LogInfo("[MoonlightUi] chat_bg: patched {} existing window object(s)", count);
}

uint32_t MoonlightUi::PickerToArgb() const {
  const uint32_t r = static_cast<uint32_t>(chat_bg_color_[0] * 255.0f + 0.5f) & 0xFF;
  const uint32_t g = static_cast<uint32_t>(chat_bg_color_[1] * 255.0f + 0.5f) & 0xFF;
  const uint32_t b = static_cast<uint32_t>(chat_bg_color_[2] * 255.0f + 0.5f) & 0xFF;
  const uint32_t a = static_cast<uint32_t>(chat_bg_color_[3] * 255.0f + 0.5f) & 0xFF;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// ── Settings persistence ──────────────────────────────────────────────────

void MoonlightUi::LoadSettings() {
  const std::string path = GetSettingsPath();
  std::ifstream f(path);
  if (!f) return;  // first run — no file yet

  try {
    const YAML::Node root = YAML::Load(f);
    const YAML::Node ui = root["moonlight_ui"];
    if (!ui) return;

    const std::string hex = ui["chat_bg"].as<std::string>("");
    if (hex.size() == 8) {
      const uint32_t argb = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
      chat_bg_color_[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
      chat_bg_color_[1] = static_cast<float>((argb >>  8) & 0xFF) / 255.0f;
      chat_bg_color_[2] = static_cast<float>( argb        & 0xFF) / 255.0f;
      chat_bg_color_[3] = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
      if (chat_bg_instr_) {
        PatchInstruction(argb);
        PatchExistingObjects(argb);
      }
      LogInfo("[MoonlightUi] loaded chat_bg 0x{:08X}", argb);
    }

    discord_chat_ = ui["discord_chat"].as<bool>(false);
    UpdateRelay();

    ui_collapsed_  = ui["ui_collapsed"].as<bool>(false);
    apply_collapse_ = true;
  } catch (const std::exception& e) {
    LogError("[MoonlightUi] failed to parse {}: {}", path, e.what());
  }
}

void MoonlightUi::SaveSettings() {
  char hex[9];
  std::snprintf(hex, sizeof(hex), "%08X", PickerToArgb());

  YAML::Emitter out;
  out << YAML::BeginMap
      << YAML::Key << "moonlight_ui"
      << YAML::Value << YAML::BeginMap
        << YAML::Key << "chat_bg"      << YAML::Value << hex
        << YAML::Key << "discord_chat" << YAML::Value << discord_chat_
        << YAML::Key << "ui_collapsed" << YAML::Value << ui_collapsed_
      << YAML::EndMap
      << YAML::EndMap;

  const std::string path = GetSettingsPath();
  std::ofstream f(path);
  if (!f) {
    LogError("[MoonlightUi] failed to write {}", path);
    return;
  }
  f << out.c_str();
  LogInfo("[MoonlightUi] saved chat_bg {} to {}", hex, path);
}

// ── Server settings sync ──────────────────────────────────────────────────

void MoonlightUi::UpdateRelay() {
  if (auto* relay = Bourgeon::Instance().discord_relay()) {
    relay->set_chat_active(discord_chat_ && in_gonryun_);
  }
}

void MoonlightUi::OnModeSwitch(ModeMgr::ModeType mode_type,
                               const char* map_name) {
  const bool was_in_game = in_game_;
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);

  // Only update in_gonryun_ when we have a real map name. OnUpdateHook fires
  // FireModeSwitch(kGame, "") on every tick for in_game_ tracking; that empty
  // call must not override the map we learned from a real CModeMgr::Switch.
  if (map_name && map_name[0] != '\0') {
    in_gonryun_ = in_game_ && (strncmp(map_name, kDiscordMap, sizeof(kDiscordMap) - 1) == 0);
  } else if (!in_game_) {
    in_gonryun_ = false;
  }

  if (in_game_ && !was_in_game)
    LoadSettings();

  UpdateRelay();
}

// ZC packet layout (data points past [opcode:2][total_len:2]):
//   [count:2][{id:2, value:2} * count]
void MoonlightUi::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  if (opcode == kOpcodeMapMove) {
    // 0x0091 ZC_NPCACK_MAPMOVE: data points at mapname[16] (e.g. "gonryun.gat").
    const char* map_name = reinterpret_cast<const char*>(data);
    in_gonryun_ = in_game_ &&
                  (strncmp(map_name, kDiscordMap, sizeof(kDiscordMap) - 1) == 0);
    LogInfo("[MoonlightUi] map move -> '{}' in_gonryun={}",
            std::string(map_name, strnlen(map_name, len)), in_gonryun_);
    UpdateRelay();
    return;
  }

  if (opcode != kOpcodeFromServer) return;
  // Layout after the [opcode:2][len:2] header: [char_id:4][count:2][{id,value}*].
  if (len < 6) return;

  const uint16_t count = *reinterpret_cast<const uint16_t*>(data + 4);
  const uint16_t expected_len = static_cast<uint16_t>(6 + count * 4);
  if (len < expected_len) {
    LogError("[MoonlightUi] ZC_BOURGEON_SETTINGS truncated: len={} count={}", len, count);
    return;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const uint16_t id    = *reinterpret_cast<const uint16_t*>(data + 6 + i * 4);
    const uint16_t value = *reinterpret_cast<const uint16_t*>(data + 6 + i * 4 + 2);
    switch (id) {
      case kSettingShowExp:
        show_exp_ = (value != 0);
        LogInfo("[MoonlightUi] show_exp={}", show_exp_);
        break;
      case kSettingShowZeny:
        show_zeny_ = (value != 0);
        LogInfo("[MoonlightUi] show_zeny={}", show_zeny_);
        break;
      case kSettingShowMobInfo:
        show_mob_info_ = (value != 0);
        LogInfo("[MoonlightUi] show_mob_info={}", show_mob_info_);
        break;
      case kSettingSeparate:
        separate_ = (value != 0);
        LogInfo("[MoonlightUi] separate={}", separate_);
        break;
      case kSettingBlockExp:
        block_exp_ = (value != 0);
        LogInfo("[MoonlightUi] block_exp={}", block_exp_);
        break;
      case kSettingAlootRare:
        aloot_rare_ = (value != 0);
        LogInfo("[MoonlightUi] aloot_rare={}", aloot_rare_);
        break;
      default:
        LogInfo("[MoonlightUi] unknown setting id={} value={}", id, value);
        break;
    }
  }
}

void MoonlightUi::SendSetting(uint16_t id, uint16_t value) {
  uint8_t buf[8];
  *reinterpret_cast<uint16_t*>(buf)     = kOpcodeToServer;
  *reinterpret_cast<uint16_t*>(buf + 2) = 8;
  *reinterpret_cast<uint16_t*>(buf + 4) = id;
  *reinterpret_cast<uint16_t*>(buf + 6) = value;
  Bourgeon::Instance().SendPacket(buf, sizeof(buf));
}

// Helper to display a little (?) mark which shows a tooltip when hovered.
// In your own code you may want to display an actual icon if you are using a merged icon fonts (see docs/FONTS.md)
static void HelpMarker(const char* desc)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ── ImGui panel ───────────────────────────────────────────────────────────

void MoonlightUi::OnRenderUI() {
  if (!in_game_) return;

  if (apply_collapse_) {
    ImGui::SetNextWindowCollapsed(ui_collapsed_, ImGuiCond_Always);
    apply_collapse_ = false;
  }

  ImGui::Begin("Moonlight-Destiny");

  const bool is_collapsed = ImGui::IsWindowCollapsed();
  if (is_collapsed != ui_collapsed_) {
    ui_collapsed_ = is_collapsed;
    SaveSettings();
  }

  if (!is_collapsed) {

    if (ImGui::CollapsingHeader("Règles du serveur"))
    {
      ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "CES RÈGLEMENTS S'APPLIQUENT PARTOUT SUR MOONLIGHT-DESTINY !");
      if (ImGui::TreeNode("Règlements généraux"))
      {
          ImGui::Text("Les règles du serveur doivent être appliquées à la lettre.\nToute personne ne respectant pas la charte sera sanctionnée dans les plus brefs délais.");
          ImGui::Spacing();
          ImGui::BulletText("Les joueurs doivent se respecter et garder un langage propre et courtois.");
          ImGui::BulletText("Les propos visant à rejeter un nouveau joueur sont interdits.");
          ImGui::BulletText("L'utilisation de programmes tels que bots ou hacks = ban définitif sans hésitation.");
          ImGui::BulletText("Le flood est strictement interdit.");
          ImGui::BulletText("Vous êtes entièrement responsable de votre compte.");
          ImGui::BulletText("Le staff ne rend pas les items perdus (vente NPC, deslotage raté, refine raté).");
          ImGui::BulletText("Le staff peut exceptionnellement rendre un item perdu si les logs prouvent un bug serveur.");
          ImGui::BulletText("Ne partagez jamais votre compte ou votre mot de passe.");
          ImGui::BulletText("La demande de support pour créer un serveur privé est non recommandée.");
          ImGui::BulletText("Le plagiat volontaire d'un membre du staff est puni.");
          ImGui::BulletText("Tout ce qui se rapporte au serveur est la propriété exclusive des administrateurs.");
          ImGui::BulletText("Le langage SMS est à proscrire.");
          ImGui::BulletText("L'exploitation d'un bug ou abus = sanction. Prévenez immédiatement un administrateur.");
          ImGui::BulletText("Si vous abusez du cashshop en votant avec plusieurs comptes forum… \ngare à vous c'est comme avec les impôts, \ntant qu'on est pas contrôlé c'est la fête, mais quand ils vous tombent dessus...");
          ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Sur le serveur de jeu"))
      {
          ImGui::BulletText("Insultes et vols de drop (Looting) = INTERDITS.");
          ImGui::BulletText("Heal ou buff un monstre qui ne vous appartient pas sans accord = puni.");
          ImGui::BulletText("Si vous êtes banni définitivement, tous les comptes liés à votre IP/PC le seront aussi.");
          ImGui::BulletText("Les sanctions (mute, jail, kick, ban) sont à la discrétion du staff.");
          ImGui::BulletText("Le Kill Steal est strictement interdit (voir définition). Utilisez @noks pour vous protéger.");
          ImGui::Spacing();
          ImGui::BulletText("Les MVPs sont FFA :");
          ImGui::Indent();
          ImGui::Text("Vous pouvez les attaquer même si quelqu'un est dessus.");
          ImGui::Text("(À vous de voir si vous voulez passer pour un gros connard selfish en KSant le MVP)");
          ImGui::Text("Si vous ne voulez pas vous faire KS, faites @noks <3");
          ImGui::Unindent();
          ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Le staff"))
      {
          ImGui::BulletText("Si vous cassez les couilles du staff ban/delete non temporaire.");
          ImGui::BulletText("Aucun membre du staff ne vous demandera votre mot de passe.");
          ImGui::BulletText("Aucun membre du staff ne vous demandera votre login.");
          ImGui::BulletText("Aucun membre du staff ne vous demandera votre email.");
          ImGui::BulletText("Seuls les admins peuvent rendre des items perdus suite à un bug serveur.");
          ImGui::BulletText("Le staff ne rend pas les items prêtés à un joueur disparu/banni.");
          ImGui::BulletText("Le staff ne donne pas d'items (hors events).");
          ImGui::BulletText("Les membres du staff ne sont pas des robots. Soyez courtois, cherchez avant de demander.");
          ImGui::BulletText("Les questions dont la réponse est sur une database = évitez.");
          ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Règlements dans les endroits spécifiques"))
      {
          if (ImGui::TreeNode("Salle de duel"))
          {
              ImGui::BulletText("Ce n'est pas un salon de thé");
              ImGui::BulletText("Si vous regardez, ok. Sinon, laissez la place.");
              ImGui::BulletText("Utilisez : @duel, @invite, @accept, @reject, @leave.");
              ImGui::TreePop();
          }
          if (ImGui::TreeNode("Carnage Room"))
          {
              ImGui::BulletText("Loi du plus fort.");
              ImGui::BulletText("Amusez‑vous dans le respect.");
              ImGui::TreePop();
          }
          if (ImGui::TreeNode("PVP Room"))
          {
              ImGui::BulletText("Free Kill interdit.");
              ImGui::TreePop();
          }
          if (ImGui::TreeNode("DB Room"))
          {
              ImGui::BulletText("Kill Steal STRICTEMENT interdit.");
              ImGui::BulletText("Si la personne meurt ou se hide les mobs sont à vous.");
              ImGui::TreePop();
          }
          if (ImGui::TreeNode("Guild Dungeon"))
          {
              ImGui::BulletText("Libre de tuer les guildiens adverses.");
              ImGui::TreePop();
          }
          if (ImGui::TreeNode("WoE Castles"))
          {
              ImGui::BulletText("Interdiction d'apporter de l'aide via un perso non participant (multi-account/perso).");
              ImGui::BulletText("Les ententes entre guildes sont informelles, non officielles, non sanctionnables.");
              ImGui::BulletText("Elles doivent être discutées entre guildes dominantes, dans le respect.");
              ImGui::TreePop();
            
          }
          ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Logiciels tiers"))
      {
          ImGui::Text("Autorisations :");
          ImGui::Indent();
            ImGui::BulletText("Je vais être clair : oui, j'autorise les scripts AHK, les macros clavier/souris, les trucs qui bouclent un sort… tant que ça reste :");
            ImGui::BulletText("SIMPLE");
            ImGui::BulletText("BASIQUE");
            ImGui::BulletText("Pas un tableau de bord de la NASA");
            ImGui::BulletText("Vous bouclez le spell, éventuellement un clic en plus pour les AOE type Storm Gust, et basta.");
          ImGui::Unindent();
          ImGui::Text("Quality of Life :");
          ImGui::Indent();
            ImGui::BulletText("Le but, c'est du Q.O.L");
            ImGui::BulletText("Vous préservez votre clavier, votre souris, vos doigts, vos poignets, vos oreilles, et celles de vos voisins qui n'ont rien demandé.");
            ImGui::BulletText("Bref : du confort, pas du cheat.");
          ImGui::Unindent();
          ImGui::Text("Les trucs interdits (et je rigole zéro) :");
          ImGui::Indent();
            ImGui::BulletText("Ne me prenez pas pour un jambon.");
            ImGui::Text("Si vous me sortez :");
            ImGui::Indent();
              ImGui::BulletText("un auto-buffer");
              ImGui::BulletText("un auto-pot");
              ImGui::BulletText("un super TP/SG de physicien quantique");
              ImGui::BulletText("un script qui ferait rougir Tony Stark");
            ImGui::Unindent();
            ImGui::Text("Alors là :");
            ImGui::Indent();
              ImGui::BulletText("Je vous fais le fion.");
              ImGui::BulletText("Je m'en bats les couilles.");
              ImGui::BulletText("Je vous dégage plus vite que Thanos avec son finger snap. *Snap*");
            ImGui::Unindent();
            ImGui::Text("Les excuses bidon :");
            ImGui::Indent();
              ImGui::BulletText("\"Mais les autres serveurs le font...\"");
              ImGui::BulletText("\"Mais j'étais pas AFK, je regardais Naruto à côté...\"");
            ImGui::Unindent();
            ImGui::Text("Résultat :");
            ImGui::Indent();
              ImGui::BulletText("Pouf.");
              ImGui::BulletText("Vous étiez sur Moon.");
              ImGui::BulletText("Vous ne l'êtes plus.");
              ImGui::BulletText("Et il ne restera de vous que des ruines numériques sur Wayback Machine.");
            ImGui::Unindent();
          ImGui::Unindent();
          ImGui::TreePop();
      }
      ImGui::Spacing();
    }

    // ── Chat Box Settings ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Chat Settings"))
    {
        if (ImGui::Checkbox("Chat Discord (Gonryun only)", &discord_chat_)) {
          UpdateRelay();
          SaveSettings();
          const char* msg = discord_chat_
              ? "Discord relay : ACTIVE"
              : "Discord relay : DESACTIVE";
          UIWindowMgr::SendMsg(UIMessage::UIM_PUSHINTOCHATHISTORY,
                              reinterpret_cast<int>(msg), 0x00FF00, 0, 0);
        }

          ImGui::SameLine();
        // ── Chat Background Color ─────────────────────────────────────────────
        if (chat_bg_instr_) {
          // Color swatch — click to open the picker popup.
          const ImVec4 swatch(chat_bg_color_[0], chat_bg_color_[1],
                              chat_bg_color_[2], chat_bg_color_[3]);
          if (ImGui::ColorButton("##chatbg_btn", swatch,
                                ImGuiColorEditFlags_AlphaPreview, ImVec2(20, 20)))
            ImGui::OpenPopup("chatbg_picker");

          ImGui::SameLine();
          ImGui::TextUnformatted("Background Chat Color");

          // Popup with full picker + explicit Close button.
          if (ImGui::BeginPopup("chatbg_picker")) {
            if (ImGui::ColorPicker4("##chatbg", chat_bg_color_,
                                    ImGuiColorEditFlags_AlphaBar |
                                    ImGuiColorEditFlags_NoSidePreview)) {
              PatchInstruction(PickerToArgb());
              picker_was_editing_ = true;
            }
            if (picker_was_editing_ && ImGui::IsMouseReleased(0)) {
              const uint32_t argb = PickerToArgb();
              PatchExistingObjects(argb);
              SaveSettings();
              picker_was_editing_ = false;
            }
            ImGui::Separator();
            if (ImGui::Button("Close", ImVec2(-1.0f, 0.0f)))
              ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
          }
        } else {
          ImGui::TextDisabled("(chat background patch unavailable)");
        }
    }
    // ── Commands Settings ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Commands Settings"))
    {
        if (ImGui::BeginTable("split", 2))
        {
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show EXP gain", &show_exp_)) SendSetting(kSettingShowExp, show_exp_ ? 1 : 0);
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show Zeny gain", &show_zeny_)) SendSetting(kSettingShowZeny, show_zeny_ ? 1 : 0);
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show mob info", &show_mob_info_)) SendSetting(kSettingShowMobInfo, show_mob_info_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche la RACE et l'ELEMENT des monstres,\nsous leur nom. (Thx Doo)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Separate Mob Kill in chat", &separate_)) SendSetting(kSettingSeparate, separate_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche un séparateur dans le chat log entre chaque kill de mobs. (Demandez à Spider)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Block EXP Gain", &block_exp_)) SendSetting(kSettingBlockExp, block_exp_ ? 1 : 0);
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Autoloot rares items", &aloot_rare_)) SendSetting(kSettingAlootRare, aloot_rare_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker(
                "Autolooting: Toutes les Cards\nOld Blue Box (603)\nYggdrasil Berry (607)\nYggdrasil Seed (608)\nOld Card Album (616)\nOld Purple Box (617)\nGift Box (644)\nGold (969)\n"
                "Temporal Crystal (6607)\nCoagulated Spell (6608)\nJitterbug's Tooth (6719)\nFragment of Agony (7436)\nFragment of Misery (7437)\nFragment of Hatred (7438)\n"
                "Piece_Of_Memory_Red (7439)\nTreasure Box (7444)\nCursed Water (12020)\nElemental Converter Fire (12114)\nElemental Converter Water (12115)\n"
                "Elemental Converter Earth (12116)\nElemental Converter Wind (12117)\nMystical Card Album (12246)\nSentimental Fragment (22687)\nCursed Fragment (23016)");
            ImGui::EndTable();
        }
    }
  }
  ImGui::End();
}
