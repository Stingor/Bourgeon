#pragma once

#include <map>
#include <string>
#include <vector>

#include "features/plugin.h"

// Un marqueur posé par le joueur, en coordonnées de CELLULE.
//
// 🔴 On ne réutilise PAS les fichiers mémo du client (`<dossier>\Memo\$$<carte>.txt`).
// Trois raisons, dont la deuxième suffit : leur format plafonne à 40 lignes et
// sépare les champs par des TABULATIONS (un libellé qui en contient le casse) ;
// le natif RÉÉCRIT le fichier entier quand on supprime un mémo et ne le relit
// que sur ses propres actions, donc deux systèmes qui y écrivent s'écrasent en
// cours de session ; et il est dans la code-page du client là où tous nos
// réglages sont en UTF-8. Le prix payé : nos marqueurs n'apparaissent pas sur la
// grande carte native. Un import se rajoute quand on veut — réparer un fichier
// écrasé, non.
struct MinimapMemo {
  int         x = 0;
  int         y = 0;
  std::string name;
};

// Réglages persistés de la minimap ImGui. Scalaires nus, pour que moonlight_ui
// les sérialise sans rien savoir de l'intérieur (même contrat que les autres
// overlays).
struct MinimapConfig {
  bool enabled     = false;  // interrupteur maître
  // Position de la fenêtre. Elle n'a PAS de réglage numérique : on la déplace en
  // la glissant, et le glissement se persiste ici. Deux sliders feraient double
  // emploi avec le geste, et se contrediraient dès qu'on utilise les deux.
  int  pos_x       = 40;
  int  pos_y       = 60;
  // Côté de la ZONE DE CARTE, en pixels — pas la taille de la fenêtre, qui y
  // ajoute ses marges et la ligne de coordonnées. Réglé à la POIGNÉE, jamais par
  // un curseur : c'est la même raison que pour la position, un geste direct et
  // son doublon numérique finissent par se contredire.
  int  size        = 220;
  // Mode « traverser les clics » : la fenêtre n'attrape plus la souris, donc le
  // jeu la reçoit. Elle cesse du même coup d'être déplaçable et redimensionnable
  // — ce sont trois faces du même choix, pas trois réglages.
  bool locked      = false;
  // Facteur d'agrandissement, réglé à la MOLETTE. 1 = la carte entière ; au-delà
  // on ne voit qu'une fenêtre de 1/zoom, centrée sur le joueur. Bornes et pas
  // sont ceux des boutons + / − du radar natif.
  float zoom       = 1.0f;
  int  bg_alpha    = 70;     // % d'opacité du fond de la fenêtre
  int  map_alpha   = 100;    // % d'opacité de l'image de carte
  bool smooth      = true;   // filtrage LINEAR (la carte est très agrandie)
  // 🔴 Clés RENOMMÉES (ex-`marker_size` / `marker_rgb`) : le repère est passé du
  // point rouge à la flèche du client, et ses deux réglages ont changé de SENS
  // (rayon -> demi-étendue) comme de défaut (rouge -> blanc, l'art portant déjà
  // sa couleur). Le fichier de réglages étant réécrit en entier, garder les
  // anciens noms aurait imposé l'ancienne valeur à tout le monde.
  int  marker_px   = 9;         // demi-étendue de la flèche, en pixels
  int  marker_tint = 0xFFFFFF;  // teinte (blanc = l'art d'origine, intact)
  bool show_coords = true;   // ligne « carte,x,y » sous l'image
  // Nom LISIBLE du lieu, celui que la grande carte native met en titre
  // (« PvP : Room Copass » pour `pvp_n_3-5`). Il est replié sur la largeur de la
  // minimap : plus long que celle-ci, il passe à la ligne au lieu d'être coupé.
  bool show_map_name = true;
  bool show_party  = true;   // carrés des membres du groupe
  bool show_guild  = true;   // pastilles des membres de la guilde
  bool show_quests = true;   // icônes de quête
  bool show_town   = true;   // PNJ / commodités déclarés pour la carte
  bool show_viewpoints = true;  // repères posés par le serveur (`viewpoint`)
  bool show_boss   = true;   // marqueur de boss (Convex Mirror / Bloody Branch)
  // Ferme le radar natif quand la nôtre est active. 🔴 Fermer, pas masquer : le
  // dessin natif est gardé par le POINTEUR de la fenêtre 14, pas par son drapeau
  // de visibilité — la masquer laisserait son quad de carte se dessiner.
  bool replace_native = true;
};

// ── Minimap Bourgeon — étape 1 : la carte et le joueur ───────────────────────
//
// Reproduction en ImGui du radar du client. Cette première étape se limite au
// socle : afficher le .bmp de la carte courante et y faire bouger le point du
// joueur. Elle NE MASQUE PAS encore le radar natif — les deux coexistent, ce
// qui est justement ce qui permet de comparer les deux à l'écran.
//
// Pas de barre de titre : un overlay de carte n'a rien à faire d'un bandeau, et
// le nom de la carte a rejoint la ligne de coordonnées (« carte,x,y »), qui est
// l'endroit où on le lit déjà en jeu. On la déplace en la glissant — d'où
// l'absence de réglage numérique de position.
//
// 🔴 Le radar natif n'est PAS le contenu d'une fenêtre. `UIMinimapZoomWnd`
// (id 14) remplit sa surface de magenta et n'écrit que les coordonnées : le fond
// de carte et les marqueurs sont un QUAD posé par `GameMode_DrawMiniMap`
// (0x00c66ab0) entre la scène 3D et les fenêtres. Conséquence pour la suite :
// masquer la fenêtre 14 (`+0x28`) laisserait le quad se dessiner, parce que la
// garde du dessin teste le POINTEUR `g_MinimapZoomWnd`, pas la visibilité. Seul
// `CloseWindow(14)` — ou un détour du dessin — supprime le radar natif.
// Récit complet : docs/minimap_re.md.
//
// Ce que cette étape recopie du natif, et pourquoi :
//   · le chemin du bitmap — `유저인터페이스\map\<carte>.bmp`, exactement le
//     gabarit que `UIMiniMapWnd_DrawContent` formate (0x0103ea7c), alimenté par
//     le même global de nom de carte (0x015fb9ac) ;
//   · la conversion position monde -> cellule — l'arithmétique de
//     `Scene_WorldPosToCellXY` (0x00c6ac10), SANS son `floor` final, pour que le
//     point glisse au lieu de sauter de cellule en cellule ;
//   · l'axe Y INVERSÉ — la ligne 0 du bitmap est le HAUT de la carte, donc la
//     cellule de plus grand Y ; c'est le `(mapH - py) / mapH` du natif ;
//   · le REPÈRE DU JOUEUR — la flèche `유저인터페이스\map\map_arrow.bmp` du
//     client, pivotée de l'angle que le natif emploie (180 − acteur+0x4C), avec
//     son ombre portée. 🔴 L'ANGLE vient du natif, la CONVENTION de rotation non :
//     recopier la formule de coins de `sub_A74E90` miroite la flèche. Le récit
//     et la preuve chiffrée sont sur `RotatedQuad`, dans le .cc.
class Minimap : public Plugin {
 public:
  // Pose le veto du dessin natif : un détour sur le SITE D'APPEL de
  // `GameMode_DrawMiniMap`. C'est lui, et pas le battement de frame ci-dessous,
  // qui empêche le radar natif de clignoter au téléport — le client le recrée
  // au milieu de sa frame, aucun battement en tête de frame ne peut arriver à
  // temps. Le récit complet est sur `kDrawMiniMapCall`, dans le .cc.
  Minimap();

  const char* name() const override { return "Minimap"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRenderUI() override;

  // Ferme (ou rend) le radar natif, et vide la bascule de fenêtre demandée par
  // le menu. Appelé par `Bourgeon::OnGameFrame`, donc à CHAQUE frame et AVANT
  // que le jeu ne dessine.
  //
  // 🔴 Deux raisons de ne PAS être dans OnTick, et aucune de nous y remettre :
  //   · le client RECRÉE sa fenêtre 14 à chaque entrée de carte ; bridé à
  //     ~100 ms, le rattrapage la laissait vivre le dixième de seconde qu'il
  //     fallait pour la voir (le clignotement RÉSIDUEL d'une frame, lui, est du
  //     ressort du veto de dessin, pas de ce battement) ;
  //   · le bridage décale l'ouverture d'une fenêtre lourde (la carte du monde)
  //     sur une frame quelconque, ce qui la faisait planter par intermittence.
  // 🔴 Et hors frame ImGui dans les deux cas : `CloseWindow` et `MakeWindow`
  // sont des commandes du client, les émettre pendant notre rendu fige le jeu
  // en silence.
  void OnGameFramePulse();

  // Réglages persistés (lus/écrits par moonlight_ui).
  MinimapConfig& config();

  // Marqueurs du joueur, par nom de carte SANS extension. Public parce que
  // `moonlight_ui/settings_containers` les sérialise — même contrat que
  // `MenuIcons::saved_`. Une carte sans marqueur n'a pas d'entrée.
  std::map<std::string, std::vector<MinimapMemo>> memos_;
  // Posé quand un marqueur est ajouté ou retiré ; moonlight_ui le draine et
  // sauvegarde. Évite d'écrire le yaml à chaque frame.
  bool memos_dirty_ = false;

  // Contenu de sa section du panneau Moonlight, et du menu du bouton de la
  // minimap — le même appel aux deux endroits. Appelé par panel_interface.
  void DrawSettings();

 private:
  // Fenêtre native à basculer, mise en file par le menu et exécutée au tick.
  // 🔴 Jamais depuis le rendu : ouvrir ou fermer une fenêtre du client pendant
  // notre frame ImGui fige le jeu en silence.
  int pending_toggle_wnd_ = 0;

  // Vrai depuis que NOUS avons fermé la fenêtre 14. Sert à ne la recréer que si
  // c'est bien nous qui l'avions retirée : certaines cartes n'en ont jamais eu,
  // et la faire apparaître là où le client n'en voulait pas serait une régression.
  bool native_closed_ = false;
};
