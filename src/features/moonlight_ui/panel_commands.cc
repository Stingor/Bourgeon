#include "features/moonlight_ui/internal.h"

#include "bourgeon.h"
#include "imgui.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/log_console.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// « Commands Settings » : bascules serveur (onglet Général) et listes d'autoloot
// (onglet Autoloots). Extrait d'OnRenderUI — 435 lignes qui n'écrivaient que des
// membres de MoonlightUi et appelaient SendSetting / SendPresetCmd.
// MÉTHODE MEMBRE : cf. la note dans moonlight_ui.h, ce panneau manipule l'état
// privé (miroirs de réglages, presets alootid).
void MoonlightUi::DrawCommandsPanel() {
  if (CollapsingHeader(i18n::Tr("Commands Settings")))
  {
    PushStyleCompact();
    ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
    if (ImGui::BeginTabBar("CommandsSettingsTabs", tab_bar_flags))
    {
      if (ImGui::BeginTabItem(i18n::Tr("Général")))
      {
        if (ImGui::BeginTable("split", 2)) // Toggles settings
        {
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("Show EXP gain"), &show_exp_)) SendSetting(kSettingShowExp, show_exp_ ? 1 : 0);
          SameLine(); HelpMarker(i18n::Tr("Affiche le gain d'EXP dans le chat log. (@showexp)"));
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("Show Zeny gain"), &show_zeny_)) SendSetting(kSettingShowZeny, show_zeny_ ? 1 : 0);
          SameLine(); HelpMarker(i18n::Tr("Affiche le gain de Zeny dans le chat log. (@showzeny)"));
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("Show mob info"), &show_mob_info_)) SendSetting(kSettingShowMobInfo, show_mob_info_ ? 1 : 0);
          SameLine(); HelpMarker(i18n::Tr("Affiche la RACE et l'ÉLÉMENT des monstres,\nsous leur nom. (Thx Doo - @showmobinfo)"));
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("Separate Kills"), &separate_kills_enabled_)) SendSetting(kSettingSeparateKills, separate_kills_enabled_ ? 1 : 0);
          SameLine(); HelpMarker(i18n::Tr("Affiche un séparateur dans le chat log entre chaque kill de mobs. (Demandez à Spider - @separate)"));
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("Block EXP Gain"), &block_exp_)) SendSetting(kSettingBlockExp, block_exp_ ? 1 : 0);
          SameLine(); HelpMarker(i18n::Tr("Bloque le gain d'EXP. (@blockexp)"));
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("Show Skill Delay"), &show_attack_delay_enabled_)) SendSetting(kSettingShowAttackDelay, show_attack_delay_enabled_ ? 1 : 0);
          SameLine(); HelpMarker(i18n::Tr("Affiche un message dans le chat quand un skill\néchoue à cause du cooldown. (@showdelay)"));
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("Show Speed"), &show_move_speed_enabled_)) SendSetting(kSettingShowMoveSpeed, show_move_speed_enabled_ ? 1 : 0);
          SameLine(); HelpMarker(i18n::Tr("Affiche la valeur de vitesse de déplacement et d'attaque\ndans le chat lors d'un changement comme après\nun buff style AgiUP ou Card. (@showspeed)"));
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("Sell Stuff"), &sell_stuff_enabled_)) SendSetting(kSettingSellStuff, sell_stuff_enabled_ ? 1 : 0);
          SameLine(); HelpMarker(i18n::Tr("Permet la vente d'items améliorés (refine > 0),\ncartes, munitions et items slotés chez les PNJ marchands.\nDésactiver pour protéger ces items. (@sellstuff)"));
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("Sell Item"), &sell_item_enabled_)) SendSetting(kSettingSellItem, sell_item_enabled_ ? 1 : 0);
          SameLine(); HelpMarker(
            i18n::Tr("Permet la vente des items du groupe IG_SELLITEM chez les PNJ marchands.\nDésactiver pour les protéger. (@sellitem)\n\n"
            "Groupe SELLITEM :\nGreen Potion (506)\nWhite Slim Potion (547)\nLucky Candy (570)\n"
            "Old Blue Box (603)\nYggdrasil Berry (607)\nYggdrasil Seed (608)\nOld Card Album (616)\n"
            "Old Violet Box (617)\nGift Box (644)\nPoison Bottle (678)\nGold (969)\n"
            "Temporal Crystal (6607)\nCoagulated Spell (6608)\nJitterbug's Tooth (6719)\n"
            "Fire Bottle (7135)\nAcid Bottle (7136)\nCoating Bottle (7139)\n"
            "Fragment of Agony (7436)\nFragment of Misery (7437)\nFragment of Hatred (7438)\nPiece of Memory Red (7439)\n"
            "Ice Scale (7562)\nCursed Water (12020)\nElemental Converter Fire (12114)\nElemental Converter Water (12115)\n"
            "Elemental Converter Earth (12116)\nElemental Converter Wind (12117)\nMystical Card Album (12246)"));
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("No Ask"), &no_ask_enabled_)) SendSetting(kSettingNoAsk, no_ask_enabled_ ? 1 : 0);
          SameLine(); HelpMarker(i18n::Tr("Refuse automatiquement les invitations\nde trade, de guilde et d'alliance. (@noask)"));
          ImGui::TableNextColumn(); if (ro::RoCheckbox(i18n::Tr("Wings"), &wings_enabled_)) SendSetting(kSettingWings, wings_enabled_ ? 1 : 0);
          SameLine(); HelpMarker(i18n::Tr("Active ou désactive le sprite alternatif des Angel wings et Devil wings (Moonlight 2005 vibe - @wings)"));
          ImGui::EndTable();
        }
        // @noks — combo 4 options (off / self / party / guild)
        {
          static const char* kNoksLabels[] = { "Off", "Self", "Party", "Guild" };
          ImGui::SetNextItemWidth(100.0f);
          if (ro::RoBeginCombo("@noks", kNoksLabels[noks_mode_ < 4 ? noks_mode_ : 0])) {
            for (int i = 0; i < 4; ++i) {
              const bool selected = (noks_mode_ == i);
              if (ImGui::Selectable(kNoksLabels[i], selected)) {
                noks_mode_ = i;
                SendSetting(kSettingNoksMode, static_cast<uint16_t>(i));
              }
              if (selected) ImGui::SetItemDefaultFocus();
            }
            // 🔴 `ro::RoEndCombo`, PAS `ImGui::EndCombo` : RoBeginCombo n'appelle
            // pas BeginCombo — il dessine le champ à la main et ouvre un
            // ImGui::BeginPopup. Le refermer avec EndCombo laissait cinq
            // PushStyleColor et un PushID sur la pile à CHAQUE frame où le combo
            // était déroulé. (Le BeginCombo natif de la ligne ~411, lui, se ferme
            // bien par ImGui::EndCombo : ne pas « corriger » celui-là.)
            ro::RoEndCombo();
          }
          SameLine(); HelpMarker(i18n::Tr("Kill Steal Protection — empêche d'autres joueurs de voler vos kills MVP.\nSelf = toi seulement, Party = ta party, Guild = ta guilde. (@noks)"));
        }
        ImGui::EndTabItem();
      }
      // (« SPR Lab » a été déplacé dans le CollapsingHeader « Staff Tools »,
      // gaté sur le group level serveur — cf. IsStaff.)
      if (ImGui::BeginTabItem(i18n::Tr("Autoloots")))
      {
        Spacing();
        {// @autoloot
          int rate = aloot_rate_;
          ImGui::SetNextItemWidth(130.0f);
          if (WheelSliderInt("@autoloot", &rate, 0, 100, "%d%%")) {
            aloot_rate_ = rate;
            SendSetting(kSettingAlootRate, static_cast<uint16_t>(rate));
          }
          SameLine();
          if (ro::RoSmallButton(i18n::Tr("Reset##rate"))) {
            aloot_rate_ = 0;
            SendSetting(kSettingAlootRate, 0);
          }
        }
        Separator();
        { // @autolootpognon
          int min_zeny = aloot_min_zeny_;
          ImGui::SetNextItemWidth(130.0f);
          if (ImGui::InputInt("@autolootpognon (z)", &min_zeny, 100, 10000)) {
            if (min_zeny < 0) min_zeny = 0;
            if (min_zeny > 1000000) min_zeny = 1000000;
            min_zeny = (min_zeny / 100) * 100;
            aloot_min_zeny_ = min_zeny;
            SendSetting(kSettingAlootMinZenyDiv100, static_cast<uint16_t>(min_zeny / 100));
          }
        SameLine(); HelpMarker(i18n::Tr("Autoloot des items ayant au minimum le prix de revente configuré."));
          auto apply_min_zeny_delta = [this](int delta) {
            int min_zeny = aloot_min_zeny_ + delta;
            if (min_zeny < 0) min_zeny = 0;
            if (min_zeny > 1000000) min_zeny = 1000000;
            min_zeny = (min_zeny / 100) * 100;
            aloot_min_zeny_ = min_zeny;
            SendSetting(kSettingAlootMinZenyDiv100, static_cast<uint16_t>(min_zeny / 100));
          };
          if (ro::RoSmallButton(i18n::Tr("-10kz")))  apply_min_zeny_delta(-10000);
          SameLine();
          if (ro::RoSmallButton(i18n::Tr("-1kz")))   apply_min_zeny_delta(-1000);
          SameLine();
          if (ro::RoSmallButton(i18n::Tr("+1kz")))   apply_min_zeny_delta(1000);
          SameLine();
          if (ro::RoSmallButton(i18n::Tr("+10kz")))  apply_min_zeny_delta(10000);
          SameLine();
          if (ro::RoSmallButton(i18n::Tr("Reset##pognon"))) {
            aloot_min_zeny_ = 0;
            SendSetting(kSettingAlootMinZenyDiv100, 0);
          }
        }
        Separator();
        if (ImGui::TreeNode("@autoloottype")) {// @autoloottype
          TextUnformatted("@autoloottype :");
          SameLine(); HelpMarker(i18n::Tr("Cochez les types d'items à lootter automatiquement.\nHealing=0 Usable=2 Etc=3 Armor=4 Weapon=5\nCard=6 PetEgg=7 PetArmor=8 Ammo=10 Cash=11"));
          SameLine();
          if (ro::RoSmallButton(i18n::Tr("Reset##type"))) {
            aloot_type_mask_ = 0;
            SendSetting(kSettingAlootType, 0);
          }
          static const struct { const char* label; int bit; } kAlootTypes[] = {
            {"Healing",   1 << 0},  {"Usable",    1 << 2},
            {"Etc",       1 << 3},  {"Armor",     1 << 4},
            {"Weapon",    1 << 5},  {"Card",      1 << 6},
            {"Pet Egg",   1 << 7},  {"Pet Armor", 1 << 8},
            {"Ammo",      1 << 10}, {"Cash",     1 << 11},
          };
          if (ImGui::BeginTable("aloottype", 2)) {
            for (const auto& t : kAlootTypes) {
              ImGui::TableNextColumn();
              bool checked = (aloot_type_mask_ & t.bit) != 0;
              if (ro::RoCheckbox(t.label, &checked)) {
                if (checked) aloot_type_mask_ |=  t.bit;
                else         aloot_type_mask_ &= ~t.bit;
                SendSetting(kSettingAlootType, static_cast<uint16_t>(aloot_type_mask_));
              }
            }
            ImGui::EndTable();
          }
          ImGui::TreePop();
        }
        Separator();
        {// @autolootrare
        if (ro::RoCheckbox(i18n::Tr("Autoloot rares"), &aloot_rare_)) SendSetting(kSettingAlootRare, aloot_rare_ ? 1 : 0);
        SameLine(); HelpMarker(
          i18n::Tr("Autolooting: Toutes les Cards\nOld Blue Box (603)\nYggdrasil Berry (607)\nYggdrasil Seed (608)\nOld Card Album (616)\nOld Purple Box (617)\nGift Box (644)\nGold (969)\n"
          "Temporal Crystal (6607)\nCoagulated Spell (6608)\nJitterbug's Tooth (6719)\nFragment of Agony (7436)\nFragment of Misery (7437)\nFragment of Hatred (7438)\n"
          "Piece_Of_Memory_Red (7439)\nTreasure Box (7444)\nCursed Water (12020)\nElemental Converter Fire (12114)\nElemental Converter Water (12115)\n"
          "Elemental Converter Earth (12116)\nElemental Converter Wind (12117)\nMystical Card Album (12246)\nSentimental Fragment (22687)\nCursed Fragment (23016)"));
        }
        Separator();
        {// @autolootmvp / @autolootmvpreward
        if (ro::RoCheckbox(i18n::Tr("Autoloot MVP"), &aloot_mvp_)) SendSetting(kSettingAlootMvp, aloot_mvp_ ? 1 : 0);
        SameLine(); HelpMarker(i18n::Tr("Loot automatiquement les MVP\nquelque soit leur taux de drop. (@autolootmvp)"));
        if (ro::RoCheckbox(i18n::Tr("Autoloot MVP rewards (actif par défaut)"), &aloot_mvp_rwd_)) SendSetting(kSettingAlootMvpRwd, aloot_mvp_rwd_ ? 1 : 0);
        SameLine(); HelpMarker(i18n::Tr("Les drops de récompense des MVP sont lootés\nautomatiquement par défaut.\nDécocher pour désactiver. (@autolootmvpreward)"));
        }
        Separator();
        if (ImGui::TreeNode("@autolootid")) {// @autolootid
          TextUnformatted("@autolootid :");
          SameLine(); HelpMarker(i18n::Tr("Loot automatiquement les items par ID.\nMax 50 IDs. (@autolootid <id>)"));
          SameLine();
          if (!ModernInterfaceEnabled()) {
            if (ro::RoCheckbox(i18n::Tr("Overlay"), &show_alootid_overlay_))
              SaveSettings();
            SameLine();
            HelpMarker(i18n::Tr("Affiche un bouton Add/Remove Alootid\nprès du curseur au clic droit sur un item dans l'affichage natif."));
          }
          SameLine();
          if (ro::RoSmallButton(i18n::Tr("Clear##alootid"))) {
            aloot_ids_.clear();
            SendSetting(kSettingAlootId, 0);
          }
          ImGui::SetNextItemWidth(100.0f);
          ImGui::InputInt("##alootid_input", &aloot_id_input_, 0, 0);
          if (aloot_id_input_ < 0) aloot_id_input_ = 0;
          SameLine();
          const bool can_add = (aloot_id_input_ > 0 && aloot_ids_.size() < 50);
          if (!can_add) ImGui::BeginDisabled();
          if (ro::RoButton(i18n::Tr("Add##alootid"))) {
            const uint32_t id = static_cast<uint32_t>(aloot_id_input_);
            bool found = false;
            for (uint32_t x : aloot_ids_) if (x == id) { found = true; break; }
            if (!found) {
              aloot_ids_.push_back(id);
              SendSetting(kSettingAlootId, id);
            }
          }
          if (!can_add) ImGui::EndDisabled();
          ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
          {
            // Detect unsaved changes: compare current list vs snapshot (order-independent).
            auto sorted_copy = [](const std::vector<uint32_t>& v) {
              auto s = v; std::sort(s.begin(), s.end()); return s;
            };
            const bool dirty = (alootid_active_preset_ != 0) &&
                               (sorted_copy(aloot_ids_) != sorted_copy(alootid_saved_ids_));

            // Recomposé et consommé DANS LE MÊME frame : il était `static`, ce qui
            // faisait croire à un état persistant d'une frame à l'autre.
            char preset_header_text[96];
            if (alootid_active_preset_ != 0) {
              const char* preset_name = nullptr;
              char slot_no_buf[8];
              for (const auto& p : alootid_presets_) {
                if (p.slot_no == alootid_active_preset_) {
                  preset_name = p.name.empty()
                    ? (std::snprintf(slot_no_buf, sizeof(slot_no_buf), "#%u", p.slot_no), slot_no_buf)
                    : p.name.c_str();
                  break;
                }
              }
              if (dirty)
                std::snprintf(preset_header_text, sizeof(preset_header_text),
                              i18n::Tr("%s (non sauvegardé)"), preset_name ? preset_name : "?");
              else
                std::snprintf(preset_header_text, sizeof(preset_header_text),
                              "%s", preset_name ? preset_name : "?");
            } else {
              std::strncpy(preset_header_text, i18n::Tr("Liste courante"),
                           sizeof(preset_header_text));
            }
            TextUnformatted(preset_header_text);
          }
          ImGui::BeginChild("##alootid_list", ImVec2(0, 160), true);
          if (ImGui::BeginTable("##alootid_tbl", 2, ImGuiTableFlags_SizingStretchSame)) {
            for (int i = 0; i < static_cast<int>(aloot_ids_.size()); ++i) {
              ImGui::TableNextColumn();
              const uint32_t id = aloot_ids_[i];
              char lbl[32];
              std::snprintf(lbl, sizeof(lbl), i18n::Tr("x##alootid_%d"), i);
              if (ro::RoSmallButton(lbl)) {
                SendSetting(kSettingAlootIdRemove, aloot_ids_[i]);
                aloot_ids_.erase(aloot_ids_.begin() + i);
                --i;
                continue;
              }
              SameLine();
              const auto it = item_names_.find(id);
              if (it != item_names_.end())
                ImGui::Text("%s [%u]", it->second.c_str(), id);
              else
                ImGui::Text("[%u]", id);
            }
            ImGui::EndTable();
          }
          ImGui::EndChild();
          ImGui::PopStyleVar();
          // Quick-add/remove from the last right-clicked item description window.
          if (g_last_viewed_item != 0) {
            Separator();
            const auto itv = item_names_.find(g_last_viewed_item);
            if (itv != item_names_.end())
              ImGui::Text(i18n::Tr("Vu: [%u] %s"), g_last_viewed_item, itv->second.c_str());
            else
              ImGui::Text(i18n::Tr("Vu: [%u]"), g_last_viewed_item);
            SameLine();
            int vu_idx = -1;
            for (int k = 0; k < static_cast<int>(aloot_ids_.size()); ++k)
              if (aloot_ids_[k] == g_last_viewed_item) { vu_idx = k; break; }
            if (vu_idx >= 0) {
              if (ro::RoSmallButton(i18n::Tr("Remove##alootid_vu"))) {
                SendSetting(kSettingAlootIdRemove, aloot_ids_[vu_idx]);
                aloot_ids_.erase(aloot_ids_.begin() + vu_idx);
              }
            } else {
              const bool can_add_vu = (aloot_ids_.size() < 50);
              if (!can_add_vu) ImGui::BeginDisabled();
              if (ro::RoSmallButton(i18n::Tr("Add##alootid_vu"))) {
                aloot_ids_.push_back(g_last_viewed_item);
                SendSetting(kSettingAlootId, g_last_viewed_item);
              }
              if (!can_add_vu) ImGui::EndDisabled();
            }
          }
          // ── Presets (server-backed, DB table `alootid`) ──
          Separator();
          TextUnformatted(i18n::Tr("Presets :"));
          // Autoload indicator + toggle, on the same line as the label.
          {
            const AlootPreset* autoload_preset = nullptr;
            for (const auto& p : alootid_presets_)
              if (p.autoload) { autoload_preset = &p; break; }
            SameLine();
            // Find the selected preset to know what toggling autoload does.
            const AlootPreset* sel_for_al = nullptr;
            for (const auto& p : alootid_presets_)
              if (p.slot_no == alootid_selected_preset_) { sel_for_al = &p; break; }
            bool al = sel_for_al && sel_for_al->autoload;
            if (!sel_for_al) ImGui::BeginDisabled();
            if (ro::RoCheckbox(i18n::Tr("Autoload##preset"), &al))
              SendPresetCmd(kAlootPresetAutoload, al ? alootid_selected_preset_ : 0);
            if (!sel_for_al) ImGui::EndDisabled();
            SameLine();
            if (autoload_preset) {
              if (autoload_preset->name.empty())
                ImGui::TextDisabled("(#%u)", autoload_preset->slot_no);
              else
                ImGui::TextDisabled("(%s)", autoload_preset->name.c_str());
            } else {
              ImGui::TextDisabled(i18n::Tr("(aucun)"));
            }
          }
          ImGui::SetNextItemWidth(120.0f);
          ImGui::InputText("##preset_name", alootid_preset_input_,
                           sizeof(alootid_preset_input_));
          SameLine();
          ro::RoCheckbox(i18n::Tr("Renommer##preset_toggle"), &alootid_rename_open_);
          if (alootid_rename_open_) {
            SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputText("##preset_rename", alootid_rename_input_,
                             sizeof(alootid_rename_input_));
          }
          SameLine();
          {
            const AlootPreset* sel_for_rename = nullptr;
            for (const auto& p : alootid_presets_)
              if (p.slot_no == alootid_selected_preset_) { sel_for_rename = &p; break; }

            // Find existing preset matching the typed name (for delete-on-empty).
            const AlootPreset* named_preset = nullptr;
            for (const auto& p : alootid_presets_)
              if (p.name == alootid_preset_input_) { named_preset = &p; break; }

            const bool is_rename = alootid_rename_open_;
            const bool list_empty = aloot_ids_.empty();
            // Rename: need a new name + a selected preset.
            // Save: need a non-empty name; list empty → delete the named preset.
            const bool can_act = is_rename
              ? (alootid_rename_input_[0] != '\0' && sel_for_rename != nullptr)
              : (alootid_preset_input_[0] != '\0' &&
                 (!list_empty || named_preset != nullptr));

            if (!can_act) ImGui::BeginDisabled();
            const char* btn_label = (!is_rename && list_empty && named_preset)
              ? "Supprimer##preset_save" : "Sauvegarder##preset";
            if (ro::RoSmallButton(btn_label)) {
              if (is_rename && sel_for_rename) {
                SendPresetCmd(kAlootPresetRename, alootid_selected_preset_,
                              alootid_rename_input_);
                alootid_rename_open_ = false;
              } else if (!is_rename && list_empty && named_preset) {
                SendPresetCmd(kAlootPresetDelete, named_preset->slot_no);
              } else {
                uint8_t save_no = 0;
                bool used[11] = {};
                for (const auto& p : alootid_presets_) {
                  if (p.name == alootid_preset_input_) { save_no = p.slot_no; break; }
                  if (p.slot_no <= 10) used[p.slot_no] = true;
                }
                if (save_no == 0)
                  for (uint8_t n = 1; n <= 10; ++n) if (!used[n]) { save_no = n; break; }
                if (save_no > 0)
                  SendPresetCmd(kAlootPresetSave, save_no, alootid_preset_input_);
              }
            }
            if (!can_act) ImGui::EndDisabled();
          }

          // Combo: select preset by name
          {
            const AlootPreset* sel_preset = nullptr;
            for (const auto& p : alootid_presets_)
              if (p.slot_no == alootid_selected_preset_) { sel_preset = &p; break; }

            auto preset_label = [](const AlootPreset& p, char* buf, size_t sz, bool mark_active) {
              if (p.name.empty())
                std::snprintf(buf, sz, "#%u%s", p.slot_no, mark_active ? " *" : "");
              else
                std::snprintf(buf, sz, "%s%s", p.name.c_str(), mark_active ? " *" : "");
            };
            char preview_buf[66];
            const char* preview;
            if (sel_preset) {
              preset_label(*sel_preset, preview_buf, sizeof(preview_buf), false);
              preview = preview_buf;
            } else {
              preview = "-- choisir --";
            }
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::BeginCombo("##preset_select", preview)) {
              for (const auto& p : alootid_presets_) {
                const bool sel = (p.slot_no == alootid_selected_preset_);
                char label[66];
                preset_label(p, label, sizeof(label), p.slot_no == alootid_active_preset_);
                if (ImGui::Selectable(label, sel))
                  alootid_selected_preset_ = p.slot_no;
                if (sel) ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }
            SameLine();
            const bool has_sel = sel_preset != nullptr;
            if (!has_sel) ImGui::BeginDisabled();
            if (ro::RoSmallButton(i18n::Tr("Charger##preset")))
              SendPresetCmd(kAlootPresetLoad, alootid_selected_preset_);
            SameLine();
            if (ro::RoSmallButton(i18n::Tr("Supprimer##preset")))
              SendPresetCmd(kAlootPresetDelete, alootid_selected_preset_);
            if (!has_sel) ImGui::EndDisabled();

          }
          ImGui::TreePop();
        }
        ImGui::EndTabItem();
      }
      // EndTabBar DOIT rester dans le if (BeginTabBar) : ImGui l'exige (assert en
      // debug, état de tab bar corrompu en release). Il était appelé juste après
      // l'accolade, donc aussi quand BeginTabBar renvoyait false — onglet replié.
      ImGui::EndTabBar();
    }
    PopStyleCompact();
  }
}

// Combo « Tri … » d'un réglage SERVEUR (e_sort_mode 0-6). Partagé entre « Commands
// Settings » et les panneaux des fenêtres concernées (InventoryViewer, StorageWindow) :
// libellé, aide, état et envoi vivent ICI, donc les points d'entrée ne peuvent pas
// diverger. Skin RO (ro::RoCombo) comme le reste du panneau.
bool MoonlightUi::DrawSortModeCombo(SortTarget target) {
  static const char* const kSortLabels[] = {
      "Par ID", "Par type", "Par quantité", "Par poids", "Par prix", "Par nom", "Aucun" };
  int* value = nullptr;
  uint16_t setting_id = 0;
  const char* label = nullptr;
  const char* help = nullptr;
  switch (target) {
    case kSortInventory:
      value = &sort_mode_inventory_; setting_id = kSettingSortModeInventory;
      label = i18n::Tr("Tri Inventaire");
      help = i18n::Tr("Tri automatique de l'inventaire.");
      break;
    case kSortCart:
      value = &sort_mode_cart_; setting_id = kSettingSortModeCart;
      label = i18n::Tr("Tri Cart");
      help = i18n::Tr("Tri automatique du cart.");
      break;
    case kSortStorage:
      value = &sort_mode_storage_; setting_id = kSettingSortModeStorage;
      label = i18n::Tr("Tri Storages");
      help = i18n::Tr("Tri automatique des Storages personnel à la prochaine ouverture.");
      break;
    case kSortGuildStorage:
      value = &sort_mode_guild_storage_; setting_id = kSettingSortModeGuildStorage;
      label = i18n::Tr("Tri Storage Guilde");
      help = i18n::Tr("Tri automatique du Storage de guilde à la prochaine ouverture.");
      break;
    default:
      return false;
  }
  // RoCombo indexe kSortLabels sans garde : une valeur hors table (mode ajouté côté
  // moonlight, yaml bidouillé) lirait à côté.
  if (*value < 0 || *value >= IM_ARRAYSIZE(kSortLabels)) *value = 0;
  ImGui::SetNextItemWidth(130.0f);
  const bool changed = ro::RoCombo(label, value, kSortLabels, IM_ARRAYSIZE(kSortLabels));
  if (changed) SendSetting(setting_id, static_cast<uint16_t>(*value));
  SameLine(); HelpMarker(help);
  return changed;
}
