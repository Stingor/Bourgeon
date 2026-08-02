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
struct Entry {
  spract::Resource res;
  std::vector<void*> tex_indexed;
  std::vector<void*> tex_bgra;
  unsigned epoch = 0;

  int   box_action = -1;
  float box_min_x = 0, box_min_y = 0, box_max_x = 0, box_max_y = 0;
  bool  box_valid = false;
};

std::unordered_map<std::string, std::unique_ptr<Entry>> g_cache;
std::deque<std::string> g_cache_order;  // ordre d'arrivée, pour l'éviction
// Un sprite coûte quelques centaines de kilo-octets de VRAM (2 à 6 images
// téléversées). Seize entrées bornent la facture tout en couvrant largement le
// va-et-vient entre plusieurs vues.
constexpr size_t kMaxCached = 16;

void ReleaseTextures(Entry* e) {
  for (void* t : e->tex_indexed) Overlay_ReleaseTexture(t);
  for (void* t : e->tex_bgra) Overlay_ReleaseTexture(t);
  e->tex_indexed.clear();
  e->tex_bgra.clear();
}

// Texture d'une image, créée au premier besoin.
void* TextureFor(Entry* e, int index, int type) {
  const unsigned epoch = Overlay_DeviceEpoch();
  if (epoch != e->epoch) {
    // ⚠ On LÂCHE sans Release : ces handles appartiennent à un device qui
    // n'existe plus (cf. [[feedback_texture_cache_device_epoch]]).
    e->tex_indexed.clear();
    e->tex_bgra.clear();
    e->epoch = epoch;
  }
  const std::vector<spract::Image>& imgs =
      (type == 0) ? e->res.indexed : e->res.bgra;
  if (index < 0 || static_cast<size_t>(index) >= imgs.size()) return nullptr;
  std::vector<void*>& tex = (type == 0) ? e->tex_indexed : e->tex_bgra;
  if (tex.size() != imgs.size()) tex.assign(imgs.size(), nullptr);
  if (!tex[index]) {
    const spract::Image& img = imgs[index];
    if (img.w > 0 && img.h > 0 && !img.argb.empty())
      tex[index] = Overlay_CreateTextureARGB(img.argb.data(), img.w, img.h);
  }
  return tex[index];
}

Entry* Acquire(const char* base_path) {
  auto it = g_cache.find(base_path);
  if (it != g_cache.end()) return it->second.get();

  char spr_path[352], act_path[352];
  std::snprintf(spr_path, sizeof(spr_path), "%s.spr", base_path);
  std::snprintf(act_path, sizeof(act_path), "%s.act", base_path);

  auto entry = std::make_unique<Entry>();
  if (!spract::Load(spr_path, act_path, &entry->res)) return nullptr;
  entry->epoch = Overlay_DeviceEpoch();

  if (g_cache_order.size() >= kMaxCached) {
    const std::string victim = g_cache_order.front();
    g_cache_order.pop_front();
    auto vit = g_cache.find(victim);
    if (vit != g_cache.end()) {
      // Le device est vivant ici (on vient d'en créer une entrée) : les textures
      // de la victime se libèrent pour de bon, sinon parcourir beaucoup de
      // sprites laisserait une texture en VRAM par image jamais revue.
      ReleaseTextures(vit->second.get());
      g_cache.erase(vit);
    }
  }
  Entry* raw = entry.get();
  g_cache[base_path] = std::move(entry);
  g_cache_order.push_back(base_path);
  return raw;
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
int ResolveFrameLayers(Entry* e, unsigned action, unsigned frame_index,
                       ResolvedLayer* out, int max_out, bool upload) {
  const spract::Frame* frame = FrameAt(e, action, frame_index);
  if (!frame) return 0;

  int count = 0;
  for (const spract::Layer& L : frame->layers) {
    if (count >= max_out) break;
    const spract::Image* img = e->res.Get(L.index, L.type);
    if (!img || img->w <= 0 || img->h <= 0) continue;

    ResolvedLayer& r = out[count];
    r.tex = upload ? TextureFor(e, L.index, L.type) : nullptr;
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
    if (L.rotation != 0) {
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

    // Texture dédiée par image : UV triviales. Le miroir est un simple échange
    // des u — un retournement SUR PLACE, dans le même quad, ce qui est bien la
    // sémantique du champ (ScaleX = -1 autour du centre de l'image).
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
  if (!res || !base_path || !*base_path) return false;
  res->res = Acquire(base_path);
  res->failed = (res->res == nullptr);
  return !res->failed;
}

int SpriteActionFrameCount(const SpriteRes& res, unsigned action) {
  const Entry* e = static_cast<const Entry*>(res.res);
  if (!e || action >= e->res.actions.size()) return 0;
  return static_cast<int>(e->res.actions[action].frames.size());
}

float SpriteFrameIntervalMs(const SpriteRes& res, unsigned action) {
  return DeclaredIntervalMs(static_cast<const Entry*>(res.res), action);
}

unsigned SpriteFrameIndex(const SpriteRes& res, unsigned action,
                          float anim_seconds, float ms_per_frame) {
  return FrameIndexFor(static_cast<const Entry*>(res.res), action, anim_seconds,
                       ms_per_frame);
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
  const Entry* e = static_cast<const Entry*>(res.res);
  if (!e) return nullptr;
  const spract::Frame* f = FrameAt(e, action, frame);
  if (!f || f->sound_id < 0) return nullptr;
  if (static_cast<size_t>(f->sound_id) >= e->res.sound_files.size()) return nullptr;
  const std::string& s = e->res.sound_files[f->sound_id];
  return IsWav(s) ? s.c_str() : nullptr;
}

const char* SpriteMainSound(const SpriteRes& res) {
  const Entry* e = static_cast<const Entry*>(res.res);
  if (!e) return nullptr;
  for (const std::string& s : e->res.sound_files)
    if (IsWav(s)) return s.c_str();
  return nullptr;
}

bool DrawSprite(ImDrawList* draw_list, const SpriteRes& res, ImVec2 rect_min,
                ImVec2 rect_max, float anim_seconds, unsigned action,
                float ms_per_frame, bool allow_upscale, float alpha) {
  Entry* e = static_cast<Entry*>(res.res);
  if (!draw_list || !e || action >= e->res.actions.size()) return false;
  const float box_w = rect_max.x - rect_min.x;
  const float box_h = rect_max.y - rect_min.y;
  if (box_w <= 1.0f || box_h <= 1.0f) return false;

  const int frames = static_cast<int>(e->res.actions[action].frames.size());
  if (frames <= 0) return false;

  // Image courante. Une cadence <= 0 fige l'animation sur la première image.
  const unsigned frame_index =
      FrameIndexFor(e, action, anim_seconds, ms_per_frame);

  ResolvedLayer layers[kMaxDrawLayers];
  const int n = ResolveFrameLayers(e, action, frame_index, layers,
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
      const int c = ResolveFrameLayers(e, action, static_cast<unsigned>(f),
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
