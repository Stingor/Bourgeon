#include "ui/sprite_view.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / ReleaseTexture / DeviceEpoch
#include "ui/spr_act.h"
#include "utils/log_console.h"

namespace ro {
namespace {

// Intervalle d'une image pour une cadence déclarée en « ticks » RO de 25 ms —
// même conversion que `Action.AnimationSpeed * 25` de GRF Editor.
constexpr float kActDelayTickMs = 25.0f;

// ── Cache des ressources, par CHEMIN ─────────────────────────────────────────
//
// Une entrée porte les fichiers parsés, les textures téléversées PARESSEUSEMENT
// (une vue n'affiche qu'une action : inutile de monter en VRAM les 8 directions
// × 5 actions du .spr) et la boîte englobante mémoïsée par action.
// Une TEINTE de l'entrée : la palette et les textures des images palettisées
// vues sous cette palette. Le slot 0 existe toujours et porte la palette
// d'origine du .spr (`path` vide, `pal` inutilisé).
//
// 🔴 C'est ici, et pas dans la clé du cache, que vit la couleur : les images
// décodées et le .act sont partagés par toutes les teintes du même sprite. Une
// clé par (chemin, palette) ferait ré-analyser le .spr pour chaque couleur de
// cheveux d'une liste de guilde.
struct PaletteSet {
  std::string        path;      // vide = palette d'origine du .spr
  uint32_t           pal[256];  // valide seulement si `path` non vide
  std::vector<void*> tex;       // textures des images INDEXÉES sous cette palette
};

struct Entry {
  std::string base;               // chemin du .spr sans extension, pour se recharger seul
  // Base du .act quand elle DIFFÈRE de celle du .spr. Vide = les deux fichiers
  // partagent le même chemin, ce qui est le cas général.
  //
  // 🔴 Le cas qui l'impose : la CAPE. `Job_BuildGarmentSpritePath_impl`
  // (0x00b442f0) essaie pour le .spr une disposition « plate »
  // (`로브\<robe>\<robe>.spr`) avant celle par job, mais prend TOUJOURS la
  // disposition par job pour le .act. Une cape peut donc légitimement apparier
  // un .spr partagé à un .act propre à la classe.
  std::string act_base;
  // Clé dans `g_cache`, et seule chose à mettre dans `g_cache_order`. Elle vaut
  // `base` tant qu'il n'y a pas d'appariement, et `base|act_base` sinon : deux
  // appariements du même .spr sont deux entrées distinctes.
  std::string key;
  spract::Resource res;
  std::vector<PaletteSet> pals;   // [0] = palette d'origine, toujours présent
  std::vector<void*> tex_bgra;    // sans palette : partagé par toutes les teintes
  unsigned epoch = 0;
  size_t   bytes = 0;             // pixels décodés retenus en RAM
  bool     loaded = false;        // les pixels sont-ils là ? (voir Unload)
  bool     failed = false;        // fichiers absents : ne pas réessayer en boucle

  int   box_action = -1;
  float box_min_x = 0, box_min_y = 0, box_max_x = 0, box_max_y = 0;
  bool  box_valid = false;
};

// 🔴 Les entrées ne sont JAMAIS retirées de cette table, et c'est délibéré : les
// appelants gardent une `SpriteRes` d'une image à l'autre, parfois d'une session
// de fenêtre à l'autre. Détruire une entrée sous leurs pieds leur laisserait un
// pointeur pendant — un use-after-free, pas un simple sprite manquant.
//
// Ce qui est récupéré, c'est le POIDS : `Unload` rend les pixels et les textures
// mais laisse l'entrée vivante, et `EnsureLoaded` la ré-analyse au prochain
// usage. Ce qui reste est une coquille de quelques dizaines d'octets par sprite
// jamais revu. Les pointeurs sont stables parce que la table possède des
// `unique_ptr` : réhachage ou pas, l'objet ne bouge pas.
std::unordered_map<std::string, std::unique_ptr<Entry>> g_cache;
std::deque<std::string> g_cache_order;  // entrées CHARGÉES, du plus ancien au plus récent
size_t g_cache_bytes = 0;

// ⚠ Le vrai garde-fou est le nombre d'OCTETS, pas celui des entrées. Les deux
// extrêmes existent : une tête de personnage fait quelques kilo-octets, un grand
// monstre plusieurs méga-octets — un plafond en nombre seul serait soit ruineux
// en RAM, soit absurdement bas pour des sprites minuscules.
//
// Le compte ne sert donc que de borne de bon sens, et il est réglé large exprès :
// la grille de coiffures du char-select en affiche 80, une liste de guilde une
// vingtaine. Trop bas, il ferait re-analyser des .spr pendant un simple
// défilement.
constexpr size_t kMaxCached   = 128;
constexpr size_t kCacheBudget = 48u * 1024u * 1024u;

size_t DecodedBytes(const spract::Resource& r) {
  size_t n = 0;
  for (const spract::Image& i : r.indexed) n += i.argb.size() * 4 + i.index.size();
  for (const spract::Image& i : r.bgra)    n += i.argb.size() * 4;
  return n;
}

void ReleaseTextures(Entry* e) {
  for (PaletteSet& p : e->pals)
    for (void* t : p.tex) Overlay_ReleaseTexture(t);
  for (void* t : e->tex_bgra) Overlay_ReleaseTexture(t);
  for (PaletteSet& p : e->pals) p.tex.clear();
  e->tex_bgra.clear();
}

// Texture d'une image sous une teinte donnée, créée au premier besoin.
void* TextureFor(Entry* e, int pal_slot, int index, int type) {
  const unsigned epoch = Overlay_DeviceEpoch();
  if (epoch != e->epoch) {
    // ⚠ On LÂCHE sans Release : ces handles appartiennent à un device qui
    // n'existe plus (cf. [[feedback_texture_cache_device_epoch]]).
    for (PaletteSet& p : e->pals) p.tex.clear();
    e->tex_bgra.clear();
    e->epoch = epoch;
  }
  const std::vector<spract::Image>& imgs =
      (type == 0) ? e->res.indexed : e->res.bgra;
  if (index < 0 || static_cast<size_t>(index) >= imgs.size()) return nullptr;
  const spract::Image& img = imgs[index];

  // Les Bgra32 portent leurs couleurs en propre : aucune palette ne s'y
  // applique, leurs textures sont partagées par toutes les teintes.
  std::vector<void*>* tex = &e->tex_bgra;
  if (type == 0) {
    if (pal_slot < 0 || static_cast<size_t>(pal_slot) >= e->pals.size())
      pal_slot = 0;
    tex = &e->pals[pal_slot].tex;
  }
  if (tex->size() != imgs.size()) tex->assign(imgs.size(), nullptr);
  if ((*tex)[index]) return (*tex)[index];
  if (img.w <= 0 || img.h <= 0 || img.argb.empty()) return nullptr;

  if (type == 0 && pal_slot > 0) {
    std::vector<uint32_t> recolored;
    if (spract::RecolorIndexed(img, e->pals[pal_slot].pal, &recolored))
      (*tex)[index] =
          Overlay_CreateTextureARGB(recolored.data(), img.w, img.h);
  }
  // Repli (et cas nominal du slot 0) : les pixels tels que le .spr les donne.
  if (!(*tex)[index])
    (*tex)[index] = Overlay_CreateTextureARGB(img.argb.data(), img.w, img.h);
  return (*tex)[index];
}

// Slot de teinte pour ce .pal, créé au premier besoin. Rend 0 (palette
// d'origine) quand le fichier manque ou n'est pas lisible : une couleur de
// cheveux absente ne doit pas faire disparaître la tête.
int PaletteSlot(Entry* e, const char* pal_path) {
  if (!pal_path || !*pal_path) return 0;
  for (size_t i = 0; i < e->pals.size(); ++i)
    if (e->pals[i].path == pal_path) return static_cast<int>(i);

  std::vector<uint8_t> bytes;
  if (!spract::ReadFile(pal_path, &bytes)) return 0;
  PaletteSet set;
  if (!spract::DecodePalette(bytes.data(), bytes.size(), set.pal)) return 0;
  set.path = pal_path;
  e->pals.push_back(std::move(set));
  return static_cast<int>(e->pals.size() - 1);
}

// Rend les pixels et les textures d'une entrée, mais PAS l'entrée : les
// poignées que les appelants gardent restent valides et la ré-analyse aura lieu
// toute seule au prochain usage.
//
// Les teintes (`pals`) sont conservées : un kilo-octet par couleur, et les
// retrouver coûterait une relecture de .pal par ligne d'une liste de guilde.
void Unload(Entry* e) {
  if (!e->loaded) return;
  // Le device est vivant ici (on est appelé depuis un chargement) : les textures
  // se libèrent pour de bon, sinon parcourir beaucoup de sprites laisserait une
  // texture en VRAM par image jamais revue.
  ReleaseTextures(e);
  e->res = spract::Resource{};
  g_cache_bytes -= e->bytes;
  e->bytes = 0;
  e->box_valid = false;
  e->loaded = false;
}

// Décharge les plus anciennes entrées tant que le cache dépasse ses bornes.
// `incoming` est le poids de celle qu'on s'apprête à charger.
void TrimFor(size_t incoming) {
  while (!g_cache_order.empty() &&
         (g_cache_order.size() >= kMaxCached ||
          g_cache_bytes + incoming > kCacheBudget)) {
    const std::string victim = g_cache_order.front();
    g_cache_order.pop_front();
    auto vit = g_cache.find(victim);
    if (vit != g_cache.end()) Unload(vit->second.get());
  }
}

// (Re)charge les fichiers d'une entrée. Appelée au premier usage comme après un
// déchargement — les deux cas sont le même code.
bool LoadInto(Entry* e) {
  char spr_path[352], act_path[352];
  std::snprintf(spr_path, sizeof(spr_path), "%s.spr", e->base.c_str());
  std::snprintf(act_path, sizeof(act_path), "%s.act",
                e->act_base.empty() ? e->base.c_str() : e->act_base.c_str());

  spract::Resource res;
  if (!spract::Load(spr_path, act_path, &res)) {
    // Fichiers absents : c'est définitif pour ce chemin. Sans cette marque, un
    // appelant qui redemande à chaque image (les vignettes de tête le font)
    // relancerait une recherche VFS soixante fois par seconde.
    e->failed = true;
    return false;
  }
  const size_t bytes = DecodedBytes(res);
  TrimFor(bytes);  // `e` n'est pas dans la file tant qu'il n'est pas chargé

  e->res = std::move(res);
  e->bytes = bytes;
  e->epoch = Overlay_DeviceEpoch();
  e->box_valid = false;
  e->loaded = true;
  if (e->pals.empty()) e->pals.resize(1);  // slot 0 = palette d'origine du .spr
  g_cache_bytes += bytes;
  // ⚠ La CLÉ, pas la base : `TrimFor` s'en sert pour retrouver l'entrée dans
  // `g_cache`, et les deux diffèrent dès qu'il y a appariement .spr/.act.
  g_cache_order.push_back(e->key);
  return true;
}

// Toute lecture de `e->res` passe par ici : l'entrée a pu être déchargée depuis
// le dernier accès.
bool EnsureLoaded(Entry* e) {
  if (!e || e->failed) return false;
  return e->loaded || LoadInto(e);
}

Entry* Acquire(const char* base_path, const char* act_base) {
  const bool split = act_base && *act_base && std::strcmp(act_base, base_path) != 0;
  std::string key = base_path;
  if (split) {
    key += '|';  // séparateur impossible dans un chemin VFS
    key += act_base;
  }
  auto it = g_cache.find(key);
  if (it == g_cache.end()) {
    auto entry = std::make_unique<Entry>();
    entry->base = base_path;
    if (split) entry->act_base = act_base;
    entry->key = key;
    entry->pals.resize(1);
    it = g_cache.emplace(std::move(key), std::move(entry)).first;
  }
  return EnsureLoaded(it->second.get()) ? it->second.get() : nullptr;
}

// Cadence déclarée par le .act, en ms par image. 0 = absente ou aberrante.
float DeclaredIntervalMs(const Entry* e, unsigned action) {
  if (!e || action >= e->res.actions.size()) return 0.0f;
  const float ms = e->res.actions[action].speed * kActDelayTickMs;
  // Bornes de bon sens : un .act corrompu ne doit ni figer l'animation ni la
  // faire clignoter à la fréquence de rafraîchissement.
  return (ms > 1.0f && ms < 10000.0f) ? ms : 0.0f;
}

// L'image à afficher pour une horloge donnée. UN seul exemplaire de ce calcul :
// le dessin et le son d'image doivent tomber sur la même.
unsigned FrameIndexFor(const Entry* e, unsigned action, float anim_seconds,
                       float ms_per_frame) {
  if (!e || action >= e->res.actions.size()) return 0;
  const int frames = static_cast<int>(e->res.actions[action].frames.size());
  if (frames <= 1 || ms_per_frame <= 0.0f) return 0;  // cadence nulle = figé
  // La cadence DÉCLARÉE par le .act prime sur celle demandée par l'appelant :
  // elle est propre à l'action et varie beaucoup d'un sprite à l'autre. Le
  // paramètre ne sert plus que d'interrupteur (0 = figé) et de repli.
  if (const float declared = DeclaredIntervalMs(e, action))
    ms_per_frame = declared;
  const float cycle = frames * (ms_per_frame / 1000.0f);
  float t = std::fmod(anim_seconds, cycle);
  if (t < 0.0f) t += cycle;
  int idx = static_cast<int>(t / cycle * frames);
  if (idx < 0) idx = 0;
  if (idx >= frames) idx = frames - 1;
  return static_cast<unsigned>(idx);
}

// ── Un calque résolu, prêt à dessiner ────────────────────────────────────────
//
// On garde les QUATRE COINS, pas un rectangle : la rotation du calque fait
// partie de la géométrie et un rectangle aligné aux axes la perdrait — à la fois
// pour le dessin et pour la boîte englobante qui sert au cadrage.
struct ResolvedLayer {
  void*  tex = nullptr;
  ImVec2 uv0{0.0f, 0.0f}, uv1{1.0f, 1.0f};
  ImVec2 corner[4];  // TL, TR, BR, BL en unités .act, rotation appliquée
  ImU32  tint = IM_COL32_WHITE;
};

constexpr int kMaxDrawLayers = 64;

const spract::Frame* FrameAt(const Entry* e, unsigned action, unsigned frame) {
  if (action >= e->res.actions.size()) return nullptr;
  const spract::Action& a = e->res.actions[action];
  if (frame >= a.frames.size()) return nullptr;
  return &a.frames[frame];
}

// Résout tous les calques de (action, frame) en textures + géométrie.
//
// `upload` : à false, on calcule la seule géométrie sans créer de texture —
// c'est ce dont le balayage de la boîte englobante a besoin, et ça évite de
// monter en VRAM des images qui ne seront jamais dessinées.
int ResolveFrameLayers(Entry* e, int pal_slot, unsigned action,
                       unsigned frame_index, ResolvedLayer* out, int max_out,
                       bool upload, bool apply_rotation = true) {
  const spract::Frame* frame = FrameAt(e, action, frame_index);
  if (!frame) return 0;

  int count = 0;
  for (const spract::Layer& L : frame->layers) {
    if (count >= max_out) break;
    const spract::Image* img = e->res.Get(L.index, L.type);
    if (!img || img->w <= 0 || img->h <= 0) continue;

    ResolvedLayer& r = out[count];
    r.tex = upload ? TextureFor(e, pal_slot, L.index, L.type) : nullptr;
    if (upload && !r.tex) continue;

    const float sx = L.scale_x;
    const float sy = L.scale_y;
    const float cw = static_cast<float>(img->w);
    const float ch = static_cast<float>(img->h);

    // ── Géométrie d'un calque : `Plane.FromLayer` de GRF Editor, à la lettre ──
    //
    // Dans CET ordre, et l'ordre compte :
    //   1. image CENTRÉE sur l'origine, translation de (-(w+1)/2, -(h+1)/2)
    //      — division entière, comme la référence ;
    //   2. échelle du calque (autour de l'origine, donc le centrage est lui
    //      aussi mis à l'échelle) ;
    //   3. rotation de -rotation, centre décalé d'un demi-pixel sur les
    //      dimensions IMPAIRES (sinon un calque de largeur impaire dérive d'un
    //      demi-pixel à chaque image) ;
    //   4. translation par (OffsetX, OffsetY) — 🔴 offset NON mis à l'échelle.
    //
    // 🔴 Le point 4 a coûté cher une première fois. Multiplier l'offset par
    // l'échelle du calque décroche les calques réduits : sur un sprite qui se
    // déplace de haut en bas, un calque à l'échelle 0,3 ne suit que 30 % du
    // mouvement et reste en arrière. Deux sources indépendantes le confirment :
    // `Plane.FromLayer` de la référence, et `Actor_DrawSprites` (0x007AC820)
    // qui pose la position à `acteur.x + layer.OffsetX` — la seule échelle
    // appliquée à un offset y est celle du JOB, jamais celle du calque.
    const int   iw = img->w, ih = img->h;
    const float cx0 = static_cast<float>(-((iw + 1) / 2));
    const float cy0 = static_cast<float>(-((ih + 1) / 2));
    float px[4] = {cx0, cx0 + cw, cx0 + cw, cx0};        // TL, TR, BR, BL
    float py[4] = {cy0, cy0,      cy0 + ch, cy0 + ch};
    for (int k = 0; k < 4; ++k) { px[k] *= sx; py[k] *= sy; }
    if (apply_rotation && L.rotation != 0) {
      // `RotateZ(-rotation)` de la référence :
      //   x' = x*cos + y*sin ;  y' = -x*sin + y*cos
      const float rad = static_cast<float>(-L.rotation) * 3.14159265358979f / 180.0f;
      const float cs = std::cos(rad), sn = std::sin(rad);
      const float rcx = (iw % 2) ? -0.5f * sx : 0.0f;
      const float rcy = (ih % 2) ? -0.5f * sy : 0.0f;
      for (int k = 0; k < 4; ++k) {
        const float dx = px[k] - rcx, dy = py[k] - rcy;
        px[k] = rcx + dx * cs + dy * sn;
        py[k] = rcy - dx * sn + dy * cs;
      }
    }
    for (int k = 0; k < 4; ++k)
      r.corner[k] = ImVec2(L.off_x + px[k], L.off_y + py[k]);

    // Texture dédiée par image : UV triviales. Le miroir est un échange des u.
    //
    // ⚠ La doc d'ActEditor dit « Mirror ≡ -1 * ScaleX », ce qui retournerait le
    // calque autour de son ORIGINE et non sur place. Essayé : le pantin s'est
    // mis à changer de taille. À reprendre avec un cas de test franc plutôt
    // qu'en aveugle.
    r.uv0 = ImVec2(0.0f, 0.0f);
    r.uv1 = ImVec2(1.0f, 1.0f);
    if (L.mirror) { r.uv0.x = 1.0f; r.uv1.x = 0.0f; }

    // Un calque à teinte entièrement nulle serait invisible : la plupart des
    // .act laissent ce champ à 0xFFFFFFFF, quelques-uns le laissent à zéro.
    r.tint = (L.tint == 0) ? IM_COL32_WHITE
                           : IM_COL32((L.tint >> 16) & 0xFF, (L.tint >> 8) & 0xFF,
                                      L.tint & 0xFF, (L.tint >> 24) & 0xFF);
    ++count;
  }
  return count;
}

}  // namespace

bool LoadSprite(const char* base_path, SpriteRes* res) {
  return LoadSpriteRecolored(base_path, nullptr, res);
}

bool LoadSpriteRecolored(const char* base_path, const char* pal_path,
                         SpriteRes* res) {
  return LoadSpritePair(base_path, nullptr, pal_path, res);
}

bool LoadSpritePair(const char* spr_base, const char* act_base,
                    const char* pal_path, SpriteRes* res) {
  if (!res || !spr_base || !*spr_base) return false;
  Entry* e = Acquire(spr_base, act_base);
  res->res = e;
  res->failed = (e == nullptr);
  res->palette = e ? PaletteSlot(e, pal_path) : 0;
  return !res->failed;
}

// ⚠ Toutes les fonctions publiques passent par `EnsureLoaded` : la poignée que
// l'appelant garde peut désigner une entrée déchargée depuis son dernier usage.
int SpriteActionCount(const SpriteRes& res) {
  Entry* e = static_cast<Entry*>(res.res);
  return EnsureLoaded(e) ? static_cast<int>(e->res.actions.size()) : 0;
}

int SpriteActionFrameCount(const SpriteRes& res, unsigned action) {
  Entry* e = static_cast<Entry*>(res.res);
  if (!EnsureLoaded(e) || action >= e->res.actions.size()) return 0;
  return static_cast<int>(e->res.actions[action].frames.size());
}

float SpriteFrameIntervalMs(const SpriteRes& res, unsigned action) {
  Entry* e = static_cast<Entry*>(res.res);
  return EnsureLoaded(e) ? DeclaredIntervalMs(e, action) : 0.0f;
}

unsigned SpriteFrameIndex(const SpriteRes& res, unsigned action,
                          float anim_seconds, float ms_per_frame) {
  Entry* e = static_cast<Entry*>(res.res);
  return EnsureLoaded(e) ? FrameIndexFor(e, action, anim_seconds, ms_per_frame)
                         : 0u;
}

namespace {
// Une entrée de la table de sons n'est un son que si c'est un .wav : le reste
// sont des marqueurs d'animation logés dans la même table.
bool IsWav(const std::string& s) {
  if (s.size() < 4) return false;
  const char* p = s.c_str() + s.size() - 4;
  return (p[0] == '.') && (p[1] == 'w' || p[1] == 'W') &&
         (p[2] == 'a' || p[2] == 'A') && (p[3] == 'v' || p[3] == 'V');
}
}  // namespace

const char* SpriteFrameSound(const SpriteRes& res, unsigned action,
                             unsigned frame) {
  Entry* e = static_cast<Entry*>(res.res);
  if (!EnsureLoaded(e)) return nullptr;
  const spract::Frame* f = FrameAt(e, action, frame);
  if (!f || f->sound_id < 0) return nullptr;
  if (static_cast<size_t>(f->sound_id) >= e->res.sound_files.size()) return nullptr;
  const std::string& s = e->res.sound_files[f->sound_id];
  return IsWav(s) ? s.c_str() : nullptr;
}

const char* SpriteFrameEvent(const SpriteRes& res, unsigned action,
                             unsigned frame) {
  Entry* e = static_cast<Entry*>(res.res);
  if (!EnsureLoaded(e)) return nullptr;
  const spract::Frame* f = FrameAt(e, action, frame);
  if (!f || f->sound_id < 0) return nullptr;
  if (static_cast<size_t>(f->sound_id) >= e->res.sound_files.size()) return nullptr;
  const std::string& s = e->res.sound_files[f->sound_id];
  return IsWav(s) ? nullptr : s.c_str();  // complément exact de SpriteFrameSound
}

const char* SpriteMainSound(const SpriteRes& res) {
  Entry* e = static_cast<Entry*>(res.res);
  if (!EnsureLoaded(e)) return nullptr;
  for (const std::string& s : e->res.sound_files)
    if (IsWav(s)) return s.c_str();
  return nullptr;
}

int SpriteResolveFrame(const SpriteRes& res, unsigned action, unsigned frame,
                       SpriteQuad* out, int max_out, bool apply_rotation) {
  Entry* e = static_cast<Entry*>(res.res);
  if (!out || max_out <= 0 || !EnsureLoaded(e)) return 0;

  ResolvedLayer layers[kMaxDrawLayers];
  const int n = ResolveFrameLayers(e, res.palette, action, frame, layers,
                                   kMaxDrawLayers, /*upload=*/true,
                                   apply_rotation);
  int count = 0;
  for (int i = 0; i < n && count < max_out; ++i) {
    if (!layers[i].tex) continue;
    SpriteQuad& q = out[count++];
    q.tex  = layers[i].tex;
    q.uv0  = layers[i].uv0;
    q.uv1  = layers[i].uv1;
    q.tint = layers[i].tint;
    for (int k = 0; k < 4; ++k) q.corner[k] = layers[i].corner[k];
  }
  return count;
}

bool SpriteFrameAnchor(const SpriteRes& res, unsigned action, unsigned frame,
                       int* x, int* y, int* attr) {
  Entry* e = static_cast<Entry*>(res.res);
  if (!EnsureLoaded(e)) return false;
  // ⚠ Actions < 8 : l'ancre de référence est celle de l'IMAGE 0 — c'est ce que
  // fait le natif (Act_GetFrame(spr, action, 0) quand action < 8). Prendre celle
  // de l'image courante ferait vibrer les pièces pendant l'animation d'attente.
  const spract::Frame* f = FrameAt(e, action, action < 8 ? 0u : frame);
  if (!f || f->anchors.empty()) return false;
  const spract::Anchor& a = f->anchors[0];
  if (x) *x = a.x;
  if (y) *y = a.y;
  if (attr) *attr = a.attr;
  return true;
}

bool DrawSprite(ImDrawList* draw_list, const SpriteRes& res, ImVec2 rect_min,
                ImVec2 rect_max, float anim_seconds, unsigned action,
                float ms_per_frame, bool allow_upscale, float alpha) {
  Entry* e = static_cast<Entry*>(res.res);
  if (!draw_list || !EnsureLoaded(e) || action >= e->res.actions.size())
    return false;
  const float box_w = rect_max.x - rect_min.x;
  const float box_h = rect_max.y - rect_min.y;
  if (box_w <= 1.0f || box_h <= 1.0f) return false;

  const int frames = static_cast<int>(e->res.actions[action].frames.size());
  if (frames <= 0) return false;

  // Image courante. Une cadence <= 0 fige l'animation sur la première image.
  const unsigned frame_index =
      FrameIndexFor(e, action, anim_seconds, ms_per_frame);

  ResolvedLayer layers[kMaxDrawLayers];
  const int n = ResolveFrameLayers(e, res.palette, action, frame_index, layers,
                                   kMaxDrawLayers, /*upload=*/true);
  if (n <= 0) return false;

  // ── Boîte englobante de TOUTE l'action ─────────────────────────────────────
  // 🔴 Calculée UNE fois, sur toutes les images — comme `GenerateBoundingBox(act,
  // actionIndex)` de GRF Editor, dont `GenerateImages` se sert ensuite pour
  // rendre chaque image du GIF.
  //
  // La recalculer par image ferait varier l'échelle de cadrage à chaque image :
  // le sprite se dilaterait et se contracterait au rythme de son animation.
  if (!e->box_valid || e->box_action != static_cast<int>(action)) {
    ResolvedLayer scratch[kMaxDrawLayers];
    bool any = false;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    for (int f = 0; f < frames; ++f) {
      // La géométrie ne dépend pas de la teinte : slot 0, et rien à téléverser.
      const int c = ResolveFrameLayers(e, 0, action, static_cast<unsigned>(f),
                                       scratch, kMaxDrawLayers, /*upload=*/false);
      for (int i = 0; i < c; ++i) {
        for (int k = 0; k < 4; ++k) {
          const ImVec2& p = scratch[i].corner[k];
          if (!any) { bx0 = bx1 = p.x; by0 = by1 = p.y; any = true; continue; }
          if (p.x < bx0) bx0 = p.x;
          if (p.x > bx1) bx1 = p.x;
          if (p.y < by0) by0 = p.y;
          if (p.y > by1) by1 = p.y;
        }
      }
    }
    if (any) {
      e->box_min_x = bx0; e->box_max_x = bx1;
      e->box_min_y = by0; e->box_max_y = by1;
      e->box_action = static_cast<int>(action);
      e->box_valid = true;
    }
  }

  // La boîte de l'ACTION, jamais celle de l'image courante : c'est elle qui fixe
  // l'échelle ET le centrage, donc les deux restent stables d'une image à
  // l'autre. Repli sur l'image courante si le balayage a échoué.
  float min_x, max_x, min_y, max_y;
  if (e->box_valid && e->box_action == static_cast<int>(action)) {
    min_x = e->box_min_x; max_x = e->box_max_x;
    min_y = e->box_min_y; max_y = e->box_max_y;
  } else {
    min_x = max_x = layers[0].corner[0].x;
    min_y = max_y = layers[0].corner[0].y;
    for (int i = 0; i < n; ++i) {
      for (int k = 0; k < 4; ++k) {
        const ImVec2& p = layers[i].corner[k];
        if (p.x < min_x) min_x = p.x;
        if (p.x > max_x) max_x = p.x;
        if (p.y < min_y) min_y = p.y;
        if (p.y > max_y) max_y = p.y;
      }
    }
  }
  const float span_x = max_x - min_x;
  const float span_y = max_y - min_y;
  if (span_x <= 0.0f || span_y <= 0.0f) return false;

  // Échelle « pour tenir dans la boîte », puis bornée à 1 : on RÉDUIT ce qui
  // dépasse, on n'agrandit jamais. Les tailles du .act sont celles du jeu.
  float scale = box_w / span_x;
  const float fit_y = box_h / span_y;
  if (fit_y < scale) scale = fit_y;
  if (!allow_upscale && scale > 1.0f) scale = 1.0f;
  if (scale <= 0.0f) return false;

  const float center_x = (min_x + max_x) * 0.5f;
  const float center_y = (min_y + max_y) * 0.5f;
  const float origin_x = (rect_min.x + rect_max.x) * 0.5f - center_x * scale;
  const float origin_y = (rect_min.y + rect_max.y) * 0.5f - center_y * scale;

  int a = static_cast<int>(alpha * 255.0f + 0.5f);
  if (a < 0) a = 0;
  if (a > 255) a = 255;

  bool drawn = false;
  for (int i = 0; i < n; ++i) {
    const ResolvedLayer& l = layers[i];
    if (!l.tex) continue;
    // Alpha global appliqué PAR-DESSUS la teinte du calque (multiplicatif sur
    // le canal A, comme le fait le moteur).
    const ImU32 tint =
        (l.tint & 0x00FFFFFFu) |
        (static_cast<ImU32>(((l.tint >> IM_COL32_A_SHIFT) & 0xFF) * a / 255)
         << IM_COL32_A_SHIFT);
    ImVec2 p[4];
    for (int k = 0; k < 4; ++k)
      p[k] = ImVec2(origin_x + l.corner[k].x * scale,
                    origin_y + l.corner[k].y * scale);
    // AddImageQuad (et non AddImage) : le quad peut être TOURNÉ, un rectangle
    // aligné aux axes ne saurait pas le représenter.
    draw_list->AddImageQuad(reinterpret_cast<ImTextureID>(l.tex), p[0], p[1],
                            p[2], p[3],
                            ImVec2(l.uv0.x, l.uv0.y), ImVec2(l.uv1.x, l.uv0.y),
                            ImVec2(l.uv1.x, l.uv1.y), ImVec2(l.uv0.x, l.uv1.y),
                            tint);
    drawn = true;
  }
  return drawn;
}

}  // namespace ro
