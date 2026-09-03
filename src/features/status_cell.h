#pragma once

// ── La CASE d'un état (buff / debuff) ───────────────────────────────────────
//
// Le pendant de `item_cell` pour les EFST : un seul endroit qui sait à quoi
// ressemble un état à l'écran, et tout ce qui en affiche passe par là.
//
// 🔴 POURQUOI CE FOYER EXISTE. Trois surfaces dessinaient la même icône, chacune
// avec sa boucle : les tuiles de la grille de groupe, les lignes de la fenêtre
// Groupe/Amis, la barre d'états de la cible. Elles ne différaient que par leur
// placement — et la troisième avait déjà pris de l'avance (grisage, compte à
// rebours) que les deux autres n'avaient pas. Ajouter l'infobulle aurait fait
// une quatrième écriture du même geste, et scellé la divergence.
//
// Le contrat est donc : l'APPELANT place, `status_cell` peint. Il donne un
// rectangle et des options ; il ne sait rien des textures, du Lua, ni de la
// façon dont une durée se dessine.

#include <cstdint>
#include <vector>

#include "features/systems/status_effects.h"
#include "imgui.h"

namespace statuscell {

// Le grisage de la part ÉCOULÉE.
//
// ⚠ Ces valeurs partent dans les réglages : ne PAS les intercaler.
enum Sweep {
  kSweepNone = 0,
  kSweepRadial,    // le balayage horaire des jeux de rôle, depuis midi
  kSweepVertical,  // le voile descend depuis le haut, plus lisible en petit
};

struct Style {
  int   sweep = kSweepNone;
  ImU32 sweep_color = IM_COL32(0, 0, 0, 140);
  // Le compte à rebours sous l'icône. `time_px` à 0 = pas de texte.
  float time_px = 0.0f;
  // Grisée quand l'entité est hors ligne, comme les icônes de classe.
  bool  dim = false;
  // Opacité de TOUT ce que la case dessine — image, pastille, voile, compte à
  // rebours. 1 = opaque.
  //
  // 🔴 UN CHAMP, ET NON `ImGuiStyleVar_Alpha`. Le style d'ImGui n'est appliqué
  // que par les widgets qui le LISENT (`GetColorU32` le multiplie) ; les
  // primitives d'un `ImDrawList` — `AddImage`, `AddRectFilled`, `AddText` —
  // l'ignorent totalement. Or cette case ne dessine QUE des primitives : un
  // `PushStyleVar(Alpha)` autour d'elle n'a aucun effet, et ça ne se voit qu'à
  // l'écran, sans le moindre avertissement.
  float alpha = 1.0f;
};

// Peint UNE case dans `p0..p1`, et rend son infobulle si le curseur est dessus.
//
// Rend faux quand rien n'a été peint — cet EFST n'a pas d'icône, ou sa texture
// n'est pas encore chargée. L'appelant qui compte ses cases doit en tenir
// compte : une case qui ne se dessine pas ne doit pas prendre de place.
//
// ⚠ `tooltip` ouvre une infobulle ImGui : à n'activer que sur une surface qui a
// le droit d'en poser une. Une tuile de grille dont l'infobulle porte déjà tout
// le membre n'en veut pas une seconde par-dessus.
// `took_hover`, s'il est fourni, est mis à true quand le curseur était sur
// cette case — que l'infobulle ait été posée ou non. L'appelant s'en sert pour
// TAIRE la sienne : une ligne de liste et une tuile de grille ont la leur, qui
// couvre toute leur surface, et deux infobulles superposées se disputeraient le
// même survol. La plus PRÉCISE gagne.
bool Draw(const StatusEffects::Entry& e, ImVec2 p0, ImVec2 p1,
          const Style& style, bool tooltip, bool* took_hover = nullptr);

// ── La RANGÉE de cases ──────────────────────────────────────────────────────
//
// 🔴 POURQUOI CE SECOND FOYER. `Draw` a réuni la CASE ; sa disposition, elle,
// était encore recopiée quatre fois — grille de groupe, liste Groupe/Amis, barre
// de la cible, barre de mes propres états. Les quatre portaient le même
// squelette : le plafond réparti sur les lignes (`(max + rows - 1) / rows`), le
// retour à la ligne, et le `continue` qui empêche une case invisible de prendre
// la place d'une autre. Elles ne différaient que par le SENS et l'écrêtage.
//
// Une cinquième copie serait arrivée avec la barre de mes états. Le réglage
// « lignes » avait déjà dû être écrit trois fois ; le suivant l'aurait été
// quatre.

// Le rangement, quand il y a plus d'états que de place. Les valeurs partent
// dans les réglages : ne PAS les intercaler.
enum Sort {
  kSortArrival = 0,  // l'ordre où ils sont tombés
  kSortEndingSoon,   // bientôt fini d'abord — ce qu'il faut relancer
  kSortLongest,      // plus long d'abord — le fond stable
};

struct RowOpts {
  float side = 16.0f;   // côté d'une case
  float gap  = 1.0f;    // écart entre deux cases
  int   max  = 8;       // plafond TOTAL, toutes lignes confondues
  int   rows = 1;       // nombre de lignes
  int   sort = kSortArrival;
  // De droite à gauche : `origin` est alors le coin haut-DROIT.
  //
  // 🔴 Le sens n'est pas cosmétique. Une rangée qui grandit vers la gauche garde
  // ses icônes à la même place quand une nouvelle tombe ; empiler par la gauche
  // fait glisser toute la rangée à chaque changement, et l'œil perd ce qu'il
  // suivait.
  bool  rtl = false;
  // Vers le HAUT : `origin` est alors le coin BAS. Même raison que `rtl`, sur
  // l'autre axe — une barre posée en bas de l'écran doit garder sa première
  // ligne collée au bord, et faire monter les suivantes.
  bool  up = false;
  // Garder les états les plus RÉCENTS quand il y en a trop. Sur un allié, un
  // buff qui vient de tomber est ce qu'on regarde — pas celui qui dure depuis
  // dix minutes. Sans effet quand `sort` impose déjà un ordre.
  bool  newest_first = false;
  // Le bord à ne pas franchir (0 = aucune contrainte). Sert aux barres, dont la
  // largeur est celle d'un cadre que le joueur tire.
  float limit = 0.0f;
};

struct RowResult {
  int    drawn  = 0;
  // Le bord ATTEINT : le plus à gauche en `rtl`, le plus à droite sinon. Un
  // appelant qui écrit du texte à côté s'y découpe.
  float  edge   = 0.0f;
  ImVec2 size{};  // encombrement total, lignes comprises
};

// Pose une rangée de cases à partir de `origin`, et rend ce qu'elle a occupé.
//
// ⚠ Le `Style` vaut pour TOUTE la rangée : les quatre appelants le construisent
// déjà hors de leur boucle, avec des valeurs constantes.
RowResult DrawRow(const std::vector<StatusEffects::Entry>& list, ImVec2 origin,
                  const RowOpts& opts, const Style& style, bool tooltip,
                  bool* took_hover = nullptr);

// Complète `out` de faux états jusqu'à `want`, pour RÉGLER un affichage sans
// attendre d'en avoir de vrais.
//
// 🔴 Aucun réglage d'aperçu n'est PERSISTÉ, où qu'il soit branché : c'est un
// outil de pose, pas un mode de jeu. Le retrouver allumé à la session suivante
// ferait croire à des buffs qu'on n'a pas, et le premier réflexe serait d'en
// chercher la cause.
//
// ⚠ Ne touche pas aux états déjà présents, et n'en double aucun.
void AppendPreview(std::vector<StatusEffects::Entry>* out, int want);

// Cet état a-t-il un rendu DE REPLI, faute d'image côté client ?
//
// Deux seulement : l'aveuglement et le saignement, que l'énumération connaît
// mais dont le Lua du client ne déclare ni l'id ni le fichier. Le registre s'en
// sert pour ne pas les écarter — il rejette normalement tout ce qui n'a pas
// d'icône, et ces deux-là seraient invisibles alors qu'on sait les dessiner.
bool HasFallback(uint16_t efst);

// Le NOM de l'état, tel que le client l'écrit dans sa propre infobulle.
// Chaîne vide s'il n'en a pas. Mémorisé — c'est un appel Lua.
const char* Name(uint16_t efst);

// L'infobulle seule, pour un appelant qui a déjà peint sa case autrement (une
// tuile de grille, par exemple, dont le survol est testé sur toute la ligne).
void Tooltip(const StatusEffects::Entry& e);

// ── Le bloc de RÉGLAGE des icônes d'état ────────────────────────────────────
//
// Les deux surfaces qui montrent les états d'autres MEMBRES — les tuiles de la
// grille de groupe (`overlays/party_frames`) et les lignes de la fenêtre
// Groupe/Amis (`windows/party_friend_window`) — offraient le même bloc de
// réglage, recopié à l'identique : mêmes libellés, mêmes infobulles, et jusqu'au
// même commentaire de dix lignes sur la collision d'identifiants ImGui entre
// traductions. Seules les bornes du curseur de taille différaient.
//
// 🔴 Deux copies de la même infobulle, ce sont deux entrées au catalogue de
// traduction, et une correction d'un seul côté produit deux textes divergents
// pour un seul réglage. C'est le même raisonnement que pour le DESSIN de la
// case, plus haut dans ce fichier.
//
// ⚠ Reste à l'appelant : la case « Buffs et debuffs » elle-même et son
// infobulle. Celle-là dit où les icônes se posent — à droite d'une tuile, à
// gauche d'une pastille — et ce n'est pas la même chose des deux côtés.
struct SettingsRefs {
  bool* preview;     // aperçu (faux statuts) : JAMAIS persisté, cf. le corps
  int*  size_px;
  int*  max_icons;
  int*  rows;
  bool* show_time;
  int*  sweep;       // une valeur de `Sweep`
};

// Rend true si un réglage PERSISTANT a changé — l'aperçu n'en fait pas partie.
bool DrawSettings(const SettingsRefs& refs, int size_min_px, int size_max_px);

}  // namespace statuscell
