# -*- coding: utf-8 -*-
"""Réorganisation de la mémoire (2026-09-04).
  --status  : ajoute status/last_verified aux fiches hors agents.
  --rename  : retype project_* -> reference_* et réécrit tous les liens (APRÈS les agents).
  --check   : vérifie que toute fiche a type/status/last_verified et qu'aucun lien ne pend.
"""
import os, re, sys, datetime, shutil

MEM = r"C:\Users\Sting\.claude\projects\d--Mes-documents-GitHub-Bourgeon\memory"
JOURNAL = r"d:\Mes documents\GitHub\Bourgeon\docs\journal"
TODAY = "2026-09-04"

AGENTS = set("""project_character_sheet project_char_portrait_re project_client_2026_port project_guild_window_re project_opcode_system project_skillbar_multibar_wip
project_item_skill_desc_window_re project_vending_window_re project_i18n_language_setting project_doll_composer project_make_item_list_re project_quick_cast
project_shortcut_bar_re project_moonlight_web_login_design project_chat_item_icons project_solved_archive project_cash_emotion_re project_chatbox_imgui_conversion
project_spr_effect_lab project_spectator_session project_npc_shop_re project_game_settings_ui_re project_target_system_re project_party_friend_window_re
project_address_directory project_palette_editor project_hat_effect_preview project_storage_window_re project_navigation_re project_basic_info_menu_icons
project_charselect_imgui project_inventory_window project_inventory_viewer_wip project_grey_world project_skill_aoe_preview_showscale project_equip_window_re
feedback_re_method feedback_imgui_pitfalls feedback_ui_conventions feedback_native_replacement feedback_code_hygiene feedback_debug_tooling feedback_native_hooking feedback_build_and_git""".split())

# Fiches « project » qui sont en réalité des relevés stables -> type reference, renommées reference_*.
RETYPE = set("""project_achievement_title_re project_achievement_window_re project_bank_zeny_re project_barter_market_re project_berserk_chat_gate_re
project_camera_zoom_re project_card_insert_re project_cart_inventory_storage_flag project_cashshop_minimap_button project_cashshop_re project_cast_bar_re
project_changematerial_and_uiwindow_composite_re project_char_delete_email_sync project_chat_clear_history project_chat_room_window_re project_chat_tag_three_renderers
project_clan_window_re project_client_install_path project_custom_atcommands_re project_damage_numbers_re project_dev_mode_gating project_doram_palettes
project_dx7_dx9_rendering project_effect_script_system_re project_effect_zorder_render_bucket project_entity_chat_balloon_re project_entity_nameplate_re
project_entry_queue_re project_external_settings_re project_forum_changelog_posts project_game_emotes_chat project_guild_storage_log_re project_homunculus_re
project_hotkey_settings_window_re project_imgui_game_textures project_item_obtain_notify_re project_local_openers_hotkeys project_login_screen_re
project_memorial_dungeon_re project_merge_item_re project_minimap_re project_mob_3d_granny project_moonlight_is_prerenewal project_moonlight_server
project_moonlightsite_corrupt_sprites project_moonlightsite_class_sprite_resolution project_npc_dialog_bourgeon_tags project_npc_dialog_re
project_npc_progress_showscript_re project_own_session_globals project_own_look_globals project_party_search_re project_pet_system_re project_plugin_architecture
project_probability_window_re project_processinput_sendmsg_hook project_quest_tracking_re project_rodex_re project_shortcut_list_macro_re
project_skill_cast_visual_re project_skill_description_window_re project_skill_driven_windows project_skill_hit_effect_dispatch_re project_skill_tree_re
project_source_layout project_sprite_rendering_re project_status_effects_re project_status_icon_bar project_status_window_re project_trade_window_re
project_transmog_viewid project_ui_richtext_link_system project_ui_window_manager project_view_equip_re project_weapon_refine_re project_weapon_trail_gm_gate
project_web_api_asyncwork_re project_whisper_window_re project_window_position_persistence project_unexplored_systems_map""".split())

# Statut des fiches project qui restent des chantiers.
STATUS = {
    "soldé": """project_afk_screen project_bug_report_system project_frame_counter_overlay project_npc_click_block project_skill_input_latency
        project_zone_recorder_gif project_entity_context_menu project_monster_info_window project_cart_window_imgui_todo project_doom_in_ro_todo
        project_entity_properties_inspector project_keyboard_move project_link_label_widget_todo project_mvp_tracker project_patch_level_enforcement
        project_wand_ranged_attack project_weapon_dual_sprites project_weapon_zorder project_code_duplication_audit project_msgstring_translation
        project_spr_act_own_parser project_status_tweaks_plugin project_char_diagnostics project_login_parade project_dps_meter project_discord_relay
        project_auto_login project_moonlight_admin project_chat_plugin project_ro_skinning""".split(),
    "actif": """project_utf8_emoji_support project_headgear_recolor project_minigames_imgui project_ro_imgui_toolkit project_login_video_and_map_backdrop
        project_item_cell_widget_todo project_settings_file_layout project_inline_smilies_todo project_uiwindow_onmsg_re project_fun_documentation_campaign
        project_ro_cursor""".split(),
    "mort": """project_4th_job_body_palettes project_first_person_view_re project_graphics_engine_improvements""".split(),
    "périmé": """project_20250716_re""".split(),
}
SLUG_STATUS = {s: st for st, lst in STATUS.items() for s in lst}

FM_RE = re.compile(r"\A---\r?\n(.*?)\r?\n---\r?\n", re.S)

def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()

def write(p, txt):
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(txt)
    os.replace(tmp, p)

def fiches():
    for f in sorted(os.listdir(MEM)):
        if f.endswith(".md") and f != "MEMORY.md":
            yield f[:-3], os.path.join(MEM, f)

def get_field(head, key):
    m = re.search(r"^[ \t]*%s:[ \t]*(.*)$" % re.escape(key), head, re.M)
    return m.group(1).strip() if m else None

def set_meta(txt, key, value, after="type"):
    """Pose ou remplace `key: value` sous metadata (indenté de 2), après la ligne `after`."""
    m = FM_RE.match(txt)
    head = m.group(1)
    if re.search(r"^[ \t]+%s:" % re.escape(key), head, re.M):
        head2 = re.sub(r"^([ \t]+)%s:.*$" % re.escape(key), lambda mm: "%s%s: %s" % (mm.group(1), key, value), head, count=1, flags=re.M)
    else:
        head2, n = re.subn(r"^([ \t]+)%s:(.*)$" % re.escape(after), lambda mm: "%s%s:%s\n%s%s: %s" % (mm.group(1), after, mm.group(2), mm.group(1), key, value), head, count=1, flags=re.M)
        if n == 0:
            raise RuntimeError("pas de ligne %s: dans le frontmatter" % after)
    return txt[:m.start(1)] + head2 + txt[m.end(1):]

def do_status():
    n = 0
    for slug, p in fiches():
        if slug in AGENTS:
            continue
        txt = read(p)
        m = FM_RE.match(txt)
        if not m:
            print("SANS FRONTMATTER:", slug); continue
        head = m.group(1)
        typ = get_field(head, "type")
        if slug in RETYPE:
            status = "stable"
        elif typ == "project":
            status = SLUG_STATUS.get(slug)
            if status is None:
                print("SANS DÉCISION:", slug); continue
        else:
            status = "stable"
        modified = get_field(head, "modified")
        if modified:
            lv = modified[:10]
        else:
            lv = datetime.date.fromtimestamp(os.path.getmtime(p)).isoformat()
        txt = set_meta(txt, "status", status, after="type")
        txt = set_meta(txt, "last_verified", lv, after="status")
        write(p, txt)
        n += 1
    print("statut posé sur", n, "fiches")

def do_rename():
    mapping = {s: "reference_" + s[len("project_"):] for s in RETYPE}
    # 1. réécrire le frontmatter et renommer les fichiers
    for old, new in mapping.items():
        p = os.path.join(MEM, old + ".md")
        if not os.path.exists(p):
            print("ABSENT:", old); continue
        txt = read(p)
        txt = re.sub(r"^name:[ \t]*%s[ \t]*$" % re.escape(old), "name: " + new, txt, count=1, flags=re.M)
        txt = re.sub(r"^([ \t]+)type:[ \t]*project[ \t]*$", r"\1type: reference", txt, count=1, flags=re.M)
        write(p, txt)
        os.replace(p, os.path.join(MEM, new + ".md"))
        jp = os.path.join(JOURNAL, old + ".md")
        if os.path.exists(jp):
            os.replace(jp, os.path.join(JOURNAL, new + ".md"))
    # 2. réécrire tous les liens, dans la mémoire ET dans docs/journal
    pat = re.compile(r"(?<![A-Za-z0-9_])(%s)(?![A-Za-z0-9_])" % "|".join(sorted(map(re.escape, mapping), key=len, reverse=True)))
    files = [os.path.join(MEM, f) for f in os.listdir(MEM) if f.endswith(".md")]
    files += [os.path.join(JOURNAL, f) for f in os.listdir(JOURNAL) if f.endswith(".md")]
    total = 0
    for p in files:
        txt = read(p)
        new, k = pat.subn(lambda mm: mapping[mm.group(1)], txt)
        if k:
            write(p, new); total += k
    print("renommées:", len(mapping), "; liens réécrits:", total)

def do_fixnames():
    """`name:` doit être le nom du fichier (certaines fiches ont un slug à tirets ou tronqué)."""
    n = 0
    for slug, p in fiches():
        txt = read(p)
        m = re.search(r"^name:[ \t]*(.*?)[ \t]*$", txt, re.M)
        if m and m.group(1) != slug:
            txt = txt[:m.start()] + "name: " + slug + txt[m.end():]
            write(p, txt); n += 1
    print("name: corrigés :", n)

def do_check():
    names = {slug for slug, _ in fiches()}
    bad = 0
    for slug, p in fiches():
        txt = read(p)
        m = FM_RE.match(txt)
        if not m:
            print("SANS FRONTMATTER:", slug); bad += 1; continue
        head = m.group(1)
        for k in ("type", "status", "last_verified"):
            if get_field(head, k) is None:
                print("CHAMP MANQUANT", k, ":", slug); bad += 1
        if get_field(head, "name") != slug:
            print("NAME ≠ FICHIER:", slug, "->", get_field(head, "name")); bad += 1
        for link in set(re.findall(r"\[\[([A-Za-z0-9_]+)\]\]", txt)) | set(re.findall(r"\(([A-Za-z0-9_]+)\.md\)", txt)):
            if link not in names:
                print("LIEN PENDANT dans", slug, "->", link); bad += 1
        sz = len(txt.encode("utf-8"))
        if sz > 6500 and not slug.startswith(("reference_index",)):
            print("GROSSE (%d o):" % sz, slug)
    print("anomalies:", bad, "; fiches:", len(names))

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    {"--status": do_status, "--rename": do_rename, "--check": do_check, "--fixnames": do_fixnames}[sys.argv[1]]()
