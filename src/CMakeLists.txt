set(BOURGEON_FILES
	"main.cc"
	"bourgeon.cc"
	"bourgeon.h"
	"pch.h"

        # DDRAW Wrapper
        "ddraw/ddraw.def"
        "ddraw/ddraw.cc"
        "ddraw/ddraw.h"
        "ddraw/proxy_idirectdraw.cc"
        "ddraw/proxy_idirectdraw.h"

	# ── Fonctionnalités (compilées dans la DLL, aucun scripting à l'exécution) ──
	# Rangées par NATURE : ce que le module fait au client natif.
	#   windows/   remplacement ImGui complet d'une fenêtre native
	#   patches/   retouche du natif en place (hook, swap de vtable, octets)
	#   overlays/  surcouche ImGui dessinée par-dessus le jeu
	#   gameplay/  entrées clavier et caméra
	#   fx/        rendu des sprites, des effets et de l'image finale
	#   systems/   services transverses (réseau, sécurité, session, méta)
	#   minigames/ jeux embarqués
	"features/plugin.h"
	"features/staff_gate.h"
	"features/item_cell.cc"
	"features/item_cell.h"
	# Gestes d'un LIEN (objet, monstre, adresse) : clic gauche = description,
	# clic droit = menu, Maj+clic = lien dans le chat. Partagé par le chat, les
	# tables de drops et toute surface qui affiche une référence.
	"features/link_gesture.cc"
	"features/link_gesture.h"
	# Données serveur embarquées (SystemEN\bourgeon_recipes.yaml) : niveaux d'arme,
	# taux de refine, recettes étendues. Partagé — le refine et la fabrication en
	# dépendent, l'Atlas suivra.
	"features/craft_data.cc"
	"features/craft_data.h"
	"features/hotkey_util.cc"
	"features/hotkey_util.h"
	"features/hotkey_actions.cc"
	"features/hotkey_actions.h"
	"features/hotkey_dispatch.cc"
	"features/hotkey_dispatch.h"

	# Menu Moonlight : le sommaire des réglages + la persistance du yaml.
	# Panneaux extraits d'OnRenderUI (en-tête privé au dossier : internal.h).
	"features/moonlight_ui/moonlight_ui.cc"
	"features/moonlight_ui/moonlight_ui.h"
	"features/moonlight_ui/internal.h"
	"features/moonlight_ui/panel_rules.cc"
	"features/moonlight_ui/panel_commands.cc"
	"features/moonlight_ui/panel_fun.cc"
	"features/moonlight_ui/item_desc_probe.cc"
	"features/moonlight_ui/panel_interface.cc"
	"features/moonlight_ui/settings_table.cc"
	"features/moonlight_ui/settings_table.h"
	"features/moonlight_ui/settings_containers.cc"
	"features/moonlight_ui/settings_containers.h"

	# windows/
	"features/windows/bank_window.cc"
	"features/windows/bank_window.h"
	"features/windows/cart_viewer.cc"
	"features/windows/cart_viewer.h"
	"features/windows/cashshop_window.cc"
	"features/windows/cashshop_window.h"
	"features/windows/char_select.cc"
	"features/windows/char_select.h"
	"features/windows/char_select_layout.cc"
	"features/windows/char_select_layout.h"
	"features/windows/chat_window.cc"
	"features/windows/chat_window.h"
	"features/windows/character_sheet.cc"
	"features/windows/character_sheet.h"
	"features/windows/entity_context_menu.cc"
	"features/windows/entity_context_menu.h"
	"features/windows/entity_inspector.cc"
	"features/windows/entity_inspector.h"
	"features/windows/game_menu.cc"
	"features/windows/game_menu.h"
	"features/windows/game_settings.cc"
	"features/windows/game_settings.h"
	"features/windows/staff_tools.cc"
	"features/windows/staff_tools.h"
	"features/windows/hotkey_settings.cc"
	"features/windows/hotkey_settings.h"
	"features/windows/inventory_viewer.cc"
	"features/windows/inventory_viewer.h"
	"features/windows/item_desc_window.cc"
	"features/windows/item_desc_window.h"
	"features/windows/monster_info_window.cc"
	"features/windows/monster_info_window.h"
	"features/windows/npc_dialog_window.cc"
	"features/windows/npc_dialog_window.h"
	"features/windows/npc_shop_window.cc"
	"features/windows/npc_shop_window.h"
	"features/windows/palette_editor.cc"
	"features/windows/palette_editor.h"
	"features/windows/pet_window.cc"
	"features/windows/pet_window.h"
	"features/windows/rodex_window.cc"
	"features/windows/rodex_window.h"
	"features/windows/storage_window.cc"
	"features/windows/storage_window.h"
	"features/windows/trade_window.cc"
	"features/windows/trade_window.h"
	"features/windows/vending_window.cc"
	"features/windows/vending_window.h"
	"features/windows/weapon_refine_window.cc"
	"features/windows/weapon_refine_window.h"
	"features/windows/make_item_window.cc"
	"features/windows/make_item_window.h"
	# Atlas des recettes : le PARCOURS de ce qui se fabrique (par métier, par
	# produit, par matériau, flèches). Ni le client ni le serveur n'offrent cette
	# vue — la table native est une lookup par clé, et le serveur ne liste
	# qu'après un lancement de compétence.
	"features/windows/craft_atlas.cc"
	"features/windows/craft_atlas.h"

	# patches/
	"features/patches/berserk_chat_unlock.cc"
	"features/patches/berserk_chat_unlock.h"
	"features/patches/chat.cc"
	"features/patches/chat.h"
	"features/patches/chat_bg.cc"
	"features/patches/equip_tweaks.cc"
	"features/patches/equip_tweaks.h"
	"features/patches/inventory_tweaks.cc"
	"features/patches/inventory_tweaks.h"
	"features/patches/skill_tree_tweaks.cc"
	"features/patches/skill_tree_tweaks.h"
	"features/patches/status_tweaks.cc"
	"features/patches/status_tweaks.h"
	"features/patches/window_pos_tweaks.cc"
	"features/patches/window_pos_tweaks.h"

	# overlays/
	"features/overlays/basic_info.cc"
	"features/overlays/basic_info.h"
	"features/overlays/dps_meter.cc"
	"features/overlays/dps_meter.h"
	"features/overlays/cast_bar.cc"
	"features/overlays/cast_bar.h"
	"features/overlays/chat_balloon.cc"
	"features/overlays/chat_balloon.h"
	"features/overlays/entity_names.cc"
	"features/overlays/entity_names.h"
	"features/overlays/item_obtain_toast.cc"
	"features/overlays/item_obtain_toast.h"
	"features/overlays/login_parade.cc"
	"features/overlays/login_parade.h"
	"features/overlays/menu_icons.cc"
	"features/overlays/menu_icons.h"
	"features/overlays/quest_tracker.cc"
	"features/overlays/quest_tracker.h"
	"features/overlays/skill_bar.cc"
	"features/overlays/skill_bar.h"
	"features/overlays/status_icon_bar.cc"
	"features/overlays/status_icon_bar.h"

	# gameplay/
	"features/gameplay/fps_view.cc"
	"features/gameplay/fps_view.h"
	"features/gameplay/keyboard_move.cc"
	"features/gameplay/keyboard_move.h"
	"features/gameplay/quick_cast.cc"
	"features/gameplay/quick_cast.h"
	"features/gameplay/player_jump.cc"
	"features/gameplay/player_jump.h"

	# fx/
	"features/fx/ez_effect_capture.cc"
	"features/fx/ez_effect_capture.h"
	"features/fx/ground_paint.cc"
	"features/fx/ground_paint.h"
	"features/fx/hat_effect_depth.cc"
	"features/fx/hat_effect_depth.h"
	"features/fx/palette_base.cc"
	"features/fx/palette_base.h"
	"features/fx/palette_cache.cc"
	"features/fx/palette_cache.h"
	"features/fx/palette_inject.cc"
	"features/fx/palette_inject.h"
	"features/fx/style_sync.cc"
	"features/fx/style_sync.h"
	"features/fx/screen_fx.cc"
	"features/fx/screen_fx.h"
	"features/fx/spr_effect_lab.cc"
	"features/fx/spr_effect_lab.h"
	"features/fx/weapon_dual_sprites.cc"
	"features/fx/weapon_dual_sprites.h"
	"features/fx/zone_recorder.cc"
	"features/fx/zone_recorder.h"
	"features/fx/weapon_layer.cc"
	"features/fx/weapon_layer.h"

	# systems/
	"features/systems/auto_login.cc"
	"features/systems/auto_login.h"
	"features/systems/bourgeon_opcodes.h"
	"features/systems/bug_report.cc"
	"features/systems/bug_report.h"
	"features/systems/cheat_detector.cc"
	"features/systems/cheat_detector.h"
	"features/systems/discord_relay.cc"
	"features/systems/discord_relay.h"
	"features/systems/image_preview.cc"
	"features/systems/image_preview.h"
	"features/systems/dx7_warning.cc"
	"features/systems/dx7_warning.h"
	"features/systems/integrity_check.cc"
	"features/systems/integrity_check.h"
	"features/systems/moonlight_auth.cc"
	"features/systems/moonlight_auth.h"
	"features/systems/native_login.cc"
	"features/systems/native_login.h"
	"features/systems/ui_caps.cc"
	"features/systems/ui_caps.h"

	# minigames/
	"features/minigames/doom.cc"
	"features/minigames/doom.h"
	"features/minigames/roggle.cc"
	"features/minigames/roggle.h"
	"features/minigames/rojeweled.cc"
	"features/minigames/rojeweled.h"

	# UI toolkit (socle ImGui « façon RO »)
	"ui/ro_imgui.cc"
	"ui/ro_imgui.h"
	"ui/ro_skin_blobs.hpp"
	"ui/ro_widgets.cc"
	"ui/ro_widgets.h"
	"ui/qty_prompt.cc"
	"ui/qty_prompt.h"
	"ui/align_grid.cc"
	"ui/align_grid.h"
	"ui/window_clamp.cc"
	"ui/window_clamp.h"
	"ui/window_zorder.cc"
	"ui/window_zorder.h"
	"ui/game_texture.cc"
	"ui/game_texture.h"
	"ui/spr_act.cc"
	"ui/spr_act.h"
	"ui/palette_ramps.cc"
	"ui/palette_ramps.h"
	"ui/sprite_view.cc"
	"ui/sprite_view.h"
	"ui/mob_sprite.cc"
	"ui/mob_sprite.h"
	# Monstres 3D (Emperium, gardiens, drapeau, coffres) : lecture .gr2 via la
	# granny2.dll du client, rendu logiciel, affichage comme un sprite animé.
	"ui/gr2_model.cc"
	"ui/gr2_model.h"
	"ui/model_raster.cc"
	"ui/model_raster.h"
	"ui/mob_model.cc"
	"ui/mob_model.h"
	"ui/game_emotes.cc"
	"ui/game_emotes.h"
	"ui/emoji_set.cc"
	"ui/emoji_set.h"
	"ui/doll.cc"
	"ui/doll.h"
	"ui/icon_cache.cc"
	"ui/icon_cache.h"
	"ui/head_icon.cc"
	"ui/head_icon.h"
	"ui/sprite_path.cc"
	"ui/sprite_path.h"
	"ui/color_codec.cc"
	"ui/color_codec.h"
	"ui/skin_panel.cc"
	"ui/skin_panel.h"
	"ui/imgui_escape.h"

	# Ragnarok
	"ragnarok/uiwnd.h"
	"ragnarok/globals.h"
	"ragnarok/grf_index.cc"
	"ragnarok/grf_index.h"
	"ragnarok/player_skills.cc"
	"ragnarok/player_skills.h"
	"ragnarok/homunculus.cc"
	"ragnarok/homunculus.h"
	"ragnarok/item_db.h"
	"ragnarok/msgstring.cc"
	"ragnarok/msgstring.h"
	"ragnarok/msgstring_override.cc"
	"ragnarok/msgstring_override.h"
	"ragnarok/pet.cc"
	"ragnarok/pet.h"
	"ragnarok/user_hotkey.cc"
	"ragnarok/user_hotkey.h"
	"ragnarok/game_settings.cc"
	"ragnarok/game_settings.h"
	"ragnarok/render.h"
	"ragnarok/lua.h"
	"ragnarok/configuration.h"
	"ragnarok/ragnarok_client.cc"
	"ragnarok/ragnarok_client.h"
	"ragnarok/object_factory.cc"
	"ragnarok/object_factory.h"
	"ragnarok/packets.h"
	"ragnarok/talktype.h"
	"ragnarok/session.cc"
	"ragnarok/session.h"
	"ragnarok/item_info.h"
	"ragnarok/rag_connection.cc"
	"ragnarok/rag_connection.h"
	"ragnarok/skill_cooldowns.cc"
	"ragnarok/skill_cooldowns.h"
	"ragnarok/ui_window_mgr.cc"
	"ragnarok/ui_window_mgr.h"
	"ragnarok/mode_mgr.cc"
	"ragnarok/mode_mgr.h"
	"ragnarok/login_mode.cc"
	"ragnarok/login_mode.h"
	"ragnarok/game_mode.cc"
	"ragnarok/game_mode.h"
	"ragnarok/held_sprites.cc"
	"ragnarok/held_sprites.h"
	"ragnarok/own_actor.cc"
	"ragnarok/own_actor.h"

        # Layouts
	"ragnarok/object_layouts/session/layouts.h"
	"ragnarok/object_layouts/session/macro_utils.h"
	"ragnarok/object_layouts/session/20151102.h"
	"ragnarok/object_layouts/session/20170613.h"
	"ragnarok/object_layouts/session/20190116.h"
	"ragnarok/object_layouts/session/20250716.h"

	# Utils
	"utils/byte_pattern.cc"
	"utils/byte_pattern.h"
	"utils/frame_profiler.cc"
	"utils/frame_profiler.h"
	"utils/gif_writer.cc"
	"utils/gif_writer.h"
	"utils/game_paths.cc"
	"utils/game_paths.h"
	"utils/i18n.cc"
	"utils/i18n.h"
	"utils/log_console.cc"
	"utils/log_console.h"
	"utils/startup_settings.cc"
	"utils/startup_settings.h"
	"utils/tinf_inflate.h"
	"utils/hooking/detours.h"
	"utils/hooking/disasm.cc"
	"utils/hooking/hook_manager.cc"
	"utils/hooking/hook_manager.h"
	"utils/hooking/proxy.h"

        # imgui
        "imgui/imgui_impl_dx7.cc"
        "imgui/imgui_impl_dx7.h"

        # D3D9 hook (for clients that use Direct3D9 instead of DX7)
        "d3d9/d3d9_hook.cc"
        "d3d9/d3d9_hook.h"
)
foreach(source IN LISTS BOURGEON_FILES)
    get_filename_component(source_path "${source}" PATH)
    string(REPLACE "/" "\\" source_path_msvc "${source_path}")
    source_group("${source_path_msvc}" FILES "${source}")
endforeach()

add_library(bourgeon MODULE ${BOURGEON_FILES})
set_target_properties(bourgeon PROPERTIES OUTPUT_NAME "ddraw")
target_include_directories(bourgeon PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(bourgeon PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/json/include")
target_compile_options(bourgeon PRIVATE "/MT")
target_link_options(bourgeon PRIVATE "/MAP")

# Compilation parallèle MSVC : un process cl.exe par cœur au lieu d'un seul, pour
# les 66 unités de compilation de la cible. Aucun effet sur le binaire produit.
target_compile_options(bourgeon PRIVATE "/MP")

# En-tête précompilé (voir src/pch.h : tiers stable UNIQUEMENT). Injecté par CMake,
# donc aucun fichier source ne l'inclut et chacun reste autonome sans lui.
# En cas de doute sur une erreur de compilation inattendue, isoler avec :
#   cmake -S . -B build -DBOURGEON_USE_PCH=OFF
# NOTE : le unity build a été délibérément ÉCARTÉ ici — 43 fichiers de src/features
# ouvrent un `namespace {` anonyme avec ~237 noms en collision (kMakeKey dans 16
# fichiers, kTexMgr/kUIWindowMgr dans 13) ; le fusionner produirait des ODR, que ce
# projet a déjà payés en crashes. /MP + PCH donnent le même gain sans ce risque.
option(BOURGEON_USE_PCH "Utiliser l'en-tête précompilé src/pch.h" ON)
if(BOURGEON_USE_PCH)
    target_precompile_headers(bourgeon PRIVATE "pch.h")
endif()
target_compile_definitions(bourgeon PUBLIC -DNOMINMAX)
target_compile_definitions(bourgeon PRIVATE -DBOURGEON_VERSION="${PROJECT_VERSION}")
target_compile_definitions(bourgeon PRIVATE $<$<CONFIG:RelWithDebInfo>:BOURGEON_DEBUG>)

# winmm: timeGetTime for the DOOM 35 Hz clock (doom).
# dxguid: définitions des IID DirectX (IID_IDirect3D7, IID_IDirect3DHALDevice,
#   IID_IDirect3DTnLHalDevice…). Elles provenaient jusqu'ici du seul ddraw.cc, qui
#   pose `#define INITGUID` avant <d3d.h>/<ddraw.h> — un mécanisme qui exige que RIEN
#   n'ait tiré guiddef.h auparavant, sinon son include guard fige DEFINE_GUID en mode
#   « déclaration seule » et les symboles manquent au link. Cette condition est trop
#   fragile (tout en-tête ajouté en amont la casse — l'en-tête précompilé l'a fait) :
#   on prend les GUID dans la bibliothèque officielle, ce qui rend le lien indépendant
#   de l'ordre d'inclusion. Sans risque de doublon : le linker n'extrait d'une lib
#   statique que les symboles restés non résolus.
target_link_libraries(bourgeon PRIVATE spdlog yaml-cpp imgui doomgeneric winhttp bcrypt crypt32 shell32 winmm comdlg32 ole32 dxguid)

# Emit the SHA-256 of the freshly-built DLL (prints it and writes a sidecar
# ddraw.dll.sha256). Use it to fill the `hash:` line in the server's
# conf/bourgeon_integrity.conf for the integrity check.
add_custom_command(TARGET bourgeon POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -D "DLL=$<TARGET_FILE:bourgeon>"
        -D "OUT=$<TARGET_FILE_DIR:bourgeon>/ddraw.dll.sha256"
        -P "${CMAKE_SOURCE_DIR}/cmake/sha256.cmake"
    VERBATIM
)

if(RO_DEPLOY_DIR)
    add_custom_command(TARGET bourgeon POST_BUILD
        # Best-effort copy: if the game is running it holds ddraw.dll open, so
        # fall back to a message instead of failing the whole build.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:bourgeon>"
            "${RO_DEPLOY_DIR}/ddraw.dll"
            || ${CMAKE_COMMAND} -E echo "Deploy skipped (ddraw.dll in use — close the game)"
        COMMENT "Deployed to ${RO_DEPLOY_DIR}"
    )

    # Les catalogues de langue (tools/lang/*.yaml) vers SaveData\lang\ du client.
    #
    # 🔴 Une cible À PART, et non un POST_BUILD de `bourgeon` : MSBuild ne relance
    # les étapes post-build que lorsque la cible a été relinkée, or retoucher une
    # traduction ne recompile RIEN — le cas d'usage le plus courant du chantier
    # i18n aurait donc été le seul à ne rien déployer. Une custom target sans
    # OUTPUT est toujours tenue pour périmée : elle tourne à chaque build, et le
    # script ne recopie que les fichiers dont le CONTENU diffère.
    add_custom_target(bourgeon_deploy_lang ALL
        COMMAND ${CMAKE_COMMAND}
            -D "SRC_DIR=${CMAKE_SOURCE_DIR}/tools/lang"
            -D "GAME_DIR=${RO_DEPLOY_DIR}"
            -P "${CMAKE_SOURCE_DIR}/cmake/deploy_lang.cmake"
        COMMENT "Deploying language catalogs to ${RO_DEPLOY_DIR}/SaveData/lang"
        VERBATIM
    )
    # Après la DLL, et pas du tout si elle n'a pas été produite : un catalogue neuf
    # en face d'un binaire qui n'a pas compilé ne ferait qu'égarer le diagnostic.
    add_dependencies(bourgeon_deploy_lang bourgeon)
endif()
