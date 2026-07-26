#include "plugins/moonlight_ui.h"

#include "plugins/moonlight_ui/internal.h"  // panneaux extraits (dossier privé)

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

// ── Presets de skin RO (jeux de couleurs nommés, sauvegardés dans le yaml) ──────
// Presets de skin RO : le TYPE et les deux globales sont déclarés dans
// moonlight_ui/internal.h — le panneau « Skin RO » (panel_interface.cc) les lit,
// LoadSettings/SaveSettings les écrivent. Définis ici.
std::vector<RoPreset> g_ro_presets;
int g_ro_preset_sel = -1;

namespace {

// ── Couleurs persistées ──────────────────────────────────────────────────────
// Deux formats coexistent sur disque, hérités et désormais FIGÉS — les unifier
// invaliderait les bourgeon_settings.yaml déjà chez les joueurs :
//   • chaîne hex ARGB « AARRGGBB » — chat_bg, dps, expbar, portrait, grid,
//     skillbar, ground_paint, presets de chat ;
//   • entier décimal ImU32        — statusicon_*, ro_skin_* et presets de skin.
// Les quatre fonctions ci-dessous sont le seul endroit du projet qui connaît
// cette dualité ; ailleurs on nomme l'encodage (cf. ui/color_codec.h).

// Laisse `picker_rgba` INTACT si la clé est absente, vide ou corrompue : la
// valeur par défaut du plugin survit. Avant, un std::stoul lançait sur une clé
// corrompue et l'exception remontait jusqu'au catch qui enveloppe TOUT
// LoadSettings — une seule couleur illisible faisait perdre le reste de la
// configuration. Rend true si la couleur a été appliquée.
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
      if (q1 != std::string::npos && q2 != std::string::npos && q1 < q2)
        item_names_[current_id] = line.substr(q1 + 1, q2 - q1 - 1);
    }
  }
  // LogInfo("[MoonlightUi] loaded {} item names", item_names_.size());
}

MoonlightUi::MoonlightUi() {
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeFromServer);
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodePresetList);
  // Observe the standard map-move packet to learn the current map name.
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
  if (!f) return;  // first run — no file yet

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
      // walk_heap = FALSE ici, à dessein : LoadSettings tourne au login, pendant
      // le chargement de map, alors qu'AUCUNE fenêtre de chat n'existe encore.
      // Le parcours du tas — trois fois par login, sous HeapLock — ne trouvait
      // donc jamais rien. Patcher les immédiats .text suffit : les fenêtres
      // créées ensuite prendront la couleur à leur construction.
      if (!g.instrs.empty()) ApplyChatBg(g, argb, false);
    }

    // « Sol uni » du SPR Lab (fond de capture) : couleur en ARGB hex, même convention
    // que les autres couleurs persistées ici.
    spr_lab::ground_paint_enabled() = ui["ground_paint"].as<bool>(false);
    ReadArgbKey(ui, "ground_paint_color", spr_lab::ground_color());
    ui_collapsed_         = ui["ui_collapsed"].as<bool>(false);
    show_alootid_overlay_ = ui["alootid_overlay"].as<bool>(false);
    if (auto* idt = Bourgeon::Instance().item_desc()) {
      idt->show_item_panel()  = ui["itemdesc_show_item"].as<bool>(true);
      idt->show_skill_panel() = ui["itemdesc_show_skill"].as<bool>(true);
      idt->cmp_show_equipped() = ui["itemdesc_compare"].as<bool>(true);
      idt->desc_spawn_at_cursor() =
          ui["itemdesc_spawn_cursor"].as<bool>(true);
      // Défaut 3 = bas-droite, la valeur déclarée dans item_desc_tweaks.h:208.
      // Elle valait 0 ici et dans le repli d'écriture : tout yaml antérieur à ce
      // réglage faisait repasser l'ancrage en haut-gauche, silencieusement.
      idt->desc_anchor()   = ui["itemdesc_anchor"].as<int>(3);
      idt->desc_offset_x() = ui["itemdesc_off_x"].as<int>(12);
      idt->desc_offset_y() = ui["itemdesc_off_y"].as<int>(12);
    }
    if (auto* br = Bourgeon::Instance().bug_report())
      br->enabled() = ui["bugreport_button"].as<bool>(true);
    if (auto* wds = Bourgeon::Instance().weapon_dual_sprites())
      wds->enabled() = ui["weapon_dual_sprites"].as<bool>(false);
    mainchat_preset_bar_  = ui["mainchat_preset_bar"].as<bool>(false);
    log_level_            = ui["log_level"].as<std::string>("info");

    chat_width_enabled_ = ui["chat_width_enabled"].as<bool>(false);
    chat_width_px_      = ui["chat_width"].as<int>(800);
    if (chat_width_px_ < 320)  chat_width_px_ = 320;
    if (chat_width_px_ > 1200) chat_width_px_ = 1200;
    chat::SetCustomWidth(chat_width_enabled_, chat_width_px_);
    chat_timestamps_ = ui["chat_timestamps"].as<bool>(false);
    chat::SetTimestamps(chat_timestamps_);
    chat_item_icons_ = ui["chat_item_icons"].as<bool>(true);
    chat::SetItemIcons(chat_item_icons_);
    LogConsole::instance().SetLevel(log_level_);
    apply_collapse_ = true;

    if (auto* dps = Bourgeon::Instance().dps_meter()) {
      dps->show_ground_dmg_in_chat_ = ui["dps_ground_dmg_chat"].as<bool>(true);
      dps->locked_ = ui["dps_locked"].as<bool>(false);
      dps->bg_alpha_ = ui["dps_bg_alpha"].as<float>(0.90f);
      ReadArgbKey(ui, "dps_text_color", dps->text_color_);
      ReadArgbKey(ui, "dps_plot_color", dps->plot_color_);
      dps->visible_             = ui["dps_visible"].as<bool>(true);
      dps->slot_ms_             = ui["dps_slot_ms"].as<int>(200);
      dps->dps_window_secs_     = ui["dps_window_secs"].as<int>(10);
      dps->combat_timeout_secs_ = ui["dps_combat_timeout_secs"].as<int>(5);
    }

    if (auto* eb = Bourgeon::Instance().basic_info()) {
      eb->visible_   = ui["expbar_visible"].as<bool>(false);
      eb->locked_    = ui["expbar_locked"].as<bool>(false);
      eb->sticky_    = ui["expbar_sticky"].as<bool>(false);
      eb->text_mode_ = ui["expbar_text"].as<int>(1);
      eb->vertical_  = ui["expbar_vertical"].as<bool>(false);
      eb->border_    = ui["expbar_border"].as<bool>(true);
      eb->rounding_  = ui["expbar_rounding"].as<float>(4.0f);
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

      // Status portrait (part of the Basic Info tweaks): per-element layout.
      eb->portrait_visible_         = ui["portrait_visible"].as<bool>(false);
      eb->portrait_locked_          = ui["portrait_locked"].as<bool>(false);
      eb->portrait_hide_basic_info_ = ui["portrait_hide_basic_info"].as<bool>(false);
      eb->portrait_border_          = ui["portrait_border"].as<bool>(false);
      eb->portrait_head_sprite_     = ui["portrait_head_sprite"].as<bool>(true);
      eb->portrait_head_only_       = ui["portrait_head_only"].as<bool>(true);
      eb->portrait_debug_log_       = ui["portrait_debug_log"].as<bool>(false);
      eb->portrait_head_zoom_       = ui["portrait_head_zoom"].as<float>(1.0f);
      eb->portrait_head_offx_       = ui["portrait_head_offx"].as<float>(0.0f);
      eb->portrait_head_offy_       = ui["portrait_head_offy"].as<float>(0.0f);
      eb->portrait_anim_            = ui["portrait_anim"].as<int>(4);
      eb->portrait_dir_             = ui["portrait_dir"].as<int>(0);
      eb->portrait_animate_         = ui["portrait_animate"].as<bool>(true);
      eb->portrait_show_garment_    = ui["portrait_show_garment"].as<bool>(true);
      // Hat effects (.str) : rendu automatique et toujours actif (aucun réglage persisté).
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
    g_ro_presets.clear();
    if (const YAML::Node ps = ui["ro_skin_presets"]) {
      for (auto it = ps.begin(); it != ps.end(); ++it) {
        RoPreset p;
        p.name = (*it)["name"].as<std::string>("");
        if (p.name.empty()) continue;
        ReadSkinCfg(*it, p.cfg, "", /*with_rounding=*/false);
        g_ro_presets.push_back(std::move(p));
      }
    }
    // Starter set au 1er lancement (aucun preset sauvegardé) : donne des thèmes
    // de départ que les joueurs peuvent Appliquer puis modifier.
    if (g_ro_presets.empty()) {
      auto setc = [](float* c, int r, int g, int b) {
        c[0] = r / 255.f; c[1] = g / 255.f; c[2] = b / 255.f; c[3] = 1.f;
      };
      g_ro_presets.push_back({"RO Classique", ro::RoSkinConfig{}});  // défauts natifs
      {
        ro::RoSkinConfig d;
        d.title_brightness = 0.90f;
        setc(d.body_col, 44, 46, 54);
        setc(d.border_col, 90, 94, 110);
        setc(d.body_text, 226, 228, 235);
        setc(d.title_text, 255, 255, 255);
        setc(d.tab_col, 90, 120, 190);
        setc(d.tab_inact, 70, 74, 86);
        setc(d.input_col, 64, 66, 76);
        setc(d.header_col, 58, 60, 70);
        setc(d.slot_col, 64, 66, 76);
        setc(d.doll_col, 54, 56, 66);
        setc(d.card_col, 54, 56, 66);
        setc(d.card_head_col, 34, 36, 44);
        setc(d.card_head_text, 226, 228, 235);
        g_ro_presets.push_back({"Sombre", d});
      }
      {
        ro::RoSkinConfig d;
        setc(d.body_col, 244, 236, 218);
        setc(d.border_col, 176, 150, 110);
        setc(d.body_text, 60, 44, 24);
        setc(d.title_text, 40, 28, 12);
        setc(d.tab_col, 196, 166, 120);
        setc(d.tab_inact, 226, 214, 190);
        setc(d.input_col, 232, 222, 200);
        setc(d.header_col, 224, 210, 184);
        setc(d.slot_col, 232, 222, 200);
        setc(d.doll_col, 240, 232, 214);
        setc(d.card_col, 250, 244, 228);
        setc(d.card_head_col, 150, 120, 80);
        setc(d.card_head_text, 250, 244, 230);
        g_ro_presets.push_back({"Sepia", d});
      }
    }
    if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
      iv->imgui_enabled_ = ui["inventory_imgui"].as<bool>(iv->imgui_enabled_);
      iv->show_filter()   = ui["inventory_filter"].as<bool>(iv->show_filter());
      iv->desc_tooltip()  =
          ui["inventory_desc_tooltip"].as<bool>(iv->desc_tooltip());
      iv->tabs_vertical() =
          ui["inventory_tabs_vertical"].as<bool>(iv->tabs_vertical());
      iv->lock_size()   = ui["inventory_lock_size"].as<bool>(iv->lock_size());
      iv->free_layout() = ui["inventory_free_layout"].as<bool>(iv->free_layout());
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
    if (auto* stg = Bourgeon::Instance().storage_tweaks()) {
      stg->imgui_enabled_ = ui["storage_imgui"].as<bool>(stg->imgui_enabled_);
      stg->desc_tooltip() =
          ui["storage_desc_tooltip"].as<bool>(stg->desc_tooltip());
      stg->show_filter()    = ui["storage_filter"].as<bool>(stg->show_filter());
      stg->tabs_vertical()  = ui["storage_tabs_vertical"].as<bool>(stg->tabs_vertical());
      stg->tab_images()     = ui["storage_tab_images"].as<bool>(stg->tab_images());
      stg->show_index_col() = ui["storage_col_index"].as<bool>(stg->show_index_col());
      stg->show_id_col()    = ui["storage_col_id"].as<bool>(stg->show_id_col());
      stg->show_slots_col() = ui["storage_col_slots"].as<bool>(stg->show_slots_col());
      stg->show_value_col() = ui["storage_col_value"].as<bool>(stg->show_value_col());
      stg->show_total_value() =
          ui["storage_total_value"].as<bool>(stg->show_total_value());
      stg->cur_tab()        = ui["storage_tab"].as<int>(stg->cur_tab());
      // Favoris storage (client-side, keyés par id d'item).
      if (const YAML::Node favs = ui["storage_favorites"]) {
        stg->favorites_.clear();
        for (const YAML::Node& f : favs) {
          const uint32_t id = f.as<uint32_t>(0);
          if (id != 0) stg->favorites_.insert(id);
        }
      }
    }
    if (auto* cs = Bourgeon::Instance().cashshop_tweaks())
      cs->imgui_enabled_ = ui["cashshop_imgui"].as<bool>(cs->imgui_enabled_);
    if (auto* sh = Bourgeon::Instance().shop_tweaks())
      sh->imgui_enabled_ = ui["shop_imgui"].as<bool>(sh->imgui_enabled_);
    if (auto* tt = Bourgeon::Instance().trade_tweaks())
      tt->imgui_enabled_ = ui["trade_imgui"].as<bool>(tt->imgui_enabled_);
    if (auto* nd = Bourgeon::Instance().npc_dialog_tweaks()) {
      nd->imgui_enabled_ = ui["npc_dialog_imgui"].as<bool>(nd->imgui_enabled_);
      nd->menu_search_ = ui["npc_menu_search"].as<bool>(nd->menu_search_);
    }
    if (auto* cse = Bourgeon::Instance().character_sheet()) {
      cse->imgui_enabled_ = ui["charsheet_imgui"].as<bool>(cse->imgui_enabled_);
      cse->set_open(ui["charsheet_open"].as<bool>(cse->is_open()));
      // Pose de l'avatar (pose/direction/animation) — persistee par personne.
      cse->avatar_anim()    = ui["charsheet_pose"].as<int>(cse->avatar_anim());
      cse->avatar_dir()     = ui["charsheet_dir"].as<int>(cse->avatar_dir());
      cse->avatar_animate() = ui["charsheet_pose_anim"].as<bool>(cse->avatar_animate());
    }
    if (auto* lp = Bourgeon::Instance().login_parade()) {
      lp->enabled_ = ui["login_parade"].as<bool>(lp->enabled_);
    }
    if (auto* sb = Bourgeon::Instance().skill_bar()) {
      sb->enabled_    = ui["skillbar_enabled"].as<bool>(sb->enabled_);
      sb->locked_     = ui["skillbar_locked"].as<bool>(sb->locked_);
      sb->bilinear_   = ui["skillbar_bilinear"].as<bool>(sb->bilinear_);
      sb->clickthrough_ = ui["skillbar_clickthrough"].as<bool>(sb->clickthrough_);
      sb->show_keys_  = ui["skillbar_show_keys"].as<bool>(sb->show_keys_);
      sb->bold_text_  = ui["skillbar_bold_text"].as<bool>(sb->bold_text_);
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
      ReadArgbKey(ui, "skillbar_col_frame",    sb->col_frame_);
      ReadArgbKey(ui, "skillbar_col_skill",    sb->col_skill_);
      ReadArgbKey(ui, "skillbar_col_item",     sb->col_item_);
      ReadArgbKey(ui, "skillbar_col_empty",    sb->col_empty_);
      ReadArgbKey(ui, "skillbar_col_border",   sb->col_border_);
      ReadArgbKey(ui, "skillbar_col_borderhi", sb->col_borderhi_);
      ReadArgbKey(ui, "skillbar_col_keytext",  sb->col_keytext_);
      ReadArgbKey(ui, "skillbar_col_count",    sb->col_count_);
      ReadArgbKey(ui, "skillbar_col_textout",  sb->col_textout_);
    }

    // « Tout-ImGui ou tout-natif » : ces 4 fenêtres (inventaire/storage/barres/
    // échange) s'activent ensemble. Un yaml antérieur au regroupement pouvait être
    // mixé — on réconcilie en OR (au moins une moderne => toutes modernes ; tout
    // natif sinon), puis les cases restent synchronisées à l'exécution.
    {
      auto* iv  = Bourgeon::Instance().inventory_viewer();
      auto* stg = Bourgeon::Instance().storage_tweaks();
      auto* sb2 = Bourgeon::Instance().skill_bar();
      auto* tt2 = Bourgeon::Instance().trade_tweaks();
      const bool modern = (iv  && iv->imgui_enabled_)  ||
                          (stg && stg->imgui_enabled_) ||
                          (sb2 && sb2->enabled_)       ||
                          (tt2 && tt2->imgui_enabled_);
      SetModernInterface(modern);
    }

    if (auto* si = Bourgeon::Instance().status_icons()) {
      StatusIconConfig& c = si->config();
      c.enabled        = ui["statusicon_enabled"].as<bool>(c.enabled);
      c.corner         = ui["statusicon_corner"].as<int>(c.corner);
      c.margin_x       = ui["statusicon_margin_x"].as<int>(c.margin_x);
      c.margin_y       = ui["statusicon_margin_y"].as<int>(c.margin_y);
      c.step_dir       = ui["statusicon_step_dir"].as<int>(c.step_dir);
      c.wrap_dir       = ui["statusicon_wrap_dir"].as<int>(c.wrap_dir);
      c.per_line       = ui["statusicon_per_line"].as<int>(c.per_line);
      c.icon_pitch     = ui["statusicon_icon_pitch"].as<int>(c.icon_pitch);
      c.line_pitch     = ui["statusicon_line_pitch"].as<int>(c.line_pitch);
      c.sort_mode      = ui["statusicon_sort_mode"].as<int>(c.sort_mode);
      c.show_remaining = ui["statusicon_show_remaining"].as<bool>(c.show_remaining);
      c.time_bg        = ui["statusicon_time_bg"].as<bool>(c.time_bg);
      c.icon_alpha     = ui["statusicon_icon_alpha"].as<int>(c.icon_alpha);
      c.icon_size      = ui["statusicon_icon_size"].as<int>(c.icon_size);
      c.time_place     = ui["statusicon_time_place"].as<int>(c.time_place);
      c.time_anchor    = ui["statusicon_time_anchor"].as<int>(c.time_anchor);
      c.time_bold      = ui["statusicon_time_bold"].as<bool>(c.time_bold);
      // Ces deux-là sont persistées en ImU32 décimal (pas en hex ARGB) : le
      // nom de la fonction le dit, ne pas y appliquer ReadArgbKey.
      if (ui["statusicon_time_text"])
        ro::PickerFromImU32(ui["statusicon_time_text"].as<unsigned>(0), c.col_time_text);
      if (ui["statusicon_time_shadow"])
        ro::PickerFromImU32(ui["statusicon_time_shadow"].as<unsigned>(0), c.col_time_shadow);
      si->MarkDirty();
    }

    if (auto* qt = Bourgeon::Instance().quest_tracker()) {
      QuestTrackerConfig& c = qt->config();
      c.enabled        = ui["questtracker_enabled"].as<bool>(c.enabled);
      c.show_titlebar  = ui["questtracker_show_titlebar"].as<bool>(c.show_titlebar);
      c.locked         = ui["questtracker_locked"].as<bool>(c.locked);
      c.pos_x          = ui["questtracker_pos_x"].as<int>(c.pos_x);
      c.pos_y          = ui["questtracker_pos_y"].as<int>(c.pos_y);
      c.width          = ui["questtracker_width"].as<int>(c.width);
      c.max_quests     = ui["questtracker_max_quests"].as<int>(c.max_quests);
      c.title_rgb      = ui["questtracker_title_rgb"].as<int>(c.title_rgb);
      c.desc_rgb       = ui["questtracker_desc_rgb"].as<int>(c.desc_rgb);
      c.hunt_rgb       = ui["questtracker_hunt_rgb"].as<int>(c.hunt_rgb);
      c.font_scale     = ui["questtracker_font_scale"].as<int>(c.font_scale);
      c.show_bg        = ui["questtracker_show_bg"].as<bool>(c.show_bg);
      c.bg_alpha       = ui["questtracker_bg_alpha"].as<int>(c.bg_alpha);
      c.show_objective = ui["questtracker_show_objective"].as<bool>(c.show_objective);
    }

    if (auto* st = Bourgeon::Instance().settings_tweaks()) {
      D3D9PostFx& g = st->fx();
      g.enabled     = ui["fx_enabled"].as<bool>(g.enabled);
      g.brightness  = ui["fx_brightness"].as<float>(g.brightness);
      g.contrast    = ui["fx_contrast"].as<float>(g.contrast);
      g.gamma       = ui["fx_gamma"].as<float>(g.gamma);
      g.saturation  = ui["fx_saturation"].as<float>(g.saturation);
      g.temperature = ui["fx_temperature"].as<float>(g.temperature);
      g.filter      = ui["fx_filter"].as<int>(g.filter);
      g.vignette    = ui["fx_vignette"].as<float>(g.vignette);
      g.grain       = ui["fx_grain"].as<float>(g.grain);
      g.aberration  = ui["fx_aberration"].as<float>(g.aberration);
      g.sharpen     = ui["fx_sharpen"].as<float>(g.sharpen);
      g.fxaa        = ui["fx_fxaa"].as<bool>(g.fxaa);
      g.fxaa_strength = ui["fx_fxaa_strength"].as<float>(g.fxaa_strength);
      st->fps_overlay() = ui["fps_overlay"].as<bool>(false);
      st->zoom_enabled() = ui["cam_zoom_enabled"].as<bool>(false);
      st->zoom_factor()  = ui["cam_zoom_factor"].as<float>(1.0f);
      st->zoom_speed()   = ui["cam_zoom_speed"].as<float>(1.0f);
      st->tex_filter()   = ui["tex_filter"].as<int>(0);
      st->gopt_x()       = ui["game_option_pos_x"].as<int>(INT_MIN);
      st->gopt_y()       = ui["game_option_pos_y"].as<int>(INT_MIN);
      st->esc_x()        = ui["esc_option_pos_x"].as<int>(INT_MIN);
      st->esc_y()        = ui["esc_option_pos_y"].as<int>(INT_MIN);
      st->Apply();  // push to the d3d9 post-process layer
    }

    if (auto* en = Bourgeon::Instance().entity_names()) {
      en->enabled()       = ui["entnames_enabled"].as<bool>(false);
      en->show_players()  = ui["entnames_players"].as<bool>(true);
      en->show_monsters() = ui["entnames_monsters"].as<bool>(false);
      en->show_npcs()     = ui["entnames_npcs"].as<bool>(false);
      en->show_self()     = ui["entnames_self"].as<bool>(false);
      en->outline()       = ui["entnames_outline"].as<bool>(true);
      en->y_offset()      = ui["entnames_yoffset"].as<int>(2);
      en->font_scale()    = ui["entnames_fontscale"].as<float>(1.0f);
    }

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
  } catch (const std::exception& e) {
    LogError("[MoonlightUi] failed to parse {}: {}", path, e.what());
  }
}

void MoonlightUi::WriteSettingsFile() {
  // Ces trois-là gardent une chaîne pré-calculée : elles ont un repli littéral à
  // écrire quand le plugin propriétaire est absent, que WriteArgbKey ne sait pas
  // exprimer (il part forcément d'un picker).
  auto* dps = Bourgeon::Instance().dps_meter();
  std::string dps_text_col = "FFFFCC33", dps_plot_col = "FFFFCC33";
  if (dps) {
    dps_text_col = HexArgb(dps->text_color_);
    dps_plot_col = HexArgb(dps->plot_color_);
  }

  auto* eb = Bourgeon::Instance().basic_info();
  std::string eb_bg_col = "B30D0D12";
  if (eb) eb_bg_col = HexArgb(eb->bg_color_);
  // Global alignment grid colour (owned by MoonlightUi, not basic_info).
  const std::string grid_col = HexArgb(grid_.color);

  // ItemDescTweaks toggles (owned by the plugin) — panels + Comparer + placement.
  bool itemdesc_show_item = true, itemdesc_show_skill = true;
  bool itemdesc_compare = true, itemdesc_spawn_cursor = true;
  int  itemdesc_anchor = 3, itemdesc_off_x = 12, itemdesc_off_y = 12;  // cf. item_desc_tweaks.h
  if (auto* idt = Bourgeon::Instance().item_desc()) {
    itemdesc_show_item    = idt->show_item_panel();
    itemdesc_show_skill   = idt->show_skill_panel();
    itemdesc_compare      = idt->cmp_show_equipped();
    itemdesc_spawn_cursor = idt->desc_spawn_at_cursor();
    itemdesc_anchor       = idt->desc_anchor();
    itemdesc_off_x        = idt->desc_offset_x();
    itemdesc_off_y        = idt->desc_offset_y();
  }
  bool bugreport_button = true;
  if (auto* br = Bourgeon::Instance().bug_report())
    bugreport_button = br->enabled();

  YAML::Emitter out;
  out << YAML::BeginMap
      << YAML::Key << "moonlight_ui"
      << YAML::Value << YAML::BeginMap;
  for (const ChatBgGroup& g : chat_bg_) WriteArgbKey(out, g.yaml_key, g.color);
  out     << YAML::Key << "ground_paint"         << YAML::Value
              << spr_lab::ground_paint_enabled();
  WriteArgbKey(out, "ground_paint_color", spr_lab::ground_color());
  out     << YAML::Key << "ui_collapsed"          << YAML::Value << ui_collapsed_
        << YAML::Key << "log_level"            << YAML::Value << log_level_
        << YAML::Key << "alootid_overlay"      << YAML::Value << show_alootid_overlay_
        << YAML::Key << "itemdesc_show_item"   << YAML::Value << itemdesc_show_item
        << YAML::Key << "itemdesc_show_skill"  << YAML::Value << itemdesc_show_skill
        << YAML::Key << "itemdesc_compare"     << YAML::Value << itemdesc_compare
        << YAML::Key << "itemdesc_spawn_cursor" << YAML::Value << itemdesc_spawn_cursor
        << YAML::Key << "itemdesc_anchor"      << YAML::Value << itemdesc_anchor
        << YAML::Key << "itemdesc_off_x"       << YAML::Value << itemdesc_off_x
        << YAML::Key << "itemdesc_off_y"       << YAML::Value << itemdesc_off_y
        << YAML::Key << "bugreport_button"     << YAML::Value << bugreport_button
        << YAML::Key << "weapon_dual_sprites"  << YAML::Value
            << (Bourgeon::Instance().weapon_dual_sprites()
                    ? Bourgeon::Instance().weapon_dual_sprites()->enabled()
                    : false)
        << YAML::Key << "mainchat_preset_bar"  << YAML::Value << mainchat_preset_bar_
        << YAML::Key << "chat_width_enabled"   << YAML::Value << chat_width_enabled_
        << YAML::Key << "chat_width"           << YAML::Value << chat_width_px_
        << YAML::Key << "chat_timestamps"      << YAML::Value << chat_timestamps_
        << YAML::Key << "chat_item_icons"      << YAML::Value << chat_item_icons_
        << YAML::Key << "dps_ground_dmg_chat"  << YAML::Value
            << (Bourgeon::Instance().dps_meter()
                    ? Bourgeon::Instance().dps_meter()->show_ground_dmg_in_chat_
                    : true)
        << YAML::Key << "dps_locked" << YAML::Value
            << (Bourgeon::Instance().dps_meter()
                    ? Bourgeon::Instance().dps_meter()->locked_
                    : false)
        << YAML::Key << "dps_bg_alpha"   << YAML::Value << (dps ? dps->bg_alpha_ : 0.90f)
        << YAML::Key << "dps_text_color" << YAML::Value << dps_text_col
        << YAML::Key << "dps_plot_color" << YAML::Value << dps_plot_col
        << YAML::Key << "dps_visible"             << YAML::Value << (dps ? dps->visible_ : true)
        << YAML::Key << "dps_slot_ms"             << YAML::Value << (dps ? dps->slot_ms_ : 200)
        << YAML::Key << "dps_window_secs"         << YAML::Value << (dps ? dps->dps_window_secs_ : 10)
        << YAML::Key << "dps_combat_timeout_secs" << YAML::Value << (dps ? dps->combat_timeout_secs_ : 5);

  // EXP/HP/SP bar settings (BasicInfoTweaks)
  out << YAML::Key << "expbar_visible"  << YAML::Value << (eb ? eb->visible_ : false)
      << YAML::Key << "expbar_locked"   << YAML::Value << (eb ? eb->locked_ : false)
      << YAML::Key << "expbar_sticky"   << YAML::Value << (eb ? eb->sticky_ : false)
      << YAML::Key << "expbar_text"     << YAML::Value << (eb ? eb->text_mode_ : 1)
      << YAML::Key << "expbar_vertical" << YAML::Value << (eb ? eb->vertical_ : false)
      << YAML::Key << "expbar_border"   << YAML::Value << (eb ? eb->border_ : true)
      << YAML::Key << "expbar_rounding" << YAML::Value << (eb ? eb->rounding_ : 4.0f)
      << YAML::Key << "expbar_bg_color" << YAML::Value << eb_bg_col
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

  // Status portrait settings (part of BasicInfoTweaks): per-element layout.
  if (eb) {
    out << YAML::Key << "portrait_visible"         << YAML::Value << eb->portrait_visible_
        << YAML::Key << "portrait_locked"          << YAML::Value << eb->portrait_locked_
        << YAML::Key << "portrait_hide_basic_info" << YAML::Value << eb->portrait_hide_basic_info_
        << YAML::Key << "portrait_border"          << YAML::Value << eb->portrait_border_
        << YAML::Key << "portrait_head_sprite"     << YAML::Value << eb->portrait_head_sprite_
        << YAML::Key << "portrait_head_only"       << YAML::Value << eb->portrait_head_only_
        << YAML::Key << "portrait_debug_log"       << YAML::Value << eb->portrait_debug_log_
        << YAML::Key << "portrait_head_zoom"       << YAML::Value << eb->portrait_head_zoom_
        << YAML::Key << "portrait_head_offx"       << YAML::Value << eb->portrait_head_offx_
        << YAML::Key << "portrait_head_offy"       << YAML::Value << eb->portrait_head_offy_
        << YAML::Key << "portrait_anim"            << YAML::Value << eb->portrait_anim_
        << YAML::Key << "portrait_dir"             << YAML::Value << eb->portrait_dir_
        << YAML::Key << "portrait_animate"         << YAML::Value << eb->portrait_animate_
        << YAML::Key << "portrait_show_garment"    << YAML::Value << eb->portrait_show_garment_;
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

  {
    auto* si = Bourgeon::Instance().status_icons();
    const StatusIconConfig c = si ? si->config() : StatusIconConfig{};
    out << YAML::Key << "statusicon_enabled"        << YAML::Value << c.enabled
        << YAML::Key << "statusicon_corner"         << YAML::Value << c.corner
        << YAML::Key << "statusicon_margin_x"       << YAML::Value << c.margin_x
        << YAML::Key << "statusicon_margin_y"       << YAML::Value << c.margin_y
        << YAML::Key << "statusicon_step_dir"       << YAML::Value << c.step_dir
        << YAML::Key << "statusicon_wrap_dir"       << YAML::Value << c.wrap_dir
        << YAML::Key << "statusicon_per_line"       << YAML::Value << c.per_line
        << YAML::Key << "statusicon_icon_pitch"     << YAML::Value << c.icon_pitch
        << YAML::Key << "statusicon_line_pitch"     << YAML::Value << c.line_pitch
        << YAML::Key << "statusicon_sort_mode"      << YAML::Value << c.sort_mode
        << YAML::Key << "statusicon_show_remaining" << YAML::Value << c.show_remaining
        << YAML::Key << "statusicon_time_bg"        << YAML::Value << c.time_bg
        << YAML::Key << "statusicon_icon_alpha"     << YAML::Value << c.icon_alpha
        << YAML::Key << "statusicon_icon_size"      << YAML::Value << c.icon_size
        << YAML::Key << "statusicon_time_place"     << YAML::Value << c.time_place
        << YAML::Key << "statusicon_time_anchor"    << YAML::Value << c.time_anchor
        << YAML::Key << "statusicon_time_bold"      << YAML::Value << c.time_bold
        // Persistées en ImU32 décimal, pas en hex ARGB (cf. ReadArgbKey).
        << YAML::Key << "statusicon_time_text"   << YAML::Value
            << ro::ImU32FromPicker(c.col_time_text)
        << YAML::Key << "statusicon_time_shadow" << YAML::Value
            << ro::ImU32FromPicker(c.col_time_shadow);
  }

  {
    auto* qt = Bourgeon::Instance().quest_tracker();
    const QuestTrackerConfig c = qt ? qt->config() : QuestTrackerConfig{};
    out << YAML::Key << "questtracker_enabled"        << YAML::Value << c.enabled
        << YAML::Key << "questtracker_show_titlebar"  << YAML::Value << c.show_titlebar
        << YAML::Key << "questtracker_locked"         << YAML::Value << c.locked
        << YAML::Key << "questtracker_pos_x"          << YAML::Value << c.pos_x
        << YAML::Key << "questtracker_pos_y"          << YAML::Value << c.pos_y
        << YAML::Key << "questtracker_width"          << YAML::Value << c.width
        << YAML::Key << "questtracker_max_quests"     << YAML::Value << c.max_quests
        << YAML::Key << "questtracker_title_rgb"      << YAML::Value << c.title_rgb
        << YAML::Key << "questtracker_desc_rgb"       << YAML::Value << c.desc_rgb
        << YAML::Key << "questtracker_hunt_rgb"       << YAML::Value << c.hunt_rgb
        << YAML::Key << "questtracker_font_scale"     << YAML::Value << c.font_scale
        << YAML::Key << "questtracker_show_bg"        << YAML::Value << c.show_bg
        << YAML::Key << "questtracker_bg_alpha"       << YAML::Value << c.bg_alpha
        << YAML::Key << "questtracker_show_objective" << YAML::Value << c.show_objective;
  }

  {
    auto* st = Bourgeon::Instance().settings_tweaks();
    const D3D9PostFx g = st ? st->fx() : D3D9PostFx{};
    out << YAML::Key << "fx_enabled"     << YAML::Value << g.enabled
        << YAML::Key << "fx_brightness"  << YAML::Value << g.brightness
        << YAML::Key << "fx_contrast"    << YAML::Value << g.contrast
        << YAML::Key << "fx_gamma"       << YAML::Value << g.gamma
        << YAML::Key << "fx_saturation"  << YAML::Value << g.saturation
        << YAML::Key << "fx_temperature" << YAML::Value << g.temperature
        << YAML::Key << "fx_filter"      << YAML::Value << g.filter
        << YAML::Key << "fx_vignette"    << YAML::Value << g.vignette
        << YAML::Key << "fx_grain"       << YAML::Value << g.grain
        << YAML::Key << "fx_aberration"  << YAML::Value << g.aberration
        << YAML::Key << "fx_sharpen"     << YAML::Value << g.sharpen
        << YAML::Key << "fx_fxaa"        << YAML::Value << g.fxaa
        << YAML::Key << "fx_fxaa_strength" << YAML::Value << g.fxaa_strength
        << YAML::Key << "fps_overlay"    << YAML::Value << (st ? st->fps_overlay() : false)
        << YAML::Key << "cam_zoom_enabled" << YAML::Value << (st ? st->zoom_enabled() : false)
        << YAML::Key << "cam_zoom_factor"  << YAML::Value << (st ? st->zoom_factor() : 1.0f)
        << YAML::Key << "cam_zoom_speed"   << YAML::Value << (st ? st->zoom_speed() : 1.0f)
        << YAML::Key << "tex_filter"       << YAML::Value << (st ? st->tex_filter() : 0)
        << YAML::Key << "game_option_pos_x" << YAML::Value << (st ? st->gopt_x() : INT_MIN)
        << YAML::Key << "game_option_pos_y" << YAML::Value << (st ? st->gopt_y() : INT_MIN)
        << YAML::Key << "esc_option_pos_x"  << YAML::Value << (st ? st->esc_x() : INT_MIN)
        << YAML::Key << "esc_option_pos_y"  << YAML::Value << (st ? st->esc_y() : INT_MIN);
  }

  {
    auto* en = Bourgeon::Instance().entity_names();
    out << YAML::Key << "entnames_enabled"   << YAML::Value << (en ? en->enabled() : false)
        << YAML::Key << "entnames_players"   << YAML::Value << (en ? en->show_players() : true)
        << YAML::Key << "entnames_monsters"  << YAML::Value << (en ? en->show_monsters() : false)
        << YAML::Key << "entnames_npcs"      << YAML::Value << (en ? en->show_npcs() : false)
        << YAML::Key << "entnames_self"      << YAML::Value << (en ? en->show_self() : false)
        << YAML::Key << "entnames_outline"   << YAML::Value << (en ? en->outline() : true)
        << YAML::Key << "entnames_yoffset"   << YAML::Value << (en ? en->y_offset() : 2)
        << YAML::Key << "entnames_fontscale" << YAML::Value << (en ? en->font_scale() : 1.0f);
  }

  {
    out << YAML::Key << "malgun_font" << YAML::Value << ro::IsFontEnabled();
    EmitSkinCfg(out, ro::SkinConfig(), "ro_skin_", /*with_rounding=*/true);
    out << YAML::Key << "ro_skin_presets" << YAML::Value << YAML::BeginSeq;
    for (const auto& p : g_ro_presets) {
      out << YAML::BeginMap << YAML::Key << "name" << YAML::Value << p.name;
      EmitSkinCfg(out, p.cfg, "", /*with_rounding=*/false);
      out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    auto* iv = Bourgeon::Instance().inventory_viewer();
    out << YAML::Key << "inventory_imgui" << YAML::Value << (iv ? iv->imgui_enabled_ : false);
    out << YAML::Key << "inventory_filter" << YAML::Value << (iv ? iv->show_filter() : true);
    out << YAML::Key << "inventory_desc_tooltip" << YAML::Value
        << (iv ? iv->desc_tooltip() : false);
    out << YAML::Key << "inventory_tabs_vertical" << YAML::Value
        << (iv ? iv->tabs_vertical() : true);
    out << YAML::Key << "inventory_lock_size" << YAML::Value << (iv ? iv->lock_size() : false);
    out << YAML::Key << "inventory_free_layout" << YAML::Value << (iv ? iv->free_layout() : false);
    // Placement libre : nameid -> case. Trié pour un yaml stable (pas de diff parasite).
    out << YAML::Key << "inventory_layout" << YAML::Value << YAML::Flow << YAML::BeginMap;
    if (iv) {
      std::vector<std::pair<uint32_t, int>> lay(iv->layout_.begin(), iv->layout_.end());
      std::sort(lay.begin(), lay.end());
      for (const auto& e : lay) out << YAML::Key << e.first << YAML::Value << e.second;
    }
    out << YAML::EndMap;
    auto* stg = Bourgeon::Instance().storage_tweaks();
    out << YAML::Key << "storage_imgui" << YAML::Value << (stg ? stg->imgui_enabled_ : false);
    out << YAML::Key << "storage_desc_tooltip" << YAML::Value
        << (stg ? stg->desc_tooltip() : false);
    out << YAML::Key << "storage_filter"    << YAML::Value << (stg ? stg->show_filter() : true);
    out << YAML::Key << "storage_tabs_vertical" << YAML::Value << (stg ? stg->tabs_vertical() : false);
    out << YAML::Key << "storage_tab_images" << YAML::Value << (stg ? stg->tab_images() : true);
    out << YAML::Key << "storage_col_index" << YAML::Value << (stg ? stg->show_index_col() : false);
    out << YAML::Key << "storage_col_id"    << YAML::Value << (stg ? stg->show_id_col() : false);
    out << YAML::Key << "storage_col_slots" << YAML::Value << (stg ? stg->show_slots_col() : false);
    out << YAML::Key << "storage_col_value" << YAML::Value << (stg ? stg->show_value_col() : true);
    out << YAML::Key << "storage_total_value" << YAML::Value << (stg ? stg->show_total_value() : true);
    out << YAML::Key << "storage_tab"       << YAML::Value << (stg ? stg->cur_tab() : 0);
    // Favoris storage (ids d'items, triés pour un yaml stable = pas de diff parasite).
    out << YAML::Key << "storage_favorites" << YAML::Value << YAML::Flow << YAML::BeginSeq;
    if (stg) {
      std::vector<uint32_t> favs(stg->favorites_.begin(), stg->favorites_.end());
      std::sort(favs.begin(), favs.end());
      for (uint32_t id : favs) out << id;
    }
    out << YAML::EndSeq;
    auto* cs = Bourgeon::Instance().cashshop_tweaks();
    out << YAML::Key << "cashshop_imgui" << YAML::Value << (cs ? cs->imgui_enabled_ : false);
    auto* sh = Bourgeon::Instance().shop_tweaks();
    out << YAML::Key << "shop_imgui" << YAML::Value << (sh ? sh->imgui_enabled_ : false);
    auto* tt = Bourgeon::Instance().trade_tweaks();
    out << YAML::Key << "trade_imgui" << YAML::Value << (tt ? tt->imgui_enabled_ : false);
    auto* nd = Bourgeon::Instance().npc_dialog_tweaks();
    out << YAML::Key << "npc_dialog_imgui" << YAML::Value
        << (nd ? nd->imgui_enabled_ : false);
    out << YAML::Key << "npc_menu_search" << YAML::Value
        << (nd ? nd->menu_search_ : true);
    auto* cse = Bourgeon::Instance().character_sheet();
    out << YAML::Key << "charsheet_imgui" << YAML::Value
        << (cse ? cse->imgui_enabled_ : false);
    out << YAML::Key << "charsheet_open" << YAML::Value
        << (cse ? cse->is_open() : true);
    // Pose de l'avatar (pose/direction/animation).
    out << YAML::Key << "charsheet_pose" << YAML::Value
        << (cse ? cse->avatar_anim() : 4);
    out << YAML::Key << "charsheet_dir" << YAML::Value
        << (cse ? cse->avatar_dir() : 0);
    out << YAML::Key << "charsheet_pose_anim" << YAML::Value
        << (cse ? cse->avatar_animate() : true);
    auto* lp = Bourgeon::Instance().login_parade();
    out << YAML::Key << "login_parade" << YAML::Value
        << (lp ? lp->enabled_ : true);
  }

  {
    auto* sb = Bourgeon::Instance().skill_bar();
    out << YAML::Key << "skillbar_enabled"  << YAML::Value << (sb ? sb->enabled_    : false)
        << YAML::Key << "skillbar_locked"   << YAML::Value << (sb ? sb->locked_     : true)
        << YAML::Key << "skillbar_bilinear" << YAML::Value << (sb ? sb->bilinear_   : false)
        << YAML::Key << "skillbar_clickthrough" << YAML::Value << (sb ? sb->clickthrough_ : false)
        << YAML::Key << "skillbar_show_keys" << YAML::Value << (sb ? sb->show_keys_ : true)
        << YAML::Key << "skillbar_bold_text" << YAML::Value << (sb ? sb->bold_text_ : false)
        << YAML::Key << "skillbar_key_scale" << YAML::Value << (sb ? sb->key_scale_ : 1.0f)
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
      WriteArgbKey(out, "skillbar_col_frame",    sb->col_frame_);
      WriteArgbKey(out, "skillbar_col_skill",    sb->col_skill_);
      WriteArgbKey(out, "skillbar_col_item",     sb->col_item_);
      WriteArgbKey(out, "skillbar_col_empty",    sb->col_empty_);
      WriteArgbKey(out, "skillbar_col_border",   sb->col_border_);
      WriteArgbKey(out, "skillbar_col_borderhi", sb->col_borderhi_);
      WriteArgbKey(out, "skillbar_col_keytext",  sb->col_keytext_);
      WriteArgbKey(out, "skillbar_col_count",    sb->col_count_);
      WriteArgbKey(out, "skillbar_col_textout",  sb->col_textout_);
    }
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
    // 0x0091 ZC_NPCACK_MAPMOVE: data points at mapname[16] (e.g. "gonryun.gat").
    const char* map_name = reinterpret_cast<const char*>(data);
    in_gonryun_ = in_game_ &&
                  (strncmp(map_name, kDiscordMap, sizeof(kDiscordMap) - 1) == 0);
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
        ImGui::TextDisabled("No presets yet.");
        ImGui::TextDisabled("Add some in the");
        ImGui::TextDisabled("Main chat picker.");
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
