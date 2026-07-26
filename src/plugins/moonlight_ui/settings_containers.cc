#include "plugins/moonlight_ui/settings_containers.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bourgeon.h"
#include "plugins/character_sheet.h"
#include "plugins/inventory_viewer.h"
#include "plugins/menu_icons.h"
#include "plugins/storage_tweaks.h"
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
    MenuIconTweaks::IconSave saved;
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

void ReadStorageFavorites(const YAML::Node& ui) {
  auto* storage = Bourgeon::Instance().storage_tweaks();
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
  auto* storage = Bourgeon::Instance().storage_tweaks();
  // Triés pour un yaml stable, comme le placement libre de l'inventaire.
  out << YAML::Key << "storage_favorites" << YAML::Value << YAML::Flow << YAML::BeginSeq;
  if (storage) {
    std::vector<uint32_t> sorted(storage->favorites_.begin(), storage->favorites_.end());
    std::sort(sorted.begin(), sorted.end());
    for (const uint32_t nameid : sorted) out << nameid;
  }
  out << YAML::EndSeq;
}

void ReadSkinAndPresets(const YAML::Node& ui) {
  ro::SetFontEnabled(ui["malgun_font"].as<bool>(ro::IsFontEnabled()));
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
  out << YAML::Key << "malgun_font" << YAML::Value << ro::IsFontEnabled();
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
    preset.hotkeyVk = node["hkvk"].as<int>(0);
    preset.hkCtrl   = node["hkc"].as<bool>(false);
    preset.hkAlt    = node["hka"].as<bool>(false);
    preset.hkShift  = node["hks"].as<bool>(false);
    if (const YAML::Node items = node["items"]) {
      for (const YAML::Node& item : items) {
        EquipPresetItem entry;
        entry.nameid   = item["id"].as<uint32_t>(0);
        entry.refine   = item["refine"].as<int>(0);
        entry.grade    = item["grade"].as<int>(0);
        entry.leftHand = item["left"].as<bool>(false);
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
          << YAML::Key << "hkvk" << YAML::Value << preset.hotkeyVk
          << YAML::Key << "hkc"  << YAML::Value << preset.hkCtrl
          << YAML::Key << "hka"  << YAML::Value << preset.hkAlt
          << YAML::Key << "hks"  << YAML::Value << preset.hkShift
          << YAML::Key << "items" << YAML::Value << YAML::BeginSeq;
      for (const EquipPresetItem& entry : preset.items) {
        out << YAML::BeginMap
            << YAML::Key << "id"     << YAML::Value << entry.nameid
            << YAML::Key << "refine" << YAML::Value << entry.refine
            << YAML::Key << "grade"  << YAML::Value << entry.grade
            << YAML::Key << "left"   << YAML::Value << entry.leftHand
            << YAML::Key << "cards"  << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (int slot = 0; slot < 4; ++slot) out << entry.cards[slot];
        out << YAML::EndSeq << YAML::EndMap;
      }
      out << YAML::EndSeq << YAML::EndMap;
    }
  }
  out << YAML::EndSeq;
}

}  // namespace moonlight_ui
