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
// Ce qui reste chez l'appelant, et c'est voulu : les onglets de CATÉGORIE
// (`g_tab`, `g_tabh`), dont la liste diffère d'une fenêtre à l'autre, et la
// mesure du strip qui en découle.

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

}  // namespace ro::grid
