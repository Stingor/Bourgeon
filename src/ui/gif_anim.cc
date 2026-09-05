#include "ui/gif_anim.h"

#include <Windows.h>

#include <mutex>
#include <thread>
#include <utility>

#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / _ReleaseTexture / _DeviceEpoch
#include "imgui.h"

namespace ro {
namespace {

// Images téléversées par frame. Créer une texture touche le device : tout monter
// d'un coup ferait un hoquet visible sur un gif de quarante images, alors que
// l'étaler sur dix frames ne se voit pas — la première image s'affiche dès qu'elle
// est prête, et l'animation démarre quand tout est monté.
constexpr size_t kUploadPerFrame = 4;

}  // namespace

// L'état que le thread de décodage remplit. Il est partagé par `shared_ptr` :
// le thread en garde une référence, donc détruire le GifAnim pendant qu'il
// travaille ne lui arrache rien sous les pieds.
struct GifAnim::Job {
  std::mutex        mutex;
  bool              done = false;
  bool              ok   = false;
  imgdec::Animation anim;
  std::string       error;
};

GifAnim::~GifAnim() {
  // Destruction : on libère POUR DE BON, sans passer par la file différée — plus
  // personne ne la videra. Un GifAnim ne se détruit donc pas depuis une frame
  // ImGui ; les propriétaires le gardent en membre et appellent `Unload()`.
  for (void* t : textures_)
    if (t) Overlay_ReleaseTexture(t);
  for (const Pending& p : pending_)
    if (p.tex) Overlay_ReleaseTexture(p.tex);
}

void GifAnim::Load(const std::string& path, const imgdec::Limits& limits) {
  if (path.empty()) return;
  // Déjà chargé, en cours, ou déjà refusé : appeler à chaque frame ne relance rien.
  // 🔴 `pixels_` compte autant que `textures_` : après une perte de device les
  // textures sont lâchées mais les pixels restent, et c'est justement d'eux que
  // `Pump` va les remonter — redécoder le fichier serait du travail pour rien.
  if (path == path_ &&
      (job_ || !textures_.empty() || !pixels_.frames.empty() || !error_.empty()))
    return;

  Unload();
  path_   = path;
  limits_ = limits;
  error_.clear();

  auto job = std::make_shared<Job>();
  job_ = job;

  // ⚠ La copie du chemin et des limites part DANS le thread : le GifAnim peut
  // mourir avant lui.
  std::thread([job, path, limits]() {
    // WIC est du COM : chaque thread doit l'initialiser pour lui-même.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    imgdec::Animation anim;
    std::string       error;
    const bool ok = imgdec::DecodeFile(path, limits, &anim, &error);

    if (SUCCEEDED(co)) CoUninitialize();

    std::lock_guard<std::mutex> lock(job->mutex);
    job->anim  = std::move(anim);
    job->error = std::move(error);
    job->ok    = ok;
    job->done  = true;
  }).detach();
}

void GifAnim::Unload() {
  ReleaseTextures();
  pixels_.frames.clear();
  pixels_.delays_ms.clear();
  pixels_.w = pixels_.h = 0;
  job_.reset();
  path_.clear();
  error_.clear();
  width_ = height_ = 0;
  total_ms_ = 0;
  uploaded_ = 0;
  frames_ready_ = false;
  held_frame_ = 0;
}

bool GifAnim::Loading() const {
  if (job_) return true;
  return !pixels_.frames.empty() && !frames_ready_;
}

void GifAnim::ReleaseTextures() {
  const int      frame = ImGui::GetFrameCount();
  const unsigned e     = Overlay_DeviceEpoch();
  for (void* t : textures_)
    if (t) pending_.push_back({t, frame, e});
  textures_.clear();
  uploaded_ = 0;
  frames_ready_ = false;
}

void GifAnim::FlushPendingReleases() {
  if (pending_.empty()) return;
  const int      frame = ImGui::GetFrameCount();
  const unsigned e     = Overlay_DeviceEpoch();

  size_t keep = 0;
  for (size_t i = 0; i < pending_.size(); ++i) {
    const Pending& p = pending_[i];
    if (p.epoch != e) continue;        // device disparu : la libérer PLANTERAIT
    if (frame <= p.frame + 1) {        // la draw-list de sa frame peut la citer
      pending_[keep++] = p;
      continue;
    }
    Overlay_ReleaseTexture(p.tex);
  }
  pending_.resize(keep);
}

void GifAnim::Pump() {
  FlushPendingReleases();

  // Le device a changé : les handles sont morts. On les LÂCHE (les libérer
  // planterait) et on remonte les textures depuis les pixels, qu'on a gardés
  // exactement pour ce cas.
  const unsigned e = Overlay_DeviceEpoch();
  if (e != epoch_) {
    textures_.clear();
    uploaded_     = 0;
    frames_ready_ = false;
    epoch_        = e;
  }

  // Le thread a-t-il fini ?
  if (job_) {
    bool done = false;
    {
      std::lock_guard<std::mutex> lock(job_->mutex);
      done = job_->done;
      if (done) {
        if (job_->ok) pixels_ = std::move(job_->anim);
        // ⚠ L'erreur se lit AUSSI quand le décodage a réussi : le décodeur s'en
        // sert pour dire « j'ai dû retomber sur une image fixe », ce qui est un
        // succès avec une réserve, pas un échec.
        error_ = job_->error;
        if (!job_->ok && error_.empty()) error_ = "image illisible";
      }
    }
    if (done) {
      job_.reset();
      if (!pixels_.frames.empty()) {
        width_    = pixels_.w;
        height_   = pixels_.h;
        total_ms_ = 0;
        for (int ms : pixels_.delays_ms) total_ms_ += ms;
        Restart();
      }
    }
  }

  // Téléversement progressif.
  if (uploaded_ < pixels_.frames.size()) {
    size_t budget = kUploadPerFrame;
    while (budget-- > 0 && uploaded_ < pixels_.frames.size()) {
      void* t = Overlay_CreateTextureARGB(pixels_.frames[uploaded_].data(),
                                          pixels_.w, pixels_.h);
      if (t == nullptr) {
        // VRAM épuisée : on garde ce qui a marché et on joue avec ça plutôt que
        // de tout perdre. Une animation tronquée reste lisible.
        error_ = "mémoire vidéo insuffisante pour cette image";
        pixels_.frames.resize(uploaded_);
        pixels_.delays_ms.resize(uploaded_);
        total_ms_ = 0;
        for (int ms : pixels_.delays_ms) total_ms_ += ms;
        break;
      }
      textures_.push_back(t);
      ++uploaded_;
    }
    if (uploaded_ >= pixels_.frames.size() && !textures_.empty()) {
      frames_ready_ = true;
      Restart();
    }
  } else if (!textures_.empty()) {
    frames_ready_ = true;
  }
}

void GifAnim::Restart() {
  origin_ms_  = GetTickCount();
  held_frame_ = 0;
}

void* GifAnim::Frame() const {
  if (textures_.empty()) return nullptr;
  // Tant que tout n'est pas monté, on montre la première image : démarrer une
  // animation à moitié téléversée la ferait sauter.
  if (!frames_ready_ || textures_.size() == 1 || total_ms_ <= 0)
    return textures_.front();
  if (paused_) {
    const int idx = (held_frame_ >= 0 && held_frame_ < FrameCount()) ? held_frame_ : 0;
    return textures_[static_cast<size_t>(idx)];
  }

  // GetTickCount déborde toutes les 49,7 jours ; la soustraction non signée reste
  // juste au passage, c'est la raison de travailler en uint32_t.
  const uint32_t elapsed = GetTickCount() - origin_ms_;
  int t = static_cast<int>(elapsed % static_cast<uint32_t>(total_ms_));
  for (size_t i = 0; i < textures_.size() && i < pixels_.delays_ms.size(); ++i) {
    t -= pixels_.delays_ms[i];
    if (t < 0) {
      held_frame_ = static_cast<int>(i);
      return textures_[i];
    }
  }
  held_frame_ = 0;
  return textures_.front();
}

}  // namespace ro
