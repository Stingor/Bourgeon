#pragma once

// ── La palette d'emoji Unicode ───────────────────────────────────────────────
//
// Rien à voir avec `ui/game_emotes.h`, malgré l'usage voisin, et la distinction
// vaut d'être tenue :
//   · une EMOTE du jeu est une image du GRF (`emotion.act`), qui voyage sur le
//     fil sous forme de raccourci ASCII `:nom:` et que seul Bourgeon sait
//     redessiner ;
//   · un EMOJI est du TEXTE. Il part tel quel, s'affiche sur Discord sans
//     traduction, et n'importe quel client capable de l'écrire le montre.
//
// D'où deux onglets distincts dans le sélecteur du chat plutôt qu'une grille
// mélangée : ce qui arrive au bout du fil n'est pas de même nature.
//
// Les chaînes sont de l'UTF-8, et elles sont écrites en ÉCHAPPEMENTS `\U…` dans
// le .cc — voir l'avertissement qui y est posé, c'est une contrainte du
// compilateur, pas un choix esthétique.

namespace ro {
namespace emoji {

// Un groupe affiché d'un bloc, avec son intertitre.
//
// 🔴 `label` est en FRANÇAIS NU : c'est l'appelant qui le passe à `i18n::Tr` au
// moment de l'afficher, comme les items de `ro::RoCombo`. L'envelopper ici le
// traduirait DEUX fois — le gabarit d'export se remplirait de textes déjà
// traduits, et on chercherait longtemps d'où ils viennent.
struct Category {
  const char*        label;
  const char* const* items;  // chaînes UTF-8, une par emoji
  int                count;
};

int             CategoryCount();
const Category& CategoryAt(int index);  // index hors bornes -> la première

// Le total, toutes catégories confondues. Sert à dimensionner une grille.
int Count();

}  // namespace emoji
}  // namespace ro
