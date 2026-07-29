# doomgeneric (https://github.com/ozkl/doomgeneric) vendored as a static C lib.
# Platform mains (doomgeneric_win/sdl/xlib/...) are EXCLUDED — the DG_* platform
# callbacks live in src/plugins/doom_tweaks.cc. Sound backends are excluded too:
# FEATURE_SOUND is undefined, i_sound.c is the NULL-guarded no-op dispatcher.
# Vendor patches are marked "BOURGEON PATCH" (no exit(), no melt wipe, config in
# the doom\ subfolder). See project_doom_in_ro memory for the integration notes.
enable_language(C)

add_library(doomgeneric STATIC
	"am_map.c"
	"doomdef.c"
	"doomgeneric.c"
	"doomstat.c"
	"dstrings.c"
	"dummy.c"
	"d_event.c"
	"d_items.c"
	"d_iwad.c"
	"d_loop.c"
	"d_main.c"
	"d_mode.c"
	"d_net.c"
	"f_finale.c"
	"f_wipe.c"
	"g_game.c"
	"hu_lib.c"
	"hu_stuff.c"
	"info.c"
	"i_cdmus.c"
	"i_dsound.c"
	"i_endoom.c"
	"i_input.c"
	"i_joystick.c"
	"i_scale.c"
	"i_sound.c"
	"i_system.c"
	"i_timer.c"
	"i_video.c"
	"memio.c"
	"m_argv.c"
	"m_bbox.c"
	"m_cheat.c"
	"m_config.c"
	"m_controls.c"
	"m_fixed.c"
	"m_menu.c"
	"m_misc.c"
	"m_random.c"
	"p_ceilng.c"
	"p_doors.c"
	"p_enemy.c"
	"p_floor.c"
	"p_inter.c"
	"p_lights.c"
	"p_map.c"
	"p_maputl.c"
	"p_mobj.c"
	"p_plats.c"
	"p_pspr.c"
	"p_saveg.c"
	"p_setup.c"
	"p_sight.c"
	"p_spec.c"
	"p_switch.c"
	"p_telept.c"
	"p_tick.c"
	"p_user.c"
	"r_bsp.c"
	"r_data.c"
	"r_draw.c"
	"r_main.c"
	"r_plane.c"
	"r_segs.c"
	"r_sky.c"
	"r_things.c"
	"sha1.c"
	"sounds.c"
	"statdump.c"
	"st_lib.c"
	"st_stuff.c"
	"s_sound.c"
	"tables.c"
	"v_video.c"
	"wi_stuff.c"
	"w_checksum.c"
	"w_file.c"
	"w_file_stdc.c"
	"w_main.c"
	"w_wad.c"
	"z_zone.c"
)

# The plugin includes doomgeneric.h / doomkeys.h from here.
target_include_directories(doomgeneric PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# Vanilla 1993 C: silence the deprecation/sign-compare flood, keep our own
# warning level clean. /MT is inherited from CMAKE_MSVC_RUNTIME_LIBRARY.
# /W0 (et non /w) : le generateur VS le traduit en <WarningLevel>TurnOffAllWarnings,
# alors que /w partirait en AdditionalOptions et entrerait en conflit avec le
# WarningLevel implicite de MSBuild (warning D9025 a chaque compilation).
target_compile_options(doomgeneric PRIVATE /W0)
# FEATURE_SOUND routes sfx through our DirectSound backend (i_dsound.c, which
# provides the DG_sound_module/DG_music_module the dispatcher expects).
target_compile_definitions(doomgeneric PRIVATE
	_CRT_SECURE_NO_WARNINGS
	_CRT_NONSTDC_NO_DEPRECATE
	FEATURE_SOUND
)
target_link_libraries(doomgeneric PUBLIC dsound)
