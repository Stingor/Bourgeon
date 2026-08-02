#include "ui/doll.h"

#include <cstdio>
#include <cstring>

#include "ui/head_icon.h"
#include "ui/mob_sprite.h"
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

// Chemin VFS du corps, SANS extension.
//
// 🔴 `data\sprite\`, PAS `data\` : le gabarit du client est relatif à la racine
// des SPRITES, et les deux préfixes que les couches natives ajoutent
// (« sprite\ » selon l'extension, puis « data\ ») sont à poser nous-mêmes
// puisqu'on les court-circuite.
bool BodyBasePath(const DollLook& look, char* out, size_t out_size) {
  char job[128] = {0};
  if (!JobResName(look.job, look.sex, job, sizeof(job))) return false;

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

// Chemin d'une coiffe. `view_id` est l'id d'accessoire (0 = rien).
//
// ⚠ NON IMPLÉMENTÉ pour l'instant : le nom de fichier d'un accessoire vient de
// `accessoryid.lub` / `accname.lub`, que le client charge au boot. Il faut
// passer par sa table Lua, pas par une copie figée — une liste recopiée
// diverge au premier costume ajouté. Rend false en attendant, donc les coiffes
// sont simplement absentes du pantin.
bool HeadgearBasePath(int /*view_id*/, int /*sex*/, char* /*out*/,
                      size_t /*out_size*/) {
  return false;
}

}  // namespace

bool DrawDoll(ImDrawList* draw_list, const DollLook& look, float x, float y,
              float w, float h, int dir, int anim, unsigned frame,
              uint32_t tint) {
  if (!draw_list || w <= 1.0f || h <= 1.0f) return false;

  const unsigned pose = static_cast<unsigned>(anim * 8 + (dir & 7));

  // ── Le CORPS est la référence d'ancrage : sans lui, rien à composer ────────
  char base[352], pal[128] = {0};
  if (!BodyBasePath(look, base, sizeof(base))) return false;
  if (look.clothes_color >= 0)
    BodyPalettePath(look.clothes_color, pal, sizeof(pal));

  Piece body;
  if (!LoadPiece(base, pal, &body)) return false;

  SpriteQuad quads[kMaxQuads];
  int n = SpriteResolveFrame(body.res, pose, frame, quads, kMaxQuads);
  if (n <= 0) return false;

  int body_ax = 0, body_ay = 0, body_attr = 0;
  const bool body_anchored =
      SpriteFrameAnchor(body.res, pose, frame, &body_ax, &body_ay, &body_attr);

  // ── Les pièces rapportées, dans l'ordre de dessin ─────────────────────────
  // Tête puis coiffes : bas, milieu, haut. Chacune s'accroche au CORPS (pas en
  // cascade) — c'est ce que fait le natif pour les parties 1 à 3.
  struct Attached { char base[352]; char pal[128]; };
  Attached list[kMaxPieces];
  int list_n = 0;

  if (list_n < kMaxPieces &&
      HeadSpriteBasePath(look.hair, look.sex, list[list_n].base,
                         sizeof(list[list_n].base))) {
    list[list_n].pal[0] = '\0';
    if (look.hair_color >= 0)
      HairPalettePath(look.hair_color, list[list_n].pal,
                      sizeof(list[list_n].pal));
    ++list_n;
  }
  const int gear[3] = {look.head_low, look.head_mid, look.head_top};
  for (int g = 0; g < 3 && list_n < kMaxPieces; ++g) {
    if (gear[g] <= 0) continue;
    if (!HeadgearBasePath(gear[g], look.sex, list[list_n].base,
                          sizeof(list[list_n].base)))
      continue;
    list[list_n].pal[0] = '\0';
    ++list_n;
  }

  // Dernier décalage VALIDE. 🔴 Quand une pièce n'a pas d'ancre exploitable, le
  // natif REPREND celui-ci au lieu de repartir de zéro — remettre à zéro
  // recollerait la pièce à l'origine du corps, donc visiblement de travers.
  float last_dx = 0.0f, last_dy = 0.0f;

  for (int i = 0; i < list_n && n < kMaxQuads; ++i) {
    Piece piece;
    if (!LoadPiece(list[i].base, list[i].pal, &piece)) continue;

    SpriteQuad tmp[kMaxQuads];
    const int m = SpriteResolveFrame(piece.res, pose, frame, tmp,
                                     kMaxQuads - n);
    if (m <= 0) continue;

    int ax = 0, ay = 0, attr = 0;
    // 🔴 L'attache ne s'applique que si les DEUX ancres portent le même `attr` :
    // c'est le test du natif, et sans lui on recale une pièce sur une ancre qui
    // ne la concerne pas.
    if (body_anchored && SpriteFrameAnchor(piece.res, pose, frame, &ax, &ay,
                                           &attr) &&
        attr == body_attr) {
      last_dx = static_cast<float>(body_ax - ax);
      last_dy = static_cast<float>(body_ay - ay);
    }
    for (int k = 0; k < m; ++k) {
      for (int c = 0; c < 4; ++c) {
        tmp[k].corner[c].x += last_dx;
        tmp[k].corner[c].y += last_dy;
      }
      quads[n++] = tmp[k];
    }
  }

  // ── Cadrage ───────────────────────────────────────────────────────────────
  float min_x = quads[0].corner[0].x, max_x = min_x;
  float min_y = quads[0].corner[0].y, max_y = min_y;
  for (int i = 0; i < n; ++i) {
    for (int c = 0; c < 4; ++c) {
      const ImVec2& p = quads[i].corner[c];
      if (p.x < min_x) min_x = p.x;
      if (p.x > max_x) max_x = p.x;
      if (p.y < min_y) min_y = p.y;
      if (p.y > max_y) max_y = p.y;
    }
  }
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
