#include "ui/game_emotes.h"

#include <Windows.h>  // SEH autour de la lecture du binaire

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ui/spr_act.h"      // pixels CPU (l'export ne peut pas lire le GPU)
#include "ui/sprite_view.h"
#include "utils/gif_writer.h"

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
// 🔴 ET LE DÉCALAGE COMMENCE PLUS TÔT QUE ÇA. Le sprite porte une action de plus
// que l'énumération dès le DÉBUT : le serveur numérote `ET_DELIGHT = 2`, le
// fichier met là un sifflement et ne place `delight` qu'en 3 (relevé en jeu,
// 2026-08-07). L'énumération du serveur n'est donc PAS la source de vérité de
// l'ordre — elle n'est qu'une bonne façon de nommer. Ce qui fait autorité, c'est
// ce que le sprite montre à l'écran, et la seule vérification qui vaille est de
// dérouler la grille en lisant les infobulles.
//
// Ce n'est pas grave, et c'est la raison pour laquelle ça ne l'est pas : cet
// index ne sert QU'À DESSINER. Ce qui voyage — dans le chat, jusqu'à Discord —
// c'est le NOM. Tant que nom et image se correspondent ici, la chaîne entière est
// juste, quel que soit le numéro que le protocole aurait mis en face.
//
// ⚠ Renommer une entrée casse en revanche le pont Discord : les GIF déposés sur
// le site portent l'ANCIEN nom, et le relais ne résout `:nom:` en image que si le
// fichier existe (sinon il laisse le texte). Renommer = ré-exporter.
//
// ── EXCLURE UNE EMOTE ────────────────────────────────────────────────────────
// Remplacer son nom par `nullptr` — ne PAS supprimer la ligne, ce qui décalerait
// tout ce qui suit. L'entrée garde sa place et l'emote disparaît de partout d'un
// coup : absente de la grille, `:nom:` ne la reconnaît plus, `Name` ne la rend
// plus.
const char* const kNames[] = {
    "surprise", "question", "whistle",   "delight",        "throb",
    "sweat",    "aha",      "fret",      "anger",          "money",
    "think",    "scissor",  "rock",      "wrap",           "flag",
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
};
constexpr int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));

// Garde-fou contre le seul geste qui casse tout en silence : retirer une ligne
// au lieu d'y mettre `nullptr`. Le compilateur le dit tout de suite, là où le
// jeu se serait contenté d'afficher chaque emote à la place de sa voisine.
// Ce nombre ne change que si le PROTOCOLE gagne des emotes — et alors on ajoute
// à la FIN, jamais au milieu.
static_assert(kCount == 79, "Table d'emotes desalignee : pour EXCLURE une emote "
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

// ── Export ───────────────────────────────────────────────────────────────────
namespace {

// Rectangle englobant TOUTE l'action, calques compris.
//
// 🔴 Une boîte par image ferait sautiller l'emote : chaque image serait recadrée
// au plus juste, donc recentrée, et l'animation deviendrait une gigue. Le GIF
// veut de toute façon une taille unique pour toutes ses images.
struct Box { float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; };

void GrowToLayer(const spract::Resource& res, const spract::Layer& layer,
                 Box* box) {
  const spract::Image* img = res.Get(layer.index, layer.type);
  if (img == nullptr || img->w <= 0 || img->h <= 0) return;
  const float w = img->w * std::fabs(layer.scale_x);
  const float h = img->h * std::fabs(layer.scale_y);
  box->x0 = std::min(box->x0, layer.off_x - w * 0.5f);
  box->y0 = std::min(box->y0, layer.off_y - h * 0.5f);
  box->x1 = std::max(box->x1, layer.off_x + w * 0.5f);
  box->y1 = std::max(box->y1, layer.off_y + h * 0.5f);
}

// Compose un calque dans le canevas, au plus proche voisin. La rotation n'est
// pas appliquée : aucune emote n'en déclare, et un échantillonnage tourné
// ajouterait de l'escalier là où le pixel art doit rester net.
void BlitLayer(const spract::Resource& res, const spract::Layer& layer,
               float ox, float oy, int cw, int ch, uint32_t* canvas) {
  const spract::Image* img = res.Get(layer.index, layer.type);
  if (img == nullptr || img->w <= 0 || img->h <= 0) return;
  const float sw = img->w * std::fabs(layer.scale_x);
  const float sh = img->h * std::fabs(layer.scale_y);
  if (sw < 1.0f || sh < 1.0f) return;

  const int dx0 = static_cast<int>(std::lround(layer.off_x - sw * 0.5f - ox));
  const int dy0 = static_cast<int>(std::lround(layer.off_y - sh * 0.5f - oy));
  const int dw  = static_cast<int>(std::lround(sw));
  const int dh  = static_cast<int>(std::lround(sh));

  for (int y = 0; y < dh; ++y) {
    const int dy = dy0 + y;
    if (dy < 0 || dy >= ch) continue;
    int sy = static_cast<int>(static_cast<int64_t>(y) * img->h / dh);
    sy = std::min(std::max(sy, 0), img->h - 1);
    for (int x = 0; x < dw; ++x) {
      const int dx = dx0 + x;
      if (dx < 0 || dx >= cw) continue;
      int sx = static_cast<int>(static_cast<int64_t>(x) * img->w / dw);
      sx = std::min(std::max(sx, 0), img->w - 1);
      if (layer.mirror) sx = img->w - 1 - sx;
      const uint32_t src = img->argb[static_cast<size_t>(sy) * img->w + sx];
      const uint32_t a   = src >> 24;
      if (a == 0) continue;
      uint32_t& dst = canvas[static_cast<size_t>(dy) * cw + dx];
      if (a == 255 || (dst >> 24) == 0) {
        dst = src;
        continue;
      }
      // Mélange linéaire pondéré par l'alpha du calque du dessus. Volontairement
      // plus simple qu'un « over » exact : les .spr palettisés ont un alpha
      // BINAIRE, et `GifWrite` reseuille de toute façon à 128 — la différence ne
      // serait visible nulle part.
      const uint32_t ia = 255 - a;
      const auto mix = [&](int shift) -> uint32_t {
        const uint32_t s = (src >> shift) & 0xFF;
        const uint32_t d = (dst >> shift) & 0xFF;
        return (s * a + d * ia) / 255;
      };
      const uint32_t out_a = std::max(a, dst >> 24);
      dst = (out_a << 24) | (mix(16) << 16) | (mix(8) << 8) | mix(0);
    }
  }
}

}  // namespace

int ExportGifs(const char* out_dir, int scale) {
  if (out_dir == nullptr || out_dir[0] == '\0') return -1;
  scale = std::min(std::max(scale, 1), 8);

  // 🔴 Le sprite est RELU ici. Le cache de rendu ne garde que des textures GPU,
  // et un GIF se fabrique avec des pixels — les redescendre du device serait un
  // détour coûteux pour un résultat moins fidèle.
  const std::string base = VfsBase();
  spract::Resource   res;
  if (!spract::Load((base + ".spr").c_str(), (base + ".act").c_str(), &res) ||
      !res.ok)
    return -1;

  CreateDirectoryA(out_dir, nullptr);

  int written = 0;
  for (int id = 0; id < kCount; ++id) {
    if (kNames[id] == nullptr) continue;  // exclue
    if (id >= static_cast<int>(res.actions.size())) continue;
    const spract::Action& action = res.actions[id];
    if (action.frames.empty()) continue;

    Box box;
    for (const spract::Frame& frame : action.frames)
      for (const spract::Layer& layer : frame.layers)
        GrowToLayer(res, layer, &box);
    if (box.x1 <= box.x0 || box.y1 <= box.y0) continue;  // action vide

    const int cw = static_cast<int>(std::lround(box.x1 - box.x0));
    const int ch = static_cast<int>(std::lround(box.y1 - box.y0));
    if (cw <= 0 || ch <= 0 || cw > 512 || ch > 512) continue;

    // Toutes les images du GIF sont montées d'abord : l'encodeur les veut
    // ensemble pour choisir sa palette.
    std::vector<std::vector<uint32_t>> pixels;
    std::vector<const uint32_t*>       ptrs;
    pixels.reserve(action.frames.size());
    ptrs.reserve(action.frames.size());
    for (const spract::Frame& frame : action.frames) {
      std::vector<uint32_t> canvas(static_cast<size_t>(cw) * ch, 0u);
      for (const spract::Layer& layer : frame.layers)
        BlitLayer(res, layer, box.x0, box.y0, cw, ch, canvas.data());
      if (scale > 1) {
        std::vector<uint32_t> big(static_cast<size_t>(cw) * scale * ch * scale);
        for (int y = 0; y < ch * scale; ++y)
          for (int x = 0; x < cw * scale; ++x)
            big[static_cast<size_t>(y) * cw * scale + x] =
                canvas[static_cast<size_t>(y / scale) * cw + (x / scale)];
        canvas.swap(big);
      }
      pixels.push_back(std::move(canvas));
    }
    for (const std::vector<uint32_t>& frame : pixels) ptrs.push_back(frame.data());

    // Cadence du .act : `speed` est en ticks RO de 25 ms, le GIF compte en
    // centièmes. Plancher à 2 cs — en dessous, les navigateurs imposent leur
    // propre minimum et l'animation ralentit au lieu d'accélérer.
    int delay_cs = static_cast<int>(std::lround(action.speed * 25.0f / 10.0f));
    delay_cs = std::min(std::max(delay_cs, 2), 200);

    const std::string path =
        std::string(out_dir) + "\\" + kNames[id] + ".gif";
    if (GifWrite(path.c_str(), ptrs.data(), cw * scale, ch * scale,
                 static_cast<int>(ptrs.size()), delay_cs))
      ++written;
  }
  return written;
}

}  // namespace emote
}  // namespace ro
