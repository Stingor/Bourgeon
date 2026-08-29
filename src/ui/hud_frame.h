#pragma once

#include "imgui.h"
#include "ui/align_grid.h"

// ── Cadre de HUD libre ───────────────────────────────────────────────────────
//
// La brique des éléments d'interface que le joueur POSE lui-même : une zone
// rectangulaire sans barre de titre, sans bordure ImGui, qu'on saisit n'importe
// où pour la déplacer et par ses BORDS pour la redimensionner. C'est ce que sont
// déjà les barres de Basic Info et les cadres du portrait ; c'est ce que sont
// les éléments du HUD de cible.
//
// 🔴 Le comportement est celui de Basic Info, au geste près — parce qu'un HUD
// qui se manipule autrement qu'un autre HUD du même jeu est un HUD raté :
//
//   · **poignée visible** : triangle clair au coin bas-droit tant que le cadre
//     n'est pas verrouillé (le jeu dessine son propre curseur, donc le curseur
//     de redimensionnement du système ne se voit jamais — il faut un repère
//     DANS le cadre) ;
//   · **bords qui s'allument** au survol et pendant le tirage, un coin en
//     allumant deux ;
//   · **aimantation entre voisins** (`sticky`) : pendant un déplacement, les
//     bords s'alignent sur ceux des cadres frères — même bord, bord opposé,
//     juste après, juste avant ;
//   · **grille d'alignement** partagée, appliquée au coin déplacé et à chaque
//     bord tiré ;
//   · **CTRL + déplacement** : tout le bloc de cadres qui se TOUCHENT suit,
//     d'un seul tenant ;
//   · **clamp écran** en dernier, parce que l'aimantation peut elle-même
//     pousser un cadre dehors.
//
// ── Ce que « verrouillé » veut dire ─────────────────────────────────────────
//
// `locked` fige la géométrie ET rend le cadre transparent aux clics
// (`NoInputs`) : un cadre verrouillé ne déclare jamais `WantCaptureMouse`, donc
// le clic tombe dans le jeu. C'est la différence entre un HUD qu'on met en place
// et un HUD dont on se sert.
//
// ✅ Basic Info — les barres et les étiquettes du portrait — a migré ici le
// 2026-08-24. Ce module avait été écrit d'après sa mécanique, geste pour geste,
// précisément pour que la migration ne change rien à ce que le joueur ressent.
// Les seuls écarts sont des ALIGNEMENTS sur le reste du HUD, et ils sont dits
// là où ils se produisent : les barres gagnent le bloc CTRL, les textes de
// cadre se tronquent au lieu de déborder sur le voisin.

namespace ro {

// Géométrie mémorisée d'un cadre, en pixels écran. C'est ce que l'appelant
// persiste.
struct HudRect {
  int x = 0;
  int y = 0;
  int w = 120;
  int h = 20;
};

// Les cadres FRÈRES : ceux avec qui celui-ci s'aimante, et qui le suivent en
// déplacement CTRL. `shown` peut être nul (tous visibles). `stride` permet de
// pointer un tableau de structures plus grosses qu'un HudRect — l'appelant donne
// l'adresse du premier `HudRect` et la distance entre deux éléments.
struct HudFrameSiblings {
  HudRect*    first  = nullptr;  // premier cadre du groupe
  const bool* shown  = nullptr;  // visibilité, même indexation (nullptr = tous)
  int         count  = 0;
  int         stride = 0;        // octets entre deux cadres (0 = sizeof(HudRect))

  HudRect* At(int i) const {
    if (!first || i < 0 || i >= count) return nullptr;
    const int step = stride > 0 ? stride : static_cast<int>(sizeof(HudRect));
    return reinterpret_cast<HudRect*>(reinterpret_cast<char*>(first) + step * i);
  }
  bool Shown(int i) const { return !shown || shown[i]; }
};

struct HudFrameOpts {
  bool  locked   = false;    // fige + laisse passer les clics
  bool  border   = false;    // liseré sombre de 1 px
  float rounding = 4.0f;     // arrondi du fond et du liseré
  // Fond peint sous le contenu (RGBA 0..1). nullptr = aucun fond.
  const float* bg = nullptr;
  // Grille d'alignement partagée (celle de MoonlightUi). nullptr = pas d'aimant.
  // Passée en paramètre plutôt que lue ici : `ui/` ne doit rien savoir de
  // `features/`.
  const AlignGrid* grid = nullptr;
  // Aimantation entre cadres frères, et bloc CTRL. `index` est la place de CE
  // cadre dans le groupe ; -1 = pas de groupe.
  HudFrameSiblings siblings;
  int   index    = -1;
  bool  sticky   = true;
  float min_w    = 8.0f;     // taille minimale, en dessous de laquelle le
  float min_h    = 8.0f;     // redimensionnement s'arrête
  // 🔴 Cadre CLIQUABLE bien que VERROUILLÉ : il garde sa géométrie figée, mais
  // reprend la souris au jeu au lieu de la laisser passer. C'est la différence
  // entre un HUD qui AFFICHE et un HUD dont on SE SERT.
  //
  // ⚠ La reprise est TOTALE, comme pour n'importe quelle fenêtre ImGui : le
  // clic droit et la molette ne vont plus au jeu non plus tant que le curseur
  // est dessus (le WndProc les bloque dès qu'une fenêtre ImGui non
  // clic-traversante est sous le curseur). Un cadre cliquable est donc à
  // n'allumer que quand il sert vraiment — l'appelant décide à chaque frame.
  bool  clickable = false;
  // Repère de CENTRE, dessiné seulement quand le cadre est déverrouillé.
  //
  // 🔴 Centrer un HUD à l'œil est faux d'une poignée de pixels, et l'erreur ne
  // se voit qu'une fois le cadre reverrouillé — trop tard pour la corriger sans
  // tout rouvrir. La grille d'alignement aimante les BORDS ; ce repère donne le
  // point que l'on veut réellement poser sur une de ses lignes.
  //
  // Il disparaît avec le déverrouillage : c'est un outil de pose, pas une
  // décoration.
  bool  center_mark = false;
};

// Ce qu'un cadre CLIQUABLE (`opts.clickable`) a reçu cette frame. Rempli par
// BeginHudFrame quand l'appelant en fournit un ; laissé à zéro sinon.
struct HudFrameClicks {
  bool hovered = false;
  bool left    = false;  // appui gauche FRAIS sur le cadre
  bool right   = false;  // appui droit  FRAIS sur le cadre
};

// Ouvre le cadre `id` (identifiant ImGui, donc stable et unique). Met à jour
// `rect` — et les frères, en déplacement CTRL — quand le joueur agit, et pose
// `*geometry_changed` à true dans ce cas ; l'appelant s'en sert pour ne
// persister qu'une fois, à la fin du geste.
//
// Renvoie true si le contenu doit être dessiné. `EndHudFrame` doit TOUJOURS être
// appelé ensuite, y compris sur false (règle Begin/End d'ImGui).
//
// `clicks` ne sert qu'aux cadres cliquables ; nul, ou cadre non cliquable, il
// n'est pas touché.
bool BeginHudFrame(const char* id, HudRect* rect, const HudFrameOpts& opts,
                   bool* geometry_changed, HudFrameClicks* clicks = nullptr);
void EndHudFrame();

// Texte CENTRÉ dans le cadre, à `px` pixels de haut, avec une ombre portée
// proportionnelle — sans quoi elle disparaît sous un gros texte. C'est le rendu
// des cadres de Basic Info, à l'identique.
//
// 🔴 La taille est passée explicitement et le texte mesuré à CETTE taille
// (`CalcTextSizeA`), au lieu de s'en remettre à `SetWindowFontScale` : mesurer à
// une taille et dessiner à une autre décale le centrage, et c'est exactement le
// travers qu'on corrige ici.
void HudCenteredText(ImDrawList* draw_list, ImVec2 p0, ImVec2 p1,
                     const char* text, ImU32 color, float px);

// Texte d'une barre : centré, blanc, à la taille courante. Cas particulier de
// HudCenteredText, celui des barres de Basic Info.
void HudBarText(ImDrawList* draw_list, ImVec2 p0, ImVec2 p1, const char* text);

// Un cadre — N'IMPORTE LEQUEL — est-il en cours de déplacement ?
//
// 🔴 GLOBAL, ET C'EST UN PIÈGE. Il sert à garder visible un élément dont le
// contenu aurait disparu (une cible qui s'en va pendant qu'on place son HUD) :
// on ne retire pas sous les doigts du joueur ce qu'il est en train de poser.
// Mais une surface qui le teste apparaît dès qu'on saisit N'IMPORTE quel autre
// cadre — saisir la barre d'états faisait surgir les cinq cadres vides de la
// fenêtre de cible.
//
// ⚠ Préférer `HudFrameDraggingIs` (un cadre) ou `HudFrameDraggingId` (une
// famille, par préfixe). Plus aucun appelant n'utilise cette forme-ci.
bool HudFrameDragging();

// L'identifiant du cadre actuellement SAISI, ou nullptr si aucun.
//
// 🔴 POURQUOI CE PENDANT EXISTE. `HudFrameDragging()` est GLOBAL : il dit qu'un
// cadre est tenu, pas lequel. Les surfaces qui apparaissent sur MAJ le testaient
// pour ne pas se dérober sous les doigts du joueur — et se montraient donc
// toutes dès qu'on en déplaçait UNE. Saisir la barre d'états faisait surgir les
// cadres vides de la fenêtre de cible.
//
// Une surface doit donc comparer : `== son id` pour un cadre unique, ou par
// PRÉFIXE pour une famille de cadres frères (la fenêtre de cible en a cinq).
const char* HudFrameDraggingId();

// Ce cadre-CI est-il celui qu'on déplace ? La forme courante du test ci-dessus,
// pour une surface qui n'a qu'un cadre.
bool HudFrameDraggingIs(const char* id);

}  // namespace ro
