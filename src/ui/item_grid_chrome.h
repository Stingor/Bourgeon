#pragma once

// ── Le chrome d'une fenêtre à GRILLE D'OBJETS ────────────────────────────────
//
// L'inventaire et le chariot dessinent le même décor, repris du client : le
// bandeau 3-slice du bas (`btnbar_*3.bmp`), le pavage de fond des cases
// (`itemwin_mid.bmp`), les icônes de poids et de nombre, et le redimensionnement
// par PALIER de tuile — une fenêtre qui saute d'une colonne entière plutôt que
// de couper une case en deux.
//
// Les deux fichiers en portaient chacun leur copie, à l'octet près pour trois
// des quatre fonctions de dessin. Ils chargeaient aussi les MÊMES quatre
// bitmaps deux fois, chacun dans ses propres globales.
//
// Ce qui reste chez l'appelant, et c'est voulu : la BOUCLE du strip d'onglets.
// Les trois fenêtres n'ont pas le même modèle d'onglet — le chariot cinq
// familles de types, l'inventaire les mêmes plus un onglet Favoris qui accepte
// un dépôt, l'entrepôt des coffres aux noms choisis par le joueur et un mode
// texte vertical — et chacune y accroche ses propres gestes. Trois modèles
// derrière une seule boucle à rappels coûteraient plus qu'ils ne rendraient.
//
// 🔴 La MESURE du strip, elle, a rejoint ce fichier (`TabStripThickness`). La
// note qui l'en tenait à l'écart disait « chacun lit son `g_tab` et son nombre
// de catégories » : c'est une DIFFÉRENCE, pas un obstacle — les deux sont
// devenus des arguments. Et elle avait déjà coûté un défaut, cf. la fonction.

#include <cstddef>

#include "imgui.h"
#include "ui/game_texture.h"

namespace ro::grid {

// `<racine d'interface>\basic_interface\<file>` — le préfixe est repris d'une
// chaîne de l'exe, donc dans la code-page du client.
void BasicInterfacePath(const char* file, char* out, size_t out_sz);

// Les quatre bitmaps communs, chargés à la PREMIÈRE demande et une seule fois.
// ⚠ À n'appeler que depuis la frame de rendu : la création de texture passe par
// le renderer.
struct Chrome {
  GameTexture bar[3];  // btnbar 3-slice : 0 = gauche, 1 = milieu étiré, 2 = droite
  GameTexture tile;    // itemwin_mid.bmp : le fond d'une case
  GameTexture weight;  // icon_weight.bmp
  GameTexture num;     // icon_num.bmp
};
const Chrome& Assets();

// Le bandeau 3-slice dans [x0..x1] à y0. Repli sur un rectangle plein si les
// bitmaps du client manquent.
void DrawFooterBar(ImDrawList* dl, float x0, float y0, float x1, float bar_h);

// Une icône du bandeau, centrée verticalement sur `cy`. Rend sa largeur, 0 si
// la texture manque — l'appelant s'en sert pour avancer son curseur.
float DrawFooterIcon(ImDrawList* dl, const GameTexture& icon, float x, float cy);

// `itemwin_mid` PAVÉ dans [mn..mx], aligné sur `origin` (la première case) pour
// que le fond et les objets partagent la même marge.
void DrawTiledBg(ImDrawList* dl, const GameTexture& tile, ImVec2 origin,
                 ImVec2 mn, ImVec2 mx);

// ── Redimensionnement par palier de tuile ───────────────────────────────────
// L'appelant renseigne `Snap()` juste avant son `Begin()`, puis passe
// `SnapWindowSize` à `SetNextWindowSizeConstraints`. UN SEUL état, comme pour
// le cadre de HUD : une seule fenêtre est redimensionnée à la fois.
struct SnapState {
  float cell = 32, gap = 0, chromew = 0, chromeh = 0;
  bool  valid = false;
};
SnapState& Snap();
void SnapWindowSize(ImGuiSizeCallbackData* d);

// ── La MESURE du strip d'onglets ────────────────────────────────────────────
//
// 🔴 Ce corps était écrit CINQ fois — deux fois dans l'inventaire, deux fois
// dans le chariot, une dans l'entrepôt — et la note qui l'accompagnait disait
// que les copies « ne peuvent pas fusionner (chacun lit son `g_tab` et son
// nombre de catégories) : elles doivent donc être corrigées ENSEMBLE, à la
// main ». Elles ne l'ont pas été. Le passage à `ro::Px` — la dimension du strip
// suit l'échelle de l'interface — a atteint l'inventaire et l'entrepôt, PAS le
// chariot, resté à 22 px à côté d'une grille agrandie. C'est le prix de la
// consigne « à la main », et la raison d'être de cette fonction.
//
// `set` = le tableau [catégorie][actif, inactif] de la disposition demandée, À
// PLAT : les appelants le déclarent en `Tex[N][2]`, qui est contigu, et passent
// `&tab[0][0]` avec `count` catégories. `horizontal` choisit la dimension
// mesurée, toujours la TRANSVERSE — celle qu'on ne laisse jamais s'étirer,
// l'autre se déduisant du ratio de l'image : la LARGEUR du strip vertical, la
// HAUTEUR de la rangée horizontale.
float TabStripThickness(const GameTexture* set, int count, bool horizontal);

// ── Le champ de FILTRE par nom, au-dessus de la grille ──────────────────────
//
// Les trois fenêtres l'écrivaient à l'identique. Masqué (`visible` faux), le
// filtre est VIDÉ : sinon il continuerait de cacher des objets sans que rien à
// l'écran ne l'explique.
//
// 🔴 `imgui_id` doit rester STABLE d'une version à l'autre — c'est l'identité
// ImGui du champ — et n'est JAMAIS traduit ; seul l'indice l'est.
void DrawNameFilter(const char* imgui_id, ImGuiTextFilter* filter, bool visible);

// ── Les dimensions de la fenêtre à grille ───────────────────────────────────
// À mesurer une fois, juste après le `Begin` : la hauteur de ligne, le bandeau
// réservé en bas (`footer_lines` lignes compactes, la barre 3-slice étirée
// dessous), et la hauteur NÉGATIVE à donner à l'enfant qui porte les cases —
// « toute la place restante moins le bandeau », la convention ImGui.
struct Metrics {
  float line_h  = 0;  // hauteur d'une ligne de texte
  float footer_h = 0; // hauteur réservée au bandeau du bas
  float main_w  = 0;  // taille de la fenêtre : sert à mesurer le chrome du snap
  float main_h  = 0;
  float child_h = 0;  // hauteur (NÉGATIVE) de l'enfant de la grille
};
Metrics Measure(int footer_lines);

// ── L'enfant DÉFILANT qui porte les cases ───────────────────────────────────
//
// Place le curseur par rapport au strip d'onglets — à sa droite en disposition
// verticale, juste dessous en horizontale — ouvre l'enfant sans marge, mesure
// le chrome pour le snap de la frame suivante, et pave le fond.
//
// 🔴 À refermer par `EndItemGrid()` : deux `PushStyleVar` restent en vol.
struct Grid {
  int   cols = 1;           // colonnes de cases qui tiennent dans la largeur
  float cell = 0, gap = 0;  // côté d'une case, et l'écart entre deux
  float avail_h = 0;        // hauteur visible, pour qui compte ses lignes
  ImDrawList* dl = nullptr;
  // Style du MENU CONTEXTUEL, mémorisé AVANT les push : la grille tourne en
  // marge nulle et espacement jointif, et un popup ouvert dans ce scope en
  // hériterait — entrées serrées, sans marge. L'appelant les repousse autour de
  // son `BeginPopup`.
  ImVec2 menu_pad, menu_spacing;
};
Grid BeginItemGrid(const char* imgui_id, bool tabs_vertical, const Metrics& m,
                   const GameTexture& bg);
void EndItemGrid();

// ── Le PONT de l'onglet actif ───────────────────────────────────────────────
// L'onglet sélectionné « mange » le bord qui le sépare de la grille : un petit
// rectangle posé sur ce bord — le DROIT en disposition verticale, le BAS en
// horizontale — donne un passage continu qui souligne l'onglet actif.
// `color` = le corps de l'image de l'onglet, pour que le pont s'y fonde au lieu
// de trancher.
void DrawActiveTabBridge(ImDrawList* dl, ImVec2 tab_min, ImVec2 tab_max,
                         bool tabs_vertical, ImU32 color);

}  // namespace ro::grid
