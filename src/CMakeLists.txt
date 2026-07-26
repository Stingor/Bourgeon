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

	# Plugins (compiled-in, no runtime scripting)
	"plugins/plugin.h"
	"plugins/staff_gate.h"
	"plugins/chat.cc"
	"plugins/chat.h"
	"plugins/chat_bg.cc"
	"plugins/status_tweaks.cc"
	"plugins/status_tweaks.h"
	"plugins/moonlight_ui.cc"
	"plugins/moonlight_ui.h"
	# Panneaux extraits d'OnRenderUI (en-tête privé au dossier)
	"plugins/moonlight_ui/internal.h"
	"plugins/moonlight_ui/panel_rules.cc"
	"plugins/moonlight_ui/panel_commands.cc"
	"plugins/moonlight_ui/panel_fun.cc"
	"plugins/moonlight_ui/item_desc_probe.cc"
	"plugins/moonlight_ui/panel_interface.cc"
	"plugins/moonlight_ui/settings_table.cc"
	"plugins/moonlight_ui/settings_table.h"
	"plugins/moonlight_ui/settings_containers.cc"
	"plugins/moonlight_ui/settings_containers.h"
	"plugins/discord_relay.cc"
	"plugins/discord_relay.h"
	"plugins/auto_login.cc"
	"plugins/auto_login.h"
	"plugins/moonlight_auth.cc"
	"plugins/moonlight_auth.h"
	"plugins/native_login.cc"
	"plugins/native_login.h"
	"plugins/char_select.cc"
	"plugins/char_select.h"
	"plugins/integrity_check.cc"
	"plugins/integrity_check.h"
	"plugins/cheat_detector.cc"
	"plugins/cheat_detector.h"
	"plugins/dps_meter.cc"
	"plugins/entity_names.cc"
	"plugins/entity_names.h"
	"plugins/dps_meter.h"
	"plugins/basic_info.cc"
	"plugins/basic_info.h"
	"plugins/menu_icons.cc"
	"plugins/menu_icons.h"
	"plugins/inventory_tweaks.cc"
	"plugins/inventory_tweaks.h"
	"plugins/inventory_viewer.cc"
	"plugins/inventory_viewer.h"
	"plugins/storage_tweaks.cc"
	"plugins/storage_tweaks.h"
	"plugins/cashshop_tweaks.cc"
	"plugins/cashshop_tweaks.h"
	"plugins/shop_tweaks.cc"
	"plugins/shop_tweaks.h"
	"plugins/trade_tweaks.cc"
	"plugins/trade_tweaks.h"
	"plugins/rodex_tweaks.cc"
	"plugins/rodex_tweaks.h"
	"plugins/npc_dialog_tweaks.cc"
	"plugins/npc_dialog_tweaks.h"
	"plugins/bug_report.cc"
	"plugins/bug_report.h"
	"plugins/character_sheet.cc"
	"plugins/character_sheet.h"
	"plugins/login_parade.cc"
	"plugins/login_parade.h"
	"plugins/equip_tweaks.cc"
	"plugins/equip_tweaks.h"
	"plugins/window_pos_tweaks.cc"
	"plugins/window_pos_tweaks.h"
	"plugins/status_icon_tweaks.cc"
	"plugins/status_icon_tweaks.h"
	"plugins/quest_tracker_tweaks.cc"
	"plugins/quest_tracker_tweaks.h"
	"plugins/settings_tweaks.cc"
	"plugins/settings_tweaks.h"
	"plugins/weapon_layer.cc"
	"plugins/weapon_layer.h"
	"plugins/weapon_dual_sprites.cc"
	"plugins/weapon_dual_sprites.h"
	"plugins/berserk_chat_unlock.cc"
	"plugins/berserk_chat_unlock.h"
	"plugins/ez_effect_capture.cc"
	"plugins/ez_effect_capture.h"
	"plugins/spr_effect_lab.cc"
	"plugins/spr_effect_lab.h"
	"plugins/hat_effect_depth.cc"
	"plugins/hat_effect_depth.h"
	"plugins/skill_tree_tweaks.cc"
	"plugins/skill_tree_tweaks.h"
	"plugins/fps_view.cc"
	"plugins/fps_view.h"
	"plugins/player_jump.cc"
	"plugins/player_jump.h"
	"plugins/keyboard_move.cc"
	"plugins/keyboard_move.h"
	"plugins/doom_tweaks.cc"
	"plugins/doom_tweaks.h"
	"plugins/roggle_tweaks.cc"
	"plugins/roggle_tweaks.h"
	"plugins/rojeweled_tweaks.cc"
	"plugins/rojeweled_tweaks.h"
	"plugins/skill_bar_tweaks.cc"
	"plugins/skill_bar_tweaks.h"
	"plugins/item_desc_tweaks.cc"
	"plugins/item_desc_tweaks.h"
	"plugins/dx7_warning.cc"
	"plugins/dx7_warning.h"

	# UI toolkit (socle ImGui « façon RO »)
	"ui/ro_imgui.cc"
	"ui/ro_imgui.h"
	"ui/ro_skin_blobs.hpp"
	"ui/ro_widgets.cc"
	"ui/ro_widgets.h"
	"ui/align_grid.cc"
	"ui/align_grid.h"
	"ui/window_clamp.cc"
	"ui/window_clamp.h"
	"ui/game_texture.cc"
	"ui/game_texture.h"
	"ui/icon_cache.cc"
	"ui/icon_cache.h"
	"ui/color_codec.cc"
	"ui/color_codec.h"
	"ui/skin_panel.cc"
	"ui/skin_panel.h"

	# Ragnarok
	"ragnarok/uiwnd.h"
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
	"ragnarok/ui_window_mgr.cc"
	"ragnarok/ui_window_mgr.h"
	"ragnarok/mode_mgr.cc"
	"ragnarok/mode_mgr.h"
	"ragnarok/login_mode.cc"
	"ragnarok/login_mode.h"
	"ragnarok/game_mode.cc"
	"ragnarok/game_mode.h"

        # Layouts
	"ragnarok/object_layouts/session/layouts.h"
	"ragnarok/object_layouts/session/macro_utils.h"
	"ragnarok/object_layouts/session/20151102.h"
	"ragnarok/object_layouts/session/20170613.h"
	"ragnarok/object_layouts/session/20190116.h"

	# Utils
	"utils/byte_pattern.cc"
	"utils/byte_pattern.h"
	"utils/frame_profiler.cc"
	"utils/frame_profiler.h"
	"utils/gif_writer.cc"
	"utils/gif_writer.h"
	"utils/game_paths.cc"
	"utils/game_paths.h"
	"utils/log_console.cc"
	"utils/log_console.h"
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
# NOTE : le unity build a été délibérément ÉCARTÉ ici — 43 fichiers de src/plugins
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

# winmm: timeGetTime for the DOOM 35 Hz clock (doom_tweaks).
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
endif()
