#include "ui/mob_model.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>  // MAX_PATH

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / ReleaseTexture / DeviceEpoch
#include "ui/model_raster.h"
#include "ui/spr_act.h"      // spract::ReadFile — le VFS du client
#include "utils/log_console.h"

namespace ro {
namespace {

// 🔴 Le client joue ces animations à VITESSE DOUBLE. `CActorSprite_RenderModel`
// (0x00C5BB10) fait avancer son horloge de `SecondsElapsed + SecondsElapsed`
// avant de la passer à `GrannySetModelClock` — ce n'est pas une subtilité de
// rendu, c'est le rythme auquel le joueur voit l'Emperium pulser. À vitesse
// réelle, la fiche « rame » à côté du jeu.
constexpr float kClientSpeed = 2.0f;

// Pas d'échantillonnage visé (~19 à 25 images/s) et plafond d'images. Le pas
// n'a plus rien d'un compromis de coût depuis que les images sont rendues À LA
// DEMANDE : seul le plafond borne la VRAM (96 images de 116×132 ≈ 5,9 Mo, et
// encore, seulement si l'animation tourne assez longtemps pour toutes les
// visiter). Une animation plus longue s'échantillonne plus large plutôt que
// d'être tronquée — couper l'Emperium (9,9 s, soit 5 s à vitesse client)
// escamoterait la moitié de son cycle.
constexpr float kTargetStepMs = 40.0f;
constexpr int   kMaxFrames    = 96;

// Le préfixe VFS des modèles, tel que le client le compose :
// `Cstr_sprintf("model\\3dmob\\%s", resname)` en 0x0071F600, puis
// `Res_MakeDataRootRelativePath` ajoute `data\`. On court-circuite les deux,
// donc on pose les deux — la même règle que pour `data\sprite\` (cf.
// ui/mob_sprite.cc), et la même erreur guette : n'écrire que `data\` rend
// « fichier introuvable » sur TOUS les modèles.
constexpr const char* kModelDir = "data\\model\\3dmob\\";

void ReleaseRetiring(MobModelRes* res) {
  for (void* t : res->retiring) {
    if (t) Overlay_ReleaseTexture(t);
  }
  res->retiring.clear();
}

// Range la série courante dans la file d'attente au lieu de la détruire tout
// de suite : ces textures peuvent être dans la draw-list de la frame EN COURS.
void RetireFrames(MobModelRes* res) {
  for (void* t : res->frames) {
    if (t) res->retiring.push_back(t);
  }
  res->frames.clear();
}

// Prépare la SÉRIE (sa longueur et sa cadence) sans rien rasteriser.
//
// 🔴 Les images sont rendues À LA DEMANDE, une par frame au pire, et gardées.
// Tout pré-rendre coûtait une centaine de rasterisations d'un coup — à
// l'ouverture de la fiche, et surtout à CHAQUE cran de molette, ce qui se
// voyait. En paresseux, le premier tour d'animation remplit le cache au rythme
// d'une image par frame (moins d'une milliseconde chacune) et les tours suivants
// ne coûtent plus rien.
void ResetSeries(MobModelRes* res) {
  RetireFrames(res);

  // Durée telle que le JOUEUR la perçoit : l'animation est jouée à la vitesse du
  // client, pas à celle du fichier.
  const float duration = res->model.animation_seconds / kClientSpeed;
  int frames = 1;
  if (duration > 0.0f) {
    frames = static_cast<int>(std::lround(duration * 1000.0f / kTargetStepMs));
    frames = std::max(1, std::min(frames, kMaxFrames));
  }
  res->ms_per_frame = (duration > 0.0f)
                          ? duration * 1000.0f / static_cast<float>(frames)
                          : 1000.0f;
  res->frames.assign(static_cast<size_t>(frames), nullptr);
  res->device_epoch = Overlay_DeviceEpoch();
}

// Rasterise l'image `index` si elle manque, et rend sa texture.
void* EnsureFrame(MobModelRes* res, int index) {
  if (index < 0 || index >= static_cast<int>(res->frames.size())) return nullptr;
  if (res->frames[static_cast<size_t>(index)]) return res->frames[static_cast<size_t>(index)];

  ModelViewParams vp;
  vp.width  = res->width;
  vp.height = res->height;
  vp.yaw    = res->yaw;

  // Instant DU FICHIER correspondant à cette image : la cadence de lecture est
  // déjà accélérée, l'échantillonnage doit donc reparcourir toute l'animation.
  const float t = res->model.animation_seconds * static_cast<float>(index) /
                  static_cast<float>(res->frames.size());
  PoseModel(&res->model, t);

  std::vector<uint8_t> rgba;
  if (!RenderModelImage(res->model, vp, &rgba)) return nullptr;

  // Le rasteriseur sort du RGBA ; les textures d'overlay veulent de l'ARGB
  // (0xAARRGGBB), la même convention que `spract::Image`.
  std::vector<uint32_t> argb(static_cast<size_t>(vp.width) * vp.height);
  for (size_t p = 0; p < argb.size(); ++p) {
    const uint8_t* s = &rgba[p * 4];
    argb[p] = (static_cast<uint32_t>(s[3]) << 24) |
              (static_cast<uint32_t>(s[0]) << 16) |
              (static_cast<uint32_t>(s[1]) << 8) | s[2];
  }
  void* tex = Overlay_CreateTextureARGB(argb.data(), vp.width, vp.height);
  res->frames[static_cast<size_t>(index)] = tex;
  return tex;
}

}  // namespace

bool LoadMobModel(const char* model_name, int width, int height,
                  MobModelRes* res) {
  if (!res || !model_name || !*model_name || width <= 0 || height <= 0) return false;

  // 🔴 Le device a pu être perdu (alt-tab, changement de résolution, TDR) : les
  // textures d'alors sont mortes et ne doivent SURTOUT pas être relâchées —
  // elles appartiennent à un device qui n'existe plus. On les oublie.
  const unsigned epoch = Overlay_DeviceEpoch();
  if (res->loaded && epoch != res->device_epoch) {
    res->frames.clear();
    res->retiring.clear();
    res->loaded = false;
  }

  if (res->loaded && res->name == model_name && res->width == width &&
      res->height == height) {
    ReleaseRetiring(res);
    return !res->frames.empty();
  }
  if (res->failed && res->name == model_name) return false;

  const bool same_model = res->loaded && res->name == model_name;
  RetireFrames(res);
  res->width  = width;
  res->height = height;

  if (!same_model) {
    FreeModel(&res->model);
    res->name   = model_name;
    res->loaded = false;
    res->failed = false;

    if (!GrannyReady()) {
      LogDiag("[mob3d] granny2.dll injoignable — modele {} non rendu", model_name);
      res->failed = true;
      return false;
    }
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s%s", kModelDir, model_name);
    std::vector<uint8_t> bytes;
    if (!spract::ReadFile(path, &bytes) || bytes.empty()) {
      LogDiag("[mob3d] introuvable dans le VFS : {}", path);
      res->failed = true;
      return false;
    }
    if (!LoadModel(std::move(bytes), &res->model)) {
      LogDiag("[mob3d] LoadModel a refuse {}", path);
      res->failed = true;
      return false;
    }
    LogDiag("[mob3d] {} : {} mesh(es), {} os, animation {:.2f} s", model_name,
            static_cast<int>(res->model.meshes.size()), res->model.bone_count,
            res->model.animation_seconds);
  }

  ResetSeries(res);
  res->loaded = !res->frames.empty();
  res->failed = !res->loaded;
  return res->loaded;
}

void SetMobModelYaw(MobModelRes* res, float yaw) {
  if (!res || !res->loaded) return;
  if (std::fabs(yaw - res->yaw) < 1e-4f) return;
  res->yaw = yaw;
  // La série garde sa longueur et sa cadence ; seules les images changent, et
  // elles se refont une par une au fil de la lecture.
  ResetSeries(res);
}

bool DrawMobModel(ImDrawList* draw_list, MobModelRes* res, ImVec2 rect_min,
                  ImVec2 rect_max, float anim_seconds, float alpha) {
  if (!draw_list || !res || res->frames.empty()) return false;
  // Les textures retirées au tour précédent ont eu leur frame : c'est le
  // moment, et le seul, où les relâcher est sûr.
  ReleaseRetiring(res);

  const int count = static_cast<int>(res->frames.size());
  int index = 0;
  if (count > 1 && res->ms_per_frame > 0.0f) {
    const float ms = anim_seconds * 1000.0f;
    index = static_cast<int>(ms / res->ms_per_frame) % count;
    if (index < 0) index += count;
  }
  void* tex = EnsureFrame(res, index);
  if (!tex) return false;

  // Cadrage : l'image a déjà les proportions demandées au rendu, mais le
  // rectangle d'affichage peut différer de celui du pré-rendu (une fiche
  // redimensionnée). On garde le rapport plutôt que d'étirer le monstre.
  const float rw = rect_max.x - rect_min.x, rh = rect_max.y - rect_min.y;
  if (rw <= 0.0f || rh <= 0.0f) return false;
  const float sx = rw / static_cast<float>(res->width);
  const float sy = rh / static_cast<float>(res->height);
  const float s  = std::min(sx, sy);
  const float w  = static_cast<float>(res->width) * s;
  const float h  = static_cast<float>(res->height) * s;
  const ImVec2 p0(rect_min.x + (rw - w) * 0.5f, rect_min.y + (rh - h) * 0.5f);

  const ImU32 tint = IM_COL32(255, 255, 255,
                              static_cast<int>(255.0f * std::max(0.0f, std::min(1.0f, alpha))));
  draw_list->AddImage(reinterpret_cast<ImTextureID>(tex), p0,
                      ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
  return true;
}

void FreeMobModel(MobModelRes* res) {
  if (!res) return;
  RetireFrames(res);
  ReleaseRetiring(res);
  FreeModel(&res->model);
  res->loaded = false;
  res->failed = false;
  res->name.clear();
}

}  // namespace ro
