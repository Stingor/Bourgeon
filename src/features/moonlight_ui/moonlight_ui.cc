#include "features/moonlight_ui/moonlight_ui.h"

#include "features/moonlight_ui/internal.h"       // panneaux extraits (dossier privé)
#include "features/moonlight_ui/settings_containers.h"  // collections persistées
#include "features/moonlight_ui/settings_table.h"       // description des réglages persistés

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
#include "features/windows/item_desc_window.h"
#include "ui/color_codec.h"
#include "ui/ro_imgui.h"
#include "ui/skin_panel.h"
#include "features/patches/chat.h"
#include "features/systems/discord_relay.h"
#include "features/overlays/basic_info.h"
#include "features/overlays/dps_meter.h"
#include "features/overlays/menu_icons.h"
#include "features/overlays/status_icon_bar.h"
#include "features/overlays/quest_tracker.h"
#include "features/fx/screen_fx.h"
#include "features/fx/zone_recorder.h"
#include "features/overlays/chat_balloon.h"
#include "features/overlays/entity_names.h"
#include "features/overlays/skill_bar.h"
#include "features/windows/storage_window.h"
#include "features/windows/inventory_viewer.h"
#include "features/windows/bank_window.h"
#include "features/windows/cart_viewer.h"
#include "features/windows/cashshop_window.h"
#include "features/windows/npc_shop_window.h"
#include "features/windows/vending_window.h"
#include "features/windows/make_item_window.h"
#include "features/windows/entity_context_menu.h"
#include "features/windows/monster_info_window.h"
#include "features/windows/weapon_refine_window.h"
#include "features/windows/trade_window.h"
#include "features/windows/chat_window.h"
#include "features/windows/rodex_window.h"
#include "features/windows/npc_dialog_window.h"
#include "features/systems/bug_report.h"
#include "features/windows/character_sheet.h"
#include "features/windows/craft_atlas.h"
#include "features/overlays/login_parade.h"
#include "features/minigames/doom.h"
#include "features/minigames/roggle.h"
#include "features/minigames/rojeweled.h"
#include "features/gameplay/keyboard_move.h"
#include "features/gameplay/player_jump.h"
#include "features/gameplay/quick_cast.h"
#include "features/patches/status_tweaks.h"
#include "features/patches/equip_tweaks.h"
#include "features/patches/window_pos_tweaks.h"
#include "features/fx/weapon_dual_sprites.h"
#include "features/fx/spr_effect_lab.h"
#include "features/fx/ground_paint.h"
#include "ragnarok/ui_window_mgr.h"
#include "ragnarok/uiwnd.h"
#include "utils/game_paths.h"
#include "spdlog/fmt/fmt.h"
#include "utils/byte_pattern.h"
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// Miroir persistable du réglage « glyphes coréens ». La valeur qui COMPTE est
// celle que ro::KoreanGlyphsWanted() lit dans le yaml à l'init — bien avant que
// ce fichier ne soit chargé. Celui-ci ne sert qu'à l'écrire et à l'afficher.
bool g_korean_glyphs = false;

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

// Bulle de chat au-dessus des têtes (remplacement du UITransBalloonText natif).
// Mêmes contraintes que ci-dessus : champs privés, défauts littéraux.
const moonlight_ui::SettingDesc kChatBalloonSettings[] = {
    // Pas de clé « balloon_enabled » : l'activation suit `chatwnd_imgui`, la
    // bulle étant une conséquence de la chatbox moderne et non une option à
    // côté (cf. le commentaire de ChatBalloon::Active).
    {"balloon_self",       SType::kBool,  MLUI_FIELD(chat_balloon, show_self()),
     MLUI_LITERAL(bool, true)},
    {"balloon_fade",       SType::kBool,  MLUI_FIELD(chat_balloon, fade()),
     MLUI_LITERAL(bool, true)},
    {"balloon_nativelife", SType::kBool,  MLUI_FIELD(chat_balloon, follow_native_life()),
     MLUI_LITERAL(bool, false)},
    {"balloon_baselife",   SType::kInt,   MLUI_FIELD(chat_balloon, base_life_ms()),
     MLUI_LITERAL(int, 5000)},
    {"balloon_perchar",    SType::kInt,   MLUI_FIELD(chat_balloon, per_char_ms()),
     MLUI_LITERAL(int, 45)},
    {"balloon_maxlife",    SType::kInt,   MLUI_FIELD(chat_balloon, max_life_ms()),
     MLUI_LITERAL(int, 12000)},
    {"balloon_yoffset",    SType::kInt,   MLUI_FIELD(chat_balloon, y_offset()),
     MLUI_LITERAL(int, 0)},
    {"balloon_fontscale",  SType::kFloat, MLUI_FIELD(chat_balloon, font_scale()),
     MLUI_LITERAL(float, 1.0f)},
    {"balloon_maxwidth",   SType::kFloat, MLUI_FIELD(chat_balloon, max_width_ratio()),
     MLUI_LITERAL(float, 0.28f)},
    {"balloon_opacity",    SType::kFloat, MLUI_FIELD(chat_balloon, opacity()),
     MLUI_LITERAL(float, 1.0f)},
};

// Post-traitement D3D9 + réglages graphiques divers (ScreenFx). Les 13
// premières vivent dans la structure fx() ; les 9 suivantes sont des accesseurs
// sur des membres privés, d'où les littéraux.
#define POSTFX(member) \
  MLUI_FIELD(screen_fx, fx().member), MLUI_DEFAULT(D3D9PostFx, member)
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
    {"fps_overlay",       SType::kBool,  MLUI_FIELD(screen_fx, fps_overlay()),
     MLUI_LITERAL(bool, false)},
    {"cam_zoom_enabled",  SType::kBool,  MLUI_FIELD(screen_fx, zoom_enabled()),
     MLUI_LITERAL(bool, false)},
    {"cam_zoom_factor",   SType::kFloat, MLUI_FIELD(screen_fx, zoom_factor()),
     MLUI_LITERAL(float, 1.0f)},
    {"cam_zoom_speed",    SType::kFloat, MLUI_FIELD(screen_fx, zoom_speed()),
     MLUI_LITERAL(float, 1.0f)},
    {"tex_filter",        SType::kInt,   MLUI_FIELD(screen_fx, tex_filter()),
     MLUI_LITERAL(int, 0)},
    // INT_MIN = « aucune position mémorisée » : la fenêtre garde son placement natif.
    {"game_option_pos_x", SType::kInt,   MLUI_FIELD(screen_fx, gopt_x()),
     MLUI_LITERAL(int, INT_MIN)},
    {"game_option_pos_y", SType::kInt,   MLUI_FIELD(screen_fx, gopt_y()),
     MLUI_LITERAL(int, INT_MIN)},
    {"esc_option_pos_x",  SType::kInt,   MLUI_FIELD(screen_fx, esc_x()),
     MLUI_LITERAL(int, INT_MIN)},
    {"esc_option_pos_y",  SType::kInt,   MLUI_FIELD(screen_fx, esc_y()),
     MLUI_LITERAL(int, INT_MIN)},
};
#undef POSTFX

// Enregistreur de zone -> GIF (staff). La ZONE est persistée en pixels écran :
// c'est un cadrage choisi une fois pour un tutoriel, qu'on veut retrouver
// identique le lendemain. Elle est rebornée à la résolution courante au moment de
// la capture (cf. D3D9_GrabBackbufferRegion), donc une fenêtre redimensionnée
// entre deux sessions ne casse rien.
const moonlight_ui::SettingDesc kZoneRecorderSettings[] = {
    {"zonerec_x",          SType::kInt,  MLUI_FIELD(zone_recorder, zone_x()),
     MLUI_LITERAL(int, 0)},
    {"zonerec_y",          SType::kInt,  MLUI_FIELD(zone_recorder, zone_y()),
     MLUI_LITERAL(int, 0)},
    {"zonerec_w",          SType::kInt,  MLUI_FIELD(zone_recorder, zone_w()),
     MLUI_LITERAL(int, 0)},
    {"zonerec_h",          SType::kInt,  MLUI_FIELD(zone_recorder, zone_h()),
     MLUI_LITERAL(int, 0)},
    {"zonerec_fps",        SType::kInt,  MLUI_FIELD(zone_recorder, fps()),
     MLUI_LITERAL(int, 10)},
    {"zonerec_seconds",    SType::kInt,  MLUI_FIELD(zone_recorder, duration_s()),
     MLUI_LITERAL(int, 6)},
    {"zonerec_max_width",  SType::kInt,  MLUI_FIELD(zone_recorder, max_width()),
     MLUI_LITERAL(int, 640)},
    {"zonerec_start_delay",SType::kInt,  MLUI_FIELD(zone_recorder, start_delay_s()),
     MLUI_LITERAL(int, 3)},
    {"zonerec_key_vk",     SType::kInt,  MLUI_FIELD(zone_recorder, key_vk()),
     MLUI_LITERAL(int, 0)},
    {"zonerec_key_ctrl",   SType::kBool, MLUI_FIELD(zone_recorder, key_ctrl()),
     MLUI_LITERAL(bool, false)},
    {"zonerec_key_alt",    SType::kBool, MLUI_FIELD(zone_recorder, key_alt()),
     MLUI_LITERAL(bool, false)},
    {"zonerec_key_shift",  SType::kBool, MLUI_FIELD(zone_recorder, key_shift()),
     MLUI_LITERAL(bool, false)},
    // Seconde touche : rouvre le TRACÉ de la zone en jeu, sans ce panneau.
    {"zonerec_sel_key_vk",    SType::kInt,  MLUI_FIELD(zone_recorder, sel_key_vk()),
     MLUI_LITERAL(int, 0)},
    {"zonerec_sel_key_ctrl",  SType::kBool, MLUI_FIELD(zone_recorder, sel_key_ctrl()),
     MLUI_LITERAL(bool, false)},
    {"zonerec_sel_key_alt",   SType::kBool, MLUI_FIELD(zone_recorder, sel_key_alt()),
     MLUI_LITERAL(bool, false)},
    {"zonerec_sel_key_shift", SType::kBool, MLUI_FIELD(zone_recorder, sel_key_shift()),
     MLUI_LITERAL(bool, false)},
    // Copie du GIF dans le presse-papier dès qu'il est écrit (éteint par défaut).
    {"zonerec_auto_copy",     SType::kBool, MLUI_FIELD(zone_recorder, auto_copy()),
     MLUI_LITERAL(bool, false)},
};

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
// la valeur déclarée dans item_desc_window.h : il valait 0 dans le repli
// d'écriture, ce qui ramenait silencieusement l'ancrage en haut-gauche.
const moonlight_ui::SettingDesc kItemDescSettings[] = {
    {"itemdesc_show_item",  SType::kBool, MLUI_FIELD(item_desc, show_item_panel()),
     MLUI_LITERAL(bool, true)},
    {"itemdesc_show_skill", SType::kBool, MLUI_FIELD(item_desc, show_skill_panel()),
     MLUI_LITERAL(bool, true)},
    {"itemdesc_show_book",  SType::kBool, MLUI_FIELD(item_desc, show_book_panel()),
     MLUI_LITERAL(bool, true)},
    {"itemdesc_book_page1", SType::kBool, MLUI_FIELD(item_desc, book_reset_page()),
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

// Cart ImGui (fenêtre sœur de l'inventaire côté client, mêmes réglages).
const moonlight_ui::SettingDesc kCartSettings[] = {
    {"cart_imgui",   SType::kBool, MLUI_FIELD(cart_viewer, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"cart_filter",  SType::kBool, MLUI_FIELD(cart_viewer, show_filter()),
     MLUI_LITERAL(bool, true)},
    {"cart_desc_tooltip", SType::kBool, MLUI_FIELD(cart_viewer, desc_tooltip()),
     MLUI_LITERAL(bool, false)},
    {"cart_tabs_vertical", SType::kBool, MLUI_FIELD(cart_viewer, tabs_vertical()),
     MLUI_LITERAL(bool, true)},
    {"cart_lock_size", SType::kBool, MLUI_FIELD(cart_viewer, lock_size()),
     MLUI_LITERAL(bool, false)},
};

// Entrepôt (Kafra / guilde / premium : la même fenêtre). storage_favorites est
// un CONTENEUR, écrit à la main après cette table.
const moonlight_ui::SettingDesc kStorageSettings[] = {
    {"storage_imgui", SType::kBool, MLUI_FIELD(storage_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"storage_desc_tooltip", SType::kBool, MLUI_FIELD(storage_window, desc_tooltip()),
     MLUI_LITERAL(bool, false)},
    {"storage_filter", SType::kBool, MLUI_FIELD(storage_window, show_filter()),
     MLUI_LITERAL(bool, true)},
    // Onglets de storage : OPT-IN (défaut OFF). Un joueur qui n'a jamais utilisé
    // les entrepôts alternatifs ne doit pas voir sa fenêtre changer de forme.
    {"storage_tabs", SType::kBool, MLUI_FIELD(storage_window, show_storage_tabs()),
     MLUI_LITERAL(bool, false)},
    // Filtres par type (onglets de catégorie + sous-type) : ON par défaut, c'est
    // le comportement historique de la fenêtre — la case sert à s'en passer.
    {"storage_type_tabs", SType::kBool, MLUI_FIELD(storage_window, show_type_tabs()),
     MLUI_LITERAL(bool, true)},
    {"storage_tabs_vertical", SType::kBool, MLUI_FIELD(storage_window, tabs_vertical()),
     MLUI_LITERAL(bool, false)},
    {"storage_tab_images", SType::kBool, MLUI_FIELD(storage_window, tab_images()),
     MLUI_LITERAL(bool, true)},
    {"storage_col_index", SType::kBool, MLUI_FIELD(storage_window, show_index_col()),
     MLUI_LITERAL(bool, false)},
    {"storage_col_id", SType::kBool, MLUI_FIELD(storage_window, show_id_col()),
     MLUI_LITERAL(bool, false)},
    {"storage_col_slots", SType::kBool, MLUI_FIELD(storage_window, show_slots_col()),
     MLUI_LITERAL(bool, false)},
    {"storage_col_value", SType::kBool, MLUI_FIELD(storage_window, show_value_col()),
     MLUI_LITERAL(bool, true)},
    {"storage_total_value", SType::kBool, MLUI_FIELD(storage_window, show_total_value()),
     MLUI_LITERAL(bool, true)},
    {"storage_tab", SType::kInt, MLUI_FIELD(storage_window, cur_tab()),
     MLUI_LITERAL(int, 0)},
};

// Banque de zeny (Ctrl+B). « bank_imgui » est basculé en GROUPE par
// SetModernInterface : défaut OFF, comme tous les membres du groupe.
// ⚠ `bank_quick_amounts` et `bank_show_total` ont été RETIRÉS : le fond de cette
// fenêtre est un bitmap du client à hauteur fixe, et masquer l'un de ces deux blocs
// décalait tout le contenu hors du fond peint. Les deux sont désormais permanents.
// Les clés éventuellement présentes dans un fichier de réglages existant sont
// simplement ignorées au chargement — aucune migration n'est nécessaire.
const moonlight_ui::SettingDesc kBankSettings[] = {
    {"bank_imgui", SType::kBool, MLUI_FIELD(bank_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
};

// Refine d'arme Whitesmith (fenêtre « Upgradeable weapons », id 111).
// « refine_imgui » est basculé en GROUPE par SetModernInterface : défaut OFF.
const moonlight_ui::SettingDesc kRefineSettings[] = {
    {"refine_imgui", SType::kBool, MLUI_FIELD(weapon_refine_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"refine_confirm", SType::kBool, MLUI_FIELD(weapon_refine_window, confirm()),
     MLUI_LITERAL(bool, true)},
    {"refine_show_cards", SType::kBool, MLUI_FIELD(weapon_refine_window, show_cards()),
     MLUI_LITERAL(bool, true)},
    {"refine_filter", SType::kBool, MLUI_FIELD(weapon_refine_window, show_filter()),
     MLUI_LITERAL(bool, true)},
    {"refine_desc_tooltip", SType::kBool, MLUI_FIELD(weapon_refine_window, desc_tooltip()),
     MLUI_LITERAL(bool, true)},
    {"refine_history", SType::kBool, MLUI_FIELD(weapon_refine_window, show_history()),
     MLUI_LITERAL(bool, false)},
    {"refine_auto_recast", SType::kBool, MLUI_FIELD(weapon_refine_window, auto_recast()),
     MLUI_LITERAL(bool, false)},
    // Défaut OFF, et il le restera : c'est le seul réglage du plugin qui joue une
    // arme à la place du joueur — chaque tentative pouvant la détruire. Il n'entre
    // PAS dans le groupe « Interface moderne » (SetModernInterface ne bascule que
    // « refine_imgui »), donc activer l'interface moderne ne l'allume jamais.
    {"refine_auto_refine", SType::kBool, MLUI_FIELD(weapon_refine_window, auto_refine()),
     MLUI_LITERAL(bool, false)},
    // Défaut OFF : Entrée ne déclenche pas le refine et reste au CHAT. Le bouton et
    // le double-clic font le travail.
    // (Elle a été confisquée dans les deux réglages pendant un temps, tant que la
    // native 111 vivait masquée derrière : une native invisible garde le clavier et
    // son bouton par défaut est un refine réel. Cette fenêtre ne naît plus — cf.
    // WeaponRefineWindow::WantsEnterKey.)
    {"refine_enter_key", SType::kBool, MLUI_FIELD(weapon_refine_window, enter_key()),
     MLUI_LITERAL(bool, false)},
    {"refine_log_time", SType::kBool, MLUI_FIELD(weapon_refine_window, log_time()),
     MLUI_LITERAL(bool, true)},
    // INT_MIN = « aucune position mémorisée » : la fenêtre se cale alors sur la
    // native, comme au premier lancement. Même convention que game_option_pos_*.
    {"refine_pos_x", SType::kInt, MLUI_FIELD(weapon_refine_window, pos_x()),
     MLUI_LITERAL(int, INT_MIN)},
    {"refine_pos_y", SType::kInt, MLUI_FIELD(weapon_refine_window, pos_y()),
     MLUI_LITERAL(int, INT_MIN)},
};

// « makeitem_imgui » est basculé en GROUPE par SetModernInterface : défaut OFF.
const moonlight_ui::SettingDesc kMakeItemSettings[] = {
    {"makeitem_imgui", SType::kBool, MLUI_FIELD(make_item_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"makeitem_show_owned", SType::kBool, MLUI_FIELD(make_item_window, show_owned()),
     MLUI_LITERAL(bool, true)},
    {"makeitem_filter", SType::kBool, MLUI_FIELD(make_item_window, show_filter()),
     MLUI_LITERAL(bool, true)},
    {"makeitem_desc_tooltip", SType::kBool, MLUI_FIELD(make_item_window, desc_tooltip()),
     MLUI_LITERAL(bool, true)},
    {"makeitem_history", SType::kBool, MLUI_FIELD(make_item_window, show_history()),
     MLUI_LITERAL(bool, false)},
    {"makeitem_log_time", SType::kBool, MLUI_FIELD(make_item_window, log_time()),
     MLUI_LITERAL(bool, true)},
    {"makeitem_auto_recast", SType::kBool, MLUI_FIELD(make_item_window, auto_recast()),
     MLUI_LITERAL(bool, false)},
    {"makeitem_enter_key", SType::kBool, MLUI_FIELD(make_item_window, enter_key()),
     MLUI_LITERAL(bool, false)},
    // Relance par OBJET : clé SÉPARÉE de makeitem_auto_recast, et défaut OFF. Elle
    // autorise une dépense de stock (chaque relance détruit un exemplaire), ce
    // qu'un réglage déjà coché ne doit jamais pouvoir accorder à sa place.
    {"makeitem_auto_reuse_item", SType::kBool,
     MLUI_FIELD(make_item_window, auto_reuse_item()), MLUI_LITERAL(bool, false)},
    // 0 = ILLIMITÉ (défaut) : la chaîne s'arrête sur une condition réelle (stock
    // épuisé, liste vide, refus serveur), pas sur un compte arbitraire.
    {"makeitem_auto_reuse_max", SType::kInt,
     MLUI_FIELD(make_item_window, auto_reuse_max()), MLUI_LITERAL(int, 0)},
    {"makeitem_pos_x", SType::kInt, MLUI_FIELD(make_item_window, pos_x()),
     MLUI_LITERAL(int, INT_MIN)},
    {"makeitem_pos_y", SType::kInt, MLUI_FIELD(make_item_window, pos_y()),
     MLUI_LITERAL(int, INT_MIN)},
};

// Atlas des recettes. 🔴 Aucune clé « imgui » ici, et ce n'est pas un oubli :
// l'Atlas ne remplace RIEN, il ajoute une fenêtre de consultation. Il n'entre
// donc pas dans le groupe « interface moderne », qui bascule des remplacements.
const moonlight_ui::SettingDesc kCraftAtlasSettings[] = {
    // La fenêtre se rouvre où on l'avait laissée : c'est une fenêtre de
    // consultation qu'on garde ouverte pendant qu'on farme.
    {"craftatlas_open", SType::kBool, MLUI_FIELD(craft_atlas, open()),
     MLUI_LITERAL(bool, false)},
    {"craftatlas_desc_tooltip", SType::kBool, MLUI_FIELD(craft_atlas, desc_tooltip()),
     MLUI_LITERAL(bool, true)},
    // Défaut OFF : l'Atlas sert d'abord à préparer ce qu'on n'a PAS encore, et un
    // filtre par défaut cacherait justement ce qu'on venait chercher.
    {"craftatlas_only_craftable", SType::kBool,
     MLUI_FIELD(craft_atlas, only_craftable()), MLUI_LITERAL(bool, false)},
    // Défaut OFF : les recettes dont aucune classe du serveur n'apprend la
    // compétence sont des lignes que le serveur refusera toujours.
    {"craftatlas_show_unavailable", SType::kBool,
     MLUI_FIELD(craft_atlas, show_unavailable()), MLUI_LITERAL(bool, false)},
    {"craftatlas_pos_x", SType::kInt, MLUI_FIELD(craft_atlas, pos_x()),
     MLUI_LITERAL(int, INT_MIN)},
    {"craftatlas_pos_y", SType::kInt, MLUI_FIELD(craft_atlas, pos_y()),
     MLUI_LITERAL(int, INT_MIN)},
};

// Fiche de monstre (remplace « Monster Info », id 0x4D, ouverte par Sense).
// « monsterinfo_imgui » est basculé en GROUPE par SetModernInterface : défaut OFF.
const moonlight_ui::SettingDesc kMonsterInfoSettings[] = {
    {"monsterinfo_imgui", SType::kBool, MLUI_FIELD(monster_info, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    // Défaut ON : l'animation du sprite est l'apport principal sur le natif, qui
    // fige la première image (docs/monster_info_re.md §4.4).
    {"monsterinfo_animate", SType::kBool, MLUI_FIELD(monster_info, animate()),
     MLUI_LITERAL(bool, true)},
    // Défaut OFF : la liste noire des Gardiens de forteresse est une règle de jeu
    // (Guerre d'Emperium), pas un défaut d'interface — on la reproduit.
    {"monsterinfo_guardians", SType::kBool,
     MLUI_FIELD(monster_info, show_guardians()), MLUI_LITERAL(bool, false)},
};

// Menu contextuel du clic droit sur une entité (remplace UIMenuWnd 0x12 côté
// monde). « ctxmenu_imgui » est basculé en GROUPE par SetModernInterface.
const moonlight_ui::SettingDesc kEntityContextMenuSettings[] = {
    {"ctxmenu_imgui", SType::kBool,
     MLUI_FIELD(entity_context_menu, imgui_enabled_), MLUI_LITERAL(bool, false)},
    // Défaut OFF : ouvrir un menu sur un monstre ou un NPC est un comportement
    // que le client n'a jamais eu — c'est au joueur de le demander.
    {"ctxmenu_all_entities", SType::kBool,
     MLUI_FIELD(entity_context_menu, all_entities()), MLUI_LITERAL(bool, false)},
    // Défaut ON : sans le gate serveur (group level >= 80) la section staff ne
    // s'affiche de toute façon pas ; l'interrupteur ne sert qu'à la replier.
    {"ctxmenu_staff_extras", SType::kBool,
     MLUI_FIELD(entity_context_menu, staff_extras()), MLUI_LITERAL(bool, true)},
};

// Fenêtres ImGui opt-in restantes + pose de l'avatar de la feuille de perso.
const moonlight_ui::SettingDesc kOptInWindowSettings[] = {
    {"cashshop_imgui", SType::kBool, MLUI_FIELD(cashshop_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"shop_imgui",  SType::kBool, MLUI_FIELD(npc_shop_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"vending_imgui", SType::kBool, MLUI_FIELD(vending_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    // Pas de widget ici : la case vit dans la fenêtre de composition (c'est là
    // qu'elle agit). Cette entrée ne sert qu'à la PERSISTER.
    {"vending_compose_grid", SType::kBool,
     MLUI_FIELD(vending_window, compose_grid_), MLUI_LITERAL(bool, false)},
    {"trade_imgui", SType::kBool, MLUI_FIELD(trade_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"rodex_imgui", SType::kBool, MLUI_FIELD(rodex_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    // Chatbox ImGui (phase 1 : écoute, la native reste). Clés préfixées
    // « chatwnd_ » : « chat_* » appartient déjà à ChatTweaks, qui règle le chat
    // NATIF — les deux jeux d'options coexistent tant que les deux chats existent.
    {"chatwnd_imgui", SType::kBool, MLUI_FIELD(chat_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"chatwnd_timestamps", SType::kBool, MLUI_FIELD(chat_window, timestamps()),
     MLUI_LITERAL(bool, false)},
    {"chatwnd_item_icons", SType::kBool, MLUI_FIELD(chat_window, item_icons()),
     MLUI_LITERAL(bool, true)},
    {"chatwnd_input_bar", SType::kBool, MLUI_FIELD(chat_window, input_bar()),
     MLUI_LITERAL(bool, true)},
    // (« chatwnd_locked » a disparu : le verrouillage est désormais PAR FENÊTRE et
    // se range avec le reste de la géométrie, dans le fichier de disposition du
    // chat. Une clé restée dans un ancien yaml est simplement ignorée à la
    // lecture, et disparaît à la prochaine écriture.)
    // ⚠ Défaut VRAI, et c'est un garde-fou, pas une gêne : une adresse de chat
    // vient d'un tiers et le texte affiché n'a aucun rapport obligé avec la
    // destination. Le joueur peut le retirer — explicitement.
    {"chatwnd_url_confirm", SType::kBool, MLUI_FIELD(chat_window, url_confirm()),
     MLUI_LITERAL(bool, true)},
    // ⚠ Défaut FAUX, et symétrique du précédent : celui-ci fait AGIR le client
    // (une requête réseau vers l'hébergeur d'un lien posté par autrui). Un
    // garde-fou est là par défaut ; une action, non.
    {"chatwnd_url_preview", SType::kBool, MLUI_FIELD(chat_window, url_preview()),
     MLUI_LITERAL(bool, false)},
    // Hôtes autorisés PAR LE JOUEUR (« a.com;b.net »), vides par défaut : rien
    // n'est accordé qu'il n'ait accordé lui-même.
    {"chatwnd_font_family", SType::kInt, MLUI_FIELD(chat_window, font_family()),
     MLUI_LITERAL(int, 0)},
    // Vignettes : la case et la taille sont DEUX clés. Défaut décoché — afficher
    // des images vient avec du trafic réseau, et ça se demande (cf.
    // chatwnd_url_preview). La taille, elle, garde une valeur utile même éteinte.
    {"chatwnd_thumbs", SType::kBool, MLUI_FIELD(chat_window, thumbs()),
     MLUI_LITERAL(bool, false)},
    {"chatwnd_thumb_px", SType::kInt, MLUI_FIELD(chat_window, thumb_px()),
     MLUI_LITERAL(int, 48)},
    {"chatwnd_url_hosts", SType::kString, MLUI_FIELD(chat_window, url_hosts()),
     MLUI_LITERAL(std::string, "")},
    // ⚠ Défaut FAUX, et ce n'est pas de la timidité : le fichier écrit contient
    // les chuchotements en clair, à côté du jeu.
    {"chatwnd_keep_history", SType::kBool, MLUI_FIELD(chat_window, keep_history()),
     MLUI_LITERAL(bool, false)},
    {"chatwnd_keep_lines", SType::kInt, MLUI_FIELD(chat_window, keep_lines()),
     MLUI_LITERAL(int, 100)},
    // Skin de la chatbox : couleurs au format picker (persistées « AARRGGBB »),
    // puis les trois leviers de mise en page demandés côté joueur.
    {"chatwnd_body",   SType::kColorHex, MLUI_FIELD(chat_window, body_rgba_),
     MLUI_LITERAL_ARGB(0x96000000)},
    {"chatwnd_border", SType::kColorHex, MLUI_FIELD(chat_window, border_rgba_),
     MLUI_LITERAL_ARGB(0xFFC5C5C5)},
    {"chatwnd_tab",    SType::kColorHex, MLUI_FIELD(chat_window, tab_rgba_),
     MLUI_LITERAL_ARGB(0xFF8E938E)},
    // « font » = le texte du LOG, « uifont » = l'habillage. La clé historique garde
    // son nom : la renommer invaliderait les fichiers déjà chez les joueurs.
    {"chatwnd_font",    SType::kInt, MLUI_FIELD(chat_window, font_scale_pct()),
     MLUI_LITERAL(int, 100)},
    {"chatwnd_uifont",  SType::kInt, MLUI_FIELD(chat_window, ui_scale_pct()),
     MLUI_LITERAL(int, 100)},
    {"chatwnd_padding", SType::kInt, MLUI_FIELD(chat_window, padding_px()),
     MLUI_LITERAL(int, 3)},
    {"chatwnd_linegap", SType::kInt, MLUI_FIELD(chat_window, line_gap_px()),
     MLUI_LITERAL(int, 2)},
    {"chatwnd_history", SType::kInt, MLUI_FIELD(chat_window, history_cap()),
     MLUI_LITERAL(int, 500)},
    {"npc_dialog_imgui", SType::kBool, MLUI_FIELD(npc_dialog_window, imgui_enabled_),
     MLUI_LITERAL(bool, false)},
    {"npc_menu_search",  SType::kBool, MLUI_FIELD(npc_dialog_window, menu_search_),
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
    {"charsheet_grimoire_bilinear", SType::kBool,
     MLUI_FIELD(character_sheet, skill_bilinear()), MLUI_LITERAL(bool, false)},
    {"login_parade", SType::kBool, MLUI_FIELD(login_parade, enabled_),
     MLUI_LITERAL(bool, true)},
};

// Saut (PlayerJump) : activation + touche. Seuls la hauteur et la durée
// de l'arc restent vives et non persistées (réglages staff, cf. panel_fun).
// 0x20 = VK_SPACE.
const moonlight_ui::SettingDesc kJumpKeySettings[] = {
    {"jump_enabled",   SType::kBool, MLUI_FIELD(player_jump, enabled()),
     MLUI_LITERAL(bool, true)},
    {"jump_key_vk",    SType::kInt,  MLUI_FIELD(player_jump, key_vk()),
     MLUI_LITERAL(int, 0x20)},
    {"jump_key_ctrl",  SType::kBool, MLUI_FIELD(player_jump, key_ctrl()),
     MLUI_LITERAL(bool, false)},
    {"jump_key_alt",   SType::kBool, MLUI_FIELD(player_jump, key_alt()),
     MLUI_LITERAL(bool, false)},
    {"jump_key_shift", SType::kBool, MLUI_FIELD(player_jump, key_shift()),
     MLUI_LITERAL(bool, false)},
};

// Déplacement au clavier (KeyboardMove) : activation + les deux options
// visibles par tous. L'anticipation et la cadence restent vives et non
// persistées (réglages staff, cf. panel_fun).
const moonlight_ui::SettingDesc kKeyboardMoveSettings[] = {
    {"kbmove_enabled", SType::kBool, MLUI_FIELD(keyboard_move, enabled()),
     MLUI_LITERAL(bool, true)},
    {"kbmove_camera_relative", SType::kBool,
     MLUI_FIELD(keyboard_move, camera_relative()), MLUI_LITERAL(bool, true)},
    {"kbmove_stop_on_release", SType::kBool,
     MLUI_FIELD(keyboard_move, stop_on_release()), MLUI_LITERAL(bool, true)},
};

// Quick cast (QuickCast, réservé staff) : cast en une action, opt-in, OFF par
// défaut. Persister est sans risque : l'action reste gatée par IsStaff() à
// chaque 0x48, comme la fenêtre de logs.
// `quickcast_repeat_ms` = période de répétition touche maintenue. Il RESTE malgré
// le cooldown réel (consulté avant, cf. quick_cast.cc) parce que le client ignore
// le délai d'après-incantation du serveur, qui borne la plupart des sorts.
// `quickcast_item` étend cette répétition aux cases d'OBJET de la barre d'action,
// avec sa PROPRE cadence : le frein n'y est pas le même — c'est le serveur qui
// impose l'intervalle minimal entre deux objets, et il le ramène à 20 ms au-dessus
// du niveau de groupe 40, donc pour tout le monde ici (l'option est staff).
const moonlight_ui::SettingDesc kQuickCastSettings[] = {
    {"quickcast_ground", SType::kBool, MLUI_FIELD(quick_cast, ground_enabled()),
     MLUI_LITERAL(bool, false)},
    {"quickcast_target", SType::kBool, MLUI_FIELD(quick_cast, target_enabled()),
     MLUI_LITERAL(bool, false)},
    {"quickcast_repeat_ms", SType::kInt, MLUI_FIELD(quick_cast, repeat_ms()),
     MLUI_LITERAL(int, 200)},
    {"quickcast_item", SType::kBool, MLUI_FIELD(quick_cast, item_enabled()),
     MLUI_LITERAL(bool, false)},
    {"quickcast_item_repeat_ms", SType::kInt,
     MLUI_FIELD(quick_cast, item_repeat_ms()), MLUI_LITERAL(int, 50)},
};

// Barres EXP/HP/SP et portrait de statut (BasicInfo). Les barres et les
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
// ro::ArgbFromPicker) des flottants déclarés dans skill_bar.h ; la couleur
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

// « Sol uni » (fond de capture, section Staff Tools). Son état n'appartient pas à
// un plugin enregistré mais à deux globales de ground_paint, atteintes par des
// fonctions libres : le résolveur s'écrit à la main, il ne peut jamais rendre nullptr.
// Les CLÉS YAML restent « ground_paint »/« ground_paint_color » : les renommer
// perdrait le réglage de tous ceux qui l'ont déjà enregistré.
const moonlight_ui::SettingDesc kGroundPaintSettings[] = {
    {"ground_paint", SType::kBool,
     []() -> void* { return &ground_paint::enabled(); },
     MLUI_LITERAL(bool, false)},
    {"ground_paint_color", SType::kColorHex,
     []() -> void* { return ground_paint::color(); },
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
  static const moonlight_ui::SettingDesc kHeader[5];
  // Réglages de chat portés par MoonlightUi (ils déménageront chez ChatTweaks à
  // l'étape C — c'est ce qui débloquera le déplacement du panneau « Chat »).
  static const moonlight_ui::SettingDesc kChat[5];
  // Grille d'alignement globale : elle sert à TOUS les overlays déplaçables,
  // d'où sa place ici plutôt que chez basic_info, où elle a commencé (cf. les
  // anciennes clés expbar_grid_*, recopiées par MigrateLegacyKeys).
  static const moonlight_ui::SettingDesc kGrid[4];
};

const moonlight_ui::SettingDesc MoonlightUiOwnSettings::kHeader[5] = {
    {"ui_collapsed", SType::kBool, MLUI_SELF(ui_collapsed_),
     MLUI_LITERAL(bool, false)},
    {"log_level", SType::kString, MLUI_SELF(log_level_),
     MLUI_LITERAL(std::string, "info")},
    {"alootid_overlay", SType::kBool, MLUI_SELF(show_alootid_overlay_),
     MLUI_LITERAL(bool, false)},
    // Fenêtre de logs en jeu. Le champ ne vit pas dans un plugin mais dans
    // Bourgeon lui-même — d'où un résolveur écrit à la main plutôt que
    // MLUI_SELF/MLUI_FIELD : ceux-ci gèrent un propriétaire NULLABLE (un plugin
    // peut ne pas être enregistré), alors que Bourgeon::Instance() existe
    // toujours. Le résolveur ne peut donc pas rendre nullptr.
    //
    // Persister une clé « staff » est sans risque : l'affichage reste gaté par
    // IsStaff() à CHAQUE frame. Un compte non-staff qui hériterait de la clé à
    // true ne verrait toujours rien.
    {"staff_log_window", SType::kBool,
     []() -> void* { return &Bourgeon::Instance().show_log_window(); },
     MLUI_LITERAL(bool, false)},
    // ── Glyphes coréens (débogage) ──────────────────────────────────────────
    // 🔴 LU À L'INIT, DIRECTEMENT DANS LE YAML, par ro::KoreanGlyphsWanted() :
    // l'atlas se construit bien avant que cette table n'existe. L'entrée est ici
    // pour que le réglage soit ÉCRIT et survive — pas pour être appliqué.
    // Changer sa valeur exige donc un redémarrage du client, et le libellé le dit.
    //
    // Défaut FAUX : le hangul pesait 97 % de l'atlas pour des caractères
    // qu'aucun joueur ne voit. Seul le staff qui lit des chemins de fichiers du
    // jeu dans la console en a l'usage.
    {"korean_glyphs", SType::kBool,
     []() -> void* { return &g_korean_glyphs; },
     MLUI_LITERAL(bool, false)},
};

// 🔴 « language » N'EST PLUS ICI, et ne doit pas y revenir. Ce fichier n'est relu
// qu'à l'entrée en jeu (LoadSettings, sur `in_game_ && !was_in_game`) : la langue
// arrivait donc APRÈS l'écran de login et le char-select, qui restaient en
// français quel que soit le réglage — précisément les écrans qu'un joueur
// anglophone rencontre en premier.
// Elle vit maintenant dans paths::StartupSettingsPath(), lu au chargement de la
// DLL par i18n::LoadLanguageSetting(), et i18n::SetLanguage l'y réécrit
// elle-même. L'ancienne clé est reprise automatiquement une fois.

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

// Item-link icon injection moved to features/patches/chat.cc (ChatTweaks).

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
// en un point unique toutes les fenêtres modernes interdépendantes. Chaque plugin
// garde son propre flag, mais il n'est plus jamais basculé isolément.
void SetModernInterface(bool on) {
  if (auto* inventory_viewer = Bourgeon::Instance().inventory_viewer())
    inventory_viewer->imgui_enabled_ = on;
  // Le cart suit l'inventaire : les deux s'échangent des objets par glisser, et
  // un cart natif ne sait pas déposer chez nous (ni l'inverse).
  if (auto* cart_viewer = Bourgeon::Instance().cart_viewer())
    cart_viewer->imgui_enabled_ = on;
  if (auto* storage_window = Bourgeon::Instance().storage_window())
    storage_window->imgui_enabled_ = on;
  if (auto* skill_bar = Bourgeon::Instance().skill_bar())
    skill_bar->enabled_ = on;
  if (auto* trade_window = Bourgeon::Instance().trade_window())
    trade_window->imgui_enabled_ = on;
  // Le courrier fait partie du lot : sa fenêtre d'écriture reçoit les objets
  // glissés depuis l'inventaire ImGui, ce qui n'a de sens que si les deux sont
  // modernes en même temps (un inventaire natif ne sait pas déposer chez nous).
  if (auto* rodex_window = Bourgeon::Instance().rodex_window())
    rodex_window->imgui_enabled_ = on;
  // L'échoppe joueur (vente ET achat) suit aussi : elle se monte à partir du
  // CHARIOT, qui est déjà du lot. Un formulaire d'échoppe moderne au-dessus d'un
  // cart natif (ou l'inverse) serait le mixe qu'on a justement supprimé.
  if (auto* vending_window = Bourgeon::Instance().vending_window())
    vending_window->imgui_enabled_ = on;
  // La feuille de personnage REMPLACE désormais les feuilles natives Status et
  // Équipement (⚠ celles-ci ne sont pas encore empêchées de NAÎTRE : chantier
  // ouvert). Elle vit du même écosystème : ses slots reçoivent
  // les objets glissés depuis l'inventaire ImGui, et son onglet Presets équipe en
  // s'appuyant dessus. Elle n'a donc plus de case isolée non plus.
  // ⚠ Son onglet Grimoire, lui, REMPLACE bel et bien la fenêtre native 0x25 : quand
  // ce groupe est actif, celle-ci est masquée dès sa création et l'onglet prend sa
  // place (cf. window_pos_tweaks + docs/skill_tree_re.md partie II).
  if (auto* character_sheet = Bourgeon::Instance().character_sheet())
    character_sheet->imgui_enabled_ = on;
  // Boutiques (cash shop et PNJ) : elles achètent VERS l'inventaire et vendent
  // DEPUIS lui — un panier moderne au-dessus d'un inventaire natif (ou l'inverse)
  // remet exactement le mixe qu'on a supprimé.
  if (auto* cashshop_window = Bourgeon::Instance().cashshop_window())
    cashshop_window->imgui_enabled_ = on;
  if (auto* npc_shop_window = Bourgeon::Instance().npc_shop_window())
    npc_shop_window->imgui_enabled_ = on;
  // La banque échange des zeny avec la POCHE, dont le montant est affiché par le
  // footer de l'inventaire moderne — et c'est le bouton « sac de zeny » de ce
  // footer qui l'ouvre. Une banque moderne au-dessus d'un inventaire natif (ou
  // l'inverse) laisserait ce bouton sans fenêtre, ou la fenêtre sans bouton.
  if (auto* bank_window = Bourgeon::Instance().bank_window())
    bank_window->imgui_enabled_ = on;
  // Le refine Whitesmith rejoint le lot : sa liste d'armes se lit dans le
  // MÊME modèle d'inventaire que le viewer moderne (noms composés, icônes,
  // aperçu de description), et il ferme/rouvre la fenêtre native à chaque
  // tentative. Moderne au-dessus d'un inventaire natif, il retomberait sur le
  // repli « nom de base » de SafeName, faute de contexte de composition.
  if (auto* weapon_refine_window = Bourgeon::Instance().weapon_refine_window())
    weapon_refine_window->imgui_enabled_ = on;
  // Les fenêtres de FABRICATION (94 « LIST » et 79 « Manufacturing List ») pour la
  // même raison que le refine : elles listent des objets dont le nom, l'icône et
  // le stock sont lus dans le même modèle d'inventaire que le viewer moderne, et
  // elles se détruisent à chaque validation. Cf. docs/make_item_list_re.md.
  if (auto* make_item_window = Bourgeon::Instance().make_item_window())
    make_item_window->imgui_enabled_ = on;
  // La fiche de monstre suit le groupe : elle REVENDIQUE le paquet 0x018C, donc
  // l'activer isolément tuerait la fenêtre native Monster Info alors que tout le
  // reste de l'interface serait encore natif. Et son lien depuis la table des
  // drops n'a de sens qu'avec la fiche d'item moderne, qui en fait partie.
  if (auto* monster_info = Bourgeon::Instance().monster_info())
    monster_info->imgui_enabled_ = on;
  // Le menu contextuel d'entité suit le groupe : une de ses entrées ouvre la
  // fiche de monstre ci-dessus, et surtout il DÉTOURNE le constructeur du menu
  // natif — l'activer seul laisserait un menu moderne au milieu d'une interface
  // entièrement native. Cf. docs/entity_context_menu_re.md.
  if (auto* entity_context_menu = Bourgeon::Instance().entity_context_menu())
    entity_context_menu->imgui_enabled_ = on;
}

bool ModernInterfaceEnabled() {
  // L'inventaire est l'ancre du groupe : SetModernInterface les écrit tous
  // ensemble, et c'est déjà sur lui que LoadSettings réconcilie un yaml mixé.
  if (auto* inventory_viewer = Bourgeon::Instance().inventory_viewer())
    return inventory_viewer->imgui_enabled_;
  return false;
}

// Case + infobulle communes aux panneaux porteurs (cf. moonlight_ui.h).
bool DrawModernInterfaceCheckbox(bool* enabled, const char* window_help) {
  bool changed = false;
  if (ro::RoCheckbox(i18n::Tr("Interface moderne"), enabled)) {
    SetModernInterface(*enabled);
    changed = true;
  }
  // Liste du groupe : SOURCE UNIQUE de l'infobulle (le code, lui, a la sienne
  // juste au-dessus, dans SetModernInterface).
  std::string help =
      i18n::Tr("Interrupteur GLOBAL — ces fenêtres s'activent ENSEMBLE, pas de mixe (tout "
      "ImGui ou tout natif) :\n"
      "  • Inventaire (et le sertissage de cartes)\n"
      "  • Cart\n"
      "  • Storage (Kafra, guilde, premium)\n"
      "  • Barres d'action\n"
      "  • Échange joueur-joueur\n"
      "  • Courrier (RODEX)\n"
      "  • Shop joueur (vending, buying store et achat chez un vendeur)\n"
      "  • Feuille de personnage (Alt+F), grimoire compris : l'icône « Skill » et\n"
      "    Alt+S ouvrent son onglet Grimoire au lieu de la fenêtre native\n"
      "  • Cash shop et shops PNJ\n"
      "  • Banque de zeny (Ctrl+B), ouverte aussi par le sac de zeny du footer\n"
      "    de l'inventaire\n"
      "  • Refine d'arme (compétence Upgrade Weapon du Whitesmith)\n"
      "  • Fiche de monstre (compétence Sense), avec sprite animé, drops et\n"
      "    lieux d'apparition\n"
      "  • Menu du clic droit sur une entité\n"
      "La case des autres sections reflète donc le même état.\n\n");
  help += window_help;
  ImGui::SameLine();
  HelpMarker(help.c_str());
  return changed;
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
    // Pas de i18n::ReloadCatalog() ici : la langue est chargée bien avant, au
    // chargement de la DLL, depuis paths::StartupSettingsPath(). La rappeler à
    // l'entrée en jeu ne servirait qu'à jeter les pointeurs déjà rendus par `Tr`.
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
    moonlight_ui::ReadSettings(ui, kCartSettings);
    moonlight_ui::ReadSettings(ui, kStorageSettings);
    moonlight_ui::ReadSettings(ui, kBankSettings);
    moonlight_ui::ReadSettings(ui, kRefineSettings);
    moonlight_ui::ReadSettings(ui, kMakeItemSettings);
    moonlight_ui::ReadSettings(ui, kCraftAtlasSettings);
    moonlight_ui::ReadSettings(ui, kMonsterInfoSettings);
    moonlight_ui::ReadSettings(ui, kEntityContextMenuSettings);
    moonlight_ui::ReadStorageFavorites(ui);
    moonlight_ui::ReadStorageTabCustom(ui);
    moonlight_ui::ReadSettings(ui, kOptInWindowSettings);
    moonlight_ui::ReadSettings(ui, kJumpKeySettings);
    moonlight_ui::ReadSettings(ui, kKeyboardMoveSettings);
    moonlight_ui::ReadSettings(ui, kQuickCastSettings);
    moonlight_ui::ReadSettings(ui, kSkillBarSettings);
    moonlight_ui::ReadSkillBarLayout(ui);
    moonlight_ui::ReadSettings(ui, kSkillBarColorSettings);

    moonlight_ui::ReadSettings(ui, kStatusIconSettings);
    moonlight_ui::ReadSettings(ui, kQuestTrackerSettings);
    moonlight_ui::ReadSettings(ui, kGraphicsSettings);
    moonlight_ui::ReadSettings(ui, kZoneRecorderSettings);
    moonlight_ui::ReadSettings(ui, kEntityNameSettings);
    moonlight_ui::ReadSettings(ui, kChatBalloonSettings);

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

  // « Tout-ImGui ou tout-natif » : la liste complète du groupe est celle de
  // SetModernInterface (moonlight_ui.h) — on ne la recopie PAS ici, elle a déjà
  // rouillé une fois. Un yaml antérieur au regroupement pouvait être mixé : on
  // réconcilie en OU (au moins une moderne => toutes modernes ; tout natif sinon),
  // puis les cases restent synchronisées.
  // Seules les fenêtres qui ont EU une case isolée par le passé sont testées : les
  // membres arrivés déjà groupés (cart, échoppe) n'ont jamais pu être activés
  // seuls, donc les lire n'apporterait rien à la réconciliation. En revanche la
  // feuille de perso et les deux boutiques, elles, ont eu la leur : les OMETTRE
  // ne serait pas neutre, SetModernInterface les ÉTEINDRAIT au chargement chez
  // qui les avait activées seules.
  auto* inventory      = Bourgeon::Instance().inventory_viewer();
  auto* storage        = Bourgeon::Instance().storage_window();
  auto* skill_bar      = Bourgeon::Instance().skill_bar();
  auto* trade          = Bourgeon::Instance().trade_window();
  auto* rodex          = Bourgeon::Instance().rodex_window();
  auto* character_sheet = Bourgeon::Instance().character_sheet();
  auto* cashshop       = Bourgeon::Instance().cashshop_window();
  auto* shop           = Bourgeon::Instance().npc_shop_window();
  SetModernInterface((inventory && inventory->imgui_enabled_) ||
                     (storage && storage->imgui_enabled_) ||
                     (skill_bar && skill_bar->enabled_) ||
                     (trade && trade->imgui_enabled_) ||
                     (rodex && rodex->imgui_enabled_) ||
                     (character_sheet && character_sheet->imgui_enabled_) ||
                     (cashshop && cashshop->imgui_enabled_) ||
                     (shop && shop->imgui_enabled_));

  if (auto* status_icons = Bourgeon::Instance().status_icons()) status_icons->MarkDirty();
  if (auto* screen_fx = Bourgeon::Instance().screen_fx())
    screen_fx->Apply();  // pousse le post-traitement vers la couche d3d9

  // « Sol uni » : le réglage peut revenir ACTIF du YAML sans que le panneau Staff
  // Tools ait jamais été ouvert de la session — ses hooks de passe terrain doivent
  // alors exister quand même, sinon le sol reste texturé et la case ment.
  if (ground_paint::enabled()) ground_paint::EnsureInstalled();
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

  // Barres EXP/HP/SP (BasicInfo)
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

  moonlight_ui::WriteSettings(out, kZoneRecorderSettings);

  moonlight_ui::WriteSettings(out, kEntityNameSettings);
  moonlight_ui::WriteSettings(out, kChatBalloonSettings);

  moonlight_ui::WriteSkinAndPresets(out);
  moonlight_ui::WriteSettings(out, kInventorySettings);
  moonlight_ui::WriteInventoryLayout(out);
  moonlight_ui::WriteSettings(out, kCartSettings);
  moonlight_ui::WriteSettings(out, kStorageSettings);
  moonlight_ui::WriteSettings(out, kBankSettings);
  moonlight_ui::WriteSettings(out, kRefineSettings);
  moonlight_ui::WriteSettings(out, kMakeItemSettings);
  moonlight_ui::WriteSettings(out, kCraftAtlasSettings);
  moonlight_ui::WriteSettings(out, kMonsterInfoSettings);
  moonlight_ui::WriteSettings(out, kEntityContextMenuSettings);
  moonlight_ui::WriteStorageFavorites(out);
  moonlight_ui::WriteStorageTabCustom(out);
  moonlight_ui::WriteSettings(out, kOptInWindowSettings);
  moonlight_ui::WriteSettings(out, kJumpKeySettings);
  moonlight_ui::WriteSettings(out, kKeyboardMoveSettings);
  moonlight_ui::WriteSettings(out, kQuickCastSettings);

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
    const bool active = discord_chat_ && on_discord_relay_map_;
    // Ce ET est le seul endroit qui peut faire taire le relais entrant, et il le
    // fait au REÇU : un message écarté ici ne laisse aucune trace ailleurs. Tracer
    // les deux moitiés SÉPARÉMENT est ce qui distingue « le réglage est éteint »
    // de « on ne se croit pas sur la bonne carte » — deux causes sans rapport, et
    // le même silence. C'est la seconde qui avait mordu (nom de carte tronqué).
    if (active != relay->chat_active())
      LogDiag("[MoonlightUi] relais Discord {} (reglage={} carte={})",
              active ? "ACTIF" : "muet", discord_chat_, on_discord_relay_map_);
    relay->set_chat_active(active);
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
    // Tracé : c'est l'autre moitié de l'énigme du gate staff. Recoupée avec la
    // ligne « reglages recus », elle dit si le niveau a été effacé APRÈS être
    // arrivé, ou s'il n'est jamais revenu.
    LogDiag("[MoonlightUi] sortie de jeu : niveau de groupe remis a zero");
    g_staff_level = 0;
  }

  UpdateRelay();
}

// ZC packet layout (data points past [opcode:2][total_len:2]):
//   [char_id:4][count:2][{id:2, value:4} * count]
// Fil RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h).
void MoonlightUi::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

// Fil PRINCIPAL : le décodage, rejoué à chaque frame, dans l'ordre d'arrivée.
void MoonlightUi::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  if (opcode == kOpcodeMapMove) {
    // 0x0091 ZC_NPCACK_MAPMOVE : `data` pointe sur mapname[16] (ex. « gonryun.gat »).
    //
    // ⚠ `len` EST la taille de ce qu'on a le droit de lire, et ça n'a pas
    // toujours été vrai. Du temps où `data` pointait dans le buffer recv vivant,
    // `len` n'était qu'une longueur déclarative et on bornait sur kMapNameLen ;
    // depuis le passage par net_inbox, `data` est une COPIE de `len` octets et
    // lire au-delà sort du tampon. On borne donc sur le plus petit des deux.
    //
    // Le nom n'est pas garanti terminé par un zéro : strnlen le borne, puis on ne
    // compare que ce qu'on a réellement.
    if (!data) return;
    const char* map_name = reinterpret_cast<const char*>(data);
    const size_t avail = (len < kMapNameLen) ? len : kMapNameLen;
    const size_t name_len = strnlen(map_name, avail);
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

  // 🔴 Mesure, pas commodité. Le gate staff est retombé deux fois sans qu'on
  // puisse dire si le paquet n'était pas arrivé, ou s'il était arrivé SANS le
  // réglage 26 : les deux donnent exactement le même « Staff Tools » absent, et
  // se corrigent à des endroits opposés (serveur ou client). Une ligne par
  // paquet, et ils sont rares — le login, puis les échos de changement.
  {
    int level = -1;
    for (uint16_t i = 0; i < setting_count; ++i)
      if (*reinterpret_cast<const uint16_t*>(data + 6 + i * 6) ==
          kSettingGroupLevel)
        level = static_cast<int>(
            *reinterpret_cast<const uint32_t*>(data + 6 + i * 6 + 2));
    const std::string lvl =
        (level < 0) ? std::string(i18n::Tr("ABSENT")) : std::to_string(level);
    LogDiag("[MoonlightUi] reglages recus : {} entree(s), niveau de groupe {}",
            setting_count, lvl);
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

uint32_t MoonlightUi::ItemIdByName(const char* name) const {
  if (!name || !*name) return 0;
  // Index inverse construit PARESSEUSEMENT, une seule fois : `item_names_` fait
  // plusieurs milliers d'entrées et l'appelant (la recette de fabrication, qui ne
  // dispose que de noms) interroge à chaque arrivée de liste. Un balayage linéaire
  // y serait invisible une fois, ruineux en boucle.
  if (ids_by_name_.empty() && !item_names_.empty()) {
    for (const auto& kv : item_names_) {
      std::string key = kv.second;
      std::transform(key.begin(), key.end(), key.begin(),
                     [](unsigned char c) { return static_cast<char>(::tolower(c)); });
      // ⚠ On garde la PREMIÈRE occurrence. Les homonymes sont fréquents (les
      // quatre « Elemental Converter »…) et aucun choix n'est meilleur qu'un
      // autre ici : cette recherche ne sert qu'à retrouver un MATÉRIAU, dont on
      // veut l'icône et la description — pas à désigner un objet précis.
      ids_by_name_.emplace(std::move(key), kv.first);
    }
  }
  std::string needle(name);
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  const auto it = ids_by_name_.find(needle);
  return (it != ids_by_name_.end()) ? it->second : 0;
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
  // spr_lab::RenderFrame();

  // Persist bars geometry once, the frame after the user finishes a drag.
  if (auto* basic_info = Bourgeon::Instance().basic_info();
      basic_info && basic_info->geometry_dirty_) {
    basic_info->geometry_dirty_ = false;
    SaveSettings();
  }

  // Same for menu-icon positions (set on drag-end in MenuIcons).
  if (auto* menu_icons = Bourgeon::Instance().menu_icons();
      menu_icons && menu_icons->geometry_dirty_) {
    menu_icons->geometry_dirty_ = false;
    SaveSettings();
  }

  // Skill-bar config (set on any panel change / drag-end in SkillBar).
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
    // ── Staff Tools (réservé group level serveur >= 80, cf. IsStaff) ──────────
    // Regroupe les fonctionnalités réservées au staff : affichage permanent des
    // noms d'entités + SPR Lab. Gaté PUREMENT sur le group level reçu au login
    // (setting id 26). Toute la section disparaît pour un non-staff, et l'overlay
    // des noms reste inerte (OnRenderUI vérifie IsStaff).
    if (IsStaff() && CollapsingHeader(i18n::Tr("Staff Tools"))) {
      PushStyleCompact();

      SeparatorText(i18n::Tr("Noms des entités"));
      if (auto* entity_names = Bourgeon::Instance().entity_names())
        entity_names->DrawSettings();

      // Cast en une action : la touche du sort suffit, la visée est résolue sous
      // le curseur et le lancement émis par les messages d'acteur du clic natif
      // (cf. quick_cast.h pour les deux approches écartées).
      SeparatorText(i18n::Tr("Quick cast"));
      if (auto* quick_cast = Bourgeon::Instance().quick_cast())
        quick_cast->DrawSettings();

      // SeparatorText(i18n::Tr("SPR Lab"));
      // spr_lab::DrawDebugControls();

      // Fond neutre pour les captures d'écran : repeint le terrain d'une couleur
      // unie sans toucher à sa géométrie (l'occlusion reste correcte). Vivait dans
      // le SPR Lab, dont il ne partageait rien — et que plus rien ne dessine.
      SeparatorText(i18n::Tr("Fond de capture"));
      ground_paint::DrawSettings();

      // Enregistrement d'une zone de l'écran en GIF animé : de quoi illustrer un
      // tutoriel avec ce que le joueur verra vraiment, interface Bourgeon comprise.
      SeparatorText(i18n::Tr("Enregistrer une zone (GIF)"));
      if (auto* zone_recorder = Bourgeon::Instance().zone_recorder())
        zone_recorder->DrawSettings();

      // ── Journal Bourgeon ────────────────────────────────────────────────────
      // Remplace la console Windows : tout ce qui passe par LogInfo/LogDiag/
      // LogError y arrive, sélectionnable et copiable. PERSISTÉ
      // (« staff_log_window ») : pour qui s'en sert comme console de travail, la
      // rouvrir à chaque lancement serait une corvée quotidienne.
      SeparatorText(i18n::Tr("Journal"));
      if (ro::RoCheckbox(i18n::Tr("Fenêtre de logs"),
                         &Bourgeon::Instance().show_log_window()))
        SaveSettings();
      ImGui::SameLine();
      HelpMarker(
          i18n::Tr("Miroir en jeu de tout ce que le client journalise "
          "(LogInfo / LogDiag / LogError), à la place de la console Windows.\n\n"
          "Le texte est SÉLECTIONNABLE et copiable : sélection à la souris, "
          "Ctrl+A, Ctrl+C, ou le bouton « Copier tout ». Un champ de filtre "
          "restreint l'affichage à une sous-chaîne.\n\n"
          "Réservé au staff, et le droit est revérifié à chaque frame : la "
          "fenêtre disparaît si le niveau de groupe change en cours de session."));

      // ── Glyphes coréens ────────────────────────────────────────────────
      // Ici, à côté du journal, parce que c'est SON usage : lire les chemins
      // des fichiers du jeu (« 유저인터페이스\… ») quand on débogue.
      //
      // 🔴 Le hangul pèse 11 172 glyphes, soit 97 % de l'atlas de polices, pour
      // des caractères qu'aucun joueur ne voit — le jeu est en français et en
      // anglais. Il n'est donc plus chargé par défaut.
      if (ro::RoCheckbox(i18n::Tr("Glyphes coréens (redémarrage)"), &g_korean_glyphs))
        SaveSettings();
      ImGui::SameLine();
      HelpMarker(
          i18n::Tr("Charge les caractères coréens dans les polices. Utile UNIQUEMENT "
          "pour lire les chemins des fichiers du jeu dans le journal — rien "
          "en jeu ne s'affiche en coréen.\n\n"
          "⚠ Prend effet au PROCHAIN LANCEMENT : les polices sont préparées "
          "une seule fois au démarrage, et le moteur DirectDraw ne sait pas "
          "les refaire en cours de partie.\n\n"
          "Éteint, l'atlas de polices est vingt fois plus léger et le client "
          "démarre plus vite. Un caractère coréen y apparaîtrait en carré."));

      PopStyleCompact();
    }

    moonlight_ui::DrawRules();

    // ── DPS Meter, Doom, Roggle, Rojeweled, Jump, QSDZ ───────────────────
    DrawFunPanels();

    DrawInterfacePanel();
    // ── Graphismes (color grading post-process, ScreenFx plugin) ───────
    if (CollapsingHeader(i18n::Tr("Graphismes"))) {
      PushStyleCompact();
      if (auto* screen_fx = Bourgeon::Instance().screen_fx())
        screen_fx->DrawSettings();

      if (auto* weapon_dual_sprites = Bourgeon::Instance().weapon_dual_sprites()) {
        if (ro::RoCheckbox(i18n::Tr("Sprites d'armes doubles"), &weapon_dual_sprites->enabled()))
          SaveSettings();
        SameLine(); HelpMarker(
            i18n::Tr("Affiche le sprite/l'animation PROPRE à chaque arme quand tu portes "
            "deux armes (assassin, kagerou/oboro) ou une seule arme en main "
            "gauche.\n\nOFF (défaut) : le client fond les deux armes en un sprite "
            "générique. ON : chaque arme garde son apparence d'origine."));
      }
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
