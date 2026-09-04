# -*- coding: utf-8 -*-
"""Génère MEMORY.md (index) à partir des frontmatters. Vérifie que chaque fiche est dans exactement un thème."""
import os, re, sys
MEM = r"C:\Users\Sting\.claude\projects\d--Mes-documents-GitHub-Bourgeon\memory"
FM_RE = re.compile(r"\A---\r?\n(.*?)\r?\n---\r?\n", re.S)

def fm(path):
    txt = open(path, encoding="utf-8").read()
    head = FM_RE.match(txt).group(1)
    g = lambda k: (re.search(r"^[ \t]*%s:[ \t]*(.*)$" % k, head, re.M) or [None, ""])[1].strip().strip('"')
    return {"type": g("type"), "status": g("status"), "desc": g("description")}

fiches = {f[:-3]: fm(os.path.join(MEM, f)) for f in os.listdir(MEM) if f.endswith(".md") and f != "MEMORY.md"}

RULES = """# Mémoire Bourgeon — index

Une fiche = un fait. `status` dans le frontmatter (actif · soldé · mort · périmé · stable). L'histoire des chantiers est dans `docs/journal/` du dépôt, les relevés dans `docs/*_re.md`.

## Règles toujours vraies (lire avant d'agir)

- 🔴🔴 [[feedback_dont_relaunch_game]] l'UTILISATEUR build et relance ; ne jamais annoncer « déployé ». [[feedback_build_and_git]] `--config Release` seul, compter les `error C`, commits FR (anglais pour WARP0716).
- 🔴🔴 [[feedback_workflow_ida_contention]] pas de Workflow dans VS Code (l'extension host meurt) ; deux IDA en parallèle = OK.
- 🔴🔴 [[feedback_absence_needs_measurement]] une recherche vide ne prouve pas l'absence : témoin positif d'abord. [[feedback_re_method]] index des leçons de RE : témoin négatif, cinq candidats, Hex-Rays ment.
- 🔴 [[feedback_native_hooking]] poser un hook : `mov ecx` ⇒ `__thiscall`, nos appels sautent notre hook, lire le `retn`. [[feedback_native_replacement]] l'ImGui est le chemin, le natif un repli : combler, pas reproduire.
- 🔴🔴 [[feedback_findwindow_id_is_not_close_id]] `FindWindow(id)` ≠ `CloseWindow(id)`. [[reference_native_window_dispatch]] trois étages. [[reference_native_window_toggle_router]] détruire, pas masquer. [[reference_client_window_layout_restore]] le client rouvre ses fenêtres à chaque map.
- 🔴🔴 [[reference_ida_is_vanilla_warp_patches]] l'IDB est vanilla : avant « le binaire ne fait pas X », regarder l'exe livré. [[feedback_warpable_targets_community]] « WARPable ? » = pour la communauté, répondre par le motif d'octets.
- 🔴🔴 [[feedback_player_setting_persistence]] réglage de joueur = serveur. [[feedback_rathena_conf_import_overrides]] la valeur effective est dans `conf/import/`. [[feedback_msgstring_no_msg_fallback]] id msgstring = index EXE.
- 🔴 [[feedback_ui_conventions]] accents FR, termes du jeu en anglais, tout est RO-skinné, changer un défaut livré = renommer la clé. [[feedback_imgui_pitfalls]] index des pièges ImGui. [[feedback_imgui_inputtext_multiline]] · [[feedback_imgui_treenode_refuses_modified_click]].
- 🔴 [[feedback_state_lifetime]] battement périmé ≠ sorti du monde ; purger au char-select. [[feedback_debug_tooling]] LogDiag d'abord, muet en nominal. [[feedback_code_hygiene]] `__try` multi-étapes cache l'échec, zéro constante magique, heredoc bloqué ⇒ Write.
- 🔴🔴 [[feedback_python_write_truncates]] `open(p,'w')` vide le fichier. [[feedback_escapeshellarg_strips_non_ascii]] escapeshellarg avale le non-ASCII. [[feedback_restart_flag_opens_web_page]] le drapeau « relance » ouvre le navigateur.
- Utilisateur : [[user_runs_local_llm]] LLM local en permanence (sur un gel, demander d'abord) · [[user_test_character_gm_999]] chances à 100 % = pas un bug · [[user_desktop_path]] · [[user_spanish_learning]] · [[reference_client_install_path]] client sur E:.
"""

THEMES = [
 ("Fenêtres natives : relevés et remplacements", """
  reference_ui_window_manager reference_native_window_dispatch reference_native_window_toggle_router reference_client_window_layout_restore reference_window_position_persistence reference_rtti_window_class_id project_uiwindow_onmsg_re reference_changematerial_and_uiwindow_composite_re
  project_inventory_window project_inventory_viewer_wip project_cart_window_imgui_todo project_storage_window_re project_equip_window_re reference_status_window_re project_status_tweaks_plugin
  project_basic_info_menu_icons project_character_sheet project_char_portrait_re project_item_skill_desc_window_re reference_skill_description_window_re reference_skill_tree_re
  project_shortcut_bar_re project_skillbar_multibar_wip reference_shortcut_list_macro_re reference_hotkey_settings_window_re project_game_settings_ui_re
  project_guild_window_re project_party_friend_window_re reference_trade_window_re project_vending_window_re project_npc_shop_re project_make_item_list_re reference_card_insert_re
  reference_merge_item_re reference_weapon_refine_re reference_rodex_re reference_bank_zeny_re reference_guild_storage_log_re reference_view_equip_re
  reference_achievement_window_re reference_achievement_title_re reference_quest_tracking_re reference_probability_window_re reference_memorial_dungeon_re
  reference_barter_market_re reference_party_search_re reference_entry_queue_re reference_clan_window_re reference_homunculus_re reference_pet_system_re
  reference_cashshop_re reference_cashshop_minimap_button project_cash_emotion_re project_navigation_re reference_minimap_re project_monster_info_window
  reference_item_obtain_notify_re reference_npc_dialog_re reference_npc_progress_showscript_re project_npc_click_block project_entity_context_menu
  reference_skill_driven_windows reference_local_openers_hotkeys reference_unexplored_systems_map
 """),
 ("Login, char-select, sessions", """
  reference_login_screen_re project_auto_login project_moonlight_web_login_design project_login_video_and_map_backdrop project_login_parade project_spectator_session project_charselect_imgui
 """),
 ("Chat", """
  project_chat_plugin project_chatbox_imgui_conversion reference_chat_clear_history project_chat_item_icons reference_chat_tag_three_renderers reference_game_emotes_chat
  reference_whisper_window_re reference_chat_room_window_re project_discord_relay reference_berserk_chat_gate_re reference_entity_chat_balloon_re project_inline_smilies_todo
 """),
 ("Sprites, effets, rendu", """
  reference_sprite_rendering_re project_spr_act_own_parser project_doll_composer reference_grf_act_spr_reference_impl reference_effect_script_system_re reference_effect_zorder_render_bucket
  reference_skill_cast_visual_re reference_skill_hit_effect_dispatch_re project_hat_effect_preview project_spr_effect_lab reference_damage_numbers_re project_ro_cursor
  project_weapon_zorder project_weapon_dual_sprites reference_weapon_trail_gm_gate reference_mob_3d_granny reference_status_icon_bar reference_status_effects_re reference_entity_nameplate_re reference_cast_bar_re
  project_palette_editor project_headgear_recolor reference_doram_palettes project_4th_job_body_palettes reference_transmog_viewid reference_own_look_globals reference_own_session_globals
  reference_render_queue_2d reference_cres_family reference_resource_io_loaders reference_grf_loading_patcher reference_audio_miles reference_dx7_dx9_rendering project_graphics_engine_improvements
 """),
 ("Caméra, contrôles, gameplay client", """
  reference_camera_zoom_re project_afk_screen project_first_person_view_re project_grey_world project_keyboard_move project_quick_cast project_skill_input_latency
  project_target_system_re project_skill_aoe_preview_showscale reference_cmode_sendmsg_use_skill reference_processinput_sendmsg_hook project_char_diagnostics project_entity_properties_inspector
 """),
 ("Moteur du client : adresses, protocole, scripting", """
  project_address_directory project_20250716_re project_client_2026_port reference_client_install_path reference_ida_is_vanilla_warp_patches reference_warp0716 reference_lotus_virtualization_low_text project_fun_documentation_campaign
  project_opcode_system reference_native_packet_len_resolver reference_replay_reassembly_re reference_web_api_asyncwork_re reference_external_settings_re
  reference_lua_c_api reference_ai_script_api reference_actorai_class_layout reference_manager_singletons_map reference_ui_richtext_link_system
  reference_dll_watchpoint_veh_diag reference_x32dbgmcp_bridge
 """),
 ("Socle Bourgeon : ImGui, i18n, rangement", """
  project_ro_imgui_toolkit project_ro_skinning reference_imgui_game_textures project_link_label_widget_todo project_item_cell_widget_todo project_utf8_emoji_support
  project_i18n_language_setting project_msgstring_translation project_settings_file_layout reference_source_layout reference_plugin_architecture reference_dev_mode_gating
  project_frame_counter_overlay project_zone_recorder_gif project_minigames_imgui project_doom_in_ro_todo project_bug_report_system project_code_duplication_audit project_solved_archive project_memory_reorganization
 """),
 ("Serveur Moonlight, site, forum", """
  reference_moonlight_server reference_moonlight_is_prerenewal reference_moonlight_server_ssh reference_rathena_basejob_baseclass reference_rathena_group_permission_or reference_prere_combat_formulas
  reference_custom_atcommands_re project_dps_meter project_mvp_tracker project_patch_level_enforcement project_wand_ranged_attack reference_char_delete_email_sync reference_cart_inventory_storage_flag
  reference_npc_dialog_bourgeon_tags reference_moonlightsite_corrupt_sprites reference_moonlightsite_class_sprite_resolution reference_moonlightsite_php8_static reference_php_local_cli
  reference_forum_changelog_posts project_moonlight_admin reference_data_folder_cp949_encoding
 """),
]

def main():
    out = [RULES]
    # Bloc B : chantiers actifs
    actifs = sorted(s for s, f in fiches.items() if f["status"] == "actif")
    out.append("## Chantiers actifs\n")
    for s in actifs:
        d = fiches[s]["desc"]
        d = d if len(d) <= 85 else d[:85].rsplit(" ", 1)[0] + "…"
        out.append("- [[%s]] %s" % (s, d))
    out.append("- 🎯 Jouable, jamais outillé : [[reference_clan_window_re]] (237, Ctrl+G) · [[reference_guild_storage_log_re]] (253) · [[reference_barter_market_re]] (à allumer) · [[reference_skill_driven_windows]] (6 fenêtres).")
    out.append("- ⛔ Morts / périmés : " + " · ".join("[[%s]]" % s for s in sorted(fiches) if fiches[s]["status"] in ("mort", "périmé")))
    out.append("")
    # Bloc C : annuaire par thème
    out.append("## Où chercher (une fiche = un fait ; ouvrir la fiche, pas deviner)\n")
    seen = {}
    for title, body in THEMES:
        slugs = body.split()
        for s in slugs:
            seen.setdefault(s, []).append(title)
        marks = []
        for s in slugs:
            st = fiches.get(s, {}).get("status", "")
            tag = {"soldé": "✅", "actif": "🔧", "mort": "⛔", "périmé": "⚠"}.get(st, "")
            marks.append("[[%s]]%s" % (s, tag))
        out.append("- **%s** : %s" % (title, " · ".join(marks)))
    # Couverture : feedback/user/index sont dans le bloc A ; tout le reste doit être dans un thème.
    missing = [s for s in sorted(fiches) if s not in seen and not s.startswith(("feedback_", "user_")) and s != "reference_index"]
    dup = {s: t for s, t in seen.items() if len(t) > 1}
    unknown = [s for s in seen if s not in fiches]
    fb_not_in_rules = [s for s in sorted(fiches) if s.startswith("feedback_") and "[[%s]]" % s not in RULES]
    txt = "\n".join(out) + "\n"
    print("taille MEMORY.md :", len(txt.encode("utf-8")), "octets ;", txt.count("\n"), "lignes")
    print("manquants dans les thèmes :", missing)
    print("en double :", dup)
    print("inconnus (slug sans fiche) :", unknown)
    print("feedback hors du bloc règles :", len(fb_not_in_rules), fb_not_in_rules[:60])
    if "--write" in sys.argv and not missing and not dup and not unknown:
        tmp = os.path.join(MEM, "MEMORY.md.tmp")
        open(tmp, "w", encoding="utf-8", newline="").write(txt)
        os.replace(tmp, os.path.join(MEM, "MEMORY.md"))
        print("MEMORY.md écrit")

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
