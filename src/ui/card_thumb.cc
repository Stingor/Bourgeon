#include "ui/card_thumb.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / ReleaseTexture / DeviceEpoch
#include "imgui.h"           // ImGui::GetFrameCount
#include "ui/spr_act.h"      // ro::spract::ReadFile (le VFS du client)

namespace ro {
namespace cardthumb {
namespace {

// Vignettes décodées et téléversées PAR FRAME, au plus. Une vignette coûte une
// lecture VFS, un décodage de 120 000 pixels et une réduction : autour de la
// milliseconde. Six par frame remplissent une double page en trois frames sans
// qu'on le voie ; toutes d'un coup feraient un à-coup à chaque page tournée.
constexpr int kLoadsPerFrame = 6;

// Entrées gardées, au plus. Une vignette de pochette (120×160 à l'échelle
// 100 %) pèse 77 Ko : 160 vignettes font 12 Mo, soit une dizaine de doubles
// pages — assez pour feuilleter dans les deux sens sans recharger, sans que la
// VRAM ne s'en ressente.
constexpr int kMaxEntries = 160;

// Chemin VFS = « data\texture\ » + chemin relatif au dossier texture. Le
// préfixe est en ASCII, le reste (CP949) vient de l'appelant tel quel.
constexpr char kTexturePrefix[] = "data\\texture\\";

struct Entry {
  void* tex = nullptr;
  int   w = 0;
  int   h = 0;
  bool  loaded = false;   // texture prête
  bool  missing = false;  // illustration absente ou illisible — MÉMORISÉ
  int   last_frame = -1;  // dernière frame où elle a été demandée
  uint64_t use_seq = 0;   // rang LRU
};

struct PendingRelease {
  void*    tex;
  unsigned epoch;
  int      frame;
};

std::unordered_map<uint64_t, Entry> g_cache;
std::vector<PendingRelease> g_pending;
Overlay_DeviceEpochWatch g_epoch_watch;
int      g_frame = 0;
int      g_budget = 0;
uint64_t g_seq = 0;

int CurrentFrame() {
  return ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0;
}

// (id, grey, boîte) en un seul entier : 32 bits d'id, un de teinte, deux fois
// douze de taille — une boîte est bornée à 4095 texels, bien au-delà d'une
// pochette.
uint64_t KeyOf(uint32_t id, bool grey, int w, int h) {
  const uint64_t bw = static_cast<uint64_t>(std::clamp(w, 1, 4095));
  const uint64_t bh = static_cast<uint64_t>(std::clamp(h, 1, 4095));
  return (static_cast<uint64_t>(id) << 25) | (static_cast<uint64_t>(grey ? 1 : 0) << 24) |
         (bw << 12) | bh;
}

void QueueRelease(void* tex) {
  if (tex == nullptr) return;
  g_pending.push_back({tex, Overlay_DeviceEpoch(), CurrentFrame()});
}

// ── Décodage .bmp ───────────────────────────────────────────────────────────
// Ce que les illustrations du client utilisent, et rien de plus : en-tête
// BITMAPINFOHEADER, sans compression, 8 bits palettisés ou 24/32 bits vrais.
// Sortie : 0xAARRGGBB, ligne 0 en HAUT. Le magenta est le colorkey de RO — les
// coins arrondis des cartes en sont peints — et il devient transparent, avec la
// même tolérance que les icônes (la frange anti-aliasée l'est aussi).

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool IsColorKey(int r, int g, int b) {
  const int drb = (r > b) ? r - b : b - r;
  return r >= 0xC8 && b >= 0xC8 && g <= 0x38 && drb <= 0x30;
}

uint32_t Pack(int r, int g, int b) {
  if (IsColorKey(r, g, b)) return 0u;
  return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

bool DecodeBmp(const std::vector<uint8_t>& f, int* out_w, int* out_h,
               std::vector<uint32_t>* out) {
  if (f.size() < 54 || f[0] != 'B' || f[1] != 'M') return false;
  const uint32_t off = U32(&f[10]);
  const uint32_t hdr = U32(&f[14]);
  if (hdr < 40) return false;  // BITMAPCOREHEADER : aucune illustration ne l'utilise
  const int32_t  w = static_cast<int32_t>(U32(&f[18]));
  const int32_t  h = static_cast<int32_t>(U32(&f[22]));
  const uint16_t bpp = U16(&f[28]);
  const uint32_t comp = U32(&f[30]);
  const int32_t  ah = h < 0 ? -h : h;
  if (w <= 0 || ah <= 0 || w > 4096 || ah > 4096) return false;
  if (comp != 0) return false;
  if (bpp != 8 && bpp != 24 && bpp != 32) return false;

  // ⚠ Toute l'arithmétique de bornes en 64 bits : ce client est en 32 bits, et
  // un en-tête corrompu (bfOffBits à 0xFFFFFF00, par exemple — un fichier sur
  // disque prime sur le GRF) ferait s'enrouler une somme en size_t sous la
  // taille du tampon, passer la garde, et lire hors du vecteur.
  const uint64_t size = f.size();
  uint32_t pal[256] = {};
  if (bpp == 8) {
    uint32_t used = U32(&f[46]);
    if (used == 0 || used > 256) used = 256;
    const uint64_t pal_off = 14ull + hdr;
    if (pal_off + used * 4ull > size) return false;
    for (uint32_t i = 0; i < used; ++i) {
      const uint8_t* e = &f[static_cast<size_t>(pal_off) + i * 4];
      pal[i] = Pack(e[2], e[1], e[0]);
    }
  }

  const size_t stride = ((static_cast<size_t>(w) * bpp + 31) / 32) * 4;
  if (static_cast<uint64_t>(off) + static_cast<uint64_t>(stride) * ah > size) return false;

  out->assign(static_cast<size_t>(w) * ah, 0u);
  for (int32_t y = 0; y < ah; ++y) {
    // Les .bmp sont rangés de BAS en haut, sauf hauteur négative.
    const int32_t src_row = (h > 0) ? (ah - 1 - y) : y;
    const uint8_t* row = &f[off + stride * static_cast<size_t>(src_row)];
    uint32_t* dst = &(*out)[static_cast<size_t>(y) * w];
    if (bpp == 8) {
      for (int32_t x = 0; x < w; ++x) dst[x] = pal[row[x]];
    } else if (bpp == 24) {
      for (int32_t x = 0; x < w; ++x) dst[x] = Pack(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
    } else {
      for (int32_t x = 0; x < w; ++x) dst[x] = Pack(row[x * 4 + 2], row[x * 4 + 1], row[x * 4]);
    }
  }
  *out_w = w;
  *out_h = ah;
  return true;
}

// ── Réduction ───────────────────────────────────────────────────────────────
// Moyenne de surface par sur-échantillonnage 3×3 : chaque texel de la vignette
// moyenne neuf points répartis dans la zone source qu'il couvre. Pour un
// facteur 2,5 c'est indiscernable d'un filtre de boîte exact, et ça tient en
// dix lignes. La couleur est PONDÉRÉE PAR L'ALPHA : sans cela le magenta rendu
// transparent aux coins baverait dans la bordure blanche de la carte.
void Shrink(const std::vector<uint32_t>& src, int sw, int sh,
            std::vector<uint32_t>* dst, int dw, int dh) {
  dst->assign(static_cast<size_t>(dw) * dh, 0u);
  const float sx = static_cast<float>(sw) / dw;
  const float sy = static_cast<float>(sh) / dh;
  constexpr int kSub = 3;
  for (int y = 0; y < dh; ++y) {
    for (int x = 0; x < dw; ++x) {
      unsigned sr = 0, sg = 0, sb = 0, sa = 0;
      for (int j = 0; j < kSub; ++j) {
        const int syy = std::min(sh - 1, static_cast<int>((y + (j + 0.5f) / kSub) * sy));
        for (int i = 0; i < kSub; ++i) {
          const int sxx = std::min(sw - 1, static_cast<int>((x + (i + 0.5f) / kSub) * sx));
          const uint32_t p = src[static_cast<size_t>(syy) * sw + sxx];
          const unsigned a = p >> 24;
          sr += ((p >> 16) & 0xFF) * a;
          sg += ((p >> 8) & 0xFF) * a;
          sb += (p & 0xFF) * a;
          sa += a;
        }
      }
      uint32_t o = 0;
      if (sa > 0) {
        const unsigned r = sr / sa, g = sg / sa, b = sb / sa;
        const unsigned a = sa / (kSub * kSub);
        o = (a << 24) | (r << 16) | (g << 8) | b;
      }
      (*dst)[static_cast<size_t>(y) * dw + x] = o;
    }
  }
}

// Silhouette : luminance ramenée entre 28 et 155. Assez sombre pour dire
// « pas à vous », assez contrastée pour reconnaître le monstre.
void Greyify(std::vector<uint32_t>* px) {
  for (uint32_t& p : *px) {
    const unsigned a = p >> 24;
    if (a == 0) continue;
    const unsigned r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    const unsigned l = (77 * r + 151 * g + 28 * b) >> 8;
    const unsigned v = 28 + l / 2;
    p = (a << 24) | (v << 16) | (v << 8) | v;
  }
}

// Fait entrer (w, h) dans (max_w, max_h) sans jamais agrandir.
void FitSize(int w, int h, int max_w, int max_h, int* dw, int* dh) {
  const float s = std::min({static_cast<float>(max_w) / w,
                            static_cast<float>(max_h) / h, 1.0f});
  *dw = std::max(1, static_cast<int>(w * s + 0.5f));
  *dh = std::max(1, static_cast<int>(h * s + 0.5f));
}

void Load(Entry* e, const char* illust_path, bool grey, int max_w, int max_h) {
  if (illust_path == nullptr || illust_path[0] == '\0') {
    e->missing = true;
    return;
  }
  std::string vfs(kTexturePrefix);
  vfs += illust_path;

  std::vector<uint8_t> file;
  std::vector<uint32_t> full, thumb;
  int w = 0, h = 0;
  if (!spract::ReadFile(vfs.c_str(), &file) || !DecodeBmp(file, &w, &h, &full)) {
    e->missing = true;
    return;
  }
  int dw = 0, dh = 0;
  FitSize(w, h, max_w, max_h, &dw, &dh);
  Shrink(full, w, h, &thumb, dw, dh);
  if (grey) Greyify(&thumb);

  void* tex = Overlay_CreateTextureARGB(thumb.data(), dw, dh);
  // Device pas encore capturé : ce n'est pas un échec de l'image, on
  // réessaiera à la frame suivante — d'où `loaded` laissé à faux sans
  // `missing`.
  if (tex == nullptr) return;
  e->tex = tex;
  e->w = dw;
  e->h = dh;
  e->loaded = true;
}

// Évince la plus ancienne entrée NON servie dans la frame courante tant que
// le cache déborde. Une entrée dessinée cette frame ne part jamais : AddImage
// n'a fait que noter sa poignée, le rendu vient après.
void EvictIfNeeded() {
  while (g_cache.size() > static_cast<size_t>(kMaxEntries)) {
    auto victim = g_cache.end();
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
      if (it->second.last_frame == g_frame) continue;
      if (victim == g_cache.end() || it->second.use_seq < victim->second.use_seq) victim = it;
    }
    if (victim == g_cache.end()) return;  // tout sert cette frame : on déborde plutôt que casser
    QueueRelease(victim->second.tex);
    g_cache.erase(victim);
  }
}

}  // namespace

void BeginFrame() {
  if (g_epoch_watch.Changed()) {
    // Poignées d'un device disparu : on les LÂCHE, sans Release.
    g_cache.clear();
  }
  g_frame = CurrentFrame();
  g_budget = kLoadsPerFrame;
}

Thumb Get(uint32_t card_id, const char* illust_path, bool grey, int max_w, int max_h) {
  Entry& e = g_cache[KeyOf(card_id, grey, max_w, max_h)];
  e.last_frame = g_frame;
  e.use_seq = ++g_seq;

  if (!e.loaded && !e.missing) {
    if (g_budget <= 0) return {nullptr, 0, 0, true};
    --g_budget;
    Load(&e, illust_path, grey, std::max(1, max_w), std::max(1, max_h));
    if (e.loaded) EvictIfNeeded();
    if (!e.loaded && !e.missing) return {nullptr, 0, 0, true};
  }
  if (e.missing) return {nullptr, 0, 0, false};
  return {e.tex, e.w, e.h, false};
}

void FlushReleases() {
  const int      frame = CurrentFrame();
  const unsigned epoch = Overlay_DeviceEpoch();
  size_t keep = 0;
  for (const PendingRelease& p : g_pending) {
    if (p.epoch != epoch) continue;  // device disparu : lâchée sans Release
    if (frame > p.frame) {
      Overlay_ReleaseTexture(p.tex);
      continue;
    }
    g_pending[keep++] = p;
  }
  g_pending.resize(keep);
}

}  // namespace cardthumb
}  // namespace ro
