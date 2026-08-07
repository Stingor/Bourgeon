#pragma once

// ── Les emotes du JEU, posées dans du TEXTE ──────────────────────────────────
//
// Ce sont les mêmes images que les bulles au-dessus des têtes : un unique
// `emotion.spr`/`.act` dont chaque ACTION est une emote, et dont l'index est
// celui du protocole (`ZC_EMOTION`, `emotion_type` côté serveur). Rien à
// télécharger, rien à héberger — le GRF du joueur les a déjà.
//
// Deux usages, une seule source :
//   • le fragment `:nom:` d'une ligne de chat, rendu à hauteur de ligne ;
//   • la grille du sélecteur, où l'on choisit celle qu'on va écrire.
//
// 🔴 Le nom ASCII n'est pas décoratif : c'est LUI qui voyage. Le message part au
// serveur en clair, donc un joueur sans Bourgeon lit « :sweat: » — lisible,
// alors qu'un index nu (« ^e[4] ») ne dirait rien à personne. C'est aussi la
// forme que Discord emploie, ce qui rend la traduction d'un bout à l'autre du
// relais mécanique.
//
// Les noms s'inspirent de l'énumération `emotion_type` du serveur (clif.hpp) —
// le client, lui, ne porte que des clés de bulle d'aide (`TT_REQ_EMOTION_*`), et
// pas pour toutes.
//
// 🔴 MAIS L'ÉNUMÉRATION NE FAIT PAS AUTORITÉ SUR L'ORDRE. La table est indexée
// par l'ACTION du sprite, et les deux numérotations divergent — dès l'index 2, et
// encore après `chat_prohibit`. Ce qui fait foi est ce que le fichier montre à
// l'écran ; le détail et la méthode de vérification sont dans le .cc, au-dessus
// de la table.

#include <cstddef>

#include "imgui.h"

namespace ro {
namespace emote {

// Taille de la table — donc le plus grand identifiant plus un, PAS le nombre
// d'emotes utilisables : une entrée peut être exclue, ou absente du fichier.
// Itérer de 0 à Count() en filtrant sur `Exists` est le bon usage.
int Count();

// Nom ASCII de l'emote `id`, sans les deux-points. nullptr hors table — ou si
// l'emote est EXCLUE (cf. la table dans le .cc : un nom mis à `nullptr` la
// retire de la grille, du parseur et d'ici, sans décaler les identifiants).
const char* Name(int id);

// L'identifiant de `:nom:`, ou -1. `len` est la longueur du nom NU.
int Find(const char* name, size_t len);

// L'emote est-elle utilisable — nommée, non exclue, et réellement présente dans
// `emotion.act` ? Charge le sprite au premier appel. Une table plus longue que
// le fichier est normale : elle suit le protocole, le GRF d'un client donné
// s'arrête où il s'arrête.
bool Exists(int id);

// Dessine l'emote dans [rect_min, rect_max], animée par `anim_seconds`
// (typiquement `ImGui::GetTime()`). Rend false si rien n'a pu être dessiné :
// l'appelant pose alors son repli textuel.
bool Draw(ImDrawList* draw_list, int id, ImVec2 rect_min, ImVec2 rect_max,
          float anim_seconds, bool allow_upscale = false);

// ── Export ───────────────────────────────────────────────────────────────────
// Écrit une emote par GIF animé dans `out_dir` (créé au besoin), nommés
// `<nom>.gif`. Destination : les emojis custom d'un serveur Discord, dont le nom
// doit correspondre au raccourci `:nom:` pour que le relais fasse le pont.
//
// `scale` agrandit d'un facteur ENTIER, au plus proche voisin : une emote RO fait
// une cinquantaine de pixels et Discord l'affiche autour de 48, donc ×2 rend un
// pixel art net là où l'agrandissement du navigateur le lisserait.
//
// Rend le nombre de fichiers écrits, ou -1 si le sprite n'a pas pu être lu.
// N'exporte QUE les emotes utilisables (nommées, non exclues, présentes).
int ExportGifs(const char* out_dir, int scale = 2);

}  // namespace emote
}  // namespace ro
