add_subdirectory(spdlog)
add_subdirectory(yaml-cpp)
# AVANT imgui : c'est lui qui consomme la cible `freetype` (rasteriseur couleur).
add_subdirectory(freetype)
add_subdirectory(imgui)
add_subdirectory(doomgeneric)