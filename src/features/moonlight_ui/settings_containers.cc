#include "features/moonlight_ui/settings_containers.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <cstdio>

#include "bourgeon.h"
#include "features/windows/mvp_tracker_window.h"
#include "features/overlays/basic_info.h"
#include "features/overlays/target_frame.h"
#include "features/patches/chat.h"
#include "features/windows/character_sheet.h"
#include "features/windows/entity_context_menu.h"
#include "features/hotkey_actions.h"
#include "features/patches/equip_tweaks.h"
#include "features/windows/inventory_viewer.h"
#include "features/overlays/menu_icons.h"
#include "features/overlays/minimap.h"
#include "features/moonlight_ui/settings_table.h"  // ReadArgbKey / WriteArgbKey
#include "features/overlays/skill_bar.h"
#include "features/patches/status_tweaks.h"
#include "features/windows/storage_window.h"
#include "features/patches/window_pos_tweaks.h"
#include "ui/color_codec.h"
#include "ui/hud_frame.h"  // ro::HudRect — nommé ici, donc inclus ici
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

// ── Le bloc `show` + rectangle, une fois ─────────────────────────────────────
// Les TROIS familles d'éléments de HUD — barres d'info, portrait, cadre de
// cible — persistent le même quintuplet sous leur propre préfixe, et toutes
// trois portent un `ro::HudRect`.
//
// 🔴 Le relevé de doublons n'en appariait que DEUX : la famille « portrait »
// porte deux champs de plus (arrondi, échelle de texte), ce qui la faisait
// passer sous le seuil de similarité. La copie qu'un relevé ne montre pas est la
// plus tenace — c'est celle qu'on ne corrige jamais.
//
// ⚠ ET UNE DIVERGENCE RÉELLE EST CORRIGÉE AU PASSAGE. `ReadBarLayout` prenait
// `true` EN DUR comme défaut de `show`, là où les deux autres reprennent la
// valeur COURANTE. Sans effet visible aujourd'hui — les sept barres sont livrées
// visibles — mais une huitième barre livrée MASQUÉE serait revenue visible au
// premier chargement de réglages, et rien ne l'aurait signalé.
void ReadRectBlock(const YAML::Node& ui, const std::string& prefix, bool* show,
                   ro::HudRect* rect) {
  *show   = ui[prefix + "show"].as<bool>(*show);
  rect->x = ui[prefix + "x"].as<int>(rect->x);
  rect->y = ui[prefix + "y"].as<int>(rect->y);
  rect->w = ui[prefix + "w"].as<int>(rect->w);
  rect->h = ui[prefix + "h"].as<int>(rect->h);
}

// ⚠ L'ORDRE DES CLÉS est celui des trois écritures d'origine : le premier yaml
// réécrit après ce changement ne doit pas différer de l'ancien.
void WriteRectBlock(YAML::Emitter& out, const std::string& prefix, bool show,
                    const ro::HudRect& rect) {
  out << YAML::Key << (prefix + "show") << YAML::Value << show
      << YAML::Key << (prefix + "x")    << YAML::Value << rect.x
      << YAML::Key << (prefix + "y")    << YAML::Value << rect.y
      << YAML::Key << (prefix + "w")    << YAML::Value << rect.w
      << YAML::Key << (prefix + "h")    << YAML::Value << rect.h;
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

void ReadMinimapMemos(const YAML::Node& ui) {
  auto* minimap = Bourgeon::Instance().minimap();
  if (!minimap) return;
  const YAML::Node maps = ui["minimap_memos"];
  if (!maps) return;
  minimap->memos_.clear();
  for (auto it = maps.begin(); it != maps.end(); ++it) {
    const std::string map_name = it->first.as<std::string>("");
    if (map_name.empty() || !it->second.IsSequence()) continue;
    std::vector<MinimapMemo> list;
    for (const YAML::Node& entry : it->second) {
      MinimapMemo memo;
      memo.x = entry["x"].as<int>(-1);
      memo.y = entry["y"].as<int>(-1);
      memo.name = entry["name"].as<std::string>("");
      // Une cellule négative ne désigne aucune case de carte : c'est une entrée
      // tronquée ou éditée à la main, on la laisse tomber plutôt que de poser un
      // marqueur invisible que le joueur ne pourra jamais retirer.
      if (memo.x < 0 || memo.y < 0) continue;
      list.push_back(std::move(memo));
    }
    if (!list.empty()) minimap->memos_[map_name] = std::move(list);
  }
}

void WriteMinimapMemos(YAML::Emitter& out) {
  auto* minimap = Bourgeon::Instance().minimap();
  out << YAML::Key << "minimap_memos" << YAML::Value << YAML::BeginMap;
  if (minimap) {
    for (const auto& entry : minimap->memos_) {
      if (entry.second.empty()) continue;
      out << YAML::Key << entry.first << YAML::Value << YAML::BeginSeq;
      for (const MinimapMemo& memo : entry.second) {
        out << YAML::BeginMap
            << YAML::Key << "x"    << YAML::Value << memo.x
            << YAML::Key << "y"    << YAML::Value << memo.y
            << YAML::Key << "name" << YAML::Value << memo.name
            << YAML::EndMap;
      }
      out << YAML::EndSeq;
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

void ReadBourgeonHotkeys(const YAML::Node& ui) {
  // Repart de ce que propose le CATALOGUE, pas d'une table vierge : le fichier ne
  // porte que les écarts. Une action sans défaut retrouve « aucune touche », celle
  // qui en a un (le rapport de bug, sur Ctrl+Alt+B) retrouve le sien — y compris
  // dans un yaml écrit avant que l'action existe, ce qui est exactement le cas de
  // tous les fichiers déjà en circulation.
  hotkeys::ResetBindingsToDefaults();
  const YAML::Node bindings = ui["bourgeon_hotkeys"];
  if (!bindings) return;
  for (const YAML::Node& entry : bindings) {
    const int index = hotkeys::IndexOf(entry["id"].as<std::string>("").c_str());
    if (index < 0) continue;  // action retirée depuis : l'entrée est jetée
    hotkeys::Binding binding;
    binding.vk    = entry["vk"].as<int>(0);
    binding.ctrl  = entry["ctrl"].as<bool>(false);
    binding.alt   = entry["alt"].as<bool>(false);
    binding.shift = entry["shift"].as<bool>(false);
    // 🔴 `vk == 0` EST UNE VALEUR, plus une entrée à ignorer. C'est le seul moyen
    // d'écrire « le joueur a effacé la touche par défaut » : la sauter ferait
    // repasser le défaut au redémarrage, et l'effacement ne tiendrait pas.
    hotkeys::SetBinding(index, binding);
  }
}

void WriteBourgeonHotkeys(YAML::Emitter& out) {
  // L'ordre du catalogue fait un fichier stable d'une sauvegarde à l'autre, sans
  // tri : il ne dépend pas de l'ordre dans lequel le joueur a réglé ses touches.
  out << YAML::Key << "bourgeon_hotkeys" << YAML::Value << YAML::BeginSeq;
  for (int i = 0; i < hotkeys::ActionCount(); ++i) {
    const hotkeys::Binding& binding = hotkeys::BindingAt(i);
    // Rien à dire d'une action sans touche ET sans défaut : c'est déjà ce que la
    // relecture reconstruit. Mais une action à DÉFAUT qu'on a laissée vide doit
    // s'écrire, sinon son absence se relit comme « remets le défaut ».
    if (binding.vk == 0 && hotkeys::ActionAt(i).default_binding.vk == 0) continue;
    out << YAML::Flow << YAML::BeginMap;
    out << YAML::Key << "id"    << YAML::Value << hotkeys::ActionAt(i).id;
    out << YAML::Key << "vk"    << YAML::Value << binding.vk;
    out << YAML::Key << "ctrl"  << YAML::Value << binding.ctrl;
    out << YAML::Key << "alt"   << YAML::Value << binding.alt;
    out << YAML::Key << "shift" << YAML::Value << binding.shift;
    out << YAML::EndMap;
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
    // Absent des presets d'avant les costumes : faux, c'est-a-dire « ne touche pas a
    // l'apparence ». C'est le seul defaut qui laisse ces presets se comporter comme ils
    // l'ont toujours fait — ils n'ont d'ailleurs aucun item de famille costume.
    preset.with_costumes = node["costumes"].as<bool>(false);
    if (const YAML::Node items = node["items"]) {
      for (const YAML::Node& item : items) {
        EquipPresetItem entry;
        entry.nameid   = item["id"].as<uint32_t>(0);
        entry.refine   = item["refine"].as<int>(0);
        entry.grade    = item["grade"].as<int>(0);
        entry.left_hand = item["left"].as<bool>(false);
        // Famille : 0 equipement, 1 costume, 2 munition. Clef absente -> equipement,
        // ce que sont tous les items des presets deja sur disque.
        const int kind = item["kind"].as<int>(0);
        entry.kind = (kind == 1)   ? PresetKind::kCostume
                     : (kind == 2) ? PresetKind::kAmmo
                                   : PresetKind::kEquip;
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
          << YAML::Key << "costumes" << YAML::Value << preset.with_costumes
          << YAML::Key << "items" << YAML::Value << YAML::BeginSeq;
      for (const EquipPresetItem& entry : preset.items) {
        out << YAML::BeginMap
            << YAML::Key << "id"     << YAML::Value << entry.nameid
            << YAML::Key << "refine" << YAML::Value << entry.refine
            << YAML::Key << "grade"  << YAML::Value << entry.grade
            << YAML::Key << "left"   << YAML::Value << entry.left_hand
            << YAML::Key << "kind"   << YAML::Value << static_cast<int>(entry.kind)
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
    ReadRectBlock(ui, prefix, &bar.show, &bar.rect);
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
    WriteRectBlock(out, prefix, bar.show, bar.rect);
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
    ReadRectBlock(ui, prefix, &element.show, &element.rect);
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
    WriteRectBlock(out, prefix, element.show, element.rect);
    out << YAML::Key << (prefix + "rounding") << YAML::Value << element.rounding
        << YAML::Key << (prefix + "tscale")   << YAML::Value << element.text_scale;
    WriteArgbKey(out, prefix + "bg", element.bg);
    WriteArgbKey(out, prefix + "fg", element.fg);
  }
}

void ReadTargetLayout(const YAML::Node& ui) {
  auto* target_frame = Bourgeon::Instance().target_frame();
  if (!target_frame) return;
  for (int i = 0; i < TargetFrame::kElemCount; ++i) {
    const std::string prefix =
        std::string("target_") + TargetFrame::kElemKeys[i] + "_";
    auto& elem = target_frame->elems_[i];
    ReadRectBlock(ui, prefix, &elem.show, &elem.rect);
    ReadArgbKey(ui, prefix + "bg", elem.bg);
    ReadArgbKey(ui, prefix + "fg", elem.fg);
  }
}

void WriteTargetLayout(YAML::Emitter& out) {
  auto* target_frame = Bourgeon::Instance().target_frame();
  if (!target_frame) return;
  for (int i = 0; i < TargetFrame::kElemCount; ++i) {
    const std::string prefix =
        std::string("target_") + TargetFrame::kElemKeys[i] + "_";
    const auto& elem = target_frame->elems_[i];
    WriteRectBlock(out, prefix, elem.show, elem.rect);
    WriteArgbKey(out, prefix + "bg", elem.bg);
    WriteArgbKey(out, prefix + "fg", elem.fg);
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
  // Contenu de la barre d'items : il a QUITTÉ ce fichier pour
  // `SaveData\bourgeon_itembar.yaml`, où il est rangé par personnage (le yaml
  // partagé est unique pour l'installation, donc tous les personnages de tous
  // les comptes y partageaient une seule barre). Ne restent ici que les clés
  // `skillbar_item*` des versions précédentes, qu'on transmet une fois à
  // SkillBar : elles deviennent la barre HÉRITÉE dont part chaque personnage.
  uint32_t legacy[SkillBar::kItemSlotMax] = {};
  for (int slot = 0; slot < SkillBar::kItemSlotMax; ++slot)
    legacy[slot] = ui["skillbar_item" + std::to_string(slot)].as<uint32_t>(0u);
  skill_bar->AdoptLegacyItemSlots(legacy, SkillBar::kItemSlotMax);
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
  // (Plus de `skillbar_item*` ici : le contenu de la barre d'items vit
  // maintenant dans `SaveData\bourgeon_itembar.yaml`, par personnage, et c'est
  // SkillBar qui l'écrit lui-même dès qu'une case bouge. Les anciennes clés
  // disparaissent donc du fichier à cette écriture — elles ont déjà été reprises
  // à la lecture, cf. ReadSkillBarLayout.)
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


// ── Les lignes détachées du carnet de chasse MVP ─────────────────────────────
//
// 🔴 La clé écrite est `(mob_id, map)`, JAMAIS le `slot_id`. Celui-ci vaut le
// rang dans un registre que le map-server reconstruit à chaque démarrage :
// ajoutez un `boss_monster` et toutes les lignes déjà posées désigneraient le
// voisin. Même clé, et même raison, que les favoris en base.
//
// `mob_id` vaut 0 pour un créneau scripté (Bio Lab, Lord of Death, Thanatos) :
// là-bas le mob change à chaque cycle et c'est la carte qui identifie.
void ReadMvpTrackerLines(const YAML::Node& ui) {
  auto* tracker = Bourgeon::Instance().mvp_tracker_window();
  if (!tracker) return;
  const YAML::Node seq = ui["mvptracker_lines"];
  if (!seq || !seq.IsSequence()) return;
  tracker->lines_.clear();
  for (const YAML::Node& entry : seq) {
    MvpTrackerWindow::PinnedLine line;
    line.mob_id = static_cast<uint16_t>(entry["mob"].as<int>(0));
    const std::string map = entry["map"].as<std::string>("");
    line.x = entry["x"].as<int>(-1);
    line.y = entry["y"].as<int>(-1);
    // Sans carte il n'y a pas de clé, et hors de l'écran il n'y a pas de ligne :
    // dans les deux cas on laisse tomber l'entrée plutôt que d'en poser une que
    // le joueur ne pourra jamais ni lire ni retirer.
    if (map.empty() || line.x < 0 || line.y < 0) continue;
    std::snprintf(line.map, sizeof(line.map), "%s", map.c_str());
    tracker->lines_.push_back(line);
  }
}

void WriteMvpTrackerLines(YAML::Emitter& out) {
  auto* tracker = Bourgeon::Instance().mvp_tracker_window();
  out << YAML::Key << "mvptracker_lines" << YAML::Value << YAML::BeginSeq;
  if (tracker) {
    for (const MvpTrackerWindow::PinnedLine& line : tracker->lines_) {
      out << YAML::BeginMap
          << YAML::Key << "mob" << YAML::Value << static_cast<int>(line.mob_id)
          << YAML::Key << "map" << YAML::Value << std::string(line.map)
          << YAML::Key << "x"   << YAML::Value << line.x
          << YAML::Key << "y"   << YAML::Value << line.y
          << YAML::EndMap;
    }
  }
  out << YAML::EndSeq;
}

}  // namespace moonlight_ui
