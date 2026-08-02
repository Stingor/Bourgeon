#pragma once

// ── Pantin d'un personnage, composé chez nous ────────────────────────────────
//
// Un personnage RO n'est pas UN sprite : c'est un ASSEMBLAGE de .spr/.act
// indépendants — corps, tête, coiffes — recalés les uns sur les autres par les
// points d'ancrage de leurs images.
//
// Ce module compose cet assemblage à partir d'une simple APPARENCE (job, sexe,
// coiffure, couleurs…), sans acteur en scène, sans session en jeu et sans
// fenêtre native. Il marche donc au login comme au char-select.
//
// ── Ce qu'il a remplacé ──────────────────────────────────────────────────────
// Un moteur de CAPTURE : le client rendait l'acteur hors écran et on interceptait
// les quads qu'il dessinait (hook sur Actor_SubmitSpriteQuad). C'était juste et
// complet, mais ça exigeait un acteur vivant, un hook global, un cache de pages
// d'atlas et une dizaine d'offsets de structure devinés. Ici, rien de tout ça :
// on lit les fichiers. Le char-select et l'aperçu marchand y sont passés, et la
// capture a été supprimée.
//
// ── Ce qu'il ne fait pas ─────────────────────────────────────────────────────
// Ni arme ni bouclier — comme le char-select natif, qui ne les montre pas non
// plus. Les compagnons (cart, faucon) et les effets .str n'en sont pas non plus.
// La CAPE, elle, est gérée (cf. `DollLook::garment`).

#include <cstdint>

#include "imgui.h"

namespace ro {

// Apparence d'un personnage. Mêmes champs que `BasicInfo::DollLook`, dont les
// offsets viennent de CHARACTER_INFO (char-select natif) — on garde une
// structure distincte pour que ce module ne dépende d'aucun plugin.
struct DollLook {
  int sex = 0;            // 0 = femme, 1 = homme (DÉJÀ résolu, jamais 99)
  int job = 0;            // id de classe
  // Style de corps (CHARACTER_INFO +0x58). 🔴 C'est LUI qui choisit le sprite
  // de corps, pas `job` : le natif résout la paire (job, body) en une classe de
  // corps, où `job` ne sert plus qu'à décider bébé / variante alternative.
  int body = 0;
  int hair = 0;           // id de coiffure BRUT (le remap est fait ici)
  int hair_color = -1;    // -1 = palette d'origine du sprite
  int clothes_color = -1; // -1 = palette d'origine du sprite
  int head_low = 0;       // accessoire du bas   (0 = aucun)
  int head_top = 0;       // accessoire du haut
  int head_mid = 0;       // accessoire du milieu
  // Cape (garment), par son id de VUE. 0 = aucune.
  //
  // ⚠ Elle passe DEVANT ou DERRIÈRE le personnage selon l'orientation, et ce
  // n'est pas nous qui en décidons : le client interroge le global Lua
  // `DrawOnTop`. Voir le .cc.
  int garment = 0;
};

// Où le pantin a atterri, une fois le cadrage résolu.
//
// `origin_x`/`origin_y` = le point où tombe l'origine (0,0) du repère sprite,
// c'est-à-dire l'ORIGINE DE L'ACTEUR — ses pieds, pas le centre de sa boîte.
// C'est exactement la convention `(ox, oy, s)` qu'attendent les couches d'effets
// de costume (`DrawStrCapLayers`, `DrawEzCapTris`) : elles peuvent donc se poser
// sur un pantin composé sans rien recalculer.
struct DollPlacement {
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  float scale    = 1.0f;  // une unité .act -> pixels écran
};

// Rappel invoqué UNE fois, le cadrage résolu et AVANT le premier calque.
//
// 🔴 C'est le seul moment où l'on peut glisser quelque chose DERRIÈRE le pantin.
// Il ne s'agit pas d'une commodité : un effet de costume marqué « avant le
// personnage » doit passer sous TOUS ses calques, cape comprise, et le pantin se
// dessine en un seul appel.
//
// ⚠ Pourquoi un rappel plutôt qu'une fonction de mesure séparée : mesurer
// demanderait de rejouer tout le chargement, l'ancrage et le cadrage une
// deuxième fois — donc de payer deux fois, et surtout de risquer que la mesure
// et le dessin divergent. Une seule traversée, une seule vérité.
using DollUnderlayFn = void (*)(void* ctx, const DollPlacement& placement);

// Réglages optionnels. Ils ont leur structure parce qu'ils sont nombreux, rares,
// et qu'une liste d'arguments à treize positions ne se relit pas.
struct DollDrawOpts {
  int      dir          = 0;       // orientation 0..7 (0 = de face)
  int      anim         = 0;       // type d'action ; pose = anim*8 + dir
  float    anim_seconds = -1.0f;   // horloge ; NÉGATIF = figé sur la 1ʳᵉ image
  uint32_t tint         = 0xFFFFFFFFu;

  // Cadrer sur le CORPS SEUL au lieu de la silhouette entière.
  //
  // 🔴 Par défaut, l'échelle est calculée pour que TOUT rentre — coiffes et cape
  // comprises. C'est ce qu'on veut d'une vignette, où rien ne doit déborder.
  // C'est faux dans un APERÇU de costume : un chapeau volumineux rétrécirait le
  // personnage, et sa taille changerait d'un article survolé au suivant alors
  // que c'est justement la référence de comparaison. Avec ce drapeau, l'échelle
  // ET le centrage ne dépendent que du corps ; les pièces rapportées peuvent
  // dépasser du rectangle — `DrawDoll` ne rogne rien, c'est à l'appelant de
  // prévoir la marge (ou de poser son propre clip s'il veut couper).
  bool fit_body_only = false;

  DollUnderlayFn underlay      = nullptr;
  void*          underlay_ctx  = nullptr;
  DollPlacement* out_placement = nullptr;
};

// Dessine le pantin dans [x, y, w, h] de la fenêtre ImGui courante : ratio
// conservé, corps centré en X, pieds ancrés en BAS du rectangle.
//
//   dir           orientation 0..7 (0 = de face)
//   anim          type d'action (0 = debout, 2 = assis…) ; pose = anim*8 + dir
//   anim_seconds  horloge de l'appelant (ex. ImGui::GetTime()). NÉGATIF = figé
//                 sur la première image, ce que veut une vignette.
//   tint          multiplication de couleur (IM_COL32) ; 0xFFFFFFFF = aucune
//
// ⚠ DEUX horloges, et c'est là qu'est tout le piège.
//
// Le CORPS et la TÊTE ne défilent qu'en MARCHE (anim 1) et en COMBAT (anim 4) —
// vérifié au débogueur (sur un personnage assis, `acteur+0x3c` reste à 0) et
// confirmé en dur dans le client, qui force l'image à 0 pour les autres types
// d'action. Partout ailleurs ils restent sur leur image 0 : leurs images sont
// des poses et des expressions de visage, pas une décoration.
//
// Les ACCESSOIRES s'animent quand même, sur leur propre horloge : c'est
// `Act_ResolveAltAnimFrame`, dont le portage vit dans le .cc. Une coiffe porte
// un MULTIPLE exact des images de la tête et parcourt son SOUS-GROUPE au fil du
// temps réel. D'où des costumes qui vivent sur un personnage immobile.
//
// Rend false si rien n'a pu être dessiné — à l'appelant de poser son
// placeholder. Un membre manquant (coiffe absente du GRF) n'est PAS un échec :
// il est simplement omis.
//
// ⚠ `underlay` et `out_placement` n'interviennent qu'une fois le cadrage
// résolu — donc jamais sur les échecs précoces (corps introuvable, rectangle
// dégénéré), mais bien dans le cas de bord où le cadrage aboutit et où aucun
// calque ne se révèle dessinable. Un appelant qui superpose des effets doit
// donc quand même tester le retour avant sa passe de devant : sans pantin, une
// ancre seule ne veut rien dire.
bool DrawDoll(ImDrawList* draw_list, const DollLook& look, float x, float y,
              float w, float h, const DollDrawOpts& opts);

// Forme courte, pour les appelants qui n'ont besoin que de la pose — la grande
// majorité. Strictement équivalente à la précédente avec des options par défaut.
bool DrawDoll(ImDrawList* draw_list, const DollLook& look, float x, float y,
              float w, float h, int dir = 0, int anim = 0,
              float anim_seconds = -1.0f, uint32_t tint = 0xFFFFFFFFu);

}  // namespace ro
