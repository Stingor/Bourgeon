#include "ui/doll.h"

#include <Windows.h>  // lstrcpynA + SEH autour de la table native

#include <cstdio>
#include <cstring>

#include "ui/head_icon.h"
#include "ui/sprite_view.h"

namespace ro {
namespace {

// ── Gabarit du corps, lu DANS le binaire (client 20250716) ───────────────────
//
// `\몸통\%s\%s_%s.%s` = `\몸통\<sexe>\<job>_<sexe>.<extension>`. Un seul gabarit
// sert le .spr et le .act — l'extension est le dernier %s.
//
// Il est ASSEMBLÉ à l'exécution par `Job_BuildBodySpritePath` (0x00b43af0), qui
// lui colle devant le préfixe de race puis rend un `std::string`. 🔴 On ne
// l'appelle pas : l'interop std::string avec le natif est précisément ce qui
// plantait au char-select (edi corrompu). On concatène nous-mêmes.
constexpr uintptr_t kFmtBodyTail = 0x01088A18;

// Jetons CP949, en hexadécimal ÉCHAPPÉ — nos sources sont en UTF-8, un littéral
// coréen y serait mal encodé et ne désignerait aucun dossier du GRF.
constexpr const char kRaceHuman[] = "\xC0\xCE\xB0\xA3\xC1\xB7";  // 인간족
constexpr const char kSexMale[]   = "\xB3\xB2";                  // 남
constexpr const char kSexFemale[] = "\xBF\xA9";                  // 여

// ── Palette de vêtements ─────────────────────────────────────────────────────
//
// 🔴 `몸\body_<couleur>.pal`, PAS `몸\<job>_<sexe>_<couleur>.pal`.
//
// Le client VANILLA compose bien le nom avec le job et le sexe — c'est ce qu'on
// lit dans IDA. Mais Moonlight applique le patch WARP `BodyPalUnisex`, qui
// réécrit le gabarit en `몸\body%.s%.s_%d.pal` : `%.s` (précision nulle)
// CONSOMME l'argument sans rien imprimer, ce qui fait disparaître le job ET le
// sexe. Une seule palette sert alors tous les personnages.
//
// ⚠ L'IDB est un exe VANILLA : aucun patch WARP n'y est visible. Se fier au
// seul désassemblage donnait ici un chemin introuvable. Vérifié dans
// WARP0716/LastSession.yml, qui liste `BodyPalUnisex` et `HeadPalUnisex`.
// Voir Scripts/Patches/SharedPal.qjs pour la mécanique.
constexpr const char kBodyPalFolder[] = "\xB8\xF6";  // 몸

const char* SexToken(int sex) { return sex ? kSexMale : kSexFemale; }

// ── Nom de SPRITE du corps ───────────────────────────────────────────────────
//
// 🔴 PAS `Job_GetDisplayNameOrResName`. Celui-là rend le nom de RESSOURCE pour
// un monstre (sex = -1) mais le nom D'AFFICHAGE pour un personnage : on
// obtenait « High Wizard », « White Smith », « Novice » — des libellés d'IHM,
// avec espaces, qui ne désignent aucun fichier. Tous les pantins échouaient.
//
// Le bon chemin est celui de `Job_BuildBodySpritePath` (0x00b43af0) :
//   1. `Job_ResolveBodyClass(job, body, 1)` -> index de classe de corps ;
//   2. ce même index dans le tableau de noms lu par `sub_D81560` (0x00d81560).
//
// ⚠ C'est le BODY STYLE qui choisit le sprite ; `job` ne sert qu'à décider
// bébé / variante alternative. Passer `job` comme index donnait déjà un nom
// plausible pour les classes ordinaires, et faux dès qu'un style est posé.
//
// `sub_D81560` n'est qu'un accès à ce tableau enrobé dans un std::string — on
// lit donc le tableau, comme pour les coiffes : pas d'interop std::string.
// Bornes = g_UIWindowContextKey (0x015FA3C0) + 5277*4 et + 5278*4.
constexpr uintptr_t kJobResolveBodyClass = 0x00D99150;
constexpr uintptr_t kBodyResNamesBegin   = 0x015FF634;
constexpr uintptr_t kBodyResNamesEnd     = 0x015FF638;

using ResolveBodyClassFn = int(__stdcall*)(int job, int body, char sub3950);

bool BodyResName(int job, int body, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  bool ok = false;
  __try {
    // Le 3e argument à 1 retranche 3950 au résultat : c'est ce qui transforme
    // un id de classe 4xxx en index de tableau.
    const int cls = reinterpret_cast<ResolveBodyClassFn>(kJobResolveBodyClass)(
        job, body, 1);
    char** begin = *reinterpret_cast<char***>(kBodyResNamesBegin);
    char** end   = *reinterpret_cast<char***>(kBodyResNamesEnd);
    // 🔴 `sub_D81560` ne borne PAS son index — on le fait, sinon une classe
    // inconnue lit hors du vecteur.
    if (begin && end > begin && cls >= 0 && cls < static_cast<int>(end - begin)) {
      const char* name = begin[cls];
      if (name && *name) {
        lstrcpynA(out, name, static_cast<int>(out_size));
        ok = out[0] != '\0';
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

// Chemin VFS du corps, SANS extension.
//
// 🔴 `data\sprite\`, PAS `data\` : le gabarit du client est relatif à la racine
// des SPRITES, et les deux préfixes que les couches natives ajoutent
// (« sprite\ » selon l'extension, puis « data\ ») sont à poser nous-mêmes
// puisqu'on les court-circuite.
bool BodyBasePath(const DollLook& look, char* out, size_t out_size) {
  char job[128] = {0};
  if (!BodyResName(look.job, look.body, job, sizeof(job))) return false;

  const char* sex = SexToken(look.sex);
  char tail[256];
  // ⚠ `std::snprintf` : le gabarit n'est pas un littéral (il est lu dans le
  // binaire), et la famille sécurisée déclencherait C4774.
  std::snprintf(tail, sizeof(tail),
                reinterpret_cast<const char*>(kFmtBodyTail), sex, job, sex,
                "spr");
  const size_t n = std::strlen(tail);
  if (n > 4) tail[n - 4] = '\0';  // retire « .spr » : sprite_view veut une base

  std::snprintf(out, out_size, "data\\sprite\\%s%s", kRaceHuman, tail);
  return true;
}

void BodyPalettePath(int color, char* out, size_t out_size) {
  std::snprintf(out, out_size, "data\\palette\\%s\\body_%d.pal", kBodyPalFolder,
                color);
}

// ── Une pièce du pantin ──────────────────────────────────────────────────────
constexpr int kMaxQuads  = 96;  // toutes pièces confondues
constexpr int kMaxPieces = 8;   // corps + tête + 3 coiffes, avec de la marge

struct Piece {
  SpriteRes res;
  bool      loaded = false;
};

// Charge une pièce. Une pièce absente n'est pas une erreur : elle est omise.
bool LoadPiece(const char* base, const char* pal, Piece* p) {
  if (!base || !*base) return false;
  p->loaded = LoadSpriteRecolored(base, (pal && *pal) ? pal : nullptr, &p->res);
  return p->loaded;
}

// ── Nom de fichier d'un accessoire ───────────────────────────────────────────
//
// Table peuplée au boot depuis `accessoryid.lub` / `accname.lub`. On la LIT
// directement plutôt que d'appeler `Job_GetHeadgearResName` (0x00d81480), qui
// n'est qu'un accès à ce même tableau enrobé dans un `std::string` — et
// l'interop std::string avec le natif est ce qui plantait au char-select.
//
// La fonction fait `*(this + 5286*4)` / `*(this + 5287*4)` sur
// g_UIWindowContextKey (0x015FA3C0), soit un `std::vector<char*>` dont les
// bornes sont ci-dessous ; l'entrée est un `char*` CP949.
//
// 🔴 Rester sur la table NATIVE et ne jamais recopier la liste : elle vient des
// .lub, qu'un patcheur peut remplacer. Une copie figée divergerait au premier
// costume ajouté.
constexpr uintptr_t kHeadgearNamesBegin = 0x015FF658;
constexpr uintptr_t kHeadgearNamesEnd   = 0x015FF65C;

bool HeadgearResName(int view_id, char* out, size_t out_size) {
  if (view_id <= 0 || !out || out_size == 0) return false;
  out[0] = '\0';
  bool ok = false;
  __try {
    char** begin = *reinterpret_cast<char***>(kHeadgearNamesBegin);
    char** end   = *reinterpret_cast<char***>(kHeadgearNamesEnd);
    if (begin && end > begin && view_id < static_cast<int>(end - begin)) {
      const char* name = begin[view_id];
      if (name && *name) {
        lstrcpynA(out, name, static_cast<int>(out_size));
        ok = out[0] != '\0';
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

// Gabarit du client : `악세사리\%s\%s%s.%s` = `악세사리\<sexe>\<sexe><nom>.<ext>`.
// ⚠ Pas de séparateur entre le sexe et le nom : le nom de la table porte DÉJÀ
// son « _ » initial (`_비틀눈` -> `여_비틀눈.spr`).
constexpr uintptr_t kFmtHeadgear = 0x01088A9C;

// Chemin VFS d'une coiffe, SANS extension. `view_id` = id d'accessoire.
//
// `lower` demande la variante en MINUSCULES du nom d'accessoire. La table donne
// « _C_Avenger » mais le GRF contient « 남_c_avenger.spr » : la casse ne suit
// aucune règle d'un costume à l'autre. L'implémentation de référence (le site
// Moonlight) fait la même chose — nom tel quel, puis strtolower.
//
// ⚠ On n'abaisse QUE le nom d'accessoire, qui est de l'ASCII. Abaisser le
// chemin entier corromprait les octets CP949 : le SECOND octet d'un caractère
// coréen peut tomber dans la plage 'A'-'Z', et le passer en minuscule change le
// caractère.
bool HeadgearBasePath(int view_id, int sex, bool lower, char* out,
                      size_t out_size) {
  char name[128];
  if (!HeadgearResName(view_id, name, sizeof(name))) return false;
  if (lower) {
    for (char* p = name; *p; ++p)
      if (*p >= 'A' && *p <= 'Z') *p = static_cast<char>(*p - 'A' + 'a');
  }

  const char* sex_tok = SexToken(sex);
  char tail[256];
  std::snprintf(tail, sizeof(tail),
                reinterpret_cast<const char*>(kFmtHeadgear), sex_tok, sex_tok,
                name, "spr");
  const size_t n = std::strlen(tail);
  if (n > 4) tail[n - 4] = '\0';  // retire « .spr »

  // 🔴 PAS de préfixe de race ici. Le corps vit sous `인간족\몸통\…`, mais les
  // accessoires sont directement sous `sprite\악세사리\…` — vérifié dans le GRF.
  // Le gabarit du client porte déjà tout ce qu'il faut ; lui ajouter `인간족\`
  // rendait toutes les coiffes introuvables.
  std::snprintf(out, out_size, "data\\sprite\\%s", tail);
  return true;
}

}  // namespace

namespace {

// Action réellement jouable par une pièce : la pose demandée, REPLIÉE sur le
// nombre d'actions du fichier. Une coiffe en a souvent moins que le corps, et
// il faut la replier plutôt que l'omettre — `$action %= $action_count` du site.
unsigned PieceAction(const SpriteRes& res, unsigned pose) {
  const int n = SpriteActionCount(res);
  return (n > 0) ? (pose % static_cast<unsigned>(n)) : 0u;
}

}  // namespace

bool DrawDoll(ImDrawList* draw_list, const DollLook& look, float x, float y,
              float w, float h, int dir, int anim, float anim_seconds,
              uint32_t tint) {
  if (!draw_list || w <= 1.0f || h <= 1.0f) return false;

  const unsigned pose = static_cast<unsigned>(anim * 8 + (dir & 7));

  // ⚠ L'image de l'ACTEUR ne bouge qu'en Marche (1) et Combat (4) — vérifié au
  // débogueur : sur un personnage assis, `acteur+0x3c` reste à 0. Le corps et
  // la tête sont donc figés ici.
  //
  // 🔴 Les ACCESSOIRES, eux, s'animent quand même : ils ont leur propre
  // horloge. Voir AltAnimFrame ci-dessous.
  const unsigned actor_frame = 0;

  // ── Le CORPS est la référence d'ancrage : sans lui, rien à composer ────────
  char base[352], pal[128] = {0};
  if (!BodyBasePath(look, base, sizeof(base))) return false;
  if (look.clothes_color >= 0)
    BodyPalettePath(look.clothes_color, pal, sizeof(pal));

  Piece body;
  if (!LoadPiece(base, pal, &body)) return false;

  const unsigned body_pose = PieceAction(body.res, pose);

  // 🔴 Le CORPS et la TÊTE restent sur l'IMAGE 0. Leurs images sont des poses et
  // des expressions de visage : les faire défiler fait tourner la tête des
  // convives. Seuls les accessoires font défiler la leur.
  SpriteQuad quads[kMaxQuads];
  int n = SpriteResolveFrame(body.res, body_pose, 0, quads, kMaxQuads,
                             /*apply_rotation=*/false);
  if (n <= 0) return false;

  // Boîte englobante de CADRAGE, accumulée sur l'image 0 de chaque pièce.
  //
  // 🔴 Elle ne doit PAS être calculée sur les quads dessinés : une coiffe
  // animée changerait la boîte à chaque image, donc l'échelle et le centrage —
  // et tout le pantin se mettrait à bouger. C'est exactement le défaut connu
  // des « hats animés qui font bouger les dolls ».
  float min_x = quads[0].corner[0].x, max_x = min_x;
  float min_y = quads[0].corner[0].y, max_y = min_y;
  auto grow = [&](const SpriteQuad* q, int count, float dx, float dy) {
    for (int i = 0; i < count; ++i) {
      for (int c = 0; c < 4; ++c) {
        const float px = q[i].corner[c].x + dx, py = q[i].corner[c].y + dy;
        if (px < min_x) min_x = px;
        if (px > max_x) max_x = px;
        if (py < min_y) min_y = py;
        if (py > max_y) max_y = py;
      }
    }
  };
  grow(quads, n, 0.0f, 0.0f);

  int body_ax = 0, body_ay = 0, body_attr = 0;
  const bool body_anchored =
      SpriteFrameAnchor(body.res, body_pose, 0, &body_ax, &body_ay, &body_attr);

  // ── Les pièces rapportées, dans l'ordre de dessin ─────────────────────────
  // Tête puis coiffes : bas, milieu, haut. Chacune s'accroche au CORPS (pas en
  // cascade) — c'est ce que fait le natif pour les parties 1 à 3.
  // `alt` = chemin de SECOURS, essayé si le premier ne donne rien (casse du nom
  // d'accessoire). Vide = pas de secours.
  //
  // `accessory` : distingue une COIFFE de la TÊTE. Seules les coiffes ont une
  // animation alternative ; la tête, elle, sert de RÉFÉRENCE à leur calcul
  // (cf. AltAnimFrame plus bas), d'où sa place en tête de liste.
  struct Attached {
    char base[352];
    char alt[352];
    char pal[128];
    bool accessory;
  };
  Attached list[kMaxPieces];
  int list_n = 0;

  if (list_n < kMaxPieces &&
      HeadSpriteBasePath(look.hair, look.sex, list[list_n].base,
                         sizeof(list[list_n].base))) {
    list[list_n].alt[0] = '\0';
    list[list_n].pal[0] = '\0';
    list[list_n].accessory = false;  // la TÊTE : référence, jamais animée
    if (look.hair_color >= 0)
      HairPalettePath(look.hair_color, list[list_n].pal,
                      sizeof(list[list_n].pal));
    ++list_n;
  }
  const int gear[3] = {look.head_low, look.head_mid, look.head_top};
  for (int g = 0; g < 3 && list_n < kMaxPieces; ++g) {
    if (gear[g] <= 0) continue;
    if (!HeadgearBasePath(gear[g], look.sex, /*lower=*/false, list[list_n].base,
                          sizeof(list[list_n].base)))
      continue;
    HeadgearBasePath(gear[g], look.sex, /*lower=*/true, list[list_n].alt,
                     sizeof(list[list_n].alt));
    list[list_n].pal[0] = '\0';
    list[list_n].accessory = true;
    ++list_n;
  }

  // Dernier décalage VALIDE. 🔴 Quand une pièce n'a pas d'ancre exploitable, le
  // natif REPREND celui-ci au lieu de repartir de zéro — remettre à zéro
  // recollerait la pièce à l'origine du corps, donc visiblement de travers.
  float last_dx = 0.0f, last_dy = 0.0f;

  // Nombre d'images de la TÊTE pour cette action : c'est la référence de
  // l'animation alternative des accessoires (cf. Act_ResolveAltAnimFrame).
  int ref_frames = 0;

  for (int i = 0; i < list_n && n < kMaxQuads; ++i) {
    Piece piece;
    if (!LoadPiece(list[i].base, list[i].pal, &piece) &&
        !LoadPiece(list[i].alt, list[i].pal, &piece))
      continue;

    // 🔴 TOUTES les pièces partagent l'action du corps. C'est ce que fait le
    // natif : `CActorSprite_RenderCompositeJobSprite` lit une seule action
    // (this+14) et une seule image (this+15) pour ses huit parties.
    //
    // Leur faire jouer le mouvement de repos à la place semblait plus logique
    // — une décoration vit pour elle-même — mais c'est faux : l'action de repos
    // d'un costume peut être tout autre chose que son animation en pose assise.
    const unsigned piece_pose = PieceAction(piece.res, pose);

    // ── Image de la pièce ────────────────────────────────────────────────────
    //
    // 🔴 Port de `Act_ResolveAltAnimFrame` (0x00d83a40). C'est LUI qui anime les
    // costumes, et c'est pour ça que le pantin de la fiche de personnage
    // « anime parfaitement » : il fait rendre un vrai acteur par le client, donc
    // le client appelle cette fonction pour nous. En composant nous-mêmes, il
    // faut la refaire.
    //
    //     nRef   = images de la TÊTE pour cette action
    //     nPiece = images de la PIÈCE pour cette action
    //     si nRef >= nPiece ou nPiece % nRef  ->  pas d'animation alternative
    //     mult   = nPiece / nRef
    //     image  = image_acteur * mult + ((écoulé / 24) / delay) % mult
    //
    // Une pièce porte donc un MULTIPLE exact des images de la tête, et parcourt
    // son SOUS-GROUPE au fil du temps réel — pendant que l'acteur, lui, reste
    // sur son image. D'où des coiffes qui vivent sur un personnage assis.
    //
    // ⚠ Le diviseur est 24, pas 25, et `delay` est la cadence BRUTE du .act (pas
    // des millisecondes) : `c_angry_fish` dit 4.0 -> 96 ms par sous-image,
    // `c_avenger` 8.0 -> 192 ms. Avec 24 images pour 3 de tête, mult = 8 : ils
    // jouent 0..7, jamais les 24 — ce qui explique qu'ils ne « tournent » pas.
    unsigned pf = actor_frame;
    if (list[i].accessory && anim_seconds >= 0.0f && ref_frames > 0) {
      const int n_piece = SpriteActionFrameCount(piece.res, piece_pose);
      if (n_piece > ref_frames && (n_piece % ref_frames) == 0) {
        const int mult = n_piece / ref_frames;
        float delay = SpriteFrameIntervalMs(piece.res, piece_pose) / 25.0f;
        if (delay <= 0.0f) delay = 4.0f;  // le natif retombe sur 4
        const float ticks = (anim_seconds * 1000.0f / 24.0f) / delay;
        int sub = static_cast<int>(ticks) % mult;
        if (sub < 0) sub += mult;
        pf = actor_frame * static_cast<unsigned>(mult) +
             static_cast<unsigned>(sub);
      }
    }
    // La TÊTE sert de référence aux pièces suivantes — elle est en tête de
    // liste, donc son compte est connu quand les coiffes arrivent.
    if (!list[i].accessory)
      ref_frames = SpriteActionFrameCount(piece.res, piece_pose);

    // 🔴 L'ancre est TOUJOURS celle de l'image 0, même pour une pièce animée.
    // Seule son IMAGE défile, jamais son point d'attache : prendre l'ancre de
    // l'image dessinée faisait dériver la pièce d'une image à l'autre — un
    // accessoire sans animation, dont les trois images sont pourtant
    // identiques, se mettait à bouger.
    //
    // 🔴 L'attache ne s'applique que si les DEUX ancres portent le même `attr` :
    // c'est le test du natif, et sans lui on recale une pièce sur une ancre qui
    // ne la concerne pas.
    int ax = 0, ay = 0, attr = 0;
    if (body_anchored &&
        SpriteFrameAnchor(piece.res, piece_pose, 0, &ax, &ay, &attr) &&
        attr == body_attr) {
      last_dx = static_cast<float>(body_ax - ax);
      last_dy = static_cast<float>(body_ay - ay);
    }

    // Cadrage : image 0 de la pièce, à son ancre. Rien de ce qui varie d'une
    // image à l'autre n'entre dans la boîte englobante.
    SpriteQuad ref[kMaxQuads];
    const int rn = SpriteResolveFrame(piece.res, piece_pose, 0, ref, kMaxQuads,
                                       /*apply_rotation=*/false);
    if (rn > 0) grow(ref, rn, last_dx, last_dy);

    SpriteQuad tmp[kMaxQuads];
    const int m =
        SpriteResolveFrame(piece.res, piece_pose, pf, tmp, kMaxQuads - n,
                           /*apply_rotation=*/false);
    if (m <= 0) continue;
    for (int k = 0; k < m; ++k) {
      for (int c = 0; c < 4; ++c) {
        tmp[k].corner[c].x += last_dx;
        tmp[k].corner[c].y += last_dy;
      }
      quads[n++] = tmp[k];
    }
  }

  // ── Cadrage ───────────────────────────────────────────────────────────────
  const float span_x = max_x - min_x, span_y = max_y - min_y;
  if (span_x <= 0.0f || span_y <= 0.0f) return false;

  float scale = h / span_y;
  const float fit_x = w / span_x;
  if (fit_x < scale) scale = fit_x;
  if (scale <= 0.0f) return false;

  // Corps centré en X, PIEDS en bas : c'est le bas de la boîte englobante qui
  // vient se poser sur le bas du rectangle, pas son centre — sinon un
  // personnage coiffé d'un grand chapeau s'enfoncerait dans le sol.
  const float origin_x = x + w * 0.5f - (min_x + max_x) * 0.5f * scale;
  const float origin_y = y + h - max_y * scale;

  bool drawn = false;
  for (int i = 0; i < n; ++i) {
    if (!quads[i].tex) continue;
    // Teinte de l'appelant PAR-DESSUS celle du calque, canal par canal.
    const ImU32 t = quads[i].tint;
    const ImU32 col = IM_COL32(
        ((t >> IM_COL32_R_SHIFT) & 0xFF) * ((tint >> IM_COL32_R_SHIFT) & 0xFF) / 255,
        ((t >> IM_COL32_G_SHIFT) & 0xFF) * ((tint >> IM_COL32_G_SHIFT) & 0xFF) / 255,
        ((t >> IM_COL32_B_SHIFT) & 0xFF) * ((tint >> IM_COL32_B_SHIFT) & 0xFF) / 255,
        ((t >> IM_COL32_A_SHIFT) & 0xFF) * ((tint >> IM_COL32_A_SHIFT) & 0xFF) / 255);
    ImVec2 p[4];
    for (int c = 0; c < 4; ++c)
      p[c] = ImVec2(origin_x + quads[i].corner[c].x * scale,
                    origin_y + quads[i].corner[c].y * scale);
    const ImVec2& uv0 = quads[i].uv0;
    const ImVec2& uv1 = quads[i].uv1;
    draw_list->AddImageQuad(reinterpret_cast<ImTextureID>(quads[i].tex), p[0],
                            p[1], p[2], p[3], ImVec2(uv0.x, uv0.y),
                            ImVec2(uv1.x, uv0.y), ImVec2(uv1.x, uv1.y),
                            ImVec2(uv0.x, uv1.y), col);
    drawn = true;
  }
  return drawn;
}

}  // namespace ro
