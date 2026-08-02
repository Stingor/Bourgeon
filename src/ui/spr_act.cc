#include "ui/spr_act.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "utils/log_console.h"

namespace ro {
namespace spract {
namespace {

// ── Le VFS du client (client 20250716, base 0x400000) ────────────────────────
//
// `FileMgr_LoadToMemory(g_FileMgr, path, &size, disk_only)` rend un tampon
// alloué par VirtualAlloc, ou nullptr. Avec `disk_only == 0`, il essaie le
// disque ET les GRF (l'ordre dépend d'un drapeau interne : le patcheur pose
// « disque d'abord », cf. [[reference_grf_loading_patcher]]) — donc un override
// posé dans `data\` prime, exactement comme pour le client lui-même.
//
// La libération passe OBLIGATOIREMENT par `FileMgr_FreeBuffer`
// (= VirtualFree MEM_RELEASE) : le tampon ne vient pas du tas C++, `free` ou
// `delete[]` dessus corromprait le processus. C'est ce que fait le natif dans
// `ResFileStream_Close` (0x00573060).
constexpr uintptr_t kFileMgr      = 0x0159d410;  // g_FileMgr (l'OBJET)
constexpr uintptr_t kLoadToMemory = 0x00a88ab0;  // __thiscall(mgr, path, DWORD*, char)
constexpr uintptr_t kFreeBuffer   = 0x00a892c0;  // __stdcall(void*)
// Enveloppe zlib du client : (src, srcLen, dst, dstCap) -> taille produite, ou
// -1. Sert aux .spr v3.2, dont les images Bgra32 sont compressées. Fonction
// standard, pas une structure interne : elle ne bougera pas d'un client à
// l'autre autrement que d'adresse.
constexpr uintptr_t kZlibDecompress = 0x00573fc0;  // __cdecl

using LoadToMemoryFn = void* (__fastcall*)(void*, void*, const char*, DWORD*, char);
using FreeBufferFn   = int   (__stdcall*)(void*);
using ZlibDecompFn   = int   (__cdecl*)(const void*, int, void*, int);

// Garde-fous : un fichier corrompu (ou un mauvais fichier tout court) ne doit
// jamais faire allouer des gigaoctets ni boucler des milliards de fois.
constexpr int kMaxImages   = 8192;
constexpr int kMaxDim      = 4096;
constexpr int kMaxActions  = 4096;
constexpr int kMaxFrames   = 8192;
constexpr int kMaxLayers   = 512;
constexpr int kMaxAnchors  = 256;
constexpr int kMaxSounds   = 4096;

// Lecteur d'octets borné. Toute lecture hors limites pose `bad` et rend zéro :
// les parseurs testent `bad` aux points de contrôle plutôt qu'après chaque
// champ, ce qui garde le code lisible sans jamais sortir du tampon.
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : d_(data), n_(size) {}

  bool     bad() const { return bad_; }
  size_t   pos() const { return p_; }
  size_t   size() const { return n_; }
  bool     eof() const { return p_ >= n_; }
  void     Seek(size_t p) { if (p > n_) { bad_ = true; p = n_; } p_ = p; }
  void     Skip(size_t n) { Seek(p_ + n); }

  uint8_t  U8()  { uint8_t v = 0;  Take(&v, 1); return v; }
  uint16_t U16() { uint16_t v = 0; Take(&v, 2); return v; }
  int32_t  I32() { int32_t v = 0;  Take(&v, 4); return v; }
  float    F32() { float v = 0.0f; Take(&v, 4); return v; }

  // Copie `n` octets. Rend false (et ne touche pas `out`) si ça déborde.
  bool Bytes(void* out, size_t n) {
    if (bad_ || n > n_ - p_) { bad_ = true; return false; }
    std::memcpy(out, d_ + p_, n);
    p_ += n;
    return true;
  }
  // Pointeur direct sur `n` octets, sans copie. nullptr si ça déborde.
  const uint8_t* Raw(size_t n) {
    if (bad_ || n > n_ - p_) { bad_ = true; return nullptr; }
    const uint8_t* r = d_ + p_;
    p_ += n;
    return r;
  }

 private:
  template <typename T>
  void Take(T* v, size_t n) {
    if (bad_ || n > n_ - p_) { bad_ = true; return; }
    std::memcpy(v, d_ + p_, n);
    p_ += n;
  }
  const uint8_t* d_;
  size_t n_;
  size_t p_ = 0;
  bool   bad_ = false;
};

// ── RLE des images palettisées (.spr >= 2.1) ─────────────────────────────────
// Encodage minimal : un octet non nul est un pixel, un octet nul est suivi d'un
// COMPTE de pixels transparents à sauter. Port exact de `Rle.Decompress` de
// GRF Editor — y compris sa tolérance : un flux qui s'arrête avant la fin laisse
// le reste à zéro (index 0 = transparent), il n'est pas traité comme une erreur.
void RleDecompress(const uint8_t* src, size_t src_len, uint8_t* dst,
                   size_t dst_len) {
  size_t out = 0;
  for (size_t k = 0; k < src_len && out < dst_len; ++k) {
    const uint8_t b = src[k];
    if (b != 0) {
      dst[out++] = b;
      continue;
    }
    if (++k >= src_len) break;   // 0 en dernier octet : compte manquant
    out += src[k];
    if (out > dst_len) out = dst_len;
  }
}

// Inflate zlib par le natif. Fonction séparée pour garder le SEH loin de tout
// code qui alloue : un __try dans une fonction qui manipule des conteneurs C++
// est refusé par MSVC dès qu'un temporaire a un destructeur.
int ZlibInflate(const void* src, int src_len, void* dst, int dst_cap) {
  __try {
    return reinterpret_cast<ZlibDecompFn>(kZlibDecompress)(src, src_len, dst,
                                                           dst_cap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// Image brute lue dans le fichier, avant conversion.
struct RawFrame {
  int w = 0, h = 0;
  std::vector<uint8_t> data;
};

bool ReadRawIndexed(Reader* r, int version, RawFrame* out) {
  out->w = r->U16();
  out->h = r->U16();
  if (r->bad() || out->w < 0 || out->h < 0 ||
      out->w > kMaxDim || out->h > kMaxDim) return false;
  size_t len;
  if (version >= 22)      len = static_cast<uint32_t>(r->I32());
  else if (version >= 21) len = r->U16();
  else                    len = static_cast<size_t>(out->w) * out->h;
  if (r->bad() || len > r->size()) return false;
  out->data.resize(len);
  return len == 0 || r->Bytes(out->data.data(), len);
}

bool ReadRawBgra(Reader* r, int version, RawFrame* out) {
  out->w = r->U16();
  out->h = r->U16();
  if (r->bad() || out->w < 0 || out->h < 0 ||
      out->w > kMaxDim || out->h > kMaxDim) return false;
  const size_t raw_len = static_cast<size_t>(out->w) * out->h * 4;
  if (version >= 32) {
    const size_t zlen = static_cast<uint32_t>(r->I32());
    if (r->bad() || zlen > r->size()) return false;
    const uint8_t* z = r->Raw(zlen);
    if (!z) return false;
    out->data.assign(raw_len, 0);
    if (raw_len == 0) return true;
    // Le tampon est pré-rempli de zéros : un flux tronqué laisse la fin
    // transparente plutôt que de perdre l'image entière — même tolérance que
    // pour le RLE.
    return ZlibInflate(z, static_cast<int>(zlen), out->data.data(),
                       static_cast<int>(raw_len)) >= 0;
  }
  if (raw_len > r->size()) return false;
  out->data.resize(raw_len);
  return raw_len == 0 || r->Bytes(out->data.data(), raw_len);
}

}  // namespace

const Image* Resource::Get(int index, int type) const {
  if (index < 0) return nullptr;
  const std::vector<Image>& list = (type == 0) ? indexed : bgra;
  if (static_cast<size_t>(index) >= list.size()) return nullptr;
  return &list[index];
}

bool ReadFile(const char* path, std::vector<uint8_t>* out) {
  if (!path || !*path || !out) return false;
  out->clear();
  void* buf = nullptr;
  DWORD size = 0;
  __try {
    buf = reinterpret_cast<LoadToMemoryFn>(kLoadToMemory)(
        reinterpret_cast<void*>(kFileMgr), nullptr, path, &size, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) { buf = nullptr; }
  if (!buf) return false;

  // ⚠ L'allocation se fait HORS du __try : mélanger une allocation C++ (qui peut
  // lever) et un gestionnaire SEH dans la même fonction est refusé par MSVC dès
  // qu'un objet local a un destructeur, et brouille l'intention. Ici le SEH ne
  // couvre que ce qu'il doit couvrir : la lecture du tampon natif.
  bool ok = false;
  if (size > 0 && size < 64u * 1024u * 1024u) {
    out->resize(size);
    __try {
      std::memcpy(out->data(), buf, size);
      ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  }

  __try {
    reinterpret_cast<FreeBufferFn>(kFreeBuffer)(buf);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  if (!ok) out->clear();
  return ok;
}

// ── .spr ─────────────────────────────────────────────────────────────────────
//
// "SP" | minor(u8) | major(u8)
//   v >= 3.2 : nBgra32 (i32)          -> pas de section palettisée
//   v >= 2.0 : nIndexed8 (u16), nBgra32 (u16)
//   v <  2.0 : nIndexed8 (u16)        -> les images commencent à l'offset 6
// puis les images palettisées, puis les Bgra32, puis (s'il y a des palettisées)
// la palette : 1024 octets RGBA, TOUJOURS aux 1024 derniers octets du fichier.
bool ParseSpr(const uint8_t* data, size_t size, Resource* out) {
  if (!data || !out || size < 8) return false;
  if (data[0] != 'S' || data[1] != 'P') return false;
  const int minor = data[2];
  const int major = data[3];
  const int v = major * 10 + minor;
  out->spr_version = v;

  Reader r(data, size);
  int n_idx = 0, n_bgra = 0;
  if (v >= 32) {
    r.Seek(4);
    n_bgra = r.I32();
    r.Seek(8);
  } else if (v >= 20) {
    r.Seek(4);
    n_idx  = r.U16();
    n_bgra = r.U16();
    r.Seek(8);
  } else {
    r.Seek(4);
    n_idx = r.U16();
    r.Seek(6);
  }
  if (r.bad()) return false;
  if (n_idx < 0 || n_bgra < 0 || n_idx > kMaxImages || n_bgra > kMaxImages)
    return false;

  // La palette occupe le dernier kilo-octet : les images ne peuvent pas empiéter
  // dessus. Un compte d'images exagéré (fichier tronqué) s'arrête donc ici au
  // lieu de lire n'importe quoi — même garde que `_getImages` de la référence.
  if (n_idx > 0 && size < 1024) return false;
  const size_t pal_off = (n_idx > 0) ? (size - 1024) : size;

  std::vector<RawFrame> raw_idx, raw_bgra;
  raw_idx.reserve(static_cast<size_t>(n_idx));
  raw_bgra.reserve(static_cast<size_t>(n_bgra));
  for (int i = 0; i < n_idx; ++i) {
    if (r.pos() >= pal_off) break;
    RawFrame f;
    if (!ReadRawIndexed(&r, v, &f)) break;
    raw_idx.push_back(std::move(f));
  }
  for (int i = 0; i < n_bgra; ++i) {
    if (r.pos() >= pal_off) break;
    RawFrame f;
    if (!ReadRawBgra(&r, v, &f)) break;
    raw_bgra.push_back(std::move(f));
  }

  // ── Palette ────────────────────────────────────────────────────────────────
  // 256 entrées RGBA. L'octet d'alpha stocké dans le fichier ne veut rien dire
  // (il est laissé à 0 par la plupart des outils) : on force 255 partout, PUIS
  // on rend l'index 0 transparent — et lui seul. C'est la convention RO, et
  // c'est `Pal.FormatMode.NoTransparencyExceptFirstPixel` de la référence.
  uint32_t pal[256];
  for (int i = 0; i < 256; ++i) pal[i] = 0xFF000000u;
  if (!raw_idx.empty()) {
    const uint8_t* p = data + (size - 1024);
    for (int i = 0; i < 256; ++i) {
      const uint32_t a = (i == 0) ? 0u : 0xFFu;
      pal[i] = (a << 24) | (static_cast<uint32_t>(p[4 * i + 0]) << 16) |
               (static_cast<uint32_t>(p[4 * i + 1]) << 8) |
               static_cast<uint32_t>(p[4 * i + 2]);
    }
  }

  out->indexed.clear();
  out->indexed.reserve(raw_idx.size());
  for (const RawFrame& f : raw_idx) {
    Image img;
    img.w = f.w;
    img.h = f.h;
    const size_t count = static_cast<size_t>(f.w) * f.h;
    img.argb.assign(count, 0);
    if (count != 0) {
      std::vector<uint8_t> idx(count, 0);
      if (v >= 21)
        RleDecompress(f.data.data(), f.data.size(), idx.data(), count);
      else
        std::memcpy(idx.data(), f.data.data(),
                    f.data.size() < count ? f.data.size() : count);
      for (size_t i = 0; i < count; ++i) img.argb[i] = pal[idx[i]];
    }
    out->indexed.push_back(std::move(img));
  }

  out->bgra.clear();
  out->bgra.reserve(raw_bgra.size());
  for (const RawFrame& f : raw_bgra) {
    Image img;
    img.w = f.w;
    img.h = f.h;
    const size_t count = static_cast<size_t>(f.w) * f.h;
    img.argb.assign(count, 0);
    if (count != 0 && f.data.size() >= count * 4) {
      if (v >= 32) {
        // v3.2 : déjà en Bgra32, ligne du haut en premier -> copie directe.
        std::memcpy(img.argb.data(), f.data.data(), count * 4);
      } else {
        // 🔴 Avant la 3.2, la section Bgra32 est stockée LIGNES INVERSÉES et en
        // ordre A,B,G,R par pixel. C'est le seul endroit du format où l'ordre
        // des lignes n'est pas celui de l'image — le rater donne une image à
        // l'envers, et rater l'ordre des composantes donne les rayures qu'on
        // obtenait en devinant la disposition en mémoire du client.
        for (int y = 0; y < f.h; ++y) {
          const uint8_t* src = f.data.data() +
                               static_cast<size_t>(f.h - 1 - y) * f.w * 4;
          uint32_t* dst = img.argb.data() + static_cast<size_t>(y) * f.w;
          for (int x = 0; x < f.w; ++x) {
            const uint8_t a = src[4 * x + 0];
            const uint8_t b = src[4 * x + 1];
            const uint8_t g = src[4 * x + 2];
            const uint8_t rr = src[4 * x + 3];
            dst[x] = (static_cast<uint32_t>(a) << 24) |
                     (static_cast<uint32_t>(rr) << 16) |
                     (static_cast<uint32_t>(g) << 8) | b;
          }
        }
      }
    }
    out->bgra.push_back(std::move(img));
  }
  return true;
}

// ── .act ─────────────────────────────────────────────────────────────────────
//
// "AC" | minor(u8) | major(u8) | nActions(u16) | 10 octets réservés
// puis, par action : nFrames(i32), puis par image : 32 octets réservés,
// nLayers(i32), les calques, l'id de son, les ancres. Enfin la table des noms
// de sons (40 octets chacun) et la table des cadences (un float par action).
//
// `load_anchors` : voir le repli en fin de ParseAct.
static bool ParseActInner(const uint8_t* data, size_t size, Resource* out,
                          bool load_anchors) {
  Reader r(data, size);
  const int v = out->act_version;
  const bool spr_is_32 = out->spr_version >= 32;

  r.Seek(4);
  const int n_actions = r.U16();
  r.Seek(16);
  if (r.bad() || n_actions < 0 || n_actions > kMaxActions) return false;

  out->actions.clear();
  out->actions.reserve(static_cast<size_t>(n_actions));
  for (int a = 0; a < n_actions; ++a) {
    Action action;
    const int n_frames = r.I32();
    if (r.bad() || n_frames < 0 || n_frames > kMaxFrames) return false;
    action.frames.reserve(static_cast<size_t>(n_frames));

    for (int f = 0; f < n_frames; ++f) {
      r.Skip(32);
      Frame frame;
      const int n_layers = r.I32();
      if (r.bad() || n_layers < 0 || n_layers > kMaxLayers) return false;
      frame.layers.reserve(static_cast<size_t>(n_layers));

      for (int l = 0; l < n_layers; ++l) {
        Layer layer;
        if (v >= 26) {
          // 🔴 En 2.6 les offsets deviennent des FLOTTANTS, et la référence les
          // tronque vers le bas (`Math.Floor`). Les lire en entier donnerait des
          // positions absurdes (le motif binaire d'un float relu en int).
          layer.off_x = std::floor(r.F32());
          layer.off_y = std::floor(r.F32());
        } else {
          layer.off_x = static_cast<float>(r.I32());
          layer.off_y = static_cast<float>(r.I32());
        }
        layer.index  = r.I32();
        layer.mirror = r.I32() != 0;

        if (v >= 20) {
          const uint8_t cr = r.U8(), cg = r.U8(), cb = r.U8(), ca = r.U8();
          layer.tint = (static_cast<uint32_t>(ca) << 24) |
                       (static_cast<uint32_t>(cr) << 16) |
                       (static_cast<uint32_t>(cg) << 8) | cb;
          layer.scale_x = r.F32();
          layer.scale_y = layer.scale_x;   // avant 2.4, une seule échelle
          if (v >= 24) layer.scale_y = r.F32();
          layer.rotation = r.I32();
          layer.type     = r.I32();
          if (v >= 25) { layer.w = r.I32(); layer.h = r.I32(); }
          // Un .act 2.6 posé sur un .spr 3.2 n'a QUE des images Bgra32 : le
          // champ `type` du fichier n'est plus renseigné, il faut le forcer.
          if (v >= 26 && spr_is_32) layer.type = 1;
        }
        if (r.bad()) return false;
        if (layer.type < 0 || layer.type > 1) layer.type = 0;

        // Les dimensions écrites dans le .act ne font pas foi : la référence les
        // écrase systématiquement par celles de l'image du .spr, qui est la
        // seule source vraie (un .act réédité peut garder d'anciennes tailles).
        if (const Image* img = out->Get(layer.index, layer.type)) {
          layer.w = img->w;
          layer.h = img->h;
        }
        frame.layers.push_back(layer);
      }

      if (v >= 20) r.I32();      // id de son (non utilisé ici)
      if (load_anchors && v >= 23) {
        const int n_anchors = r.I32();
        if (r.bad() || n_anchors < 0 || n_anchors > kMaxAnchors) return false;
        r.Skip(static_cast<size_t>(n_anchors) * 16);
      }
      if (r.bad()) return false;
      action.frames.push_back(std::move(frame));
    }
    out->actions.push_back(std::move(action));
  }

  // ── Sons, puis cadences ────────────────────────────────────────────────────
  // Les cadences sont la DERNIÈRE table du fichier, et elle est souvent
  // tronquée : la référence retient alors la dernière valeur lue au lieu
  // d'échouer. On fait pareil — cette table est ce qui fixe la vitesse
  // d'animation, et une valeur approchée vaut mieux que pas d'animation.
  if (v >= 21) {
    const int n_sounds = r.I32();
    if (!r.bad() && n_sounds >= 0 && n_sounds <= kMaxSounds)
      r.Skip(static_cast<size_t>(n_sounds) * 40);
    if (v >= 22) {
      float speed = 1.0f;
      for (size_t i = 0; i < out->actions.size(); ++i) {
        if (r.bad() || r.size() - r.pos() < 4) {
          out->actions[i].speed = speed;
        } else {
          speed = r.F32();
          out->actions[i].speed = speed;
        }
      }
    }
  }
  return true;
}

bool ParseAct(const uint8_t* data, size_t size, Resource* out) {
  if (!data || !out || size < 6) return false;
  if (data[0] != 'A' || data[1] != 'C') return false;
  const int minor = data[2];
  const int major = data[3];
  out->act_version = major * 10 + minor;

  if (ParseActInner(data, size, out, /*load_anchors=*/true)) return true;

  // 🔴 Repli hérité de la référence (« Fix : 2015-04-06 ») : sur quelques .act
  // 2.3 et 2.4, le compte d'ancres vaut 0 alors que des octets d'ancre suivent
  // quand même. La lecture déraille alors en plein milieu. Le seul remède connu
  // est de tout relire en IGNORANT les ancres — qu'on n'utilise de toute façon
  // pas ici (elles servent à accrocher deux .act entre eux, coiffe sur tête).
  if (out->act_version == 23 || out->act_version == 24) {
    out->actions.clear();
    return ParseActInner(data, size, out, /*load_anchors=*/false);
  }
  return false;
}

bool Load(const char* spr_path, const char* act_path, Resource* out) {
  if (!spr_path || !*spr_path || !act_path || !*act_path || !out) return false;
  *out = Resource{};

  std::vector<uint8_t> spr_bytes, act_bytes;
  if (!ReadFile(spr_path, &spr_bytes)) {
    LogDiag("[SprAct] fichier introuvable : {}", spr_path);
    return false;
  }
  if (!ReadFile(act_path, &act_bytes)) {
    LogDiag("[SprAct] fichier introuvable : {}", act_path);
    return false;
  }
  // 🔴 Le .spr AVANT le .act, et ce n'est pas une préférence : le parseur .act
  // a besoin de `spr_version` (pour forcer le type des calques en 2.6/3.2) et
  // des dimensions des images (pour renseigner chaque calque).
  if (!ParseSpr(spr_bytes.data(), spr_bytes.size(), out)) {
    LogDiag("[SprAct] .spr illisible : {}", spr_path);
    return false;
  }
  if (!ParseAct(act_bytes.data(), act_bytes.size(), out)) {
    LogDiag("[SprAct] .act illisible : {}", act_path);
    return false;
  }
  out->ok = true;
  return true;
}

}  // namespace spract
}  // namespace ro
