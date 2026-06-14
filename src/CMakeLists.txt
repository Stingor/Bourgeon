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
	"plugins/moonlight_ui.cc"
	"plugins/moonlight_ui.h"
	"plugins/discord_relay.cc"
	"plugins/discord_relay.h"
	"plugins/auto_login.cc"
	"plugins/auto_login.h"

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
	"utils/log_console.cc"
	"utils/log_console.h"
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

target_link_libraries(bourgeon PRIVATE spdlog yaml-cpp imgui winhttp bcrypt shell32)

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
