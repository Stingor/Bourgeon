// Validation croisée de `src/ui/palette_ramps.cc` contre sa référence Python.
//
// ── Pourquoi ce programme existe ─────────────────────────────────────────────
// 🔴 Ce qui circule entre deux clients ne porte AUCUNE couleur : rien que des
// rangs de rampes et des décalages HSV. Chaque client recalcule les couleurs sur
// le `.spr` qu'il possède déjà. Si l'outil de mesure (Python) et le jeu (C++)
// divergent d'un seul index, deux joueurs ne voient pas le même personnage — et
// l'écart est INVISIBLE en revue de code, puisque les deux fichiers disent la
// même chose en deux langages. Seule la sortie tranche.
//
// ── Comment s'en servir ──────────────────────────────────────────────────────
//     python tools/palette_ramps.py --croiser build\xcheck
//     cl /std:c++17 /EHsc /O2 /I src tools\xcheck_ramps.cc src\ui\palette_ramps.cc
//     xcheck_ramps.exe build\xcheck\cas.bin > build\xcheck\cpp.txt
//     fc build\xcheck\python.txt build\xcheck\cpp.txt
// Les deux fichiers doivent être IDENTIQUES, à l'octet près.
//
// ⚠ Il ne lit ni GRF ni `.spr` : les entrées sont figées dans `cas.bin` par la
// passe Python. C'est voulu — le VFS du client passe par des fonctions natives
// que ce programme n'a pas, et refaire un lecteur de GRF ici introduirait
// justement le genre de divergence qu'on cherche à exclure.

#define _CRT_SECURE_NO_WARNINGS  // `fopen` sur un chemin passé en argument

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ui/palette_ramps.h"

namespace {

// Miroir exact de `recette_test()` dans tools/palette_ramps.py. Il ne s'agit pas
// d'un goût de joueur mais d'un balayage : les deux modes, les deux signes, des
// saturations qui débordent et des luminosités qui s'écroulent, pour que la
// comparaison traverse toutes les branches d'`ApplyRecipe`.
//
// 🔴 Redérivé plutôt que transmis. Faire voyager les réglages dans `cas.bin`
// rendrait le test complice de lui-même : si les deux côtés calculent la même
// chose à partir du rang, on vérifie AUSSI que le rang veut dire la même chose
// des deux côtés — ce qui est précisément l'objet de la v6.
ro::RampAdjust TestAdjust(int n) {
  ro::RampAdjust a;
  if (n % 2) {  // rangs impairs : mode ABSOLU
    a.absolute = 1;
    a.hue = static_cast<int16_t>((n * 53) % 360);
    a.sat = static_cast<int8_t>((n * 17) % 101);
    a.val = static_cast<int8_t>((n * 7) % 41 - 20);
  } else {
    a.absolute = 0;
    a.hue = static_cast<int16_t>((n * 91) % 719 - 359);
    a.sat = static_cast<int8_t>((n * 23) % 201 - 100);
    a.val = static_cast<int8_t>((n * 13) % 201 - 100);
  }
  return a;
}

uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

// Lit un `cas.bin` entier. Petit-boutiste, comme l'écrit la passe Python :
//     "BRMP" | nb_cas:u32 | { nom_len:u16 | nom | palette:1024 | usage:256×u32 }
struct Case {
  std::string name;
  std::vector<uint8_t> palette;
  int usage[256];
};

bool ReadCases(const char* path, std::vector<Case>* out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "illisible : %s\n", path);
    return false;
  }
  char magic[4];
  uint32_t count = 0;
  bool ok = std::fread(magic, 1, 4, f) == 4 &&
            std::memcmp(magic, "BRMP", 4) == 0 &&
            std::fread(&count, 4, 1, f) == 1;
  for (uint32_t i = 0; ok && i < count; ++i) {
    uint16_t len = 0;
    if (std::fread(&len, 2, 1, f) != 1) { ok = false; break; }
    Case c;
    c.name.resize(len);
    c.palette.resize(1024);
    uint32_t raw[256];
    ok = (len == 0 || std::fread(&c.name[0], 1, len, f) == len) &&
         std::fread(c.palette.data(), 1, 1024, f) == 1024 &&
         std::fread(raw, 4, 256, f) == 256;
    if (!ok) break;
    for (int k = 0; k < 256; ++k) c.usage[k] = static_cast<int>(raw[k]);
    out->push_back(std::move(c));
  }
  std::fclose(f);
  if (!ok) std::fprintf(stderr, "cas.bin tronqué ou corrompu\n");
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage : xcheck_ramps <cas.bin>\n");
    return 2;
  }
  std::vector<Case> cases;
  if (!ReadCases(argv[1], &cases)) return 2;

  for (const Case& c : cases) {
    ro::PaletteRamp ramps[ro::kMaxRamps];
    const int n = ro::DetectRamps(c.palette.data(), c.palette.size(), c.usage,
                                  ramps, ro::kMaxRamps);

    ro::PaletteRecipe recipe;
    for (int i = 0; i < n; ++i) recipe.ramps[i] = TestAdjust(i);

    uint8_t painted[1024];
    if (!ro::ApplyRecipe(c.palette.data(), c.palette.size(), ramps, n, recipe,
                         painted, sizeof(painted))) {
      std::fprintf(stderr, "ApplyRecipe a refusé : %s\n", c.name.c_str());
      return 2;
    }

    std::printf("%s|%d|", c.name.c_str(), n);
    for (int i = 0; i < n; ++i) {
      if (i) std::printf(";");
      std::printf("%d,%d,%d,%d", ramps[i].start, ramps[i].length,
                  ramps[i].pixels, ramps[i].hue);
    }
    std::printf("|%016llx\n",
                static_cast<unsigned long long>(Fnv1a(painted, sizeof(painted))));
  }
  return 0;
}
