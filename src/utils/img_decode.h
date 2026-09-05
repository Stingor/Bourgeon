#pragma once

// ── imgdec : décoder une image (fixe ou ANIMÉE) en pixels BGRA ───────────────
//
// WIC — le décodeur d'images de Windows — sait lire PNG/JPEG/GIF/BMP/WebP sans
// ajouter la moindre dépendance au projet. Ce module l'enveloppe, et il ne fait
// QUE des pixels : aucune texture n'est créée ici, aucun appel D3D. C'est ce qui
// le rend appelable depuis un thread de travail, où le device n'a rien à faire.
//
// 🔴 IL A DEUX APPELANTS AUX BESOINS OPPOSÉS, ET C'EST POUR ÇA QU'IL EXISTE.
// L'aperçu d'image du chat (`imgprev`) décode une donnée HOSTILE, venue d'un
// serveur tiers : ses bornes sont serrées et l'échec y est la normale. Le
// tutoriel (`TutorialWindow`) décode NOS PROPRES gifs, livrés avec le patch :
// ses bornes sont plus larges et un échec y est un bug d'auteur, à dire tout
// haut. Le code de décodage, lui, est le même — il était écrit une fois dans
// image_preview.cc, où le tutoriel ne pouvait pas l'atteindre (namespace
// anonyme d'un .cc). D'où l'extraction, et d'où `Limits` en paramètre plutôt
// qu'en constantes de fichier.
//
// ⚠ WIC EST DU COM. Chaque thread qui appelle ces fonctions doit avoir fait son
// propre `CoInitializeEx` — ce n'est PAS fait ici, parce qu'un module qui
// initialise COM sur le thread de quelqu'un d'autre décide à sa place de son
// modèle d'appartenance.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace imgdec {

// ── Les bornes, et le dégât que chacune évite ────────────────────────────────
struct Limits {
  // Garde-fou d'EN-TÊTE : une image dont l'en-tête annonce des dimensions
  // délirantes (« zip bomb ») est refusée AVANT toute allocation.
  int max_source_dim = 16384;
  // Taille de SORTIE. On réduit au décodage plutôt que de garder la pleine
  // résolution : c'est tout ce qui sera montré, et la VRAM d'un client RO n'est
  // pas extensible. Jamais d'agrandissement — une petite image reste petite.
  int max_dim = 512;
  // Animation seulement : nombre d'images gardées, et poids total des pixels
  // gardés (donc APRÈS réduction). Dépassement = l'animation est refusée, à
  // charge de l'appelant de retomber sur l'image fixe.
  int    max_frames = 60;
  size_t max_bytes  = 12u * 1024u * 1024u;
};

// Une animation décodée : des canevas COMPLETS, prêts à téléverser, tous de la
// même taille `w`x`h`, et le temps d'affichage de chacun.
struct Animation {
  std::vector<std::vector<uint8_t>> frames;
  std::vector<int>                  delays_ms;  // même taille que `frames`
  int w = 0;
  int h = 0;
};

// La première image du fichier, réduite à `limits.max_dim`. Convient à tout
// format que WIC connaît, y compris un GIF animé — dont elle rend alors la
// première image, figée.
bool DecodeStill(const uint8_t* data, size_t size, const Limits& limits,
                 std::vector<uint8_t>* out_bgra, int* out_w, int* out_h);

// Toutes les images d'un fichier animé, composées.
//
// 🔴 UN GIF NE STOCKE PAS N IMAGES COMPLÈTES. Le plus souvent chaque image n'est
// qu'un RECTANGLE de différences, posé à une position donnée, avec une consigne
// d'effacement pour la suivante. Décoder « l'image n » et l'afficher telle
// quelle donnerait des fragments sur fond transparent — il faut les COMPOSER sur
// un canevas persistant, dans l'ordre, en respectant ces consignes. C'est ce que
// fait cette fonction, et c'est toute sa raison d'être.
//
// false = pas animable : une seule image, en-tête hors bornes, ou budget
// dépassé. L'appelant retombe alors sur `DecodeStill`.
bool DecodeAnimation(const uint8_t* data, size_t size, const Limits& limits,
                     Animation* out);

// ── Depuis un FICHIER du disque ──────────────────────────────────────────────
// Lit `path` puis tente l'animation, et à défaut l'image fixe — une `Animation`
// d'une seule image, de délai nul. L'appelant n'a donc qu'un chemin de code.
//
// `out_error` (facultatif) reçoit une phrase FRANÇAISE disant ce qui a échoué :
// fichier introuvable, trop gros, format refusé. Elle est destinée à être
// MONTRÉE — pour des gifs qu'on livre soi-même, un échec muet se paie en heures
// de recherche.
bool DecodeFile(const std::string& path, const Limits& limits, Animation* out,
                std::string* out_error = nullptr);

// Plafond de lecture d'un fichier, en octets. Au-delà, `DecodeFile` renonce sans
// allouer : un fichier de cette taille n'est pas un gif d'interface.
constexpr size_t kMaxFileBytes = 32u * 1024u * 1024u;

}  // namespace imgdec
