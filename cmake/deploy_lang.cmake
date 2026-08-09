# Deploys the language catalogs of tools/lang/ into the client's SaveData\lang\,
# where i18n::ReloadCatalog reads them (see paths::LangPath in
# src/utils/game_paths.cc). Invoked by the bourgeon_deploy_lang target through
# `cmake -P`, with:
#   SRC_DIR   the tools/lang/ folder of the repository
#   GAME_DIR  the client's root folder (RO_DEPLOY_DIR)
#
# BEST-EFFORT, just like the ddraw.dll copy: an unreachable client folder (E:\ on
# another machine) or a locked file is reported and the build keeps going. The DLL
# is what the build produces; the catalogs are a convenience on top.
#
# The list of catalogs is globbed HERE and not passed in from the configure step,
# on purpose: this script runs on every build, so dropping a new <code>.yaml into
# tools/lang/ deploys it without re-running cmake. Only `.missing.yaml` files are
# skipped — those are export templates for a translator, produced by
# i18n::ExportMissing, and installing one would blank out translated labels
# (an empty value counts as untranslated, see i18n::Tr).

if(NOT GAME_DIR OR NOT SRC_DIR)
  message(FATAL_ERROR "deploy_lang.cmake: SRC_DIR and GAME_DIR are required")
endif()

if(NOT IS_DIRECTORY "${GAME_DIR}")
  message(STATUS "Lang deploy skipped (${GAME_DIR} unreachable)")
  return()
endif()

file(GLOB catalogs "${SRC_DIR}/*.yaml")

set(lang_dir "${GAME_DIR}/SaveData/lang")
file(MAKE_DIRECTORY "${lang_dir}")

set(updated "")
foreach(source IN LISTS catalogs)
  get_filename_component(name "${source}" NAME)
  if(name MATCHES "\\.missing\\.yaml$")
    continue()
  endif()

  set(target "${lang_dir}/${name}")

  # Compared by CONTENT, not by timestamp: the catalogs come out of git, whose
  # checkouts freely rewrite mtimes, and rewriting an identical file would churn
  # the client folder on every single build.
  set(needs_copy TRUE)
  if(EXISTS "${target}")
    file(SHA256 "${source}" source_hash)
    file(SHA256 "${target}" target_hash)
    if(source_hash STREQUAL target_hash)
      set(needs_copy FALSE)
    endif()
  endif()
  if(NOT needs_copy)
    continue()
  endif()

  # Binary copy, so the CRLF line endings of the catalogs survive untouched.
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy "${source}" "${target}"
    RESULT_VARIABLE copy_failed
    ERROR_VARIABLE copy_error
  )
  if(copy_failed)
    message(WARNING "Lang deploy failed for ${name}: ${copy_error}")
  else()
    list(APPEND updated "${name}")
  endif()
endforeach()

if(updated)
  string(REPLACE ";" ", " updated_text "${updated}")
  message(STATUS "Lang deployed to ${lang_dir}: ${updated_text}")
else()
  message(STATUS "Lang catalogs already up to date in ${lang_dir}")
endif()
