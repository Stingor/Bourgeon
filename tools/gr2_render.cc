// Rendu d'un modèle Granny (.gr2) HORS du client — la preuve avant l'intégration.
//
// ── Pourquoi ce programme existe ─────────────────────────────────────────────
// `src/ui/gr2_model.cc` et `src/ui/model_raster.cc` doivent marcher avant d'être
// branchés dans le jeu : une erreur de layout ou de convention d'appel sur
// `granny2.dll` ne produit pas un rendu bancal mais une violation d'accès, et
// la déboguer DANS le client (avec son overlay, ses hooks et son anti-triche)
// coûte dix fois plus cher qu'ici. Même démarche que pour `.spr`/`.act`, où le
// parseur a été validé sur 178 fichiers réels avant la première compilation.
//
// Il tourne sur les MÊMES sources que le jeu — pas une transcription : ce qui
// est prouvé ici est exactement ce qui s'exécutera.
//
// ── Comment s'en servir ──────────────────────────────────────────────────────
//     "C:\...\vcvarsall.bat" x86
//     cl /std:c++17 /EHsc /O2 /I src tools\gr2_render.cc src\ui\gr2_model.cc ^
//        src\ui\model_raster.cc
//     gr2_render.exe "E:\...\data\model\3dmob\empelium90_0.gr2" sortie\ ^
//        --dll "E:\...\granny2.dll" [--images 8]
// Écrit `sortie\<nom>_<i>.bmp`, une image par instant de l'animation.
//
// 🔴 x86 obligatoire : `granny2.dll` est 32 bits, comme le client.

#define _CRT_SECURE_NO_WARNINGS  // `fopen` sur un chemin passé en argument
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ui/gr2_model.h"
#include "ui/model_raster.h"

namespace {

std::vector<uint8_t> ReadFileBytes(const char* path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path, "rb");
  if (!f) return data;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    data.resize(static_cast<size_t>(n));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
  }
  std::fclose(f);
  return data;
}

// BMP 32 bits à masques : le contrôle final d'un rendu est l'œil, et un BMP
// s'ouvre partout sans dépendance.
bool WriteBmp32(const char* path, int w, int h, const uint8_t* rgba) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const uint32_t pixels = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4u;
  const uint32_t offset = 14 + 108;
  const uint32_t size   = offset + pixels;
  uint8_t hdr[14 + 108] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  std::memcpy(hdr + 2, &size, 4);
  std::memcpy(hdr + 10, &offset, 4);
  const uint32_t dib = 108;
  const int32_t  iw = w, ih = -h;   // hauteur négative : première ligne en haut
  const uint16_t planes = 1, bpp = 32;
  const uint32_t compression = 3;   // BI_BITFIELDS
  const uint32_t mr = 0x00FF0000, mg = 0x0000FF00, mb = 0x000000FF, ma = 0xFF000000;
  std::memcpy(hdr + 14 +  0, &dib, 4);
  std::memcpy(hdr + 14 +  4, &iw, 4);
  std::memcpy(hdr + 14 +  8, &ih, 4);
  std::memcpy(hdr + 14 + 12, &planes, 2);
  std::memcpy(hdr + 14 + 14, &bpp, 2);
  std::memcpy(hdr + 14 + 16, &compression, 4);
  std::memcpy(hdr + 14 + 20, &pixels, 4);
  std::memcpy(hdr + 14 + 40, &mr, 4);
  std::memcpy(hdr + 14 + 44, &mg, 4);
  std::memcpy(hdr + 14 + 48, &mb, 4);
  std::memcpy(hdr + 14 + 52, &ma, 4);
  std::fwrite(hdr, 1, sizeof(hdr), f);
  std::vector<uint8_t> row(static_cast<size_t>(w) * 4);
  for (int y = 0; y < h; ++y) {
    const uint8_t* src = rgba + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      row[x * 4 + 0] = src[x * 4 + 2];
      row[x * 4 + 1] = src[x * 4 + 1];
      row[x * 4 + 2] = src[x * 4 + 0];
      row[x * 4 + 3] = src[x * 4 + 3];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

std::string BaseName(const char* path) {
  std::string s(path);
  const size_t slash = s.find_last_of("\\/");
  if (slash != std::string::npos) s = s.substr(slash + 1);
  const size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  // Sortie non bufferisée : si `granny2.dll` lâche, le tampon partirait avec
  // elle et on perdrait la trace de l'étape fautive.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  if (argc < 3) {
    std::printf("usage: gr2_render <fichier.gr2> <dossier_sortie> "
                "[--dll <granny2.dll>] [--images N] [--taille LxH]\n");
    return 2;
  }
  const char* gr2_path = argv[1];
  const std::string out_dir = argv[2];
  const char* dll_path = nullptr;
  int frames = 6;
  int width = 128, height = 160;
  // Vue par défaut : celle de `ModelViewParams`. Les options `--yaw` / `--pitch`
  // servent à la CALIBRER contre une capture du jeu, ce qui est la seule façon
  // honnête de régler un angle « comme en jeu ».
  float yaw = ro::ModelViewParams{}.yaw;
  float pitch = ro::ModelViewParams{}.pitch;
  for (int i = 3; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--dll") == 0) dll_path = argv[i + 1];
    else if (std::strcmp(argv[i], "--images") == 0) frames = std::atoi(argv[i + 1]);
    else if (std::strcmp(argv[i], "--taille") == 0)
      std::sscanf(argv[i + 1], "%dx%d", &width, &height);
    else if (std::strcmp(argv[i], "--yaw") == 0) yaw = static_cast<float>(std::atof(argv[i + 1]));
    else if (std::strcmp(argv[i], "--pitch") == 0) pitch = static_cast<float>(std::atof(argv[i + 1]));
  }
  if (frames < 1) frames = 1;

  if (!ro::GrannyReady(dll_path)) {
    std::printf("!! granny2.dll introuvable ou incomplète (%s)\n",
                dll_path ? dll_path : "granny2.dll");
    return 1;
  }
  std::printf("granny2.dll %s\n", ro::GrannyVersion());

  std::vector<uint8_t> bytes = ReadFileBytes(gr2_path);
  if (bytes.empty()) {
    std::printf("!! fichier illisible : %s\n", gr2_path);
    return 1;
  }

  ro::Model model;
  if (!ro::LoadModel(std::move(bytes), &model)) {
    std::printf("!! LoadModel a échoué\n");
    return 1;
  }
  std::printf("%s : %zu mesh(es), %zu texture(s), %d os, animation %.3f s\n",
              gr2_path, model.meshes.size(), model.textures.size(),
              model.bone_count, model.animation_seconds);
  std::printf("  boîte de repos x[%.2f %.2f] y[%.2f %.2f] z[%.2f %.2f]\n",
              model.bb_min[0], model.bb_max[0], model.bb_min[1], model.bb_max[1],
              model.bb_min[2], model.bb_max[2]);
  for (const ro::ModelMesh& mm : model.meshes) {
    std::printf("  mesh %-20s %5d sommets %5zu triangles %s\n", mm.name.c_str(),
                mm.vertex_count, mm.indices.size() / 3,
                mm.deformer ? "(déformé)" : "(rigide)");
  }

  const std::string base = BaseName(gr2_path);
  ro::ModelViewParams vp;
  vp.width = width;
  vp.height = height;
  vp.yaw = yaw;
  vp.pitch = pitch;

  for (int i = 0; i < frames; ++i) {
    // Réparties sur toute la boucle d'animation : c'est ce qui montre qu'elle
    // est réellement échantillonnée et pas figée sur la pose de repos.
    const float t = (model.animation_seconds > 0.0f)
                        ? model.animation_seconds * static_cast<float>(i) /
                              static_cast<float>(frames)
                        : 0.0f;
    ro::PoseModel(&model, t);

    std::vector<uint8_t> rgba;
    if (!ro::RenderModelImage(model, vp, &rgba)) {
      std::printf("!! rendu impossible à t=%.3f\n", t);
      continue;
    }
    // Combien de pixels ont été peints ? Une image vide passe inaperçue à la
    // lecture d'un journal, mais pas avec ce compte.
    size_t painted = 0;
    for (size_t p = 3; p < rgba.size(); p += 4)
      if (rgba[p] != 0) ++painted;

    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\%s_%02d.bmp", out_dir.c_str(),
                  base.c_str(), i);
    const bool ok = WriteBmp32(path, vp.width, vp.height, rgba.data());
    std::printf("  t=%6.3f s  %6zu pixels peints (%.1f %%)  -> %s%s\n", t, painted,
                100.0 * static_cast<double>(painted) /
                    static_cast<double>(vp.width * vp.height),
                path, ok ? "" : "  (ÉCHEC D'ÉCRITURE)");
  }

  ro::FreeModel(&model);
  std::printf("fini.\n");
  return 0;
}
