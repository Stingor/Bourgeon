# Compilation parallèle MSVC pour TOUTES les bibliothèques ci-dessous. Posé ici,
# le flag est hérité par chaque add_subdirectory qui suit — donc par les
# submodules eux-mêmes, sans avoir à les modifier.
#
# Seule la cible `bourgeon` portait /MP : les tiers compilaient un fichier à la
# fois et dominaient le build complet. Mesure du 2026-08-12 (20 cœurs, horodatage
# des .obj) : 90 s de build clean, dont 63 s de tiers — 47 s pour yaml-cpp SEUL
# (32 unités en séquentiel) — contre 27 s pour les 134 unités de bourgeon.
# Aucun effet sur les binaires produits, c'est un flag d'ordonnancement.
if(MSVC)
    add_compile_options(/MP)
endif()

add_subdirectory(spdlog)
add_subdirectory(yaml-cpp)
# AVANT imgui : c'est lui qui consomme la cible `freetype` (rasteriseur couleur).
add_subdirectory(freetype)
add_subdirectory(imgui)
add_subdirectory(doomgeneric)