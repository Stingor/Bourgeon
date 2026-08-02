#include "ui/head_icon.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ui/sprite_view.h"

namespace ro {
namespace {

// Format-strings NATIVES (CP949) : « %d » = numéro de FICHIER de coiffure.
// 🔴 On les lit dans le binaire plutôt que de les écrire ici : nos sources sont
// en UTF-8, un littéral coréen y serait encodé en UTF-8 et ne désignerait aucun
// dossier du GRF. Mêmes adresses que les icônes de coiffure du char-select.
constexpr uintptr_t kFmtSprFemale = 0x0108F9BC;  // 인간족\머리통\여\%d_여.spr
constexpr uintptr_t kFmtSprMale   = 0x0108F9F4;  // 인간족\머리통\남\%d_남.spr

// Remap historique des 12 premières coiffures : id de coiffure -> numéro de FICHIER
// .spr. Le client ne nomme pas le fichier d'après l'id brut, il passe par une table
// de noms (celle qu'indexe Job_BuildBodyOrHeadSpritePath_impl) ; sans ce remap la
// ligne de guilde affiche une coupe voisine. Table identique à celle du char-select
// (vérifiée à l'œil : 1↔2, 3→7/6→3/7→6, 4↔5, 11↔12 ; 8/9/10 et 13+ = identité), et
// préservée par le patch WARP Allow65kHairs pour les index < 43.
int HairFile(int hair) {
  static const int kHairFile[13] = {0, 2, 1, 7, 5, 4, 3, 6, 8, 9, 10, 12, 11};
  return (hair >= 1 && hair <= 12) ? kHairFile[hair] : hair;
}

// Chemin VFS du sprite de tête, SANS extension.
//
// 🔴 `data\sprite\`, PAS `data\`. Le gabarit du client est relatif à la racine
// des SPRITES et deux couches natives le complètent : `UITextureMgr_Load`
// (0x00a8d4a0) ajoute un préfixe choisi selon l'EXTENSION (« sprite\ » pour
// .spr/.act), puis `Res_MakeDataRootRelativePath` (0x00573340) ajoute « data\ ».
// On court-circuite les deux, donc on pose les deux.
void BasePathFor(int hair_file, int sex, char* out, size_t out_size) {
  char tail[192];
  // ⚠ `std::snprintf` et non `_snprintf_s` : le gabarit n'est pas un littéral
  // (il est lu dans le binaire du client), et la famille sécurisée déclenche
  // alors C4774.
  std::snprintf(tail, sizeof(tail),
                reinterpret_cast<const char*>(sex ? kFmtSprMale : kFmtSprFemale),
                hair_file);
  const size_t n = std::strlen(tail);
  if (n > 4) tail[n - 4] = '\0';  // retire « .spr » : sprite_view veut une base
  std::snprintf(out, out_size, "data\\sprite\\%s", tail);
}

// Palette de couleur de cheveux. Le dossier 머리 est écrit en CP949 ÉCHAPPÉ
// (B8 D3 B8 AE) : c'est bien l'encodage du GRF, pas de l'UTF-8 déguisé.
//
// Préfixe `data\palette\` : `CPaletteRes_RegisterResFactory` (0x00446850)
// enregistre « palette\ » comme préfixe d'extension des .pal, puis « data\ »
// s'ajoute comme pour tout le reste — vérifié dans le binaire, pas supposé.
void PalettePathFor(int color, char* out, size_t out_size) {
  std::snprintf(out, out_size, "data\\palette\\\xB8\xD3\xB8\xAE\\head_%d.pal",
                color);
}

}  // namespace

bool DrawHeadIcon(ImDrawList* draw_list, float x, float y, float box, int hair,
                  int sex, int hair_color, bool allow_upscale) {
  if (!draw_list || box <= 1.0f || hair < 0) return false;

  char base[256];
  // L'id de coiffure n'est PAS le numéro de fichier pour les 12 premières.
  BasePathFor(HairFile(hair), sex ? 1 : 0, base, sizeof(base));

  char pal[96] = {0};
  if (hair_color >= 0) PalettePathFor(hair_color, pal, sizeof(pal));

  // Une palette introuvable n'est pas un échec : sprite_view retombe sur celle
  // du .spr, et la tête s'affiche dans sa couleur d'origine.
  SpriteRes res;
  if (!LoadSpriteRecolored(base, pal[0] ? pal : nullptr, &res)) return false;

  // Action 0 = de face. Cadence nulle = figé sur la première image : c'est une
  // vignette, elle n'a pas à cligner des yeux. sprite_view garde les proportions
  // et centre dans le carré.
  return DrawSprite(draw_list, res, ImVec2(x, y), ImVec2(x + box, y + box),
                    /*anim_seconds=*/0.0f, /*action=*/0, /*ms_per_frame=*/0.0f,
                    allow_upscale, /*alpha=*/1.0f);
}

}  // namespace ro
