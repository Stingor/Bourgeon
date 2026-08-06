#include "ui/game_emotes.h"

#include <Windows.h>  // SEH autour de la lecture du binaire

#include <cstdint>
#include <cstring>

#include "ui/sprite_view.h"

namespace ro {
namespace emote {
namespace {

// Le gabarit du CLIENT, `이팩트\emotion.spr` — lu dans le binaire plutôt que
// recopié, comme tous les chemins de sprite de la maison. C'est celui qu'utilise
// `ActorEffect_CreateChildSprite` (0x00c6ff20) pour les bulles au-dessus des
// têtes, et le lexer `^e[]` de TextLayout pour les emotes dans du texte : une
// seule ressource, deux usages, et donc la garantie que nos index sont ceux du
// jeu.
constexpr uintptr_t kEmotionPathAddr = 0x010277B8;
constexpr size_t    kEmotionPathLen  = 19;  // « 이팩트\emotion.spr » + NUL

// Repli si l'adresse ne répond pas. En hexadécimal ÉCHAPPÉ : nos sources sont en
// UTF-8, un littéral coréen n'y désignerait aucun dossier du GRF.
constexpr const char kEmotionFallback[] = "\xC0\xCC\xC6\xD1\xC6\xAE\\emotion";

// 🔴 `LoadSprite` court-circuite les DEUX préfixes que le client empile
// (`sprite\` selon l'extension, puis `data\`) : on les pose donc nous-mêmes, et
// l'extension est ajoutée par le chargeur.
constexpr const char kVfsPrefix[] = "data\\sprite\\";

// Table des noms des emotes, dans l'ordre des ACTIONS de `emotion.act`.
//
// 🔴 CE N'EST PAS L'ORDRE DU PROTOCOLE, et la nuance a un nom : `ET_CHAT_PROHIBIT`
// (34 chez rAthena) n'a AUCUNE action dans le sprite. Ce n'est pas une bulle mais
// l'icône « ce joueur est muet », que le client affiche par un autre chemin — le
// serveur refuse d'ailleurs de la relayer (`clif_parse_Emotion` la rejette avec
// un skill_fail). Le fichier ne lui réserve donc pas de case, et TOUT ce qui la
// suit dans l'énumération occupe ici l'action précédente :
//
//     protocole 33 `ok`             -> action 33
//     protocole 34 `chat_prohibit`  -> AUCUNE action
//     protocole 35 `indonesia_flag` -> action 34
//     …                                … et ainsi jusqu'à la fin
//
// Vérifié en jeu : la table alignée sur l'énumération décalait toutes les emotes
// au-delà de la 33 d'un cran, chacune affichant sa voisine.
//
// ⚠ CONSÉQUENCE POUR PLUS TARD : cet index N'EST PAS celui à envoyer dans un
// `CZ_REQ_EMOTION`. Le jour où l'on voudra déclencher une bulle au-dessus de la
// tête depuis ce module, il faudra rajouter 1 au-delà de 33.
//
// ── EXCLURE UNE EMOTE ────────────────────────────────────────────────────────
// Remplacer son nom par `nullptr` — ne PAS supprimer la ligne, ce qui décalerait
// tout ce qui suit. L'entrée garde sa place et l'emote disparaît de partout d'un
// coup : absente de la grille, `:nom:` ne la reconnaît plus, `Name` ne la rend
// plus.
const char* const kNames[] = {
    "surprise", "question", "delight",   "throb",          "sweat",
    "aha",      "fret",     "anger",     "money",          "think",
    "scissor",  "rock",     "wrap",      "flag",           "bigthrob",
    "thanks",   "kek",      "sorry",     "smile",          "profusely_sweat",
    "scratch",  "best",     "stare_about", "huk",          "o",
    "x",        "help",     "go",        "cry",            "kik",
    "chup",     "chupchup", "hng",       "ok",             "indonesia_flag",
    "stare",    "hungry",   "cool",      "merong",
    "shy",      "goodboy",  "sptime",    "sexy",           "comeon",
    "sleepy",   "congratulation", "hptime", "ph_flag",     "my_flag",
    "si_flag",  "br_flag",  "spark",     "confuse",        "ohno",
    "hum",      "blabla",   "otl",       nullptr,          nullptr,
    nullptr,    nullptr,    nullptr,     nullptr,          "india_flag",
    "luv",      "flag8",    "flag9",     "mobile",         "mail",
    "antenna0", "antenna1", "antenna2",  "antenna3",       "hum2",
    "abs",      "oops",     "spit",      "ene",            "panic",
    nullptr,    nullptr,     nullptr,      nullptr,        nullptr,
    nullptr,    nullptr,     nullptr,      nullptr,        nullptr,
    nullptr,    nullptr,     nullptr,
};
constexpr int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));

// Garde-fou contre le seul geste qui casse tout en silence : retirer une ligne
// au lieu d'y mettre `nullptr`. Le compilateur le dit tout de suite, là où le
// jeu se serait contenté d'afficher chaque emote à la place de sa voisine.
// Ce nombre ne change que si le PROTOCOLE gagne des emotes — et alors on ajoute
// à la FIN, jamais au milieu.
static_assert(kCount == 92, "Table d'emotes desalignee : pour EXCLURE une emote "
                            "mettre nullptr, ne pas supprimer la ligne");

// Le sprite, chargé une seule fois pour tout le processus : c'est un fichier de
// quelques centaines de kio partagé par la grille et par chaque ligne de chat.
SpriteRes g_res;

// Recompose le chemin VFS complet à partir du gabarit du client.
const char* VfsBase() {
  static char path[128] = {};
  if (path[0] != '\0') return path;

  char tail[64] = {};
  bool read_ok = false;
  __try {
    std::memcpy(tail, reinterpret_cast<const void*>(kEmotionPathAddr),
                kEmotionPathLen);
    tail[kEmotionPathLen] = '\0';
    read_ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { read_ok = false; }

  if (!read_ok || tail[0] == '\0')
    std::strcpy(tail, kEmotionFallback);

  // L'extension est l'affaire du chargeur : il essaiera `.spr` puis `.act`.
  char* dot = std::strrchr(tail, '.');
  if (dot != nullptr) *dot = '\0';

  std::strcpy(path, kVfsPrefix);
  std::strcat(path, tail);
  return path;
}

// Charge à la première demande. `LoadSprite` est idempotent une fois la poignée
// posée sur ce chemin, et retient lui-même les échecs (`SpriteRes::failed`) : on
// peut donc l'appeler à chaque frame sans garde de notre côté.
//
// 🔴 Et il FAUT l'appeler à chaque fois plutôt que de mémoriser « déjà tenté » :
// la chatbox vit avant que le VFS ne soit prêt (écran de sélection), et un essai
// unique raté à cet instant-là aurait éteint les emotes pour toute la session.
const SpriteRes& Res() {
  LoadSprite(VfsBase(), &g_res);
  return g_res;
}

}  // namespace

int Count() { return kCount; }

const char* Name(int id) {
  if (id < 0 || id >= kCount) return nullptr;
  return kNames[id];
}

int Find(const char* name, size_t len) {
  if (name == nullptr || len == 0) return -1;
  for (int i = 0; i < kCount; ++i) {
    if (kNames[i] == nullptr) continue;  // exclue
    if (std::strlen(kNames[i]) == len &&
        std::memcmp(kNames[i], name, len) == 0)
      return i;
  }
  return -1;
}

bool Exists(int id) {
  if (id < 0 || id >= kCount) return false;
  if (kNames[id] == nullptr) return false;  // exclue : ni proposée, ni rendue
  const SpriteRes& res = Res();
  if (res.res == nullptr) return false;
  // L'action doit exister ET porter au moins une image : un .act peut déclarer
  // une action vide, et la dessiner ne produirait qu'un trou.
  return id < SpriteActionCount(res) &&
         SpriteActionFrameCount(res, static_cast<unsigned>(id)) > 0;
}

bool Draw(ImDrawList* draw_list, int id, ImVec2 rect_min, ImVec2 rect_max,
          float anim_seconds, bool allow_upscale) {
  if (draw_list == nullptr || !Exists(id)) return false;
  return DrawSprite(draw_list, Res(), rect_min, rect_max, anim_seconds,
                    static_cast<unsigned>(id), 130.0f, allow_upscale);
}

}  // namespace emote
}  // namespace ro
