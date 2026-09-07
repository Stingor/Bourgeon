#pragma once

// ── Vignettes des illustrations de cartes ────────────────────────────────────
//
// L'album de cartes montre neuf cents pochettes, chacune avec l'illustration
// `cardBmp` de sa carte. Ces illustrations font 300×400 : 480 Ko de texture
// chacune, soit plus de 400 Mo pour le catalogue entier. Le cache de
// `GetCardIllust` (item_desc_window.cc) est fait pour UNE illustration à la
// fois, au survol, et n'évince jamais : le parcourir avec l'album ferait
// grimper la VRAM d'un client 32 bits jusqu'à la panne.
//
// D'où ce module, qui ne partage rien avec lui :
//   · les octets sont lus par le VFS du client (`ro::spract::ReadFile`), et NON
//     par le gestionnaire de ressources natif, qui MÉMORISE chaque image qu'on
//     lui demande — la lecture VFS, elle, ne retient rien ;
//   · le .bmp est décodé ici (1 289 des 1 462 illustrations sont en 8 bits
//     palettisés, le reste en 24 bits) et RÉDUIT à la taille EXACTE de la
//     pochette avant d'être téléversé : une vignette pèse 77 Ko, pas 480, et
//     se dessine 1:1 en POINT — nette, là où un rééchantillonnage au dessin
//     la rendait floue ;
//   · le cache est BORNÉ, à éviction LRU, et ses libérations sont différées
//     hors frame (cf. feedback_imgui_no_texture_release_mid_frame) ;
//   · le chargement est ÉTALÉ : quelques vignettes par frame, les autres
//     restent « en attente » une frame ou deux. Tourner une page ne fige donc
//     jamais le rendu, la page se remplit sous les yeux.
//
// La version « silhouette » d'une pochette scellée est une SECONDE texture,
// désaturée et assombrie à la génération : ImGui ne sait que teinter, et une
// teinte grise sur une image en couleur reste en couleur.

#include <cstdint>

namespace ro {
namespace cardthumb {

struct Thumb {
  void* tex = nullptr;  // ImTextureID ; nul = rien à dessiner (voir `pending`)
  int   w   = 0;
  int   h   = 0;
  // Vrai tant que la vignette n'est pas encore chargée (budget de la frame
  // épuisé, ou device pas prêt) : l'appelant dessine un repli et REDEMANDE à
  // la frame suivante. Faux avec `tex` nul = l'illustration n'existe pas, et
  // c'est mémorisé — inutile de redemander.
  bool  pending = false;
};

// À appeler UNE fois par frame, avant tout Get : rouvre le budget de
// chargement et jette le cache entier si le device a été recréé (les poignées
// appartiennent alors à un device disparu, elles se LÂCHENT sans Release).
void BeginFrame();

// La vignette de la carte `card_id`. `illust_path` = chemin CP949 de son
// illustration RELATIF au dossier texture (« 유저인터페이스\cardBmp\x.bmp »,
// tel que `itemdesc::CardIllustPath` le rend) ; vide = pas d'illustration.
// `grey` = silhouette (pochette scellée). `max_w`/`max_h` = la boîte, en
// TEXELS, dans laquelle l'illustration est réduite en gardant son ratio : c'est
// la taille de la pochette à l'écran, pour que la vignette se dessine 1:1 en
// POINT — le pixel-perfect ne se rattrape pas au dessin. Le chemin n'est lu
// qu'au premier appel pour une clé donnée — la clé est (id, grey, boîte), pas
// le chemin ; un changement d'échelle d'interface change la clé, et les
// anciennes vignettes s'évincent d'elles-mêmes.
Thumb Get(uint32_t card_id, const char* illust_path, bool grey, int max_w, int max_h);

// Hors frame (OnTick) : libère les textures évincées depuis le dernier appel.
// Jamais pendant une frame — AddImage ne fait que NOTER une texture, le device
// ne la lit qu'au rendu.
void FlushReleases();

}  // namespace cardthumb
}  // namespace ro
