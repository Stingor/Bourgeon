#include "features/fx/palette_base.h"

#include <cstring>
#include <deque>
#include <map>
#include <string>

#include "features/fx/palette_inject.h"
#include "ui/spr_act.h"
#include "ui/sprite_path.h"  // BodyPalettePathForSprite
#include "utils/i18n.h"

namespace fx {
namespace palette_base {

const char* StatusText(Status s) {
  switch (s) {
    case kNoSprite:   return i18n::Tr("Corps introuvable.");
    case kUnreadable: return i18n::Tr("Sprite du corps illisible.");
    case kNoPalette:
      return i18n::Tr("Ce corps n'a pas de palette : il n'est pas recolorable.");
    case kNoRamp:     return i18n::Tr("Aucun dégradé détecté sur ce corps.");
    default:          return "";
  }
}

bool OfficialPalettePath(uint32_t gid, int palette_id, char* out,
                         size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  // 1..553 : voir `kOfficialPaletteMax`. 0 n'est pas une palette, c'est
  // l'absence de palette.
  if (palette_id < 1 || palette_id > kOfficialPaletteMax) return false;

  // 🔴 On dérive du chemin du SPRITE, et non du chemin natif de palette.
  //
  // Réécrire le numéro dans le chemin natif semblait plus simple, et c'était un
  // piège : un personnage dont la couleur de vêtement vaut 0 n'a AUCUN chemin
  // natif — le client ne lui charge pas de palette externe. La résolution
  // échouait donc en silence, et choisir une palette ne changeait rien du tout.
  // Le sprite, lui, existe toujours et porte le dossier de race.
  char spr[300];
  if (!palette_inject::ActorBodySpritePath(gid, spr, sizeof(spr))) return false;
  return ro::BodyPalettePathForSprite(spr, palette_id, out, out_size);
}

Status BuildFromSpritePath(const char* spr_base, uint32_t gid, Body* out) {
  // Le chemin de la palette de vêtement, tel que le NATIF l'avait choisi pour
  // cet acteur — capturé par le détour avant qu'on n'écrive le nôtre.
  char natif[128];
  if (!palette_inject::NativePalettePath(gid, natif, sizeof(natif)))
    return BuildFromPaths(spr_base, nullptr, out);
  const std::string pal_path = std::string("data\\palette\\") + natif;
  return BuildFromPaths(spr_base, pal_path.c_str(), out);
}

namespace {

// ── Cache des bases construites, par couple (sprite, palette) ────────────────
//
// 🔴 Une construction coûte l'analyse COMPLÈTE d'un `.spr` : une centaine
// d'images décodées, puis un comptage sur chacun de leurs pixels. C'est de loin
// la dépense de ce module — et elle se répétait à l'identique pour CHAQUE
// acteur, alors que dans une ville vingt joueurs se partagent une poignée de
// classes et de couleurs de vêtement.
//
// Le symptôme : à la connexion, les joueurs ayant un style apparaissaient une
// seconde dans leurs couleurs d'origine avant que la recette ne se pose. Le
// serveur n'y était pour rien — il envoie le style au moment même où le joueur
// entre en vue. C'est la boucle d'application qui, ne s'accordant que deux
// constructions par battement, mettait N/2 battements à traiter N joueurs.
//
// ⚠ Le contenu ne dépend QUE des deux chemins, et ce sont deux FICHIERS : ils ne
// changent pas en cours de session, donc rien à invalider. Les échecs sont mis
// en cache eux aussi — un fichier absent le reste, et le relire cinq fois pour
// aboutir au même « non » ne sert personne.
constexpr size_t kMaxCache = 64;

struct Cached {
  Status status = kNoSprite;
  Body body;
};
std::map<std::string, Cached> g_cache;
std::deque<std::string> g_cache_order;  // ordre d'arrivée, pour évincer

Status BuildUncached(const char* spr_base, const char* pal_path, Body* out);

}  // namespace

Status BuildFromPaths(const char* spr_base, const char* pal_path, Body* out) {
  if (!out) return kNoSprite;
  if (!spr_base || !*spr_base) return BuildUncached(spr_base, pal_path, out);

  std::string key = spr_base;
  key += '|';
  if (pal_path && *pal_path) key += pal_path;

  auto it = g_cache.find(key);
  if (it != g_cache.end()) {
    *out = it->second.body;
    out->cached = true;
    return it->second.status;
  }

  const Status st = BuildUncached(spr_base, pal_path, out);
  out->cached = false;

  // Éviction du plus ancien. Une file d'arrivée plutôt qu'un vrai LRU : sur une
  // carte, l'ensemble des apparences visibles tient largement sous le plafond,
  // et un LRU coûterait un remaniement à chaque lecture pour un gain nul.
  Cached& e = g_cache[key];
  e.status = st;
  e.body = *out;
  e.body.cached = true;  // toute relecture de cette entrée EST un succès de cache
  g_cache_order.push_back(key);
  while (g_cache_order.size() > kMaxCache) {
    g_cache.erase(g_cache_order.front());
    g_cache_order.pop_front();
  }
  return st;
}

namespace {

Status BuildUncached(const char* spr_base, const char* pal_path, Body* out) {
  if (!out) return kNoSprite;
  out->spr_path.clear();
  out->base.clear();
  out->spr_palette.clear();
  out->ramp_count = 0;
  out->pixels_total = 0;
  out->pixels_covered = 0;
  out->cached = false;
  if (!spr_base || !*spr_base) return kNoSprite;
  out->spr_path = spr_base;

  // Le chemin est une BASE sans extension — c'est le parseur qui veut le
  // fichier.
  const std::string spr = out->spr_path + ".spr";
  std::vector<uint8_t> bytes;
  if (!ro::spract::ReadFile(spr.c_str(), &bytes)) return kUnreadable;

  ro::spract::Resource res;
  if (!ro::spract::ParseSpr(bytes.data(), bytes.size(), &res) ||
      res.palette.size() < 1024) {
    // Un .spr entièrement Bgra32 (v3.2) n'a PAS de palette : il porte ses
    // couleurs par pixel, et rien n'y est recolorable.
    return kNoPalette;
  }

  // ── Le serveur PAR-DESSUS le sprite ────────────────────────────────────────
  // Ni l'une ni l'autre ne suffit. Celle du serveur porte la couleur de vêtement
  // que tout le monde voit déjà, mais laisse la moitié de ses index vides — d'où
  // les corps de 4e classe en silhouette noire. Celle du sprite est complète
  // mais ignore cette couleur.
  std::vector<uint8_t> server;
  if (pal_path && *pal_path) {
    std::vector<uint8_t> raw;
    if (ro::spract::ReadFile(pal_path, &raw) && raw.size() >= 1024) {
      // 🔴 Les 1024 PREMIERS octets : les .pal de 1028 portent une QUEUE de
      // quatre zéros, pas un en-tête. Lire par la fin décalerait tout d'un
      // index.
      server.assign(raw.begin(), raw.begin() + 1024);
    }
  }

  out->spr_palette.assign(res.palette.begin(), res.palette.begin() + 1024);

  out->base.assign(1024, 0);
  ro::MergeServerPalette(res.palette.data(), res.palette.size(),
                         server.empty() ? nullptr : server.data(),
                         server.size(), out->base.data(), out->base.size());

  // Les rampes se détectent sur la base FUSIONNÉE : ce sont les couleurs que le
  // joueur va effectivement régler, pas celles du fichier de sprite.
  int usage[256];
  ro::CountIndexUsage(res, usage);
  out->ramp_count = ro::DetectRamps(out->base.data(), out->base.size(), usage,
                                    out->ramps, ro::kMaxRamps);

  // Ce que la fusion a réparé, en pixels. Même seuil de « noir » que
  // `MergeServerPalette` : 24 sur 765, soit ce qui est indistinguable du noir.
  const auto est_noir = [](const uint8_t* pal, int i) {
    return pal[4 * i] + pal[4 * i + 1] + pal[4 * i + 2] <= 24;
  };
  for (int i = 1; i < 256; ++i) {
    if (usage[i] == 0) continue;
    // Sans palette de serveur, le sprite fait foi : il n'y a rien à réparer.
    if (!server.empty() && est_noir(server.data(), i))
      out->pixels_black_native += usage[i];
    if (est_noir(out->base.data(), i)) out->pixels_black_merged += usage[i];
  }

  for (int i = 0; i < 256; ++i) out->pixels_total += usage[i];
  for (int r = 0; r < out->ramp_count; ++r) {
    const int begin = out->ramps[r].start;
    const int end = begin + out->ramps[r].length;
    for (int i = begin; i < end && i < 256; ++i) out->pixels_covered += usage[i];
  }

  return out->ramp_count ? kOk : kNoRamp;
}

}  // namespace

Status BuildForGid(uint32_t gid, Body* out) {
  char spr[300];
  if (!palette_inject::ActorBodySpritePath(gid, spr, sizeof(spr)))
    return kNoSprite;
  return BuildFromSpritePath(spr, gid, out);
}

Status BuildForGid(uint32_t gid, int palette_id, Body* out) {
  if (palette_id < 0) return BuildForGid(gid, out);
  char spr[300];
  if (!palette_inject::ActorBodySpritePath(gid, spr, sizeof(spr)))
    return kNoSprite;
  char pal[160];
  // Palette demandée introuvable (numéro hors bornes, acteur pas encore vu) :
  // on retombe sur celle du serveur plutôt que sur aucune — un corps sans
  // palette externe perdrait la couleur de vêtement du joueur.
  if (!OfficialPalettePath(gid, palette_id, pal, sizeof(pal)))
    return BuildFromSpritePath(spr, gid, out);
  return BuildFromPaths(spr, pal, out);
}

}  // namespace palette_base
}  // namespace fx
