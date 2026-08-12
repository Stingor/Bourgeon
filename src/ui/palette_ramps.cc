#include "ui/palette_ramps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ro {
namespace {

// ── Seuils ───────────────────────────────────────────────────────────────────
// 🔴 Transcrits de `tools/palette_ramps.py`, où ils ont été calibrés sur les 421
// corps du client. Les changer ici SEUL ferait diverger l'outil de mesure et le
// jeu ; les changer sans refaire la passe complète ferait apparaître des rampes
// fantômes ou disparaître des pièces entières du costume.
// 🔴 UN seul index suffit à faire une « rampe ». Ça semble contredire le mot,
// et ça a d'ailleurs valu 3 pendant un temps, au motif que deux teintes ne font
// pas un dégradé. C'était une erreur, mesurée sur les 421 corps : beaucoup de
// palettes sont ÉPARSES et peignent des aplats de une ou deux couleurs, qu'on
// écartait alors qu'ils couvrent de vraies pièces du costume.
//
//   seuil 3 : 85,7 % des pixels réglables en moyenne, 36 corps sous 70 %
//   seuil 2 : 87,7 %,                                 14 corps sous 70 %
//   seuil 1 : 91,0 %,                                  9 corps sous 70 %
//
// Sans contrepartie : la couverture est calculée sur les 8 pièces RETENUES, donc
// accepter les aplats ne chasse aucune vraie pièce du classement. Le vrai filtre
// anti-bruit reste `kMinPixels`, qui écarte ce qui ne se voit pas.
constexpr int kMinLength = 1;
constexpr double kHueTolerance = 40.0;   // degrés
constexpr double kValueRebound = 0.08;   // remontée de luminosité tolérée
constexpr double kNeutralSat = 0.12;     // en dessous, la teinte ne veut rien dire
constexpr int kMinPixels = 200;      // sous ce seuil, pas de curseur

struct Hsv {
  double h = 0.0;  // 0..1
  double s = 0.0;
  double v = 0.0;
};

// Transcription de `colorsys.rgb_to_hsv` — même découpage en secteurs, pour que
// l'outil Python et le jeu trouvent les mêmes rampes sur les mêmes octets.
Hsv RgbToHsv(int r, int g, int b) {
  const double rd = r / 255.0, gd = g / 255.0, bd = b / 255.0;
  const double maxc = (std::max)((std::max)(rd, gd), bd);
  const double minc = (std::min)((std::min)(rd, gd), bd);
  Hsv out;
  out.v = maxc;
  if (maxc == minc) return out;  // gris pur : teinte et saturation nulles
  const double range = maxc - minc;
  out.s = range / maxc;
  const double rc = (maxc - rd) / range;
  const double gc = (maxc - gd) / range;
  const double bc = (maxc - bd) / range;
  double h;
  if (rd == maxc) h = bc - gc;
  else if (gd == maxc) h = 2.0 + rc - bc;
  else h = 4.0 + gc - rc;
  h = std::fmod(h / 6.0, 1.0);
  if (h < 0.0) h += 1.0;
  out.h = h;
  return out;
}

// Transcription de `colorsys.hsv_to_rgb`.
void HsvToRgb(double h, double s, double v, int* r, int* g, int* b) {
  double rd, gd, bd;
  if (s == 0.0) {
    rd = gd = bd = v;
  } else {
    const double h6 = h * 6.0;
    int i = static_cast<int>(h6);
    const double f = h6 - i;
    const double p = v * (1.0 - s);
    const double q = v * (1.0 - s * f);
    const double t = v * (1.0 - s * (1.0 - f));
    i %= 6;
    switch (i) {
      case 0: rd = v; gd = t; bd = p; break;
      case 1: rd = q; gd = v; bd = p; break;
      case 2: rd = p; gd = v; bd = t; break;
      case 3: rd = p; gd = q; bd = v; break;
      case 4: rd = t; gd = p; bd = v; break;
      default: rd = v; gd = p; bd = q; break;
    }
  }
  const auto clamp255 = [](double x) {
    const int n = static_cast<int>(x * 255.0 + 0.5);
    return n < 0 ? 0 : (n > 255 ? 255 : n);
  };
  *r = clamp255(rd);
  *g = clamp255(gd);
  *b = clamp255(bd);
}

Hsv EntryHsv(const uint8_t* palette, int index) {
  return RgbToHsv(palette[4 * index + 0], palette[4 * index + 1],
                  palette[4 * index + 2]);
}

// Écart CIRCULAIRE entre deux teintes, en degrés (0..180). Sans le repliement,
// une rampe rouge qui passe par 359° puis 2° serait coupée en deux.
double HueGap(double a, double b) {
  double d = std::fmod(std::fabs(a - b), 360.0);
  return d > 180.0 ? 360.0 - d : d;
}

// Teinte de l'index le plus SATURÉ de la rampe — pas la moyenne : une moyenne
// circulaire sur des gris donne un angle arbitraire, alors que l'index le plus
// saturé porte la couleur que le joueur croit voir.
int DominantHue(const uint8_t* palette, const int usage[256], int begin,
                int end) {
  double best = -1.0, max_sat = -1.0;
  for (int i = begin; i < end; ++i) {
    if (usage[i] == 0) continue;
    const Hsv c = EntryHsv(palette, i);
    if (c.s > max_sat) {
      max_sat = c.s;
      best = c.h * 360.0;
    }
  }
  if (max_sat < kNeutralSat || best < 0.0) return -1;
  const int deg = static_cast<int>(best + 0.5);
  return deg >= 360 ? deg - 360 : deg;
}

}  // namespace

void CountIndexUsage(const spract::Resource& res, int usage[256]) {
  std::memset(usage, 0, sizeof(int) * 256);
  for (const spract::Image& img : res.indexed) {
    for (uint8_t idx : img.index) ++usage[idx];
  }
  usage[0] = 0;  // l'index 0 est le transparent : jamais colorié
}

namespace {

// Somme R+V+B d'une entrée. En dessous de `kBlackSum`, on parle de « noir » :
// le seuil est volontairement bas (24 sur 765) pour ne prendre que ce qui est
// visuellement indistinguable du noir.
constexpr int kBlackSum = 24;

int Luma(const uint8_t* pal, int i) {
  return pal[4 * i] + pal[4 * i + 1] + pal[4 * i + 2];
}

// Une entrée dont les QUATRE octets sont nuls : jamais écrite par l'outil qui a
// produit le .pal. C'est le séparateur naturel entre deux pièces du costume.
bool IsUnset(const uint8_t* pal, int i) {
  return Luma(pal, i) == 0 && pal[4 * i + 3] == 0;
}

}  // namespace

bool MergeServerPalette(const uint8_t* sprite, size_t sprite_size,
                        const uint8_t* server, size_t server_size,
                        uint8_t* out, size_t out_size) {
  if (!sprite || sprite_size < 1024 || !out || out_size < 1024) return false;
  // Sans palette serveur, la palette du sprite EST le résultat : c'est ce que
  // le client afficherait si aucune couleur de vêtement n'était posée.
  if (!server || server_size < 1024) {
    std::memcpy(out, sprite, 1024);
    return true;
  }

  // On part du serveur — c'est lui qui porte la couleur que le joueur voit —
  // puis on ne rapatrie du sprite que les trous.
  std::memcpy(out, server, 1024);

  int i = 1;  // l'index 0 est le transparent : il ne se peint jamais
  while (i < 256) {
    if (IsUnset(server, i)) {
      std::memcpy(out + 4 * i, sprite + 4 * i, 4);
      ++i;
      continue;
    }
    // Une plage définie va d'ici jusqu'au prochain séparateur.
    int j = i;
    int brightest = 0;
    while (j < 256 && !IsUnset(server, j)) {
      const int l = Luma(server, j);
      if (l > brightest) brightest = l;
      ++j;
    }
    // Toute la plage est noire : ce n'est pas une ombre, c'est du remplissage.
    // Une plage qui MONTE vers du clair, elle, est un vrai dégradé — son noir
    // final est voulu, et c'est souvent le contour du sprite.
    if (brightest <= kBlackSum)
      std::memcpy(out + 4 * i, sprite + 4 * i, 4 * static_cast<size_t>(j - i));
    i = j;
  }
  return true;
}

int DetectRamps(const uint8_t* palette, size_t palette_size,
                const int usage[256], PaletteRamp* out, int max_out) {
  if (!palette || palette_size < 1024 || !usage || !out || max_out <= 0)
    return 0;

  PaletteRamp found[256];
  int count = 0;

  int begin = -1;
  double hue_ref = -1.0;    // < 0 : la rampe n'a pas encore de teinte
  double prev_value = 0.0;

  // Clôt la rampe courante sur [begin, end) si elle mérite un curseur.
  const auto close = [&](int end) {
    if (begin < 0 || count >= 256) return;
    const int length = end - begin;
    if (length < kMinLength) return;
    int pixels = 0;
    for (int i = begin; i < end; ++i) pixels += usage[i];
    if (pixels < kMinPixels) return;
    PaletteRamp& r = found[count++];
    r.start = static_cast<uint8_t>(begin);
    r.length = static_cast<uint8_t>(length);
    r.pixels = pixels;
    r.hue = DominantHue(palette, usage, begin, end);
  };

  for (int i = 1; i < 256; ++i) {
    if (usage[i] == 0) {          // un trou coupe toujours
      close(i);
      begin = -1;
      hue_ref = -1.0;
      continue;
    }
    const Hsv c = EntryHsv(palette, i);
    const double hue_deg = c.h * 360.0;

    if (begin < 0) {
      begin = i;
      hue_ref = (c.s >= kNeutralSat) ? hue_deg : -1.0;
      prev_value = c.v;
      continue;
    }

    // Deux ruptures possibles, et il faut les DEUX : un dégradé RO va du clair
    // au foncé dans une teinte stable.
    bool cut = false;
    if (c.s >= kNeutralSat) {
      if (hue_ref < 0.0) hue_ref = hue_deg;        // la rampe démarrait en gris
      else if (HueGap(hue_deg, hue_ref) > kHueTolerance) cut = true;
    }
    if (c.v > prev_value + kValueRebound) cut = true;  // retour au clair

    if (cut) {
      close(i);
      begin = i;
      hue_ref = (c.s >= kNeutralSat) ? hue_deg : -1.0;
    }
    prev_value = c.v;
  }
  close(256);

  // Les plus VISIBLES d'abord : c'est l'ordre des curseurs de l'éditeur, et le
  // plafond doit couper les rampes anecdotiques, pas la tunique. Tri STABLE, de
  // sorte qu'à surface égale l'ordre reste celui des index — sans quoi deux
  // clients pourraient numéroter les rampes différemment et une recette
  // désignerait la mauvaise pièce du costume.
  std::stable_sort(found, found + count,
                   [](const PaletteRamp& a, const PaletteRamp& b) {
                     return a.pixels > b.pixels;
                   });

  const int n = (count < max_out) ? count : max_out;
  for (int i = 0; i < n; ++i) out[i] = found[i];
  return n;
}

int DetectRamps(const spract::Resource& res, PaletteRamp* out, int max_out) {
  if (res.palette.size() < 1024) return 0;
  int usage[256];
  CountIndexUsage(res, usage);
  return DetectRamps(res.palette.data(), res.palette.size(), usage, out,
                     max_out);
}

namespace {

// L'index « représentatif » d'une rampe : son milieu.
int RampMidIndex(const PaletteRamp& ramp) {
  const int i = ramp.start + ramp.length / 2;
  return (i < 0) ? 0 : (i > 255 ? 255 : i);
}

}  // namespace

uint32_t RampColor(const uint8_t* palette, size_t palette_size,
                   const PaletteRamp& ramp) {
  if (!palette || palette_size < 1024) return 0;
  const int i = RampMidIndex(ramp);
  return (static_cast<uint32_t>(palette[4 * i]) << 16) |
         (static_cast<uint32_t>(palette[4 * i + 1]) << 8) |
         static_cast<uint32_t>(palette[4 * i + 2]);
}

RampAdjust AdjustToReach(const uint8_t* palette, size_t palette_size,
                         const PaletteRamp& ramp, uint32_t target) {
  RampAdjust adj;
  if (!palette || palette_size < 1024) return adj;
  const int i = RampMidIndex(ramp);
  const Hsv from = EntryHsv(palette, i);
  const Hsv to = RgbToHsv(static_cast<int>((target >> 16) & 0xFF),
                          static_cast<int>((target >> 8) & 0xFF),
                          static_cast<int>(target & 0xFF));

  // 🔴 Le choix direct produit toujours un réglage ABSOLU. En relatif, la
  // couleur demandée serait souvent inatteignable : la saturation étant un
  // facteur, une pièce terne ou grise ne peut pas devenir vive, et le joueur
  // désignerait un rouge franc pour obtenir un rose délavé.
  adj.absolute = 1;
  int deg = static_cast<int>(to.h * 360.0 + 0.5);
  if (deg < 0) deg = 0;
  if (deg > 359) deg -= 360;
  adj.hue = static_cast<int16_t>(deg);

  int sat = static_cast<int>(to.s * 100.0 + 0.5);
  adj.sat = static_cast<int8_t>(sat < 0 ? 0 : (sat > 100 ? 100 : sat));

  // La luminosité reste relative : c'est le rapport entre la clarté demandée et
  // celle d'origine, pour que le dégradé garde son modelé.
  if (from.v > 0.001) {
    const double pct = (to.v / from.v - 1.0) * 100.0;
    const double c = pct < -100.0 ? -100.0 : (pct > 100.0 ? 100.0 : pct);
    adj.val = static_cast<int8_t>(c < 0 ? c - 0.5 : c + 0.5);
  }
  return adj;
}

bool ApplyRecipe(const uint8_t* palette, size_t palette_size,
                 const PaletteRamp* ramps, int ramp_count,
                 const PaletteRecipe& recipe, uint8_t* out, size_t out_size) {
  if (!palette || palette_size < 1024 || !out || out_size < 1024) return false;
  std::memcpy(out, palette, 1024);
  if (!ramps || ramp_count <= 0) return true;

  const int n = (ramp_count < kMaxRamps) ? ramp_count : kMaxRamps;
  for (int r = 0; r < n; ++r) {
    const RampAdjust& adj = recipe.ramps[r];
    if (adj.IsNeutral()) continue;
    const int begin = ramps[r].start;
    const int end = begin + ramps[r].length;
    for (int i = begin; i < end && i < 256; ++i) {
      const Hsv c = EntryHsv(palette, i);
      double h, s;
      if (adj.absolute) {
        // Teinte et saturation IMPOSÉES : la rampe devient une couleur unie que
        // seule la lumière du sprite module. C'est ce qui permet à une pièce
        // terne d'atteindre une couleur vive — impossible par multiplication.
        h = static_cast<double>(adj.hue);
        s = adj.sat / 100.0;
      } else {
        h = std::fmod(c.h * 360.0 + adj.hue, 360.0);
        s = c.s * (1.0 + adj.sat / 100.0);
      }
      if (h < 0.0) h += 360.0;
      // La luminosité reste TOUJOURS relative, dans les deux modes : c'est elle
      // qui porte le modelé du dégradé, et l'imposer aplatirait le volume.
      double v = c.v * (1.0 + adj.val / 100.0);
      s = (s < 0.0) ? 0.0 : ((s > 1.0) ? 1.0 : s);
      v = (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v);
      int rr, gg, bb;
      HsvToRgb(h / 360.0, s, v, &rr, &gg, &bb);
      out[4 * i + 0] = static_cast<uint8_t>(rr);
      out[4 * i + 1] = static_cast<uint8_t>(gg);
      out[4 * i + 2] = static_cast<uint8_t>(bb);
      // L'octet d'alpha du fichier ne veut rien dire : on le laisse INTACT
      // plutôt que d'inventer une valeur (cf. `DecodePalette`).
    }
  }
  return true;
}

}  // namespace ro
