#include "plugins/moonlight_ui.h"

#include "plugins/moonlight_ui/internal.h"       // panneaux extraits (dossier privé)
#include "plugins/moonlight_ui/settings_containers.h"  // collections persistées
#include "plugins/moonlight_ui/settings_table.h"       // description des réglages persistés

#include <Windows.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bourgeon.h"
#include "plugins/item_desc_tweaks.h"
#include "ui/color_codec.h"
#include "ui/ro_imgui.h"
#include "ui/skin_panel.h"
#include "plugins/chat.h"
#include "plugins/discord_relay.h"
#include "plugins/basic_info.h"
#include "plugins/dps_meter.h"
#include "plugins/menu_icons.h"
#include "plugins/status_icon_tweaks.h"
#include "plugins/quest_tracker_tweaks.h"
#include "plugins/settings_tweaks.h"
#include "plugins/entity_names.h"
#include "plugins/skill_bar_tweaks.h"
#include "plugins/storage_tweaks.h"
#include "plugins/inventory_viewer.h"
#include "plugins/cashshop_tweaks.h"
#include "plugins/shop_tweaks.h"
#include "plugins/trade_tweaks.h"
#include "plugins/rodex_tweaks.h"
#include "plugins/npc_dialog_tweaks.h"
#include "plugins/bug_report.h"
#include "plugins/character_sheet.h"
#include "plugins/login_parade.h"
#include "plugins/doom_tweaks.h"
#include "plugins/roggle_tweaks.h"
#include "plugins/rojeweled_tweaks.h"
#include "plugins/keyboard_move.h"
#include "plugins/player_jump.h"
#include "plugins/status_tweaks.h"
#include "plugins/equip_tweaks.h"
#include "plugins/window_pos_tweaks.h"
#include "plugins/weapon_dual_sprites.h"
#include "plugins/spr_effect_lab.h"
#include "ragnarok/ui_window_mgr.h"
#include "ragnarok/uiwnd.h"
#include "utils/game_paths.h"
#include "spdlog/fmt/fmt.h"
#include "utils/byte_pattern.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// Les helpers de couleur persistée vivent avec les moteurs de la table
// (moonlight_ui/settings_table.h) : ce sont eux qui connaissent la dualité
// « hex ARGB » / « ImU32 décimal » des yaml déjà chez les joueurs. Il n'en reste
// qu'un ici, pour les fonds de chat — dont les clés sont portées par les groupes
// eux-mêmes (ChatBgGroup::yaml_key), pas par des littéraux.
using moonlight_ui::WriteArgbKey;

// ── Réglages décrits une fois, lus et écrits par la table ────────────────────
// Chaque bloc ci-dessous remplace une paire lecture/écriture qu'il fallait tenir
// synchronisée à la main. Le défaut ne s'écrit plus ici : MLUI_DEFAULT le prend
// dans le header du plugin, seule source de vérité.
//
// ⚠ L'ORDRE de la table est l'ordre d'émission dans le yaml : le conserver
// garde les fichiers des joueurs diff-identiques après le refactor.
using SType = moonlight_ui::SettingType;

// Barre d'icônes de statut (buffs/debuffs). Les deux dernières couleurs sont
// persistées en ImU32 DÉCIMAL, pas en hex ARGB — héritage figé.
#define SICON(member) \
  MLUI_FIELD(status_icons, config().member), MLUI_DEFAULT(StatusIconConfig, member)
const moonlight_ui::SettingDesc kStatusIconSettings[] = {
    {"statusicon_enabled",        SType::kBool,     SICON(enabled)},
    {"statusicon_corner",         SType::kInt,      SICON(corner)},
    {"statusicon_margin_x",       SType::kInt,      SICON(margin_x)},
    {"statusicon_margin_y",       SType::kInt,      SICON(margin_y)},
    {"statusicon_step_dir",       SType::kInt,      SICON(step_dir)},
    {"statusicon_wrap_dir",       SType::kInt,      SICON(wrap_dir)},
    {"statusicon_per_line",       SType::kInt,      SICON(per_line)},
    {"statusicon_icon_pitch",     SType::kInt,      SICON(icon_pitch)},
    {"statusicon_line_pitch",     SType::kInt,      SICON(line_pitch)},
    {"statusicon_sort_mode",      SType::kInt,      SICON(sort_mode)},
    {"statusicon_show_remaining", SType::kBool,     SICON(show_remaining)},
    {"statusicon_time_bg",        SType::kBool,     SICON(time_bg)},
    {"statusicon_icon_alpha",     SType::kInt,      SICON(icon_alpha)},
    {"statusicon_icon_size",      SType::kInt,      SICON(icon_size)},
    {"statusicon_time_place",     SType::kInt,      SICON(time_place)},
    {"statusicon_time_anchor",    SType::kInt,      SICON(time_anchor)},
    {"statusicon_time_bold",      SType::kBool,     SICON(time_bold)},
    {"statusicon_time_text",      SType::kColorU32, SICON(col_time_text)},
    {"statusicon_time_shadow",    SType::kColorU32, SICON(col_time_shadow)},
};
#undef SICON

// Suivi de quête (overlay ImGui).
#define QTRACK(member) \
  MLUI_FIELD(quest_tracker, config().member), MLUI_DEFAULT(QuestTrackerConfig, member)
const moonlight_ui::SettingDesc kQuestTrackerSettings[] = {
    {"questtracker_enabled",        SType::kBool, QTRACK(enabled)},
    {"questtracker_show_titlebar",  SType::kBool, QTRACK(show_titlebar)},
    {"questtracker_locked",         SType::kBool, QTRACK(locked)},
    {"questtracker_pos_x",          SType::kInt,  QTRACK(pos_x)},
    {"questtracker_pos_y",          SType::kInt,  QTRACK(pos_y)},
    {"questtracker_width",          SType::kInt,  QTRACK(width)},
    {"questtracker_max_quests",     SType::kInt,  QTRACK(max_quests)},
    {"questtracker_title_rgb",      SType::kInt,  QTRACK(title_rgb)},
    {"questtracker_desc_rgb",       SType::kInt,  QTRACK(desc_rgb)},
    {"questtracker_hunt_rgb",       SType::kInt,  QTRACK(hunt_rgb)},
    {"questtracker_font_scale",     SType::kInt,  QTRACK(font_scale)},
    {"questtracker_show_bg",        SType::kBool, QTRACK(show_bg)},
    {"questtracker_bg_alpha",       SType::kInt,  QTRACK(bg_alpha)},
    {"questtracker_show_objective", SType::kBool, QTRACK(show_objective)},
};
#undef QTRACK

// Noms d'entités au-dessus des acteurs. Les champs d'EntityNames sont privés et
// n'existent que derrière des accesseurs : le défaut ne peut pas être lu dans le
// header, il est donc littéral ici (et une seule fois, cf. MLUI_LITERAL).
const moonlight_ui::SettingDesc kEntityNameSettings[] = {
    {"entnames_enabled",   SType::kBool,  MLUI_FIELD(entity_names, enabled()),
     MLUI_LITERAL(bool, false)},
    {"entnames_players",   SType::kBool,  MLUI_FIELD(entity_names, show_players()),
     MLUI_LITERAL(bool, true)},
    {"entnames_monsters",  SType::kBool,  MLUI_FIELD(entity_names, show_monsters()),
     MLUI_LITERAL(bool, false)},
    {"entnames_npcs",      SType::kBool,  MLUI_FIELD(entity_names, show_npcs()),
     MLUI_LITERAL(bool, false)},
    {"entnames_self",      SType::kBool,  MLUI_FIELD(entity_names, show_self()),
     MLUI_LITERAL(bool, false)},
    {"entnames_outline",   SType::kBool,  MLUI_FIELD(entity_names, outline()),
     MLUI_LITERAL(bool, true)},
    {"entnames_yoffset",   SType::kInt,   MLUI_FIELD(entity_names, y_offset()),
     MLUI_LITERAL(int, 2)},
    {"entnames_fontscale", SType::kFloat, MLUI_FIELD(entity_names, font_scale()),
     MLUI_LITERAL(float, 1.0f)},
};

// Post-traitement D3D9 + réglages graphiques divers (SettingsTweaks). Les 13
// premières vivent dans la structure fx() ; les 9 suivantes sont des accesseurs
// sur des membres privés, d'où les littéraux.
#define POSTFX(member) \
  MLUI_FIELD(settings_tweaks, fx().member), MLUI_DEFAULT(D3D9PostFx, member)
const moonlight_ui::SettingDesc kGraphicsSettings[] = {
    {"fx_enabled",       SType::kBool,  POSTFX(enabled)},
    {"fx_brightness",    SType::kFloat, POSTFX(brightness)},
    {"fx_contrast",      SType::kFloat, POSTFX(contrast)},
    {"fx_gamma",         SType::kFloat, POSTFX(gamma)},
    {"fx_saturation",    SType::kFloat, POSTFX(saturation)},
    {"fx_temperature",   SType::kFloat, POSTFX(temperature)},
    {"fx_filter",        SType::kInt,   POSTFX(filter)},
    {"fx_vignette",      SType::kFloat, POSTFX(vignette)},
    {"fx_grain",         SType::kFloat, POSTFX(grain)},
    {"fx_aberration",    SType::kFloat, POSTFX(aberration)},
    {"fx_sharpen",       SType::kFloat, POSTFX(sharpen)},
    {"fx_fxaa",          SType::kBool,  POSTFX(fxaa)},
    {"fx_fxaa_strength", SType::kFloat, POSTFX(fxaa_strength)},
    {"fps_overlay",       SType::kBool,  MLUI_FIELD(settings_tweaks, fps_overlay()),
     MLUI_LITERAL(bool, false)},
    {"cam_zoom_enabled",  SType::kBool,  MLUI_FIELD(settings_tweaks, zoom_enabled()),
     MLUI_LITERAL(bool, false)},
    {"cam_zoom_factor",   SType::kFloat, MLUI_FIELD(settings_tweaks, zoom_factor()),
     MLUI_LITERAL(float, 1.0f)},
    {"cam_zoom_speed",    SType::kFloat, MLUI_FIELD(settings_tweaks, zoom_speed()),
     MLUI_LITERAL(float, 1.0f)},
    {"tex_filter",        SType::kInt,   MLUI_FIELD(settings_tweaks, tex_filter()),
     MLUI_LITERAL(int, 0)},
    // INT_MIN = « aucune position mémorisée » : la fenêtre garde son placement natif.
    {"game_option_pos_x", SType::kInt,   MLUI_FIELD(settings_tweaks, gopt_x()),
     MLUI_LITERAL(int, INT_MIN)},
    {"game_option_pos_y", SType::kInt,   MLUI_FIELD(settings_tweaks, gopt_y()),
     MLUI_LITERAL(int, INT_MIN)},
    {"esc_option_pos_x",  SType::kInt,   MLUI_FIELD(settings_tweaks, esc_x()),
     MLUI_LITERAL(int, INT_MIN)},
    {"esc_option_pos_y",  SType::kInt,   MLUI_FIELD(settings_tweaks, esc_y()),
     MLUI_LITERAL(int, INT_MIN)},
};
#undef POSTFX

// Compteur de dégâts. Les membres sont publics mais DpsMeter est un plugin : on
// ne peut pas en instancier un exemplaire juste pour lire ses défauts, d'où les
// littéraux (qui restent, eux, la seule source pour la lecture ET l'écriture).
const moonlight_ui::SettingDesc kDpsSettings[] = {
    {"dps_ground_dmg_chat", SType::kBool,
     MLUI_FIELD(dps_meter, show_ground_dmg_in_chat_), MLUI_LITERAL(bool, true)},
    {"dps_locked",   SType::kBool,  MLUI_FIELD(dps_meter, locked_),
     MLUI_LITERAL(bool, false)},
    {"dps_bg_alpha", SType::kFloat, MLUI_FIELD(dps_meter, bg_alpha_),
     MLUI_LITERAL(float, 0.90f)},
    {"dps_text_color", SType::kColorHex, MLUI_FIELD(dps_meter, text_color_),
     MLUI_LITERAL_ARGB(0xFFFFCC33)},
    {"dps_plot_color", SType::kColorHex, MLUI_FIELD(dps_meter, plot_color_),
     MLUI_LITERAL_ARGB(0xFFFFCC33)},
    {"dps_visible",             SType::kBool, MLUI_FIELD(dps_meter, visible_),
     MLUI_LITERAL(bool, true)},
    {"dps_slot_ms",             SType::kInt,  MLUI_FIELD(dps_meter, slot_ms_),
     MLUI_LITERAL(int, 200)},
    {"dps_window_secs",         SType::kInt,  MLUI_FIELD(dps_meter, dps_window_secs_),
     MLUI_LITERAL(int, 10)},
    {"dps_combat_timeout_secs", SType::kInt,  MLUI_FIELD(dps_meter, combat_timeout_secs_),
     MLUI_LITERAL(int, 5)},
};

// Fenêtre de description (item + skill). Le défaut d'ancrage vaut 3 = bas-droite,
// la valeur déclarée dans item_desc_tweaks.h : il valait 0 dans le repli
// d'écriture, ce qui ramenait silencieusement l'ancrage en haut-gauche.
const moonlight_ui::SettingDesc kItemDescSettings[] = {
    {"itemdesc_show_item",  SType::kBool, MLUI_FIELD(item_desc, show_item_panel()),
     MLUI_LITERAL(bool, true)},
    {"itemdesc_show_skill", SType::kBool, MLUI_FIELD(item_desc, show_skill_panel()),
     MLUI_LITERAL(bool, true)},
    {"itemdesc_compare",    SType::kBool, MLUI_FIELD(item_desc, cmp_show_equipped()),
     MLUI_LITERAL(bool, true)},
    {"itemdesc_spawn_cursor", SType::kBool,
     MLUI_FIELD(item_desc, desc_spawn_at_cursor()), MLUI_LITERAL(bool, true)},
    {"itemdesc_anchor", SType::kInt, MLUI_FIELD(item_desc, desc_anchor()),
     MLUI_LITERAL(int, 3)},
    {"itemdesc_off_x",  SType::kInt, MLUI_FIELD(item_desc, desc_offset_x()),
     MLUI_LITERAL(int, 12)},
    {"itemdesc_off_y",  SType::kInt, MLUI_FIELD(item_desc, desc_offset_y()),
     MLUI_LITERAL(int, 12)},
};

// Deux interrupteurs isolés, chacun chez son plugin.
const moonlight_ui::SettingDesc kBugReportSettings[] = {
    {"bugreport_button", SType::kBool, MLUI_FIELD(bug_report, enabled()),
     MLUI_LITERAL(bool, true)},
};
const moonlight_ui::SettingDesc kWeaponSpriteSettings[] = {
    {"weapon_dual_sprites", SType::kBool, MLUI_FIELD(weapon_dual_sprites, enabled()),
     MLUI_LITERAL(bool, false)},
};

// Inventaire ImGui. Le placement libre (inventory_layout) est un CONTENEUR :
// il reste écrit à la main, juste après cette table, à sa place d'origine.
const moonlight_ui::SettingDesc kInventorySettings[] = {
    {"inventory_imgui",   SType::kBool, MLUI_FIELD(inventory_viewer, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"inventory_filter",  SType::kBool, MLUI_FIELD(inventory_viewer, show_filter()),
     MLUI_LITERAL(bool, true)},
    {"inventory_desc_tooltip", SType::kBool,
     MLUI_FIELD(inventory_viewer, desc_tooltip()), MLUI_LITERAL(bool, false)},
    {"inventory_tabs_vertical", SType::kBool,
     MLUI_FIELD(inventory_viewer, tabs_vertical()), MLUI_LITERAL(bool, true)},
    {"inventory_lock_size",   SType::kBool, MLUI_FIELD(inventory_viewer, lock_size()),
     MLUI_LITERAL(bool, false)},
    {"inventory_free_layout", SType::kBool, MLUI_FIELD(inventory_viewer, free_layout()),
     MLUI_LITERAL(bool, false)},
};

// Entrepôt (Kafra / guilde / premium : la même fenêtre). storage_favorites est
// un CONTENEUR, écrit à la main après cette table.
const moonlight_ui::SettingDesc kStorageSettings[] = {
    {"storage_imgui", SType::kBool, MLUI_FIELD(storage_tweaks, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"storage_desc_tooltip", SType::kBool, MLUI_FIELD(storage_tweaks, desc_tooltip()),
     MLUI_LITERAL(bool, false)},
    {"storage_filter", SType::kBool, MLUI_FIELD(storage_tweaks, show_filter()),
     MLUI_LITERAL(bool, true)},
    {"storage_tabs_vertical", SType::kBool, MLUI_FIELD(storage_tweaks, tabs_vertical()),
     MLUI_LITERAL(bool, false)},
    {"storage_tab_images", SType::kBool, MLUI_FIELD(storage_tweaks, tab_images()),
     MLUI_LITERAL(bool, true)},
    {"storage_col_index", SType::kBool, MLUI_FIELD(storage_tweaks, show_index_col()),
     MLUI_LITERAL(bool, false)},
    {"storage_col_id", SType::kBool, MLUI_FIELD(storage_tweaks, show_id_col()),
     MLUI_LITERAL(bool, false)},
    {"storage_col_slots", SType::kBool, MLUI_FIELD(storage_tweaks, show_slots_col()),
     MLUI_LITERAL(bool, false)},
    {"storage_col_value", SType::kBool, MLUI_FIELD(storage_tweaks, show_value_col()),
     MLUI_LITERAL(bool, true)},
    {"storage_total_value", SType::kBool, MLUI_FIELD(storage_tweaks, show_total_value()),
     MLUI_LITERAL(bool, true)},
    {"storage_tab", SType::kInt, MLUI_FIELD(storage_tweaks, cur_tab()),
     MLUI_LITERAL(int, 0)},
};

// Fenêtres ImGui opt-in restantes + pose de l'avatar de la feuille de perso.
const moonlight_ui::SettingDesc kOptInWindowSettings[] = {
    {"cashshop_imgui", SType::kBool, MLUI_FIELD(cashshop_tweaks, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"shop_imgui",  SType::kBool, MLUI_FIELD(shop_tweaks, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"trade_imgui", SType::kBool, MLUI_FIELD(trade_tweaks, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"rodex_imgui", SType::kBool, MLUI_FIELD(rodex_tweaks, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"npc_dialog_imgui", SType::kBool, MLUI_FIELD(npc_dialog_tweaks, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"npc_menu_search",  SType::kBool, MLUI_FIELD(npc_dialog_tweaks, menu_search_),
     MLUI_LITERAL(bool, true)},
    {"charsheet_imgui", SType::kBool, MLUI_FIELD(character_sheet, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"charsheet_open",  SType::kBool, MLUI_FIELD(character_sheet, open()),
     MLUI_LITERAL(bool, true)},
    {"charsheet_pose",  SType::kInt,  MLUI_FIELD(character_sheet, avatar_anim()),
     MLUI_LITERAL(int, 4)},
    {"charsheet_dir",   SType::kInt,  MLUI_FIELD(character_sheet, avatar_dir()),
     MLUI_LITERAL(int, 0)},
    {"charsheet_pose_anim", SType::kBool,
     MLUI_FIELD(character_sheet, avatar_animate()), MLUI_LITERAL(bool, true)},
    {"login_parade", SType::kBool, MLUI_FIELD(login_parade, enabled_),
     MLUI_LITERAL(bool, true)},
};

// Touche de saut (PlayerJumpTweaks). Le reste du plugin (activation, hauteur,
// durée) reste vif et non persisté ; seul le remappage doit survivre au
// redémarrage, sinon le joueur le referait à chaque session. 0x20 = VK_SPACE.
const moonlight_ui::SettingDesc kJumpKeySettings[] = {
    {"jump_key_vk",    SType::kInt,  MLUI_FIELD(player_jump, key_vk()),
     MLUI_LITERAL(int, 0x20)},
    {"jump_key_ctrl",  SType::kBool, MLUI_FIELD(player_jump, key_ctrl()),
     MLUI_LITERAL(bool, false)},
    {"jump_key_alt",   SType::kBool, MLUI_FIELD(player_jump, key_alt()),
     MLUI_LITERAL(bool, false)},
    {"jump_key_shift", SType::kBool, MLUI_FIELD(player_jump, key_shift()),
     MLUI_LITERAL(bool, false)},
};

// Barres EXP/HP/SP et portrait de statut (BasicInfoTweaks). Les barres et les
// éléments du portrait sont indexés (expbar_<barre>_*, portrait_<élément>_*) :
// leurs clés se construisent à l'exécution, elles restent en boucle.
const moonlight_ui::SettingDesc kBasicInfoSettings[] = {
    {"expbar_visible",  SType::kBool,  MLUI_FIELD(basic_info, visible_),
     MLUI_LITERAL(bool, false)},
    {"expbar_locked",   SType::kBool,  MLUI_FIELD(basic_info, locked_),
     MLUI_LITERAL(bool, false)},
    {"expbar_sticky",   SType::kBool,  MLUI_FIELD(basic_info, sticky_),
     MLUI_LITERAL(bool, false)},
    {"expbar_text",     SType::kInt,   MLUI_FIELD(basic_info, text_mode_),
     MLUI_LITERAL(int, 1)},
    {"expbar_vertical", SType::kBool,  MLUI_FIELD(basic_info, vertical_),
     MLUI_LITERAL(bool, false)},
    {"expbar_border",   SType::kBool,  MLUI_FIELD(basic_info, border_),
     MLUI_LITERAL(bool, true)},
    {"expbar_rounding", SType::kFloat, MLUI_FIELD(basic_info, rounding_),
     MLUI_LITERAL(float, 4.0f)},
    // Le repli d'écriture disait B30D0D12 là où le plugin porte B20D0D12 : une
    // unité d'alpha d'écart, invisible mais bien une divergence de plus entre
    // les deux miroirs. La table garde la valeur du plugin.
    {"expbar_bg_color", SType::kColorHex, MLUI_FIELD(basic_info, bg_color_),
     MLUI_LITERAL_ARGB(0xB20D0D12)},
};

// Portrait de statut (même plugin) : un bloc séparé parce qu'il s'écrit après
// les positions de fenêtres et la boucle des barres.
const moonlight_ui::SettingDesc kPortraitSettings[] = {
    {"portrait_visible", SType::kBool, MLUI_FIELD(basic_info, portrait_visible_),
     MLUI_LITERAL(bool, false)},
    {"portrait_locked",  SType::kBool, MLUI_FIELD(basic_info, portrait_locked_),
     MLUI_LITERAL(bool, false)},
    {"portrait_hide_basic_info", SType::kBool,
     MLUI_FIELD(basic_info, portrait_hide_basic_info_), MLUI_LITERAL(bool, false)},
    {"portrait_border", SType::kBool, MLUI_FIELD(basic_info, portrait_border_),
     MLUI_LITERAL(bool, false)},
    {"portrait_head_sprite", SType::kBool,
     MLUI_FIELD(basic_info, portrait_head_sprite_), MLUI_LITERAL(bool, true)},
    {"portrait_head_only", SType::kBool, MLUI_FIELD(basic_info, portrait_head_only_),
     MLUI_LITERAL(bool, true)},
    {"portrait_head_zoom", SType::kFloat, MLUI_FIELD(basic_info, portrait_head_zoom_),
     MLUI_LITERAL(float, 1.0f)},
    {"portrait_head_offx", SType::kFloat, MLUI_FIELD(basic_info, portrait_head_offx_),
     MLUI_LITERAL(float, 0.0f)},
    {"portrait_head_offy", SType::kFloat, MLUI_FIELD(basic_info, portrait_head_offy_),
     MLUI_LITERAL(float, 0.0f)},
    {"portrait_anim", SType::kInt, MLUI_FIELD(basic_info, portrait_anim_),
     MLUI_LITERAL(int, 4)},
    {"portrait_dir",  SType::kInt, MLUI_FIELD(basic_info, portrait_dir_),
     MLUI_LITERAL(int, 0)},
    {"portrait_animate", SType::kBool, MLUI_FIELD(basic_info, portrait_animate_),
     MLUI_LITERAL(bool, true)},
    {"portrait_show_garment", SType::kBool,
     MLUI_FIELD(basic_info, portrait_show_garment_), MLUI_LITERAL(bool, true)},
};

// Barres de raccourcis ImGui. skillbar_key_scale / skillbar_count_scale restent
// hors table : leur défaut n'est pas une constante mais l'ANCIENNE clé unique
// skillbar_text_scale, encore lue en repli — une migration, pas un réglage.
const moonlight_ui::SettingDesc kSkillBarSettings[] = {
    {"skillbar_enabled", SType::kBool, MLUI_FIELD(skill_bar, enabled_),
     MLUI_LITERAL(bool, false)},
    {"skillbar_locked",  SType::kBool, MLUI_FIELD(skill_bar, locked_),
     MLUI_LITERAL(bool, true)},
    {"skillbar_bilinear", SType::kBool, MLUI_FIELD(skill_bar, bilinear_),
     MLUI_LITERAL(bool, false)},
    {"skillbar_clickthrough", SType::kBool, MLUI_FIELD(skill_bar, clickthrough_),
     MLUI_LITERAL(bool, false)},
    {"skillbar_show_keys", SType::kBool, MLUI_FIELD(skill_bar, show_keys_),
     MLUI_LITERAL(bool, true)},
    {"skillbar_bold_text", SType::kBool, MLUI_FIELD(skill_bar, bold_text_),
     MLUI_LITERAL(bool, false)},
    // Ces deux-ci n'avaient pas de défaut constant : c'était l'ANCIENNE clé
    // unique skillbar_text_scale, lue en repli. MigrateLegacyKeys la recopie
    // maintenant en amont, donc le défaut redevient une simple constante.
    {"skillbar_key_scale",   SType::kFloat, MLUI_FIELD(skill_bar, key_scale_),
     MLUI_LITERAL(float, 1.0f)},
    {"skillbar_count_scale", SType::kFloat, MLUI_FIELD(skill_bar, count_scale_),
     MLUI_LITERAL(float, 1.0f)},
};

// Couleurs des barres. Les ARGB ci-dessous sont la conversion EXACTE (formule de
// ro::ArgbFromPicker) des flottants déclarés dans skill_bar_tweaks.h ; la couleur
// étant de toute façon quantifiée sur 8 bits dès la première sauvegarde, le
// défaut est identique à ce que le plugin porte.
const moonlight_ui::SettingDesc kSkillBarColorSettings[] = {
    {"skillbar_col_frame",    SType::kColorHex, MLUI_FIELD(skill_bar, col_frame_),
     MLUI_LITERAL_ARGB(0x990D0D12)},
    {"skillbar_col_skill",    SType::kColorHex, MLUI_FIELD(skill_bar, col_skill_),
     MLUI_LITERAL_ARGB(0xDC288246)},
    {"skillbar_col_item",     SType::kColorHex, MLUI_FIELD(skill_bar, col_item_),
     MLUI_LITERAL_ARGB(0xDC285096)},
    {"skillbar_col_empty",    SType::kColorHex, MLUI_FIELD(skill_bar, col_empty_),
     MLUI_LITERAL_ARGB(0xC81E1E24)},
    {"skillbar_col_border",   SType::kColorHex, MLUI_FIELD(skill_bar, col_border_),
     MLUI_LITERAL_ARGB(0xC8000000)},
    {"skillbar_col_borderhi", SType::kColorHex, MLUI_FIELD(skill_bar, col_borderhi_),
     MLUI_LITERAL_ARGB(0xE6FFDC78)},
    {"skillbar_col_keytext",  SType::kColorHex, MLUI_FIELD(skill_bar, col_keytext_),
     MLUI_LITERAL_ARGB(0xFF000000)},
    {"skillbar_col_count",    SType::kColorHex, MLUI_FIELD(skill_bar, col_count_),
     MLUI_LITERAL_ARGB(0xFF000000)},
    {"skillbar_col_textout",  SType::kColorHex, MLUI_FIELD(skill_bar, col_textout_),
     MLUI_LITERAL_ARGB(0xFFFFFFFF)},
};

// « Sol uni » du SPR Lab (fond de capture). Son état n'appartient pas à un
// plugin enregistré mais à deux globales de spr_lab, atteintes par des fonctions
// libres : le résolveur s'écrit à la main, il ne peut jamais rendre nullptr.
const moonlight_ui::SettingDesc kGroundPaintSettings[] = {
    {"ground_paint", SType::kBool,
     []() -> void* { return &spr_lab::ground_paint_enabled(); },
     MLUI_LITERAL(bool, false)},
    {"ground_paint_color", SType::kColorHex,
     []() -> void* { return spr_lab::ground_color(); },
     MLUI_LITERAL_ARGB(0xFF000000)},  // noir opaque
};

}  // namespace

// ── Réglages qui appartiennent à MoonlightUi elle-même ───────────────────────
// Struct-amie déclarée dans moonlight_ui.h (cf. le commentaire là-bas) : un
// descripteur pointe l'ADRESSE du champ, et ceux-ci sont privés. Elle ne porte
// que les deux tables, pas de code — c'est le strict minimum d'ouverture.
//
// Ces réglages n'ont pas de « plugin absent » possible… sauf en théorie : le
// résolveur passe quand même par Bourgeon::Instance().moonlight_ui(), qui rend
// nullptr si l'instance n'est pas (encore) enregistrée.
#define MLUI_SELF(member)                                 \
  []() -> void* {                                         \
    auto* self = Bourgeon::Instance().moonlight_ui();     \
    return self ? &self->member : nullptr;                \
  }
struct MoonlightUiOwnSettings {
  using SType = moonlight_ui::SettingType;

  // En-tête du fichier : état de la fenêtre + journalisation + overlay alootid.
  static const moonlight_ui::SettingDesc kHeader[3];
  // Réglages de chat portés par MoonlightUi (ils déménageront chez ChatTweaks à
  // l'étape C — c'est ce qui débloquera le déplacement du panneau « Chat »).
  static const moonlight_ui::SettingDesc kChat[5];
  // Grille d'alignement globale : elle sert à TOUS les overlays déplaçables,
  // d'où sa place ici plutôt que chez basic_info, où elle a commencé (cf. les
  // anciennes clés expbar_grid_*, recopiées par MigrateLegacyKeys).
  static const moonlight_ui::SettingDesc kGrid[4];
};

const moonlight_ui::SettingDesc MoonlightUiOwnSettings::kHeader[3] = {
    {"ui_collapsed", SType::kBool, MLUI_SELF(ui_collapsed_),
     MLUI_LITERAL(bool, false)},
    {"log_level", SType::kString, MLUI_SELF(log_level_),
     MLUI_LITERAL(std::string, "info")},
    {"alootid_overlay", SType::kBool, MLUI_SELF(show_alootid_overlay_),
     MLUI_LITERAL(bool, false)},
};

// Seule « mainchat_preset_bar » appartient encore à MoonlightUi : c'est un
// réglage de SON panneau (afficher la barre de préréglages de couleur), pas du
// chat. Les quatre autres vivent chez ChatTweaks depuis qu'il porte ses propres
// réglages — MoonlightUi n'en garde que la persistance.
const moonlight_ui::SettingDesc MoonlightUiOwnSettings::kChat[5] = {
    {"mainchat_preset_bar", SType::kBool, MLUI_SELF(mainchat_preset_bar_),
     MLUI_LITERAL(bool, false)},
    {"chat_width_enabled", SType::kBool, MLUI_FIELD(chat_tweaks, custom_width()),
     MLUI_LITERAL(bool, false)},
    // La largeur est BORNÉE à 320..1200 par ChatTweaks::ApplySettings : un yaml
    // édité à la main ne doit pas pouvoir rendre le chat inutilisable.
    {"chat_width", SType::kInt, MLUI_FIELD(chat_tweaks, custom_width_px()),
     MLUI_LITERAL(int, 800)},
    {"chat_timestamps", SType::kBool, MLUI_FIELD(chat_tweaks, timestamps()),
     MLUI_LITERAL(bool, false)},
    {"chat_item_icons", SType::kBool, MLUI_FIELD(chat_tweaks, item_icons()),
     MLUI_LITERAL(bool, true)},
};

const moonlight_ui::SettingDesc MoonlightUiOwnSettings::kGrid[4] = {
    {"grid_show",  SType::kBool, MLUI_SELF(grid_.show), MLUI_LITERAL(bool, false)},
    {"grid_snap",  SType::kBool, MLUI_SELF(grid_.snap), MLUI_LITERAL(bool, false)},
    {"grid_size",  SType::kInt,  MLUI_SELF(grid_.cell_size_px), MLUI_LITERAL(int, 32)},
    {"grid_color", SType::kColorHex, MLUI_SELF(grid_.color),
     MLUI_LITERAL_ARGB(0x26FFFFFF)},
};
#undef MLUI_SELF

// Item-link icon injection moved to plugins/chat.cc (ChatTweaks).

// ── Item description window hook ──────────────────────────────────────────
// Hooks FUN_008c18b0 (__thiscall, 20250716 client) to capture the nameid of
// whichever item the player right-clicks to inspect.
//
// The function is a UI window message handler.  Message 0x18 means "set item":
//   param_3 is the item data struct; the item's nameid is stored as a
//   MSVC std::string (SSO layout) at byte offset 0x2C (= param_3[11]):
//     [0..3]  char* ptr  (or inline buf when small)
//     [4..7]  inline buf continued
//     [8..11] inline buf continued
//     [12]    _Mysize (string length)
//     [16]    _Myres  (capacity - 1)  <- checked against 15 to detect heap
//   If capacity-1 > 15 the string is heap-allocated and param_3[11] is a ptr;
//   otherwise the 16-byte inline buffer starts at param_3+0x2C.
//   atoi() on that text gives the numeric nameid.

// Niveau de groupe serveur du compte courant, reçu au login via le setting id 26
// (ZC_BOURGEON_SETTINGS), rempli depuis pc_get_group_level(sd). Non persisté :
// autoritatif serveur, rafraîchi à chaque login. File-static (pas un membre de
// MoonlightUi) pour ne pas changer le layout de la classe. Le seuil « staff »
// est appliqué dans IsStaff() ci-dessous.
static int g_staff_level = 0;

// Seuil de niveau de groupe serveur à partir duquel un compte est considéré
// « staff ». 80 pour ce serveur (des groupes non-staff peuvent avoir un level
// > 0 ; les vrais pouvoirs GM y sont gatés vers 60/90).
static constexpr int kStaffMinGroupLevel = 80;

// Staff = niveau de groupe serveur >= seuil (reçu au login via le setting id 26).
// Gate PUREMENT serveur : si le serveur n'envoie pas l'id 26 (sources pas à
// jour), la fonctionnalité reste masquée, y compris sur un poste dev.
bool IsStaff() { return g_staff_level >= kStaffMinGroupLevel; }



void MoonlightUi::LoadItemNames() {
  const std::string& base = paths::GameDir();

  // Try common RO client layouts in order.
  static const char* kCandidates[] = {
    "System\\itemInfoMerged.lua",
    "SystemEN\\itemInfoMerged.lua",
    "System\\itemInfo.lua",
    "SystemEN\\itemInfo.lua",
  };
  std::ifstream f;
  std::string path;
  for (const char* cand : kCandidates) {
    path = base + cand;
    f.open(path);
    if (f) break;
    f.clear();
  }
  if (!f) {
    LogError("[MoonlightUi] itemInfoMerged.lua not found (tried System\\ and SystemEN\\)");
    return;
  }
  // LogInfo("[MoonlightUi] loading item names from {}", path);

  uint32_t current_id = 0;
  std::string line;
  while (std::getline(f, line)) {
    // Match item ID line: \t[501] = {
    // Only treat as item ID when the bracket content is purely numeric.
    // Lines like  identifiedDisplayName = "Horn Card [Shield]",  contain
    // non-numeric brackets and must fall through to the Name check below.
    const auto lb = line.find('[');
    if (lb != std::string::npos) {
      const auto rb = line.find(']', lb + 1);
      if (rb != std::string::npos) {
        const auto between = line.substr(lb + 1, rb - lb - 1);
        const bool all_digits = !between.empty() &&
            std::all_of(between.begin(), between.end(),
                        [](unsigned char c){ return std::isdigit(c) != 0; });
        if (all_digits) {
          try {
            current_id = static_cast<uint32_t>(std::stoul(between));
          } catch (...) { current_id = 0; }
          continue;  // item ID line fully consumed
        }
        // Non-numeric bracket (e.g. "[Shield]"): fall through to Name check.
      }
    }
    // Match: Name = "Red Potion"  (compact format used by this client)
    // Also handles legacy: identifiedDisplayName = "Sleipnir",
    if (current_id > 0 &&
        (line.find("Name =") != std::string::npos)) {
      const auto q1 = line.find('"');
      const auto q2 = line.rfind('"');  // rfind: last quote handles names with "
      if (q1 != std::string::npos && q2 != std::string::npos && q1 < q2) {
        // itemInfoMerged.lua est en CP949, ImGui attend de l'UTF-8 : sans cette
        // conversion, tout nom d'item non-ASCII s'affichait en octets bruts.
        // Convertir ICI, une fois à l'insertion, plutôt qu'à chaque affichage :
        // Cp949ToUtf8 rend un buffer thread-local rotatif à 8 emplacements qu'il
        // ne faut jamais conserver — la copie dans la std::string règle la
        // question définitivement.
        const std::string raw = line.substr(q1 + 1, q2 - q1 - 1);
        item_names_[current_id] = ro::Cp949ToUtf8(raw.c_str());
      }
    }
  }
  // LogInfo("[MoonlightUi] loaded {} item names", item_names_.size());
}

MoonlightUi::MoonlightUi() {
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeFromServer);
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodePresetList);
  // Observe the standard map-move packet to learn the current map name.
  // ⚠ Le second argument est la longueur DÉCLARÉE du champ observé, et c'est
  // elle que OnRecvPacket recevra dans `len` — jamais le nombre d'octets
  // réellement reçus. Pour un opcode observé, `len` ne prouve donc rien sur ce
  // qui est lisible : borner le champ soi-même (cf. le strnlen côté réception).
  Bourgeon::Instance().RegisterObserveOpcode(kOpcodeMapMove, kMapNameLen);
  LoadItemNames();

  InstallItemDescProbe();
  // NB : hook de la fenêtre skill (0x2e) RETIRÉ — il crashait le chemin natif
  // du message 0x3d. Repro skill désactivée (kSkillWindowEnabled) en attendant
  // une approche sûre (inspection live du rich-text natif).
}

// ── Chat background colours ───────────────────────────────────────────────


// « Tout-ImGui ou tout-natif » (cf. déclaration dans moonlight_ui.h) : synchronise
// les 4 fenêtres modernes interdépendantes en un point unique (inventaire, storage,
// barres de skill, échange). Chaque plugin garde son propre flag, mais il n'est plus
// jamais basculé isolément.
void SetModernInterface(bool on) {
  if (auto* inventory_viewer = Bourgeon::Instance().inventory_viewer())
    inventory_viewer->imgui_enabled_ = on;
  if (auto* storage_tweaks = Bourgeon::Instance().storage_tweaks())
    storage_tweaks->imgui_enabled_ = on;
  if (auto* skill_bar = Bourgeon::Instance().skill_bar())
    skill_bar->enabled_ = on;
  if (auto* trade_tweaks = Bourgeon::Instance().trade_tweaks())
    trade_tweaks->imgui_enabled_ = on;
  // Le courrier fait partie du lot : sa fenêtre d'écriture reçoit les objets
  // glissés depuis l'inventaire ImGui, ce qui n'a de sens que si les deux sont
  // modernes en même temps (un inventaire natif ne sait pas déposer chez nous).
  if (auto* rodex_tweaks = Bourgeon::Instance().rodex_tweaks())
    rodex_tweaks->imgui_enabled_ = on;
}

// ── Settings persistence ──────────────────────────────────────────────────

void MoonlightUi::LoadSettings() {
  const std::string path = paths::SettingsPath();
  // Horodatage de COMPILATION, gardé volontairement (une ligne par login) : le
  // déploiement POST_BUILD est best-effort et SILENCIEUSEMENT sauté quand le jeu tient
  // ddraw.dll ouvert (cf. src/CMakeLists.txt) — le build passe au vert sans rien
  // déployer. Cette ligne dit immédiatement quelle DLL tourne réellement.
  // LogInfo("[Bourgeon] build " __DATE__ " " __TIME__);
  std::ifstream f(path);
  if (!f) return;  // premier lancement : pas encore de fichier

  // Un chargement qui échoue à mi-parcours laisse la moitié des réglages aux
  // défauts. Écrire dans cet état GRAVERAIT la perte, puisque WriteSettingsFile
  // réémet tout depuis l'état courant. Le drapeau interdit donc d'écrire tant
  // qu'on n'a pas relu le fichier correctement (au prochain changement de carte).
  settings_load_failed_ = false;

  try {
    const YAML::Node root = YAML::Load(f);
    // Non-const : MigrateLegacyKeys recopie les anciens noms de clés sur les
    // nouveaux DANS L'ARBRE EN MÉMOIRE, avant que quoi que ce soit ne lise.
    // Tout le reste ignore ainsi l'existence des anciens noms.
    YAML::Node ui = root["moonlight_ui"];
    if (!ui) return;
    moonlight_ui::MigrateLegacyKeys(ui);

    // Les couleurs sont seulement LUES ici ; c'est PostLoadApply qui demande à
    // ChatTweaks de les pousser dans le client, une fois tout le fichier relu.
    moonlight_ui::ReadChatBackgrounds(ui);
    moonlight_ui::ReadSettings(ui, kGroundPaintSettings);
    moonlight_ui::ReadSettings(ui, MoonlightUiOwnSettings::kHeader);
    moonlight_ui::ReadSettings(ui, kItemDescSettings);
    moonlight_ui::ReadSettings(ui, kBugReportSettings);
    moonlight_ui::ReadSettings(ui, kWeaponSpriteSettings);
    moonlight_ui::ReadSettings(ui, MoonlightUiOwnSettings::kChat);

    moonlight_ui::ReadSettings(ui, kDpsSettings);

    moonlight_ui::ReadSettings(ui, kBasicInfoSettings);
    moonlight_ui::ReadSettings(ui, kPortraitSettings);
    moonlight_ui::ReadBarLayout(ui);
    moonlight_ui::ReadPortraitLayout(ui);
    moonlight_ui::ReadSettings(ui, MoonlightUiOwnSettings::kGrid);
    moonlight_ui::ReadWindowPositions(ui);
    moonlight_ui::ReadMenuIcons(ui);
    moonlight_ui::ReadSkinAndPresets(ui);
    moonlight_ui::ReadSettings(ui, kInventorySettings);
    moonlight_ui::ReadInventoryLayout(ui);
    moonlight_ui::ReadSettings(ui, kStorageSettings);
    moonlight_ui::ReadStorageFavorites(ui);
    moonlight_ui::ReadSettings(ui, kOptInWindowSettings);
    moonlight_ui::ReadSettings(ui, kJumpKeySettings);
    moonlight_ui::ReadSettings(ui, kSkillBarSettings);
    moonlight_ui::ReadSkillBarLayout(ui);
    moonlight_ui::ReadSettings(ui, kSkillBarColorSettings);

    moonlight_ui::ReadSettings(ui, kStatusIconSettings);
    moonlight_ui::ReadSettings(ui, kQuestTrackerSettings);
    moonlight_ui::ReadSettings(ui, kGraphicsSettings);
    moonlight_ui::ReadSettings(ui, kEntityNameSettings);

    moonlight_ui::ReadChatBgPresets(ui);
    moonlight_ui::ReadEquipPresets(ui);

    PostLoadApply();
  } catch (const std::exception& e) {
    settings_load_failed_ = true;
    LogError("[MoonlightUi] lecture de {} interrompue : {} — SAUVEGARDE SUSPENDUE "
             "pour ne pas écraser la configuration existante (elle reprendra après "
             "un chargement complet ; corriger ou supprimer le fichier)", path, e.what());
  }
}

// ── Effets de bord d'un chargement ───────────────────────────────────────────
// Lire un réglage ne suffit pas toujours : certains doivent être BORNÉS, ou
// poussés vers une couche qui en garde une copie (chat natif, journal, d3d9,
// barre d'icônes de statut). Ces gestes étaient dispersés dans LoadSettings,
// chacun collé à sa lecture ; ils tiennent ici, après que TOUT a été lu.
//
// Appelé en DERNIER dans le try : si la lecture s'interrompt, on n'applique rien
// d'une configuration à moitié lue — même raison que le drapeau
// settings_load_failed_ qui suspend la sauvegarde.
void MoonlightUi::PostLoadApply() {
  // Le chat borne sa largeur et pousse ses quatre réglages vers le moteur : il
  // les possède, on ne fait que lui dire qu'ils viennent d'être relus.
  if (auto* chat_tweaks = Bourgeon::Instance().chat_tweaks())
    chat_tweaks->ApplySettings();
  LogConsole::instance().SetLevel(log_level_);
  pending_collapse_restore_ = true;

  // « Tout-ImGui ou tout-natif » : ces 5 fenêtres (inventaire/entrepôt/barres/
  // échange/courrier) s'activent ensemble. Un yaml antérieur au regroupement
  // pouvait être mixé — on réconcilie en OU (au moins une moderne => toutes
  // modernes ; tout natif sinon), puis les cases restent synchronisées.
  auto* inventory = Bourgeon::Instance().inventory_viewer();
  auto* storage   = Bourgeon::Instance().storage_tweaks();
  auto* skill_bar = Bourgeon::Instance().skill_bar();
  auto* trade     = Bourgeon::Instance().trade_tweaks();
  auto* rodex     = Bourgeon::Instance().rodex_tweaks();
  SetModernInterface((inventory && inventory->imgui_enabled_) ||
                     (storage && storage->imgui_enabled_) ||
                     (skill_bar && skill_bar->enabled_) ||
                     (trade && trade->imgui_enabled_) ||
                     (rodex && rodex->imgui_enabled_));

  if (auto* status_icons = Bourgeon::Instance().status_icons()) status_icons->MarkDirty();
  if (auto* settings_tweaks = Bourgeon::Instance().settings_tweaks())
    settings_tweaks->Apply();  // pousse le post-traitement vers la couche d3d9
}

void MoonlightUi::WriteSettingsFile() {
  // Refus d'écrire après un chargement interrompu : l'état en mémoire ne
  // représente alors qu'une PARTIE du fichier, le reste étant aux défauts.
  // Réémettre depuis là rendrait la perte définitive — c'est le second volet du
  // bug B9, le premier étant les std::stoul qui provoquaient l'interruption.
  if (settings_load_failed_) {
    LogError("[MoonlightUi] sauvegarde refusée : la dernière lecture de "
             "bourgeon_settings.yaml avait échoué, écrire écraserait la config");
    return;
  }

  YAML::Emitter out;
  out << YAML::BeginMap
      << YAML::Key << "moonlight_ui"
      << YAML::Value << YAML::BeginMap;
  moonlight_ui::WriteChatBackgrounds(out);
  moonlight_ui::WriteSettings(out, kGroundPaintSettings);
  moonlight_ui::WriteSettings(out, MoonlightUiOwnSettings::kHeader);
  moonlight_ui::WriteSettings(out, kItemDescSettings);
  moonlight_ui::WriteSettings(out, kBugReportSettings);
  moonlight_ui::WriteSettings(out, kWeaponSpriteSettings);
  moonlight_ui::WriteSettings(out, MoonlightUiOwnSettings::kChat);
  moonlight_ui::WriteSettings(out, kDpsSettings);

  // Barres EXP/HP/SP (BasicInfoTweaks)
  moonlight_ui::WriteSettings(out, kBasicInfoSettings);
  moonlight_ui::WriteSettings(out, MoonlightUiOwnSettings::kGrid);
  moonlight_ui::WriteWindowPositions(out);
  moonlight_ui::WriteBarLayout(out);
  // Portrait de statut (même plugin) : scalaires puis disposition par élément.
  moonlight_ui::WriteSettings(out, kPortraitSettings);
  moonlight_ui::WritePortraitLayout(out);
  moonlight_ui::WriteMenuIcons(out);

  moonlight_ui::WriteSettings(out, kStatusIconSettings);

  moonlight_ui::WriteSettings(out, kQuestTrackerSettings);

  moonlight_ui::WriteSettings(out, kGraphicsSettings);

  moonlight_ui::WriteSettings(out, kEntityNameSettings);

  moonlight_ui::WriteSkinAndPresets(out);
  moonlight_ui::WriteSettings(out, kInventorySettings);
  moonlight_ui::WriteInventoryLayout(out);
  moonlight_ui::WriteSettings(out, kStorageSettings);
  moonlight_ui::WriteStorageFavorites(out);
  moonlight_ui::WriteSettings(out, kOptInWindowSettings);
  moonlight_ui::WriteSettings(out, kJumpKeySettings);

  moonlight_ui::WriteSettings(out, kSkillBarSettings);
  moonlight_ui::WriteSkillBarLayout(out);
  moonlight_ui::WriteSettings(out, kSkillBarColorSettings);

  moonlight_ui::WriteChatBgPresets(out);
  moonlight_ui::WriteEquipPresets(out);
  out << YAML::EndMap << YAML::EndMap;

  const std::string path = paths::SettingsPath();

  // bourgeon_settings.yaml est PARTAGÉ : AutoLogin (section « auto_login »), CharSelect
  // (« char_select ») et MoonlightAuth (« moonlight_auth ») y lisent chacun leur propre
  // section racine et ne la réécrivent JAMAIS. Écrire `out` directement tronquait donc le
  // fichier et DÉTRUISAIT ces sections : la première case cochée en jeu effaçait les
  // identifiants d'auto-login. On relit le document existant et on n'y remplace QUE la
  // clé « moonlight_ui ».
  // ⚠ yaml-cpp ne conserve pas les commentaires : ceux écrits à la main dans le fichier
  // disparaissent à la première sauvegarde. Toutes les VALEURS sont préservées.
  YAML::Node settings_root;
  try {
    settings_root = YAML::LoadFile(path);
  } catch (const std::exception&) {
    // Fichier absent (premier lancement) ou illisible : on repart d'un document vide
    // plutôt que de renoncer à sauvegarder nos propres réglages.
  }
  if (!settings_root.IsMap()) settings_root = YAML::Node(YAML::NodeType::Map);

  // `emitted_doc` doit rester vivant jusqu'à l'écriture : après l'assignation,
  // settings_root référence sa mémoire.
  YAML::Node emitted_doc;
  try {
    emitted_doc = YAML::Load(out.c_str());
  } catch (const std::exception& e) {
    LogError("[MoonlightUi] re-parse of emitted settings failed, not saving: {}", e.what());
    return;
  }
  settings_root["moonlight_ui"] = emitted_doc["moonlight_ui"];

  // Écriture ATOMIQUE : on remplit un fichier temporaire voisin, on le ferme (donc
  // on le vide sur disque), puis on le déplace PAR-DESSUS la cible. MoveFileEx avec
  // MOVEFILE_REPLACE_EXISTING est atomique sur un même volume — et le .tmp est dans
  // le même dossier, donc c'est bien le cas. À aucun instant bourgeon_settings.yaml
  // n'existe à moitié écrit.
  // Ce n'est pas de la prudence gratuite : le fichier porte AUSSI les sections
  // auto_login, char_select et moonlight_auth (cf. plus haut). Une écriture
  // tronquée par un crash, une coupure ou un disque plein n'emporterait donc pas
  // seulement nos réglages, mais les identifiants d'auto-login du joueur.
  const std::string tmp_path = path + ".tmp";
  {
    std::ofstream f(tmp_path, std::ios::trunc);
    if (!f) {
      LogError("[MoonlightUi] failed to open {}", tmp_path);
      return;
    }
    f << settings_root;
    f.flush();
    if (!f) {
      // Disque plein ou erreur d'écriture : on garde le fichier précédent INTACT.
      LogError("[MoonlightUi] write failed for {}, keeping previous settings", tmp_path);
      f.close();
      DeleteFileA(tmp_path.c_str());
      return;
    }
  }  // fermeture du flux (donc flush complet) AVANT le remplacement
  if (!MoveFileExA(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    LogError("[MoonlightUi] failed to replace {} (GetLastError={})", path,
             GetLastError());
    DeleteFileA(tmp_path.c_str());
  }
  // LogInfo("[MoonlightUi] saved chat backgrounds to {}", path);
}

// ── Anti-rebond de la sauvegarde ──────────────────────────────────────────
// Les ~40 sites d'appel de SaveSettings sont des widgets ImGui évalués à chaque
// frame. Un slider ou un color picker renvoie true à CHAQUE frame de glissement :
// le motif `if (changed) SaveSettings()` déclenchait donc une sérialisation YAML
// complète (~330 clés, ~140 std::string, 36 lectures mémoire client sous SEH) plus
// un cycle create/truncate/write/close, par frame — ~120 pour un drag de 2 s, sur
// le thread de rendu. On ne marque désormais que l'intention ; l'écriture a lieu
// une fois l'utilisateur stabilisé. Les appelants n'ont rien à changer.

void MoonlightUi::SaveSettings() {
  settings_dirty_    = true;
  settings_dirty_ms_ = GetTickCount();
}

void MoonlightUi::FlushSettings() {
  if (!settings_dirty_) return;
  settings_dirty_ = false;
  WriteSettingsFile();
}

void MoonlightUi::OnTick() {
  // OnTick tourne ~10 Hz même panneau fermé : le flush arrive donc aussi pour les
  // réglages poussés par les plugins frères hors de notre fenêtre.
  if (!settings_dirty_) return;
  if (GetTickCount() - settings_dirty_ms_ < kSettingsFlushDelayMs) return;
  FlushSettings();
}

// ── Server settings sync ──────────────────────────────────────────────────

void MoonlightUi::UpdateRelay() {
  if (auto* relay = Bourgeon::Instance().discord_relay()) {
    relay->set_chat_active(discord_chat_ && on_discord_relay_map_);
  }
}

// Called by ModeMgr::Switch (and OnUpdateHook for in_game_ tracking).
void MoonlightUi::OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) {
  const bool was_in_game = in_game_;
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);

  // Only update on_discord_relay_map_ when we have a real map name. OnUpdateHook
  // fires FireModeSwitch(kGame, "") on every tick for in_game_ tracking; that
  // empty call must not override the map we learned from a real CModeMgr::Switch.
  if (map_name && map_name[0] != '\0') {
    on_discord_relay_map_ =
        in_game_ && (strncmp(map_name, kDiscordRelayMapPrefix,
                             sizeof(kDiscordRelayMapPrefix) - 1) == 0);
  } else if (!in_game_) {
    on_discord_relay_map_ = false;
  }

  if (in_game_ && !was_in_game) {
    // Symétrique de la sortie : on écrit ce qui traîne AVANT de recharger, sinon un
    // réglage touché sur l'écran de login/char-select serait écrasé par LoadSettings
    // puis reperdu au flush suivant.
    FlushSettings();
    LoadSettings();
  }

  if (!in_game_ && was_in_game) {
    // Sortie du jeu (retour login / char-select) : on écrit TOUT DE SUITE, sans
    // attendre la fenêtre d'anti-rebond, sinon un réglage touché dans les dernières
    // centaines de ms serait perdu. Impérativement AVANT le clear : aloot_ids_ est
    // persisté, et flusher après sauvegarderait une liste @autolootid vide.
    FlushSettings();
    aloot_ids_.clear();
    // Le niveau staff est autoritatif SERVEUR, renvoyé au login via le setting
    // id 26 — mais rien ne garantit qu'il soit renvoyé quand il vaut 0. Sans ce
    // retour à zéro, changer de compte laissait les « Staff Tools » et les
    // réglages fins de saut/marche exposés sur un compte qui n'y a pas droit,
    // jusqu'au prochain paquet. On retombe fermé par défaut.
    g_staff_level = 0;
  }

  UpdateRelay();
}

// ZC packet layout (data points past [opcode:2][total_len:2]):
//   [char_id:4][count:2][{id:2, value:4} * count]
void MoonlightUi::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  if (opcode == kOpcodeMapMove) {
    // 0x0091 ZC_NPCACK_MAPMOVE : `data` pointe sur mapname[16] (ex. « gonryun.gat »).
    //
    // ⚠ `len` ne vaut RIEN pour un opcode OBSERVÉ : rag_connection transmet la
    // longueur ENREGISTRÉE au RegisterObserveOpcode (kMapNameLen), pas le nombre
    // d'octets réellement reçus. Un `if (len < 7) return;` donnerait donc une
    // fausse assurance — c'est le champ lui-même qu'il faut borner.
    //
    // Le nom n'est pas garanti terminé par un zéro : strnlen le borne à la taille
    // déclarée du champ, puis on ne compare que ce qu'on a réellement.
    if (!data) return;
    const char* map_name = reinterpret_cast<const char*>(data);
    const size_t name_len = strnlen(map_name, kMapNameLen);
    constexpr size_t kPrefixLen = sizeof(kDiscordRelayMapPrefix) - 1;
    on_discord_relay_map_ = in_game_ && name_len >= kPrefixLen &&
                  (strncmp(map_name, kDiscordRelayMapPrefix, kPrefixLen) == 0);
    // LogInfo("[MoonlightUi] map move -> '{}' on_discord_relay_map={}",
            // std::string(map_name, strnlen(map_name, len)), on_discord_relay_map_);
    UpdateRelay();
    return;
  }

  if (opcode == kOpcodePresetList) {
    // ZC_BOURGEON_PRESET_LIST: [active_no:1][count:1][{no:1,autoload:1,namelen:1,name:var}...]
    // data points past [opcode:2][len:2], so data[0]=active_no, data[1]=count.
    if (len < 2) return;
    alootid_active_preset_   = data[0];
    alootid_selected_preset_ = data[0];  // select active preset in combo by default
    const uint8_t preset_count = data[1];
    alootid_presets_.clear();
    uint16_t read_offset_bytes = 2;
    for (uint8_t i = 0; i < preset_count && read_offset_bytes + 3 <= len; ++i) {
      AlootPreset preset;
      preset.slot_no  = data[read_offset_bytes];
      preset.autoload = data[read_offset_bytes + 1] != 0;
      const uint8_t name_len_bytes = data[read_offset_bytes + 2];
      read_offset_bytes += 3;
      if (read_offset_bytes + name_len_bytes > len) break;
      preset.name.assign(reinterpret_cast<const char*>(data + read_offset_bytes), name_len_bytes);
      read_offset_bytes += name_len_bytes;
      alootid_presets_.push_back(std::move(preset));
    }
    // Auto-fill the save input with the active preset's name so "Sauvegarder"
    // updates it directly instead of creating a new slot.
    if (alootid_active_preset_ != 0) {
      for (const auto& preset : alootid_presets_) {
        if (preset.slot_no == alootid_active_preset_) {
          std::strncpy(alootid_preset_input_, preset.name.c_str(),
                       sizeof(alootid_preset_input_) - 1);
          alootid_preset_input_[sizeof(alootid_preset_input_) - 1] = '\0';
          break;
        }
      }
    }
    // Snapshot the current list as "saved state" — the server sends this packet
    // after every save/load/delete, so the snapshot stays in sync automatically.
    alootid_saved_ids_ = aloot_ids_;
    return;
  }

  if (opcode != kOpcodeFromServer) return;
  // Layout after the [opcode:2][len:2] header: [char_id:4][count:2][{id,value}*].
  if (len < 6) return;

  const uint16_t setting_count = *reinterpret_cast<const uint16_t*>(data + 4);
  // 32-bit math: a uint16_t cast here would truncate (e.g. count=0xFFFF wraps
  // 393216 -> 0), defeating the length check and allowing an OOB read.
  const uint32_t expected_len = 6u + static_cast<uint32_t>(setting_count) * 6u;
  if (len < expected_len) {
    LogError("[MoonlightUi] ZC_BOURGEON_SETTINGS truncated: len={} count={}", len,
             setting_count);
    return;
  }

  for (uint16_t i = 0; i < setting_count; ++i) {
    const uint16_t setting_id    = *reinterpret_cast<const uint16_t*>(data + 6 + i * 6);
    const uint32_t setting_value = *reinterpret_cast<const uint32_t*>(data + 6 + i * 6 + 2);
    switch (setting_id) {
      case kSettingShowExp:
        show_exp_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] show_exp={}", show_exp_);
        break;
      case kSettingShowZeny:
        show_zeny_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] show_zeny={}", show_zeny_);
        break;
      case kSettingShowMobInfo:
        show_mob_info_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] show_mob_info={}", show_mob_info_);
        break;
      case kSettingSeparateKills:
        separate_kills_enabled_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] separate={}", separate_kills_enabled_);
        break;
      case kSettingBlockExp:
        block_exp_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] block_exp={}", block_exp_);
        break;
      case kSettingAlootRare:
        aloot_rare_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] aloot_rare={}", aloot_rare_);
        break;
      case kSettingAlootRate:
        aloot_rate_ = static_cast<int>(setting_value);
        // LogInfo("[MoonlightUi] aloot_rate={}", aloot_rate_);
        break;
      case kSettingAlootMinZenyDiv100:
        aloot_min_zeny_ = static_cast<int>(setting_value) * 100;
        // LogInfo("[MoonlightUi] aloot_pognon={}", aloot_min_zeny_);
        break;
      case kSettingAlootType:
        aloot_type_mask_ = static_cast<int>(setting_value);
        // LogInfo("[MoonlightUi] aloot_type_mask=0x{:04X}", aloot_type_mask_);
        break;
      case kSettingDiscordChat:
        discord_chat_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] discord_chat={}", discord_chat_);
        UpdateRelay();
        break;
      case kSettingShowAttackDelay:
        show_attack_delay_enabled_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] show_delay={}", show_attack_delay_enabled_);
        break;
      case kSettingShowMoveSpeed:
        show_move_speed_enabled_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] show_speed={}", show_move_speed_enabled_);
        break;
      case kSettingSellStuff:
        sell_stuff_enabled_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] sell_stuff={}", sell_stuff_enabled_);
        break;
      case kSettingSellItem:
        sell_item_enabled_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] sell_item={}", sell_item_enabled_);
        break;
      case kSettingNoAsk:
        no_ask_enabled_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] no_ask={}", no_ask_enabled_);
        break;
      case kSettingNoksMode:
        noks_mode_ = static_cast<int>(setting_value);
        // LogInfo("[MoonlightUi] noks={}", noks_mode_);
        break;
      case kSettingWings:
        wings_enabled_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] wings={}", wings_enabled_);
        break;
      case kSettingAlootMvp:
        aloot_mvp_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] aloot_mvp={}", aloot_mvp_);
        break;
      case kSettingAlootMvpRwd:
        aloot_mvp_rwd_ = (setting_value != 0);
        // LogInfo("[MoonlightUi] aloot_mvp_rwd={}", aloot_mvp_rwd_);
        break;
      case kSettingSortModeInventory:
        sort_mode_inventory_ = static_cast<int>(setting_value);
        // LogInfo("[MoonlightUi] tri_inv={}", sort_mode_inventory_);
        break;
      case kSettingSortModeCart:
        sort_mode_cart_ = static_cast<int>(setting_value);
        // LogInfo("[MoonlightUi] tri_cart={}", sort_mode_cart_);
        break;
      case kSettingSortModeStorage:
        sort_mode_storage_ = static_cast<int>(setting_value);
        // LogInfo("[MoonlightUi] tri_storage={}", sort_mode_storage_);
        break;
      case kSettingSortModeGuildStorage:
        sort_mode_guild_storage_ = static_cast<int>(setting_value);
        // LogInfo("[MoonlightUi] tri_gstorage={}", sort_mode_guild_storage_);
        break;
      case kSettingAlootId:
        if (setting_value == 0) {
          aloot_ids_.clear();
          // LogInfo("[MoonlightUi] aloot_ids cleared");
        } else {
          bool found = false;
          for (uint32_t x : aloot_ids_) if (x == setting_value) { found = true; break; }
          if (!found) aloot_ids_.push_back(setting_value);
          // LogInfo("[MoonlightUi] aloot_id added={}", setting_value);
        }
        break;
      case kSettingAlootIdRemove:
        break;
      case kSettingGroupLevel:
        // Niveau de groupe serveur (pc_get_group_level). > 0 => staff/GM ; active
        // les fonctionnalités réservées (IsStaff), sans édition manuelle du yaml.
        g_staff_level = static_cast<int>(setting_value);
        // LogInfo("[MoonlightUi] staff_level={}", g_staff_level);
        break;
      default:
        // LogInfo("[MoonlightUi] unknown setting id={} value={}", setting_id, setting_value);
        break;
    }
  }
}

// Send a setting change to the server.
// The server will echo it back in a ZC_BOURGEON_SETTINGS packet, which is how we know the change was accepted.
void MoonlightUi::SendSetting(uint16_t id, uint32_t value) {
  uint8_t buf[10];
  *reinterpret_cast<uint16_t*>(buf)     = kOpcodeToServer;
  *reinterpret_cast<uint16_t*>(buf + 2) = 10;
  *reinterpret_cast<uint16_t*>(buf + 4) = id;
  *reinterpret_cast<uint32_t*>(buf + 6) = value;
  Bourgeon::Instance().SendPacket(buf, sizeof(buf));
}

// API autolootid partagée (utilisée par le panneau de description enrichi) 
bool MoonlightUi::IsAlootId(uint32_t id) const {
  for (uint32_t v : aloot_ids_)
    if (v == id) return true;
  return false;
}

// Add/remove an item id to the autoloot list. Returns true if the list changed.
bool MoonlightUi::AddAlootId(uint32_t id) {
  if (id == 0 || aloot_ids_.size() >= 50 || IsAlootId(id)) return false;
  aloot_ids_.push_back(id);
  SendSetting(kSettingAlootId, id);
  return true;
}

// Remove an item id from the autoloot list. Returns true if the list changed.
bool MoonlightUi::RemoveAlootId(uint32_t id) {
  for (size_t k = 0; k < aloot_ids_.size(); ++k) {
    if (aloot_ids_[k] == id) {
      SendSetting(kSettingAlootIdRemove, id);
      aloot_ids_.erase(aloot_ids_.begin() + k);
      return true;
    }
  }
  return false;
}

// Return the name of an item id, or nullptr if unknown. Used by the autolootid panel.
const char* MoonlightUi::ItemName(uint32_t id) const {
  const auto it = item_names_.find(id);
  return (it != item_names_.end()) ? it->second.c_str() : nullptr;
}

// Send a preset command to the server (save/load/delete).
// The server will echo it back in a ZC_BOURGEON_PRESET_CMD packet.
void MoonlightUi::SendPresetCmd(AlootPresetCmd command, uint8_t preset_no,
                                const char* preset_name) {
  const uint16_t name_len =
      preset_name ? static_cast<uint16_t>(strnlen(preset_name, 50)) : 0;
  const uint16_t total_len = static_cast<uint16_t>(6 + name_len);
  std::vector<uint8_t> packet(total_len);
  *reinterpret_cast<uint16_t*>(packet.data())     = kOpcodePresetCmd;
  *reinterpret_cast<uint16_t*>(packet.data() + 2) = total_len;
  packet[4] = static_cast<uint8_t>(command);
  packet[5] = preset_no;
  if (name_len > 0) std::memcpy(packet.data() + 6, preset_name, name_len);
  Bourgeon::Instance().SendPacket(packet.data(), total_len);
}

// HelpMarker, WheelSliderFloat/Int, PushStyleCompact/PopStyleCompact ont été
// déplacés dans ui/ro_widgets.cc ; AlignGrid::Draw et HudReplaced dans
// ui/align_grid.cc — ce sont des widgets d'usage général, pas du panneau de
// réglages. Ce fichier les consomme via les en-têtes correspondants.

// Ouvre le panneau directement sur une section d'« Interface de jeu ». Ne dessine
// rien : pose l'état que le prochain OnRenderUI consomme (déplier la fenêtre +
// ouvrir l'en-tête + sélectionner l'entrée). Sûr à appeler pendant le rendu d'une
// AUTRE fenêtre (le bullet de barre de titre du storage, p. ex.).
void MoonlightUi::OpenInterfaceSection(int section) {
  // Borne sur kIfaceCount plutôt que sur la dernière section nommée : ajouter une
  // entrée à l'enum suffit, il n'y a plus rien à penser ici.
  if (section < 0 || section >= kIfaceCount) return;
  iface_nav_ = section;
  pending_iface_jump_ = true;
  // Fenêtre repliée : la déplier, sinon le saut serait invisible. Même chemin que
  // la restauration au login (pending_collapse_restore_ -> SetNextWindowCollapsed).
  if (ui_collapsed_) {
    ui_collapsed_ = false;
    pending_collapse_restore_ = true;
  }
  ImGui::SetWindowFocus("Moonlight-Destiny");
}

// ── ImGui panel ───────────────────────────────────────────────────────────
void MoonlightUi::OnRenderUI() {
  if (!in_game_) return;

  // Global alignment grid (shared HUD overlay). Drawn here on the background
  // list so it shows even with the bars hidden; suppressed while a full-screen
  // UI (world map) replaces the HUD, matching the bars/icons.
  if (grid_.show) grid_.Draw();

  // SPR Effect Lab : reconcile spawn + overlay au centre (foreground drawlist, indépendant
  // de la fenêtre principale). Inerte tant qu'aucun effet n'est demandé.
  spr_lab::RenderFrame();

  // Persist bars geometry once, the frame after the user finishes a drag.
  if (auto* basic_info = Bourgeon::Instance().basic_info();
      basic_info && basic_info->geometry_dirty_) {
    basic_info->geometry_dirty_ = false;
    SaveSettings();
  }

  // Same for menu-icon positions (set on drag-end in MenuIconTweaks).
  if (auto* menu_icons = Bourgeon::Instance().menu_icons();
      menu_icons && menu_icons->geometry_dirty_) {
    menu_icons->geometry_dirty_ = false;
    SaveSettings();
  }

  // Skill-bar config (set on any panel change / drag-end in SkillBarTweaks).
  if (auto* skill_bar = Bourgeon::Instance().skill_bar();
      skill_bar && skill_bar->dirty_) {
    skill_bar->dirty_ = false;
    SaveSettings();
  }

  // Persist the collapsed state of the main window (set on any collapse/expand).
  if (pending_collapse_restore_) {
    ImGui::SetNextWindowCollapsed(ui_collapsed_, ImGuiCond_Always);
    pending_collapse_restore_ = false;
  }

  // Échap a demandé le repli (fenêtre principale = dernière avant le jeu) : on force le
  // repli ce frame ; la détection is_collapsed ci-dessous met à jour ui_collapsed_ + persiste.
  if (pending_collapse_request_) {
    pending_collapse_request_ = false;
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_Always);
  }

  // Default window style (rounded corners, thin border, rounded sliders/knobs).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  // Skin RO (toggleable : BeginRoWindow retombe sur ImGui::Begin si skin off).
  ro::BeginRoWindow("Moonlight-Destiny");

  const bool is_collapsed = ImGui::IsWindowCollapsed();
  if (is_collapsed != ui_collapsed_) {
    ui_collapsed_ = is_collapsed;
    SaveSettings();
  }

  // Fenêtre principale = cible « minimiser » d'Échap, en DERNIER recours (seulement
  // dépliée, seulement s'il ne reste aucune autre fenêtre fermable) : Échap la replie
  // avant d'être rendu au jeu pour ses fenêtres natives.
  if (!is_collapsed) ro::RegisterEscapeMinimizeWindow(&pending_collapse_request_);

  if (!is_collapsed) {
    moonlight_ui::DrawRules();

    // ── DPS Meter, Doom, Roggle, Rojeweled, Jump, QSDZ ───────────────────
    DrawFunPanels();

    DrawInterfacePanel();
    // ── Graphismes (color grading post-process, SettingsTweaks plugin) ───────
    if (CollapsingHeader("Graphismes")) {
      PushStyleCompact();
      if (auto* settings_tweaks = Bourgeon::Instance().settings_tweaks())
        settings_tweaks->DrawSettings();

      if (auto* weapon_dual_sprites = Bourgeon::Instance().weapon_dual_sprites()) {
        if (ro::RoCheckbox("Sprites d'armes doubles", &weapon_dual_sprites->enabled()))
          SaveSettings();
        SameLine(); HelpMarker(
            "Affiche le sprite/l'animation PROPRE à chaque arme quand tu portes "
            "deux armes (assassin, kagerou/oboro) ou une seule arme en main "
            "gauche.\n\nOFF (défaut) : le client fond les deux armes en un sprite "
            "générique. ON : chaque arme garde son apparence d'origine.");
      }
      PopStyleCompact();
    }

    // ── Staff Tools (réservé group level serveur >= 80, cf. IsStaff) ──────────
    // Regroupe les fonctionnalités réservées au staff : affichage permanent des
    // noms d'entités + SPR Lab. Gaté PUREMENT sur le group level reçu au login
    // (setting id 26). Toute la section disparaît pour un non-staff, et l'overlay
    // des noms reste inerte (OnRenderUI vérifie IsStaff).
    if (IsStaff() && CollapsingHeader("Staff Tools")) {
      PushStyleCompact();

      SeparatorText("Noms des entités");
      if (auto* entity_names = Bourgeon::Instance().entity_names())
        entity_names->DrawSettings();

      SeparatorText("SPR Lab");
      spr_lab::DrawDebugControls();

      PopStyleCompact();
    }

    // ── Commands Settings ────────────────────────────────────────────────────
    DrawCommandsPanel();
  }
  ro::EndRoWindow();
  ImGui::PopStyleVar(4);

  DrawAlootOverlay();

  // Barre flottante de préréglages du chat principal : elle appartient à
  // ChatTweaks (couleurs, préréglages), MoonlightUi ne décide que de l'AFFICHER
  // — c'est son réglage « mainchat_preset_bar » — et de sauvegarder après coup.
  if (mainchat_preset_bar_) {
    if (auto* chat_tweaks = Bourgeon::Instance().chat_tweaks())
      if (chat_tweaks->DrawPresetBar()) SaveSettings();
  }
}
