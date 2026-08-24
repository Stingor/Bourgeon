#include "ui/sprite_path.h"

#include <Windows.h>  // SEH autour des lectures du binaire

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ro {
namespace {

// ── Corps : résolution du nom de classe et du chemin ─────────────────────────
// `Job_ResolveBodyClass` (0x00d99150). Son 3e argument à 1 retranche 3950 au
// résultat : c'est ce qui transforme un id de classe 4xxx en index de tableau.
constexpr uintptr_t kJobResolveBodyClass = 0x00D99150;
// Les deux bornes du vecteur de noms de corps (std::vector<char*>).
constexpr uintptr_t kBodyResNamesBegin = 0x015FF634;
constexpr uintptr_t kBodyResNamesEnd   = 0x015FF638;
// Gabarit CP949 `\몸통\%s\%s_%s.%s`, LU DANS LE BINAIRE : nos sources sont en
// UTF-8 et un littéral coréen y serait mal encodé.
constexpr uintptr_t kFmtBodyTail = 0x01088A18;

using ResolveBodyClassFn = int(__stdcall*)(int job, int body, char sub3950);

// Jetons CP949, en hexadécimal ÉCHAPPÉ — nos sources sont en UTF-8, un littéral
// coréen y serait mal encodé et ne désignerait aucun dossier du GRF.
constexpr const char kSexMale[]   = "\xB3\xB2";  // 남
constexpr const char kSexFemale[] = "\xBF\xA9";  // 여

// `Job_NeedsLuaItemPosOffset` (0x00d9cf80). Le nom vient de son premier usage
// connu (elle déclenche le Lua `OffsetItemPos_GetOffsetForDoram`), mais ce
// qu'elle EST vaut mieux que ce qu'elle sert : un `switch` sur les sept classes
// Doram — 4217..4221, 4308, 4315. Le client s'en sert partout où la race change
// quelque chose. Pas d'argument caché : elle ne lit pas `ecx`.
constexpr uintptr_t kJobIsDoram = 0x00D9CF80;

// Les DEUX dossiers de race, lus DANS le binaire. `Race_GetBodyPrefix6`
// (0x00b44190) ne fait rien d'autre que choisir entre eux et en recopier six
// octets — d'où le nom, et d'où le fait qu'aucun des deux ne soit une chaîne
// terminée du point de vue du client : c'est lui qui pose le NUL après sa
// copie. On copie donc pareil, sans supposer le terminateur.
constexpr uintptr_t kRaceDoramAddr = 0x01088C2C;  // 도람족
constexpr uintptr_t kRaceHumanAddr = 0x01088C34;  // 인간족
constexpr size_t    kRaceLen       = 6;

using JobIsDoramFn = char(__stdcall*)(int job);

// Repli si la lecture échoue (adresse non mappée : jamais vu, mais un chemin
// vide ferait disparaître un personnage entier sans rien dire).
constexpr const char kRaceDoramFallback[] = "\xB5\xB5\xB6\xF7\xC1\xB7";  // 도람족
constexpr const char kRaceHumanFallback[] = "\xC0\xCE\xB0\xA3\xC1\xB7";  // 인간족

}  // namespace

const char* SexFolder(int sex) { return sex ? kSexMale : kSexFemale; }

void BodyPalettePath(int color, int job, char* out, size_t out_size) {
  // 몸 — dossier UNISEXE des palettes de corps. Le client vanilla range les
  // `.pal` par sexe ; le patch WARP `BodyPalUnisex` réécrit le gabarit pour
  // qu'une seule palette serve tout le monde (cf. WARP0716/LastSession.yml).
  //
  // 🔴 Le dossier dépend de la RACE, et les fichiers ne se partagent PAS.
  // `Job_BuildBodyPalettePath_impl` (0x00b42580) a deux gabarits : `몸\…` pour un
  // humain, un préfixe de race pour un Doram — et le patch garde cette
  // distinction. Mesuré le 2026-08-04 sur `summoner_남.spr` : 46 % des pixels
  // opaques d'un corps Doram tombent sur des index que la palette humaine laisse
  // NOIRS. Les deux races n'ont pas la même indexation ; aucun fichier commun
  // n'est possible.
  static constexpr char kBodyPalFolder[] = "\xB8\xF6";  // 몸
  std::snprintf(out, out_size, "data\\palette\\%s\\body_%d.pal",
                IsDoramJob(job) ? RaceFolder(job) : kBodyPalFolder, color);
}

// 🔴 LA RACE SE LIT DANS LE CHEMIN DU SPRITE, et non depuis la classe. C'est ce
// qui permet de servir un AUTRE joueur, dont on n'a pas la classe : le client a
// déjà résolu son sprite, et le dossier de race y est.
// Gabarit : `data\sprite\<race>\몸통\<sexe>\<nom>`.
//
// L'extraction était écrite deux fois — corps et cheveux — à l'identique. Ce qui
// SUIT, en revanche, diffère vraiment : le corps compose un chemin absolu, les
// cheveux un chemin relatif dont la FORME change avec la race. Seule la partie
// commune est partagée ; le reste reste écrit là où il se lit.
//
// `out` reçoit le dossier de race, false si le chemin n'a pas la forme attendue
// (ou si le dossier ne tient pas — 32 octets suffisent largement).
namespace {
bool RaceFolderFromSpritePath(const char* spr, char* out, size_t out_size) {
  const char* p = std::strstr(spr, "sprite\\");
  if (!p) return false;
  p += 7;
  const char* fin = std::strchr(p, '\\');
  if (!fin) return false;
  const size_t n = static_cast<size_t>(fin - p);
  if (n == 0 || n >= out_size) return false;
  std::memcpy(out, p, n);
  out[n] = '\0';
  return true;
}
}  // namespace

bool BodyPalettePathForSprite(const char* spr_base, int color, char* out,
                              size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  if (!spr_base || !*spr_base || color < 0) return false;
  static constexpr char kBodyPalFolder[] = "\xB8\xF6";  // 몸

  char race[32];
  if (!RaceFolderFromSpritePath(spr_base, race, sizeof(race))) return false;

  // Humain (ou race inconnue) : le dossier de palettes UNISEXE `몸`. Doram : son
  // propre dossier, dont l'indexation n'a rien de commun avec l'humaine.
  const bool humain = std::memcmp(race, kRaceHumanFallback, kRaceLen) == 0;
  std::snprintf(out, out_size, "data\\palette\\%s\\body_%d.pal",
                humain ? kBodyPalFolder : race, color);
  return true;
}

bool HairPaletteRelForSprite(const char* head_spr, int color, char* out,
                             size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  if (!head_spr || !*head_spr || color < 0) return false;
  static constexpr char kHairPalFolder[] = "\xB8\xD3\xB8\xAE";  // 머리

  char race[32];
  if (!RaceFolderFromSpritePath(head_spr, race, sizeof(race))) return false;

  // 🔴 Chemin RELATIF à `palette\` : c'est sous cette forme que l'acteur porte
  // le sien, le préfixe de type étant recollé par le chargeur de ressources.
  //
  // ⚠ Les deux races ne partagent AUCUN fichier : mesuré le 2026-08-04, 90 % des
  // pixels opaques d'une tête Doram tombent sur des index que `head_<c>.pal`
  // laisse noirs.
  if (std::memcmp(race, kRaceHumanFallback, kRaceLen) == 0)
    std::snprintf(out, out_size, "%s\\head_%d.pal", kHairPalFolder, color);
  else
    std::snprintf(out, out_size, "%s\\%s\\head_%d.pal", race, kHairPalFolder,
                  color);
  return true;
}

bool IsDoramJob(int job) {
  bool doram = false;
  __try {
    doram = reinterpret_cast<JobIsDoramFn>(kJobIsDoram)(job) == 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { doram = false; }
  return doram;
}

const char* RaceFolder(int job) {
  // Une copie par race, faite une fois : la chaîne est constante pour toute la
  // durée du processus, et la relire à chaque pièce de chaque pantin de chaque
  // image n'apporterait rien.
  static char folder[2][8] = {};
  const int idx = IsDoramJob(job) ? 1 : 0;
  if (folder[idx][0] == '\0') {
    __try {
      std::memcpy(folder[idx],
                  reinterpret_cast<const void*>(idx ? kRaceDoramAddr
                                                    : kRaceHumanAddr),
                  kRaceLen);
      folder[idx][kRaceLen] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) { folder[idx][0] = '\0'; }
  }
  if (folder[idx][0] != '\0') return folder[idx];
  return idx ? kRaceDoramFallback : kRaceHumanFallback;
}

bool BodyResName(int job, int body, char* out, size_t out_size, int* out_job) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  if (out_job) *out_job = job;
  bool ok = false;
  __try {
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

bool BodySpritePath(int job, int body, int sex, char* out, size_t out_size,
                    int* out_job) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  char name[128] = {0};
  int body_job = job;
  if (!BodyResName(job, body, name, sizeof(name), &body_job)) return false;
  if (out_job) *out_job = body_job;

  const char* sex_folder = SexFolder(sex);
  char tail[256];
  // ⚠ `std::snprintf` : le gabarit n'est pas un littéral (il est lu dans le
  // binaire), et la famille sécurisée déclencherait C4774.
  std::snprintf(tail, sizeof(tail), reinterpret_cast<const char*>(kFmtBodyTail),
                sex_folder, name, sex_folder, "spr");
  const size_t n = std::strlen(tail);
  if (n > 4) tail[n - 4] = '\0';  // retire « .spr » : on rend une BASE

  std::snprintf(out, out_size, "data\\sprite\\%s%s", RaceFolder(body_job), tail);
  return out[0] != '\0';
}

uint32_t BodySpriteKey(const char* spr_path) {
  if (!spr_path || !*spr_path) return 0;

  size_t n = std::strlen(spr_path);
  // L'extension ne fait pas partie de l'identité : selon l'appelant, le chemin
  // est une BASE (`...\dragon_knight_남`) ou un fichier (`....spr`). Les deux
  // désignent le même corps et doivent donner la même clé.
  if (n > 4 && (spr_path[n - 4] == '.') &&
      (spr_path[n - 3] == 's' || spr_path[n - 3] == 'S') &&
      (spr_path[n - 2] == 'p' || spr_path[n - 2] == 'P') &&
      (spr_path[n - 1] == 'r' || spr_path[n - 1] == 'R'))
    n -= 4;

  // FNV-1a 32 bits. Choisie pour être trivialement réimplémentable ailleurs — la
  // clé voyage sur le réseau et se range en base, donc elle survivra à ce code.
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = static_cast<unsigned char>(spr_path[i]);
    if (c == '/') c = '\\';                    // le client mélange les deux
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';  // 🔴 ASCII SEUL : cf. le .h
    h ^= c;
    h *= 16777619u;
  }
  // 0 est la sentinelle « corps inconnu » : on la déplace plutôt que de la
  // rendre par accident sur un chemin parfaitement valide.
  return h ? h : 1u;
}

}  // namespace ro
