#pragma once

// ── Le dessin d'une case au sol ──────────────────────────────────────────────
//
// Deux modules peignent des cases sur le terrain : `grey_world` (le quadrillage
// de la carte) et `skill_range` (la zone d'un sort armé). Ils n'ont rien d'autre
// en commun — l'un encadre la passe terrain du renderer, l'autre lit la table de
// portées du Lua — mais ils dessinent EXACTEMENT la même case, avec les mêmes
// trois textures et le même réglage.
//
// 🔴 Ils en tenaient chacun une copie. `skill_range.cc` le disait même en
// commentaire : « Les trois dessins, les mêmes que GreyWorld ». Une texture
// renommée d'un seul côté ne se détecte pas en code : quand un nom ne résout
// pas, le client rend son sprite de REPLI plutôt que nullptr (cf. le
// commentaire IDB de `SpriteRes_GetOrLoadByName`) — ça se voit à l'écran, et
// nulle part ailleurs.
//
// Ce que ce fichier NE porte pas : l'énumération persistée de chaque module.
// `grey_world::Config::Pattern` et `skill_range::Config::Pattern` restent chez
// eux, parce que ce sont leurs réglages joueur ; chacun vérifie par
// `static_assert` qu'il s'aligne encore sur la table ci-dessous.
namespace cellstyle {

// Une texture, et le texel qu'on y prélève.
struct Tex {
  const char* texture;
  float u, v;  // < 0 : texture entière (UV par défaut du client)
};

// 🔴 L'ORDRE EST UN CONTRAT : c'est l'index qui part dans le yaml des joueurs
// (« greyworld_pattern », « skillrange_pattern »). Insérer une entrée au milieu
// change le dessin de tout le monde. On ajoute à la FIN.
//
// Un texel étiré aux quatre coins donne un aplat de la couleur diffuse, ce qui
// laisse le joint tracer la bordure — d'où l'UV explicite du carreau plein.
inline constexpr Tex kTextures[] = {
    {"grid.tga",                -1.0f, -1.0f},  // anneau
    {"effect\\SquareRange.tga", -1.0f, -1.0f},  // carrelage
    {"bourgeon_cell.tga",        0.5f,  0.5f},  // carreau plein : un seul texel
};

inline constexpr int kCount = static_cast<int>(sizeof(kTextures) /
                                               sizeof(kTextures[0]));

// L'index du CARREAU PLEIN, le seul dessin sur lequel le joint ait un sens :
// creuser un cadre ou un anneau ne ferait que les rapetisser. Les deux modules
// comparent leur énumération à celle-ci.
inline constexpr int kSolid = 2;

// Le bloc de réglage commun : le combo « Dessin », son infobulle, et le curseur
// de joint qui n'apparaît que sur le carreau plein. Rend true si le joueur a
// touché à l'un des deux.
//
// ⚠ `gap_tooltip` est à la charge de l'appelant, et ce n'est pas un oubli : ce
// que le joint fait apparaître n'est pas la même chose des deux côtés (un
// quadrillage qui disparaît, une zone qui devient un aplat d'un seul tenant).
// Les deux textes existent déjà séparément au catalogue de traduction ; les
// fondre en un seul en perdrait un. Passer le résultat d'`i18n::Tr`.
bool DrawPatternSettings(int* pattern, int* gap, const char* gap_tooltip);

}  // namespace cellstyle
