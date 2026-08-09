# FreeType — uniquement pour le rasteriseur de police d'ImGui (imgui_freetype).
#
# stb_truetype, le rasteriseur intégré d'ImGui, ne sait rien des glyphes en
# COULEUR : il ne lit que les contours `glyf`. Un emoji de Segoe UI Emoji en
# sortirait en dessin au trait monochrome — lisible, mais pas ce qu'on veut.
# FreeType, lui, rend les tables COLR/CPAL (v0 ET v1) en bitmaps BGRA, que
# imgui_freetype recopie tels quels dans l'atlas.
#
# ⚠ VERSION ÉPINGLÉE À 2.14.3 (submodule). Le rendu COLRv1 — celui qu'emploie
# le Segoe UI Emoji de Windows 11 — demande FreeType ≥ 2.13 ; 2.14.3 est la
# version sur laquelle le rendu couleur de seguiemj.ttf a été vérifié.
#
# Toutes les dépendances optionnelles sont coupées : elles ne servent qu'à des
# formats qu'on ne charge pas (fonts gzip/bzip2, bitmaps PNG des polices emoji
# Android/Noto, shaping HarfBuzz, WOFF2). Sans ça, CMake irait les chercher sur
# la machine du développeur et le build casserait selon ce qui y traîne.
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL    ON CACHE BOOL "" FORCE)

add_subdirectory(freetype EXCLUDE_FROM_ALL)
