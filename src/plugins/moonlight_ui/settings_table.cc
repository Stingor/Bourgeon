#include "plugins/moonlight_ui/settings_table.h"

#include <cstdint>
#include <cstdio>

#include "ui/color_codec.h"

namespace moonlight_ui {
namespace {

// Copie `count` flottants : un picker fait toujours 4 composantes RGBA.
constexpr int kPickerComponents = 4;

}  // namespace

bool ReadArgbKey(const YAML::Node& ui, const std::string& key, float picker_rgba[4]) {
  uint32_t argb = 0;
  if (!ro::ParseHex8(ui[key].as<std::string>(""), &argb)) return false;
  ro::PickerFromArgb(picker_rgba, argb);
  return true;
}

std::string HexArgb(const float picker_rgba[4]) {
  char hex[16];
  std::snprintf(hex, sizeof(hex), "%08X", ro::ArgbFromPicker(picker_rgba));
  return hex;
}

void WriteArgbKey(YAML::Emitter& out, const std::string& key, const float picker_rgba[4]) {
  out << YAML::Key << key << YAML::Value << HexArgb(picker_rgba);
}

void ReadSettings(const YAML::Node& ui, const SettingDesc* table, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const SettingDesc& desc = table[i];
    void* live = desc.live();
    if (!live) continue;  // plugin absent : rien à remplir
    const void* def = desc.def();
    const YAML::Node node = ui[desc.key];

    switch (desc.type) {
      case SettingType::kBool:
        *static_cast<bool*>(live) = node.as<bool>(*static_cast<const bool*>(def));
        break;
      case SettingType::kInt:
        *static_cast<int*>(live) = node.as<int>(*static_cast<const int*>(def));
        break;
      case SettingType::kUInt:
        *static_cast<uint32_t*>(live) =
            node.as<uint32_t>(*static_cast<const uint32_t*>(def));
        break;
      case SettingType::kFloat:
        *static_cast<float*>(live) = node.as<float>(*static_cast<const float*>(def));
        break;
      case SettingType::kString:
        *static_cast<std::string*>(live) =
            node.as<std::string>(*static_cast<const std::string*>(def));
        break;
      case SettingType::kColorHex: {
        float* picker = static_cast<float*>(live);
        if (!ReadArgbKey(ui, desc.key, picker)) {
          const float* fallback = static_cast<const float*>(def);
          for (int c = 0; c < kPickerComponents; ++c) picker[c] = fallback[c];
        }
        break;
      }
      case SettingType::kColorU32: {
        float* picker = static_cast<float*>(live);
        if (node) {
          ro::PickerFromImU32(node.as<unsigned>(0), picker);
        } else {
          const float* fallback = static_cast<const float*>(def);
          for (int c = 0; c < kPickerComponents; ++c) picker[c] = fallback[c];
        }
        break;
      }
    }
  }
}

void WriteSettings(YAML::Emitter& out, const SettingDesc* table, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const SettingDesc& desc = table[i];
    // Plugin absent : on émet quand même, depuis l'instance par défaut. Une clé
    // qui disparaît du fichier est un réglage perdu au prochain chargement.
    const void* value = desc.live();
    if (!value) value = desc.def();

    switch (desc.type) {
      case SettingType::kBool:
        out << YAML::Key << desc.key << YAML::Value << *static_cast<const bool*>(value);
        break;
      case SettingType::kInt:
        out << YAML::Key << desc.key << YAML::Value << *static_cast<const int*>(value);
        break;
      case SettingType::kUInt:
        out << YAML::Key << desc.key << YAML::Value
            << *static_cast<const uint32_t*>(value);
        break;
      case SettingType::kFloat:
        out << YAML::Key << desc.key << YAML::Value << *static_cast<const float*>(value);
        break;
      case SettingType::kString:
        out << YAML::Key << desc.key << YAML::Value
            << *static_cast<const std::string*>(value);
        break;
      case SettingType::kColorHex:
        WriteArgbKey(out, desc.key, static_cast<const float*>(value));
        break;
      case SettingType::kColorU32:
        out << YAML::Key << desc.key << YAML::Value
            << ro::ImU32FromPicker(static_cast<const float*>(value));
        break;
    }
  }
}

}  // namespace moonlight_ui
