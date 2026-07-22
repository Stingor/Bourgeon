set(BOURGEON_FILES
	"main.cc"
	"bourgeon.cc"
	"bourgeon.h"

        # DDRAW Wrapper
        "ddraw/ddraw.def"
        "ddraw/ddraw.cc"
        "ddraw/ddraw.h"
        "ddraw/proxy_idirectdraw.cc"
        "ddraw/proxy_idirectdraw.h"

	# Plugins (compiled-in, no runtime scripting)
	"plugins/plugin.h"
	"plugins/chat.cc"
	"plugins/chat.h"
	"plugins/status_tweaks.cc"
	"plugins/status_tweaks.h"
	"plugins/moonlight_ui.cc"
	"plugins/moonlight_ui.h"
	"plugins/discord_relay.cc"
	"plugins/discord_relay.h"
	"plugins/auto_login.cc"
	"plugins/auto_login.h"
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

	# UI toolkit (socle ImGui « façon RO »)
	"ui/ro_imgui.cc"
	"ui/ro_imgui.h"
	"ui/ro_skin_blobs.hpp"

	# Ragnarok
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
target_compile_definitions(bourgeon PUBLIC -DNOMINMAX)
target_compile_definitions(bourgeon PRIVATE -DBOURGEON_VERSION="${PROJECT_VERSION}")
target_compile_definitions(bourgeon PRIVATE $<$<CONFIG:RelWithDebInfo>:BOURGEON_DEBUG>)

# winmm: timeGetTime for the DOOM 35 Hz clock (doom_tweaks).
target_link_libraries(bourgeon PRIVATE spdlog yaml-cpp imgui doomgeneric winhttp bcrypt shell32 winmm comdlg32 ole32)

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
