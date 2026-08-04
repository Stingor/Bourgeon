#include "ui/sprite_path.h"

#include <Windows.h>  // SEH autour des lectures du binaire

#include <cstdint>
#include <cstring>

namespace ro {
namespace {

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

}  // namespace ro
