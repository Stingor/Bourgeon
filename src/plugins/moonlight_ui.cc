#include "plugins/moonlight_ui.h"

#include "plugins/moonlight_ui/internal.h"       // panneaux extraits (dossier privé)
#include "plugins/moonlight_ui/settings_table.h"  // description des réglages persistés

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

namespace {

// Les trois helpers de couleur persistée vivent avec les moteurs de la table
// (moonlight_ui/settings_table.h) : ce sont eux qui connaissent la dualité
// « hex ARGB » / « ImU32 décimal » des yaml déjà chez les joueurs.
using moonlight_ui::HexArgb;
using moonlight_ui::ReadArgbKey;
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
    {"portrait_debug_log", SType::kBool, MLUI_FIELD(basic_info, portrait_debug_log_),
     MLUI_LITERAL(bool, false)},
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

// `with_rounding` : `rounding` n'a jamais été persisté dans les presets, et l'y
// ajouter changerait le comportement — appliquer un preset écraserait alors
// l'arrondi choisi par le joueur. Le paramètre garde aussi l'ordre des clés
// identique aux deux sites, pour que le premier yaml réécrit ne diffère pas.
void ReadSkinCfg(const YAML::Node& n, ro::RoSkinConfig& cfg,
                 const std::string& prefix, bool with_rounding) {
  cfg.title_brightness = n[prefix + "bright"].as<float>(cfg.title_brightness);
  if (with_rounding) cfg.rounding = n[prefix + "rounding"].as<float>(cfg.rounding);
  cfg.alpha = n[prefix + "alpha"].as<float>(cfg.alpha);
  for (const SkinColorField& f : kSkinColorFields) {
    const YAML::Node node = n[prefix + f.key];
    if (node) ro::PickerFromImU32(node.as<unsigned>(0), cfg.*f.member);
  }
}

void EmitSkinCfg(YAML::Emitter& out, const ro::RoSkinConfig& cfg,
                 const std::string& prefix, bool with_rounding) {
  out << YAML::Key << prefix + "bright" << YAML::Value << cfg.title_brightness;
  if (with_rounding)
    out << YAML::Key << prefix + "rounding" << YAML::Value << cfg.rounding;
  out << YAML::Key << prefix + "alpha" << YAML::Value << cfg.alpha;
  for (const SkinColorField& f : kSkinColorFields)
    out << YAML::Key << prefix + f.key << YAML::Value
        << ro::ImU32FromPicker(cfg.*f.member);
}

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
};

const moonlight_ui::SettingDesc MoonlightUiOwnSettings::kHeader[3] = {
    {"ui_collapsed", SType::kBool, MLUI_SELF(ui_collapsed_),
     MLUI_LITERAL(bool, false)},
    {"log_level", SType::kString, MLUI_SELF(log_level_),
     MLUI_LITERAL(std::string, "info")},
    {"alootid_overlay", SType::kBool, MLUI_SELF(show_alootid_overlay_),
     MLUI_LITERAL(bool, false)},
};

const moonlight_ui::SettingDesc MoonlightUiOwnSettings::kChat[5] = {
    {"mainchat_preset_bar", SType::kBool, MLUI_SELF(mainchat_preset_bar_),
     MLUI_LITERAL(bool, false)},
    {"chat_width_enabled", SType::kBool, MLUI_SELF(chat_width_enabled_),
     MLUI_LITERAL(bool, false)},
    // La largeur est BORNÉE à 320..1200 après lecture (cf. PostLoadApply) : un
    // yaml édité à la main ne doit pas pouvoir rendre le chat inutilisable.
    {"chat_width", SType::kInt, MLUI_SELF(chat_width_px_), MLUI_LITERAL(int, 800)},
    {"chat_timestamps", SType::kBool, MLUI_SELF(chat_timestamps_),
     MLUI_LITERAL(bool, false)},
    {"chat_item_icons", SType::kBool, MLUI_SELF(chat_item_icons_),
     MLUI_LITERAL(bool, true)},
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
  FindChatBgSites();
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
  if (auto* iv  = Bourgeon::Instance().inventory_viewer()) iv->imgui_enabled_ = on;
  if (auto* stg = Bourgeon::Instance().storage_tweaks())   stg->imgui_enabled_ = on;
  if (auto* sb  = Bourgeon::Instance().skill_bar())        sb->enabled_ = on;
  if (auto* tt  = Bourgeon::Instance().trade_tweaks())     tt->imgui_enabled_ = on;
}

// ── Settings persistence ──────────────────────────────────────────────────

void MoonlightUi::LoadSettings() {
  const std::string path = paths::SettingsPath();
  // Horodatage de COMPILATION, gardé volontairement (une ligne par login) : le
  // déploiement POST_BUILD est best-effort et SILENCIEUSEMENT sauté quand le jeu tient
  // ddraw.dll ouvert (cf. src/CMakeLists.txt) — le build passe au vert sans rien
  // déployer. Cette ligne dit immédiatement quelle DLL tourne réellement.
  LogInfo("[Bourgeon] build " __DATE__ " " __TIME__);
  std::ifstream f(path);
  if (!f) return;  // premier lancement : pas encore de fichier

  // Un chargement qui échoue à mi-parcours laisse la moitié des réglages aux
  // défauts. Écrire dans cet état GRAVERAIT la perte, puisque WriteSettingsFile
  // réémet tout depuis l'état courant. Le drapeau interdit donc d'écrire tant
  // qu'on n'a pas relu le fichier correctement (au prochain changement de carte).
  settings_load_failed_ = false;

  try {
    const YAML::Node root = YAML::Load(f);
    const YAML::Node ui = root["moonlight_ui"];
    if (!ui) return;

    // Seul site de lecture qui a besoin de l'ENTIER natif et pas seulement du
    // picker : la couleur est écrite telle quelle dans les instructions patchées
    // du client. D'où le ParseHex8 explicite plutôt que ReadArgbKey.
    for (ChatBgGroup& g : chat_bg_) {
      uint32_t argb = 0;
      if (!ro::ParseHex8(ui[g.yaml_key].as<std::string>(""), &argb)) continue;
      ro::PickerFromArgb(g.color, argb);
      // walk_heap = TRUE, et il le faut. L'audit affirmait qu'aucune fenêtre de
      // chat n'existe à ce moment et que le parcours du tas ne trouvait jamais
      // rien : c'est FAUX, vérifié depuis. LoadSettings n'est appelé que par
      // OnModeSwitch à l'ENTRÉE EN JEU (in_game_ && !was_in_game), donc une fois
      // le HUD construit — la fenêtre de chat principale est déjà là.
      //
      // Le patch des immédiats .text ne vaut que pour les fenêtres créées APRÈS.
      // Sans le parcours, le chat principal gardait sa couleur par défaut à
      // chaque login, en donnant l'impression que le réglage n'était pas
      // sauvegardé alors qu'il était correctement écrit et relu.
      if (!g.instrs.empty()) ApplyChatBg(g, argb, true);
    }

    moonlight_ui::ReadSettings(ui, kGroundPaintSettings);
    moonlight_ui::ReadSettings(ui, MoonlightUiOwnSettings::kHeader);
    moonlight_ui::ReadSettings(ui, kItemDescSettings);
    moonlight_ui::ReadSettings(ui, kBugReportSettings);
    moonlight_ui::ReadSettings(ui, kWeaponSpriteSettings);
    moonlight_ui::ReadSettings(ui, MoonlightUiOwnSettings::kChat);

    moonlight_ui::ReadSettings(ui, kDpsSettings);

    moonlight_ui::ReadSettings(ui, kBasicInfoSettings);
    moonlight_ui::ReadSettings(ui, kPortraitSettings);
    if (auto* eb = Bourgeon::Instance().basic_info()) {
      for (int i = 0; i < BasicInfoTweaks::kBarCount; ++i) {
        const std::string p =
            std::string("expbar_") + BasicInfoTweaks::kBarKeys[i] + "_";
        auto& b = eb->bars_[i];
        b.show = ui[p + "show"].as<bool>(true);
        b.x = ui[p + "x"].as<int>(b.x);
        b.y = ui[p + "y"].as<int>(b.y);
        b.w = ui[p + "w"].as<int>(b.w);
        b.h = ui[p + "h"].as<int>(b.h);
        ReadArgbKey(ui, p + "color", b.fill);
      }
      ReadArgbKey(ui, "expbar_bg_color", eb->bg_color_);

      // Portrait de statut : disposition par ÉLÉMENT (les scalaires sont dans
      // kPortraitSettings, lus plus haut). Effets de chapeau (.str) : rendu
      // automatique et toujours actif, aucun réglage persisté.
      for (int i = 0; i < BasicInfoTweaks::kPortCount; ++i) {
        const std::string p =
            std::string("portrait_") + BasicInfoTweaks::kPortKeys[i] + "_";
        auto& e = eb->ports_[i];
        e.show     = ui[p + "show"].as<bool>(e.show);
        e.x        = ui[p + "x"].as<int>(e.x);
        e.y        = ui[p + "y"].as<int>(e.y);
        e.w        = ui[p + "w"].as<int>(e.w);
        e.h        = ui[p + "h"].as<int>(e.h);
        e.rounding = ui[p + "rounding"].as<float>(e.rounding);
        ReadArgbKey(ui, p + "bg", e.bg);
        ReadArgbKey(ui, p + "fg", e.fg);
      }
    }

    // Global alignment grid. Reads grid_*, falling back to the legacy
    // expbar_grid_* keys so existing settings files keep working (they get
    // rewritten under the new keys on the next save).
    grid_.show = ui["grid_show"].as<bool>(ui["expbar_grid_show"].as<bool>(false));
    grid_.snap = ui["grid_snap"].as<bool>(ui["expbar_grid_snap"].as<bool>(false));
    grid_.size = ui["grid_size"].as<int>(ui["expbar_grid_size"].as<int>(32));
    if (!ReadArgbKey(ui, "grid_color", grid_.color))
      ReadArgbKey(ui, "expbar_grid_color", grid_.color);  // repli sur la clé héritée

    // STATUS window saved position (applied by StatusTweaks' msg-handler hook).
    StatusTweaks_SetSavedPos(ui["status_pos_x"].as<int>(INT_MIN),
                             ui["status_pos_y"].as<int>(INT_MIN));
    // EQUIP window saved position (applied by EquipTweaks' msg-handler hook).
    EquipTweaks_SetSavedPos(ui["equip_pos_x"].as<int>(INT_MIN),
                            ui["equip_pos_y"].as<int>(INT_MIN));
    // Generic per-window saved positions (WindowPosTweaks table: achievement,
    // bank, mail, ...). One "<key>_pos_x/y" pair each; applied on the next tick.
    for (int i = 0; i < WindowPosTweaks_Count(); ++i) {
      const std::string k = WindowPosTweaks_Key(i);
      WindowPosTweaks_SetSavedPos(i, ui[k + "_pos_x"].as<int>(INT_MIN),
                                  ui[k + "_pos_y"].as<int>(INT_MIN));
    }

    if (auto* mi = Bourgeon::Instance().menu_icons()) {
      mi->enabled_   = ui["menu_icons_enabled"].as<bool>(mi->enabled_);
      mi->edit_mode_ = ui["menu_icons_edit"].as<bool>(false);
      mi->saved_.clear();
      // Per-icon saved position/visibility under "menu_icons: { <name>: {...} }".
      // Stored here because the live icon list only exists once in-game; applied
      // later in MenuIconTweaks::BuildIconList.
      if (const YAML::Node icons = ui["menu_icons"]) {
        for (auto it = icons.begin(); it != icons.end(); ++it) {
          const std::string nm = it->first.as<std::string>("");
          if (nm.empty()) continue;
          MenuIconTweaks::IconSave s;
          s.x      = it->second["x"].as<int>(-1);
          s.y      = it->second["y"].as<int>(-1);
          s.hidden = it->second["hidden"].as<bool>(false);
          s.valid  = true;
          mi->saved_[nm] = s;
        }
      }
    }

    ro::SetFontEnabled(ui["malgun_font"].as<bool>(ro::IsFontEnabled()));
    // (« ro_skin » : clé abandonnée — le skin RO est désormais toujours actif. Une
    // ancienne valeur false dans le yaml est simplement ignorée.)
    ReadSkinCfg(ui, ro::SkinConfig(), "ro_skin_", /*with_rounding=*/true);
    auto& skin_presets = ro::SkinPresets();
    skin_presets.clear();
    if (const YAML::Node ps = ui["ro_skin_presets"]) {
      for (auto it = ps.begin(); it != ps.end(); ++it) {
        ro::SkinPreset preset;
        preset.name = (*it)["name"].as<std::string>("");
        if (preset.name.empty()) continue;
        ReadSkinCfg(*it, preset.cfg, "", /*with_rounding=*/false);
        skin_presets.push_back(std::move(preset));
      }
    }
    // Thèmes de départ si le yaml n'en portait aucun (1er lancement) : le
    // toolkit les fournit, il n'y a rien de spécifique à moonlight_ui dedans.
    ro::EnsureDefaultSkinPresets();
    moonlight_ui::ReadSettings(ui, kInventorySettings);
    if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
      // Placement libre : map nameid -> index de case (client-side, comme les
      // favoris du storage).
      if (const YAML::Node lay = ui["inventory_layout"]) {
        iv->layout_.clear();
        for (auto it = lay.begin(); it != lay.end(); ++it) {
          const uint32_t id = it->first.as<uint32_t>(0);
          const int cell = it->second.as<int>(-1);
          if (id != 0 && cell >= 0) iv->layout_[id] = cell;
        }
      }
    }
    moonlight_ui::ReadSettings(ui, kStorageSettings);
    if (auto* stg = Bourgeon::Instance().storage_tweaks()) {
      // Favoris storage (client-side, keyés par id d'item).
      if (const YAML::Node favs = ui["storage_favorites"]) {
        stg->favorites_.clear();
        for (const YAML::Node& f : favs) {
          const uint32_t id = f.as<uint32_t>(0);
          if (id != 0) stg->favorites_.insert(id);
        }
      }
    }
    moonlight_ui::ReadSettings(ui, kOptInWindowSettings);
    moonlight_ui::ReadSettings(ui, kSkillBarSettings);
    if (auto* sb = Bourgeon::Instance().skill_bar()) {
      const float legacy_scale = ui["skillbar_text_scale"].as<float>(1.0f);  // ancienne clé unique (repli)
      sb->key_scale_   = ui["skillbar_key_scale"].as<float>(legacy_scale);
      sb->count_scale_ = ui["skillbar_count_scale"].as<float>(legacy_scale);
      // 3 barres fixes (0=Onglet1, 1=Onglet2, 2=Items) : clés skillbarN_*
      for (int b = 0; b < SkillBarTweaks::kBarCount; ++b) {
        auto& bc = sb->bars_[b];
        const std::string p = "skillbar" + std::to_string(b) + "_";
        bc.visible    = ui[p + "visible"].as<bool>(bc.visible);
        bc.x          = ui[p + "x"].as<int>(bc.x);
        bc.y          = ui[p + "y"].as<int>(bc.y);
        bc.columns    = ui[p + "columns"].as<int>(bc.columns);
        bc.first_slot = ui[p + "first"].as<int>(bc.first_slot);
        bc.slot_count = ui[p + "slots"].as<int>(bc.slot_count);
        bc.icon_size  = ui[p + "size"].as<float>(bc.icon_size);
        bc.spacing    = ui[p + "spacing"].as<float>(bc.spacing);
      }
      for (int i = 0; i < SkillBarTweaks::kItemSlotMax; ++i)  // contenu persisté barre d'items (nameids)
        sb->item_slots_[i] = ui["skillbar_item" + std::to_string(i)].as<uint32_t>(sb->item_slots_[i]);
    }
    moonlight_ui::ReadSettings(ui, kSkillBarColorSettings);

    moonlight_ui::ReadSettings(ui, kStatusIconSettings);
    moonlight_ui::ReadSettings(ui, kQuestTrackerSettings);
    moonlight_ui::ReadSettings(ui, kGraphicsSettings);
    moonlight_ui::ReadSettings(ui, kEntityNameSettings);

    chat_bg_presets_.clear();
    if (const YAML::Node presets = ui["chat_bg_presets"]) {
      for (const YAML::Node& p : presets) {
        const std::string name = p["name"].as<std::string>("");
        uint32_t argb = 0;
        if (name.empty() || !ro::ParseHex8(p["color"].as<std::string>(""), &argb)) continue;
        chat_bg_presets_.push_back({name, argb});
      }
    }

    // Presets d'équipement (loadouts nommés, par CID) — possédés par CharacterSheet.
    if (auto* cse = Bourgeon::Instance().character_sheet()) {
      auto& presets = cse->equip_presets();
      presets.clear();
      if (const YAML::Node eps = ui["equip_presets"]) {
        for (const YAML::Node& pn : eps) {
          EquipPreset ep;
          ep.cid  = pn["cid"].as<uint32_t>(0);
          ep.name = pn["name"].as<std::string>("");
          if (ep.name.empty()) continue;
          ep.hotkeyVk = pn["hkvk"].as<int>(0);
          ep.hkCtrl   = pn["hkc"].as<bool>(false);
          ep.hkAlt    = pn["hka"].as<bool>(false);
          ep.hkShift  = pn["hks"].as<bool>(false);
          if (const YAML::Node items = pn["items"]) {
            for (const YAML::Node& it : items) {
              EquipPresetItem pi;
              pi.nameid   = it["id"].as<uint32_t>(0);
              pi.refine   = it["refine"].as<int>(0);
              pi.grade    = it["grade"].as<int>(0);
              pi.leftHand = it["left"].as<bool>(false);
              if (const YAML::Node cards = it["cards"])
                for (int c = 0; c < 4 && c < static_cast<int>(cards.size()); ++c)
                  pi.cards[c] = cards[c].as<uint32_t>(0);
              ep.items.push_back(pi);
            }
          }
          presets.push_back(std::move(ep));
        }
      }
    }

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
  // Bornage : un yaml édité à la main ne doit pas rendre le chat inutilisable.
  if (chat_width_px_ < 320)  chat_width_px_ = 320;
  if (chat_width_px_ > 1200) chat_width_px_ = 1200;
  chat::SetCustomWidth(chat_width_enabled_, chat_width_px_);
  chat::SetTimestamps(chat_timestamps_);
  chat::SetItemIcons(chat_item_icons_);
  LogConsole::instance().SetLevel(log_level_);
  apply_collapse_ = true;

  // « Tout-ImGui ou tout-natif » : ces 4 fenêtres (inventaire/entrepôt/barres/
  // échange) s'activent ensemble. Un yaml antérieur au regroupement pouvait être
  // mixé — on réconcilie en OU (au moins une moderne => toutes modernes ; tout
  // natif sinon), puis les cases restent synchronisées à l'exécution.
  auto* inventory = Bourgeon::Instance().inventory_viewer();
  auto* storage   = Bourgeon::Instance().storage_tweaks();
  auto* skill_bar = Bourgeon::Instance().skill_bar();
  auto* trade     = Bourgeon::Instance().trade_tweaks();
  SetModernInterface((inventory && inventory->imgui_enabled_) ||
                     (storage && storage->imgui_enabled_) ||
                     (skill_bar && skill_bar->enabled_) ||
                     (trade && trade->imgui_enabled_));

  if (auto* si = Bourgeon::Instance().status_icons()) si->MarkDirty();
  if (auto* st = Bourgeon::Instance().settings_tweaks())
    st->Apply();  // pousse le post-traitement vers la couche d3d9
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

  // Ces deux-là gardent une chaîne pré-calculée : elles ont un repli littéral à
  // écrire quand le plugin propriétaire est absent, que WriteArgbKey ne sait pas
  // exprimer (il part forcément d'un picker).
  auto* eb = Bourgeon::Instance().basic_info();
  std::string eb_bg_col = "B30D0D12";
  if (eb) eb_bg_col = HexArgb(eb->bg_color_);
  // Couleur de la grille d'alignement globale (elle appartient à MoonlightUi,
  // pas à basic_info).
  const std::string grid_col = HexArgb(grid_.color);

  YAML::Emitter out;
  out << YAML::BeginMap
      << YAML::Key << "moonlight_ui"
      << YAML::Value << YAML::BeginMap;
  for (const ChatBgGroup& g : chat_bg_) WriteArgbKey(out, g.yaml_key, g.color);
  moonlight_ui::WriteSettings(out, kGroundPaintSettings);
  moonlight_ui::WriteSettings(out, MoonlightUiOwnSettings::kHeader);
  moonlight_ui::WriteSettings(out, kItemDescSettings);
  moonlight_ui::WriteSettings(out, kBugReportSettings);
  moonlight_ui::WriteSettings(out, kWeaponSpriteSettings);
  moonlight_ui::WriteSettings(out, MoonlightUiOwnSettings::kChat);
  moonlight_ui::WriteSettings(out, kDpsSettings);

  // Barres EXP/HP/SP (BasicInfoTweaks)
  moonlight_ui::WriteSettings(out, kBasicInfoSettings);
  out << YAML::Key << "expbar_bg_color" << YAML::Value << eb_bg_col
      << YAML::Key << "grid_show"  << YAML::Value << grid_.show
      << YAML::Key << "grid_snap"  << YAML::Value << grid_.snap
      << YAML::Key << "grid_size"  << YAML::Value << grid_.size
      << YAML::Key << "grid_color" << YAML::Value << grid_col
      << YAML::Key << "status_pos_x" << YAML::Value << StatusTweaks_SavedX()
      << YAML::Key << "status_pos_y" << YAML::Value << StatusTweaks_SavedY()
      << YAML::Key << "equip_pos_x" << YAML::Value << EquipTweaks_SavedX()
      << YAML::Key << "equip_pos_y" << YAML::Value << EquipTweaks_SavedY();
  // Generic per-window saved positions (WindowPosTweaks table).
  for (int i = 0; i < WindowPosTweaks_Count(); ++i) {
    const std::string k = WindowPosTweaks_Key(i);
    out << YAML::Key << (k + "_pos_x") << YAML::Value << WindowPosTweaks_X(i)
        << YAML::Key << (k + "_pos_y") << YAML::Value << WindowPosTweaks_Y(i);
  }
  if (eb) {
    for (int i = 0; i < BasicInfoTweaks::kBarCount; ++i) {
      const std::string p =
          std::string("expbar_") + BasicInfoTweaks::kBarKeys[i] + "_";
      const auto& b = eb->bars_[i];
      out << YAML::Key << (p + "show")  << YAML::Value << b.show
          << YAML::Key << (p + "x")     << YAML::Value << b.x
          << YAML::Key << (p + "y")     << YAML::Value << b.y
          << YAML::Key << (p + "w")     << YAML::Value << b.w
          << YAML::Key << (p + "h")     << YAML::Value << b.h;
      WriteArgbKey(out, p + "color", b.fill);
    }
  }

  // Portrait de statut (même plugin) : scalaires puis disposition par élément.
  moonlight_ui::WriteSettings(out, kPortraitSettings);
  if (eb) {
    for (int i = 0; i < BasicInfoTweaks::kPortCount; ++i) {
      const std::string p =
          std::string("portrait_") + BasicInfoTweaks::kPortKeys[i] + "_";
      const auto& e = eb->ports_[i];
      out << YAML::Key << (p + "show")     << YAML::Value << e.show
          << YAML::Key << (p + "x")        << YAML::Value << e.x
          << YAML::Key << (p + "y")        << YAML::Value << e.y
          << YAML::Key << (p + "w")        << YAML::Value << e.w
          << YAML::Key << (p + "h")        << YAML::Value << e.h
          << YAML::Key << (p + "rounding") << YAML::Value << e.rounding;
      WriteArgbKey(out, p + "bg", e.bg);
      WriteArgbKey(out, p + "fg", e.fg);
    }
  }

  {
    auto* mi = Bourgeon::Instance().menu_icons();
    out << YAML::Key << "menu_icons_enabled"
        << YAML::Value << (mi ? mi->enabled_ : false);
    out << YAML::Key << "menu_icons_edit"
        << YAML::Value << (mi ? mi->edit_mode_ : false);
    out << YAML::Key << "menu_icons" << YAML::Value << YAML::BeginMap;
    if (mi) {
      for (const auto& kv : mi->saved_) {
        out << YAML::Key << kv.first << YAML::Value << YAML::BeginMap
            << YAML::Key << "x"      << YAML::Value << kv.second.x
            << YAML::Key << "y"      << YAML::Value << kv.second.y
            << YAML::Key << "hidden" << YAML::Value << kv.second.hidden
            << YAML::EndMap;
      }
    }
    out << YAML::EndMap;
  }

  moonlight_ui::WriteSettings(out, kStatusIconSettings);

  moonlight_ui::WriteSettings(out, kQuestTrackerSettings);

  moonlight_ui::WriteSettings(out, kGraphicsSettings);

  moonlight_ui::WriteSettings(out, kEntityNameSettings);

  {
    out << YAML::Key << "malgun_font" << YAML::Value << ro::IsFontEnabled();
    EmitSkinCfg(out, ro::SkinConfig(), "ro_skin_", /*with_rounding=*/true);
    out << YAML::Key << "ro_skin_presets" << YAML::Value << YAML::BeginSeq;
    for (const auto& preset : ro::SkinPresets()) {
      out << YAML::BeginMap << YAML::Key << "name" << YAML::Value << preset.name;
      EmitSkinCfg(out, preset.cfg, "", /*with_rounding=*/false);
      out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    moonlight_ui::WriteSettings(out, kInventorySettings);
    auto* iv = Bourgeon::Instance().inventory_viewer();
    // Placement libre : nameid -> case. Trié pour un yaml stable (pas de diff parasite).
    out << YAML::Key << "inventory_layout" << YAML::Value << YAML::Flow << YAML::BeginMap;
    if (iv) {
      std::vector<std::pair<uint32_t, int>> lay(iv->layout_.begin(), iv->layout_.end());
      std::sort(lay.begin(), lay.end());
      for (const auto& e : lay) out << YAML::Key << e.first << YAML::Value << e.second;
    }
    out << YAML::EndMap;
    moonlight_ui::WriteSettings(out, kStorageSettings);
    auto* stg = Bourgeon::Instance().storage_tweaks();
    // Favoris storage (ids d'items, triés pour un yaml stable = pas de diff parasite).
    out << YAML::Key << "storage_favorites" << YAML::Value << YAML::Flow << YAML::BeginSeq;
    if (stg) {
      std::vector<uint32_t> favs(stg->favorites_.begin(), stg->favorites_.end());
      std::sort(favs.begin(), favs.end());
      for (uint32_t id : favs) out << id;
    }
    out << YAML::EndSeq;
    moonlight_ui::WriteSettings(out, kOptInWindowSettings);
  }

  {
    auto* sb = Bourgeon::Instance().skill_bar();
    moonlight_ui::WriteSettings(out, kSkillBarSettings);
    out << YAML::Key << "skillbar_key_scale" << YAML::Value << (sb ? sb->key_scale_ : 1.0f)
        << YAML::Key << "skillbar_count_scale" << YAML::Value << (sb ? sb->count_scale_ : 1.0f);
    if (sb) {
      // 3 barres fixes (0=Onglet1, 1=Onglet2, 2=Items)
      for (int b = 0; b < SkillBarTweaks::kBarCount; ++b) {
        const auto& bc = sb->bars_[b];
        const std::string p = "skillbar" + std::to_string(b) + "_";
        out << YAML::Key << (p + "visible") << YAML::Value << bc.visible
            << YAML::Key << (p + "x")       << YAML::Value << bc.x
            << YAML::Key << (p + "y")       << YAML::Value << bc.y
            << YAML::Key << (p + "columns") << YAML::Value << bc.columns
            << YAML::Key << (p + "first")   << YAML::Value << bc.first_slot
            << YAML::Key << (p + "slots")   << YAML::Value << bc.slot_count
            << YAML::Key << (p + "size")    << YAML::Value << bc.icon_size
            << YAML::Key << (p + "spacing") << YAML::Value << bc.spacing;
      }
      sb->SnapshotItemSlots();  // capture le contenu live de la barre d'items -> yaml (persistance client)
      for (int i = 0; i < SkillBarTweaks::kItemSlotMax; ++i)
        out << YAML::Key << ("skillbar_item" + std::to_string(i)) << YAML::Value << sb->item_slots_[i];
    }
    moonlight_ui::WriteSettings(out, kSkillBarColorSettings);
  }

  out << YAML::Key << "chat_bg_presets" << YAML::Value << YAML::BeginSeq;
  for (const auto& p : chat_bg_presets_) {
    // Le preset porte déjà l'ARGB natif : pas de picker à convertir, on formate.
    char pbuf[9];
    std::snprintf(pbuf, sizeof(pbuf), "%08X", p.argb);
    out << YAML::BeginMap
        << YAML::Key << "name"  << YAML::Value << p.name
        << YAML::Key << "color" << YAML::Value << pbuf
        << YAML::EndMap;
  }
  out << YAML::EndSeq;

  // Presets d'équipement (loadouts par CID) — possédés par CharacterSheet.
  out << YAML::Key << "equip_presets" << YAML::Value << YAML::BeginSeq;
  if (auto* cse = Bourgeon::Instance().character_sheet()) {
    for (const auto& ep : cse->equip_presets()) {
      out << YAML::BeginMap
          << YAML::Key << "cid"  << YAML::Value << ep.cid
          << YAML::Key << "name" << YAML::Value << ep.name
          << YAML::Key << "hkvk" << YAML::Value << ep.hotkeyVk
          << YAML::Key << "hkc"  << YAML::Value << ep.hkCtrl
          << YAML::Key << "hka"  << YAML::Value << ep.hkAlt
          << YAML::Key << "hks"  << YAML::Value << ep.hkShift
          << YAML::Key << "items" << YAML::Value << YAML::BeginSeq;
      for (const auto& pi : ep.items) {
        out << YAML::BeginMap
            << YAML::Key << "id"     << YAML::Value << pi.nameid
            << YAML::Key << "refine" << YAML::Value << pi.refine
            << YAML::Key << "grade"  << YAML::Value << pi.grade
            << YAML::Key << "left"   << YAML::Value << pi.leftHand
            << YAML::Key << "cards"  << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (int c = 0; c < 4; ++c) out << pi.cards[c];
        out << YAML::EndSeq << YAML::EndMap;
      }
      out << YAML::EndSeq << YAML::EndMap;
    }
  }
  out       << YAML::EndSeq
      << YAML::EndMap
      << YAML::EndMap;

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
    relay->set_chat_active(discord_chat_ && in_gonryun_);
  }
}

// Called by ModeMgr::Switch (and OnUpdateHook for in_game_ tracking).
void MoonlightUi::OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) {
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
    constexpr size_t kDiscordMapLen = sizeof(kDiscordMap) - 1;
    in_gonryun_ = in_game_ && name_len >= kDiscordMapLen &&
                  (strncmp(map_name, kDiscordMap, kDiscordMapLen) == 0);
    // LogInfo("[MoonlightUi] map move -> '{}' in_gonryun={}",
            // std::string(map_name, strnlen(map_name, len)), in_gonryun_);
    UpdateRelay();
    return;
  }

  if (opcode == kOpcodePresetList) {
    // ZC_BOURGEON_PRESET_LIST: [active_no:1][count:1][{no:1,autoload:1,namelen:1,name:var}...]
    // data points past [opcode:2][len:2], so data[0]=active_no, data[1]=count.
    if (len < 2) return;
    alootid_active_preset_   = data[0];
    alootid_selected_preset_ = data[0];  // select active preset in combo by default
    const uint8_t count = data[1];
    alootid_presets_.clear();
    uint16_t off = 2;
    for (uint8_t i = 0; i < count && off + 3 <= len; ++i) {
      AlootPreset p;
      p.no       = data[off];
      p.autoload = data[off + 1] != 0;
      const uint8_t namelen = data[off + 2];
      off += 3;
      if (off + namelen > len) break;
      p.name.assign(reinterpret_cast<const char*>(data + off), namelen);
      off += namelen;
      alootid_presets_.push_back(std::move(p));
    }
    // Auto-fill the save input with the active preset's name so "Sauvegarder"
    // updates it directly instead of creating a new slot.
    if (alootid_active_preset_ != 0) {
      for (const auto& p : alootid_presets_) {
        if (p.no == alootid_active_preset_) {
          std::strncpy(alootid_preset_input_, p.name.c_str(),
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

  const uint16_t count = *reinterpret_cast<const uint16_t*>(data + 4);
  // 32-bit math: a uint16_t cast here would truncate (e.g. count=0xFFFF wraps
  // 393216 -> 0), defeating the length check and allowing an OOB read.
  const uint32_t expected_len = 6u + static_cast<uint32_t>(count) * 6u;
  if (len < expected_len) {
    LogError("[MoonlightUi] ZC_BOURGEON_SETTINGS truncated: len={} count={}", len, count);
    return;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const uint16_t id    = *reinterpret_cast<const uint16_t*>(data + 6 + i * 6);
    const uint32_t value = *reinterpret_cast<const uint32_t*>(data + 6 + i * 6 + 2);
    switch (id) {
      case kSettingShowExp:
        show_exp_ = (value != 0);
        // LogInfo("[MoonlightUi] show_exp={}", show_exp_);
        break;
      case kSettingShowZeny:
        show_zeny_ = (value != 0);
        // LogInfo("[MoonlightUi] show_zeny={}", show_zeny_);
        break;
      case kSettingShowMobInfo:
        show_mob_info_ = (value != 0);
        // LogInfo("[MoonlightUi] show_mob_info={}", show_mob_info_);
        break;
      case kSettingSeparate:
        separate_ = (value != 0);
        // LogInfo("[MoonlightUi] separate={}", separate_);
        break;
      case kSettingBlockExp:
        block_exp_ = (value != 0);
        // LogInfo("[MoonlightUi] block_exp={}", block_exp_);
        break;
      case kSettingAlootRare:
        aloot_rare_ = (value != 0);
        // LogInfo("[MoonlightUi] aloot_rare={}", aloot_rare_);
        break;
      case kSettingAlootRate:
        aloot_rate_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] aloot_rate={}", aloot_rate_);
        break;
      case kSettingAlootPognon:
        aloot_pognon_ = static_cast<int>(value) * 100;
        // LogInfo("[MoonlightUi] aloot_pognon={}", aloot_pognon_);
        break;
      case kSettingAlootType:
        aloot_type_mask_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] aloot_type_mask=0x{:04X}", aloot_type_mask_);
        break;
      case kSettingDiscordChat:
        discord_chat_ = (value != 0);
        // LogInfo("[MoonlightUi] discord_chat={}", discord_chat_);
        UpdateRelay();
        break;
      case kSettingShowDelay:
        show_delay_ = (value != 0);
        // LogInfo("[MoonlightUi] show_delay={}", show_delay_);
        break;
      case kSettingShowSpeed:
        show_speed_ = (value != 0);
        // LogInfo("[MoonlightUi] show_speed={}", show_speed_);
        break;
      case kSettingSellStuff:
        sell_stuff_ = (value != 0);
        // LogInfo("[MoonlightUi] sell_stuff={}", sell_stuff_);
        break;
      case kSettingSellItem:
        sell_item_ = (value != 0);
        // LogInfo("[MoonlightUi] sell_item={}", sell_item_);
        break;
      case kSettingNoAsk:
        no_ask_ = (value != 0);
        // LogInfo("[MoonlightUi] no_ask={}", no_ask_);
        break;
      case kSettingNoks:
        noks_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] noks={}", noks_);
        break;
      case kSettingWings:
        wings_ = (value != 0);
        // LogInfo("[MoonlightUi] wings={}", wings_);
        break;
      case kSettingAlootMvp:
        aloot_mvp_ = (value != 0);
        // LogInfo("[MoonlightUi] aloot_mvp={}", aloot_mvp_);
        break;
      case kSettingAlootMvpRwd:
        aloot_mvp_rwd_ = (value != 0);
        // LogInfo("[MoonlightUi] aloot_mvp_rwd={}", aloot_mvp_rwd_);
        break;
      case kSettingTriInv:
        tri_inv_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] tri_inv={}", tri_inv_);
        break;
      case kSettingTriCart:
        tri_cart_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] tri_cart={}", tri_cart_);
        break;
      case kSettingTriStorage:
        tri_storage_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] tri_storage={}", tri_storage_);
        break;
      case kSettingTriGstorage:
        tri_gstorage_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] tri_gstorage={}", tri_gstorage_);
        break;
      case kSettingAlootId:
        if (value == 0) {
          aloot_ids_.clear();
          // LogInfo("[MoonlightUi] aloot_ids cleared");
        } else {
          bool found = false;
          for (uint32_t x : aloot_ids_) if (x == value) { found = true; break; }
          if (!found) aloot_ids_.push_back(value);
          // LogInfo("[MoonlightUi] aloot_id added={}", value);
        }
        break;
      case kSettingAlootIdRemove:
        break;
      case kSettingStaff:
        // Niveau de groupe serveur (pc_get_group_level). > 0 => staff/GM ; active
        // les fonctionnalités réservées (IsStaff), sans édition manuelle du yaml.
        g_staff_level = static_cast<int>(value);
        // LogInfo("[MoonlightUi] staff_level={}", g_staff_level);
        break;
      default:
        // LogInfo("[MoonlightUi] unknown setting id={} value={}", id, value);
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
void MoonlightUi::SendPresetCmd(uint8_t cmd, uint8_t no, const char* name) {
  const uint16_t namelen = name ? static_cast<uint16_t>(strnlen(name, 50)) : 0;
  const uint16_t total   = static_cast<uint16_t>(6 + namelen);
  std::vector<uint8_t> buf(total);
  *reinterpret_cast<uint16_t*>(buf.data())     = kOpcodePresetCmd;
  *reinterpret_cast<uint16_t*>(buf.data() + 2) = total;
  buf[4] = cmd;
  buf[5] = no;
  if (namelen > 0) std::memcpy(buf.data() + 6, name, namelen);
  Bourgeon::Instance().SendPacket(buf.data(), total);
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
  iface_jump_ = true;
  // Fenêtre repliée : la déplier, sinon le saut serait invisible. Même chemin que
  // la restauration au login (apply_collapse_ -> SetNextWindowCollapsed).
  if (ui_collapsed_) {
    ui_collapsed_ = false;
    apply_collapse_ = true;
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
  if (auto* eb = Bourgeon::Instance().basic_info(); eb && eb->geometry_dirty_) {
    eb->geometry_dirty_ = false;
    SaveSettings();
  }

  // Same for menu-icon positions (set on drag-end in MenuIconTweaks).
  if (auto* mi = Bourgeon::Instance().menu_icons(); mi && mi->geometry_dirty_) {
    mi->geometry_dirty_ = false;
    SaveSettings();
  }

  // Skill-bar config (set on any panel change / drag-end in SkillBarTweaks).
  if (auto* sb = Bourgeon::Instance().skill_bar(); sb && sb->dirty_) {
    sb->dirty_ = false;
    SaveSettings();
  }

  // Persist the collapsed state of the main window (set on any collapse/expand).
  if (apply_collapse_) {
    ImGui::SetNextWindowCollapsed(ui_collapsed_, ImGuiCond_Always);
    apply_collapse_ = false;
  }

  // Échap a demandé le repli (fenêtre principale = dernière avant le jeu) : on force le
  // repli ce frame ; la détection is_collapsed ci-dessous met à jour ui_collapsed_ + persiste.
  if (collapse_requested_) {
    collapse_requested_ = false;
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
  if (!is_collapsed) ro::RegisterEscapeMinimizeWindow(&collapse_requested_);

  if (!is_collapsed) {
    moonlight_ui::DrawRules();

    // ── DPS Meter, Doom, Roggle, Rojeweled, Jump, QSDZ ───────────────────
    DrawFunPanels();

    DrawInterfacePanel();
    // ── Graphismes (color grading post-process, SettingsTweaks plugin) ───────
    if (CollapsingHeader("Graphismes")) {
      PushStyleCompact();
      if (auto* st = Bourgeon::Instance().settings_tweaks())
        st->DrawSettings();

      if (auto* wds = Bourgeon::Instance().weapon_dual_sprites()) {
        if (ro::RoCheckbox("Sprites d'armes doubles", &wds->enabled()))
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
      if (auto* en = Bourgeon::Instance().entity_names())
        en->DrawSettings();

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

  // ── Main-chat quick preset switcher (compact, draggable, resizable) ────────
  if (mainchat_preset_bar_ && chat_bg_found_) {
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize(ImVec2(80.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(40.0f, 1.0f), ImVec2(8000.0f, 8000.0f));
    // Match the main Moonlight-Destiny window's frame/grab/window rounding.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    // Lower the per-window minimum size (default 32x32) so this bar can be made
    // as thin as a single preset row.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(40.0f, 1.0f));
    // No title bar (minimalist). Still draggable from the body and resizable.
    if (ImGui::Begin("Chat presets", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoNav)) {
      PushStyleCompact();
      ChatBgGroup& g = chat_bg_[kChatBgMain];
      if (chat_bg_presets_.empty()) {
        ImGui::TextDisabled("Aucun préréglage.");
        ImGui::TextDisabled("Ajoute-en depuis le");
        ImGui::TextDisabled("sélecteur du chat.");
      } else {
        for (int i = 0; i < static_cast<int>(chat_bg_presets_.size()); ++i) {
          const auto& p = chat_bg_presets_[i];
          const ImVec4 col = ro::ImVec4FromArgb(p.argb);
          ImGui::PushID(i);
          if (ImGui::ColorButton("##sw", col,
                                 ImGuiColorEditFlags_AlphaPreview |
                                 ImGuiColorEditFlags_NoTooltip,
                                 ImVec2(14, 14))) {
            ro::PickerFromArgb(g.color, p.argb);
            ApplyChatBg(g, p.argb, true);
            SaveSettings();
          }
          SameLine();
          TextUnformatted(p.name.c_str());
          ImGui::PopID();
          SameLine();
        }
      }
      PopStyleCompact();
    }
    ImGui::End();
    ImGui::PopStyleVar(6);
    // Closing is done by un-ticking the "Preset bar" checkbox (no title-bar [x]).
  }
}
