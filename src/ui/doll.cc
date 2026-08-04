#include "ui/doll.h"

#include <Windows.h>  // lstrcpynA + SEH autour de la table native

#include <cstdio>
#include <cstring>

#include "ragnarok/lua.h"  // la cape interroge deux globaux Lua
#include "ui/head_icon.h"
#include "ui/sprite_path.h"
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
//
// 🔴 Le préfixe de race n'est PAS constant : `Race_GetBodyPrefix6` (0x00b44190)
// rend `도람족` pour un Doram et `인간족` pour tout le monde d'autre. Il vient
// donc de `ro::RaceFolder` (ui/sprite_path.h) — l'écrire en dur, c'était perdre
// le Summoner et, avec son corps, le pantin entier.
constexpr uintptr_t kFmtBodyTail = 0x01088A18;

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

// `out_job` (facultatif) reçoit la CLASSE effective du corps — l'index rendu
// par `Job_ResolveBodyClass` remis à son échelle d'origine (+3950).
//
// 🔴 C'est elle, et non `look.job`, qui décide de la race du corps : le natif
// passe très exactement `index + 3950` à `Race_GetBodyPrefix6`. La distinction
// compte dès qu'un style de corps est équipé, et c'est aussi le seul champ
// fiable quand l'appelant renseigne `body` sans `job`.
bool BodyResName(int job, int body, char* out, size_t out_size,
                 int* out_job = nullptr) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  if (out_job) *out_job = job;
  bool ok = false;
  __try {
    // Le 3e argument à 1 retranche 3950 au résultat : c'est ce qui transforme
    // un id de classe 4xxx en index de tableau.
    const int cls = reinterpret_cast<ResolveBodyClassFn>(kJobResolveBodyClass)(
        job, body, 1);
    if (out_job) *out_job = cls + 3950;
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
//
// `out_job` (facultatif) : la classe effective du corps, cf. `BodyResName`.
// L'appelant s'en sert pour la TÊTE et les COIFFES, qui doivent suivre la même
// race que le corps.
bool BodyBasePath(const DollLook& look, char* out, size_t out_size,
                  int* out_job = nullptr) {
  char job[128] = {0};
  int  body_job = look.job;
  if (!BodyResName(look.job, look.body, job, sizeof(job), &body_job))
    return false;
  if (out_job) *out_job = body_job;

  const char* sex = SexFolder(look.sex);
  char tail[256];
  // ⚠ `std::snprintf` : le gabarit n'est pas un littéral (il est lu dans le
  // binaire), et la famille sécurisée déclencherait C4774.
  std::snprintf(tail, sizeof(tail),
                reinterpret_cast<const char*>(kFmtBodyTail), sex, job, sex,
                "spr");
  const size_t n = std::strlen(tail);
  if (n > 4) tail[n - 4] = '\0';  // retire « .spr » : sprite_view veut une base

  std::snprintf(out, out_size, "data\\sprite\\%s%s", RaceFolder(body_job), tail);
  return true;
}

// 🔴 Le dossier dépend de la RACE, et les fichiers ne se partagent PAS.
//
// `Job_BuildBodyPalettePath_impl` (0x00b42580) a DEUX gabarits : `몸\…` pour un
// humain, `%s\body\…` (préfixe de race) pour un Doram. `BodyPalUnisex` réécrit
// les deux — il traite explicitement le cas Doram — mais en gardant le dossier
// de race : `도람족\body_<n>.pal`.
//
// ⚠ Et c'est TANT MIEUX : mesuré le 2026-08-04 sur `summoner_남.spr`, 46 % des
// pixels opaques du corps Doram tombent sur des index que la palette humaine
// laisse NOIRS, et 5 index sur 74 seulement y portent la couleur attendue. Les
// deux races n'ont pas la même indexation — aucun fichier commun n'est possible.
void BodyPalettePath(int color, int job, char* out, size_t out_size) {
  std::snprintf(out, out_size, "data\\palette\\%s\\body_%d.pal",
                IsDoramJob(job) ? RaceFolder(job) : kBodyPalFolder, color);
}

// ── Une pièce du pantin ──────────────────────────────────────────────────────
constexpr int kMaxQuads  = 96;  // toutes pièces confondues
constexpr int kMaxPieces = 12;  // tête + 3 coiffes + cape + arme/traînée/bouclier
// Chemins de repli d'une même pièce (cf. `Attached::cand`) : une coiffe de
// Doram en demande quatre — variante `_doram` et variante humaine, chacune dans
// les deux casses possibles du nom d'accessoire.
constexpr int kMaxCandidates = 4;

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

// Variante DORAM : `악세사리\%s_doram\%s%s.%s` — même nom d'accessoire, dossier
// `<sexe>_doram`.
//
// 🔴 Elle n'est pas exclusive. `Hair_BuildHeadgearSpritePath_impl` (0x00b41d90)
// essaie ce chemin, DEMANDE AU GESTIONNAIRE DE TEXTURES s'il existe
// (`UITextureMgr_ResourceExists` 0x00a8e500) et retombe sur le gabarit humain
// sinon : la plupart des chapeaux n'ont pas de version Doram et se portent tels
// quels. On reproduit ce repli par la liste de candidats du composeur, qui
// essaie les chemins dans l'ordre jusqu'à ce qu'un fichier réponde.
constexpr uintptr_t kFmtHeadgearDoram = 0x01088A80;

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
bool HeadgearBasePath(int view_id, int sex, bool lower, bool doram, char* out,
                      size_t out_size) {
  char name[128];
  if (!HeadgearResName(view_id, name, sizeof(name))) return false;
  if (lower) {
    for (char* p = name; *p; ++p)
      if (*p >= 'A' && *p <= 'Z') *p = static_cast<char>(*p - 'A' + 'a');
  }

  const char* sex_tok = SexFolder(sex);
  char tail[256];
  std::snprintf(tail, sizeof(tail),
                reinterpret_cast<const char*>(doram ? kFmtHeadgearDoram
                                                    : kFmtHeadgear),
                sex_tok, sex_tok, name, "spr");
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
    const char* sex = SexFolder(look.sex);
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
  // La classe effective du corps sort d'ici : c'est elle qui donne la RACE, et
  // la tête comme les coiffes doivent suivre la même que lui.
  int body_job = look.job;
  if (!BodyBasePath(look, base, sizeof(base), &body_job)) return false;
  // Doram (Summoner) : dossier de race, table de coiffures et variante de
  // couvre-chef changent tous les trois. `look.job` reste consulté parce qu'un
  // appelant peut renseigner la classe sans le style de corps.
  const int  race_job = IsDoramJob(body_job) ? body_job : look.job;
  const bool doram    = IsDoramJob(race_job);
  if (look.clothes_color >= 0)
    BodyPalettePath(look.clothes_color, race_job, pal, sizeof(pal));

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
  //
  // ⚠ `freeze_body` fige le corps SANS toucher aux accessoires : l'appelant qui
  // propose « Marche » et « Marche (animé) » ne parle que du corps.
  //
  // ⚠ La règle est bien « TOUT SAUF le repos et l'assise », et non « seulement
  // la marche et le combat » comme ici auparavant : c'est ce que fait le client,
  // qui force `frame = 0` pour les types d'action 0 et 2 uniquement. Une attaque
  // ou un ramassage doivent donc pouvoir se jouer — c'est tout leur intérêt,
  // leur image 0 ne montre rien de la posture.
  unsigned actor_frame = 0;
  if (anim_seconds >= 0.0f && !opts.freeze_body && anim != 0 && anim != 2)
    actor_frame = SpriteFrameIndex(body.res, body_pose, anim_seconds);
  // Image imposée : elle court-circuite l'horloge ET `freeze_body`. C'est ce
  // que veut un enregistrement — viser une image, pas un instant.
  if (opts.force_frame >= 0)
    actor_frame = static_cast<unsigned>(opts.force_frame);

  // ── Tête tournée (`/doridori`) ────────────────────────────────────────────
  //
  // 🔴 L'index d'image est PARTAGÉ par tout l'acteur : le client n'a qu'un seul
  // champ (`acteur+0x3C`), lu aussi bien pour le corps que pour la tête. C'est
  // pour cela que `Actor_ComputeHeadAttach` (0x007adbc0) compare l'ancre de la
  // tête à celle du corps PRISE SUR LA MÊME IMAGE — les deux sont forcément
  // cohérentes.
  //
  // Ne l'imposer qu'ici garantit cette cohérence. Le faire seulement pour la
  // tête laissait le corps à son image 0 : les deux ancres venaient alors
  // d'images différentes et la tête se détachait du cou (visible surtout sur
  // l'image 2, dont l'ancre s'écarte le plus — mesuré sur `1_남.act`, assis :
  // ancres (1,-35) / (-2,-36) / (4,-36) pour les images 0 / 1 / 2).
  //
  // Réservé au corps IMMOBILE : en jeu, `/doridori` se joue à l'arrêt, et
  // écraser l'index pendant une marche figerait l'animation.
  if (opts.head_dir > 0 && opts.force_frame < 0 && actor_frame == 0)
    actor_frame = static_cast<unsigned>(opts.head_dir);
  // Le corps a-t-il seulement cette image ? `Act_GetFrame` (0x0070f4b0) retombe
  // sur la PREMIÈRE quand l'index dépasse ; sans ce repli, une classe dont le
  // corps porte moins d'images que la tête ne rendrait AUCUN quad, et le pantin
  // disparaîtrait au lieu de simplement ne pas pencher la tête.
  {
    const int body_frames = SpriteActionFrameCount(body.res, body_pose);
    if (body_frames > 0 && actor_frame >= static_cast<unsigned>(body_frames))
      actor_frame = 0;
  }

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

  // Enveloppe MAXIMALE d'une pièce : ses 8 orientations, toutes leurs images.
  //
  // 🔴 C'est ce qui rend l'échelle immobile. La mesure par défaut ne regarde
  // que l'image 0 de l'orientation affichée, donc elle change quand on tourne
  // le personnage (une arme s'écarte de face, se replie de dos) ou quand son
  // animation l'étire. Ici on prend le maximum de tout : ce qui rentre rentre
  // dans toutes les positions, et l'échelle ne dépend plus de celle qu'on
  // regarde.
  //
  // Le décalage d'ancrage passé est celui de la pose COURANTE. C'est une
  // approximation assumée : on cherche une borne, pas une position, et
  // recalculer l'ancrage des 8 orientations coûterait bien plus que ce que le
  // pixel gagné vaut.
  auto grow_span = [&](const SpriteRes& res, float dx, float dy, bool rotate) {
    for (int dir_i = 0; dir_i < 8; ++dir_i) {
      const unsigned span_pose =
          PieceAction(res, static_cast<unsigned>(anim * 8 + dir_i));
      const int span_frames = SpriteActionFrameCount(res, span_pose);
      for (int f = 0; f < span_frames; ++f) {
        SpriteQuad span[kMaxQuads];
        const int sn = SpriteResolveFrame(res, span_pose,
                                          static_cast<unsigned>(f), span,
                                          kMaxQuads, rotate);
        if (sn > 0) grow(span, sn, dx, dy);
      }
    }
  };
  if (opts.fit_span) grow_span(body.res, 0.0f, 0.0f, /*rotate=*/false);

  // Boîte du CORPS SEUL, retenue avant la moindre pièce rapportée : c'est le
  // repère de `center_on_body`, et le seul qui ne bouge pas quand on change
  // d'arme ou de chapeau.
  const float body_min_x = min_x, body_max_x = max_x;
  const float body_min_y = min_y, body_max_y = max_y;

  int body_ax = 0, body_ay = 0, body_attr = 0;
  const bool body_anchored =
      SpriteFrameAnchor(body.res, body_pose, 0, &body_ax, &body_ay, &body_attr);

  // Ancre du corps à l'image DESSINÉE. C'est elle que réclame `Actor_
  // ComputeHeadAttach` (0x007adbc0) : il pose la tête à `corps.ancre −
  // tête.ancre`, les deux prises à l'action ET à l'image COURANTES.
  //
  // 🔴 C'est le défaut « tête et corps désynchronisés en combat » : le corps
  // s'élance de plusieurs pixels d'une image à l'autre, et une tête recalée à
  // l'image 0 gardait un décalage constant — elle décrochait du cou. Au repos
  // les deux valent la même chose (`SpriteFrameAnchor` force l'image 0 pour les
  // actions < 8), donc rien ne change hors marche et combat.
  int body_cax = 0, body_cay = 0, body_cattr = 0;
  const bool body_canchored = SpriteFrameAnchor(
      body.res, body_pose, actor_frame, &body_cax, &body_cay, &body_cattr);

  // ── Les pièces rapportées, dans l'ordre de dessin ─────────────────────────
  // Tête puis coiffes : bas, milieu, haut. Chacune s'accroche au CORPS (pas en
  // cascade) — c'est ce que fait le natif pour les parties 1 à 3.
  //
  // `cand` = les chemins à essayer, dans l'ordre, jusqu'à ce qu'un fichier
  // réponde. 🔴 Ce n'est pas un luxe : le client lui-même SONDE l'existence des
  // ressources avant de choisir (`UITextureMgr_ResourceExists`), et deux règles
  // s'empilent sur une seule coiffe — la casse du nom d'accessoire, qui ne suit
  // aucune règle, et la variante `_doram` que la plupart des chapeaux n'ont pas.
  // D'où quatre candidats possibles là où deux suffisaient avant le Summoner.
  //
  // `accessory` : distingue une COIFFE de la TÊTE. Seules les coiffes ont une
  // animation alternative ; la tête, elle, sert de RÉFÉRENCE à leur calcul
  // (cf. AltAnimFrame plus bas), d'où sa place en tête de liste.
  //
  // `act` : base du .act quand elle diffère de celle du .spr — la CAPE seule.
  // `behind` : la cape, et elle seule, peut passer DERRIÈRE le corps.
  struct Attached {
    char cand[kMaxCandidates][352];
    int  cand_n;
    char act[352];
    char pal[128];
    bool accessory;
    bool behind;
    // S'accroche à la TÊTE plutôt qu'au corps. Vrai pour les seules coiffes :
    // `Actor_DrawSprites` (0x007ac820) donne aux parties 3 à 6 la partie 2 —
    // la tête — pour référence d'ancrage. La cape, elle, reste sur le corps.
    bool on_head;
    // RÉFÉRENCE des accessoires : la tête, et elle seule. Distinct de
    // `!accessory` depuis que l'arme existe — elle n'est pas un accessoire non
    // plus, et laisser la déduction la promouvoir référence enverrait les
    // coiffes s'accrocher au fer d'une épée.
    bool head_ref;
    // PIÈCE TENUE — arme, traînée, bouclier : AUCUN décalage d'accrochage.
    //
    // 🔴 Mesuré dans `CActorSprite_ComputePartAttachOffset` (0x00605170), qui
    // est un `switch` sur le numéro de partie et ne connaît que trois cas :
    //   partie 1 (tête)          -> ancre du CORPS moins la sienne ;
    //   parties 2, 3, 4, 8       -> ancre de la TÊTE moins la sienne ;
    //   default — 0 (corps) et 5, 6, 7 (arme, traînée, bouclier) -> ZÉRO.
    // La fonction sort avec `param_1[2] = 0` et `param_1[3] = 0` : les pièces
    // tenues sont posées à l'origine de l'acteur, exactement comme le corps.
    // Tout leur mouvement est déjà dans les offsets de leurs calques.
    //
    // ⚠ J'avais cru l'inverse le 2026-08-03, sur un bouclier de Novice qui
    // partait de travers : les ancres corps `8,-50` et bouclier `1,-50` ne
    // coïncidaient pas, j'y ai vu la cause. C'en était une coïncidence — le
    // vrai défaut était l'angle du `.act`, ignoré. Une fois l'angle honoré, le
    // faux ancrage est resté et déplace désormais l'arme et le bouclier de
    // quelques pixels sur toutes les classes dont les ancres diffèrent.
    bool held;
    // Appliquer l'ANGLE des calques.
    //
    // 🔴 Faux pour les huit parties composites — corps, tête, coiffes, cape :
    // elles passent par `Actor_SubmitSpriteQuad` (0x00a1b7c0), qui construit un
    // rectangle aligné aux axes et IGNORE l'angle qu'on lui donne. Les honorer
    // faisait pivoter les chapeaux sur la tête.
    //
    // 🔴 Vrai pour l'arme et le bouclier, qui ne passent JAMAIS par cette
    // fonction — ils vivent sur le `CActorSprite` et suivent un autre chemin de
    // soumission. Et pour eux l'angle n'est pas une décoration : le `.spr` d'un
    // bouclier ne contient que DEUX images, une de face et une de dos. Tout le
    // mouvement du bras pendant l'animation d'attaque est porté par les
    // transformations du `.act`. Les ignorer, c'est afficher un bouclier figé,
    // posé de travers pendant toute l'animation.
    bool rotate;
    // Étiquette de mesure, pour lire un journal d'assemblage sans compter les
    // indices. Purement descriptive.
    const char* tag;
  };
  Attached list[kMaxPieces] = {};
  int list_n = 0;

  // Boîte de la TÊTE et de ses coiffes, en unités .act — de quoi cadrer un
  // portrait sur le visage plutôt que sur la silhouette entière.
  float head_x0 = 0.0f, head_y0 = 0.0f, head_x1 = 0.0f, head_y1 = 0.0f;
  bool  head_seen = false;

  // ── LA TABLE D'ORDRE, du fond vers l'avant ────────────────────────────────
  //
  //   DE DOS  {2,3,4,5} : bouclier · CORPS · tête · coiffes · arme · cape
  //   DE FACE {0,1,6,7} : CORPS · tête · coiffes · cape · arme · bouclier
  //
  // (le corps en capitales : il n'est pas dans la liste, il est posé avant
  //  elle ; tout ce qui doit passer derrière lui part dans le seau `behind`.)
  //
  // Elle n'est pas une convention de confort — c'est le résultat OBSERVABLE de
  // trois mécanismes natifs enchaînés, qu'il vaut mieux écrire une fois pour
  // toutes que redécouvrir pièce par pièce.
  //
  // Numérotation des NEUF parties, celle du natif :
  //   0 corps · 1 tête · 2-4 coiffes bas/milieu/haut · 8 cape
  //   5 arme  · 6 traînée · 7 bouclier
  //
  // 1. `Actor_SelectPartLayerByDir` (0x00d38850) donne l'ordre d'ÉMISSION, et
  //    il dépend du groupe de direction :
  //      FACE : 1, 0, 2, 3, 4, 8, 5, 6, 7
  //      DOS  : 7, 1, 0, 2, 3, 4, 8, 5, 6
  //    🔴 Le BOUCLIER est la seule pièce qui change de camp : de dos il est
  //    émis EN PREMIER, donc derrière le corps lui-même — le personnage le
  //    porte de l'autre côté et son propre dos le recouvre.
  //
  // 2. `CActorSprite_DeferQuadSorted` (0x006046e0) RETRIE : les parties 2, 3,
  //    4 et 8 prennent une clé Lua (100 à 400), toutes les autres une clé de
  //    séquence (1, 2, 3…). Coiffes et CAPE remontent donc au-dessus de l'arme
  //    dans les deux orientations — d'où la cape en dernier de dos.
  //
  // 3. `weapon_layer.cc`, notre patch, relève l'arme et le bouclier au-dessus
  //    de tout pour le seul groupe FACE, et laisse le DOS intact.
  //
  // ⚠ La liste est bâtie dans l'ordre des DÉPENDANCES autant que du dessin : la
  // TÊTE doit précéder les coiffes (elle est leur référence d'ancrage) et la
  // cape (elle donne le gabarit d'animation des accessoires). Les deux tables
  // ci-dessus la placent avant elles, donc les deux contraintes coïncident.
  const int  dir_group = opts.dir & 7;
  const bool back_view = (dir_group >= 2 && dir_group <= 5);

  // Ajoute un chemin à la liste d'essais d'une pièce. Silencieusement ignoré si
  // la liste est pleine ou le chemin vide : un candidat de moins n'est pas une
  // erreur, c'est une variante qui n'existe pas.
  auto add_cand = [](Attached& a, const char* path) {
    if (!path || !*path || a.cand_n >= kMaxCandidates) return;
    lstrcpynA(a.cand[a.cand_n], path, static_cast<int>(sizeof(a.cand[0])));
    ++a.cand_n;
  };

  auto add_held = [&](const DollHeldPiece& p, const char* tag, bool behind) {
    if (!p.spr_base || !*p.spr_base || list_n >= kMaxPieces) return;
    Attached& a = list[list_n];
    a.tag = tag;
    add_cand(a, p.spr_base);
    if (p.act_base && *p.act_base)
      lstrcpynA(a.act, p.act_base, static_cast<int>(sizeof(a.act)));
    a.held   = true;
    a.rotate = true;
    a.behind = behind;
    ++list_n;
  };

  auto add_garment = [&]() {
    // Deux refus repris tels quels de `CActorSprite_DrawGarmentLayer` : le type
    // d'action 8 (poses 64 à 71) n'a jamais de cape, et trois classes n'en ont
    // pas au FÉMININ.
    //
    // Elle est marquée `accessory` : le natif lui applique la même animation
    // alternative qu'aux coiffes, avec la même référence — le slot 1,
    // c'est-à-dire la TÊTE. Sa place après la tête dans la liste suffit donc à
    // ce que le calcul tombe juste.
    const bool garment_job_ok =
        look.sex != 0 ||
        (look.job != 4086 && look.job != 4087 && look.job != 4112);
    if (look.garment <= 0 || list_n >= kMaxPieces || !garment_job_ok ||
        (pose >= 64 && pose <= 71))
      return;
    char robe[128];
    char job_layout[352], flat[352];
    Attached& g = list[list_n];
    // Le .spr peut venir de la disposition plate, le .act JAMAIS : il est
    // toujours par classe. Plate d'abord, par classe en repli — et c'est bien
    // celle par classe, la seule sûre, qui sert de base au .act.
    if (!GarmentResNameCached(look.garment, robe, sizeof(robe)) ||
        !GarmentBasePath(look, robe, /*job_layout=*/true, job_layout,
                         sizeof(job_layout)))
      return;
    if (!GarmentBasePath(look, robe, /*job_layout=*/false, flat, sizeof(flat)))
      flat[0] = '\0';
    add_cand(g, flat);
    add_cand(g, job_layout);
    lstrcpynA(g.act, job_layout, static_cast<int>(sizeof(g.act)));
    g.pal[0] = '\0';
    g.tag = "cape";
    g.accessory = true;
    // Devant ou derrière le CORPS : c'est le Lua `DrawOnTop` qui tranche, et
    // lui seul. Derrière, elle rejoint le seau `behind` — où le bouclier de dos
    // l'a déjà précédée, l'ordre entre eux étant celui de la liste.
    g.behind = !GarmentDrawsOnTopCached(look.garment, look.sex, look.job, pose,
                                        actor_frame);
    ++list_n;
  };

  // 1. Le bouclier de DOS : tout au fond, avant même le corps.
  if (back_view) add_held(look.shield, "bouclier", /*behind=*/true);

  // 2. La tête — référence de tout ce qui suit.
  char head_base[352];
  if (list_n < kMaxPieces &&
      HeadSpriteBasePath(look.hair, look.sex, race_job, head_base,
                         sizeof(head_base))) {
    add_cand(list[list_n], head_base);
    list[list_n].pal[0] = '\0';
    list[list_n].accessory = false;  // la TÊTE : référence, jamais animée
    list[list_n].head_ref  = true;
    list[list_n].tag       = "tete";
    // 🔴 De DOS, la tête passe DERRIÈRE le corps.
    //
    // Le natif ne traite pas la tête à part : `Actor_BuildLayerDrawOrder`
    // (0x007ad0b0) construit l'empilement de ses huit parties, puis le parcourt
    // À L'ENVERS quand le drapeau `acteur+0x45` est posé — tout bascule d'un
    // coup, la tête comme le reste.
    //
    // Debout, l'inversion ne se voit pas : la tête est au-dessus des épaules,
    // rien ne se recouvre. Elle saute aux yeux dès que le personnage se PENCHE
    // — au ramassage, le buste doit masquer la nuque, et la tête dessinée en
    // dernier passait par-dessus le dos.
    list[list_n].behind = back_view;
    if (look.hair_color >= 0)
      HairPalettePath(look.hair_color, race_job, list[list_n].pal,
                      sizeof(list[list_n].pal));
    ++list_n;
  }

  // 3. Les coiffes, du bas vers le haut.
  //
  // Ordre des essais, celui du client : la variante de RACE d'abord — elle
  // n'existe que pour une poignée de chapeaux, et le natif la sonde avant de se
  // rabattre sur la commune — puis chacune dans les deux casses du nom
  // d'accessoire. Sur un humain, les deux premières ne sont même pas formées.
  struct GearVariant { bool doram, lower; };
  static const GearVariant kGearVariants[kMaxCandidates] = {
      {true, false}, {true, true}, {false, false}, {false, true}};

  const int gear[3] = {look.head_low, look.head_mid, look.head_top};
  for (int g = 0; g < 3 && list_n < kMaxPieces; ++g) {
    if (gear[g] <= 0) continue;
    char gear_path[352];
    Attached& a = list[list_n];
    for (const GearVariant& v : kGearVariants) {
      if (v.doram && !doram) continue;
      if (HeadgearBasePath(gear[g], look.sex, v.lower, v.doram, gear_path,
                           sizeof(gear_path)))
        add_cand(a, gear_path);
    }
    if (a.cand_n == 0) continue;  // accessoire inconnu de la table

    a.pal[0] = '\0';
    a.accessory = true;
    a.on_head   = true;
    // Les coiffes suivent la tête de part et d'autre du corps : elles s'y
    // accrochent, et les séparer ferait flotter un chapeau devant un dos dont
    // la tête est derrière. Placées APRÈS la tête dans le seau arrière, elles
    // restent au-dessus d'elle — ce qu'on veut : de dos, on voit le chapeau.
    a.behind    = back_view;
    a.tag       = (g == 0) ? "coiffe-bas"
                : (g == 1) ? "coiffe-milieu"
                           : "coiffe-haut";
    ++list_n;
  }

  // 4. De FACE, la cape passe SOUS l'arme : `weapon_layer.cc` relève l'arme
  //    au-dessus de tout, cape comprise.
  if (!back_view) add_garment();

  // 5. L'arme et sa traînée.
  add_held(look.weapon, "arme", /*behind=*/false);
  add_held(look.weapon_trail, "trainee", /*behind=*/false);

  // 6. De FACE, le bouclier ferme la marche, toujours par le même patch.
  if (!back_view) add_held(look.shield, "bouclier", /*behind=*/false);

  // 7. De DOS, c'est la CAPE qui ferme la marche — la clé Lua la fait remonter
  //    au-dessus de l'arme, et c'est bien ce qui se voit : elle pend dans le
  //    dos, devant le personnage vu de derrière.
  if (back_view) add_garment();

  // Dernier décalage VALIDE. 🔴 Quand une pièce n'a pas d'ancre exploitable, le
  // natif REPREND celui-ci au lieu de repartir de zéro — remettre à zéro
  // recollerait la pièce à l'origine du corps, donc visiblement de travers.
  //
  // 🔴 DEUX décalages par pièce, et pas un : celui du DESSIN, calculé sur les
  // images qu'on affiche, et celui de la MESURE, calculé sur l'image 0.
  //
  // Sans cette séparation, la boîte englobante se met à dépendre de l'image
  // courante — donc l'échelle et le centrage —, et tout le pantin « respire » au
  // rythme de l'animation. C'est le même piège que pour les QUADS, où l'on
  // mesure déjà sur l'image 0 : il vaut aussi pour ce qui les DÉPLACE.
  float last_dx = 0.0f, last_dy = 0.0f;
  float last_rdx = 0.0f, last_rdy = 0.0f;

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
  //
  // Le doublet `_r` est la même chose à l'image 0 — la référence de MESURE.
  float head_dx = 0.0f, head_dy = 0.0f;
  int   head_ax = 0, head_ay = 0, head_attr = 0;
  bool  head_ok = false;
  float head_rdx = 0.0f, head_rdy = 0.0f;
  int   head_rax = 0, head_ray = 0, head_rattr = 0;
  bool  head_rok = false;

  // Calques à poser DERRIÈRE le corps. Seule la CAPE y atterrit, et seulement
  // dans certaines orientations — c'est la seule pièce que le natif fasse
  // changer de côté. Un tampon plutôt qu'un tri : l'ordre à l'intérieur de
  // chaque passe reste celui de la liste, qui EST l'ordre de dessin.
  SpriteQuad back[kMaxQuads];
  int back_n = 0;

  for (int i = 0; i < list_n; ++i) {
    SpriteQuad* dst = list[i].behind ? back : quads;
    int& dst_n      = list[i].behind ? back_n : n;
    if (dst_n >= kMaxQuads) continue;

    // Le premier candidat qui répond gagne. Aucun n'est obligatoire : une pièce
    // dont pas un seul chemin n'existe est simplement omise, pas une erreur.
    Piece piece;
    bool loaded = false;
    for (int c = 0; c < list[i].cand_n && !loaded; ++c)
      loaded = LoadPiece(list[i].cand[c], list[i].act, list[i].pal, &piece);
    if (!loaded) continue;

    // 🔴 TOUTES les pièces partagent l'action du corps. C'est ce que fait le
    // natif : `CActorSprite_RenderCompositeJobSprite` lit une seule action
    // (this+14) et une seule image (this+15) pour ses huit parties.
    //
    // Leur faire jouer le mouvement de repos à la place semblait plus logique
    // — une décoration vit pour elle-même — mais c'est faux : l'action de repos
    // d'un costume peut être tout autre chose que son animation en pose assise.
    //
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
    // ── Tête tournée (`/doridori`) ────────────────────────────────────────────
    //
    // 🔴 Une tête tourne par son IMAGE, pas par son action ni par sa direction.
    // Un `.act` de tête porte exactement **3 images** par action — droit devant,
    // penché d'un côté, penché de l'autre — et c'est l'index d'image de l'acteur
    // (`acteur+0x3C`) qui les choisit. Le client le confirme dans
    // `Actor_DrawSprites` (0x007ac820), dont le calcul d'offset Doram teste
    // `+0x3C == 1` / `== 2` : ce champ porte bien 0/1/2 pour la tête.
    //
    // Les deux autres pistes ont été essayées et sont FAUSSES, chacune avec sa
    // signature à l'écran :
    //   * changer d'ACTION (`head_dir*8 + dir`) : aucun effet visible, les
    //     actions d'une tête sont les mêmes mouvements que celles du corps ;
    //   * décaler la DIRECTION (`dir ± 1`) : la tête se détache du corps (les
    //     ancres d'une autre direction ne correspondent plus) et bascule en
    //     MIROIR d'un côté — les directions 5..7 sont les vues 3..1 retournées.
    //
    // Les coiffes suivent SANS traitement particulier : leur `.act` porte un
    // multiple exact des images de la tête (mesuré : 24 = 8 × 3), donc
    // l'animation alternative ci-dessous les place dans le sous-groupe de la
    // bonne image de tête. C'est exactement ce que fait le natif.
    //
    // Rien de spécifique à la tête ICI non plus : `actor_frame` porte déjà
    // l'image voulue pour TOUT l'acteur (cf. son calcul, plus haut).
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
    // ⚠ Image hors bornes -> IMAGE 0, et surtout pas la dernière.
    //
    // 🔴 `Act_GetFrame` (0x0070f4b0) est explicite : si l'index dépasse, il rend
    // `*begin`, c'est-à-dire la PREMIÈRE image. J'avais mis un plafonnement sur
    // la dernière, copié du site Moonlight (`min($animation, $count - 1)`) —
    // c'est le comportement du site, pas celui du client.
    //
    // La différence ne se voit que si la pièce a MOINS d'images que le corps
    // pour cette action, ce qui dépend de la classe : d'où une tête synchronisée
    // sur un High Wizard et décrochée sur un White Smith, avec exactement le
    // même fichier de tête.
    const int piece_frames = SpriteActionFrameCount(piece.res, piece_pose);
    if (piece_frames > 0 && pf >= static_cast<unsigned>(piece_frames)) pf = 0u;



    // ── Accrochage ────────────────────────────────────────────────────────
    //
    // Trois régimes, et ce n'est pas une commodité : c'est ce que fait
    // `Actor_DrawSprites` (0x007ac820) et sa `Actor_ComputeHeadAttach`.
    //
    //   TÊTE                 -> référence = le CORPS, ancres des images
    //                           COURANTES des deux.
    //   COIFFE               -> référence = la TÊTE, ancres des images
    //                           COURANTES des deux.
    //   ARME, TRAÎNÉE, BOUCLIER -> AUCUN décalage (cf. `Attached::held`).
    //   CAPE                 -> référence = le CORPS, ancre de l'image 0.
    //
    // ⚠ La CAPE est la seule exception, et elle est mesurée : le natif ne lui
    // donne aucun décalage du tout, son `.act` portant lui-même son mouvement.
    // La passer à l'image courante la ferait dériver.
    //
    // 🔴 L'attache ne s'applique que si les DEUX ancres portent le même `attr` :
    // c'est le test du natif, et sans lui on recale une pièce sur une ancre qui
    // ne la concerne pas.
    //
    // Chaque régime produit DEUX décalages : celui du dessin et celui de la
    // mesure (image 0). Cf. `last_rdx` — sans lui la boîte englobante suivrait
    // l'animation et le pantin respirerait.
    int ax = 0, ay = 0, attr = 0;
    if (list[i].held) {
      // Arme, traînée, bouclier : à l'origine, comme le corps. Remise à zéro
      // EXPLICITE — `last_dx` survit d'une pièce à l'autre, et sans elle la
      // pièce tenue héritait du décalage de la coiffe qui la précède.
      last_dx = last_dy = last_rdx = last_rdy = 0.0f;
    } else if (list[i].on_head) {
      // Coiffe : ancre de la TÊTE moins la sienne, toutes deux sur l'image
      // qu'elles DESSINENT — c'est ce mouvement-là qui porte le balancement.
      //
      // 🔴 Ancre inexploitable -> on retombe sur le décalage de la TÊTE, et
      // SURTOUT pas sur celui de la coiffe précédente. Le natif initialise sa
      // différence à zéro avant le test (`param_3 = 0`) et n'ajoute ensuite que
      // le décalage de tête mémorisé : une coiffe sans ancre se pose donc sur la
      // tête, pas là où s'était posée sa voisine. Mesuré sur un costume dont
      // l'ancre manque à certaines images — il héritait des 3 px de la coiffe
      // d'avant.
      if (head_ok &&
          SpriteFrameAnchor(piece.res, piece_pose, pf, &ax, &ay, &attr) &&
          attr == head_attr) {
        last_dx = head_dx + static_cast<float>(head_ax - ax);
        last_dy = head_dy + static_cast<float>(head_ay - ay);
      } else {
        last_dx = head_dx;
        last_dy = head_dy;
      }
      if (head_rok &&
          SpriteFrameAnchor(piece.res, piece_pose, 0, &ax, &ay, &attr) &&
          attr == head_rattr) {
        last_rdx = head_rdx + static_cast<float>(head_rax - ax);
        last_rdy = head_rdy + static_cast<float>(head_ray - ay);
      } else {
        last_rdx = head_rdx;
        last_rdy = head_rdy;
      }
    } else if (list[i].head_ref) {
      // La TÊTE sur le CORPS, aux images courantes des deux : c'est le calcul
      // exact du `case 1` de `CActorSprite_ComputePartAttachOffset`.
      if (body_canchored &&
          SpriteFrameAnchor(piece.res, piece_pose, pf, &ax, &ay, &attr) &&
          attr == body_cattr) {
        last_dx = static_cast<float>(body_cax - ax);
        last_dy = static_cast<float>(body_cay - ay);
      }
      if (body_anchored &&
          SpriteFrameAnchor(piece.res, piece_pose, 0, &ax, &ay, &attr) &&
          attr == body_attr) {
        last_rdx = static_cast<float>(body_ax - ax);
        last_rdy = static_cast<float>(body_ay - ay);
      }
    } else if (body_anchored &&
               SpriteFrameAnchor(piece.res, piece_pose, 0, &ax, &ay, &attr) &&
               attr == body_attr) {
      last_dx  = static_cast<float>(body_ax - ax);
      last_dy  = static_cast<float>(body_ay - ay);
      last_rdx = last_dx;
      last_rdy = last_dy;
    }

    // La TÊTE devient la référence des pièces suivantes : son nombre d'images
    // pour l'animation alternative, son décalage et son ancre pour l'accrochage.
    // Elle est en tête de liste, donc tout est connu quand les coiffes arrivent.
    if (list[i].head_ref) {
      ref_frames = piece_frames;
      head_dx = last_dx;
      head_dy = last_dy;
      head_ok = SpriteFrameAnchor(piece.res, piece_pose, pf, &head_ax, &head_ay,
                                  &head_attr);
      head_rdx = last_rdx;
      head_rdy = last_rdy;
      head_rok = SpriteFrameAnchor(piece.res, piece_pose, 0, &head_rax,
                                   &head_ray, &head_rattr);
    }

    // Cadrage : image 0 de la pièce, à son ancre D'IMAGE 0. Rien de ce qui varie
    // d'une image à l'autre n'entre dans la boîte englobante — ni les quads, ni
    // le décalage qui les déplace.
    //
    // 🔴 Sauf en cadrage « corps seul » : là, l'échelle ne doit dépendre QUE du
    // corps, sinon un chapeau volumineux rétrécit le personnage.
    if (opts.fit_span) {
      grow_span(piece.res, last_rdx, last_rdy, list[i].rotate);
    } else if (!opts.fit_body_only) {
      SpriteQuad ref[kMaxQuads];
      const int rn = SpriteResolveFrame(piece.res, piece_pose, 0, ref, kMaxQuads,
                                        list[i].rotate);
      if (rn > 0) grow(ref, rn, last_rdx, last_rdy);
    }

    SpriteQuad tmp[kMaxQuads];
    const int m =
        SpriteResolveFrame(piece.res, piece_pose, pf, tmp, kMaxQuads - dst_n,
                           list[i].rotate);
    if (m <= 0) continue;
    // La tête et ses coiffes forment la boîte que réclame un portrait. On la
    // relève sur les quads DESSINÉS : c'est là qu'ils sont, et une vignette de
    // tête n'a pas le problème de « respiration » du cadrage général — elle est
    // recalculée à chaque frame de toute façon.
    const bool is_head_part = list[i].head_ref || list[i].on_head;
    for (int k = 0; k < m; ++k) {
      for (int c = 0; c < 4; ++c) {
        tmp[k].corner[c].x += last_dx;
        tmp[k].corner[c].y += last_dy;
        if (is_head_part) {
          const float hx = tmp[k].corner[c].x, hy = tmp[k].corner[c].y;
          if (!head_seen) {
            head_x0 = head_x1 = hx;
            head_y0 = head_y1 = hy;
            head_seen = true;
          } else {
            if (hx < head_x0) head_x0 = hx;
            if (hx > head_x1) head_x1 = hx;
            if (hy < head_y0) head_y0 = hy;
            if (hy > head_y1) head_y1 = hy;
          }
        }
      }
      dst[dst_n++] = tmp[k];
    }
  }

  // ── Cadrage ───────────────────────────────────────────────────────────────
  //
  // Deux repères possibles, et un seul jeu de règles : tout doit rentrer, le
  // personnage est centré en X, ses PIEDS touchent le bas du rectangle. Ce qui
  // change avec `center_on_body`, c'est QUI définit « centré » et « les pieds ».
  //
  // Par défaut c'est la silhouette entière — bon pour une vignette, où l'objet
  // affiché EST la silhouette. Sur un personnage équipé c'est faux : l'arme ne
  // dépasse que d'un côté, la cape ne descend que derrière, et le personnage se
  // met à glisser dans son cadre au gré de son équipement.
  float ref_cx   = (min_x + max_x) * 0.5f;
  float ref_feet = max_y;
  if (opts.center_on_body) {
    // 🔴 Les PIEDS viennent toujours du corps. Sans ça, une cape longue ou une
    // coiffe qui descend plus bas que les bottes enfoncerait le personnage sous
    // le bas du rectangle.
    ref_feet = body_max_y;
    // Le CENTRE horizontal, lui, ne se recale sur le corps que si l'enveloppe
    // est instable.
    //
    // 🔴 Recentrer sur le corps coûte la moitié du cadre : la demi-largeur
    // retenue est la plus grande des deux mesurées DEPUIS ce centre, donc une
    // coiffe latérale — un compagnon, une monture — fait réserver autant
    // d'espace du côté où il n'y a rien. C'est le « petit pantin au milieu d'un
    // grand cadre vide ».
    //
    // Avec `fit_span`, l'enveloppe couvre déjà toutes les orientations et toutes
    // les images : son centre ne bouge plus, donc le recalage ne protège plus de
    // rien et on garde la pleine largeur.
    if (!opts.fit_span) ref_cx = (body_min_x + body_max_x) * 0.5f;
  }

  // Demi-largeur mesurée DEPUIS ce centre, côté le plus débordant : c'est elle
  // qu'il faut faire tenir dans la demi-largeur du rectangle, pas la largeur
  // totale. Sinon le côté long sort du cadre dès que le centre est décentré.
  float half = max_x - ref_cx;
  if (ref_cx - min_x > half) half = ref_cx - min_x;
  const float span_y = ref_feet - min_y;
  if (half <= 0.0f || span_y <= 0.0f) return false;

  float scale = h / span_y;
  const float fit_x = w * 0.5f / half;
  if (fit_x < scale) scale = fit_x;

  // Plancher de stature : on renonce à tout faire rentrer plutôt que de réduire
  // le personnage sous cette taille. C'est la LARGEUR qui l'impose presque
  // toujours — une coiffe posée à côté du personnage double l'enveloppe — et le
  // résultat est un pantin minuscule sous un grand vide.
  //
  // Mesuré sur la hauteur du CORPS : la seule qui ne dépende ni de l'équipement
  // ni de l'orientation.
  const float body_height = body_max_y - body_min_y;
  if (opts.min_body_height > 0.0f && body_height > 0.0f) {
    const float floor_scale = opts.min_body_height / body_height;
    if (scale < floor_scale) scale = floor_scale;
  }

  // Plafond de l'appelant : il fait rentrer autre chose que le pantin dans le
  // même cadre (un effet de costume), et le pantin doit rétrécir AVEC. Il passe
  // APRÈS le plancher, et l'emporte donc sur lui : un effet qui déborde a une
  // raison de réduire que le pantin seul n'a pas.
  if (opts.scale_limit > 0.0f && opts.scale_limit < scale)
    scale = opts.scale_limit;
  if (scale <= 0.0f) return false;

  // PIEDS en bas : c'est le bas de la boîte de référence qui vient se poser sur
  // le bas du rectangle, pas son centre — sinon un personnage coiffé d'un grand
  // chapeau s'enfoncerait dans le sol.
  float origin_x = x + w * 0.5f - ref_cx * scale;
  float origin_y = y + h - ref_feet * scale;

  // Placement imposé par l'appelant : il a mesuré, il a décidé. Tout le calcul
  // ci-dessus reste utile — il a produit les boîtes qu'il a lues.
  if (opts.place_override) {
    origin_x = opts.place_override->origin_x;
    origin_y = opts.place_override->origin_y;
    scale    = opts.place_override->scale;
  }

  // Le cadrage est fixé : tout ce qui veut se caler sur le pantin peut l'être.
  // 🔴 (origin_x, origin_y) est l'ORIGINE DE L'ACTEUR, pas le centre de la
  // boîte : les coins des quads vivent dans le repère d'ancrage du .act, donc
  // le point (0,0) de ce repère tombe exactement là. C'est la convention que
  // demandent les couches d'effets de costume.
  DollPlacement placement;
  placement.origin_x = origin_x;
  placement.origin_y = origin_y;
  placement.scale    = scale;
  placement.head_x0 = head_x0;  placement.head_y0 = head_y0;
  placement.head_x1 = head_x1;  placement.head_y1 = head_y1;
  placement.head_valid = head_seen;
  placement.body_x0 = body_min_x;  placement.body_y0 = body_min_y;
  placement.body_x1 = body_max_x;  placement.body_y1 = body_max_y;
  placement.body_valid = true;
  // Ce qu'il faut pour rejouer l'action entière sans rouvrir le `.act`.
  placement.frame_count = SpriteActionFrameCount(body.res, body_pose);
  if (placement.frame_count < 1) placement.frame_count = 1;
  {
    const float ms = SpriteFrameIntervalMs(body.res, body_pose);
    // Le `.act` compte en unités de 25 ms — c'est la granularité que le format
    // exprime, et la seule qu'un GIF ait besoin de connaître.
    int units = (ms > 0.0f) ? static_cast<int>(ms / 25.0f + 0.5f) : 4;
    if (units < 1) units = 1;
    placement.frame_delay = units;
  }
  if (opts.out_placement) *opts.out_placement = placement;

  // Mesure seule : on avait besoin des boîtes, pas des pixels.
  if (opts.measure_only) return true;

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
      if (opts.quad_sink) {
        // Le calque part chez l'appelant au lieu d'ImGui — même géométrie, même
        // ordre. C'est ce que consomme l'export GIF pour compositer hors écran.
        DollQuad out;
        out.tex = q[i].tex;
        for (int c = 0; c < 4; ++c) out.corner[c] = p[c];
        out.uv0  = uv0;
        out.uv1  = uv1;
        out.tint = col;
        opts.quad_sink(opts.quad_sink_ctx, out);
      } else {
        draw_list->AddImageQuad(reinterpret_cast<ImTextureID>(q[i].tex), p[0],
                                p[1], p[2], p[3], ImVec2(uv0.x, uv0.y),
                                ImVec2(uv1.x, uv0.y), ImVec2(uv1.x, uv1.y),
                                ImVec2(uv0.x, uv1.y), col);
      }
      drawn = true;
    }
  };
  // 🔴 L'arrière-plan D'ABORD. Le moteur du jeu ne mélange jamais un calque de
  // cape aux calques du corps : c'est tout devant ou tout derrière, décidé par
  // le Lua `DrawOnTop`. Deux passes suffisent donc à le reproduire.
  //
  // Le BOUCLIER emprunte le même seau en vue de dos, où il est émis avant tout
  // le reste (cf. la table d'ordre plus haut).
  emit(back, back_n);
  emit(quads, n);
  return drawn;
}

}  // namespace ro
