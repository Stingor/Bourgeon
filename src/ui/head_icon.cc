#include "ui/head_icon.h"

#include <Windows.h>  // lstrcpynA + SEH autour des tables natives

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ui/sprite_path.h"
#include "ui/sprite_view.h"

namespace ro {
namespace {

// Gabarit NATIF du sprite de tête : `\머리통\%s\%s_%s.%s`, soit
// `\머리통\<sexe>\<n° de coiffure>_<sexe>.<extension>`.
//
// 🔴 Il est RELATIF au dossier de RACE. `Hair_BuildBodyOrHeadSpritePath_impl`
// (0x00b433b0) le colle derrière `Race_GetBodyPrefix6` — c'est ce préfixe qui
// vaut `도람족` pour un Doram. On lisait avant deux gabarits déjà assemblés
// (0x0108F9BC / 0x0108F9F4) qui portaient `인간족\` en dur : un Summoner n'avait
// donc pas de tête, et comme le corps souffrait du même mal, pas de pantin non
// plus.
//
// 🔴 On lit le gabarit dans le binaire plutôt que de l'écrire ici : nos sources
// sont en UTF-8, un littéral coréen y serait encodé en UTF-8 et ne désignerait
// aucun dossier du GRF.
constexpr uintptr_t kFmtHead = 0x01088A6C;

// ── Numéro de FICHIER d'une coiffure ─────────────────────────────────────────
//
// Le client ne nomme pas le .spr d'après l'id de coiffure : il indexe une table
// de numéros, peuplée au boot depuis les DataInfo Lua. C'est ce qui explique le
// remap historique des douze premières coupes (1↔2, 3→7, 4↔5, 11↔12…).
//
// Quatre tables, choisies par le SEXE et la RACE — un Doram n'a que dix coupes
// et ne partage pas la numérotation humaine. Chacune est un `std::vector<char*>`
// dont `begin` est à l'adresse ci-dessous et `end` juste après.
constexpr uintptr_t kHairNumMale        = 0x015FB30C;
constexpr uintptr_t kHairNumFemale      = 0x015FB318;
constexpr uintptr_t kHairNumDoramMale   = 0x0160052C;
constexpr uintptr_t kHairNumDoramFemale = 0x01600538;

// Les tables ne couvrent que les 43 premières coupes.
//
// 🔴 Au-delà, le client N'EN PREND PAS UNE PAR DÉFAUT — il écrit le numéro tel
// quel. C'est le patch WARP `Allow65kHairs` qui le veut : il neutralise le
// bornage (`Exe.SetJMP`) et greffe un itoa pour les index >= 43, en laissant le
// détour de table intact en deçà. ⚠ L'IDB est un exe VANILLA, où l'on ne lit
// que le bornage d'origine (repli sur la coupe 13, ou 10 pour un Doram) : s'y
// fier ferait porter la même coupe à toutes les coiffures modernes.
constexpr int kHairTableSize = 0x2B;

// Repli des douze premières coupes humaines, si la table n'est pas lisible :
// id de coiffure -> numéro de fichier .spr. Copie figée de ce que contient la
// table native, conservée uniquement comme filet — la table, elle, suit les
// .lub et les patchs.
int HairFileFallback(int hair) {
  static const int kHairFile[13] = {0, 2, 1, 7, 5, 4, 3, 6, 8, 9, 10, 12, 11};
  return (hair >= 1 && hair <= 12) ? kHairFile[hair] : hair;
}

// Numéro de fichier, en TEXTE (la table en contient des chaînes).
void HairNumToken(int hair, int sex, bool doram, char* out, size_t out_size) {
  out[0] = '\0';
  const uintptr_t table = doram ? (sex ? kHairNumDoramMale : kHairNumDoramFemale)
                                : (sex ? kHairNumMale : kHairNumFemale);
  if (hair >= 0 && hair < kHairTableSize) {
    __try {
      char** begin = *reinterpret_cast<char***>(table);
      char** end   = *reinterpret_cast<char***>(table + 4);
      // 🔴 On borne sur la taille RÉELLE du vecteur : celui des Doram ne compte
      // qu'une dizaine d'entrées, et une table pas encore peuplée en compte
      // zéro. Le natif, lui, ne borne rien.
      if (begin && end > begin && hair < static_cast<int>(end - begin)) {
        const char* name = begin[hair];
        if (name && *name) lstrcpynA(out, name, static_cast<int>(out_size));
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
  }
  if (out[0] == '\0')
    std::snprintf(out, out_size, "%d", doram ? hair : HairFileFallback(hair));
}

// Chemin VFS du sprite de tête, SANS extension.
//
// 🔴 `data\sprite\`, PAS `data\`. Le gabarit du client est relatif à la racine
// des SPRITES et deux couches natives le complètent : `UITextureMgr_Load`
// (0x00a8d4a0) ajoute un préfixe choisi selon l'EXTENSION (« sprite\ » pour
// .spr/.act), puis `Res_MakeDataRootRelativePath` (0x00573340) ajoute « data\ ».
// On court-circuite les deux, donc on pose les deux.
void BasePathFor(int hair, int sex, int job, char* out, size_t out_size) {
  char num[32];
  HairNumToken(hair, sex, IsDoramJob(job), num, sizeof(num));

  const char* sex_tok = SexFolder(sex);
  char tail[192];
  // ⚠ `std::snprintf` et non `_snprintf_s` : le gabarit n'est pas un littéral
  // (il est lu dans le binaire du client), et la famille sécurisée déclenche
  // alors C4774.
  std::snprintf(tail, sizeof(tail), reinterpret_cast<const char*>(kFmtHead),
                sex_tok, num, sex_tok, "spr");
  const size_t n = std::strlen(tail);
  if (n > 4) tail[n - 4] = '\0';  // retire « .spr » : sprite_view veut une base
  std::snprintf(out, out_size, "data\\sprite\\%s%s", RaceFolder(job), tail);
}

// Palette de couleur de cheveux. Le dossier 머리 est écrit en CP949 ÉCHAPPÉ
// (B8 D3 B8 AE) : c'est bien l'encodage du GRF, pas de l'UTF-8 déguisé.
//
// Préfixe `data\palette\` : `CPaletteRes_RegisterResFactory` (0x00446850)
// enregistre « palette\ » comme préfixe d'extension des .pal, puis « data\ »
// s'ajoute comme pour tout le reste — vérifié dans le binaire, pas supposé.
//
// ⚠ Le nom est écrit ici et non lu dans le binaire : le patch WARP n'édite pas
// la chaîne, il redirige le `push` vers une chaîne AJOUTÉE ailleurs (les `%.s`
// y avalent coiffure et sexe sans rien imprimer). L'adresse d'origine porte
// donc encore l'ancien gabarit, et la lire donnerait un chemin périmé.
//
// 🔴 Le Doram garde son dossier de RACE, et il ne peut pas en être autrement.
// `Hair_BuildHeadPalettePath_impl` (0x00b42db0) a deux gabarits — `%s\머리\머리…`
// pour un Doram, `머리\머리…` pour les autres — et `HeadPalUnisex` ne réécrit QUE
// le second. C'est le patch maison `DoramHeadPalShared`
// (`WARP0716/Scripts/Patches/SharedPalDoram.qjs`, 2026-08-04) qui redirige le
// premier vers `<race>\머리\head_<c>.pal`.
//
// ⚠ Fusionner les deux races sur un seul fichier a été ESSAYÉ et mesuré le
// 2026-08-04 : 90 % des pixels opaques d'une tête Doram tombent sur des index
// que `head_<c>.pal` laisse noirs (elle utilise les tranches 176/224/240, que la
// palette humaine ne définit pas). Les indexations sont incompatibles.
//
// ⚠ Les palettes Doram n'existent pas dans le client officiel : leurs couleurs
// vivent DANS le .spr. Celles du serveur sont générées depuis cette palette
// embarquée. Sans ces fichiers, la tête reste noire.
void PalettePathFor(int color, int job, char* out, size_t out_size) {
  if (IsDoramJob(job)) {
    std::snprintf(out, out_size,
                  "data\\palette\\%s\\\xB8\xD3\xB8\xAE\\head_%d.pal",
                  RaceFolder(job), color);
    return;
  }
  std::snprintf(out, out_size, "data\\palette\\\xB8\xD3\xB8\xAE\\head_%d.pal",
                color);
}

}  // namespace

bool HeadSpriteBasePath(int hair, int sex, int job, char* out,
                        size_t out_size) {
  if (!out || out_size == 0 || hair < 0) return false;
  out[0] = '\0';
  BasePathFor(hair, sex ? 1 : 0, job, out, out_size);
  return out[0] != '\0';
}

bool HairPalettePath(int color, int job, char* out, size_t out_size) {
  if (!out || out_size == 0 || color < 0) return false;
  out[0] = '\0';
  PalettePathFor(color, job, out, out_size);
  return out[0] != '\0';
}

bool DrawHeadIcon(ImDrawList* draw_list, float x, float y, float box, int hair,
                  int sex, int hair_color, bool allow_upscale, int job) {
  if (!draw_list || box <= 1.0f || hair < 0) return false;

  char base[256];
  // L'id de coiffure n'est PAS le numéro de fichier, et la RACE change de table
  // comme de dossier : `job` n'est pas facultatif.
  BasePathFor(hair, sex ? 1 : 0, job, base, sizeof(base));

  char pal[128] = {0};
  if (hair_color >= 0) PalettePathFor(hair_color, job, pal, sizeof(pal));

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
