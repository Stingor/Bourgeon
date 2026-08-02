#include "ui/doll.h"

#include <Windows.h>  // lstrcpynA + SEH autour de la table native

#include <cstdio>
#include <cstring>

#include "ragnarok/lua.h"  // la cape interroge deux globaux Lua
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
constexpr int kMaxPieces = 8;   // tête + 3 coiffes + cape, avec de la marge

struct Piece {
  SpriteRes res;
  bool      loaded = false;
};

// Charge une pièce. Une pièce absente n'est pas une erreur : elle est omise.
// `act` vide = le .act partage la base du .spr ; seule la cape en diffère.
bool LoadPiece(const char* base, const char* act, const char* pal, Piece* p) {
  if (!base || !*base) return false;
  p->loaded = LoadSpritePair(base, (act && *act) ? act : nullptr,
                             (pal && *pal) ? pal : nullptr, &p->res);
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

// ── La CAPE (garment) ────────────────────────────────────────────────────────
//
// Tout vient de `Job_BuildGarmentSpritePath_impl` (0x00b442f0) et de
// `CActorSprite_DrawGarmentLayer` (0x00d36430).
//
// ⚠ Ces deux gabarits-ci portent DÉJÀ leur `sprite\` — contrairement à ceux du
// corps et des accessoires. Le préfixe à poser n'est donc que `data\`.
constexpr uintptr_t kFmtGarmentFlat = 0x01088B88;  // sprite\로브\%s\%s.%s
constexpr uintptr_t kFmtGarmentJob  = 0x01088BBC;  // sprite\로브\%s\%s\%s_%s.%s

// Drapeau du client : choisit `_New_DrawOnTop` plutôt que `DrawOnTop`.
constexpr uintptr_t kGarmentUseNewDrawOnTop = 0x015FB2A4;

// Nom de sprite d'une cape.
//
// 🔴 Il ne sort d'AUCUNE table C. Le constructeur de chemin appelle le global
// Lua `ReqRobSprName(viewId)` (cf. `Lua_GetReqRobSprName_ByRobeId` 0x00b44a10).
// Il existe bien un vecteur natif de noms voisin de ceux du corps et des
// coiffes, à 0x015FF664 — mais c'est celui des BOUCLIERS, pas des capes.
bool GarmentResName(int view_id, char* out, size_t out_size) {
  if (view_id <= 0 || !out || out_size == 0) return false;
  out[0] = '\0';
  void* L = lua::State();
  if (!L) return false;
  bool ok = false;
  __try {
    // 🔴 `lua_checkstack` AVANT le moindre push. Sans lui, une pile déjà pleine
    // — ce qui arrive en cours de partie — avale les arguments en silence et
    // l'appel rend n'importe quoi, sans erreur.
    if (lua::CheckStack(L, 4)) {
      lua::GetField(L, lua::kGlobalsIndex, "ReqRobSprName");
      lua::PushNumber(L, static_cast<double>(view_id));
      if (lua::PCall(L, 1, 1, 0) == 0) {
        size_t len = 0;
        const char* s = lua::ToLString(L, -1, &len);
        if (s && *s) {
          lstrcpynA(out, s, static_cast<int>(out_size));
          ok = out[0] != '\0';
        }
      }
      // Succès comme échec laissent UNE valeur au sommet (le résultat, ou le
      // message d'erreur) : un seul dépilement dans les deux cas.
      lua::Pop(L, 1);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

// La cape passe-t-elle DEVANT le personnage ?
//
// 🔴 La réponse n'est pas dans le binaire : `Actor_GetGarmentDrawOnTopPass`
// (0x00d36d60) la demande au Lua, qui la fait dépendre de l'orientation. C'est
// pour ça qu'une cape disparaît derrière son porteur quand il se retourne.
//
// Le natif initialise sa sortie à 1 AVANT l'appel : Lua muet = devant. On garde
// ce défaut, qui est aussi le moins mauvais (une cape devant se voit ; une cape
// derrière un dos qui ne l'attend pas est invisible).
//
// La donnée derrière ce Lua est la table `RobeTopLayer` de
// `System\spriterobename.lub`, à côté de `RobeNameTable` que sert
// `ReqRobSprName` — d'où l'intérêt d'appeler la fonction du client plutôt que de
// recopier une règle : la liste bouge à chaque costume ajouté.
bool GarmentDrawsOnTop(int view_id, int sex, int job, unsigned pose,
                       unsigned frame) {
  bool on_top = true;
  void* L = lua::State();
  if (!L) return on_top;
  __try {
    // Lu en OCTET : le type exact de la globale n'est pas établi, et son octet
    // de poids faible suffit à trancher quel que soit sa largeur.
    const bool use_new =
        *reinterpret_cast<const uint8_t*>(kGarmentUseNewDrawOnTop) != 0;
    if (lua::CheckStack(L, 8)) {
      lua::GetField(L, lua::kGlobalsIndex,
                    use_new ? "_New_DrawOnTop" : "DrawOnTop");
      // Mêmes arguments et même ordre que le natif : id de cape, sexe, job,
      // action, image.
      lua::PushNumber(L, static_cast<double>(view_id));
      lua::PushNumber(L, static_cast<double>(sex));
      lua::PushNumber(L, static_cast<double>(job));
      lua::PushNumber(L, static_cast<double>(pose));
      lua::PushNumber(L, static_cast<double>(frame));
      // `_New_DrawOnTop` rend une valeur de plus ; on n'en demande qu'une, Lua
      // jette le reste.
      if (lua::PCall(L, 5, 1, 0) == 0) on_top = lua::ToBoolean(L, -1) != 0;
      lua::Pop(L, 1);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return on_top;
}

// ── Mémo des deux réponses Lua ───────────────────────────────────────────────
//
// `DrawDoll` est appelée à chaque image ET pour chaque personnage : au
// char-select, neuf convives à soixante images par seconde feraient plus de
// mille appels Lua par seconde pour redemander des réponses invariantes.
//
// Les deux ne dépendent que de leurs arguments, donc un mémo suffit — et c'est
// déjà ce que fait basic_info pour les paramètres de hat effect. Table courte à
// écrasement circulaire : on ne cherche pas à tout retenir, juste à couvrir les
// quelques capes visibles en même temps.
constexpr int kGarmentNameMemo = 16;
// Le devant/derrière dépend AUSSI de l'orientation et de l'image (le natif les
// passe au Lua) : 8 orientations × 8 images = 64 combinaisons pour une seule
// cape. Une table plus courte se ferait écraser en boucle pendant la marche et
// ne mémoriserait plus rien.
constexpr int kGarmentPassMemo = 64;

bool GarmentResNameCached(int view_id, char* out, size_t out_size) {
  struct Slot { int id; bool ok; char name[128]; };
  static Slot memo[kGarmentNameMemo] = {};
  static int  next = 0;
  for (const Slot& m : memo) {
    // `id` vaut 0 dans un emplacement vierge, et l'appelant garantit view_id > 0 :
    // pas de fausse correspondance possible.
    if (m.id != view_id) continue;
    if (m.ok) lstrcpynA(out, m.name, static_cast<int>(out_size));
    return m.ok;
  }
  Slot& s = memo[next];
  next = (next + 1) % kGarmentNameMemo;
  s.id = view_id;
  s.ok = GarmentResName(view_id, s.name, sizeof(s.name));
  if (s.ok) lstrcpynA(out, s.name, static_cast<int>(out_size));
  return s.ok;
}

bool GarmentDrawsOnTopCached(int view_id, int sex, int job, unsigned pose,
                             unsigned frame) {
  struct Slot { bool used; int id, sex, job; unsigned pose, frame; bool on_top; };
  static Slot memo[kGarmentPassMemo] = {};
  static int  next = 0;
  for (const Slot& m : memo) {
    if (m.used && m.id == view_id && m.sex == sex && m.job == job &&
        m.pose == pose && m.frame == frame)
      return m.on_top;
  }
  Slot& s = memo[next];
  next = (next + 1) % kGarmentPassMemo;
  s.used = true;
  s.id = view_id; s.sex = sex; s.job = job; s.pose = pose; s.frame = frame;
  s.on_top = GarmentDrawsOnTop(view_id, sex, job, pose, frame);
  return s.on_top;
}

// Chemin VFS de la cape, SANS extension. `job_layout` demande la disposition
// PAR CLASSE ; sinon la disposition « plate ».
//
// 🔴 Les deux ne sont pas interchangeables selon l'extension : le client essaie
// la plate pour le `.spr` et retombe sur celle par classe si le fichier
// n'existe pas, mais prend TOUJOURS celle par classe pour le `.act`.
bool GarmentBasePath(const DollLook& look, const char* robe, bool job_layout,
                     char* out, size_t out_size) {
  if (!robe || !*robe) return false;
  char tail[352];
  if (job_layout) {
    char job[128] = {0};
    if (!BodyResName(look.job, look.body, job, sizeof(job))) return false;
    const char* sex = SexToken(look.sex);
    std::snprintf(tail, sizeof(tail),
                  reinterpret_cast<const char*>(kFmtGarmentJob), robe, sex, job,
                  sex, "spr");
  } else {
    std::snprintf(tail, sizeof(tail),
                  reinterpret_cast<const char*>(kFmtGarmentFlat), robe, robe,
                  "spr");
  }
  const size_t n = std::strlen(tail);
  if (n > 4) tail[n - 4] = '\0';  // retire « .spr »
  std::snprintf(out, out_size, "data\\%s", tail);
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
  DollDrawOpts opts;
  opts.dir = dir;
  opts.anim = anim;
  opts.anim_seconds = anim_seconds;
  opts.tint = tint;
  return DrawDoll(draw_list, look, x, y, w, h, opts);
}

bool DrawDoll(ImDrawList* draw_list, const DollLook& look, float x, float y,
              float w, float h, const DollDrawOpts& opts) {
  if (!draw_list || w <= 1.0f || h <= 1.0f) return false;

  const int      anim         = opts.anim;
  const float    anim_seconds = opts.anim_seconds;
  const uint32_t tint         = opts.tint;
  const unsigned pose = static_cast<unsigned>(anim * 8 + (opts.dir & 7));

  // ── Le CORPS est la référence d'ancrage : sans lui, rien à composer ────────
  char base[352], pal[128] = {0};
  if (!BodyBasePath(look, base, sizeof(base))) return false;
  if (look.clothes_color >= 0)
    BodyPalettePath(look.clothes_color, pal, sizeof(pal));

  Piece body;
  if (!LoadPiece(base, nullptr, pal, &body)) return false;

  const unsigned body_pose = PieceAction(body.res, pose);

  // ⚠ L'image de l'ACTEUR ne défile qu'en MARCHE (1) et en COMBAT (4). Mesuré
  // au débogueur — sur un personnage assis, `acteur+0x3c` reste à 0 — et
  // confirmé en dur dans le client : `CActorSprite_DrawGarmentLayer` force
  // `frame = 0` pour les types d'action 0 et 2 juste avant de dessiner.
  //
  // Faire défiler les autres ferait tourner la tête des convives : leurs images
  // sont des poses et des expressions de visage, pas une décoration.
  //
  // 🔴 Les ACCESSOIRES, eux, s'animent quand même, sur leur propre horloge.
  // Voir AltAnimFrame plus bas — et noter que `actor_frame` ENTRE dans ce
  // calcul (`image = actor_frame * mult + sous-image`), donc les deux horloges
  // se composent au lieu de s'exclure.
  unsigned actor_frame = 0;
  if (anim_seconds >= 0.0f && (anim == 1 || anim == 4))
    actor_frame = SpriteFrameIndex(body.res, body_pose, anim_seconds);

  // 🔴 DEUX résolutions du corps, exactement comme pour les pièces rapportées :
  // l'image 0 sert à MESURER, l'image courante à DESSINER. N'en faire qu'une
  // remettrait la boîte englobante sur les quads dessinés — et tout le pantin
  // se mettrait à grossir et rétrécir au rythme de la marche, le défaut connu
  // des « hats animés qui font bouger les dolls ».
  SpriteQuad body_ref[kMaxQuads];
  const int body_rn = SpriteResolveFrame(body.res, body_pose, 0, body_ref,
                                         kMaxQuads, /*apply_rotation=*/false);
  if (body_rn <= 0) return false;

  SpriteQuad quads[kMaxQuads];
  int n = SpriteResolveFrame(body.res, body_pose, actor_frame, quads, kMaxQuads,
                             /*apply_rotation=*/false);
  if (n <= 0) return false;

  // Boîte englobante de CADRAGE, accumulée sur l'image 0 de chaque pièce.
  //
  // 🔴 Elle ne doit PAS être calculée sur les quads dessinés : une coiffe
  // animée changerait la boîte à chaque image, donc l'échelle et le centrage —
  // et tout le pantin se mettrait à bouger. C'est exactement le défaut connu
  // des « hats animés qui font bouger les dolls ».
  float min_x = body_ref[0].corner[0].x, max_x = min_x;
  float min_y = body_ref[0].corner[0].y, max_y = min_y;
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
  grow(body_ref, body_rn, 0.0f, 0.0f);

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
  //
  // `act` : base du .act quand elle diffère de celle du .spr — la CAPE seule.
  // `behind` : la cape, et elle seule, peut passer DERRIÈRE le corps.
  struct Attached {
    char base[352];
    char alt[352];
    char act[352];
    char pal[128];
    bool accessory;
    bool behind;
    // S'accroche à la TÊTE plutôt qu'au corps. Vrai pour les seules coiffes :
    // `Actor_DrawSprites` (0x007ac820) donne aux parties 3 à 6 la partie 2 —
    // la tête — pour référence d'ancrage. La cape, elle, reste sur le corps.
    bool on_head;
  };
  Attached list[kMaxPieces] = {};
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
    list[list_n].on_head   = true;
    ++list_n;
  }

  // ── La cape, en dernier ───────────────────────────────────────────────────
  //
  // Deux refus repris tels quels de `CActorSprite_DrawGarmentLayer` : le type
  // d'action 8 (poses 64 à 71) n'a jamais de cape, et trois classes n'en ont pas
  // au FÉMININ.
  //
  // Elle est marquée `accessory` : le natif lui applique la même animation
  // alternative qu'aux coiffes, avec la même référence — le slot 1, c'est-à-dire
  // la TÊTE (`CActorSprite_BuildHead_Slot1`). Sa place après la tête dans la
  // liste suffit donc à ce que le calcul tombe juste.
  const bool garment_job_ok =
      look.sex != 0 ||
      (look.job != 4086 && look.job != 4087 && look.job != 4112);
  if (look.garment > 0 && list_n < kMaxPieces && garment_job_ok &&
      (pose < 64 || pose > 71)) {
    char robe[128];
    Attached& g = list[list_n];
    // Le .spr peut venir de la disposition plate, le .act JAMAIS : il est
    // toujours par classe. `base` = plate, `alt` = repli par classe — le même
    // couple base/alt que la casse des accessoires, réutilisé tel quel.
    if (GarmentResNameCached(look.garment, robe, sizeof(robe)) &&
        GarmentBasePath(look, robe, /*job_layout=*/true, g.alt, sizeof(g.alt))) {
      if (!GarmentBasePath(look, robe, /*job_layout=*/false, g.base,
                           sizeof(g.base)))
        g.base[0] = '\0';
      lstrcpynA(g.act, g.alt, static_cast<int>(sizeof(g.act)));
      g.pal[0] = '\0';
      g.accessory = true;
      g.behind = !GarmentDrawsOnTopCached(look.garment, look.sex, look.job,
                                          pose, actor_frame);
      ++list_n;
    }
  }

  // Dernier décalage VALIDE. 🔴 Quand une pièce n'a pas d'ancre exploitable, le
  // natif REPREND celui-ci au lieu de repartir de zéro — remettre à zéro
  // recollerait la pièce à l'origine du corps, donc visiblement de travers.
  float last_dx = 0.0f, last_dy = 0.0f;

  // Nombre d'images de la TÊTE pour cette action : c'est la référence de
  // l'animation alternative des accessoires (cf. Act_ResolveAltAnimFrame).
  int ref_frames = 0;

  // ── La TÊTE, référence d'ANCRAGE des coiffes ──────────────────────────────
  //
  // 🔴 Une coiffe ne s'accroche PAS au corps. `Actor_DrawSprites` (0x007ac820)
  // donne aux parties 3 à 6 la partie 2 — la tête — pour référence, et compare
  // les ancres des IMAGES COURANTES des deux (`head.ancre − pièce.ancre`).
  //
  // C'est là qu'était le défaut « les coiffes ne suivent pas le balancement de
  // la marche » : accrochées au corps à l'image 0, elles gardaient un décalage
  // CONSTANT pendant que la tête oscillait. Ce n'était ni un repliement d'action
  // ni un plafonnement d'image — mesuré, les deux étaient hors de cause.
  //
  // On garde en plus le décalage de la TÊTE elle-même, faute de quoi les coiffes
  // atterriraient à l'origine du corps. Au repos les deux images valent 0 et le
  // calcul retombe exactement sur l'ancien : rien ne bouge hors marche.
  float head_dx = 0.0f, head_dy = 0.0f;
  int   head_ax = 0, head_ay = 0, head_attr = 0;
  bool  head_ok = false;

  // Calques à poser DERRIÈRE le corps. Seule la cape peut y atterrir, et
  // seulement dans certaines orientations — d'où un second tampon plutôt qu'un
  // tri : l'ordre à l'intérieur de chaque passe reste celui de la liste.
  SpriteQuad back[kMaxQuads];
  int back_n = 0;

  for (int i = 0; i < list_n; ++i) {
    SpriteQuad* dst = list[i].behind ? back : quads;
    int& dst_n      = list[i].behind ? back_n : n;
    if (dst_n >= kMaxQuads) continue;

    Piece piece;
    if (!LoadPiece(list[i].base, list[i].act, list[i].pal, &piece) &&
        !LoadPiece(list[i].alt, list[i].act, list[i].pal, &piece))
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
      if (n_piece > ref_frames && (n_piece % ref_frames) == 0) {  // multiple exact
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
    // ⚠ Plafonnement sur ce que la pièce contient VRAIMENT. Tant que le corps
    // restait figé sur l'image 0 la question ne se posait pas ; dès qu'il
    // défile, une tête ou une coiffe qui a moins d'images que lui se verrait
    // demander une image inexistante. L'implémentation de référence (le site
    // Moonlight) fait la même chose : `min($animation, $count - 1)`.
    const int piece_frames = SpriteActionFrameCount(piece.res, piece_pose);
    if (piece_frames > 0 && pf >= static_cast<unsigned>(piece_frames))
      pf = static_cast<unsigned>(piece_frames - 1);



    // ── Accrochage ────────────────────────────────────────────────────────
    //
    // Deux régimes, et ce n'est pas une commodité : c'est ce que fait
    // `Actor_DrawSprites` (0x007ac820).
    //
    //   COIFFE  -> référence = la TÊTE, ancres des images COURANTES des deux.
    //   le RESTE -> référence = le CORPS, ancres de l'image 0.
    //
    // ⚠ Ne pas généraliser l'un ou l'autre. Tout accrocher au corps à l'image 0
    // fige les coiffes pendant la marche ; tout passer à l'image courante fait
    // dériver une pièce dont l'ancre bouge sans que sa référence bouge.
    //
    // 🔴 L'attache ne s'applique que si les DEUX ancres portent le même `attr` :
    // c'est le test du natif, et sans lui on recale une pièce sur une ancre qui
    // ne la concerne pas.
    int ax = 0, ay = 0, attr = 0;
    if (list[i].on_head) {
      // Coiffe : ancre de la TÊTE moins la sienne, toutes deux sur l'image
      // qu'elles DESSINENT — c'est ce mouvement-là qui porte le balancement.
      if (head_ok &&
          SpriteFrameAnchor(piece.res, piece_pose, pf, &ax, &ay, &attr) &&
          attr == head_attr) {
        last_dx = head_dx + static_cast<float>(head_ax - ax);
        last_dy = head_dy + static_cast<float>(head_ay - ay);
      }
    } else if (body_anchored &&
               SpriteFrameAnchor(piece.res, piece_pose, 0, &ax, &ay, &attr) &&
               attr == body_attr) {
      last_dx = static_cast<float>(body_ax - ax);
      last_dy = static_cast<float>(body_ay - ay);
    }

    // La TÊTE devient la référence des pièces suivantes : son nombre d'images
    // pour l'animation alternative, son décalage et son ancre pour l'accrochage.
    // Elle est en tête de liste, donc tout est connu quand les coiffes arrivent.
    if (!list[i].accessory) {
      ref_frames = piece_frames;
      head_dx = last_dx;
      head_dy = last_dy;
      head_ok = SpriteFrameAnchor(piece.res, piece_pose, pf, &head_ax, &head_ay,
                                  &head_attr);
    }

    // Cadrage : image 0 de la pièce, à son ancre. Rien de ce qui varie d'une
    // image à l'autre n'entre dans la boîte englobante.
    //
    // 🔴 Sauf en cadrage « corps seul » : là, l'échelle ne doit dépendre QUE du
    // corps, sinon un chapeau volumineux rétrécit le personnage.
    if (!opts.fit_body_only) {
      SpriteQuad ref[kMaxQuads];
      const int rn = SpriteResolveFrame(piece.res, piece_pose, 0, ref, kMaxQuads,
                                        /*apply_rotation=*/false);
      if (rn > 0) grow(ref, rn, last_dx, last_dy);
    }

    SpriteQuad tmp[kMaxQuads];
    const int m =
        SpriteResolveFrame(piece.res, piece_pose, pf, tmp, kMaxQuads - dst_n,
                           /*apply_rotation=*/false);
    if (m <= 0) continue;
    for (int k = 0; k < m; ++k) {
      for (int c = 0; c < 4; ++c) {
        tmp[k].corner[c].x += last_dx;
        tmp[k].corner[c].y += last_dy;
      }
      dst[dst_n++] = tmp[k];
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

  // Le cadrage est fixé : tout ce qui veut se caler sur le pantin peut l'être.
  // 🔴 (origin_x, origin_y) est l'ORIGINE DE L'ACTEUR, pas le centre de la
  // boîte : les coins des quads vivent dans le repère d'ancrage du .act, donc
  // le point (0,0) de ce repère tombe exactement là. C'est la convention que
  // demandent les couches d'effets de costume.
  DollPlacement placement;
  placement.origin_x = origin_x;
  placement.origin_y = origin_y;
  placement.scale    = scale;
  if (opts.out_placement) *opts.out_placement = placement;
  // AVANT le moindre calque — y compris ceux de la cape passée derrière : un
  // effet « avant le personnage » se place sous TOUT le pantin.
  if (opts.underlay) opts.underlay(opts.underlay_ctx, placement);

  bool drawn = false;
  auto emit = [&](const SpriteQuad* q, int count) {
    for (int i = 0; i < count; ++i) {
      if (!q[i].tex) continue;
      // Teinte de l'appelant PAR-DESSUS celle du calque, canal par canal.
      const ImU32 t = q[i].tint;
      const ImU32 col = IM_COL32(
          ((t >> IM_COL32_R_SHIFT) & 0xFF) * ((tint >> IM_COL32_R_SHIFT) & 0xFF) / 255,
          ((t >> IM_COL32_G_SHIFT) & 0xFF) * ((tint >> IM_COL32_G_SHIFT) & 0xFF) / 255,
          ((t >> IM_COL32_B_SHIFT) & 0xFF) * ((tint >> IM_COL32_B_SHIFT) & 0xFF) / 255,
          ((t >> IM_COL32_A_SHIFT) & 0xFF) * ((tint >> IM_COL32_A_SHIFT) & 0xFF) / 255);
      ImVec2 p[4];
      for (int c = 0; c < 4; ++c)
        p[c] = ImVec2(origin_x + q[i].corner[c].x * scale,
                      origin_y + q[i].corner[c].y * scale);
      const ImVec2& uv0 = q[i].uv0;
      const ImVec2& uv1 = q[i].uv1;
      draw_list->AddImageQuad(reinterpret_cast<ImTextureID>(q[i].tex), p[0],
                              p[1], p[2], p[3], ImVec2(uv0.x, uv0.y),
                              ImVec2(uv1.x, uv0.y), ImVec2(uv1.x, uv1.y),
                              ImVec2(uv0.x, uv1.y), col);
      drawn = true;
    }
  };
  // 🔴 L'arrière-plan D'ABORD. Le moteur du jeu ne mélange jamais un calque de
  // cape aux calques du corps : c'est tout devant ou tout derrière, décidé par
  // le Lua `DrawOnTop`. Deux passes suffisent donc à le reproduire.
  emit(back, back_n);
  emit(quads, n);
  return drawn;
}

}  // namespace ro
