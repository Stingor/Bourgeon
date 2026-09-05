#pragma once

// ── ro::GifAnim — un gif animé, joué dans une fenêtre ImGui ──────────────────
//
// Décode des octets d'image et rend, à chaque frame, la texture de l'instant.
// Écrit pour le tutoriel (des captures de l'interface Bourgeon), mais il ne sait
// rien de lui : `ui/` ne connaît pas `features/`.
//
// 🔴 IL NE LIT PAS LE FICHIER LUI-MÊME, ET C'EST LA RAISON DE SON API EN OCTETS.
// Le contenu qu'il affiche vit chez le joueur DANS UN GRF, que seul le VFS du
// client sait ouvrir (`ro::spract::ReadFile`) — et ce VFS est du code natif du
// client, qu'on n'appelle pas depuis un thread de travail. L'appelant lit donc
// dans SA frame, et ne confie ici que des octets, que le thread peut mâcher
// tranquillement.
//
// Le décodage est dans `utils/img_decode` ; ce qui est ICI, c'est tout ce qu'un
// décodeur ne peut pas savoir — les trois contraintes du rendu :
//
// 🔴 1. LE DÉCODAGE NE BLOQUE PAS LE JEU. Composer 40 images d'un gif prend des
//    dizaines de millisecondes : fait dans la frame, ça se voit. Un thread
//    détaché s'en charge et dépose ses pixels ; l'appelant voit `Loading()` puis
//    `Ready()`. Le thread tient un `shared_ptr` sur l'état partagé, donc
//    détruire le GifAnim pendant qu'il travaille est sûr — le travail finit dans
//    le vide, sans rien écrire chez un mort.
//
// 🔴 2. LES TEXTURES SUIVENT L'EPOCH DU DEVICE. Elles vivent en D3DPOOL_DEFAULT
//    et appartiennent à UN device : après un alt-tab, un changement de résolution
//    ou un TDR, les handles sont morts et les dessiner plante dans le pilote. On
//    compare donc `Overlay_DeviceEpoch()` à chaque `Pump()`, et on relève les
//    textures depuis les PIXELS, qu'on garde en mémoire pour cette raison
//    précise (cf. feedback_imgui_texture_cache_follows_device_epoch).
//
// 🔴 3. AUCUNE TEXTURE N'EST RELÂCHÉE PENDANT UNE FRAME. `AddImage` ne fait que
//    noter un identifiant, que le rendu lira EN FIN de frame : libérer entre les
//    deux corrompt le tas (0xC0000374). `Unload()` met donc les textures dans une
//    file, et `Pump()` ne les libère qu'une frame plus tard — ou les LÂCHE sans
//    les libérer si l'epoch a changé, car elles sont déjà mortes
//    (cf. feedback_imgui_no_texture_release_mid_frame).
//
// ── L'ordre d'appel ──────────────────────────────────────────────────────────
//     anim.Load(chemin, octets, limites);  // une fois
//     anim.Pump();                      // AU DÉBUT de chaque frame
//     if (anim.Ready()) ImGui::Image(anim.Frame(), ...);
//
// `Pump()` fait le téléversement et les libérations : l'appeler avant de dessiner
// est ce qui garantit que rien n'est libéré alors qu'une draw-list le référence.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "utils/img_decode.h"

namespace ro {

class GifAnim {
 public:
  GifAnim() = default;
  ~GifAnim();

  GifAnim(const GifAnim&) = delete;
  GifAnim& operator=(const GifAnim&) = delete;

  // Demande le décodage de `bytes`, désigné par `key` — en pratique le chemin
  // VFS d'où l'appelant les a lus. Sans effet si `key` est déjà chargé, en cours
  // de chargement ou déjà refusé : appeler à chaque frame ne relance rien.
  //
  // ⚠ La garde ne dispense PAS l'appelant de la lecture, qu'il a déjà payée
  // avant d'arriver ici. Interroger `Holds()` d'abord est ce qui l'en dispense —
  // et c'est ce qu'il faut faire si l'appel est dans une frame.
  //
  // `bytes` vide n'est pas silencieux : c'est un échec nommé dans `Error()`,
  // parce que « rien ne s'affiche » est précisément ce qu'on ne veut pas.
  void Load(const std::string& key, std::vector<uint8_t> bytes,
            const imgdec::Limits& limits);

  // true si `key` est ce que ce GifAnim porte déjà (chargé, en cours, ou refusé).
  bool Holds(const std::string& key) const;

  // Rend la mémoire et la VRAM. Les textures partent en libération différée —
  // appeler depuis une frame ImGui est donc sûr.
  void Unload();

  // Téléversement, libérations différées, reprise après perte du device. À
  // appeler une fois par frame, AVANT de dessiner.
  void Pump();

  bool Loading() const;
  bool Ready() const { return !textures_.empty() && frames_ready_; }
  // Vide tant que rien n'a échoué. Phrase FRANÇAISE, destinée à être montrée :
  // ces gifs-là sont les nôtres, un échec est un bug d'auteur à corriger.
  const std::string& Error() const { return error_; }

  int Width() const { return width_; }
  int Height() const { return height_; }
  int FrameCount() const { return static_cast<int>(textures_.size()); }

  // La texture de l'instant, ou nullptr. L'horloge tourne toute seule et
  // l'animation boucle.
  void* Frame() const;

  // Repart de la première image. À appeler quand l'animation (re)devient
  // visible : sans ça, une page revue reprend au milieu de son geste.
  void Restart();

  // Fige l'animation sur l'image courante.
  void SetPaused(bool paused) { paused_ = paused; }
  bool Paused() const { return paused_; }

 private:
  // L'état que le thread de décodage remplit. Partagé : le thread en tient une
  // référence, donc il survit à la destruction du GifAnim.
  struct Job;

  void ReleaseTextures();        // -> file différée
  void FlushPendingReleases();   // libère ce qui a passé une frame

  std::string        key_;
  imgdec::Limits     limits_;
  std::shared_ptr<Job> job_;

  // Les pixels décodés, GARDÉS : ils resservent à refaire les textures après une
  // perte de device. C'est le prix, en RAM, de ne pas re-décoder à chaque
  // alt-tab — et la raison des plafonds passés dans `limits`.
  imgdec::Animation  pixels_;
  std::vector<void*> textures_;
  size_t             uploaded_    = 0;      // images déjà téléversées
  bool               frames_ready_ = false; // toutes téléversées
  unsigned           epoch_       = 0;      // device auquel appartiennent textures_
  int                width_       = 0;
  int                height_      = 0;
  std::string        error_;

  // Horloge de lecture. `origin_ms` est l'instant de la première image.
  uint32_t           origin_ms_ = 0;
  int                total_ms_  = 0;
  bool               paused_    = false;
  mutable int        held_frame_ = 0;  // image montrée quand on est en pause

  // Textures en attente de libération : la frame où elles ont été rendues, et
  // l'epoch auquel elles appartenaient.
  struct Pending {
    void*    tex;
    int      frame;
    unsigned epoch;
  };
  std::vector<Pending> pending_;
};

}  // namespace ro
