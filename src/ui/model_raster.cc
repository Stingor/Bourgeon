#include "ui/model_raster.h"

#include <algorithm>
#include <cmath>

namespace ro {
namespace {

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

// Repère du modèle : X et Y au sol, Z vers le haut. La caméra tourne d'abord
// autour de Z (yaw, ce que le joueur manipule), puis bascule (pitch).
Vec3 ViewTransform(const float p[3], float cy, float sy, float cp, float sp) {
  const float x = p[0] * cy - p[1] * sy;
  const float y = p[0] * sy + p[1] * cy;
  const float z = p[2];
  // Après le yaw : x reste l'horizontale de l'écran, et (y, z) basculent.
  //
  // 🔴 Le signe se vérifie sur la PROFONDEUR, pas sur la hauteur. Une caméra
  // placée au-dessus voit le sommet de l'objet PLUS PRÈS que sa base : le
  // coefficient de `z` dans la profondeur doit donc être négatif. Avec le signe
  // opposé, l'image reste parfaitement plausible — elle montre simplement le
  // monstre vu de DESSOUS, ce qui ne saute aux yeux que sur un modèle qu'on
  // connaît déjà. C'est exactement l'erreur qui a été livrée une fois.
  Vec3 v;
  v.x = x;
  v.y = z * cp + y * sp;   // hauteur à l'écran (ce qui est loin monte)
  v.z = y * cp - z * sp;   // profondeur (plus grand = plus loin)
  return v;
}

uint8_t ToByte(float v) {
  const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  return static_cast<uint8_t>(c * 255.0f + 0.5f);
}

}  // namespace

bool RenderModelImage(const Model& model, const ModelViewParams& params,
                      std::vector<uint8_t>* out_rgba) {
  if (!out_rgba || params.width <= 0 || params.height <= 0) return false;
  if (model.meshes.empty()) return false;

  const int ss = params.supersample ? 2 : 1;
  const int w = params.width * ss;
  const int h = params.height * ss;

  std::vector<uint8_t> img(static_cast<size_t>(w) * h * 4, 0);
  std::vector<float> depth(static_cast<size_t>(w) * h, 1e30f);

  const float cy = std::cos(params.yaw), sy = std::sin(params.yaw);
  const float cp = std::cos(params.pitch), sp = std::sin(params.pitch);

  // ── Cadrage ────────────────────────────────────────────────────────────────
  // Sur la POSE COURANTE, pas sur la boîte du fichier : une animation sort du
  // volume de repos (le drapeau claque, le gardien lève son arme), et cadrer sur
  // le repos ferait respirer le modèle hors du cadre au fil de l'animation.
  float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
  for (const ModelMesh& mm : model.meshes) {
    for (const ModelVertex& v : mm.posed) {
      const Vec3 t = ViewTransform(v.p, cy, sy, cp, sp);
      mnx = std::min(mnx, t.x); mxx = std::max(mxx, t.x);
      mny = std::min(mny, t.y); mxy = std::max(mxy, t.y);
    }
  }
  if (mxx <= mnx || mxy <= mny) return false;

  const float usable = 1.0f - 2.0f * params.margin;
  const float scale = std::min(static_cast<float>(w) * usable / (mxx - mnx),
                               static_cast<float>(h) * usable / (mxy - mny));
  const float cxm = (mnx + mxx) * 0.5f, cym = (mny + mxy) * 0.5f;
  const float ox = static_cast<float>(w) * 0.5f - cxm * scale;
  // L'axe Y de l'écran descend, celui du modèle monte : le signe s'inverse ici
  // et NULLE PART ailleurs.
  const float oy = static_cast<float>(h) * 0.5f + cym * scale;

  // Lumière fixe par rapport à la CAMÉRA (venant du haut-avant-gauche), pour
  // que la rotation du modèle éclaire successivement toutes ses faces au lieu
  // de laisser un côté définitivement noir.
  const float lx = -0.42f, ly = 0.66f, lz = -0.62f;

  for (const ModelMesh& mm : model.meshes) {
    for (const ModelPart& part : mm.parts) {
      const ModelTexture* tex =
          (part.texture >= 0 && part.texture < static_cast<int>(model.textures.size()))
              ? &model.textures[static_cast<size_t>(part.texture)]
              : nullptr;
      const bool has_tex = tex && tex->w > 0 && tex->h > 0 && !tex->rgba.empty();

      const int tri_end = part.tri_first + part.tri_count;
      for (int tri = part.tri_first; tri < tri_end; ++tri) {
        const size_t i0 = static_cast<size_t>(tri) * 3;
        if (i0 + 2 >= mm.indices.size()) break;
        const ModelVertex* v[3];
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
          const uint16_t idx = mm.indices[i0 + k];
          if (idx >= mm.posed.size()) { ok = false; break; }
          v[k] = &mm.posed[idx];
        }
        if (!ok) continue;

        Vec3 s[3];
        for (int k = 0; k < 3; ++k) {
          const Vec3 t = ViewTransform(v[k]->p, cy, sy, cp, sp);
          s[k].x = ox + t.x * scale;
          s[k].y = oy - t.y * scale;
          s[k].z = t.z;
        }

        // Aire signée : elle sert au test barycentrique ET donne l'orientation.
        // 🔴 Pas d'élimination des faces arrière : ces modèles ont des surfaces
        // ouvertes (le drapeau est un plan, les cristaux de l'Emperium sont des
        // coques) et les éliminer perce le monstre. On éclaire donc la normale
        // des deux côtés plutôt que de jeter le triangle.
        const float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) -
                           (s[2].x - s[0].x) * (s[1].y - s[0].y);
        if (std::fabs(area) < 1e-6f) continue;
        const float inv_area = 1.0f / area;

        int x0 = static_cast<int>(std::floor(std::min({s[0].x, s[1].x, s[2].x})));
        int x1 = static_cast<int>(std::ceil (std::max({s[0].x, s[1].x, s[2].x})));
        int y0 = static_cast<int>(std::floor(std::min({s[0].y, s[1].y, s[2].y})));
        int y1 = static_cast<int>(std::ceil (std::max({s[0].y, s[1].y, s[2].y})));
        x0 = std::max(x0, 0); y0 = std::max(y0, 0);
        x1 = std::min(x1, w - 1); y1 = std::min(y1, h - 1);

        for (int y = y0; y <= y1; ++y) {
          for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float w0 = ((s[1].x - px) * (s[2].y - py) -
                              (s[2].x - px) * (s[1].y - py)) * inv_area;
            const float w1 = ((s[2].x - px) * (s[0].y - py) -
                              (s[0].x - px) * (s[2].y - py)) * inv_area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            const float z = w0 * s[0].z + w1 * s[1].z + w2 * s[2].z;
            const size_t o = static_cast<size_t>(y) * w + x;
            if (z >= depth[o]) continue;

            // Texture au plus proche voisin : c'est le filtre du jeu, et un
            // modèle agrandi dans une fiche doit rester dans la même famille
            // visuelle que les sprites autour de lui.
            uint8_t r = 200, g = 200, b = 200, a = 255;
            if (has_tex) {
              const float u = w0 * v[0]->uv[0] + w1 * v[1]->uv[0] + w2 * v[2]->uv[0];
              const float vv = w0 * v[0]->uv[1] + w1 * v[1]->uv[1] + w2 * v[2]->uv[1];
              int tx = static_cast<int>(u * static_cast<float>(tex->w));
              int ty = static_cast<int>(vv * static_cast<float>(tex->h));
              tx = ((tx % tex->w) + tex->w) % tex->w;  // répétition
              ty = ((ty % tex->h) + tex->h) % tex->h;
              const uint8_t* t = &tex->rgba[(static_cast<size_t>(ty) * tex->w + tx) * 4];
              r = t[0]; g = t[1]; b = t[2]; a = t[3];
            }
            // Un pixel quasi transparent ne doit ni peindre ni occulter :
            // sans ce test, le carré de la texture masquerait ce qu'il y a
            // derrière (les faces d'un cristal se cachent entre elles).
            if (a < 8) continue;

            const float nx = w0 * v[0]->n[0] + w1 * v[1]->n[0] + w2 * v[2]->n[0];
            const float ny = w0 * v[0]->n[1] + w1 * v[1]->n[1] + w2 * v[2]->n[1];
            const float nz = w0 * v[0]->n[2] + w1 * v[1]->n[2] + w2 * v[2]->n[2];
            const float nrm[3] = {nx, ny, nz};
            const Vec3 nv = ViewTransform(nrm, cy, sy, cp, sp);
            const float len = std::sqrt(nv.x * nv.x + nv.y * nv.y + nv.z * nv.z);
            float lambert = 0.0f;
            if (len > 1e-6f) {
              lambert = (nv.x * lx + nv.y * ly + nv.z * lz) / len;
              lambert = std::fabs(lambert);  // éclairé des deux côtés, cf. plus haut
            }
            const float k = params.ambient + params.diffuse * lambert;

            depth[o] = z;
            uint8_t* dst = &img[o * 4];
            dst[0] = ToByte(static_cast<float>(r) / 255.0f * k);
            dst[1] = ToByte(static_cast<float>(g) / 255.0f * k);
            dst[2] = ToByte(static_cast<float>(b) / 255.0f * k);
            dst[3] = a;
          }
        }
      }
    }
  }

  if (ss == 1) {
    *out_rgba = std::move(img);
    return true;
  }

  // Réduction 2×2. La moyenne se fait sur des couleurs PRÉMULTIPLIÉES par
  // l'alpha : sinon les pixels transparents (dont la couleur est arbitraire)
  // délavent les bords, et le modèle se retrouve cerné d'un halo.
  out_rgba->assign(static_cast<size_t>(params.width) * params.height * 4, 0);
  for (int y = 0; y < params.height; ++y) {
    for (int x = 0; x < params.width; ++x) {
      int acc[4] = {0, 0, 0, 0};
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          const uint8_t* s = &img[((static_cast<size_t>(y) * 2 + dy) * w + x * 2 + dx) * 4];
          acc[0] += s[0] * s[3];
          acc[1] += s[1] * s[3];
          acc[2] += s[2] * s[3];
          acc[3] += s[3];
        }
      }
      uint8_t* d = &(*out_rgba)[(static_cast<size_t>(y) * params.width + x) * 4];
      if (acc[3] > 0) {
        d[0] = static_cast<uint8_t>(acc[0] / acc[3]);
        d[1] = static_cast<uint8_t>(acc[1] / acc[3]);
        d[2] = static_cast<uint8_t>(acc[2] / acc[3]);
        d[3] = static_cast<uint8_t>(acc[3] / 4);
      }
    }
  }
  return true;
}

}  // namespace ro
