#include "utils/img_decode.h"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "windowscodecs.lib")

namespace imgdec {
namespace {

constexpr int kMinFrameMs     = 20;   // délai 0 = « aussi vite que possible »
constexpr int kDefaultFrameMs = 100;

// Sous cette largeur, une capture d'interface n'est plus lisible : on cesse de
// réduire pour tenir dans le budget, et on retire des IMAGES à la place. C'est le
// dernier recours — perdre la fin d'un geste se voit moins qu'une bouillie.
constexpr int kMinAnimDim = 140;

// Réduction par MOYENNE DE BLOC, en place sur nos propres pixels BGRA. Pas de
// WIC ici : il faudrait recréer un bitmap et un scaler PAR IMAGE, alors que la
// moyenne d'un bloc tient en quinze lignes et travaille sur un tampon qu'on a
// déjà sous la main.
void DownscaleBgra(const std::vector<uint8_t>& src, int sw, int sh,
                   std::vector<uint8_t>* dst, int dw, int dh) {
  dst->assign(static_cast<size_t>(dw) * dh * 4u, 0);
  for (int y = 0; y < dh; ++y) {
    const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
    for (int x = 0; x < dw; ++x) {
      const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
      unsigned acc[4] = {0, 0, 0, 0};
      unsigned n = 0;
      for (int sy = y0; sy < y1 && sy < sh; ++sy) {
        for (int sx = x0; sx < x1 && sx < sw; ++sx) {
          const uint8_t* p = &src[(static_cast<size_t>(sy) * sw + sx) * 4u];
          // Prémultiplier par l'alpha pendant la moyenne : sans ça, un pixel
          // transparent (dont la couleur est arbitraire) déteindrait sur ses
          // voisins et cernerait les bords d'un halo.
          acc[0] += p[0] * p[3]; acc[1] += p[1] * p[3];
          acc[2] += p[2] * p[3]; acc[3] += p[3];
          ++n;
        }
      }
      uint8_t* d = &(*dst)[(static_cast<size_t>(y) * dw + x) * 4u];
      if (n == 0 || acc[3] == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
      d[0] = static_cast<uint8_t>(acc[0] / acc[3]);
      d[1] = static_cast<uint8_t>(acc[1] / acc[3]);
      d[2] = static_cast<uint8_t>(acc[2] / acc[3]);
      d[3] = static_cast<uint8_t>(acc[3] / n);
    }
  }
}

UINT MetaUint(IWICMetadataQueryReader* reader, const wchar_t* name, UINT def) {
  if (reader == nullptr) return def;
  PROPVARIANT v;
  PropVariantInit(&v);
  UINT out = def;
  if (SUCCEEDED(reader->GetMetadataByName(name, &v))) {
    if      (v.vt == VT_UI1) out = v.bVal;
    else if (v.vt == VT_UI2) out = v.uiVal;
    else if (v.vt == VT_UI4) out = v.ulVal;
  }
  PropVariantClear(&v);
  return out;
}

// Lit un fichier entier, plafonné. `out_error` reçoit la raison d'un refus.
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>* out,
                   std::string* out_error) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (out_error) *out_error = "fichier introuvable";
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0 || static_cast<size_t>(size) > kMaxFileBytes) {
    std::fclose(f);
    if (out_error) *out_error = "fichier vide ou trop gros";
    return false;
  }
  out->resize(static_cast<size_t>(size));
  const size_t read = std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  if (read != out->size()) {
    if (out_error) *out_error = "lecture interrompue";
    return false;
  }
  return true;
}

}  // namespace

bool DecodeStill(const uint8_t* data, size_t size, const Limits& limits,
                 std::vector<uint8_t>* out_bgra, int* out_w, int* out_h) {
  if (data == nullptr || size == 0) return false;

  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    return false;

  bool ok = false;
  IWICStream*            stream  = nullptr;
  IWICBitmapDecoder*     decoder = nullptr;
  IWICBitmapFrameDecode* frame   = nullptr;
  IWICBitmapScaler*      scaler  = nullptr;
  IWICFormatConverter*   conv    = nullptr;

  if (SUCCEEDED(factory->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                             static_cast<DWORD>(size))) &&
      SUCCEEDED(factory->CreateDecoderFromStream(
          stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
      SUCCEEDED(decoder->GetFrame(0, &frame))) {  // GIF animé : première image
    UINT sw = 0, sh = 0;
    if (SUCCEEDED(frame->GetSize(&sw, &sh)) && sw > 0 && sh > 0 &&
        sw <= static_cast<UINT>(limits.max_source_dim) &&
        sh <= static_cast<UINT>(limits.max_source_dim)) {
      // Réduction à la taille d'affichage, ratio préservé. Jamais d'agrandissement.
      double scale = 1.0;
      const UINT big = (sw > sh) ? sw : sh;
      if (big > static_cast<UINT>(limits.max_dim))
        scale = static_cast<double>(limits.max_dim) / static_cast<double>(big);
      UINT dw = static_cast<UINT>(sw * scale);
      UINT dh = static_cast<UINT>(sh * scale);
      if (dw == 0) dw = 1;
      if (dh == 0) dh = 1;

      IWICBitmapSource* source = frame;
      if (scale < 1.0 && SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
          SUCCEEDED(scaler->Initialize(frame, dw, dh,
                                       WICBitmapInterpolationModeFant))) {
        source = scaler;
      } else {
        dw = sw;
        dh = sh;
      }
      // BGRA32 : l'ordre d'octets qu'attend Overlay_CreateTextureARGB.
      // ⚠ Alpha DROIT (BGRA), surtout pas prémultiplié (PBGRA) : l'overlay mélange
      // en SRCALPHA/INVSRCALPHA, et du prémultiplié y ressortirait assombri sur
      // tout ce qui est semi-transparent — typiquement un PNG à bords adoucis.
      if (SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
          SUCCEEDED(conv->Initialize(source, GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        const size_t stride = static_cast<size_t>(dw) * 4u;
        out_bgra->resize(stride * dh);
        if (SUCCEEDED(conv->CopyPixels(nullptr, static_cast<UINT>(stride),
                                       static_cast<UINT>(out_bgra->size()),
                                       out_bgra->data()))) {
          *out_w = static_cast<int>(dw);
          *out_h = static_cast<int>(dh);
          ok = true;
        }
      }
    }
  }

  if (conv)    conv->Release();
  if (scaler)  scaler->Release();
  if (frame)   frame->Release();
  if (decoder) decoder->Release();
  if (stream)  stream->Release();
  factory->Release();
  return ok;
}

bool DecodeAnimation(const uint8_t* data, size_t size, const Limits& limits,
                     Animation* out) {
  if (data == nullptr || size == 0) return false;

  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    return false;

  bool ok = false;
  IWICStream*              stream  = nullptr;
  IWICBitmapDecoder*       decoder = nullptr;
  IWICMetadataQueryReader* global  = nullptr;

  if (SUCCEEDED(factory->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                             static_cast<DWORD>(size))) &&
      SUCCEEDED(factory->CreateDecoderFromStream(
          stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
    UINT count = 0;
    decoder->GetFrameCount(&count);
    if (count > 1) {
      // Le canevas est celui du LOGICAL SCREEN, pas de la première image : une
      // image peut être plus petite que la surface d'animation.
      UINT cw = 0, ch = 0;
      if (SUCCEEDED(decoder->GetMetadataQueryReader(&global))) {
        cw = MetaUint(global, L"/logscrdesc/Width", 0);
        ch = MetaUint(global, L"/logscrdesc/Height", 0);
      }
      IWICBitmapFrameDecode* f0 = nullptr;
      if ((cw == 0 || ch == 0) && SUCCEEDED(decoder->GetFrame(0, &f0))) {
        f0->GetSize(&cw, &ch);
      }
      if (f0) f0->Release();

      const size_t canvas_bytes = static_cast<size_t>(cw) * ch * 4u;
      const UINT   big = (cw > ch) ? cw : ch;
      // Taille de SORTIE : on compose à la taille NATIVE (il le faut, les images
      // ne sont que des rectangles de différences) mais on RÉDUIT avant de
      // garder — sans quoi le budget part en fumée. Un gif de 498x280 pèse
      // 558 Ko par image gardée en natif, soit 21 images dans 12 Mio ; à 256 px
      // la même animation coûte 262 Ko par image, et 45 images tiennent.
      double ascale = 1.0;
      if (big > static_cast<UINT>(limits.max_dim))
        ascale = static_cast<double>(limits.max_dim) / static_cast<double>(big);

      if (count > static_cast<UINT>(limits.max_frames))
        count = static_cast<UINT>(limits.max_frames);

      // 🔴 LE BUDGET RÉDUIT L'IMAGE, IL NE REFUSE PLUS L'ANIMATION. `max_dim` ne
      // suffit pas à prévoir le coût : à 360 px de côté long, un clip 16:9 pèse
      // 292 Ko par image et un clip CARRÉ 514 Ko — 1,75 fois plus, pour le même
      // réglage. Le second dépassait donc le plafond et retombait sur l'image
      // FIXE, sans que rien ne l'explique : c'est exactement ce qui est arrivé au
      // premier gif de tutoriel tourné en vrai (491x488, 60 images, 2026-09-05).
      //
      // On calcule donc l'échelle qui fait TENIR l'animation : le budget est une
      // surface totale, et une surface se divise par le carré de l'échelle, d'oû
      // la racine. L'auteur du gif n'a ainsi jamais à raisonner en octets.
      auto out_size = [&](double scale, int* w, int* h) {
        *w = std::max(1, static_cast<int>(cw * scale));
        *h = std::max(1, static_cast<int>(ch * scale));
        return static_cast<size_t>(*w) * (*h) * 4u;
      };
      int ow = 0, oh = 0;
      size_t out_bytes = out_size(ascale, &ow, &oh);
      if (count > 0 && out_bytes * count > limits.max_bytes) {
        // ⚠ On vise 99 % du budget, pas 100 : les dimensions sont des ENTIERS,
        // et l'arrondi peut faire repasser la surface juste au-dessus du plafond
        // — de 28 Ko sur un cas réel, ce qui coûtait la dernière image du geste.
        // Un pour cent de marge vaut mieux qu'une image perdue.
        constexpr double kFitMargin = 0.99;
        const double fit = std::sqrt(static_cast<double>(limits.max_bytes) /
                                     (static_cast<double>(out_bytes) * count));
        ascale *= fit * kFitMargin;
        // Plancher de lisibilité : en deçà, on rend des images plutôt que des
        // pixels. La troncature qui suit garde le début du geste.
        const double floor_scale =
            static_cast<double>(kMinAnimDim) / static_cast<double>(big);
        if (ascale < floor_scale && big > static_cast<UINT>(kMinAnimDim))
          ascale = floor_scale;
        out_bytes = out_size(ascale, &ow, &oh);
        if (out_bytes > 0 && out_bytes * count > limits.max_bytes) {
          const size_t fits = limits.max_bytes / out_bytes;
          count = static_cast<UINT>(std::max<size_t>(1, fits));
        }
      }

      if (cw > 0 && ch > 0 && big <= static_cast<UINT>(limits.max_source_dim) &&
          out_bytes > 0 && out_bytes * 2u <= limits.max_bytes) {

        std::vector<uint8_t> canvas(canvas_bytes, 0);
        std::vector<uint8_t> saved;   // pour la consigne « restaurer le précédent »
        size_t budget = 0;
        bool   failed = false;

        for (UINT i = 0; i < count && !failed; ++i) {
          IWICBitmapFrameDecode*   fr = nullptr;
          IWICFormatConverter*     cv = nullptr;
          IWICMetadataQueryReader* md = nullptr;
          if (FAILED(decoder->GetFrame(i, &fr))) { failed = true; break; }

          UINT fw = 0, fh = 0;
          fr->GetSize(&fw, &fh);
          fr->GetMetadataQueryReader(&md);
          const UINT left = MetaUint(md, L"/imgdesc/Left", 0);
          const UINT top  = MetaUint(md, L"/imgdesc/Top", 0);
          // Délai en centièmes de seconde. 0 = « le plus vite possible » : les
          // navigateurs y substituent 100 ms, on fait pareil (sinon le gif
          // clignote à la fréquence de rendu).
          const UINT delay_cs = MetaUint(md, L"/grctlext/Delay", 0);
          const UINT disposal = MetaUint(md, L"/grctlext/Disposal", 0);
          int delay_ms = static_cast<int>(delay_cs) * 10;
          if (delay_ms <= 0) delay_ms = kDefaultFrameMs;
          if (delay_ms < kMinFrameMs) delay_ms = kMinFrameMs;

          if (disposal == 3) saved = canvas;  // à restaurer APRÈS cette image

          std::vector<uint8_t> px;
          if (fw > 0 && fh > 0 &&
              SUCCEEDED(factory->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom))) {
            px.resize(static_cast<size_t>(fw) * fh * 4u);
            if (FAILED(cv->CopyPixels(nullptr, fw * 4,
                                      static_cast<UINT>(px.size()), px.data())))
              failed = true;
          } else {
            failed = true;
          }

          if (!failed) {
            // Report sur le canevas. La transparence GIF est binaire : un pixel
            // à alpha 0 LAISSE VOIR ce qui est dessous, il ne l'efface pas.
            for (UINT y = 0; y < fh; ++y) {
              const UINT cy = top + y;
              if (cy >= ch) break;
              for (UINT x = 0; x < fw; ++x) {
                const UINT cx = left + x;
                if (cx >= cw) break;
                const uint8_t* s = &px[(static_cast<size_t>(y) * fw + x) * 4u];
                if (s[3] == 0) continue;
                uint8_t* d = &canvas[(static_cast<size_t>(cy) * cw + cx) * 4u];
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
              }
            }
            budget += out_bytes;  // ce qu'on GARDE, pas ce qu'on compose
            if (budget > limits.max_bytes) failed = true;
          }

          if (!failed) {
            // Instantané du canevas complet, RÉDUIT à la taille de sortie.
            if (ascale < 1.0) {
              // ⚠ PAS `small` : Windows.h en fait une macro (`#define small
              // char`), et la déclaration devenait « std::vector<uint8_t> char ».
              // Même famille que min/max/near/far.
              std::vector<uint8_t> reduced;
              DownscaleBgra(canvas, static_cast<int>(cw), static_cast<int>(ch),
                            &reduced, ow, oh);
              out->frames.push_back(std::move(reduced));
            } else {
              out->frames.push_back(canvas);
            }
            out->delays_ms.push_back(delay_ms);

            // Consigne d'effacement, appliquée pour l'image SUIVANTE.
            if (disposal == 2) {  // remettre le rectangle au fond (transparent)
              for (UINT y = 0; y < fh; ++y) {
                const UINT cy = top + y;
                if (cy >= ch) break;
                for (UINT x = 0; x < fw; ++x) {
                  const UINT cx = left + x;
                  if (cx >= cw) break;
                  uint8_t* d = &canvas[(static_cast<size_t>(cy) * cw + cx) * 4u];
                  d[0] = d[1] = d[2] = d[3] = 0;
                }
              }
            } else if (disposal == 3 && !saved.empty()) {
              canvas = saved;
            }
          }

          if (md) md->Release();
          if (cv) cv->Release();
          fr->Release();
        }

        if (!failed && out->frames.size() > 1) {
          out->w = ow;  // la taille RÉDUITE : c'est celle des pixels rendus
          out->h = oh;
          ok = true;
        } else {
          out->frames.clear();
          out->delays_ms.clear();
        }
      }
    }
  }

  if (global)  global->Release();
  if (decoder) decoder->Release();
  if (stream)  stream->Release();
  factory->Release();
  return ok;
}

bool DecodeFile(const std::string& path, const Limits& limits, Animation* out,
                std::string* out_error) {
  std::vector<uint8_t> bytes;
  if (!ReadWholeFile(path, &bytes, out_error)) return false;

  // Animé d'abord ; sinon l'image fixe, qui elle sait aussi RÉDUIRE les grandes
  // images — d'où l'ordre : un gif trop long pour être animé y retombe et
  // s'affiche quand même, figé, plutôt que de ne rien montrer.
  if (DecodeAnimation(bytes.data(), bytes.size(), limits, out)) {
    if (out_error) out_error->clear();
    return true;
  }

  // L'animation n'a pas pris : on montrera une image FIXE. Le dire, même si le
  // reste réussit — un gif immobile passe sinon pour une panne d'affichage, et
  // c'est par cette question-là que le problème est remonté.
  if (out_error) *out_error = "animé illisible : affiché en image fixe";

  std::vector<uint8_t> single;
  int w = 0, h = 0;
  if (DecodeStill(bytes.data(), bytes.size(), limits, &single, &w, &h)) {
    out->frames.clear();
    out->delays_ms.clear();
    out->frames.push_back(std::move(single));
    out->delays_ms.push_back(0);
    out->w = w;
    out->h = h;
    return true;
  }

  if (out_error) *out_error = "format illisible, ou image plus grande que la limite";
  return false;
}

}  // namespace imgdec
