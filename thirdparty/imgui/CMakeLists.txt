add_library(imgui STATIC
  "imgui/imconfig.h"
  "imgui/imgui.cpp"
  "imgui/imgui.h"
  "imgui/imgui_demo.cpp"
  "imgui/imgui_draw.cpp"
  "imgui/imgui_internal.h"
  "imgui/imgui_tables.cpp"
  "imgui/imgui_widgets.cpp"
  "imgui/imstb_rectpack.h"
  "imgui/imstb_textedit.h"
  "imgui/imstb_truetype.h"

  "imgui/misc/freetype/imgui_freetype.cpp"
  "imgui/misc/freetype/imgui_freetype.h"

  "imgui/backends/imgui_impl_win32.cpp"
  "imgui/backends/imgui_impl_win32.h"

  "imgui/backends/imgui_impl_dx9.cpp"
  "imgui/backends/imgui_impl_dx9.h"
)
target_include_directories(imgui PUBLIC "imgui" "imgui/misc/freetype")
target_link_libraries(imgui PUBLIC d3d9 freetype)

# ── Les deux définitions sont PUBLIQUES, et c'est vital ───────────────────────
# Elles changent la TAILLE de `ImWchar` et le rasteriseur de l'atlas. En PRIVATE,
# la DLL compilerait une moitié de son code avec un `ImWchar` de 16 bits et
# l'autre avec 32 : violation d'ODR silencieuse, donc des files de saisie lues de
# travers. Tout ce qui inclut imgui.h doit voir exactement le même type.
target_compile_definitions(imgui PUBLIC
  # Le rasteriseur FreeType à la place de stb_truetype : seul lui sait rendre les
  # tables COLR/CPAL, c'est-à-dire les glyphes en COULEUR des polices emoji.
  #
  # ⚠ IL DOIT ÊTRE GLOBAL, et ce n'est pas le choix qu'on aurait préféré : tout
  # le texte de l'interface est désormais rastérisé par FreeType, donc son dessin
  # bouge très légèrement (généralement plus net). ImGui expose pourtant un
  # rasteriseur PAR SOURCE (`ImFontConfig::FontLoader`), ce qui aurait laissé le
  # reste du texte intact — mais il le REFUSE pour FreeType :
  #   IM_ASSERT(FontLoader->LoaderInit == NULL ...); // FIXME-NEWATLAS: Unsupported yet.
  # (imgui_draw.cpp, ImFontAtlas::AddFont). Et ce n'est pas qu'un garde-fou :
  # `ImGui_ImplFreeType_FontSrcInit` lit sa `FT_Library` dans
  # `atlas->FontLoaderData`, que seul `LoaderInit` renseigne — en Release, où les
  # IM_ASSERT sautent, on déréférencerait un pointeur nul. À re-tester quand ImGui
  # lèvera ce FIXME.
  IMGUI_ENABLE_FREETYPE

  # 🔴 SANS ÇA, AUCUN EMOJI NE PEUT S'AFFICHER. Par défaut `ImWchar` fait 16 bits
  # et `IM_UNICODE_CODEPOINT_MAX` vaut 0xFFFF : tout point de code au-dessus —
  # c'est-à-dire TOUS les emoji, qui vivent au plan 1 (U+1F300…) — est remplacé
  # par U+FFFD au décodage de l'UTF-8, et rejeté à la saisie par
  # `AddInputCharacterUTF16`. Ce n'est ni une question de police ni d'atlas : le
  # caractère n'atteint jamais le rendu.
  IMGUI_USE_WCHAR32
)
