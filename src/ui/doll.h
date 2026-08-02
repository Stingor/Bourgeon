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
// ── Pourquoi il existe à côté de BasicInfo::RenderDoll ───────────────────────
// L'autre chemin laisse le CLIENT rendre l'acteur et CAPTURE les quads qu'il
// aurait dessinés (hook sur Actor_SubmitSpriteQuad). C'est juste et complet,
// mais ça exige un acteur vivant, un hook global, et une dizaine d'offsets de
// structure devinés. Ici, rien de tout ça : on lit les fichiers.
//
// 🔴 Les deux coexistent volontairement le temps de comparer. Voir le TODO de
// bascule dans char_select.cc.
//
// ── Ce qu'il ne fait pas ─────────────────────────────────────────────────────
// Ni arme ni bouclier — comme le char-select natif, qui ne les montre pas non
// plus. Les compagnons (cart, faucon) et les effets .str n'en sont pas non plus.

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
// ⚠ Animer n'est pas toujours souhaitable. Les images d'un .act de TÊTE sont
// des expressions de visage, pas des étapes d'animation : les faire défiler
// donne un personnage qui cligne et regarde partout. Le rendu natif ne les
// anime que sur Marche (1) et Combat (4), et gèle Repos (0) et Assis (2).
//
// La cadence vient du .act du CORPS et s'applique à toutes les pièces, sinon
// elles se désynchronisent. Chaque pièce est bornée à SON nombre d'images : une
// coiffe plus courte que le corps rejoue les siennes au lieu de disparaître.
//
// Rend false si rien n'a pu être dessiné — à l'appelant de poser son
// placeholder. Un membre manquant (coiffe absente du GRF) n'est PAS un échec :
// il est simplement omis.
bool DrawDoll(ImDrawList* draw_list, const DollLook& look, float x, float y,
              float w, float h, int dir = 0, int anim = 0,
              float anim_seconds = -1.0f, uint32_t tint = 0xFFFFFFFFu);

}  // namespace ro
