#include "features/fx/grey_world.h"

#include <Windows.h>

#include <cstdint>
#include <vector>

#include "imgui.h"
#include "bourgeon.h"                  // chat_window(), IsGameActive, IsMapLoading
#include "features/fx/ground_paint.h"  // levier 3 : le sol uni, déjà écrit
#include "features/systems/login_spectator.h"  // le décor de login n'est pas le jeu
#include "features/windows/chat_window.h"      // « @refreshmap » : armer, puis suivre son départ
#include "ragnarok/game_scene.h"       // gamescene:: — le foyer des offsets de scène
#include "ragnarok/globals.h"          // rag::ActiveModeIfReady(), kCurrentMapNameAddr
#include "ragnarok/render.h"           // render::Context(), kSpriteRefCacheAddr
#include "ui/color_codec.h"            // ro::ArgbFromPicker : LA conversion ARGB
#include "ui/game_texture.h"           // ro::texmgr:: — le gestionnaire de ressources
#include "ui/ro_imgui.h"               // ro::RoCheckbox
#include "ui/ro_widgets.h"             // mui::WheelSliderInt, RoColorSwatch
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"
#include "utils/log_console.h"  // LogDiag : le rebuild doit pouvoir dire ce qu'il fait
#include "utils/startup_settings.h"  // lecture AVANT la première carte

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// Backend actif (DX9 vs DX7) — défini dans la couche d3d9. Le sol uni n'existe
// que sur le chemin DX9 ; le reste vaut pour les deux.
extern bool g_imgui_dx7_active;

namespace grey_world {
namespace {

// ── Adresses natives (client 20250716, base 0x400000) ────────────────────────
//
// Les trois fonctions ci-dessous sont documentées dans l'IDB (commentaires posés
// le 2026-09-01). Ce qui suit n'est que le strict nécessaire pour les appeler.

// Rsm3dModel::RenderNode(this, worldMatrix, backface, cull, draw) __thiscall.
// Rend UN nœud de modèle .rsm puis recurse sur ses enfants par vtbl+0x0C. C'est
// le SEUL chemin des décors du .rsw : les mobs et PNJ 3D passent par Granny
// (.gr2), les acteurs par CActorSprite_RenderModel*. Court-circuiter ici ôte le
// décor sans toucher à une seule entité.
constexpr uintptr_t kRsmRenderNode = 0x00a59e20;

// CScene::RenderCellsAndCursor(this) __fastcall — la passe de scène qui dessine
// la trace de navigation, le curseur de destination au sol et la case de
// l'homoncule. `scene` (le contexte que réclame tout dessin au sol) = this + 152.
constexpr uintptr_t kSceneRenderCells = 0x00a7b0a0;
constexpr int       kSceneCtxOffset   = 152;

// C3dGround15::DrawCellQuad(scene, cellX, cellY, argb, spriteRes, uvScale8f).
//
// 🔴 __thiscall, PAS __stdcall — Hex-Rays affiche __stdcall parce que la fonction
// n'utilise pas `this` elle-même, mais elle transmet ecx INTACT à
// C3dGround15_GetCellCorners, qui lit this+8. Appeler sans poser `this` rend les
// coins de cellule depuis une adresse arbitraire. On émule en __fastcall (edx
// ignoré), comme partout ailleurs dans le projet.
constexpr uintptr_t kDrawCellQuad = 0x00a63800;

// ── Les briques pour dessiner une case NOUS-MÊMES ────────────────────────────
//
// `C3dGround15_DrawCellQuad` couvre la case ENTIÈRE : elle ne peut donc pas
// laisser de joint entre deux carreaux, et c'est précisément le joint qui fait
// lire un quadrillage. On refait donc son travail — c'est cinquante lignes, et
// elles sont copiées de son désassemblage, pas devinées.
//
// Conventions vérifiées à l'épilogue, une par une :
//   GetCellCorners            retn 0Ch  -> this en ecx + 3 args pile
//   AcquirePrimRecord         retn      -> la file en ecx, rien sur la pile
//   World_ProjectPointToScreen retn 0Ch -> la file en ecx + 3 args pile
//   RenderQueue_InsertPrimitive retn 08h -> la file en ecx + 2 args pile
constexpr uintptr_t kGetCellCorners  = 0x00a62b70;
constexpr uintptr_t kAcquirePrimRec  = 0x0053add0;
constexpr uintptr_t kProjectToScreen = 0x00554380;
constexpr uintptr_t kInsertPrimitive = 0x00550b10;

// Un sommet de la file fait 32 octets : x,y,z,rhw écrits par la projection,
// puis la couleur, le spéculaire, et les coordonnées de texture.
constexpr int kVertexStride = 32;
constexpr int kVertexZ      = 8;
constexpr int kVertexColor  = 16;
constexpr int kVertexU      = 24;

// Le biais de profondeur du client, à l'identique : c'est lui qui fait passer le
// quad DEVANT le sol sans z-fighting. Le recopier plutôt que le réinventer.
constexpr float kDepthBias = 0.000030517578f;

// SpriteRes_GetOrLoadByName(cache, nom, 0, 0, 1, 0) __thiscall — get-or-load par
// nom dans l'arbre du cache de sprites. Rend la ressource, AVEC un addref.
//
// 🔴🔴 CINQ arguments sur la pile, pas trois. Le prototype d'Hex-Rays n'en montre
// que trois, et l'appeler ainsi laisse 8 octets sur la pile à CHAQUE frame :
// l'appelant récupère alors un `this` décalé, et le client meurt plus loin, dans
// une fonction qu'on n'a pas touchée (0xC0000005 sur `mov eax,[esi+1B0h]` au
// début de CScene_RenderCellsAndCursor, avec esi = une coordonnée de cellule).
// LA PREUVE EST DANS L'ÉPILOGUE : `retn 14h` @0x005688DF = 20 octets = 5 args.
// Les valeurs sont celles que le natif passe pour `grid.tga` (0, 0, 1, 0) — on
// les recopie plutôt que de les deviner.
constexpr uintptr_t kSpriteResGetOrLoad = 0x00568760;

// Les 8 multiplicateurs d'UV que le client passe pour une tuile pleine. On les
// réemploie tels quels : ce sont EXACTEMENT ceux du curseur de destination.
constexpr uintptr_t kCellQuadDefaultUV = 0x01211c30;

// ── Les styles de case, dans l'ordre de Config::Pattern ──────────────────────
//
// Deux viennent du CLIENT, la troisième est à NOUS :
//
//   grid.tga                 le marqueur du curseur de destination. 🔴 C'est un
//                            ANNEAU, pas une croix (mesuré sur capture le
//                            2026-09-01) — et son CENTRE EST TRANSPARENT.
//   effect\SquareRange.tga   le cadre carré des sorts de zone.
//   bourgeon_cell.tga        la nôtre : un carré blanc opaque (tools/gen_cell_tga.py),
//                            à plat dans data\texture\ comme les autres. Le nom est
//                            PRÉFIXÉ parce que le disque passe avant les GRF : un
//                            « cell.tga » masquerait une texture du jeu du même nom,
//                            et data.grf étant chiffré on ne peut pas vérifier
//                            qu'elle n'existe pas.
//
// ⭐ POURQUOI UNE TEXTURE À NOUS. On avait d'abord voulu s'en passer : DrawCellQuad
// multiplie les UV par les 8 flottants qu'on lui donne, donc poser LE MÊME point
// aux quatre coins fait échantillonner un seul texel, étiré sur toute la case =
// un aplat. Encore faut-il que ce texel soit OPAQUE — et le centre de `grid.tga`
// ne l'est pas : l'anneau est creux, le carreau sortait entièrement vide.
// Viser « au juge » un point de l'anneau serait un pari invérifiable, `data.grf`
// de Moonlight étant chiffré. Un carré blanc de 4 Ko règle la question pour de
// bon, et le diffuse du sommet lui donne sa couleur — une seule texture sert donc
// aux trois familles de cases.
//
// La BORDURE, elle, ne vient d'aucune texture : c'est le JOINT (cf. DrawCellShrunk).
//
// ⚠ Si un nom ne résout pas, le client rend son sprite de REPLI plutôt que
// nullptr (cf. le commentaire IDB de SpriteRes_GetOrLoadByName) : une texture
// absente se voit donc à l'écran, elle ne se détecte pas en code.
struct CellStyle {
  const char* texture;
  float u, v;  // < 0 : texture entière (UV par défaut du client)
};
constexpr CellStyle kCellStyles[] = {
    {"grid.tga",                -1.0f, -1.0f},  // anneau
    {"effect\\SquareRange.tga",  -1.0f, -1.0f},  // carrelage
    {"bourgeon_cell.tga",         0.5f,  0.5f},  // carreau plein : un seul texel
};
static_assert(sizeof(kCellStyles) / sizeof(kCellStyles[0]) ==
                  Config::kPatternCount,
              "kCellStyles doit couvrir exactement Config::Pattern");

// ── Offsets ──────────────────────────────────────────────────────────────────
// Tout ce qui décrit la scène et le terrain vit dans `gamescene::` — le dessinateur
// de sol (kAmGround), la .gat (kAmTerrain, kTerrainCells, kCellStride, kCellType)
// et les deux types de case infranchissables y ont été relevés avec ce module.
// Rien de tout cela ne se redéclare ici.
//
// Position monde d'un acteur : vec3 en +0x10 (x, y, z). Le déplacement au
// clavier lit le même bloc pour demander sa case au client.
constexpr int kActorPosVec = 0x10;

// ── État ─────────────────────────────────────────────────────────────────────
Config g_cfg;

using RsmRenderFn   = int(__fastcall*)(void*, void*, void*, int, int, int);
using SceneCellsFn  = void(__fastcall*)(void*);
using DrawCellFn    = void(__fastcall*)(void*, void*, void*, int, int, uint32_t, void*, void*);
using SpriteResFn   = void*(__fastcall*)(void*, void*, const char*, int, int, int, int);

RsmRenderFn  g_orig_rsm         = nullptr;
SceneCellsFn g_orig_scene_cells = nullptr;

// ── La FAMILLE d'une case ────────────────────────────────────────────────────
// Trois familles seulement : ce qu'on marche, le mur, et le vide qu'on traverse
// à distance. C'est ce qui décide de la couleur, et c'est aussi la frontière que
// suit le motif « contour » — un changement de famille est un bord à montrer,
// alors qu'un changement de type à l'intérieur d'une même famille ne se voit pas
// et n'intéresse personne.
int CellFamily(const char* cells, int width, int height, int x, int y) {
  if (x < 0 || y < 0 || x >= width || y >= height) return -1;  // hors carte
  const int type = *reinterpret_cast<const int*>(
      cells + (static_cast<size_t>(y) * width + x) * gamescene::kCellStride +
      gamescene::kCellType);
  if (type == gamescene::kCellBlocked) return 1;
  if (type == gamescene::kCellSnipeable) return 2;
  return 0;
}

// ── GreyWorld se tait pendant le DÉCOR DE LOGIN ──────────────────────────────
//
// 🔴 Le fond de l'écran de connexion est une VRAIE carte, chargée par une session
// spectateur : elle passe donc par les mêmes chargeurs. Depuis que nos détours
// existent dès le démarrage de la DLL (cf. LoadStartupState), ils la frappaient
// aussi — décor déchargé et terrain aplati, c'est-à-dire un décor de login
// ruiné. Le joueur, lui, n'a rien réglé pour cet écran-là.
//
// `spectator::Active()` couvre la connexion ET la présence dans le monde : tout
// le temps où le décor tourne, y compris pendant qu'il charge sa carte.
bool Enabled() { return g_cfg.enabled && !spectator::Active(); }

// ── Ne pas charger les décors du tout ────────────────────────────────────────
//
// 🔴 CE N'EST PAS UN RÉGLAGE : c'est la moitié de l'aplatissement. Un décor garde
// sa hauteur d'origine, donc sur un sol aplati il flotte ou s'enfonce ; et
// masquer un décor au rendu ne le retire pas du QUADTREE DE PICKING — il est
// toujours là, le clic au sol BUTE dessus, un arbre invisible arrête le curseur.
// Aplatir sans décharger est un état cassé : on ne l'offre pas au joueur.
//
// ⭐ On ne vide aucune liste à la main : `CRsm_Load` est le slot vtable +20 que
// `UITextureMgr_Load` appelle pour charger la ressource. S'il rend autre chose
// que 1, le gestionnaire DÉTRUIT proprement la ressource et rend nullptr, et
// `CWorld_Load` ignore alors ce modèle (`if (v33)`) — chemin d'erreur natif, pas
// de fuite, pas de liste à recoudre. Il ne reste qu'une ligne au journal du
// client par fichier (UITextureMgr_ReportLoadFailureOnce).
constexpr uintptr_t kRsmLoad = 0x0071a720;  // CRsm::Load (retn 4)

// ── Aplatir le terrain ───────────────────────────────────────────────────────
//
// Un GRF « greyworld » livre un `.gnd` déjà plat. On ne peut pas remplacer le
// fichier, mais on peut réécrire les hauteurs À LEUR CHARGEMENT, juste après que
// le client les ait lues et avant qu'il n'en fasse des primitives — c'est ce qui
// oblige à repasser par un changement de carte, et c'est le seul prix à payer.
//
// 🔴 IL FAUT APLATIR LES DEUX, ET AU MÊME NIVEAU :
//   le .gnd  = la géométrie DESSINÉE ;
//   le .gat  = les hauteurs que `Terrain_GetHeightAt` donne aux ACTEURS.
// N'en aplatir qu'un ferait flotter ou enfoncer tout le monde.
//
// Le niveau retenu est la MOYENNE des hauteurs de la carte : poser le sol à zéro
// le décrocherait du niveau d'eau du .rsw, qu'on ne touche pas.
//
// Les deux structures ont des offsets voisins mais n'ont RIEN à voir — les
// confondre lirait des tuiles comme des hauteurs.
constexpr uintptr_t kAttrLoad = 0x00710820;  // C3dAttr::Load  (retn 4)
constexpr uintptr_t kGndLoad  = 0x00716010;  // CGnd::Load     (retn 4)

// C3dGnd : relevé dans CGnd_ParseStream 0x007169e0. Une case fait 28 octets —
// quatre hauteurs de coin puis trois indices de tuile, le format du .gnd.
constexpr int kGndWidth      = 280;  // 0x118
constexpr int kGndHeight     = 284;  // 0x11c
constexpr int kGndCells      = 388;  // 0x184
constexpr int kGndCellStride = 28;
constexpr int kCornersPerCell = 4;

// Le niveau commun aux deux fichiers. Le .gat est chargé AVANT le .gnd
// (CWorld_Load 0x00a6aff0) : c'est donc lui qui fixe le niveau, et le .gnd le
// reprend. Le repli existe pour l'ordre inverse, qu'on n'a pas observé.
bool  g_flat_level_known = false;
float g_flat_level       = 0.0f;


// Le côté d'une case du terrain, en unités de monde — relevé dans le .gat au
// chargement, parce que c'est l'unité dans laquelle la carte se mesure et qu'on
// s'en sert pour dimensionner autre chose que des cases (cf. les boîtes du
// quadtree). Zéro = pas encore lu.
int g_terrain_cell_size = 0;

// 🔴🔴 UN SOL EXACTEMENT PLAT REND DES CASES NON CLIQUABLES — et c'est le .GAT
// qu'il faut corriger, pas le .gnd.
//
// Le clic-sol (`GameMode_PickGroundCellUnderMouse` 0x00c69a40) procède en deux
// temps, et un seul compte ici :
//   1. il collecte les nœuds du quadtree que le rayon traverse. Ce test-là n'est
//      PAS le coupable : `QuadTree_Subdivide` 0x00a69420 initialise les bornes
//      de hauteur d'un nœud à ±999999 — une boîte n'est jamais plate ;
//   2. pour chaque case de ces nœuds, `sub_711640` (une méthode du C3dAttr, donc
//      du .GAT) construit DEUX TRIANGLES sur les quatre hauteurs de la case et
//      teste le rayon contre eux.
//
// Ce sont ces triangles qui posent problème : parfaitement horizontaux, ils sont
// PARALLÈLES à un rayon rasant, et l'intersection n'existe alors pas. Les trois
// symptômes observés le disent, chacun à sa façon : caméra perpendiculaire au
// sol ⇒ tout cliquable ; caméra inclinée ⇒ des bandes inertes ; caméra rasante
// ⇒ plus rien du tout.
//
// D'où `jitter` : des dents de scie d'un dixième d'unité sur des cases de cinq —
// une pente invisible à l'œil, mais qui suffit à ce que les triangles ne soient
// plus coplanaires avec le rayon.
//
// ⚠ Le .gnd, lui, reste EXACTEMENT plat : c'est le sol DESSINÉ, et le jitter n'y
// apporterait rien au clic tout en faisant onduler le terrain.
//
// 🔴🔴 MAIS LE JITTER SE VOIT QUAND MÊME, ET PAS LÀ OÙ ON LE CROYAIT. Ce fichier
// a longtemps affirmé que le quadrillage lisait ses coins dans le .gnd. C'EST
// FAUX, et le désassemblage de `C3dGround15_GetCellCorners` 0x00a62b70 le dit :
// elle calcule X et Z dans la grille de TUILES, puis va chercher ses QUATRE
// hauteurs dans `C3dAttr_GetCell(this+4, …)` — le .GAT, donc ici même.
//
// Conséquence, avec l'ordre des coins que rend cette fonction — (x₀,z₀), (x₁,z₀),
// (x₀,z₁), (x₁,z₁) — et un signe alterné sur `c & 1` : les deux coins de gauche
// montent, les deux de droite descendent. Chaque case devient une plaque
// INCLINÉE, avec une marche à la frontière de la suivante, et sa moitié basse
// passe SOUS le plan du .gnd, qui la mange. Le biais de profondeur du client ne
// rattrape pas ça : il départage deux surfaces coplanaires, pas 0,1 unité de
// monde. À l'écran, des losanges penchés sur un sol pourtant plan.
//
// D'où la règle : le jitter est POUR LE CLIC, et le clic seul le voit. Le
// quadrillage, lui, se dessine au niveau du plan (cf. DrawCellShrunk).
constexpr float kFlatJitter = 0.1f;

// Moyenne des hauteurs, puis écriture de cette moyenne partout. `stride` et
// `count` diffèrent entre les deux fichiers, le reste est identique : quatre
// flottants en tête de chaque case.
void FlattenHeights(char* cells, int count, int stride, float jitter) {
  if (!cells || count <= 0 || !g_flat_level_known) return;
  for (int i = 0; i < count; ++i) {
    float* h = reinterpret_cast<float*>(cells + i * stride);
    // Dents de scie sur les coins : deux hauts, deux bas. Chaque case garde
    // ainsi un min strictement inférieur à son max, donc tout nœud du quadtree
    // aussi — quelle que soit la profondeur de subdivision, jusqu'à la case
    // unique. `jitter` nul rend le plan exact.
    for (int c = 0; c < kCornersPerCell; ++c) {
      h[c] = g_flat_level + ((c & 1) ? -jitter : jitter);
    }
  }
}

// ── Les hauteurs D'ORIGINE du .gat courant ───────────────────────────────────
//
// 🔴🔴 SANS CETTE COPIE, DÉCOCHER NE REND RIEN. Aplatir écrit les hauteurs EN
// PLACE, et le fichier ne sera relu qu'au prochain chargement de carte : tant
// qu'il n'a pas eu lieu, la seule façon de rendre son relief au .gat est de le
// réécrire depuis une copie — encore faut-il l'avoir prise AVANT d'écraser.
//
// Elle ne vaut que pour LE C3dAttr dont l'adresse est notée : un chargement de
// carte en fabrique un autre, et ce qu'on gardait de l'ancien n'a plus cours.
//
// ⚠ Le QUADTREE, lui, n'a pas de copie et n'en a pas besoin. Rouvrir ses bornes
// de hauteur n'ôte RIEN à la justesse du clic — ce sont les triangles du .gat qui
// tranchent, l'arbre ne fait que présélectionner — cela ne coûte que du culling,
// et les nœuds sont vides tant que les décors ne sont pas revenus. Or ils ne
// peuvent revenir que par un chargement de carte, qui reconstruit l'arbre depuis
// le .rsw.
std::vector<float> g_attr_backup;
void*              g_attr_backup_owner = nullptr;

// Prend la copie, une seule fois par C3dAttr. Appelée depuis un `__try` : le
// vecteur vit ici et non chez l'appelant, où il interdirait le SEH (C2712).
void BackupAttrHeights(void* attr, const char* cells, int count, int stride) {
  if (!attr || !cells || count <= 0) return;
  if (g_attr_backup_owner == attr) return;  // déjà prise, et sur le bon objet
  g_attr_backup.resize(static_cast<size_t>(count) * kCornersPerCell);
  for (int i = 0; i < count; ++i) {
    const float* h = reinterpret_cast<const float*>(cells + i * stride);
    for (int c = 0; c < kCornersPerCell; ++c) {
      g_attr_backup[static_cast<size_t>(i) * kCornersPerCell + c] = h[c];
    }
  }
  g_attr_backup_owner = attr;
}

// Réécrit les hauteurs d'origine. Rend faux quand il n'y a rien à rendre — soit
// qu'on n'ait jamais aplati ce terrain-là, soit que la copie soit celle d'un
// autre : dans les deux cas l'appelant n'a rien à faire.
bool RestoreAttrHeights(void* attr, char* cells, int count, int stride) {
  if (!attr || !cells || count <= 0) return false;
  if (g_attr_backup_owner != attr) return false;
  if (g_attr_backup.size() != static_cast<size_t>(count) * kCornersPerCell) {
    return false;  // le terrain a changé de taille sous la copie : on n'y touche pas
  }
  for (int i = 0; i < count; ++i) {
    float* h = reinterpret_cast<float*>(cells + i * stride);
    for (int c = 0; c < kCornersPerCell; ++c) {
      h[c] = g_attr_backup[static_cast<size_t>(i) * kCornersPerCell + c];
    }
  }
  return true;
}

// ── Où le sol EXISTE, tuile par tuile ────────────────────────────────────────
//
// 🔴 LE .GAT DÉCRIT TOUT LE RECTANGLE, Y COMPRIS CE QUI N'EST PAS DU SOL. Une
// carte à trous — gonryun est une île dans le ciel — a des cases de .gat au-dessus
// du VIDE, et le .gnd n'y pose aucune face du dessus : le client n'y dessine rien,
// et c'est normal. Sans aplatissement ces cases gardent leur hauteur d'origine et
// sortent du champ ; APLATIES, elles remontent au niveau du plan et le quadrillage
// se met à flotter dans le ciel, au-delà du bord de l'île.
//
// La face du dessus est le PREMIER des trois indices de surface qui suivent les
// quatre hauteurs — cf. `CGnd_ParseStream` 0x007169e0, qui lit `28 × largeur ×
// hauteur` octets de cellules. Négatif = pas de face, donc pas de sol.
constexpr int kGndCellTileUp = 16;

std::vector<uint8_t> g_gnd_has_surface;  // une case par TUILE du .gnd
int                  g_gnd_tiles_w = 0;
int                  g_gnd_tiles_h = 0;

// Relevé à chaque chargement du .gnd. Le vecteur vit ici et non chez l'appelant,
// où il interdirait le SEH (C2712) — comme la copie des hauteurs.
void NoteGroundSurfaces(int w, int h, const char* cells) {
  g_gnd_has_surface.clear();
  g_gnd_has_surface.shrink_to_fit();
  g_gnd_tiles_w = 0;
  g_gnd_tiles_h = 0;
  if (!cells || w <= 0 || h <= 0) return;

  const size_t count = static_cast<size_t>(w) * h;
  g_gnd_has_surface.assign(count, 0);
  size_t solid = 0;
  for (size_t i = 0; i < count; ++i) {
    const int up = *reinterpret_cast<const int*>(cells + i * kGndCellStride +
                                                 kGndCellTileUp);
    if (up >= 0) {
      g_gnd_has_surface[i] = 1;
      ++solid;
    }
  }
  // 🔴 UN FILTRE QUI EFFACERAIT TOUT NE PROUVERAIT RIEN. Aucune tuile avec face
  // du dessus, c'est l'offset qu'on lit mal, pas une carte sans sol : on renonce
  // à filtrer plutôt que de faire disparaître le quadrillage en silence.
  if (solid == 0) {
    g_gnd_has_surface.clear();
    LogDiag("[GreyWorld] .gnd : aucune face du dessus, filtre du sol désarmé");
    return;
  }
  g_gnd_tiles_w = w;
  g_gnd_tiles_h = h;

  // ── Et le NIVEAU, tant qu'on tient le seul fichier qui sache les deux ─────
  //
  // 🔴🔴 PAS LA MOYENNE DE TOUT. Le .gat comme le .gnd décrivent le rectangle
  // ENTIER de la carte, vide compris, et sur une carte à trous le vide est
  // MAJORITAIRE. Mesuré sur les fichiers : gonryun a 65 % de tuiles sans face du
  // dessus, dont les hauteurs plongent très au-dessous de l'île — la moyenne de
  // toutes les cases donnait -25.29 quand le sol réel est à -58.18. On aplatissait
  // la carte 33 unités trop bas, soit plus de six cases.
  //
  // La moyenne des seules cases QUI ONT DU SOL donne le niveau où l'on marche.
  // Sur une carte pleine, les deux se confondent (prontera : 4 centièmes d'écart).
  double sum = 0.0;
  size_t n = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!g_gnd_has_surface[i]) continue;
    const float* height = reinterpret_cast<const float*>(cells + i * kGndCellStride);
    for (int c = 0; c < kCornersPerCell; ++c) sum += height[c];
    n += kCornersPerCell;
  }
  if (n == 0) return;  // `solid` non nul l'exclut, mais on ne divise pas à l'aveugle
  g_flat_level       = static_cast<float>(sum / n);
  g_flat_level_known = true;
}

// Aplatit le .gat que la copie retient, une fois le niveau connu.
//
// 🔴 LE .GAT EST CHARGÉ AVANT LE .GND, et c'est le .gnd qui sait où est le sol :
// on ne peut donc pas fixer le niveau au chargement du .gat. On y prend seulement
// la copie des hauteurs, et on aplatit ICI, quand le niveau existe. `CWorld_Load`
// charge les deux avant de construire quoi que ce soit — personne ne lit le .gat
// entre les deux.
void FlattenPendingAttr() {
  if (!g_attr_backup_owner || !g_flat_level_known) return;
  __try {
    auto* a = reinterpret_cast<char*>(g_attr_backup_owner);
    const int w = *reinterpret_cast<int*>(a + gamescene::kTerrainWidth);
    const int h = *reinterpret_cast<int*>(a + gamescene::kTerrainHeight);
    auto* cells = *reinterpret_cast<char**>(a + gamescene::kTerrainCells);
    // C'est le .gat qui porte les triangles du clic-sol : c'est LUI qui a besoin
    // du micro-relief, sans quoi un rayon rasant ne les rencontre jamais.
    FlattenHeights(cells, w * h, gamescene::kCellStride, kFlatJitter);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Y a-t-il du sol sous cette case de .gat ? Une tuile de .gnd couvre 2×2 cases,
// et c'est le décalage qu'emploie le client lui-même (`GetCellCorners` indexe sa
// grille de tuiles par `cellX >> 1`). Vrai par défaut : sans relevé, on dessine
// comme avant.
bool GroundHasSurface(int cell_x, int cell_y) {
  if (g_gnd_has_surface.empty()) return true;
  const int tx = cell_x >> 1;
  const int ty = cell_y >> 1;
  if (tx < 0 || ty < 0 || tx >= g_gnd_tiles_w || ty >= g_gnd_tiles_h) return false;
  return g_gnd_has_surface[static_cast<size_t>(ty) * g_gnd_tiles_w + tx] != 0;
}

// Le chargement d'une carte périme la copie : le C3dAttr d'avant n'existe plus,
// et le nouveau terrain arrive avec son propre relief, tout frais du fichier.
void DropAttrBackup() {
  g_attr_backup.clear();
  g_attr_backup.shrink_to_fit();  // un .gat fait quelques mégaoctets
  g_attr_backup_owner = nullptr;
}

// Le terrain EN MÉMOIRE est-il le nôtre, et à quel niveau ? C'est la copie qui
// répond : elle n'existe que si on a aplati CE .gat-là, et le chargement suivant
// la périme.
//
// 🔴 PAS `FlattenActive()`, qui dit ce que le RÉGLAGE demande. Entre la case
// cochée et la carte rechargée il s'écoule plusieurs secondes pendant lesquelles
// le terrain est encore l'ancien, avec son relief : y poser le quadrillage sur un
// plan que le sol n'a pas encore le ferait flotter ou s'enfoncer, et le niveau
// retenu serait celui de la carte d'avant.
bool TerrainIsFlattened() {
  return g_attr_backup_owner != nullptr && g_flat_level_known;
}

using LoadResFn = int(__fastcall*)(void*, void*, void*);
LoadResFn g_orig_attr_load = nullptr;
LoadResFn g_orig_gnd_load  = nullptr;
LoadResFn g_orig_rsm_load  = nullptr;

// Rendre 0 (donc « pas 1 ») suffit : c'est le gestionnaire qui fait le ménage.
// La condition est `flatten`, pas un réglage à part — cf. le bloc ci-dessus.
int __fastcall Hooked_RsmLoad(void* self, void* edx, void* path) {
  if (Enabled() && g_cfg.flatten) return 0;
  return g_orig_rsm_load ? g_orig_rsm_load(self, edx, path) : 0;
}

// Défini avec la machine d'état, plus bas : charger un .gat EST le signe qu'une
// carte neuve vient d'arriver, et c'est ici qu'on le constate.
void NoteMapLoaded();

int __fastcall Hooked_AttrLoad(void* self, void* edx, void* path) {
  const int ok = g_orig_attr_load ? g_orig_attr_load(self, edx, path) : 0;
  if (!ok || !self) return ok;

  // 🔴 AVANT toute condition de réglage. Ce hook est traversé à CHAQUE
  // chargement de carte, leviers cochés ou non — c'est précisément ce qui en
  // fait un témoin sur lequel on peut compter.
  DropAttrBackup();
  // Le niveau appartient à la carte qui arrive. Garder celui d'avant ne
  // servirait qu'à égarer le .gnd, qui s'en sert de repli (cf. FlattenHeights).
  g_flat_level_known = false;
  NoteMapLoaded();

  if (!Enabled() || !g_cfg.flatten) return ok;
  __try {
    auto* a = reinterpret_cast<char*>(self);
    const int w = *reinterpret_cast<int*>(a + gamescene::kTerrainWidth);
    const int h = *reinterpret_cast<int*>(a + gamescene::kTerrainHeight);
    auto* cells = *reinterpret_cast<char**>(a + gamescene::kTerrainCells);
    g_terrain_cell_size = *reinterpret_cast<int*>(a + gamescene::kTerrainCellSize);
    // La copie, et rien d'autre : le niveau ne sera connu qu'au chargement du
    // .gnd, juste après (cf. FlattenPendingAttr).
    BackupAttrHeights(self, cells, w * h, gamescene::kCellStride);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return ok;
}

int __fastcall Hooked_GndLoad(void* self, void* edx, void* path) {
  const int ok = g_orig_gnd_load ? g_orig_gnd_load(self, edx, path) : 0;
  if (!ok || !self) return ok;
  __try {
    auto* g = reinterpret_cast<char*>(self);
    const int w = *reinterpret_cast<int*>(g + kGndWidth);
    const int h = *reinterpret_cast<int*>(g + kGndHeight);
    auto* cells = *reinterpret_cast<char**>(g + kGndCells);
    // 🔴 SANS CONDITION DE RÉGLAGE : où est le sol ne dépend pas de nos leviers,
    // et c'est ICI qu'on peut le savoir — le gestionnaire détruit le .gnd sitôt
    // le terrain construit.
    NoteGroundSurfaces(w, h, cells);
    if (!Enabled() || !g_cfg.flatten) return ok;
    // Le .gnd n'est que du rendu : plan EXACT, pour que les carreaux du
    // quadrillage ne se mettent pas à onduler.
    FlattenHeights(cells, w * h, kGndCellStride, /*jitter=*/0.0f);
    // Et le .gat, qui attendait ce niveau depuis son propre chargement.
    FlattenPendingAttr();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return ok;
}

// ── Notre propre dessin de case ──────────────────────────────────────────────
//
// Calqué sur C3dGround15_DrawCellQuad, à deux choses près : après avoir réduit
// les coins à la case GAT (la parité de x et de y choisit le quart de tuile GND —
// c'est le calcul exact du client), on rapproche encore les quatre coins de leur
// centre — le sol reste visible tout autour, c'est le JOINT, et c'est lui qui
// fait un carrelage plutôt qu'une nappe ; et `flat` pose les quatre hauteurs sur
// le plan au lieu de les prendre au terrain.
//
// `k` = 1 et `flat` faux ne laissent rien qui nous soit propre : on appelle alors
// la fonction du CLIENT, qui est éprouvée.
void DrawCellShrunk(void* ground, void* scene, int cell_x, int cell_y,
                    uint32_t argb, void* res, float k, const float uv[8],
                    bool flat) {
  float c[12];
  auto get_corners =
      reinterpret_cast<int(__fastcall*)(void*, void*, int, int, float*)>(
          kGetCellCorners);
  if (!get_corners(ground, nullptr, cell_x, cell_y, c)) return;  // hors carte

  // 🔴 LE JITTER DU .GAT N'EST PAS POUR NOUS. Les quatre hauteurs qu'on vient de
  // recevoir sortent du .gat (cf. kFlatJitter) : sur un terrain aplati elles
  // portent les dents de scie du clic-sol, et le carreau se retrouve incliné,
  // à moitié sous le plan du .gnd. Le sol dessiné est à `g_flat_level` — le
  // carreau s'y pose, et le biais de profondeur fait le reste.
  if (flat) {
    for (int i = 0; i < kCornersPerCell; ++i) c[i * 3 + 1] = g_flat_level;
  }

  // Les coins 0 et 1 bornent l'axe X, les coins 0 et 2 l'axe Z. Le client lit
  // ces bornes AVANT de modifier quoi que ce soit ; garder des copies donne le
  // même résultat, et dit mieux ce qui se passe.
  const float x_min = c[0], x_max = c[3];
  const float z_min = c[2], z_max = c[8];
  for (int i = 0; i < 4; ++i) {
    float& px = c[i * 3];
    float& pz = c[i * 3 + 2];
    if (cell_x & 1) { if (x_max > px) px += (x_max - px) * 0.5f; }
    else            { if (px > x_min) px -= (px - x_min) * 0.5f; }
    if (cell_y & 1) { if (z_max > pz) pz += (z_max - pz) * 0.5f; }
    else            { if (pz > z_min) pz -= (pz - z_min) * 0.5f; }
  }

  // Le joint. La hauteur n'est PAS touchée : le carreau doit continuer d'épouser
  // le relief, sinon il s'enfoncerait dans une pente ou flotterait au-dessus.
  const float cx = (c[0] + c[3] + c[6] + c[9]) * 0.25f;
  const float cz = (c[2] + c[5] + c[8] + c[11]) * 0.25f;
  for (int i = 0; i < 4; ++i) {
    c[i * 3]     = cx + (c[i * 3] - cx) * k;
    c[i * 3 + 2] = cz + (c[i * 3 + 2] - cz) * k;
  }

  void* queue = render::Context();
  if (!queue) return;
  auto acquire = reinterpret_cast<uintptr_t*(__fastcall*)(void*)>(kAcquirePrimRec);
  uintptr_t* rec = acquire(queue);
  if (!rec || !rec[0]) return;

  auto project =
      reinterpret_cast<void(__fastcall*)(void*, void*, const float*, void*, float*)>(
          kProjectToScreen);
  char* verts = reinterpret_cast<char*>(rec[0]);

  // Ratio « surface utile / taille de texture » du sprite, exactement comme le
  // client : les deux ne coïncident pas, une texture étant montée à la puissance
  // de deux supérieure.
  const auto* r = reinterpret_cast<const unsigned*>(res);
  const float su = static_cast<float>(r[5]) / static_cast<float>(r[3]);
  const float sv = static_cast<float>(r[6]) / static_cast<float>(r[4]);

  for (int i = 0; i < 4; ++i) {
    char* v = verts + i * kVertexStride;
    project(queue, nullptr, &c[i * 3], scene, reinterpret_cast<float*>(v));
    *reinterpret_cast<float*>(v + kVertexZ) -= kDepthBias;
    *reinterpret_cast<uint32_t*>(v + kVertexColor) = argb;
    *reinterpret_cast<float*>(v + kVertexU)     = su * uv[i * 2];
    *reinterpret_cast<float*>(v + kVertexU + 4) = sv * uv[i * 2 + 1];
  }

  rec[2] = reinterpret_cast<uintptr_t>(res);
  rec[6] = 5;  // blend source      = SRCALPHA
  rec[7] = 6;  // blend destination = INVSRCALPHA
  auto insert =
      reinterpret_cast<void(__fastcall*)(void*, void*, void*, unsigned)>(
          kInsertPrimitive);
  insert(queue, nullptr, rec, 1u);
}

// ── Ne soumettre que ce qui peut SE VOIR ─────────────────────────────────────
//
// 🔴 Le parcours est un CARRÉ de cases autour du joueur, mais la caméra le
// regarde de biais : ce qu'on en voit est un trapèze, et le reste — derrière la
// caméra, ou au-delà des bords — est du travail jeté. RIEN ne le rattrape en
// aval, c'est ce qui rend le test nécessaire ici : `RenderQueue_InsertPrimitive`
// 0x00550b10 ne teste aucune visibilité (vérifié dans l'IDB), il range le record
// dans son seau, et le quad va jusqu'au GPU — qui le découpe une fois que le CPU
// a payé la projection, une place dans le tri global des primitives transparentes
// et un appel de dessin.
//
// La part perdue CROÎT avec la portée : le trapèze est borné par l'écran, le
// carré grandit en r². À 24 cases c'est 2401 quads, à 60 c'en est 14 641.
//
// 🔴🔴 LE SIGNE DE RHW D'ABORD, ET IL N'EST PAS FACULTATIF.
// `World_ProjectPointToScreen` 0x00554380 ne clippe rien et ne signale rien :
// elle calcule `rhw = 1/w` et divise, sans jamais regarder le signe de `w`. Un
// point DERRIÈRE la caméra a `w < 0`, donc des coordonnées d'écran MIROIR —
// parfaitement plausibles, souvent en plein milieu de l'écran. Un test qui ne
// regarderait que x et y garderait donc exactement les cases qu'il faut jeter.
// `rhw` est le seul témoin de « devant », et il se lit avant tout le reste.
constexpr int kProjRhw = 3;  // la projection écrit x, y, z, rhw

// L'écran tel que LA PROJECTION le voit, et seulement s'il est sûr.
//
// ⭐ DEUX LECTURES QUI DOIVENT S'ACCORDER. La taille (+0x28/+0x2c) et le centre
// que la projection ajoute à chaque point (+0x30/+0x34) décrivent le même écran :
// si le double du centre ne redonne pas la taille, l'un des deux offsets n'est
// pas ce qu'on croit, et on ne jette RIEN plutôt que de vider le quadrillage sur
// une lecture fausse. Un pixel de tolérance : une largeur impaire tronque.
struct ScreenBox {
  float w     = 0.0f;
  float h     = 0.0f;
  bool  known = false;
};

ScreenBox ScreenBounds() {
  ScreenBox box;
  void* ctx = render::Context();
  if (!ctx) return box;
  const int vw = render::ViewportWidth();
  const int vh = render::ViewportHeight();
  if (vw <= 0 || vh <= 0) return box;
  auto* c = reinterpret_cast<uint8_t*>(ctx);
  const int cx = *reinterpret_cast<int*>(c + render::kOffScreenCenterX);
  const int cy = *reinterpret_cast<int*>(c + render::kOffScreenCenterY);
  const int dx = vw - 2 * cx;
  const int dy = vh - 2 * cy;
  if (dx < 0 || dx > 1 || dy < 0 || dy > 1) return box;
  box.w     = static_cast<float>(vw);
  box.h     = static_cast<float>(vh);
  box.known = true;
  return box;
}

// Faut-il soumettre cette case ? Conservateur par construction : au moindre
// doute on rend `true`, et c'est le GPU qui tranche comme avant.
bool CellOnScreen(void* ground, void* scene, void* queue, const ScreenBox& box,
                  int cell_x, int cell_y) {
  float c[12];
  auto get_corners =
      reinterpret_cast<int(__fastcall*)(void*, void*, int, int, float*)>(
          kGetCellCorners);
  if (!get_corners(ground, nullptr, cell_x, cell_y, c)) return false;  // hors carte

  auto project =
      reinterpret_cast<void(__fastcall*)(void*, void*, const float*, void*, float*)>(
          kProjectToScreen);
  float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
  int in_front = 0;
  for (int i = 0; i < kCornersPerCell; ++i) {
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    project(queue, nullptr, &c[i * 3], scene, v);
    if (v[kProjRhw] <= 0.0f) continue;  // derrière : ses x,y ne veulent rien dire
    if (in_front == 0) {
      min_x = max_x = v[0];
      min_y = max_y = v[1];
    } else {
      if (v[0] < min_x) min_x = v[0];
      if (v[0] > max_x) max_x = v[0];
      if (v[1] < min_y) min_y = v[1];
      if (v[1] > max_y) max_y = v[1];
    }
    ++in_front;
  }
  // Pas un seul coin devant la caméra : la case est derrière nous.
  if (in_front == 0) return false;
  // À CHEVAL sur le plan proche. La boîte des seuls coins de devant ne décrit
  // pas la case, et une case coupée par le plan proche est de toute façon sous
  // le nez du joueur : on la garde sans discuter.
  if (in_front < kCornersPerCell) return true;
  // Boîte entièrement d'un côté de l'écran : rien à en voir.
  return !(max_x < 0.0f || min_x > box.w || max_y < 0.0f || min_y > box.h);
}

// ── Le quadrillage ───────────────────────────────────────────────────────────
//
// Un quad par cellule, dessiné par la fonction du client. Elle épouse le relief
// (elle demande les quatre coins 3D de la tuile), applique le même biais de
// profondeur que le curseur de destination — ce qui la fait passer devant le sol
// sans z-fighting — et insère dans la file de scène, donc le tri avec les
// acteurs reste celui du jeu.
// Définie plus bas, mais lue par le contexte de passe juste en dessous.
bool FlattenActive();

// ── Les peintres invités ─────────────────────────────────────────────
// Voir le contrat dans le header. Le contexte de dessin ne vaut que le temps de
// la passe et il est le MÊME pour tous : on le pose dans ces variables pour la
// durée de l'appel, plutôt que de le faire traverser la signature de chaque
// peintre — qui aurait alors à le comprendre.
struct Invited {
  ScenePainter   paint;
  PainterStyleFn style;
};
std::vector<Invited> g_painters;

void* g_paint_ground = nullptr;
void* g_paint_scene  = nullptr;
void* g_paint_res    = nullptr;
bool  g_paint_flat   = false;
float g_paint_shrink = 1.0f;
const float* g_paint_uv = nullptr;

void PaintInvitedCell(int cell_x, int cell_y, uint32_t argb) {
  // Alpha nul = rien à soumettre. Le test vit ici et non chez l'appelant, pour
  // qu'« invisible » coûte le même prix quel que soit le peintre.
  if (!g_paint_ground || !g_paint_res || (argb >> 24) == 0) return;
  DrawCellShrunk(g_paint_ground, g_paint_scene, cell_x, cell_y, argb,
                 g_paint_res, g_paint_shrink, g_paint_uv, g_paint_flat);
}

void RunScenePainters(void* self) {
  if (g_painters.empty() || !self) return;
  __try {
    void* gm = rag::ActiveModeIfReady();
    if (!gm) return;
    void* world = *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) +
                                            gamescene::kGmActorMgr);
    if (!world) return;
    void* ground = *reinterpret_cast<void**>(reinterpret_cast<char*>(world) +
                                             gamescene::kAmGround);
    if (!ground) return;

    auto get_res = reinterpret_cast<SpriteResFn>(kSpriteResGetOrLoad);
    g_paint_ground = ground;
    g_paint_scene  = reinterpret_cast<char*>(self) + kSceneCtxOffset;
    // Sur terrain aplati, les hauteurs justes sont celles du .gnd : c'est la
    // même règle que pour le quadrillage, pour la même raison.
    g_paint_flat   = FlattenActive();

    // Un seul point d'échantillonnage, réécrit par peintre. Statique et sur le
    // fil de rendu : le client ne garde le pointeur que le temps de l'appel.
    static float s_point_uv[8];

    // Index et non itérateur : MSVC refuse un `__try` dans une fonction qui a
    // des objets à dérouler (C2712).
    for (size_t i = 0; i < g_painters.size(); ++i) {
      if (!g_painters[i].paint || !g_painters[i].style) continue;
      const PainterStyle st = g_painters[i].style();
      if (!st.texture) continue;  // ce peintre n'a rien à peindre : coût nul

      // 🔴 UNE résolution par peintre et par passe, jamais par case : le natif
      // fait un addref à chaque appel, et une par case ferait déborder son
      // compteur en une soirée de jeu.
      void* res = get_res(reinterpret_cast<void*>(render::kSpriteRefCacheAddr),
                          nullptr, st.texture, 0, 0, 1, 0);
      if (!res) continue;

      if (st.u >= 0.0f) {
        for (int c = 0; c < 4; ++c) {
          s_point_uv[c * 2]     = st.u;
          s_point_uv[c * 2 + 1] = st.v;
        }
        g_paint_uv = s_point_uv;
      } else {
        g_paint_uv = reinterpret_cast<const float*>(kCellQuadDefaultUV);
      }
      g_paint_res    = res;
      g_paint_shrink = st.shrink;
      g_painters[i].paint(&PaintInvitedCell);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  // Hors du `__try` : le contexte doit être rendu invalide MÊME si un peintre a
  // faussé compagnie, sinon le sink de la passe suivante lirait un sol mort.
  g_paint_ground = nullptr;
  g_paint_scene  = nullptr;
  g_paint_res    = nullptr;
}

void DrawCellGrid(void* self) {
  __try {
    void* gm = rag::ActiveModeIfReady();
    if (!gm) return;
    void* world = *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) +
                                            gamescene::kGmActorMgr);
    if (!world) return;

    auto* w = reinterpret_cast<char*>(world);
    void* ground = *reinterpret_cast<void**>(w + gamescene::kAmGround);
    void* attr   = *reinterpret_cast<void**>(w + gamescene::kAmTerrain);
    void* own    = *reinterpret_cast<void**>(w + gamescene::kAmOwnPlayer);
    if (!ground || !attr || !own) return;

    auto* a = reinterpret_cast<char*>(attr);
    const int width     = *reinterpret_cast<int*>(a + gamescene::kTerrainWidth);
    const int height    = *reinterpret_cast<int*>(a + gamescene::kTerrainHeight);
    const int cell_size = *reinterpret_cast<int*>(a + gamescene::kTerrainCellSize);
    auto* cells = *reinterpret_cast<char**>(a + gamescene::kTerrainCells);
    if (width <= 0 || height <= 0 || cell_size <= 0 || !cells) return;

    // Cellule du joueur. C'est l'inverse exact de World_TileToPosition, la
    // formule que gamescene:: documente : monde = (case - taille/2) * côté.
    const float* pos = reinterpret_cast<const float*>(
        reinterpret_cast<char*>(own) + kActorPosVec);
    const int px = static_cast<int>(pos[0] / cell_size + width * 0.5f);
    const int py = static_cast<int>(pos[2] / cell_size + height * 0.5f);

    // UNE résolution de la texture par frame, pas par cellule : la fonction du
    // client fait un addref à chaque appel, et 2400 addrefs par frame feraient
    // déborder le compteur en une soirée de jeu. Le natif en fait un par frame
    // pour son curseur — on s'aligne exactement sur lui.
    const int pat = (g_cfg.pattern >= 0 && g_cfg.pattern < Config::kPatternCount)
                        ? g_cfg.pattern
                        : Config::kPatternCross;
    const CellStyle& style = kCellStyles[pat];
    auto get_res = reinterpret_cast<SpriteResFn>(kSpriteResGetOrLoad);
    void* grid_res =
        get_res(reinterpret_cast<void*>(render::kSpriteRefCacheAddr), nullptr,
                style.texture, 0, 0, 1, 0);
    if (!grid_res) return;

    auto draw = reinterpret_cast<DrawCellFn>(kDrawCellQuad);
    void* scene = reinterpret_cast<char*>(self) + kSceneCtxOffset;

    // L'écran, relevé UNE fois pour la passe : c'est ce qui permet de ne pas
    // soumettre les cases qu'on ne verra pas (cf. CellOnScreen). `known` faux =
    // les deux lectures ne s'accordent pas, on ne jette alors rien et le
    // quadrillage est exactement celui d'avant.
    const ScreenBox box = ScreenBounds();
    void* queue = render::Context();
    if (!queue) return;  // sans file de scène, il n'y a rien à dessiner du tout
    if (!box.known) {
      // Le nominal se tait ; ceci ne sort que si un offset a bougé sous nous.
      static bool warned = false;
      if (!warned) {
        warned = true;
        LogDiag("[GreyWorld] écran non reconnu : quadrillage soumis en entier");
      }
    }

    // Les quatre coins sur le MÊME point de texture = un seul texel étiré sur
    // toute la case (cf. kCellStyles). Statique et réécrit à chaque frame : on
    // est sur le fil de rendu, seul, et le client garde le pointeur le temps de
    // l'appel seulement.
    static float s_point_uv[8];
    void* uv = reinterpret_cast<void*>(kCellQuadDefaultUV);
    if (style.u >= 0.0f) {
      for (int i = 0; i < 4; ++i) {
        s_point_uv[i * 2]     = style.u;
        s_point_uv[i * 2 + 1] = style.v;
      }
      uv = s_point_uv;
    }

    const uint32_t col_walk  = ro::ArgbFromPicker(g_cfg.col_walk);
    const uint32_t col_block = ro::ArgbFromPicker(g_cfg.col_block);
    const uint32_t col_snipe = ro::ArgbFromPicker(g_cfg.col_snipe);

    const int r  = g_cfg.radius;
    const int x0 = (px - r < 0) ? 0 : px - r;
    const int y0 = (py - r < 0) ? 0 : py - r;
    const int x1 = (px + r >= width)  ? width  - 1 : px + r;
    const int y1 = (py + r >= height) ? height - 1 : py + r;

    const int fill = (g_cfg.fill >= 0 && g_cfg.fill < Config::kFillCount)
                         ? g_cfg.fill
                         : Config::kFillAll;

    // Le joint n'a de sens que sur un carreau plein : creuser un cadre ou une
    // croix ne ferait que les rapetisser, sans rien séparer.
    const int gap = (g_cfg.gap < 0) ? 0 : (g_cfg.gap > 40 ? 40 : g_cfg.gap);
    const float shrink = (pat == Config::kPatternSolid)
                             ? 1.0f - static_cast<float>(gap) * 0.01f
                             : 1.0f;

    // Le terrain est-il le nôtre ? Si oui, les hauteurs du .gat portent le jitter
    // du clic-sol et le carreau doit s'en passer (cf. kFlatJitter).
    const bool flat = TerrainIsFlattened();

    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        // Pas de sol sous cette case : rien à dessiner. Le .gat couvre tout le
        // rectangle de la carte, le .gnd dit où le sol existe — et sur une carte
        // à trous, aplatir ramène ces cases-là dans le champ (cf.
        // GroundHasSurface). Test le plus court, donc en tête.
        if (!GroundHasSurface(x, y)) continue;
        const int fam = CellFamily(cells, width, height, x, y);
        if (fill == Config::kFillOutline &&
            fam == CellFamily(cells, width, height, x - 1, y) &&
            fam == CellFamily(cells, width, height, x + 1, y) &&
            fam == CellFamily(cells, width, height, x, y - 1) &&
            fam == CellFamily(cells, width, height, x, y + 1)) {
          continue;  // entourée de son propre terrain : ce n'est pas un bord
        }

        const uint32_t argb = (fam == 1) ? col_block
                              : (fam == 2) ? col_snipe
                                           : col_walk;
        // Alpha nul = le joueur a éteint cette famille de cellules. Sauter le
        // quad plutôt que d'en soumettre un invisible, c'est autant de
        // primitives en moins dans la file — c'est LE réglage qui décide du coût.
        if ((argb >> 24) == 0) continue;
        // Hors champ : rien à soumettre. APRÈS les deux tests ci-dessus, qui ne
        // coûtent que des lectures, et AVANT tout ce qui coûte vraiment.
        if (box.known && !CellOnScreen(ground, scene, queue, box, x, y)) continue;
        // Sans joint ET sans aplatissement, on laisse faire le client : sa
        // fonction est éprouvée, la nôtre n'existe que pour ce que la sienne ne
        // sait pas faire. 🔴 Aplati, elle ne sait justement pas : elle prendrait
        // ses hauteurs dans le .gat, dents de scie comprises (cf. kFlatJitter).
        if (shrink < 1.0f || flat) {
          DrawCellShrunk(ground, scene, x, y, argb, grid_res, shrink,
                         reinterpret_cast<const float*>(uv), flat);
        } else {
          draw(ground, nullptr, scene, x, y, argb, grid_res, uv);
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ── Les hooks ────────────────────────────────────────────────────────────────

// Décors .rsm : on avale l'appel, enfants compris (la récursion passe par le
// même slot de vtable, donc elle est avalée aussi).
int __fastcall Hooked_RsmRenderNode(void* self, void* edx, void* mtx, int a, int b,
                                    int c) {
  if (Enabled() && g_cfg.hide_models) return 0;
  return g_orig_rsm ? g_orig_rsm(self, edx, mtx, a, b, c) : 0;
}

// ── Ce qui se rejoue À CHAUD, et pourquoi ────────────────────────────────────
//
// Aplatir ne se décide pas qu'au chargement : le joueur coche la case au milieu
// d'une partie, et deux choses savent lui répondre sans attendre — le .gat, qui
// n'est pas pré-transformé et se réécrit en place, et les bornes du quadtree,
// qu'on rouvre. Le reste (la géométrie dessinée, les décors) demande un
// rechargement.
//
// ⚠ CE N'EST PAS UN RATTRAPAGE DE CACHE — la version précédente le croyait.
// `UITextureMgr_Load` 0x00a8d4a0 sert bien depuis son cache quand la ressource y
// est, mais AUCUNE ressource de carte n'y survit à un changement de carte :
// `CGameMode::OnExit` 0x00c73130 appelle `CWorld_Unload`, puis purge par
// extension "rsm", "rsw" et "gat" (`UITextureMgr_PurgeByExtension` 0x00a8f740),
// et `CWorld_Load` 0x00a6aff0 relâche le .gnd sitôt le terrain construit
// (destruction inconditionnelle). Nos chargeurs sont donc traversés à CHAQUE
// carte, y compris au retour sur une carte déjà visitée : ce qu'ils font n'a
// jamais besoin d'être refait après coup.
//
// 🔴 NE PAS se fier à un compteur de chargements pour savoir s'il faut agir.
// Première version : `if (MapLoadEpoch() != g_last_map_epoch)`, avec le témoin à
// 0 — or l'epoch vaut 0 lui aussi tant qu'aucune TRANSITION n'a été comptée, et
// entrer en jeu n'en est pas forcément une. Le rattrapage n'a alors jamais
// tourné, mesuré à x32dbg : .gat aplati (par le chargeur) mais quadtree intact.
//
// L'ÉTAT est un bien meilleur témoin, et il ne peut pas mentir. Il y en a deux,
// un par chose à faire : la borne haute de la racine du quadtree pour l'arbre, la
// copie des hauteurs d'origine pour le .gat. Chacun coûte une comparaison par
// frame et se réarme tout seul au chargement suivant.

// ── Les boîtes du quadtree viennent du .RSW, pas du terrain ──────────────────
//
// 🔴🔴🔴 LA cause des cases inertes, et la plus retorse : `QuadTree_Rebuild`
// 0x00a69610 ne calcule ses boîtes que si le .rsw n'en fournit PAS. Quand il en
// fournit — c'est le cas courant — il appelle `QuadTree_CopyGeometry` 0x00a69b30,
// qui recopie telles quelles les bornes stockées dans le fichier :
//   node[5..7] = min (x,y,z)   node[8..10] = max (x,y,z)
// Ces bornes portent le RELIEF D'ORIGINE. Aplatir le .gnd et le .gat n'y change
// donc rien : le rayon du clic vise un sol plat, à la hauteur moyenne, tandis
// que les boîtes sont restées aux altitudes de la carte d'avant. Le rayon ne
// traverse plus que celles qui croisent encore son chemin — d'où des BANDES
// inertes, et le rôle décisif de l'angle de caméra :
//   vertical  → il traverse toutes les altitudes  → tout répond ;
//   rasant    → il reste à une altitude constante → presque rien ne répond.
//
// On rouvre donc les bornes de HAUTEUR à ±999999 — exactement ce que
// `QuadTree_Subdivide` 0x00a69420 pose quand il n'a pas de géométrie à copier.
// Le rayon traverse alors tous les nœuds et le clic retombe sur le seul test qui
// compte, celui des triangles du .gat, qu'on a bien aplatis.
//
// ⚠ On ne touche QUE Y : X et Z portent le découpage spatial de l'arbre, qui est
// juste. Le culling de rendu y perd un peu, mais les nœuds sont vides — aplatir
// emporte le déchargement des décors.
// 🔴🔴 ON ÉLARGIT, ON NE REMPLACE PLUS. La version précédente écrasait les deux
// bornes par ±999999 — les valeurs que `QuadTree_Subdivide` pose sur un nœud SANS
// géométrie. Sur un nœud qui en a, ça fabrique une boîte de deux millions
// d'unités de haut, et cet arbre ne sert pas qu'aux décors : `QuadTree_Rebuild`
// 0x00a69610 calcule ses extents DEPUIS LE SOL (`world+40`, le C3dGround15, dont
// il lit largeur, hauteur et taille de tuile) et propage par nœud. Des boîtes
// absurdes s'y payent en tuiles de terrain écartées au loin — une bande dont la
// forme suit le découpage de l'arbre, donc le MONDE, et qui devient diagonale
// quand la caméra tourne.
//
// La boîte doit CONTENIR le plan aplati ; elle n'a pas besoin d'être infinie. On
// garde donc les bornes du .rsw et on y ajoute le plan.
// ⚠ ZÉRO NE CONVIENDRAIT PAS, et pas pour la raison qu'on croit : la boîte ne
// pouvant que grandir, une marge nulle la laisserait le plus souvent intacte. Le
// cas qui compte est celui où le plan SORT des bornes d'origine — et la boîte
// s'arrêterait alors EXACTEMENT sur lui. Le rayon du clic frôlerait la frontière
// au lieu de la traverser : la même dégénérescence que des triangles horizontaux
// parallèles à un rayon rasant, déplacée de la case vers le nœud.
//
// La marge se lit donc dans la carte plutôt que de s'inventer : UNE CASE de
// terrain, l'unité de ce que la boîte borne. Le repli couvre le cas où le .gat
// n'a pas encore été lu — une case de cinq unités est la valeur de tous les
// terrains du client, gonryun compris (zoom .gnd = 10, donc 5 par case .gat).
constexpr float kFallbackCellSize = 5.0f;

// ── ⚠ DÉFAUT CONNU, NON RÉSOLU : la bande de sol manquante ───────────────────
//
// Sur une carte à trous (gonryun), aplatir laisse une bande où le CLIENT ne
// soumet pas ses tuiles de terrain — on voit le ciel à travers. Le sol existe :
// il est dessiné sans aplatissement, au même endroit.
//
// Ce qui a été ÉLIMINÉ, par la mesure et non par le raisonnement (2026-09-02) —
// ne pas refaire ces essais :
//   · le BORD de carte et les planchers de DÉCOR : le sol est bien là ;
//   · le PLAN LOINTAIN (1500, en dur dans World_SetupFogAndProjection
//     0x00551370) : l'horizon ne bouge pas d'un pixel entre zoom mini et maxi ;
//   · ce QUADTREE : la bande persiste alors qu'on n'écrit RIEN dans ses bornes ;
//   · la CAMÉRA : elle suit le plan au centième près. Lu en jeu à deux niveaux,
//     `scene+32` (cible) et `scene+128` (œil) — écart identique (546,73 en Y,
//     458,76 en Z, distance 713,7). Tout se translate ensemble ;
//   · l'EAU du .rsw : c'est du ciel qu'on voit, pas de l'eau ;
//   · une INVERSION DE SIGNE : la racine va de −285,6 à +34, même espace et même
//     sens que les hauteurs du .gnd.
//
// Il reste donc une contradiction ouverte : tout ce qui compose la scène suit le
// plan, l'image devrait être invariante par translation verticale — et elle ne
// l'est pas. Un décalage d'une dizaine d'unités vers le HAUT la rend presque
// invisible sur gonryun, sans qu'on sache pourquoi ; ce n'est donc pas figé ici,
// une valeur qui corrige sans qu'on sache ce qu'elle corrige ne tient pas.
//
// ➡ La suite : traverser `Camera_ComputeFrustumCorners` 0x00a79f90 au pas à pas
// et comparer les nœuds RETENUS à deux niveaux. Le pipeline est cartographié —
// CScene_RenderCellsAndCursor 0x00a7b0a0 sélectionne les nœuds, appelle le slot
// vtable+0x18 du sol avec le rectangle de tuiles du nœud (`node[22..25]`,
// mesuré : 6×6 tuiles) et le drapeau de tri à 1 ; `sub_A63680` 0x00a63680
// parcourt le rectangle ; `sub_A63E10` 0x00a63e10 projette les quatre coins et
// jette la tuile si sa boîte écran rate le viewport (queue+40/+44) ou si un seul
// des quatre `rhw` est négatif.

float FlatBoxMargin() {
  return g_terrain_cell_size > 0 ? static_cast<float>(g_terrain_cell_size)
                                 : kFallbackCellSize;
}

constexpr int   kQuadTreeMaxDepth = 5;   // le client s'arrête à 5 (1365 nœuds)
constexpr int   kNodeMinY = 6;           // node[5..7] = min(x,y,z)
constexpr int   kNodeMaxY = 9;           // node[8..10] = max(x,y,z)

void OpenNodeHeights(int* node, int depth) {
  if (!node) return;
  auto* box = reinterpret_cast<float*>(node);
  const float margin = FlatBoxMargin();
  // 🔴🔴 ON REMPLACE LES DEUX BORNES, on ne les élargit pas. Version précédente :
  // n'agrandir que si la boîte ne contenait pas déjà le plan — et un nœud dont
  // les bornes du .rsw l'englobaient restait donc décrit par le relief d'AVANT,
  // haut de cent unités, alors que son sol tient désormais dans une case.
  //
  // Ça se voyait, et c'est ce qui a mis sur la piste : les boîtes sont FIXES dans
  // le monde, la caméra SUIT le plan. Déplacer le plan de dix unités déplaçait
  // donc la caméra par rapport à des boîtes périmées, et faisait apparaître ou
  // disparaître des tuiles de terrain au loin — un réglage qui « marche » sans
  // qu'on sache pourquoi est un symptôme, pas une correction.
  //
  // Une fois le monde aplati, un nœud ne contient plus QUE du terrain — les
  // décors sont déchargés — et ce terrain est un plan. Sa boîte, c'est ce plan,
  // à une case près. Elle vaut alors pour n'importe quel niveau, et le décalage
  // d'essai n'a plus rien à corriger.
  box[kNodeMinY] = g_flat_level - margin;
  box[kNodeMaxY] = g_flat_level + margin;
  if (depth >= kQuadTreeMaxDepth) return;
  for (int i = 1; i <= 4; ++i) {
    OpenNodeHeights(reinterpret_cast<int*>(node[i]), depth + 1);
  }
}

// Posé par les cases à cocher et par la restauration à chaud, consommé au début
// d'une passe de scène : on ne reconstruit pas des tampons de rendu depuis le
// dessin d'une fenêtre ImGui.
bool g_rebuild_requested = false;

void ReflattenLiveAttr() {
  __try {
    void* gm = rag::ActiveModeIfReady();
    if (!gm) return;
    void* world = *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) +
                                            gamescene::kGmActorMgr);
    if (!world) return;
    auto* w = reinterpret_cast<char*>(world);

    // 🔴 LE QUADTREE D'ABORD, et sans rien exiger d'autre. Version précédente :
    // il venait en dernier, après quatre sorties anticipées — dont une qui
    // déréférençait `C3dGround15 + 8` en croyant y trouver le .gnd. Ce champ vaut
    // en réalité la LARGEUR du .gnd en tuiles (150 sur une carte de 300, mesuré à
    // x32dbg) : la lecture partait dans le vide, le `__except` l'avalait sans un
    // mot, et le correctif du picking ne s'exécutait jamais.
    // ⚠ Un `__try` qui protège plusieurs étapes cache laquelle a échoué : mettre
    // en tête ce qui doit aboutir coûte que coûte.
    //
    // Son témoin : tant que la racine porte EXACTEMENT notre borne, l'arbre
    // décrit déjà le plan. Elle ne peut valoir autre chose que par un chargement
    // de carte, qui recopie les bornes du .rsw — ou par un déplacement du plan,
    // qu'il faut justement reporter sur tout l'arbre.
    int* root = reinterpret_cast<int*>(w + gamescene::kWorldQuadTree);
    if (reinterpret_cast<float*>(root)[kNodeMaxY] != g_flat_level + FlatBoxMargin()) {
      OpenNodeHeights(root, 0);
    }

    void* attr = *reinterpret_cast<void**>(w + gamescene::kAmTerrain);
    if (!attr) return;
    // Le témoin du .gat : la copie de ses hauteurs d'origine. Tant qu'elle est à
    // lui, c'est nous qui l'avons écrit — rien à refaire. 🔴 Et il faut bien un
    // témoin PROPRE au .gat : celui du quadtree reste vrai après une restauration
    // à chaud, où l'arbre demeure ouvert alors que le relief, lui, est revenu.
    if (g_attr_backup_owner == attr) return;
    auto* a = reinterpret_cast<char*>(attr);
    const int aw = *reinterpret_cast<int*>(a + gamescene::kTerrainWidth);
    const int ah = *reinterpret_cast<int*>(a + gamescene::kTerrainHeight);
    auto* cells = *reinterpret_cast<char**>(a + gamescene::kTerrainCells);
    // La copie D'ABORD : après, le relief d'origine n'existe plus nulle part.
    BackupAttrHeights(attr, cells, aw * ah, gamescene::kCellStride);
    // Le niveau vient du .gnd de CETTE carte (cf. NoteGroundSurfaces) : il n'y a
    // rien à recalculer ici, et surtout pas sur le .gat, qui ne sait pas où est
    // le sol.
    FlattenHeights(cells, aw * ah, gamescene::kCellStride, kFlatJitter);
    // La géométrie DESSINÉE, elle, ne se réécrit pas en place : il faut la
    // reconstruire depuis un .gnd rechargé.
    g_rebuild_requested = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ── Rendre le relief SANS changer de carte ───────────────────────────────────
//
// Le pendant exact du précédent, et il ne s'appuie que sur la copie : pas de
// copie, rien à rendre — on n'a rien écrasé. C'est ce qui le rend sûr à appeler
// à chaque frame, y compris quand GreyWorld n'a jamais servi.
//
// ⚠ Les décors, eux, ne reviennent PAS ici : ils sont refusés au chargement, et
// seul un chargement peut les rendre. Le panneau le dit tant que la carte
// affichée ne correspond pas au réglage.
void RestoreLiveAttr() {
  if (g_attr_backup_owner == nullptr) return;
  __try {
    void* gm = rag::ActiveModeIfReady();
    if (!gm) return;
    void* world = *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) +
                                            gamescene::kGmActorMgr);
    if (!world) return;
    auto* w = reinterpret_cast<char*>(world);
    void* attr = *reinterpret_cast<void**>(w + gamescene::kAmTerrain);
    if (!attr || attr != g_attr_backup_owner) return;
    auto* a = reinterpret_cast<char*>(attr);
    const int aw = *reinterpret_cast<int*>(a + gamescene::kTerrainWidth);
    const int ah = *reinterpret_cast<int*>(a + gamescene::kTerrainHeight);
    auto* cells = *reinterpret_cast<char**>(a + gamescene::kTerrainCells);
    if (!RestoreAttrHeights(attr, cells, aw * ah, gamescene::kCellStride)) return;
    // 🔴 La copie EST le témoin : la lâcher est ce qui autorise un nouvel
    // aplatissement à chaud si le joueur recoche.
    DropAttrBackup();
    // Le .gnd est encore plat : sans reconstruction, le sol dessiné resterait
    // une nappe sous des acteurs qui, eux, ont retrouvé leurs altitudes.
    g_rebuild_requested = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ── Reconstruire le terrain SANS changer de carte ────────────────────────────
//
// Le .gat et les boîtes du quadtree se réécrivent à chaud, mais la GÉOMÉTRIE du
// terrain est construite une fois pour toutes au chargement : cocher « aplatir »
// en cours de partie ne se voyait donc qu'au warp suivant.
//
// 🔴 Faire semblant de recevoir un warp NE MARCHE PAS : le client rechargerait
// bien le terrain, mais il vide sa liste d'acteurs et renvoie un `loadendack`
// que le serveur IGNORE (`clif_parse_LoadEndAck` sort d'entrée sur
// `sd->prev != nullptr`, le joueur n'ayant jamais quitté la carte de son point de
// vue). On se retrouverait avec un décor juste et une scène vide.
//
// On refait donc exactement ce que `CWorld_Load` 0x00a6aff0 fait à cet endroit,
// et rien d'autre : recharger le .gnd, appeler `Build` puis la seconde passe,
// relâcher. Aucun paquet, aucune bascule de mode, aucun acteur perdu.
// ⭐ `Build` commence par un `Resize(w,h)` (vtbl+4) qui réalloue ses tableaux :
// c'est ce qui rend l'appel rejouable sur un objet déjà construit.
// Obtenir le gestionnaire et lui demander une ressource : `ro::texmgr::`, le
// foyer du projet (ui/game_texture.h). Rien à redéclarer ici.
//
// 🔴 SAUF CELLE-CI, ET CE N'EST PAS UN OUBLI. `UITextureMgr_Release` 0x00a8f4b0
// est le gestionnaire qui DÉTRUIT une ressource — c'est ce que `CWorld_Load`
// appelle sur le .gnd sitôt le terrain construit, et donc ce qu'on rejoue. Elle
// n'a rien à voir avec `ro::texmgr::kReleaseAddr` 0x00a8f910, qui est le
// COMPTEUR DE RÉFÉRENCES de la ressource elle-même. Deux adresses, deux
// fonctions, des noms voisins : les confondre laisserait le terrain vivant sous
// un compteur décrémenté, ou détruirait ce qu'un autre tient encore.
constexpr uintptr_t kTexMgrRelease = 0x00a8f4b0;  // __thiscall(mgr, res)  retn 4

// Les trois triplets de lumière du monde que CWorld_Load passe à Build.
constexpr uintptr_t kWorldLightA = 0x0159b1a4;
constexpr uintptr_t kWorldLightB = 0x0159b1b0;
constexpr uintptr_t kWorldLightC = 0x0159b1bc;

// Slots de la vtable du C3dGround15, en OCTETS (cf. CWorld_Load).
constexpr int kGroundVtBuild  = 8;   // Build(gnd, a, b, c)  __thiscall retn 10h
constexpr int kGroundVtFinish = 52;  // seconde passe(gnd)   __thiscall retn 4

void RebuildTerrain() {
  __try {
    void* gm = rag::ActiveModeIfReady();
    if (!gm) return;
    void* world = *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) +
                                            gamescene::kGmActorMgr);
    if (!world) return;
    void* ground =
        *reinterpret_cast<void**>(reinterpret_cast<char*>(world) + gamescene::kAmGround);
    if (!ground) return;

    // Le .gnd porte le nom de la carte. C'est le cas de toutes les cartes du
    // client ; si l'une y échappait, le gestionnaire rendrait nullptr et on ne
    // ferait rien plutôt que de reconstruire avec le mauvais terrain.
    char map_name[64] = {0};
    if (!rag::CurrentMapName(map_name, sizeof(map_name))) return;
    char gnd_name[80] = {0};
    size_t n = 0;
    while (n < sizeof(map_name) - 1 && map_name[n]) {
      gnd_name[n] = map_name[n];
      ++n;
    }
    const char kExt[] = ".gnd";
    for (size_t i = 0; i < sizeof(kExt); ++i) gnd_name[n + i] = kExt[i];

    auto tex_release = reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(
        kTexMgrRelease);

    void* mgr = ro::texmgr::Mgr();
    if (!mgr) {
      LogDiag("[GreyWorld] rebuild : pas de gestionnaire de ressources");
      return;
    }
    // Sans résolution de skin : un .gnd ne vit pas sous la racine d'interface, et
    // `CWorld_Load` appelle lui aussi le chargeur en direct.
    void* gnd = ro::texmgr::LoadResourceRaw(gnd_name);
    if (!gnd) {
      LogDiag("[GreyWorld] rebuild : '{}' introuvable", gnd_name);
      return;
    }

    // ⭐ RIEN À FAIRE DE SES HAUTEURS ICI. `CWorld_Load` relâche le .gnd sitôt le
    // terrain construit, et le gestionnaire le DÉTRUIT : celui qu'on vient de
    // demander est donc relu du fichier, à travers `CGnd::Load` — notre hook.
    // C'est lui qui aplatit, ou non, selon le réglage du moment ; le décider une
    // seconde fois ici ne saurait qu'aplatir, et la reconstruction sert autant à
    // RENDRE le relief qu'à l'ôter.
    void** vt = *reinterpret_cast<void***>(ground);
    auto build = reinterpret_cast<void(__fastcall*)(void*, void*, void*, void*, void*,
                                                    void*)>(
        vt[kGroundVtBuild / sizeof(void*)]);
    auto finish = reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(
        vt[kGroundVtFinish / sizeof(void*)]);
    build(ground, nullptr, gnd, reinterpret_cast<void*>(kWorldLightA),
          reinterpret_cast<void*>(kWorldLightB), reinterpret_cast<void*>(kWorldLightC));
    finish(ground, nullptr, gnd);
    tex_release(mgr, nullptr, gnd);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ── Le client DÉCLINE un warp vers la carte où il est déjà ───────────────────
//
// 🔴🔴 MESURÉ, et c'est LA raison pour laquelle un warp sur place ne suffisait
// pas. `ZC_MapChange_Handler` 0x00ccea30 (le case 0x0091 du dispatch) compare le
// nom reçu à celui de la carte courante, et n'a que deux branches :
//
//   strcmp != 0  →  CModeMgr_RequestSwitch(..., "<carte>.rsw") + écran de
//                   chargement : LE rechargement, terrain et décors compris ;
//   strcmp == 0  →  GameMode_ResetAndRecreateOwnActor + GameMode_OnEnterMapSetup :
//                   on repositionne le pantin, RIEN d'autre n'est rechargé.
//
// C'est aussi pourquoi une fly wing ne montre pas d'écran de chargement. Le
// serveur a beau faire un `pc_setpos` complet, le client range son paquet dans
// la seconde branche et garde son monde.
//
// ⭐ On ne patche donc AUCUNE branche : on fait diverger la comparaison, en
// vidant le nom de la carte courante juste avant l'appel, et en le remettant
// juste après si le client ne l'a pas réécrit lui-même. Un octet, le temps d'un
// appel, sur un thread unique — rien d'autre ne lit ce nom entre les deux.
//
// 🔴 UN SEUL COUP, ARMÉ PAR NOUS. Forcer toutes les fois ferait un écran de
// chargement à chaque fly wing, chaque @jump, chaque téléportation de PNJ sur
// place. Le drapeau expire avec le délai de grâce de la demande.
constexpr uintptr_t kZcMapChange    = 0x00ccea30;  // __thiscall(this, paquet) retn 4
// Le nom de la carte courante vit dans `rag::kCurrentMapNameAddr` — une seule
// déclaration pour tout le projet, cf. globals.h. Ici on l'ÉCRIT, ce que
// `rag::CurrentMapName` ne sait pas faire : d'où l'adresse brute plutôt que
// l'accesseur.
constexpr size_t kMapNameMax = 32;  // large : les noms font ~16

using MapChangeFn = int(__fastcall*)(void*, void*, int);
MapChangeFn g_orig_map_change   = nullptr;
bool        g_force_full_reload = false;

int __fastcall Hooked_ZcMapChange(void* self, void* edx, int packet) {
  if (!g_orig_map_change) return 0;
  if (!g_force_full_reload) return g_orig_map_change(self, edx, packet);
  g_force_full_reload = false;

  // Pas de SEH sur la LECTURE : ce nom est une donnée statique du module, donc
  // toujours cartographiée. Un `__try` ici n'apporterait rien et rendrait `n`
  // suspecte (une locale modifiée sous SEH n'est pas fiable).
  auto*  live = reinterpret_cast<char*>(rag::kCurrentMapNameAddr);
  char   saved[kMapNameMax];
  size_t n = 0;
  while (n < kMapNameMax && live[n] != '\0') {
    saved[n] = live[n];
    ++n;
  }
  // Pas de terminateur là où on l'attendait : ce n'est pas le tampon qu'on croit.
  // On laisse le client faire ce qu'il fait d'habitude plutôt que d'y écrire.
  if (n >= kMapNameMax) return g_orig_map_change(self, edx, packet);
  saved[n] = '\0';

  int result = 0;
  __try {
    live[0] = '\0';  // le strcmp diverge : la branche RECHARGEMENT est prise
    result  = g_orig_map_change(self, edx, packet);
    // Le basculement de mode n'est que DEMANDÉ ici (il se joue plus tard dans la
    // frame), donc le nom n'a pas encore été réécrit : on le rend, pour que la
    // minimap et la navigation ne lisent pas du vide en attendant.
    if (live[0] == '\0')
      for (size_t i = 0; i <= n; ++i) live[i] = saved[i];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return result;
}

// ── Faire recharger la carte SUR PLACE ───────────────────────────────────────
//
// Ce que « aplatir » fait au monde est figé au CHARGEMENT : le terrain reçoit
// ses hauteurs une fois, et les décors sont acceptés ou refusés une fois. La
// bascule ne se voit donc franchement qu'au changement de carte suivant — dans
// les deux sens, et surtout au décochage, où les décors manquants ne reviennent
// pas tout seuls.
//
// 🔴 LE CLIENT NE PEUT PAS SE RECHARGER TOUT SEUL, et se fabriquer un faux warp
// serait pire que rien : il rechargerait bien le décor, mais il vide sa liste
// d'acteurs et renvoie un `loadendack` que le serveur IGNORE (il sort d'entrée
// sur `sd->prev != nullptr`, le joueur n'ayant jamais quitté la carte de son
// point de vue). Décor juste, scène vide.
//
// On demande donc au SERVEUR un warp sur la case où l'on est déjà, par la
// commande `@refreshmap` : son `pc_setpos` retire le joueur de la carte, ce qui
// rend au `loadendack` tout son travail. Rien à ajouter côté client — le
// chargement qui suit est celui de n'importe quel warp.
//
// ⭐ UNE COMMANDE, PAS UN OPCODE. Un CZ custom que le serveur ne connaîtrait pas
// (client déployé en avance, serveur non redémarré) fait DÉCONNECTER la session :
// rAthena `set_eof` sur tout opcode absent de sa table. Une commande inconnue,
// elle, se répond en une ligne de chat. La commande se tape aussi à la main, et
// elle DIT pourquoi elle refuse — un paquet, lui, échouait en silence.
//
// Ce que le serveur peut refuser (combat récent, fenêtre ouverte) : on n'insiste
// pas, on le DIT dans le panneau et on retombe sur la reconstruction à chaud,
// qui rattrape au moins le terrain.
constexpr uint32_t kReloadGraceMs = 3000;  // au-delà, le serveur n'a pas suivi

// Ce que la carte AFFICHÉE porte : décors déchargés et terrain aplati, ou non.
// C'est lui, et non le réglage, qui dit s'il faut recharger.
bool g_flatten_on_map = false;

// ── Les quatre temps d'une demande de rechargement ───────────────────────────
//
// 🔴🔴 « ARMÉE » ET « PARTIE » NE SONT PAS LE MÊME INSTANT, et les confondre
// était un bug : `ChatWindow::FlushPending` ne tourne pas à la frame mais sur
// ÉVÉNEMENT (CMode::SendMsg, cf. Bourgeon::OnProcessInput), donc une ligne armée
// attend un geste du joueur pour partir. Le délai de grâce compté depuis
// l'armement pouvait expirer avant même que le serveur ait vu la commande, et le
// panneau annonçait alors un refus qui n'avait pas eu lieu.
enum class Reload {
  kIdle,    // rien en cours
  kArmed,   // demande posée par une case à cocher, pas encore remise au chat
  kQueued,  // remise au chat, en attente de son départ réel
  kSent,    // partie : c'est de LÀ que court le délai de grâce
};
Reload   g_reload         = Reload::kIdle;
uint32_t g_reload_serial  = 0;  // rang de NOTRE ligne dans la file du chat
uint32_t g_reload_sent_ms = 0;

// Ce que le RÉGLAGE demande — à ne pas confondre avec `TerrainIsFlattened()`,
// qui dit ce que le terrain en mémoire porte VRAIMENT. Les deux diffèrent tout
// le temps d'un rechargement, et c'est précisément ce que la machine d'état
// ci-dessous a pour travail de résorber.
bool FlattenActive() { return Enabled() && g_cfg.flatten; }

// Vrai quand le monde affiché ne correspond plus au réglage : le panneau s'en
// sert pour dire au joueur ce qu'il attend.
bool MapOutOfDate() { return FlattenActive() != g_flatten_on_map; }

// ── Le témoin d'une carte neuve ──────────────────────────────────────────────
//
// Appelé par `Hooked_AttrLoad`, et de là seulement : `CWorld_Load` charge le .gat
// une fois par carte et TOUJOURS (rien de ce qu'il ouvre ne survit dans le cache
// à un changement de carte, cf. plus haut). Aucun compteur, aucune comparaison —
// le chargement lui-même est l'événement.
//
// 🔴 Il remplace un compteur de paquets 0x0091 qui mentait dans les DEUX sens :
// il bougeait sur les warps SUR PLACE, que le client ne recharge pas, et restait
// muet au retour du char-select, qu'il recharge. Le témoin s'en trouvait faussé,
// et la demande suivante mourait d'entrée sur `MapOutOfDate()`.
void NoteMapLoaded() {
  g_flatten_on_map = FlattenActive();
  // Quel qu'en soit l'auteur — notre commande, un warp de PNJ, une
  // téléportation — le monde affiché est neuf : la demande n'a plus d'objet, et
  // le forçage encore armé n'aurait plus qu'à frapper un warp innocent.
  g_reload            = Reload::kIdle;
  g_force_full_reload = false;
}

// Posée par les cases à cocher, consommée hors frame ImGui comme le rebuild.
void RequestMapReload() {
  if (!MapOutOfDate()) {
    // Le joueur est revenu à ce qui est déjà chargé. Une demande pas encore
    // remise au chat n'a plus d'objet : on la retire plutôt que de faire
    // recharger pour rien. Partie, en revanche, elle suit son cours — le
    // chargement qui vient appliquera le réglage du moment, quel qu'il soit.
    if (g_reload == Reload::kArmed) g_reload = Reload::kIdle;
    return;
  }
  if (g_reload == Reload::kIdle) g_reload = Reload::kArmed;
}

// La chatbox est TOUJOURS instanciée (LoadPlugins l'ajoute sans condition), même
// quand le joueur a gardé la chatbox native : c'est son `FlushPending` qui joue
// l'envoi, hors frame ImGui, et il tourne dans les deux cas.
//
// On retient le RANG de notre ligne : c'est lui qui dira qu'elle est réellement
// partie, et non pas seulement mise en file.
bool SendMapReload() {
  auto* chat = Bourgeon::Instance().chat_window();
  if (chat == nullptr || !chat->SendTextNow("@refreshmap")) return false;
  g_reload_serial = chat->LastArmedSendSerial();
  return true;
}

// Une passe par frame de scène : elle fait avancer la demande d'un temps, et
// jamais plus. Le retour à `kIdle` sur un chargement, lui, vient du témoin.
void PumpMapReload() {
  if (g_reload == Reload::kIdle) return;
  Bourgeon& b = Bourgeon::Instance();
  if (b.IsMapLoading() || !b.IsGameActive()) return;

  if (g_reload == Reload::kArmed) {
    if (SendMapReload()) g_reload = Reload::kQueued;
    return;
  }

  if (g_reload == Reload::kQueued) {
    auto* chat = b.chat_window();
    if (chat == nullptr) {
      g_reload = Reload::kIdle;  // plus de chatbox : plus personne pour l'envoyer
      return;
    }
    if (chat->SentSendSerial() < g_reload_serial) return;  // toujours en file
    // 🔴 LE FORÇAGE S'ARME ICI, au départ de la ligne — pas à son armement. Le
    // paquet qui va venir répond à CETTE commande ; posé plus tôt, le drapeau
    // aurait pu être consommé par un warp ordinaire survenu entre-temps, et
    // notre rechargement serait retombé dans la branche « rien à recharger ».
    g_force_full_reload = true;
    g_reload_sent_ms    = GetTickCount();
    g_reload            = Reload::kSent;
    return;
  }

  // kSent : passé le délai de grâce, le serveur a refusé (combat récent, fenêtre
  // ouverte) ou ne connaît pas la commande. On cesse de l'attendre ; le terrain
  // se rattrape à chaud dès la frame suivante, dans les deux sens, puisque la
  // reprise à chaud ne s'abstient que le temps d'un rechargement en vue. Les
  // décors, eux, attendront un vrai changement de carte — c'est ce que le
  // panneau annonce tant que `MapOutOfDate()` reste vrai.
  if (GetTickCount() - g_reload_sent_ms < kReloadGraceMs) return;
  g_reload = Reload::kIdle;
  // 🔴 DÉSARMER. Le paquet n'est pas venu ; laissé armé, le drapeau se
  // déclencherait sur le prochain warp sur place du joueur — une fly wing, un
  // @jump — et lui infligerait un écran de chargement qu'il n'a pas demandé.
  g_force_full_reload = false;
}

// Le quadrillage est posé AVANT l'appel natif : à profondeur égale, la file de
// scène départage les primitives par leur adresse d'insertion, donc soumettre
// en premier laisse le curseur de destination et la trace de navigation du
// client par-dessus notre grille — ce qui est l'ordre qu'on veut.
void __fastcall Hooked_SceneRenderCells(void* self) {
  // 🔴 HORS du test sur `Enabled()` : le cas qui compte le plus est justement
  // celui où le joueur vient d'ÉTEINDRE GreyWorld sur une carte chargée à plat.
  // Sous la garde, la demande ne serait jamais envoyée.
  PumpMapReload();

  // ── Le relief à chaud, dans les DEUX sens ──────────────────────────────────
  //
  // 🔴 HORS de `Enabled()`, comme la pompe : le cas qui compte le plus est celui
  // où le joueur vient d'ÉTEINDRE GreyWorld sur une carte chargée à plat, et
  // sous la garde on ne lui rendrait jamais son relief.
  //
  // 🔴 MAIS PAS PENDANT QU'UN RECHARGEMENT EST EN VUE : celui-ci fait mieux (les
  // décors suivent, les fichiers sont relus tels quels) et il effacerait de toute
  // façon ce qu'on aurait écrit entre-temps. On ne s'en mêle donc qu'une fois
  // qu'il a échoué — ou quand il n'a jamais été demandé, ce qui est le cas de
  // toutes les frames ordinaires, y compris celle qui suit un chargement.
  //
  // Les deux fonctions se gardent elles-mêmes : elles sortent sur une
  // comparaison quand tout est déjà en place.
  if (g_reload == Reload::kIdle) {
    if (FlattenActive()) {
      ReflattenLiveAttr();  // fixe le niveau que la reconstruction reprend
    } else {
      RestoreLiveAttr();
    }
  }
  // Consommée hors de tout test de réglage : la reconstruction sert autant à
  // rendre le relief qu'à l'ôter, et l'une des deux est demandée par un module
  // qu'on vient peut-être d'éteindre.
  if (g_rebuild_requested) {
    g_rebuild_requested = false;
    RebuildTerrain();
  }
  if (Enabled() && g_cfg.grid && self) DrawCellGrid(self);
  RunScenePainters(self);
  if (g_orig_scene_cells) g_orig_scene_cells(self);
}

}  // namespace

// ── API publique ─────────────────────────────────────────────────────────────
Config& cfg() { return g_cfg; }

void AddScenePainter(ScenePainter paint, PainterStyleFn style) {
  if (!paint || !style) return;
  // Poser les détours ici, et pas au premier réglage : un module qui peint ne
  // doit pas dépendre de l'état de GreyWorld.
  EnsureInstalled();
  for (size_t i = 0; i < g_painters.size(); ++i)
    if (g_painters[i].paint == paint) return;  // idempotent : s'inscrire deux
                                               // fois ne doit pas peindre deux fois
  Invited inv;
  inv.paint = paint;
  inv.style = style;
  g_painters.push_back(inv);
}

void EnsureInstalled() {
  static bool done = false;
  if (!done) {
    done = true;
    using namespace hooking;
    // Un trampoline nul = SetHook a échoué (prologue non relocalisable). Chaque
    // hook teste donc son original avant de le chaîner : un décor qui disparaît
    // pour de bon vaut mieux qu'un plantage, mais un décor qui disparaît alors
    // que la case est décochée serait pire que les deux.
    g_orig_rsm = reinterpret_cast<RsmRenderFn>(
        HookManager::Instance().SetHook(HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kRsmRenderNode),
            reinterpret_cast<uint8_t*>(&Hooked_RsmRenderNode)));
    g_orig_scene_cells = reinterpret_cast<SceneCellsFn>(
        HookManager::Instance().SetHook(HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kSceneRenderCells),
            reinterpret_cast<uint8_t*>(&Hooked_SceneRenderCells)));
    // Les deux chargeurs de terrain. Posés même si « aplatir » est décoché : ils
    // ne font alors que chaîner l'original, et ils DOIVENT exister avant le
    // prochain changement de carte — c'est là que tout se joue.
    g_orig_attr_load = reinterpret_cast<LoadResFn>(
        HookManager::Instance().SetHook(HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kAttrLoad),
            reinterpret_cast<uint8_t*>(&Hooked_AttrLoad)));
    g_orig_gnd_load = reinterpret_cast<LoadResFn>(
        HookManager::Instance().SetHook(HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kGndLoad),
            reinterpret_cast<uint8_t*>(&Hooked_GndLoad)));
    g_orig_rsm_load = reinterpret_cast<LoadResFn>(
        HookManager::Instance().SetHook(HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kRsmLoad),
            reinterpret_cast<uint8_t*>(&Hooked_RsmLoad)));
    // Le case 0x0091 du dispatch réseau. Posé en permanence, mais inerte tant
    // que `g_force_full_reload` est faux — c'est-à-dire toujours, sauf pendant
    // la poignée de frames qui suivent notre propre demande de rechargement.
    g_orig_map_change = reinterpret_cast<MapChangeFn>(
        HookManager::Instance().SetHook(HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kZcMapChange),
            reinterpret_cast<uint8_t*>(&Hooked_ZcMapChange)));
  }
}

void LoadStartupState() {
  // Les deux seuls champs qui doivent valoir avant le premier chargement de
  // carte. Les autres ne pilotent que du rendu et peuvent attendre la lecture
  // normale du fichier.
  try {
    const YAML::Node ui = startup::Section("moonlight_ui");
    if (ui) {
      if (ui["greyworld"]) g_cfg.enabled = ui["greyworld"].as<bool>(g_cfg.enabled);
      if (ui["greyworld_flatten"]) {
        g_cfg.flatten = ui["greyworld_flatten"].as<bool>(g_cfg.flatten);
      }
    }
  } catch (const std::exception&) {
    // Un fichier illisible ne doit pas empêcher les détours d'exister : ils ne
    // font rien tant que les drapeaux sont faux, et le réglage repassera par la
    // lecture normale.
  }
  // La première carte du jeu sera chargée avec CES valeurs-là. Le témoin sera de
  // toute façon posé par son chargement (cf. NoteMapLoaded) : ceci n'est que la
  // valeur de départ, le temps qu'il ait lieu.
  g_flatten_on_map = g_cfg.enabled && g_cfg.flatten;
  EnsureInstalled();
}

void Apply() {
  if (g_cfg.enabled) EnsureInstalled();

  // Sol uni : GreyWorld ne repeint pas lui-même, il le DEMANDE au module qui
  // sait déjà le faire. Si le staff a coché son propre fond de capture, c'est sa
  // couleur qui l'emporte — un outil de capture d'écran ne doit pas se faire
  // voler sa teinte par un réglage de confort.
  const bool want_ground = Enabled() && g_cfg.flat_ground && !g_imgui_dx7_active;
  ground_paint::SetExternalPaint(want_ground, g_cfg.col_ground);
  if (want_ground) ground_paint::EnsureInstalled();
}

bool DrawSettings() {
  bool changed = false;

  ImGui::TextWrapped("%s", i18n::Tr(
      "Le monde dépouillé, comme les GRF « greyworld » que la communauté RO se "
      "passe pour la WoE — mais réglable, réversible, et sans rien installer : "
      "plus rien ne cache les monstres, et le sol dit où l'on peut marcher."));
  ImGui::Spacing();

  if (ro::RoCheckbox(i18n::Tr("Activer GreyWorld"), &g_cfg.enabled)) {
    Apply();
    // Seul « aplatir » est figé au chargement : c'est donc le seul levier qui
    // fasse recharger la carte, et seulement si le joueur l'a coché.
    RequestMapReload();
    changed = true;
  }
  Tooltip(i18n::Tr("Un seul interrupteur pour les quatre leviers ci-dessous.\n"
                   "Rien n'est modifié dans tes fichiers : tout se fait à "
                   "l'exécution, donc sur toutes les cartes, même celles ajoutées "
                   "après coup."));

  // 🔴 HORS du bloc grisé ci-dessous : le cas le plus utile est celui où le
  // joueur vient d'ÉTEINDRE l'interrupteur sur une carte chargée à plat, et il
  // ne verrait alors qu'un message éteint sous une case éteinte.
  if (MapOutOfDate()) {
    if (g_reload != Reload::kIdle) {
      ImGui::TextDisabled("%s", i18n::Tr("Rechargement de la carte demandé…"));
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", i18n::Tr(
          "Le serveur n'a pas rechargé la carte : les décors ne suivront qu'au "
          "prochain changement de carte."));
      Tooltip(i18n::Tr("Il refuse tant que tu sors du combat, ou qu'une fenêtre "
                       "lui reste ouverte (PNJ, échange, entrepôt) — il te le "
                       "dit alors dans le chat.\n"
                       "C'est la commande « @refreshmap » : tu peux la taper "
                       "toi-même, et n'importe quel warp fait la même chose."));
    }
  }

  // Les leviers restent VISIBLES quand l'interrupteur est levé, simplement
  // grisés : le joueur voit du même coup ce que GreyWorld recouvre, sans avoir
  // à l'allumer pour le découvrir. Un repli les cachait — c'était une porte de
  // plus pour rien.
  ImGui::BeginDisabled(!g_cfg.enabled);
  ImGui::Spacing();

  // ── Décors ────────────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Décors"));
  if (ro::RoCheckbox(i18n::Tr("Masquer les décors 3D"), &g_cfg.hide_models)) {
    changed = true;
  }
  Tooltip(i18n::Tr("Arbres, murs, ponts, rochers, mobilier : tout ce que la carte "
                   "pose par-dessus le sol.\n"
                   "Les monstres et PNJ en 3D ne sont PAS concernés — ils passent "
                   "par un autre chemin de rendu."));


  // ── Quadrillage ───────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Quadrillage du sol"));
  if (ro::RoCheckbox(i18n::Tr("Afficher les cellules"), &g_cfg.grid)) changed = true;
  Tooltip(i18n::Tr("Une case par cellule de la carte, colorée selon ce qu'on peut "
                   "y faire. Le dessin épouse le relief : c'est la fonction dont "
                   "le client se sert déjà pour ton curseur de destination."));

  if (g_cfg.grid) {
    // 🔴 Libellés NUS : `ro::RoCombo` traduit ses items lui-même, à la lecture.
    static const char* const kPatterns[] = {"Anneau (curseur du jeu)",
                                            "Carrelage (cadre de case)",
                                            "Carreau plein (avec joint)"};
    static const char* const kFills[] = {"Toutes les cases",
                                         "Contour des obstacles"};
    ImGui::SetNextItemWidth(ro::Px(220.0f));
    if (ro::RoCombo(i18n::Tr("Dessin"), &g_cfg.pattern, kPatterns,
                    IM_ARRAYSIZE(kPatterns))) {
      changed = true;
    }
    Tooltip(i18n::Tr("L'anneau et le cadre sont des textures du client — celle de "
                     "ton curseur de destination et celle des sorts de zone.\n"
                     "« Carreau plein » emploie la nôtre : la case devient un "
                     "aplat, un peu plus petit qu'elle, et c'est le sol laissé "
                     "visible tout autour qui trace la bordure."));

    if (g_cfg.pattern == Config::kPatternSolid) {
      ImGui::SetNextItemWidth(ro::Px(220.0f));
      if (WheelSliderInt(i18n::Tr("Joint (%)"), &g_cfg.gap, 0, 40, "%d %%")) {
        changed = true;
      }
      Tooltip(i18n::Tr("Largeur de la bordure, en pourcentage du côté de la "
                       "case. À zéro, les carreaux se touchent et le quadrillage "
                       "disparaît."));
    }

    ImGui::SetNextItemWidth(ro::Px(220.0f));
    if (ro::RoCombo(i18n::Tr("Cases dessinées"), &g_cfg.fill, kFills,
                    IM_ARRAYSIZE(kFills))) {
      changed = true;
    }
    Tooltip(i18n::Tr("« Contour des obstacles » ne garde que les cases qui "
                     "touchent un terrain différent : on lit la SILHOUETTE des "
                     "murs, et il reste dix fois moins de cases à dessiner.\n"
                     "C'est donc aussi le réglage le moins cher, avant même la "
                     "portée — et il ne peut pas cacher un obstacle, puisqu'il "
                     "garde toujours les bords."));

    if (WheelSliderInt(i18n::Tr("Portée (cellules)"), &g_cfg.radius, 8, 60)) {
      changed = true;
    }
    Tooltip(i18n::Tr("Demi-côté du carré dessiné autour de toi. C'est LE réglage "
                     "qui coûte : le nombre de cases croît au carré (24 en vaut "
                     "2401, 60 en vaut 14 641). Baisse-le si tu perds des images "
                     "par seconde."));

    ImGui::Spacing();
    if (RoColorSwatch(i18n::Tr("Cellule marchable"), g_cfg.col_walk)) changed = true;
    Tooltip(i18n::Tr("Mets son opacité à zéro pour ne garder que les obstacles : "
                     "c'est aussi la façon la moins chère d'afficher le "
                     "quadrillage, puisque les cases invisibles ne sont plus "
                     "dessinées du tout."));
    if (RoColorSwatch(i18n::Tr("Mur"), g_cfg.col_block)) changed = true;
    Tooltip(i18n::Tr("Ni marche, ni tir : la cellule est fermée."));
    if (RoColorSwatch(i18n::Tr("Infranchissable mais tirable"), g_cfg.col_snipe)) {
      changed = true;
    }
    Tooltip(i18n::Tr("Un vide ou une dénivellation : on ne peut pas y aller, mais "
                     "les sorts et les flèches passent."));
  }

  // ── Sol ───────────────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Sol"));
  if (ro::RoCheckbox(i18n::Tr("Sol d'une seule couleur"), &g_cfg.flat_ground)) {
    Apply();
    changed = true;
  }
  Tooltip(i18n::Tr("Repeint le terrain sans toucher à sa géométrie : l'occlusion "
                   "par le relief reste correcte."));
  if (g_imgui_dx7_active) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s",
                       i18n::Tr("(DX7 : non supporté)"));
  } else if (g_cfg.flat_ground) {
    if (RoColorSwatch(i18n::Tr("Couleur du sol"), g_cfg.col_ground)) {
      Apply();
      changed = true;
    }
    if (ground_paint::enabled()) {
      ImGui::TextDisabled("%s", i18n::Tr(
          "Le « Sol uni » des Staff Tools est actif : c'est SA couleur qui "
          "s'affiche."));
    }
  }

  if (ro::RoCheckbox(i18n::Tr("Aplatir le relief"), &g_cfg.flatten)) {
    // Le terrain ET les décors sont figés au chargement : la seule bascule
    // honnête, dans les deux sens, est de refaire charger la carte. On la
    // demande au serveur ; s'il refuse, `PumpMapReload` retombe sur la
    // reconstruction à chaud du terrain.
    RequestMapReload();
    changed = true;
  }
  Tooltip(i18n::Tr("Met toute la carte à une seule altitude — la moyenne de son "
                   "relief — comme le fait un GRF greyworld, sols et personnages "
                   "ensemble.\n"
                   "Les décors ne sont alors plus chargés du tout : ils garderaient "
                   "leur hauteur d'origine et flotteraient au-dessus du sol, et "
                   "même invisibles ils arrêteraient ton curseur.\n"
                   "🔴 Tout cela est figé au chargement de la carte. Cocher ou "
                   "décocher demande donc au serveur de te faire recharger, sur "
                   "la case où tu es : tu vois l'écran de chargement, tu ne "
                   "bouges pas.\n"
                   "Le niveau de l'eau n'est pas touché : sur une carte qui en a, "
                   "le sol peut passer dessous."));

  ImGui::EndDisabled();
  return changed;
}

}  // namespace grey_world
