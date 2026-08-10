#include "features/moonlight_ui/settings_containers.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <cstdio>

#include "bourgeon.h"
#include "features/overlays/basic_info.h"
#include "features/patches/chat.h"
#include "features/windows/character_sheet.h"
#include "features/windows/entity_context_menu.h"
#include "features/patches/equip_tweaks.h"
#include "features/windows/inventory_viewer.h"
#include "features/overlays/menu_icons.h"
#include "features/moonlight_ui/settings_table.h"  // ReadArgbKey / WriteArgbKey
#include "features/overlays/skill_bar.h"
#include "features/patches/status_tweaks.h"
#include "features/windows/storage_window.h"
#include "features/patches/window_pos_tweaks.h"
#include "ui/color_codec.h"
#include "ui/ro_imgui.h"
#include "ui/skin_panel.h"

namespace moonlight_ui {
namespace {

// Les 14 couleurs du skin RO, dans l'ordre d'émission du yaml. Elles étaient
// épelées QUATRE fois — lecture ro_skin_*, lecture preset, écriture ro_skin_*,
// écriture preset — sans que rien ne garantisse que les quatre listes restent
// d'accord : ajouter une couleur au skin demandait quatre éditions, en oublier
// une donnait un réglage qui se perd au relancement sans aucun diagnostic.
using SkinColorRef = float (ro::RoSkinConfig::*)[4];
struct SkinColorField {
  const char* key;      // suffixe ; préfixé « ro_skin_ » hors des presets
  SkinColorRef member;
};
constexpr SkinColorField kSkinColorFields[] = {
    {"body",     &ro::RoSkinConfig::body_col},
    {"border",   &ro::RoSkinConfig::border_col},
    {"titletx",  &ro::RoSkinConfig::title_text},
    {"bodytx",   &ro::RoSkinConfig::body_text},
    {"tab",      &ro::RoSkinConfig::tab_col},
    {"tabinact", &ro::RoSkinConfig::tab_inact},
    {"input",    &ro::RoSkinConfig::input_col},
    {"header",   &ro::RoSkinConfig::header_col},
    {"slot",     &ro::RoSkinConfig::slot_col},
    {"doll",     &ro::RoSkinConfig::doll_col},
    {"card",     &ro::RoSkinConfig::card_col},
    {"cardhead", &ro::RoSkinConfig::card_head_col},
    {"cardtx",   &ro::RoSkinConfig::card_head_text},
    {"list",     &ro::RoSkinConfig::list_col},
};

// `with_rounding` : cf. l'avertissement de settings_containers.h. Le paramètre
// garde aussi l'ordre des clés identique aux deux sites, pour que le premier
// yaml réécrit ne diffère pas.
void ReadSkinCfg(const YAML::Node& node, ro::RoSkinConfig& cfg,
                 const std::string& prefix, bool with_rounding) {
  cfg.title_brightness = node[prefix + "bright"].as<float>(cfg.title_brightness);
  if (with_rounding) cfg.rounding = node[prefix + "rounding"].as<float>(cfg.rounding);
  cfg.alpha = node[prefix + "alpha"].as<float>(cfg.alpha);
  for (const SkinColorField& field : kSkinColorFields) {
    const YAML::Node color = node[prefix + field.key];
    if (color) ro::PickerFromImU32(color.as<unsigned>(0), cfg.*field.member);
  }
}

void EmitSkinCfg(YAML::Emitter& out, const ro::RoSkinConfig& cfg,
                 const std::string& prefix, bool with_rounding) {
  out << YAML::Key << prefix + "bright" << YAML::Value << cfg.title_brightness;
  if (with_rounding)
    out << YAML::Key << prefix + "rounding" << YAML::Value << cfg.rounding;
  out << YAML::Key << prefix + "alpha" << YAML::Value << cfg.alpha;
  for (const SkinColorField& field : kSkinColorFields)
    out << YAML::Key << prefix + field.key << YAML::Value
        << ro::ImU32FromPicker(cfg.*field.member);
}

}  // namespace

void ReadMenuIcons(const YAML::Node& ui) {
  auto* menu_icons = Bourgeon::Instance().menu_icons();
  if (!menu_icons) return;
  menu_icons->enabled_   = ui["menu_icons_enabled"].as<bool>(menu_icons->enabled_);
  menu_icons->edit_mode_ = ui["menu_icons_edit"].as<bool>(false);
  menu_icons->saved_.clear();
  const YAML::Node icons = ui["menu_icons"];
  if (!icons) return;
  for (auto it = icons.begin(); it != icons.end(); ++it) {
    const std::string name = it->first.as<std::string>("");
    if (name.empty()) continue;
    MenuIcons::IconSave saved;
    saved.x      = it->second["x"].as<int>(-1);
    saved.y      = it->second["y"].as<int>(-1);
    saved.hidden = it->second["hidden"].as<bool>(false);
    saved.valid  = true;
    menu_icons->saved_[name] = saved;
  }
}

void WriteMenuIcons(YAML::Emitter& out) {
  auto* menu_icons = Bourgeon::Instance().menu_icons();
  out << YAML::Key << "menu_icons_enabled"
      << YAML::Value << (menu_icons ? menu_icons->enabled_ : false)
      << YAML::Key << "menu_icons_edit"
      << YAML::Value << (menu_icons ? menu_icons->edit_mode_ : false)
      << YAML::Key << "menu_icons" << YAML::Value << YAML::BeginMap;
  if (menu_icons) {
    for (const auto& entry : menu_icons->saved_) {
      out << YAML::Key << entry.first << YAML::Value << YAML::BeginMap
          << YAML::Key << "x"      << YAML::Value << entry.second.x
          << YAML::Key << "y"      << YAML::Value << entry.second.y
          << YAML::Key << "hidden" << YAML::Value << entry.second.hidden
          << YAML::EndMap;
    }
  }
  out << YAML::EndMap;
}

void ReadInventoryLayout(const YAML::Node& ui) {
  auto* inventory = Bourgeon::Instance().inventory_viewer();
  if (!inventory) return;
  const YAML::Node layout = ui["inventory_layout"];
  if (!layout) return;
  inventory->layout_.clear();
  for (auto it = layout.begin(); it != layout.end(); ++it) {
    const uint32_t nameid = it->first.as<uint32_t>(0);
    const int cell = it->second.as<int>(-1);
    if (nameid != 0 && cell >= 0) inventory->layout_[nameid] = cell;
  }
}

void WriteInventoryLayout(YAML::Emitter& out) {
  auto* inventory = Bourgeon::Instance().inventory_viewer();
  // Trié pour un yaml stable (pas de diff parasite d'une sauvegarde à l'autre).
  out << YAML::Key << "inventory_layout" << YAML::Value << YAML::Flow << YAML::BeginMap;
  if (inventory) {
    std::vector<std::pair<uint32_t, int>> sorted(inventory->layout_.begin(),
                                                 inventory->layout_.end());
    std::sort(sorted.begin(), sorted.end());
    for (const auto& entry : sorted)
      out << YAML::Key << entry.first << YAML::Value << entry.second;
  }
  out << YAML::EndMap;
}

void ReadBlockedNpcs(const YAML::Node& ui) {
  auto* menu = Bourgeon::Instance().entity_context_menu();
  if (!menu) return;
  const YAML::Node blocked = ui["ctxmenu_blocked_npcs"];
  if (!blocked) return;
  menu->blocked_npcs_.clear();
  for (const YAML::Node& entry : blocked) {
    const uint32_t gid = entry["id"].as<uint32_t>(0);
    // Hors de la plage réservée, l'entrée ne peut désigner aucun NPC épinglé :
    // un GID dynamique recyclé viserait un jour une autre entité. On la jette
    // plutôt que de bloquer au hasard.
    if (!EntityContextMenu::IsFixedIdNpc(gid)) continue;
    menu->blocked_npcs_[gid] = entry["name"].as<std::string>("");
  }
}

void WriteBlockedNpcs(YAML::Emitter& out) {
  auto* menu = Bourgeon::Instance().entity_context_menu();
  // La std::map est déjà ordonnée par GID : le fichier est stable d'une
  // sauvegarde à l'autre sans tri supplémentaire.
  out << YAML::Key << "ctxmenu_blocked_npcs" << YAML::Value << YAML::BeginSeq;
  if (menu) {
    for (const auto& entry : menu->blocked_npcs_) {
      out << YAML::Flow << YAML::BeginMap;
      out << YAML::Key << "id" << YAML::Value << entry.first;
      out << YAML::Key << "name" << YAML::Value << entry.second;
      out << YAML::EndMap;
    }
  }
  out << YAML::EndSeq;
}

void ReadStorageFavorites(const YAML::Node& ui) {
  auto* storage = Bourgeon::Instance().storage_window();
  if (!storage) return;
  const YAML::Node favorites = ui["storage_favorites"];
  if (!favorites) return;
  storage->favorites_.clear();
  for (const YAML::Node& entry : favorites) {
    const uint32_t nameid = entry.as<uint32_t>(0);
    if (nameid != 0) storage->favorites_.insert(nameid);
  }
}

void WriteStorageFavorites(YAML::Emitter& out) {
  auto* storage = Bourgeon::Instance().storage_window();
  // Triés pour un yaml stable, comme le placement libre de l'inventaire.
  out << YAML::Key << "storage_favorites" << YAML::Value << YAML::Flow << YAML::BeginSeq;
  if (storage) {
    std::vector<uint32_t> sorted(storage->favorites_.begin(), storage->favorites_.end());
    std::sort(sorted.begin(), sorted.end());
    for (const uint32_t nameid : sorted) out << nameid;
  }
  out << YAML::EndSeq;
}

void ReadStorageTabCustom(const YAML::Node& ui) {
  auto* storage = Bourgeon::Instance().storage_window();
  if (!storage) return;
  const YAML::Node tabs = ui["storage_tab_custom"];
  if (!tabs) return;
  storage->tab_custom_.clear();
  for (const YAML::Node& entry : tabs) {
    const uint32_t id = entry["id"].as<uint32_t>(0xFFFFFFFFu);
    if (id > 0xFF) continue;  // id de storage : un uint8 côté serveur
    StorageWindow::TabCustom custom;
    const std::string name = entry["name"].as<std::string>("");
    std::snprintf(custom.name, sizeof(custom.name), "%s", name.c_str());
    custom.icon_id = entry["icon"].as<uint32_t>(0);
    // Entrée sans effet (ni nom ni icône) : ne pas la recharger, sinon le yaml
    // accumule des lignes vides au fil des popups simplement ouverts.
    if (!custom.name[0] && custom.icon_id == 0) continue;
    storage->tab_custom_[id] = custom;
  }
}

void WriteStorageTabCustom(YAML::Emitter& out) {
  auto* storage = Bourgeon::Instance().storage_window();
  out << YAML::Key << "storage_tab_custom" << YAML::Value << YAML::BeginSeq;
  if (storage) {
    // Triés par id : yaml stable d'une sauvegarde à l'autre (unordered_map).
    std::vector<uint32_t> ids;
    ids.reserve(storage->tab_custom_.size());
    for (const auto& entry : storage->tab_custom_) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());
    for (const uint32_t id : ids) {
      const StorageWindow::TabCustom& custom = storage->tab_custom_[id];
      if (!custom.name[0] && custom.icon_id == 0) continue;  // rien à retenir
      out << YAML::Flow << YAML::BeginMap;
      out << YAML::Key << "id" << YAML::Value << id;
      out << YAML::Key << "name" << YAML::Value << custom.name;
      out << YAML::Key << "icon" << YAML::Value << custom.icon_id;
      out << YAML::EndMap;
    }
  }
  out << YAML::EndSeq;
}

void ReadSkinAndPresets(const YAML::Node& ui) {
  // 🔴 « malgun_font » et « ui_font_family » NE SONT PLUS ICI, et ne doivent pas
  // y revenir. Ce fichier n'est relu qu'à l'entrée en jeu (LoadSettings, sur
  // `in_game_ && !was_in_game`) : la police arrivait donc après l'écran de login
  // et le char-select, et cette relecture ÉCRASAIT le choix que le joueur venait
  // de faire au combo du login. Elle vit maintenant à la racine de
  // paths::StartupSettingsPath(), lue au chargement de la DLL par
  // startup::UiFontFamily() — qui reprend les deux anciennes clés d'ici tant que
  // la neuve n'a jamais été posée — et écrite par startup::SaveUiFontFamily().
  // (Même déménagement que « language », cf. moonlight_ui.cc.)
  //
  // (« ro_skin » : clé abandonnée — le skin RO est désormais toujours actif. Une
  // ancienne valeur false dans le yaml est simplement ignorée.)
  ReadSkinCfg(ui, ro::SkinConfig(), "ro_skin_", /*with_rounding=*/true);

  auto& presets = ro::SkinPresets();
  presets.clear();
  if (const YAML::Node saved = ui["ro_skin_presets"]) {
    for (auto it = saved.begin(); it != saved.end(); ++it) {
      ro::SkinPreset preset;
      preset.name = (*it)["name"].as<std::string>("");
      if (preset.name.empty()) continue;
      ReadSkinCfg(*it, preset.cfg, "", /*with_rounding=*/false);
      presets.push_back(std::move(preset));
    }
  }
  // Thèmes de départ si le yaml n'en portait aucun (1er lancement) : le toolkit
  // les fournit, il n'y a rien de spécifique à moonlight_ui dedans.
  ro::EnsureDefaultSkinPresets();
}

void WriteSkinAndPresets(YAML::Emitter& out) {
  // (La police de l'interface n'est plus écrite ici — cf. ReadSkinAndPresets.
  // Les anciennes clés restent dans les yaml existants : elles servent encore de
  // repli à startup::UiFontFamily() au premier lancement après la mise à jour.)
  EmitSkinCfg(out, ro::SkinConfig(), "ro_skin_", /*with_rounding=*/true);
  out << YAML::Key << "ro_skin_presets" << YAML::Value << YAML::BeginSeq;
  for (const ro::SkinPreset& preset : ro::SkinPresets()) {
    out << YAML::BeginMap << YAML::Key << "name" << YAML::Value << preset.name;
    EmitSkinCfg(out, preset.cfg, "", /*with_rounding=*/false);
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;
}

void ReadEquipPresets(const YAML::Node& ui) {
  auto* character_sheet = Bourgeon::Instance().character_sheet();
  if (!character_sheet) return;
  auto& presets = character_sheet->equip_presets();
  presets.clear();
  const YAML::Node saved = ui["equip_presets"];
  if (!saved) return;
  for (const YAML::Node& node : saved) {
    EquipPreset preset;
    preset.cid  = node["cid"].as<uint32_t>(0);
    preset.name = node["name"].as<std::string>("");
    if (preset.name.empty()) continue;
    preset.hotkey_vk    = node["hkvk"].as<int>(0);
    preset.hotkey_ctrl  = node["hkc"].as<bool>(false);
    preset.hotkey_alt   = node["hka"].as<bool>(false);
    preset.hotkey_shift = node["hks"].as<bool>(false);
    if (const YAML::Node items = node["items"]) {
      for (const YAML::Node& item : items) {
        EquipPresetItem entry;
        entry.nameid   = item["id"].as<uint32_t>(0);
        entry.refine   = item["refine"].as<int>(0);
        entry.grade    = item["grade"].as<int>(0);
        entry.left_hand = item["left"].as<bool>(false);
        if (const YAML::Node cards = item["cards"])
          for (int slot = 0; slot < 4 && slot < static_cast<int>(cards.size()); ++slot)
            entry.cards[slot] = cards[slot].as<uint32_t>(0);
        preset.items.push_back(entry);
      }
    }
    presets.push_back(std::move(preset));
  }
}

void WriteEquipPresets(YAML::Emitter& out) {
  out << YAML::Key << "equip_presets" << YAML::Value << YAML::BeginSeq;
  if (auto* character_sheet = Bourgeon::Instance().character_sheet()) {
    for (const EquipPreset& preset : character_sheet->equip_presets()) {
      out << YAML::BeginMap
          << YAML::Key << "cid"  << YAML::Value << preset.cid
          << YAML::Key << "name" << YAML::Value << preset.name
          << YAML::Key << "hkvk" << YAML::Value << preset.hotkey_vk
          << YAML::Key << "hkc"  << YAML::Value << preset.hotkey_ctrl
          << YAML::Key << "hka"  << YAML::Value << preset.hotkey_alt
          << YAML::Key << "hks"  << YAML::Value << preset.hotkey_shift
          << YAML::Key << "items" << YAML::Value << YAML::BeginSeq;
      for (const EquipPresetItem& entry : preset.items) {
        out << YAML::BeginMap
            << YAML::Key << "id"     << YAML::Value << entry.nameid
            << YAML::Key << "refine" << YAML::Value << entry.refine
            << YAML::Key << "grade"  << YAML::Value << entry.grade
            << YAML::Key << "left"   << YAML::Value << entry.left_hand
            << YAML::Key << "cards"  << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (int slot = 0; slot < 4; ++slot) out << entry.cards[slot];
        out << YAML::EndSeq << YAML::EndMap;
      }
      out << YAML::EndSeq << YAML::EndMap;
    }
  }
  out << YAML::EndSeq;
}

void ReadChatBackgrounds(const YAML::Node& ui) {
  auto* chat_tweaks = Bourgeon::Instance().chat_tweaks();
  if (!chat_tweaks) return;
  for (int group = 0; group < ChatTweaks::kBgCount; ++group) {
    float* picker = chat_tweaks->bg_color(group);
    if (!picker) continue;
    // Clé absente ou corrompue : le picker garde la couleur lue dans le binaire
    // au démarrage (cf. FindBackgroundSites), qui est le vrai défaut du client.
    ReadArgbKey(ui, chat_tweaks->bg_yaml_key(group), picker);
  }
}

void WriteChatBackgrounds(YAML::Emitter& out) {
  auto* chat_tweaks = Bourgeon::Instance().chat_tweaks();
  if (!chat_tweaks) return;
  for (int group = 0; group < ChatTweaks::kBgCount; ++group) {
    const float* picker = chat_tweaks->bg_color(group);
    if (picker) WriteArgbKey(out, chat_tweaks->bg_yaml_key(group), picker);
  }
}

void ReadChatBgPresets(const YAML::Node& ui) {
  auto* chat_tweaks = Bourgeon::Instance().chat_tweaks();
  if (!chat_tweaks) return;
  auto& presets = chat_tweaks->bg_presets();
  presets.clear();
  const YAML::Node saved = ui["chat_bg_presets"];
  if (!saved) return;
  for (const YAML::Node& node : saved) {
    const std::string name = node["name"].as<std::string>("");
    uint32_t argb = 0;
    if (name.empty() || !ro::ParseHex8(node["color"].as<std::string>(""), &argb))
      continue;
    presets.push_back({name, argb});
  }
}

void WriteChatBgPresets(YAML::Emitter& out) {
  out << YAML::Key << "chat_bg_presets" << YAML::Value << YAML::BeginSeq;
  if (auto* chat_tweaks = Bourgeon::Instance().chat_tweaks()) {
    for (const ChatTweaks::BgPreset& preset : chat_tweaks->bg_presets()) {
      // Le préréglage porte déjà l'ARGB natif : pas de picker à convertir, on
      // formate.
      char hex[9];
      std::snprintf(hex, sizeof(hex), "%08X", preset.argb);
      out << YAML::BeginMap
          << YAML::Key << "name"  << YAML::Value << preset.name
          << YAML::Key << "color" << YAML::Value << hex
          << YAML::EndMap;
    }
  }
  out << YAML::EndSeq;
}

void ReadBarLayout(const YAML::Node& ui) {
  auto* basic_info = Bourgeon::Instance().basic_info();
  if (!basic_info) return;
  for (int i = 0; i < BasicInfo::kBarCount; ++i) {
    const std::string prefix =
        std::string("expbar_") + BasicInfo::kBarKeys[i] + "_";
    auto& bar = basic_info->bars_[i];
    bar.show = ui[prefix + "show"].as<bool>(true);
    bar.x = ui[prefix + "x"].as<int>(bar.x);
    bar.y = ui[prefix + "y"].as<int>(bar.y);
    bar.w = ui[prefix + "w"].as<int>(bar.w);
    bar.h = ui[prefix + "h"].as<int>(bar.h);
    ReadArgbKey(ui, prefix + "color", bar.fill);
  }
}

void WriteBarLayout(YAML::Emitter& out) {
  auto* basic_info = Bourgeon::Instance().basic_info();
  if (!basic_info) return;
  for (int i = 0; i < BasicInfo::kBarCount; ++i) {
    const std::string prefix =
        std::string("expbar_") + BasicInfo::kBarKeys[i] + "_";
    const auto& bar = basic_info->bars_[i];
    out << YAML::Key << (prefix + "show") << YAML::Value << bar.show
        << YAML::Key << (prefix + "x")    << YAML::Value << bar.x
        << YAML::Key << (prefix + "y")    << YAML::Value << bar.y
        << YAML::Key << (prefix + "w")    << YAML::Value << bar.w
        << YAML::Key << (prefix + "h")    << YAML::Value << bar.h;
    WriteArgbKey(out, prefix + "color", bar.fill);
  }
}

void ReadPortraitLayout(const YAML::Node& ui) {
  auto* basic_info = Bourgeon::Instance().basic_info();
  if (!basic_info) return;
  // Effets de chapeau (.str) : rendu automatique et toujours actif, aucun
  // réglage persisté.
  for (int i = 0; i < BasicInfo::kPortCount; ++i) {
    const std::string prefix =
        std::string("portrait_") + BasicInfo::kPortKeys[i] + "_";
    auto& element = basic_info->ports_[i];
    element.show     = ui[prefix + "show"].as<bool>(element.show);
    element.x        = ui[prefix + "x"].as<int>(element.x);
    element.y        = ui[prefix + "y"].as<int>(element.y);
    element.w        = ui[prefix + "w"].as<int>(element.w);
    element.h        = ui[prefix + "h"].as<int>(element.h);
    element.rounding = ui[prefix + "rounding"].as<float>(element.rounding);
    element.text_scale = ui[prefix + "tscale"].as<float>(element.text_scale);
    ReadArgbKey(ui, prefix + "bg", element.bg);
    ReadArgbKey(ui, prefix + "fg", element.fg);
  }
}

void WritePortraitLayout(YAML::Emitter& out) {
  auto* basic_info = Bourgeon::Instance().basic_info();
  if (!basic_info) return;
  for (int i = 0; i < BasicInfo::kPortCount; ++i) {
    const std::string prefix =
        std::string("portrait_") + BasicInfo::kPortKeys[i] + "_";
    const auto& element = basic_info->ports_[i];
    out << YAML::Key << (prefix + "show")     << YAML::Value << element.show
        << YAML::Key << (prefix + "x")        << YAML::Value << element.x
        << YAML::Key << (prefix + "y")        << YAML::Value << element.y
        << YAML::Key << (prefix + "w")        << YAML::Value << element.w
        << YAML::Key << (prefix + "h")        << YAML::Value << element.h
        << YAML::Key << (prefix + "rounding") << YAML::Value << element.rounding
        << YAML::Key << (prefix + "tscale")   << YAML::Value << element.text_scale;
    WriteArgbKey(out, prefix + "bg", element.bg);
    WriteArgbKey(out, prefix + "fg", element.fg);
  }
}

void ReadSkillBarLayout(const YAML::Node& ui) {
  auto* skill_bar = Bourgeon::Instance().skill_bar();
  if (!skill_bar) return;
  // 3 barres fixes (0 = Onglet 1, 1 = Onglet 2, 2 = Items).
  for (int index = 0; index < SkillBar::kBarCount; ++index) {
    auto& bar = skill_bar->bars_[index];
    const std::string prefix = "skillbar" + std::to_string(index) + "_";
    bar.visible    = ui[prefix + "visible"].as<bool>(bar.visible);
    bar.x          = ui[prefix + "x"].as<int>(bar.x);
    bar.y          = ui[prefix + "y"].as<int>(bar.y);
    bar.columns    = ui[prefix + "columns"].as<int>(bar.columns);
    bar.first_slot = ui[prefix + "first"].as<int>(bar.first_slot);
    bar.slot_count = ui[prefix + "slots"].as<int>(bar.slot_count);
    bar.icon_size  = ui[prefix + "size"].as<float>(bar.icon_size);
    bar.spacing    = ui[prefix + "spacing"].as<float>(bar.spacing);
  }
  // Contenu persisté de la barre d'items (nameids).
  for (int slot = 0; slot < SkillBar::kItemSlotMax; ++slot)
    skill_bar->item_slots_[slot] =
        ui["skillbar_item" + std::to_string(slot)].as<uint32_t>(
            skill_bar->item_slots_[slot]);
}

void WriteSkillBarLayout(YAML::Emitter& out) {
  auto* skill_bar = Bourgeon::Instance().skill_bar();
  if (!skill_bar) return;
  for (int index = 0; index < SkillBar::kBarCount; ++index) {
    const auto& bar = skill_bar->bars_[index];
    const std::string prefix = "skillbar" + std::to_string(index) + "_";
    out << YAML::Key << (prefix + "visible") << YAML::Value << bar.visible
        << YAML::Key << (prefix + "x")       << YAML::Value << bar.x
        << YAML::Key << (prefix + "y")       << YAML::Value << bar.y
        << YAML::Key << (prefix + "columns") << YAML::Value << bar.columns
        << YAML::Key << (prefix + "first")   << YAML::Value << bar.first_slot
        << YAML::Key << (prefix + "slots")   << YAML::Value << bar.slot_count
        << YAML::Key << (prefix + "size")    << YAML::Value << bar.icon_size
        << YAML::Key << (prefix + "spacing") << YAML::Value << bar.spacing;
  }
  // Capture le contenu VIVANT de la barre d'items avant de l'écrire : c'est le
  // jeu qui le modifie (drag&drop), pas nous.
  skill_bar->SnapshotItemSlots();
  for (int slot = 0; slot < SkillBar::kItemSlotMax; ++slot)
    out << YAML::Key << ("skillbar_item" + std::to_string(slot))
        << YAML::Value << skill_bar->item_slots_[slot];
}

void ReadWindowPositions(const YAML::Node& ui) {
  // Fenêtre STATUS et fenêtre ÉQUIPEMENT : appliquées par le hook de leur
  // gestionnaire de messages respectif.
  StatusTweaks_SetSavedPos(ui["status_pos_x"].as<int>(INT_MIN),
                           ui["status_pos_y"].as<int>(INT_MIN));
  EquipTweaks_SetSavedPos(ui["equip_pos_x"].as<int>(INT_MIN),
                          ui["equip_pos_y"].as<int>(INT_MIN));
  // Table générique : une paire « <clé>_pos_x/y » par fenêtre, appliquée au tick
  // suivant. Ajouter une fenêtre ne demande aucune édition ici.
  for (int i = 0; i < WindowPosTweaks_Count(); ++i) {
    const std::string key = WindowPosTweaks_Key(i);
    WindowPosTweaks_SetSavedPos(i, ui[key + "_pos_x"].as<int>(INT_MIN),
                                ui[key + "_pos_y"].as<int>(INT_MIN));
  }
}

void WriteWindowPositions(YAML::Emitter& out) {
  out << YAML::Key << "status_pos_x" << YAML::Value << StatusTweaks_SavedX()
      << YAML::Key << "status_pos_y" << YAML::Value << StatusTweaks_SavedY()
      << YAML::Key << "equip_pos_x"  << YAML::Value << EquipTweaks_SavedX()
      << YAML::Key << "equip_pos_y"  << YAML::Value << EquipTweaks_SavedY();
  for (int i = 0; i < WindowPosTweaks_Count(); ++i) {
    const std::string key = WindowPosTweaks_Key(i);
    out << YAML::Key << (key + "_pos_x") << YAML::Value << WindowPosTweaks_X(i)
        << YAML::Key << (key + "_pos_y") << YAML::Value << WindowPosTweaks_Y(i);
  }
}

void MigrateLegacyKeys(YAML::Node ui) {
  // La grille d'alignement était propre aux barres EXP/HP/SP avant de devenir
  // globale : expbar_grid_* -> grid_*.
  static const struct RenamedKey {
    const char* current;
    const char* legacy;
  } kRenamedKeys[] = {
      {"grid_show",  "expbar_grid_show"},
      {"grid_snap",  "expbar_grid_snap"},
      {"grid_size",  "expbar_grid_size"},
      {"grid_color", "expbar_grid_color"},
  };
  for (const RenamedKey& renamed : kRenamedKeys)
    if (!ui[renamed.current] && ui[renamed.legacy])
      ui[renamed.current] = ui[renamed.legacy];

  // Une seule échelle de texte pour les barres de raccourcis, devenue deux
  // (touches et nombres réglables séparément).
  if (const YAML::Node legacy_scale = ui["skillbar_text_scale"]) {
    if (!ui["skillbar_key_scale"])   ui["skillbar_key_scale"]   = legacy_scale;
    if (!ui["skillbar_count_scale"]) ui["skillbar_count_scale"] = legacy_scale;
  }
}

}  // namespace moonlight_ui
