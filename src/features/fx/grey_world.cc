#include "features/fx/grey_world.h"

#include <Windows.h>

#include <cstdint>

#include "imgui.h"
#include "bourgeon.h"                  // chat_window(), IsGameActive, MapLoadEpoch
#include "features/fx/ground_paint.h"  // levier 3 : le sol uni, déjà écrit
#include "features/systems/login_spectator.h"  // le décor de login n'est pas le jeu
#include "features/windows/chat_window.h"      // SendTextNow : notre « @refreshmap »
#include "ragnarok/game_scene.h"       // gamescene:: — le foyer des offsets de scène
#include "ragnarok/globals.h"          // rag::ActiveModeIfReady()
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
constexpr uintptr_t kSpriteTexCache     = 0x0125161c;  // l'objet, pas un pointeur

// Les 8 multiplicateurs d'UV que le client passe pour une tuile pleine. On les
// réemploie tels quels : ce sont EXACTEMENT ceux du curseur de destination.
constexpr uintptr_t kCellQuadDefaultUV = 0x01211c30;

// La file de scène. `*(void**)kSceneRenderQueuePtr` = l'objet ; son slot de
// vtable +0x10 est le « brouillard actif ou non » (World_ApplyMapFogParams
// l'appelle avec 0 quand la carte n'a pas de données de brouillard, avec 1
// après en avoir lu les quatre paramètres).
constexpr uintptr_t kSceneRenderQueuePtr = 0x012515f8;
constexpr int       kVtFogEnable         = 4;  // slot 4 = octet +0x10

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
using FogEnableFn   = int(__fastcall*)(void*, void*, int);

RsmRenderFn  g_orig_rsm         = nullptr;
SceneCellsFn g_orig_scene_cells = nullptr;
FogEnableFn  g_orig_fog_enable  = nullptr;

// Ce que le CLIENT a demandé en dernier pour le brouillard. C'est la valeur à
// lui rendre quand on cesse de la couper : on ne peut pas se contenter de
// remettre 1, la plupart des cartes n'ont aucun brouillard et le rallumer y
// poserait une brume que le joueur n'avait jamais vue.
//
// 🔴 Tant qu'on n'a pas TRAVERSÉ le hook une fois — c'est-à-dire tant qu'on n'a
// pas changé de carte depuis l'activation — cette valeur ne veut rien dire, et
// la pousser éteindrait pour de bon un brouillard qu'on devait seulement
// suspendre. D'où le drapeau : sans lui, on ne restaure RIEN et la carte
// suivante rétablit d'elle-même ce qu'elle demande.
int  g_native_fog       = 0;
bool g_native_fog_known = false;

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

// Le picker travaille en RGBA, le client en ARGB.
uint32_t ToArgb(const float rgba[4]) {
  auto ch = [](float v) -> uint32_t {
    const int i = static_cast<int>(v * 255.0f + 0.5f);
    return static_cast<uint32_t>(i < 0 ? 0 : (i > 255 ? 255 : i));
  };
  return (ch(rgba[3]) << 24) | (ch(rgba[0]) << 16) | (ch(rgba[1]) << 8) | ch(rgba[2]);
}

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
// ⚠ Le .gnd, lui, reste EXACTEMENT plat : il ne sert qu'au RENDU et aux coins
// que lit le quadrillage (`GetCellCorners` prend ses hauteurs du C3dGnd). Y
// mettre le jitter ferait onduler les carreaux sans rien apporter au clic — c'est
// l'erreur qu'a corrigée cette version.
constexpr float kFlatJitter = 0.1f;

// Moyenne des hauteurs, puis écriture de cette moyenne partout. `stride` et
// `count` diffèrent entre les deux fichiers, le reste est identique : quatre
// flottants en tête de chaque case.
void FlattenHeights(char* cells, int count, int stride, bool set_level,
                    float jitter) {
  if (!cells || count <= 0) return;
  if (set_level || !g_flat_level_known) {
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
      const float* h = reinterpret_cast<const float*>(cells + i * stride);
      for (int c = 0; c < kCornersPerCell; ++c) sum += h[c];
    }
    g_flat_level = static_cast<float>(sum / (count * kCornersPerCell));
    g_flat_level_known = true;
  }
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

int __fastcall Hooked_AttrLoad(void* self, void* edx, void* path) {
  const int ok = g_orig_attr_load ? g_orig_attr_load(self, edx, path) : 0;
  if (!ok || !Enabled() || !g_cfg.flatten || !self) return ok;
  __try {
    auto* a = reinterpret_cast<char*>(self);
    const int w = *reinterpret_cast<int*>(a + gamescene::kTerrainWidth);
    const int h = *reinterpret_cast<int*>(a + gamescene::kTerrainHeight);
    // La carte QUI VIENT D'ARRIVER fixe le niveau : `true`, sans quoi on
    // garderait celui de la précédente et le sol serait décalé.
    auto* cells = *reinterpret_cast<char**>(a + gamescene::kTerrainCells);
    // C'est le .gat qui porte les triangles du clic-sol : c'est LUI qui a besoin
    // du micro-relief, sans quoi un rayon rasant ne les rencontre jamais.
    FlattenHeights(cells, w * h, gamescene::kCellStride, /*set_level=*/true,
                   /*jitter=*/kFlatJitter);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return ok;
}

int __fastcall Hooked_GndLoad(void* self, void* edx, void* path) {
  const int ok = g_orig_gnd_load ? g_orig_gnd_load(self, edx, path) : 0;
  if (!ok || !Enabled() || !g_cfg.flatten || !self) return ok;
  __try {
    auto* g = reinterpret_cast<char*>(self);
    const int w = *reinterpret_cast<int*>(g + kGndWidth);
    const int h = *reinterpret_cast<int*>(g + kGndHeight);
    auto* cells = *reinterpret_cast<char**>(g + kGndCells);
    // Le .gnd n'est que du rendu : plan EXACT, pour que les carreaux du
    // quadrillage ne se mettent pas à onduler.
    FlattenHeights(cells, w * h, kGndCellStride, /*set_level=*/false, /*jitter=*/0.0f);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return ok;
}

// ── Notre propre dessin de case, avec un JOINT ───────────────────────────────
//
// Calqué sur C3dGround15_DrawCellQuad, à une chose près : après avoir réduit les
// coins à la case GAT (la parité de x et de y choisit le quart de tuile GND —
// c'est le calcul exact du client), on rapproche encore les quatre coins de leur
// centre. Le sol reste visible tout autour : c'est le joint, et c'est lui qui
// fait un carrelage plutôt qu'une nappe.
//
// `k` = 1 donnerait la case pleine ; on n'appelle alors PAS cette fonction mais
// celle du client, qui est éprouvée.
void DrawCellShrunk(void* ground, void* scene, int cell_x, int cell_y,
                    uint32_t argb, void* res, float k, const float uv[8]) {
  float c[12];
  auto get_corners =
      reinterpret_cast<int(__fastcall*)(void*, void*, int, int, float*)>(
          kGetCellCorners);
  if (!get_corners(ground, nullptr, cell_x, cell_y, c)) return;  // hors carte

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

  void* queue = *reinterpret_cast<void**>(kSceneRenderQueuePtr);
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

// ── Le quadrillage ───────────────────────────────────────────────────────────
//
// Un quad par cellule, dessiné par la fonction du client. Elle épouse le relief
// (elle demande les quatre coins 3D de la tuile), applique le même biais de
// profondeur que le curseur de destination — ce qui la fait passer devant le sol
// sans z-fighting — et insère dans la file de scène, donc le tri avec les
// acteurs reste celui du jeu.
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
    void* grid_res = get_res(reinterpret_cast<void*>(kSpriteTexCache), nullptr,
                             style.texture, 0, 0, 1, 0);
    if (!grid_res) return;

    auto draw = reinterpret_cast<DrawCellFn>(kDrawCellQuad);
    void* scene = reinterpret_cast<char*>(self) + kSceneCtxOffset;

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

    const uint32_t col_walk  = ToArgb(g_cfg.col_walk);
    const uint32_t col_block = ToArgb(g_cfg.col_block);
    const uint32_t col_snipe = ToArgb(g_cfg.col_snipe);

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

    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
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
        // Sans joint, on laisse faire le client : sa fonction est éprouvée, la
        // nôtre n'existe que pour ce que la sienne ne sait pas faire.
        if (shrink < 1.0f) {
          DrawCellShrunk(ground, scene, x, y, argb, grid_res, shrink,
                         reinterpret_cast<const float*>(uv));
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

// ── Le CACHE court-circuite les chargeurs — d'où ce rattrapage ───────────────
//
// 🔴🔴 `UITextureMgr_Load` 0x00a8d4a0 cherche D'ABORD dans son cache
// (`UITextureMgr_FindCachedRes`) et ne rappelle le `Load` de la ressource que si
// elle n'y est pas. Une carte déjà visitée dans la session ne repasse donc
// JAMAIS par `C3dAttr_Load` : son .gat garde son relief d'origine.
//
// C'est invisible à l'œil — le sol dessiné vient du .gnd — mais le clic, lui,
// teste les triangles du .GAT : on vise alors une géométrie qu'on ne voit plus,
// et l'angle de la caméra décide de ce qui répond. Exactement le défaut signalé.
//
// Le rattrapage est simple parce que le .gat n'est pas pré-transformé : le
// réécrire à chaud suffit. On le fait une fois par chargement de carte, et on
// prend le niveau DANS LE .GND courant — celui qui est réellement dessiné — ce
// qui garantit que les deux racontent la même chose, quel que soit celui des
// deux qui est passé par le cache.
// 🔴 NE PAS se fier à un compteur de chargements pour savoir s'il faut agir.
// Première version : `if (MapLoadEpoch() != g_last_map_epoch)`, avec le témoin à
// 0 — or l'epoch vaut 0 lui aussi tant qu'aucune TRANSITION n'a été comptée, et
// entrer en jeu n'en est pas forcément une. Le rattrapage n'a alors jamais
// tourné, mesuré à x32dbg : .gat aplati (par le chargeur) mais quadtree intact.
//
// L'état lui-même est un bien meilleur témoin, et il ne peut pas mentir : si la
// borne haute de la racine n'est pas la nôtre, c'est que le client vient de
// recopier celles du .rsw — donc qu'une carte a été chargée. Le test coûte une
// comparaison par frame et se réarme tout seul.

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
constexpr float kOpenSkyLow  = -999999.0f;
constexpr float kOpenSkyHigh =  999999.0f;
constexpr int   kQuadTreeMaxDepth = 5;   // le client s'arrête à 5 (1365 nœuds)
constexpr int   kNodeMinY = 6;           // node[5..7] = min(x,y,z)
constexpr int   kNodeMaxY = 9;           // node[8..10] = max(x,y,z)

void OpenNodeHeights(int* node, int depth) {
  if (!node) return;
  reinterpret_cast<float*>(node)[kNodeMinY] = kOpenSkyLow;
  reinterpret_cast<float*>(node)[kNodeMaxY] = kOpenSkyHigh;
  if (depth >= kQuadTreeMaxDepth) return;
  for (int i = 1; i <= 4; ++i) {
    OpenNodeHeights(reinterpret_cast<int*>(node[i]), depth + 1);
  }
}

void ReflattenLiveAttr() {
  __try {
    void* gm = rag::ActiveModeIfReady();
    if (!gm) return;
    void* world = *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) +
                                            gamescene::kGmActorMgr);
    if (!world) return;
    auto* w = reinterpret_cast<char*>(world);

    // Le témoin : tant que la racine porte notre borne, il n'y a rien à faire.
    // Elle ne peut revenir à autre chose que par un chargement de carte, qui
    // recopie celles du .rsw.
    int* root = reinterpret_cast<int*>(w + gamescene::kWorldQuadTree);
    if (reinterpret_cast<float*>(root)[kNodeMaxY] == kOpenSkyHigh) return;

    // 🔴 LE QUADTREE D'ABORD, et sans rien exiger d'autre. Version précédente :
    // il venait en dernier, après quatre sorties anticipées — dont une qui
    // déréférençait `C3dGround15 + 8` en croyant y trouver le .gnd. Ce champ vaut
    // en réalité la LARGEUR du .gnd en tuiles (150 sur une carte de 300, mesuré à
    // x32dbg) : la lecture partait dans le vide, le `__except` l'avalait sans un
    // mot, et le correctif du picking ne s'exécutait jamais.
    // ⚠ Un `__try` qui protège plusieurs étapes cache laquelle a échoué : mettre
    // en tête ce qui doit aboutir coûte que coûte.
    OpenNodeHeights(root, 0);

    void* attr = *reinterpret_cast<void**>(w + gamescene::kAmTerrain);
    if (!attr) return;
    auto* a = reinterpret_cast<char*>(attr);
    const int aw = *reinterpret_cast<int*>(a + gamescene::kTerrainWidth);
    const int ah = *reinterpret_cast<int*>(a + gamescene::kTerrainHeight);
    // `set_level` : on recalcule la moyenne sur place plutôt que d'aller la
    // chercher ailleurs. Sur un .gat déjà aplati par le chargeur, la moyenne des
    // dents de scie redonne exactement le même niveau — l'opération est donc sans
    // effet, et elle rattrape le cas où le cache l'a soustrait à notre chargeur.
    FlattenHeights(*reinterpret_cast<char**>(a + gamescene::kTerrainCells), aw * ah,
                   gamescene::kCellStride, /*set_level=*/true,
                   /*jitter=*/kFlatJitter);
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
constexpr uintptr_t kTexMgrGet     = 0x00a90350;  // singleton, sans argument
constexpr uintptr_t kTexMgrLoad    = 0x00a8d4a0;  // __thiscall(mgr, nom)  retn 4
constexpr uintptr_t kTexMgrRelease = 0x00a8f4b0;  // __thiscall(mgr, res)  retn 4

// Les trois triplets de lumière du monde que CWorld_Load passe à Build.
constexpr uintptr_t kWorldLightA = 0x0159b1a4;
constexpr uintptr_t kWorldLightB = 0x0159b1b0;
constexpr uintptr_t kWorldLightC = 0x0159b1bc;

// Slots de la vtable du C3dGround15, en OCTETS (cf. CWorld_Load).
constexpr int kGroundVtBuild  = 8;   // Build(gnd, a, b, c)  __thiscall retn 10h
constexpr int kGroundVtFinish = 52;  // seconde passe(gnd)   __thiscall retn 4

// Posés par la case à cocher, consommés au début d'une passe de scène : on ne
// reconstruit pas des tampons de rendu depuis le dessin d'une fenêtre ImGui.
bool g_rebuild_requested = false;

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

    auto tex_get     = reinterpret_cast<void*(__cdecl*)()>(kTexMgrGet);
    auto tex_load    = reinterpret_cast<void*(__fastcall*)(void*, void*, const char*)>(
        kTexMgrLoad);
    auto tex_release = reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(
        kTexMgrRelease);

    void* mgr = tex_get();
    if (!mgr) {
      LogDiag("[GreyWorld] rebuild : pas de gestionnaire de textures");
      return;
    }
    void* gnd = tex_load(mgr, nullptr, gnd_name);
    if (!gnd) {
      LogDiag("[GreyWorld] rebuild : '{}' introuvable", gnd_name);
      return;
    }

    auto* g = reinterpret_cast<char*>(gnd);
    const int gw = *reinterpret_cast<int*>(g + kGndWidth);
    const int gh = *reinterpret_cast<int*>(g + kGndHeight);
    auto* gnd_cells = *reinterpret_cast<char**>(g + kGndCells);

    {
      // Il revient très probablement du CACHE : s'il n'a jamais traversé notre
      // chargeur, il porte encore son relief. On l'aplatit ici, au niveau déjà
      // retenu par le .gat (`set_level=false`), pour que les deux s'accordent.
      FlattenHeights(gnd_cells, gw * gh, kGndCellStride, /*set_level=*/false,
                     /*jitter=*/0.0f);
    }

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
constexpr uintptr_t kCurrentMapName = 0x015fb9ac;  // « <carte>.gat », en clair
constexpr size_t    kMapNameMax     = 32;          // large : les noms font ~16

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
  auto*  live = reinterpret_cast<char*>(kCurrentMapName);
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
bool     g_flatten_on_map = false;
uint32_t g_seen_map_epoch = 0;
bool     g_reload_pending = false;  // demande posée, pas encore aboutie
uint32_t g_reload_sent_ms = 0;      // 0 = pas encore partie

bool FlattenActive() { return Enabled() && g_cfg.flatten; }

// Vrai quand le monde affiché ne correspond plus au réglage : le panneau s'en
// sert pour dire au joueur ce qu'il attend.
bool MapOutOfDate() { return FlattenActive() != g_flatten_on_map; }

// Posée par les cases à cocher, consommée hors frame ImGui comme le rebuild.
void RequestMapReload() {
  if (!MapOutOfDate()) return;
  g_reload_pending = true;
  g_reload_sent_ms = 0;
}

// La chatbox est TOUJOURS instanciée (LoadPlugins l'ajoute sans condition), même
// quand le joueur a gardé la chatbox native : c'est son `FlushPending` qui joue
// l'envoi, hors frame ImGui, et il tourne dans les deux cas.
bool SendMapReload() {
  auto* chat = Bourgeon::Instance().chat_window();
  if (chat == nullptr || !chat->SendTextNow("@refreshmap")) return false;
  // Le paquet qui reviendra désignera la carte où l'on est déjà : sans ceci, le
  // client le rangerait dans sa branche « rien à recharger ».
  g_force_full_reload = true;
  return true;
}

// Une passe par frame de scène. Trois choses y sont faites, dans cet ordre :
// constater qu'une carte a fini de charger, envoyer la demande en attente, et
// abandonner celle qui n'a rien donné.
void PumpMapReload() {
  Bourgeon& b = Bourgeon::Instance();
  const uint32_t epoch = b.MapLoadEpoch();

  // Un chargement a eu lieu ET s'est terminé : le monde affiché est neuf, quel
  // qu'en soit l'auteur — notre demande, un warp de PNJ, une téléportation.
  if (epoch != g_seen_map_epoch && !b.IsMapLoading()) {
    g_seen_map_epoch = epoch;
    g_flatten_on_map = FlattenActive();
    g_reload_pending = false;
    g_reload_sent_ms = 0;
    return;
  }

  if (!g_reload_pending) return;
  if (b.IsMapLoading() || !b.IsGameActive()) return;

  // Un envoi de chat en attente refuse le nôtre : on réessaie à la frame
  // suivante plutôt que de brûler la tentative. Le compteur du délai de grâce ne
  // démarre qu'une fois la ligne réellement armée.
  if (g_reload_sent_ms == 0) {
    if (SendMapReload()) g_reload_sent_ms = GetTickCount();
    return;
  }

  // Passé le délai de grâce, le serveur a refusé (ou ne connaît pas l'opcode).
  // On cesse de l'attendre et on rattrape ce qu'on sait faire à chaud : le
  // terrain. Les décors, eux, attendront un vrai changement de carte — c'est ce
  // que le panneau annonce tant que `MapOutOfDate()` reste vrai.
  if (GetTickCount() - g_reload_sent_ms >= kReloadGraceMs) {
    g_reload_pending = false;
    g_reload_sent_ms = 0;
    // 🔴 DÉSARMER. Le paquet n'est pas venu ; laissé armé, le drapeau se
    // déclencherait sur le prochain warp sur place du joueur — une fly wing, un
    // @jump — et lui infligerait un écran de chargement qu'il n'a pas demandé.
    g_force_full_reload = false;
    if (FlattenActive()) g_rebuild_requested = true;
  }
}

void TryInstallFogHook();  // défini plus bas : il lui faut Hooked_FogEnable

// Le quadrillage est posé AVANT l'appel natif : à profondeur égale, la file de
// scène départage les primitives par leur adresse d'insertion, donc soumettre
// en premier laisse le curseur de destination et la trace de navigation du
// client par-dessus notre grille — ce qui est l'ordre qu'on veut.
void __fastcall Hooked_SceneRenderCells(void* self) {
  // Le seul endroit d'où l'on soit SÛR d'être en jeu, la file de scène
  // construite : c'est là, et pas au chargement de la DLL, que le hook du
  // brouillard peut se poser (sa vtable n'a pas d'adresse statique).
  if (Enabled()) TryInstallFogHook();
  // 🔴 HORS du test sur `Enabled()` : le cas qui compte le plus est justement
  // celui où le joueur vient d'ÉTEINDRE GreyWorld sur une carte chargée à plat.
  // Sous la garde, la demande ne serait jamais envoyée.
  PumpMapReload();
  // Rattrapage : les boîtes du quadtree (que le .rsw fige au relief d'origine)
  // et le .gat (que le cache de ressources peut soustraire à notre chargeur).
  // La fonction se garde elle-même — elle sort sur une comparaison quand tout
  // est déjà en place.
  if (Enabled()) {
    if (g_cfg.flatten) ReflattenLiveAttr();  // fixe le niveau que le rebuild reprend
    // 🔴 Les deux demandes sont traitées HORS du test sur `flatten` : au
    // décochage il vaut déjà false, et c'est précisément là qu'il faut rendre le
    // relief.
    if (g_rebuild_requested) {
      g_rebuild_requested = false;
      RebuildTerrain();
    }
  }
  if (Enabled() && g_cfg.grid && self) DrawCellGrid(self);
  if (g_orig_scene_cells) g_orig_scene_cells(self);
}

// Brouillard. On ne l'éteint pas dans le vide : on retient ce que le client
// demandait, pour le lui rendre intact quand le joueur décoche.
int __fastcall Hooked_FogEnable(void* self, void* edx, int on) {
  g_native_fog       = on;
  g_native_fog_known = true;
  const int want = (Enabled() && g_cfg.no_fog) ? 0 : on;
  return g_orig_fog_enable ? g_orig_fog_enable(self, edx, want) : 0;
}

// L'objet de la file de scène n'existe qu'une fois en jeu, et sa vtable n'a pas
// d'adresse statique : le hook du brouillard ne peut donc pas se poser au
// chargement de la DLL comme les deux autres. On le tente à chaque frame de
// rendu tant qu'il n'est pas posé — c'est-à-dire une poignée de fois, puis
// jamais plus.
void TryInstallFogHook() {
  if (g_orig_fog_enable) return;
  __try {
    void* queue = *reinterpret_cast<void**>(kSceneRenderQueuePtr);
    if (!queue) return;
    void** vt = *reinterpret_cast<void***>(queue);
    if (!vt || !vt[kVtFogEnable]) return;
    g_orig_fog_enable = reinterpret_cast<FogEnableFn>(
        hooking::HookManager::Instance().SetHook(
            hooking::HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(vt[kVtFogEnable]),
            reinterpret_cast<uint8_t*>(&Hooked_FogEnable)));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Repousse vers le client l'état du brouillard voulu MAINTENANT. Le hook ne suffit
// pas : il n'est traversé qu'aux changements de carte, alors que la case se coche
// au milieu d'une partie.
void PushFogState() {
  if (!g_orig_fog_enable) return;
  const bool cut = Enabled() && g_cfg.no_fog;
  // Rien à rendre tant qu'on n'a pas vu passer la demande du client : cf. le
  // drapeau, plus haut. La carte suivante rétablira son brouillard elle-même.
  if (!cut && !g_native_fog_known) return;
  __try {
    void* queue = *reinterpret_cast<void**>(kSceneRenderQueuePtr);
    if (!queue) return;
    g_orig_fog_enable(queue, nullptr, cut ? 0 : g_native_fog);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

}  // namespace

// ── API publique ─────────────────────────────────────────────────────────────
Config& cfg() { return g_cfg; }

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
  TryInstallFogHook();
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
  // La première carte du jeu sera chargée avec CES valeurs-là : c'est le point
  // de départ du témoin, et il doit être posé ici. L'époque, elle, ne bouge
  // qu'aux warps — l'entrée en jeu n'en est pas un.
  g_flatten_on_map = g_cfg.enabled && g_cfg.flatten;
  g_seen_map_epoch = Bourgeon::Instance().MapLoadEpoch();
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

  PushFogState();
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
    if (g_reload_pending) {
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

  // ── Ambiance ──────────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Ambiance"));

  if (ro::RoCheckbox(i18n::Tr("Couper le brouillard"), &g_cfg.no_fog)) {
    Apply();
    changed = true;
  }
  Tooltip(i18n::Tr("La brume de distance que certaines cartes posent. Décocher la "
                   "case rend à la carte exactement le brouillard qu'elle "
                   "demandait — et rien sur celles qui n'en ont pas."));
  ImGui::TextDisabled("%s", i18n::Tr(
      "Les nuages et le ciel ne sont pas touchés : ce sont des effets que la "
      "carte fait naître à l'entrée, pas un réglage de rendu."));

  ImGui::EndDisabled();
  return changed;
}

}  // namespace grey_world
