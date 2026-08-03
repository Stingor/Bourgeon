#include "features/item_cell.h"
#include "ragnarok/globals.h"
#include "features/windows/storage_window.h"
#include "ui/game_texture.h"

// Icônes d'item : ro::ItemIcon (ui/icon_cache.h). Le chargement, le colorkey
// magenta et l'invalidation au reset de device y sont partagés — ce fichier en
// gardait sa propre copie, comme cinq autres plugins.
#include "ui/icon_cache.h"
#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <algorithm>
#include <cfloat>   // FLT_MAX (contraintes de taille de la fenêtre)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "imgui.h"
#include "ui/imgui_escape.h"
#include "features/systems/bourgeon_opcodes.h"  // bopcodes::kStoragePrices
#include "features/windows/inventory_viewer.h"  // PointOverViewer (retrait par glisser vers le viewer inventaire)
#include "features/windows/cart_viewer.h"       // PointOverViewer (dépôt par glisser vers le viewer cart)
#include "features/windows/item_desc_window.h"  // itemdesc::RenderSimpleDesc (aperçu au survol)
#include "features/moonlight_ui/moonlight_ui.h"      // HelpMarker (tooltip) + DrawSortModeCombo (tri serveur)
#include "features/windows/vending_window.h"    // IsComposing (shop en cours -> transferts figés)
#include "ui/qty_prompt.h"             // ro::QuantityPrompt (dialogue « combien ? »)
#include "ui/ro_imgui.h"               // BeginRoWindow / RoButton (skin RO)

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Constantes RE (client 20250716, base 0x400000 ; cf. project_storage_window_re)
namespace {

// Slot manager de la fenêtre storage (id 0x21) : mgr+0x288. Non-nul <=> ouverte,
// remis à 0 à la fermeture. = ce que FindWindow(0x21) renvoie. Relire FRAIS.

// Plus aucun offset de UIItemStoreWnd : la fenêtre native ne naît plus (cf. le
// commentaire de tête du .h). Ce qu'ils servaient à lire vient maintenant des
// paquets — used/max de 0x00f2 — ou du modèle de session pour les items. Les
// seuls offsets de fenêtre qui restent plus bas portent sur l'inventaire et le
// cart, dont les natives, elles, existent toujours.

// Nœud de liste (std::list MSVC) : next@+0, prev@+4, value@+8.
constexpr int kNodeNext = 0x00;
constexpr int kNodeAmt  = 0x18;  // value+0x10 = quantité
constexpr int kNodeInfo = 0x08;  // value = ItemSkillInfo (arg de GetBaseName)

// Champs DANS l'ItemSkillInfo (= node+kNodeInfo), tels que lus par FUN_008711a0 :
// l'id est une std::string à +0x2c (le jeu fait atoi dessus pour l'icône), le
// flag identifié est à +0x5c. (node+0xc N'EST PAS l'id fiable pour la liste vue.)
constexpr int kInfoIdStr = 0x2c;  // std::string id (SSO ; heap si cap>0xf)
constexpr int kInfoIdCap = 0x40;  // capacité de la std::string id (= +0x2c+0x14)
constexpr int kInfoIdent = 0x5c;  // byte : item identifié ?

// Le nom d'affichage complet (raffinement / [slots] / cartes / enchant) passe par
// itemcell::BuildDisplayName : mêmes appels natifs, mais un SEH par item.

// Fenêtre native du storage. Elle ne naît plus (cf. le .h) ; l'id ne sert
// qu'au filet de OnTick, qui la détruit si un chemin oublié la faisait naître.
constexpr int kWinStorage = 0x21;  // UIItemStoreWnd

// Placement et taille par défaut du viewer, à la toute 1re ouverture seulement.
// (700, 85) = la position où la native se créait ; 320x420 = la taille posée
// juste après par SetNextWindowSize, reprise ici pour le clamp à l'écran.
constexpr float kSpawnX = 700.0f, kSpawnY = 85.0f;
constexpr float kSpawnW = 320.0f, kSpawnH = 420.0f;

// Délai au-delà duquel une bascule d'onglet de storage est considérée perdue.
// Un storage alternatif est chargé de façon ASYNCHRONE (le map-server interroge
// le char-server) : 5 s couvrent très largement l'aller-retour, tout en évitant
// qu'un refus tardif laisse le viewer figé sur « Chargement… ».
constexpr uint32_t kSwitchTimeoutMs = 5000;

// Liseré des onglets de storage : 1 px, #ADADAD. En dur et NON pris au thème
// (ImGuiCol_Border) : ces onglets sont peints sur le blanc de la liste, pas sur
// le corps de la fenêtre, et la couleur de bordure du skin y passait tantôt
// invisible tantôt trop dure selon le preset choisi.
constexpr ImU32 kStgTabBorder = IM_COL32(0xAD, 0xAD, 0xAD, 255);

// ── Icônes d'item (bmp inventaire) ──────────────────────────────────────────
// BuildItemIconGrfPath(id_str, out[128], identified) __stdcall (RET 0xc, 3 args) :
// atoi(id) -> ResolveItemResNameById -> sprintf "유저인터페이스\item\<res>.bmp"
// (identified!=0 -> resname [rec+8], sinon [rec+0x1c]). On passe identified=1.






// ── Ouverture de la description d'item (clic-droit) ─────────────────────────
// FIDÈLE AU NATIF : le clic-droit du storage passe l'ItemSkillInfo COMPLET du
// nœud (chargé par le serveur, avec tout ce dont la desc a besoin) à OnMsg 0x18.
// Un ItemSkillInfo reconstruit (ctor+SetId) pose l'id mais PAS la desc -> vide.
// Donc on re-parcourt la liste live au clic pour retrouver le nœud par id et
// passer SON info (node+8). OnMsg 0x18 copie ce qu'il faut (on ne possède pas
// l'info -> aucun free). item_desc_window détecte 0xc et rend sa version enrichie.

// Appelle une méthode virtuelle (offset en octets) de `self`.

// Liste STORAGE du modèle session. ⚠ C'est bien elle qu'on parcourt, et pas la
// liste d'affichage de la fenêtre (wnd+0xe8) : quand on cache le natif
// (wnd+0x28=0) sa liste d'affichage n'est plus peuplée, alors que le modèle
// session l'est toujours — c'est déjà lui que lit Extract.
constexpr uintptr_t kStorageListHead = 0x015fbad8;

// Ouvre la fenêtre de description native (id 0xc) pour l'item d'index storage
// `index`, à (mx,my) écran, avec l'info COMPLÈTE du nœud (cartes, refine,
// enchants).
//
// ⚠ Par INDEX, jamais par id : deux exemplaires du même objet (même id, refines
// ou cartes différents) sont deux nœuds distincts, et une recherche par id
// rendrait toujours le PREMIER pour toutes leurs cases.
//
// DIFFÉRÉE au relâchement du bouton (itemcell::FlushDeferredDesc) : ouverte dès
// le clic, un appui PROLONGÉ faisait passer la description DERRIÈRE nous.
void OpenItemDesc(int index, int mx, int my) {
  itemcell::DeferDescFromIndex(kStorageListHead, index, mx, my);
}

// ── Retrait d'un item vers l'inventaire (interactif) ────────────────────────
// Réplique la branche ALT du clic-droit natif (UIItemStoreWnd_OnRButtonUp) :
// dispatcher = *(0x0121333c) ; dispatcher->vtable[0x18](0x38, index, amount, 0, 0)
// __thiscall. cmd 0x38 = "storage -> inventaire" ; index = info+4 (node+0xc),
// amount = quantité à retirer. Le serveur renvoie l'update -> le modèle et le
// viewer se rafraîchissent seuls (synchro). Le flag natif disp+0x5ce est un
// simple anti-rebond côté appelant, pas requis par la commande.
constexpr int       kCmdWithdraw = 0x38;         // storage -> body/inventaire
using DispCmd_t = void(__thiscall*)(void*, int, int, int, int, int);

// Défini plus bas, avec les autres émetteurs.
bool VendingComposing();

void WithdrawItem(int index, int amount) {
  if (amount <= 0 || VendingComposing()) return;
  __try {
    void* disp = *reinterpret_cast<void**>(rag::kActiveModePtr);
    if (disp)
      uiwnd::Vf<DispCmd_t>(disp, 0x18)(disp, kCmdWithdraw, index, amount, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Cibles d'un glisser PARTANT du viewer ───────────────────────────────────
// Uniquement les rects des VIEWERS ImGui : « Interface moderne » est un groupe
// tout-ou-rien (SetModernInterface), donc si ce viewer-ci est actif, ceux de
// l'inventaire et du cart le sont aussi et leurs fenêtres natives sont masquées
// — un rect natif ne peut plus être une cible ici.
bool MouseOverInventory(float x, float y) {
  auto* inventory = Bourgeon::Instance().inventory_viewer();
  return inventory &&
         inventory->PointOverViewer(static_cast<int>(x), static_cast<int>(y));
}

constexpr int kWinCart          = 0x28;   // UICartWnd
constexpr uintptr_t kCartVTable = 0x0103d538;
bool MouseOverCart(float x, float y) {
  auto* cart = Bourgeon::Instance().cart_viewer();
  return cart && cart->PointOverViewer(static_cast<int>(x), static_cast<int>(y));
}

// Le cart est-il OUVERT (natif classique OU remplacé par son viewer ImGui) ?
// ⚠ Ne teste PAS la visibilité, contrairement aux hit-tests ci-dessus : en
// « Interface moderne » la native est cachée alors que le cart est bel et bien
// ouvert. Sert à proposer « Vers le cart » dans le menu contextuel.
bool CartOpen() {
  __try {
    auto* cart = reinterpret_cast<uint8_t*>(uiwnd::FindWindow(kWinCart));
    return cart && *reinterpret_cast<uintptr_t*>(cart) == kCartVTable;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── Onglets de catégorie (groupes de types d'item, repris du filtre natif) ──
// `sub` = dimension de sous-catégorie de l'onglet (combo déroulant) : voir SubDim.
enum SubDim { kSubNone = 0, kSubWeapon, kSubArmor, kSubCard, kSubAmmo, kSubCostume };
// Masque des slots costume (rAthena EQP_COSTUME_* : head top/mid/low + garment).
// Un item avec un de ces bits = costume -> onglet Costumes, exclu des Armures.
constexpr uint32_t kCostumeMask = 0x3C00;
// `fav` = onglet spécial « Favoris » : filtré non par type mais par le set client
// favorites_ (cf. StorageWindow::IsFavorite). Les autres champs sont ignorés.
// `img` = base des .bmp d'onglet du client (basic_interface\<img>1.bmp actif /
// <img>2.bmp inactif), comme inventory_viewer : sert à la disposition VERTICALE,
// qui reprend les vrais onglets images du natif. Le client n'a que 6 arts d'onglet
// pour nos 10 catégories : les catégories sans art propre REUTILISENT celui de leur
// famille (armes/armures -> équipement, munitions/cash -> etc) plutôt que de
// retomber sur du texte, sinon le strip mélange tuiles et libellés. Le tooltip
// lève l'ambiguïté entre deux onglets au même art.
// `tag` = sigle de 2 caractères peint au centre de la tuile, UNIQUEMENT pour les
// catégories qui partagent un art avec une voisine (sinon trois tuiles identiques
// d'affilée sont indistinguables). nullptr = art déjà univoque.
struct StgCat {
  const char* label; const int* types; int n; int sub; bool fav;
  const char* img; const char* tag;
};
const int kCatConso[]  = {0, 1, 2};
const int kCatArme[]   = {5, 8, 9, 0xf};
const int kCatArmure[] = {4, 0xb, 0xc, 0xd, 0xe};
const int kCatCarte[]  = {6};
const int kCatMuni[]   = {10, 0x10, 0x11, 0x13};
const int kCatCash[]   = {0x12};
const int kCatDivers[] = {3, 7};
const StgCat kStgCats[] = {
    {"Tout", nullptr, 0, kSubNone, false, "tab_all", nullptr},
    {"Favoris", nullptr, 0, kSubNone, true, "tab_favs", nullptr},  // set client
    {"Consos", kCatConso, 3, kSubNone, false, "tab_use", nullptr},
    {"Armes", kCatArme, 4, kSubWeapon, false, "tab_wea", nullptr},
    {"Armures", kCatArmure, 5, kSubArmor, false, "tab_arm", nullptr},
    {"Costumes", nullptr, 0, kSubCostume, false, "tab_cos", nullptr},  // par equip mask
    {"Cartes", kCatCarte, 1, kSubCard, false, "tab_card", nullptr},
    {"Ammo", kCatMuni, 4, kSubAmmo, false, "tab_ammo", nullptr},
    {"Cash", kCatCash, 1, kSubNone, false, "tab_cash", nullptr},
    {"Etc", kCatDivers, 2, kSubNone, false, "tab_etc", nullptr},
};
constexpr int kNumStgCats = 10;
// ── Onglets IMAGES du client (disposition verticale) ────────────────────────
// Même recette que inventory_viewer : les .bmp d'onglet vivent dans
// 유저인터페이스\basic_interface\, préfixe repris de la string exe du btnbar (pas de
// chemin en dur). <img>1.bmp = onglet ACTIF, <img>2.bmp = inactif.
constexpr uintptr_t kBtnbarPath = 0x010357b8;  // "…\basic_interface\btnbar_left.bmp"

using BarTex = ro::GameTexture;  // (même forme ; le chargeur est partagé)
BarTex g_tab[kNumStgCats][2];   // onglets VERTICAUX   : tab_<x>{1,2}.bmp
BarTex g_tabh[kNumStgCats][2];  // onglets HORIZONTAUX : tabh_<x>{1,2}.bmp
// Cadres des onglets de STORAGE. Même art, même grammaire que les onglets de
// catégorie ci-dessus : <nom>1.bmp = ACTIF (bord ouvert du côté du contenu),
// <nom>2.bmp = inactif (fermé). Un seul jeu pour tous les entrepôts — c'est
// l'ICÔNE ou le libellé posé dessus qui les distingue, pas le cadre.
BarTex g_stg_tab[2];   // strip VERTICAL    : tab_sto{1,2}.bmp   (23x27)
BarTex g_stg_tabh[2];  // rangée HORIZONTALE : tabh_sto{1,2}.bmp (27x25)
bool   g_tabs_tried = false;
unsigned g_tab_epoch = 0;


// `<préfixe basic_interface\> + <file>`, préfixe pris sur la string exe du btnbar.
void BasicInterfacePath(const char* file, char* out, size_t out_sz) {
  const char* base = reinterpret_cast<const char*>(kBtnbarPath);
  const char* slash = std::strrchr(base, '\\');
  const size_t n = slash ? static_cast<size_t>(slash - base + 1) : 0;
  if (n && n < out_sz) std::memcpy(out, base, n);
  std::snprintf(out + n, out_sz - n, "%s", file);
}

// Charge (une fois) les onglets images. Les textures sont en D3DPOOL_DEFAULT :
// mortes après un reset de device -> rechargées quand l'epoch change.
void EnsureTabTextures() {
  const unsigned e = Overlay_DeviceEpoch();
  if (e != g_tab_epoch) {
    g_tab_epoch = e;
    for (auto& row : g_tab) for (auto& b : row) b = BarTex{};
    for (auto& row : g_tabh) for (auto& b : row) b = BarTex{};
    g_tabs_tried = false;
  }
  if (g_tabs_tried) return;
  g_tabs_tried = true;
  char path[160], nm[48];
  for (int c = 0; c < kNumStgCats; ++c) {
    const char* base = kStgCats[c].img;
    if (!base) continue;
    std::snprintf(nm, sizeof(nm), "%s1.bmp", base);
    BasicInterfacePath(nm, path, sizeof(path)); g_tab[c][0] = ro::TextureFromGameFile(path);
    std::snprintf(nm, sizeof(nm), "%s2.bmp", base);
    BasicInterfacePath(nm, path, sizeof(path)); g_tab[c][1] = ro::TextureFromGameFile(path);
    // Jeu HORIZONTAL : mêmes noms avec un « h » après « tab » (tab_all -> tabh_all).
    char hbase[40];
    std::snprintf(hbase, sizeof(hbase), "tabh%s", base + 3);  // saute "tab"
    std::snprintf(nm, sizeof(nm), "%s1.bmp", hbase);
    BasicInterfacePath(nm, path, sizeof(path)); g_tabh[c][0] = ro::TextureFromGameFile(path);
    std::snprintf(nm, sizeof(nm), "%s2.bmp", hbase);
    BasicInterfacePath(nm, path, sizeof(path)); g_tabh[c][1] = ro::TextureFromGameFile(path);
  }
  // Cadres des onglets de storage (un seul jeu, indépendant des catégories).
  BasicInterfacePath("tab_sto1.bmp", path, sizeof(path));
  g_stg_tab[0] = ro::TextureFromGameFile(path);
  BasicInterfacePath("tab_sto2.bmp", path, sizeof(path));
  g_stg_tab[1] = ro::TextureFromGameFile(path);
  BasicInterfacePath("tabh_sto1.bmp", path, sizeof(path));
  g_stg_tabh[0] = ro::TextureFromGameFile(path);
  BasicInterfacePath("tabh_sto2.bmp", path, sizeof(path));
  g_stg_tabh[1] = ro::TextureFromGameFile(path);
}

// ── Cadres d'onglet de storage ──────────────────────────────────────────────
// L'art est un CONTOUR (#ADADAD) à intérieur TRANSPARENT : le blanc de l'onglet
// est celui du corps de la fenêtre, qui est déjà la couleur de liste. Rien à
// remplir donc — et le skin continue de piloter cette couleur.
//
// L'état ACTIF est ouvert du côté du contenu (bas en rangée, droite en strip) ;
// l'INACTIF s'y ferme 2 px avant le bord. Les deux images font la même taille,
// c'est le dessin qui diffère — un onglet ne bouge donc pas d'un pixel quand il
// devient actif.
constexpr float kStgArtClosedInset = 2.0f;
// Repli sur l'autre état si un seul des deux .bmp a pu être chargé (même règle
// que les onglets de catégorie).
const BarTex& StorageTabArt(BarTex (&art)[2], bool selected) {
  const int want = selected ? 0 : 1;
  return art[want].tex ? art[want] : art[want ^ 1];
}

// Largeur du strip d'onglets de storage = largeur NATIVE de l'art. Sert aussi à
// la rangée de CATÉGORIES, qui s'indente d'autant pour démarrer à l'abscisse de
// la table (le strip la précède). Appeler EnsureTabTextures avant.
float StorageStripWidth() {
  const BarTex& art = g_stg_tab[0].tex ? g_stg_tab[0] : g_stg_tab[1];
  return art.tex && art.w > 0 ? static_cast<float>(art.w) : 23.0f;
}

// Hauteur du strip horizontal = hauteur NATIVE des images (repli 22 px). Même
// principe que TabStripWidth : ne jamais l'étirer, la largeur en découle.
float TabStripHeight() {
  float h = 0.0f;
  for (int c = 0; c < kNumStgCats; ++c)
    for (int s = 0; s < 2; ++s)
      if (g_tabh[c][s].h > h) h = static_cast<float>(g_tabh[c][s].h);
  return h > 0.0f ? h : 22.0f;
}

// Teinte des AddImage d'onglets = luminosité du skin RO (l'opacité vient déjà de
// style.Alpha), pour que les réglages du skin s'appliquent aussi à ces images
// dessinées en draw-list brute. Même recette que inventory_viewer.

// Largeur du strip = largeur NATIVE des images d'onglet (repli 22 px). Surtout pas
// élargie par les libellés : à 60 px de large les .bmp partent en tuiles géantes
// (l'image est mise à l'échelle de la largeur du strip, hauteur incluse).
float TabStripWidth() {
  float w = 0.0f;
  for (int c = 0; c < kNumStgCats; ++c)
    for (int s = 0; s < 2; ++s)
      if (g_tab[c][s].w > w) w = static_cast<float>(g_tab[c][s].w);
  return w > 0.0f ? w : 22.0f;
}

// Largeur du strip en mode TEXTE VERTICAL : le libellé est tourné à 90°, donc sa
// largeur à l'écran = la HAUTEUR d'une ligne de texte (+ marges).
float TabStripWidthText() {
  return ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.x;
}

// Écrit `text` tourné à 90° (lecture bas -> haut), centré sur `center`. ImGui ne
// sait pas dessiner de texte tourné : on l'écrit à plat puis on fait pivoter les
// VERTICES produits. Le clip est élargi le temps du tracé — AddText coupe les
// glyphes AVANT rotation, donc contre le rect ÉTROIT du strip, ce qui mangerait
// tout le libellé.
// `clip_min/clip_max` = zone écran où le texte a le droit d'apparaître APRÈS
// rotation (typiquement le rect de l'onglet). AddText coupant AVANT rotation, on
// lui passe l'IMAGE INVERSE de cette zone (rotation +90° autour du centre) : une
// fois les vertices tournés, le texte retombe exactement dans la zone voulue. Sans
// ça (clip plein écran), le libellé débordait du cadre et du strip.
void AddTextVertical(ImDrawList* dl, ImVec2 center, ImU32 col, const char* text,
                     ImVec2 clip_min, ImVec2 clip_max) {
  const ImVec2 sz = ImGui::CalcTextSize(text);
  // ALIGNEMENT PIXEL : centre ET origine arrondis à l'entier. Les onglets n'ont pas
  // tous la même hauteur (elle suit la longueur du libellé) et CalcTextSize est
  // fractionnaire -> sans ça, une origine sur un demi-pixel échantillonne les
  // glyphes hors grille et le libellé sort flou/plus gras que ses voisins (on
  // croirait une autre police). Une rotation de 90° pile conserve la grille, donc
  // il suffit que l'entrée soit alignée.
  const ImVec2 c(std::floor(center.x), std::floor(center.y));
  const ImVec2 pos(std::floor(c.x - sz.x * 0.5f), std::floor(c.y - sz.y * 0.5f));
  // Clip PLEIN ÉCRAN pendant le tracé : le texte est écrit à plat, donc bien plus
  // large que l'onglet — tout autre clip ferait culler les glyphes dès la
  // construction (ImFont::RenderText saute ce qui sort du rect).
  dl->PushClipRectFullScreen();
  const int v0 = dl->VtxBuffer.Size;
  dl->AddText(pos, col, text);
  const int v1 = dl->VtxBuffer.Size;
  for (int i = v0; i < v1; ++i) {  // rotation -90° autour de `c`
    ImDrawVert& v = dl->VtxBuffer[i];
    const float dx = v.pos.x - c.x, dy = v.pos.y - c.y;
    v.pos.x = c.x + dy;
    v.pos.y = c.y - dx;
  }
  // La clip rect d'une ImDrawCmd est un SCISSOR appliqué au rendu, pas seulement un
  // filtre de construction : on corrige donc a posteriori celle de la commande qui
  // vient de recevoir les glyphes, avec la zone voulue APRÈS rotation. Le
  // PushClipRect ci-dessus a ouvert une commande dédiée (ElemCount == 0), elle ne
  // contient que notre texte.
  if (v1 > v0 && !dl->CmdBuffer.empty())
    dl->CmdBuffer.back().ClipRect =
        ImVec4(clip_min.x, clip_min.y, clip_max.x, clip_max.y);
  dl->PopClipRect();
}

bool ItemInCat(int tab, int type) {
  if (tab <= 0 || tab >= kNumStgCats) return true;  // Tout
  const StgCat& c = kStgCats[tab];
  for (int i = 0; i < c.n; ++i)
    if (c.types[i] == type) return true;
  return false;
}
// Filtrage d'onglet meta-aware (a besoin du masque equip pour les costumes) :
//   - onglet Costumes : uniquement les items avec un bit costume ;
//   - autres onglets (sauf Tout) : les costumes sont EXCLUS (ils vont dans Costumes) ;
//   - reste : filtre par type (Tout inclut tout, costumes compris).
bool ItemInTab(int tab, int type, uint32_t equip) {
  if (kStgCats[tab].sub == kSubCostume) return (equip & kCostumeMask) != 0;
  if (tab != 0 && (equip & kCostumeMask) != 0) return false;
  return ItemInCat(tab, type);
}

// ── Sous-catégories (subtype d'arme/munition, slot d'équip) ──────────────────
// Type d'arme (item_data.subtype pour IT_WEAPON = rAthena e_weapon_type W_*).
const char* WeaponLabel(uint8_t st) {
  switch (st) {
    case 0:  return "Fists";       case 1:  return "Dagger";
    case 2:  return "Sword 1H";     case 3:  return "Sword 2H";
    case 4:  return "Spear 1H";    case 5:  return "Spear 2H";
    case 6:  return "Axe 1H";    case 7:  return "Axe 2H";
    case 8:  return "Mace";       case 9:  return "Mace 2H";
    case 10: return "Staff";       case 11: return "Bow";
    case 12: return "Knuckle";     case 13: return "Instrument";
    case 14: return "Whip";       case 15: return "Book";
    case 16: return "Katar";       case 17: return "Revolver";
    case 18: return "Rifle";       case 19: return "Gatling";
    case 20: return "Shotgun";     case 21: return "Grenade Launcher";
    case 22: return "Huuma";       case 23: return "Two-Handed Staff";
    default: return "Other";
  }
}
// Type de munition (item_data.subtype pour IT_AMMO = rAthena e_ammo_type A_*).
const char* AmmoLabel(uint8_t st) {
  switch (st) {
    case 1: return "Arrow";     case 2: return "Throwing Dagger";
    case 3: return "Bullet";      case 4: return "Cartridge";
    case 5: return "Grenade";    case 6: return "Shuriken";
    case 7: return "Kunai";      case 8: return "Cannonball";
    case 9: return "Throwing Weapon";   default: return "Other";
  }
}
// Slot d'équipement principal depuis le masque item_data.equip (rAthena EQP_*).
// Renvoie {clé d'ordre stable, label} ; sert aux armures ET aux cartes (cible).
struct SubCat { int key; const char* label; };
SubCat PrimaryEquipSlot(uint32_t e) {
  // Coiffe séparée en 3 slots distincts (priorité haut > milieu > bas si multi-slot).
  if (e & 0x100)                    return {0,  "Head top"};    // HEAD_TOP
  if (e & 0x200)                    return {1,  "Head mid"};    // HEAD_MID
  if (e & 0x001)                    return {2,  "Head bot"};    // HEAD_LOW
  if (e & 0x010)                    return {3,  "Armor"};       // ARMOR
  if (e & 0x004)                    return {4,  "Garment"};     // GARMENT
  if (e & 0x040)                    return {5,  "Shoes"};       // SHOES
  if (e & (0x008 | 0x080))          return {6,  "Accessory"};   // ACC L/R
  if (e & 0x020)                    return {7,  "Shield"};      // HAND_L
  if (e & 0x002)                    return {8,  "Weapon"};      // HAND_R (cartes d'arme)
  if (e & 0x8000)                   return {9,  "Ammunition"};  // AMMO
  if (e & 0x3C00)                   return {10, "Costume"};     // COSTUME_*
  if (e & 0x3F0000)                 return {11, "Shadow"};      // SHADOW_*
  return {99, "Other"};  // pas de slot principal connu (ex: cartes d'arme, cartes de costume)
}
// Slot d'un COSTUME depuis le masque equip (bits COSTUME_* distincts des slots
// normaux). Labels alignés sur PrimaryEquipSlot pour la cohérence visuelle.
SubCat CostumeSlot(uint32_t e) {
  if (e & 0x0400) return {0, "Head top"};  // COSTUME_HEAD_TOP
  if (e & 0x0800) return {1, "Head mid"};  // COSTUME_HEAD_MID
  if (e & 0x1000) return {2, "Head bot"};  // COSTUME_HEAD_LOW
  if (e & 0x2000) return {3, "Garment"};   // COSTUME_GARMENT
  return {99, "Other"};
}
// Sous-catégorie d'un item pour la dimension `dim` de l'onglet courant.
// Renvoie {-1, nullptr} si pas de sous-catégorie applicable.
SubCat SubCatOf(int dim, uint8_t subtype, uint32_t equip) {
  switch (dim) {
    case kSubWeapon:  return {subtype, WeaponLabel(subtype)};
    case kSubAmmo:    return {subtype, AmmoLabel(subtype)};
    case kSubArmor:   return PrimaryEquipSlot(equip);
    case kSubCard:    return PrimaryEquipSlot(equip);  // cible de la carte
    case kSubCostume: return CostumeSlot(equip);
    default:          return {-1, nullptr};
  }
}

// ── Paquets sortants ────────────────────────────────────────────────────────
// Fermeture du storage : CZ_CloseKafra, opcode fixe 2 octets (juste l'opcode).
// Confirmé client (opcode_map.md : 0x0193 CZ FIX 2 "CloseKafra") ET serveur moonlight
// (clif_packetdb.hpp: parseable_packet(0x0193,2,clif_parse_CloseKafra) -> storage_storageclose).
// PAS remappé par le shuffle 20130320 (contrairement à MoveToKafra 0x08ac / MoveFromKafra 0x0874).
constexpr uint16_t  kOpCloseStorage = 0x0193;
// storage -> cart : CZ_MOVE_ITEM_FROM_STORE_TO_CART, fixe 8 octets [op][index:2][amount:4].
// Confirmé client (opcode_map.md 0x0128 CZ FIX 8) + serveur (server_storage_index -> -1).
// On envoie items_[idx].index (= index storage CLIENT, le serveur fait -1). PAS remappé.
constexpr uint16_t  kOpStorageToCart = 0x0128;

// Composition d'un shop en cours (cf. VendingWindow::IsComposing).
//   - cart <-> storage : REFUSÉ par le serveur (storage_storageaddfromcart /
//     storage_storagegettocart testent sd->state.prevend).
//   - inventaire <-> storage : le serveur l'AUTORISE (clif_parse_MoveToKafra ne
//     teste que pc_istrading). On le fige quand même côté client pour que rien ne
//     bouge sous une composition en cours ; un bandeau le dit dans la fenêtre.
bool VendingComposing() {
  auto* vending = Bourgeon::Instance().vending_window();
  return vending && vending->IsComposing();
}

// Demande au serveur de fermer le storage (CZ_CloseKafra, 2 octets = juste l'opcode).
// Le serveur ferme la session et répond 0x00f8, qui lève `open_` (cf. CloseLocal).
void SendCloseStorage() {
  uint16_t op = kOpCloseStorage;
  Bourgeon::Instance().SendPacket(reinterpret_cast<uint8_t*>(&op), sizeof(op));
}

// storage -> cart : envoie un item du storage vers le cart. index = index
// storage CLIENT (items_[idx].index) ; le serveur applique server_storage_index (-1).
void SendStorageToCart(int index, int amount) {
  if (amount <= 0 || VendingComposing()) return;
  uint8_t pkt[8];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpStorageToCart;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(index);
  *reinterpret_cast<uint32_t*>(pkt + 4) = static_cast<uint32_t>(amount);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}


// Étoile pleine (marqueur favori). Le glyphe ★ (U+2605) est HORS des polices
// chargées (ProggyClean = ASCII, Malgun = range coréen) -> tracé main via
// ImDrawList : 10 triangles en éventail depuis le centre (l'étoile est concave,
// donc AddConvexPolyFilled est exclu), + un liseré foncé pour le contraste sur
// n'importe quelle icône. cx,cy = centre ; r = rayon des pointes.
void DrawFavStar(ImDrawList* dl, float cx, float cy, float r, ImU32 fill,
                 ImU32 edge) {
  ImVec2 p[10];
  for (int i = 0; i < 10; ++i) {
    const float rad = (i & 1) ? r * 0.42f : r;         // creux / pointe
    const float a = -1.57079633f + i * 0.62831853f;    // -90° puis +36° par point
    p[i] = ImVec2(cx + std::cos(a) * rad, cy + std::sin(a) * rad);
  }
  const ImVec2 c(cx, cy);
  for (int i = 0; i < 10; ++i)
    dl->AddTriangleFilled(c, p[i], p[(i + 1) % 10], fill);
  dl->AddPolyline(p, 10, edge, ImDrawFlags_Closed, 1.0f);
}

// Callback ImDrawList : échantillonnage en POINT.
//
// 🔴 À poser AVANT les icônes des onglets, toujours. L'état ambiant d'une draw
// list est LINEAR — le backend DX9 le repose dans SetupRenderState — et une
// icône de 24x24 blittée en LINEAR ressort baveuse. Le mode NET doit être
// IMPOSÉ, il ne s'obtient pas en s'abstenant (cf. ui/ro_imgui.cc, qui bascule
// pareil pour chaque pièce de skin).
void ImCbPointFilter(const ImDrawList*, const ImDrawCmd*) {
  Overlay_SetTextureFilter(false);
}

// Icône d'item en NIVEAUX DE GRIS — l'onglet de storage INACTIF porte son icône
// éteinte, l'actif la porte en couleur.
//
// Une teinte ne suffit PAS : `AddImage` multiplie, donc un gris ne fait
// qu'assombrir l'icône en gardant ses couleurs. Il faut vraiment désaturer les
// pixels, donc une seconde texture, construite une fois puis mémorisée — échec
// de chargement compris, sinon on retenterait la résolution à chaque frame.
//
// ⚠ Ces textures vivent en D3DPOOL_DEFAULT : elles MEURENT à un reset de device
// (ALT-TAB en plein écran). D'où le cache vidé sur changement d'epoch — sans
// quoi on dessinerait des handles morts.
ro::IconTex GrayItemIcon(uint32_t nameid) {
  static std::unordered_map<uint32_t, ro::IconTex> cache;
  static unsigned cache_epoch = 0;
  const unsigned epoch = Overlay_DeviceEpoch();
  if (epoch != cache_epoch) { cache.clear(); cache_epoch = epoch; }
  auto it = cache.find(nameid);
  if (it != cache.end()) return it->second;

  ro::IconTex gray;
  std::vector<uint8_t> pixels;
  int w = 0, h = 0;
  if (ro::ItemIconPixels(nameid, &pixels, &w, &h) && w > 0 && h > 0) {
    // Pixels en B,G,R,A. Luminance perceptuelle (Rec. 601, en entiers) : une
    // moyenne plate rendrait les rouges trop clairs et les bleus trop sombres.
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
      const int lum = (pixels[i + 2] * 77 + pixels[i + 1] * 150 + pixels[i] * 29) >> 8;
      pixels[i] = pixels[i + 1] = pixels[i + 2] = static_cast<uint8_t>(lum);
    }
    gray.tex = Overlay_CreateTextureARGB(pixels.data(), w, h);
    gray.w = w;
    gray.h = h;
  }
  cache[nameid] = gray;
  return gray;
}

}  // namespace

// ── Paquets de la session « storage » ──────────────────────────────────────
//
// ZC_INVENTORY_START (0x0b08, variable) : [len:2][invType:1][name:≤24] à partir
// de +2. C'est l'OUVERTURE, et elle est MULTIPLEXÉE — invType 0 = inventaire,
// 1 = cart, 2 = storage, 3 = storage de guilde. On ne revendique donc que
// 2 et 3, sinon on tuerait l'ouverture des deux autres fenêtres.
constexpr uint16_t kOpInventoryStart = 0x0b08;
constexpr int      kInvTypeStorage   = 2;  // e_inventory_type INVTYPE_STORAGE
constexpr int      kInvTypeGuildStorage = 3;
// ZC_NOTIFY_STOREITEM_COUNTINFO (0x00f2, fixe 6) : [amount:2][max:2] à partir de
// +2. Le compteur « 114/600 ». Son handler natif appelle MakeWindow(0x21) et
// déréférence le retour sans test : c'est un SECOND créateur de la fenêtre, à
// remplacer sous peine de la voir renaître aussitôt après l'ouverture.
constexpr uint16_t kOpStoreCount = 0x00f2;
// ZC_CLOSE_STORE (0x00f8, fixe 2) : la fermeture. On l'OBSERVE seulement — son
// handler natif détruit la fenêtre (inoffensif, elle n'existe pas) mais VIDE
// aussi le modèle de session, et ce vidage-là, nous le voulons : sans lui les
// items du précédent storage resteraient et se mélangeraient au suivant.
constexpr uint16_t kOpStoreClose = 0x00f8;
// Changement de map : le serveur ferme le storage sans le dire (cf. CloseLocal).
constexpr uint16_t kOpMapChange  = 0x0091;  // ZC_NPCACK_MAPMOVE
constexpr uint16_t kOpServerMove = 0x0092;  // ZC_NPCACK_SERVERMOVE

StorageWindow::StorageWindow() {
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kStoragePrices);
  // Liste des storages accessibles : écoutée MÊME en mode natif. Le serveur
  // l'envoie à chaque ouverture quelle qu'en soit l'origine ; ne pas l'écouter
  // ne changerait rien au natif, mais l'écouter garantit que le jour où le
  // joueur rebascule en ImGui la liste est déjà là. Aucun effet de bord : ce
  // handler ne fait que remplir un tableau.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kStorageList);
  // Ouverture : revendiquée seulement pour un ENTREPÔT, et seulement en mode
  // ImGui. Le prédicat lit l'invType dans le paquet — même offset que le
  // décodage plus bas, il n'y a qu'une lecture à tenir juste.
  Bourgeon::Instance().RegisterReplaceOpcode(
      kOpInventoryStart, [this](const uint8_t* data, uint16_t len) {
        if (!imgui_enabled_ || !data || len < 3) return false;
        return data[2] == kInvTypeStorage || data[2] == kInvTypeGuildStorage;
      });
  // Compteur : revendiqué dès que le mode ImGui est actif. Il ne concerne que
  // le storage, donc pas de champ à discriminer.
  Bourgeon::Instance().RegisterReplaceOpcode(kOpStoreCount,
                                             [this] { return imgui_enabled_; });
  Bourgeon::Instance().RegisterObserveOpcode(kOpStoreClose, 2);
  Bourgeon::Instance().RegisterObserveOpcode(kOpMapChange, 4);
  Bourgeon::Instance().RegisterObserveOpcode(kOpServerMove, 4);
}

// Prix de vente NPC du storage (ZC_BOURGEON_STORAGE_PRICES). data = payload après
// [op:2][len:2] : [count:2] puis count * [id:4][sell:4].
// Fil RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h). Tous ces
// paquets se décodent dans les octets transmis — Push suffit.
void StorageWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

// Vide tout ce qui décrit le CONTENU du storage courant. Ne touche NI à la
// fenêtre (position, taille, onglet de catégorie) NI aux réglages : c'est
// exactement ce qu'il faut jeter quand on passe d'un storage à un autre.
// `prices_` / `meta_` sont exclus À DESSEIN — ce sont des données d'itemdb
// (prix de revente, slots, type d'arme), identiques quel que soit le storage.
void StorageWindow::ClearStorageData() {
  item_count_ = 0;
  used_ = max_ = 0;
  pend_id_ = 0;         // une action en attente n'a plus de destinataire
  pend_open_prompt_ = false;
  drag_active_ = false;
  hover_desc_id_ = 0;
  hover_desc_idx_ = -1;
  storage_name_[0] = '\0';
  reset_view_ = true;   // filtre par nom + sous-catégorie (cf. reset_view_)
}

// Ferme la session côté client. N'envoie RIEN : soit le serveur vient de nous
// dire qu'il a fermé, soit il a fermé en silence (warp).
void StorageWindow::CloseLocal() {
  open_ = false;
  show_panel_ = true;   // le viewer doit réapparaître à la prochaine ouverture
  switching_ = false;   // une bascule qui n'aboutira plus
  cur_storage_id_ = -1;
  ClearStorageData();
}

// Demande au serveur d'ouvrir (ou de basculer vers) le storage `id`. Le serveur
// ferme le courant puis ouvre le demandé ; nous, on note juste qu'une bascule
// est en vol pour que la fermeture intermédiaire ne referme pas le viewer.
void StorageWindow::SendOpenStorage(uint8_t id) {
  uint8_t pkt[5];
  *reinterpret_cast<uint16_t*>(pkt + 0) = bopcodes::kOpenStorage;
  *reinterpret_cast<uint16_t*>(pkt + 2) = sizeof(pkt);
  pkt[4] = id;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  switching_ = true;
  switch_target_ = id;
  switch_tick_ = GetTickCount();
  // Purge IMMÉDIATE : la liste affichée n'est déjà plus celle du storage
  // demandé. Ce que le modèle natif contient encore sera vidé par le ZC 0x00f8
  // de la fermeture serveur ; ici on coupe juste le rendu d'une liste périmée.
  ClearStorageData();
}

// Fil PRINCIPAL : le décodage, rejoué en phase d'entrée, dans l'ordre d'arrivée.
void StorageWindow::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  // OUVERTURE (revendiquée) : data = [len:2][invType:1][name:≤24]. Le prédicat a
  // déjà filtré l'invType ; ici on prend le nom, qui devient le titre du viewer.
  if (opcode == kOpInventoryStart) {
    if (len < 4) return;
    const char* name = reinterpret_cast<const char*>(data + 3);
    size_t i = 0;
    const size_t cap = sizeof(storage_name_) - 1;
    while (i < cap && i + 3 < len && name[i]) { storage_name_[i] = name[i]; ++i; }
    storage_name_[i] = '\0';
    // C'est NOTRE ouverture : l'état ne se déduit plus de la présence d'une
    // fenêtre native, qui ne naîtra pas.
    if (!open_) need_pos_ = true;
    open_ = true;
    show_panel_ = true;
    // Fin de bascule : le storage demandé est arrivé. `cur_storage_id_` n'est
    // pas posé ici — il vient de ZC_BOURGEON_STORAGE_LIST, envoyé dans la
    // foulée par le serveur, qui est le seul à savoir SUR QUOI on a atterri
    // (le nom ne suffit pas : deux storages peuvent porter le même).
    switching_ = false;
    return;
  }
  // COMPTEUR (revendiqué) : [amount:2][max:2]. Le natif l'écrivait dans la
  // fenêtre (+0x188/+0x18c), d'où on le relisait ; on le prend à la source.
  if (opcode == kOpStoreCount) {
    if (len < 4) return;
    used_ = *reinterpret_cast<const uint16_t*>(data);
    max_  = *reinterpret_cast<const uint16_t*>(data + 2);
    return;
  }
  // FERMETURE (observée) : le handler natif a déjà vidé le modèle de session.
  // 🔴 Pendant une BASCULE, cette fermeture est une ÉTAPE, pas une fin : c'est
  // le serveur qui ferme le storage courant avant d'ouvrir le suivant, et c'est
  // même précisément ce qu'on veut (c'est ce vidage qui empêche les deux
  // contenus de se mélanger). On garde donc la fenêtre ouverte et on n'oublie
  // que le contenu.
  if (opcode == kOpStoreClose) {
    if (switching_) ClearStorageData();
    else            CloseLocal();
    return;
  }
  // Changement de map : fermeture SILENCIEUSE côté serveur (cf. CloseLocal).
  if (opcode == kOpMapChange || opcode == kOpServerMove) {
    if (open_) CloseLocal();
    return;
  }
  // ZC_BOURGEON_STORAGE_LIST : [cur_id:1][count:1] puis count * [id:1][name:24].
  // `cur_id` = 0xFF quand aucun storage n'est ouvert (envoi au login).
  // REMPLACE la liste (pas de merge) : les droits peuvent changer en cours de
  // session (@adjgroup), et un onglet qui survivrait à sa permission enverrait
  // une demande que le serveur refuserait, sans que le joueur comprenne.
  if (opcode == bopcodes::kStorageList) {
    if (len < 2) return;
    const uint8_t cur = data[0];
    int count = data[1];
    if (count > kMaxStorageTabs) count = kMaxStorageTabs;
    stg_tab_count_ = 0;
    size_t off = 2;
    for (int i = 0; i < count && off + 25 <= len; ++i, off += 25) {
      StorageTab& tab = stg_tabs_[stg_tab_count_];
      tab.id = data[off];
      const char* name = reinterpret_cast<const char*>(data + off + 1);
      size_t n = 0;
      while (n < sizeof(tab.name) - 1 && n < 24 && name[n]) { tab.name[n] = name[n]; ++n; }
      tab.name[n] = '\0';
      ++stg_tab_count_;
    }
    cur_storage_id_ = (cur == 0xFF) ? -1 : static_cast<int>(cur);
    return;
  }
  // ZC_BOURGEON_STORAGE_PRICES : [count:2] puis
  // count * [id:4][sell:4][subtype:1][equip:4][slots:2].
  if (opcode != bopcodes::kStoragePrices || len < 2) return;
  const int16_t count = *reinterpret_cast<const int16_t*>(data);
  // MERGE (pas de clear) : subtype/equip/slots/prix sont statiques (itemdb) -> on
  // accumule ; ainsi une MAJ 1-item (ajout au storage) n'efface pas les métas.
  size_t off = 2;
  for (int i = 0; i < count && off + 15 <= len; ++i) {
    const uint32_t id      = *reinterpret_cast<const uint32_t*>(data + off);
    const uint32_t sell    = *reinterpret_cast<const uint32_t*>(data + off + 4);
    const uint8_t  subtype = data[off + 8];
    const uint32_t equip   = *reinterpret_cast<const uint32_t*>(data + off + 9);
    const uint16_t slots   = *reinterpret_cast<const uint16_t*>(data + off + 13);
    prices_[id] = sell;
    meta_[id] = ItemMeta{subtype, equip, slots};
    off += 15;
  }
}

// Remplit items_/item_count_ depuis le MODÈLE COMPLET (g_session+0x1718). Ce
// modèle est peuplé par les paquets de liste (0x0b09/0x0b39) sans passer par la
// fenêtre — leur ingesteur ne touche à g_StorageWnd_ptr que sous un test de
// nullité. C'est précisément ce qui permet de tuer la native. POD-only sous SEH.
void StorageWindow::Extract() {
  item_count_ = 0;
  __try {
    // 0x015fbad8 = g_session+0x1718 : sentinelle de la std::list storage complète.
    uint8_t* head = *reinterpret_cast<uint8_t**>(0x015fbad8);
    if (!head) return;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);  // 1er nœud
    int guard = 0;
    while (node && node != head && item_count_ < kMaxItems && guard < kMaxItems) {
      Item& it = items_[item_count_];
      uint8_t* info = node + kNodeInfo;  // node+8 = ItemSkillInfo
      // id : lu de la std::string à info+0x2c (là où le jeu le lit pour l'icône).
      const uint32_t idcap = *reinterpret_cast<uint32_t*>(info + kInfoIdCap);
      const char* ids = (idcap > 0xf)
                            ? *reinterpret_cast<char**>(info + kInfoIdStr)
                            : reinterpret_cast<const char*>(info + kInfoIdStr);
      it.id = ids ? static_cast<uint32_t>(atoi(ids)) : 0;
      it.identified = *reinterpret_cast<uint8_t*>(info + kInfoIdent);
      it.amount = *reinterpret_cast<int*>(node + kNodeAmt);
      it.index = *reinterpret_cast<int*>(info + 4);  // storage index (arg du retrait)
      it.type  = *reinterpret_cast<int*>(info);      // info+0 = type (onglets)
      // Cartes/enchants (info+0x1c, 4 x uint32) et random options (compte à +0x98,
      // entrées de 5 octets à +0x9c) : données d'INSTANCE du stack, pas de la DB.
      // Mêmes offsets que ceux utilisés par la fenêtre de description native.
      for (int k = 0; k < 4; ++k)
        it.cards[k] = *reinterpret_cast<uint32_t*>(info + 0x1c + k * 4);
      it.refine = *reinterpret_cast<int*>(info + 0x60);  // niveau de refine (aperçu)
      it.damaged = *reinterpret_cast<uint8_t*>(info + 0x5d);  // cassé (rendu rouge)
      int nopt = *reinterpret_cast<int*>(info + 0x98);
      if (nopt < 0) nopt = 0;
      if (nopt > 5) nopt = 5;
      it.opt_count = nopt;
      for (int k = 0; k < nopt; ++k) {
        const uint8_t* e = info + 0x9c + k * 5;
        it.opts[k].index = *reinterpret_cast<const int16_t*>(e);
        it.opts[k].value = *reinterpret_cast<const int16_t*>(e + 2);
        it.opts[k].param = e[4];
      }
      // Nom COMPLET (refine/cartes/enchant), via la brique partagée : elle porte
      // son PROPRE SEH, par item, ce qui compte ici — le __try d'Extract couvre
      // toute la boucle, donc un seul item fautif y ferait disparaître tous les
      // suivants (la leçon de l'inventaire, cf. features/item_cell.cc).
      // Le 1er argument n'est lu que sur une branche « nom du forgeron pas encore
      // résolu », où le jeu le traite comme une liste de demandes en attente : on
      // passe le gestionnaire de fenêtres, comme weapon_refine_window, et non une
      // fenêtre storage qui n'existe plus.
      itemcell::BuildDisplayName(info, it.name, sizeof(it.name));
      ++item_count_;
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
      ++guard;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // laisse item_count_ à ce qui a été extrait avant la faute
  }
}

void StorageWindow::OnTick() {
  // `open_` n'est PLUS déduit de la présence de la fenêtre native : elle ne naît
  // plus. Il est posé par le paquet d'ouverture et levé par CloseLocal.
  //
  // 🔴 Basculer de mode PENDANT une session ouverte est le cas qui mord : la
  // fenêtre qui affichait le storage disparaît, et l'autre ne s'ouvre pas de son
  // propre chef (le paquet d'ouverture est passé). Le joueur se retrouverait
  // sans rien devant une session bien vivante côté serveur — qui lui bloque au
  // passage inventaire <-> cart. Les deux sens sont donc traités.
  const bool mode_changed = (imgui_enabled_ != prev_imgui_enabled_);
  prev_imgui_enabled_ = imgui_enabled_;

  if (!imgui_enabled_) {
    // Mode natif : la fenêtre native fait tout, on ne rend rien. `open_` reste
    // faux — les prédicats ayant rendu la main, aucun paquet ne nous parvient.
    // Retour au natif alors que NOTRE session était ouverte : le natif n'a pas
    // de fenêtre à reprendre (on a empêché sa naissance) et n'en recevra plus
    // l'ordre. Plus personne ne pourrait fermer la session : on la ferme.
    if (mode_changed && open_) SendCloseStorage();
    open_ = false;
    return;
  }
  // Filet : si une fenêtre 0x21 existe malgré tout, on la DÉTRUIT. Cinq cases du
  // dispatcher la créent encore (les listes storage des packetvers anciens, que
  // ce serveur n'envoie pas), et la bascule vers l'ImGui peut en trouver une
  // déjà ouverte. La masquer ne suffirait pas — une native cachée garde le
  // clavier et son bouton par défaut répond à Entrée/Espace.
  if (uiwnd::SafeFindWindow(kWinStorage)) {
    // Sa présence PROUVE qu'une session est ouverte : on l'adopte avant de la
    // détruire. Le nom et le compteur manqueront jusqu'au prochain paquet — le
    // titre retombe sur « Storage » et le compteur sur 0/0, le temps d'un
    // mouvement d'item.
    if (!open_) { open_ = true; need_pos_ = true; show_panel_ = true; }
    uiwnd::SafeCloseWindow(kWinStorage);
  }

  // BASCULE en vol : on ne lit PAS le modèle. Entre la demande et l'arrivée du
  // nouveau storage, il contient encore l'ancien contenu (jusqu'au ZC 0x00f8 qui
  // le vide) — l'afficher donnerait des items qu'on ne peut plus manipuler, et
  // un clic dessus enverrait un index appartenant à un autre storage.
  // Garde-fou : un storage alternatif se charge de façon ASYNCHRONE (le serveur
  // interroge le char-server), et un refus tardif ou un char-server muet
  // laisserait le viewer figé sur « Chargement… » pour toujours.
  if (switching_ && GetTickCount() - switch_tick_ > kSwitchTimeoutMs) {
    switching_ = false;
    CloseLocal();
    return;
  }
  if (open_ && !switching_) Extract();
  // Suffixe [N] (nb de slots de carte, meta serveur) sur le nom, hors SEH d'Extract.
  // BuildDisplayName n'affiche pas le compte de slots -> on l'ajoute si l'item a des
  // slots et que le nom n'a pas déjà de crochet (évite tout double [N]/enchant).
  if (open_) {
    for (int i = 0; i < item_count_; ++i) {
      auto it = meta_.find(items_[i].id);
      if (it == meta_.end() || it->second.slots == 0) continue;
      if (std::strchr(items_[i].name, '[')) continue;
      char suf[8];
      std::snprintf(suf, sizeof(suf), " [%u]", it->second.slots);
      const size_t cur = std::strlen(items_[i].name);
      if (cur + std::strlen(suf) < sizeof(items_[i].name))
        std::strcat(items_[i].name, suf);
    }
  }
  // L'aperçu de description est une fenêtre ImGui à nous : elle disparaît d'elle-même
  // dès qu'on ne la dessine plus. On oublie juste l'item survolé quand le viewer ne
  // rend plus (storage fermé, viewer désactivé, option décochée), sinon l'aperçu
  // resurgirait tel quel à la réouverture.
  if (!open_ || !show_desc_tooltip_) hover_desc_id_ = 0;
}

// (Le pont vers le DRAG NATIF entrant — HandleNativeDrop, OnMouseDown, la lecture
// du payload et le redessin de l'icône — a été retiré. Il n'existait que pour un
// drag parti d'un inventaire ou d'un cart NATIFS ; or « Interface moderne » est
// un groupe tout-ou-rien, donc quand ce viewer est actif ces deux fenêtres sont
// des viewers ImGui et leurs natives sont masquées, hors rendu ET hors hit-test.
// Aucun drag natif ne peut donc plus en partir : les deux sens passent par le
// glisser ImGui.)

// ── Verrou « description en vol » (anti-flicker de l'aperçu au survol) ───────
// Armé au moment où l'utilisateur DEMANDE une description (menu contextuel ou
// Ctrl+clic droit) : la fenêtre de description met quelques frames à apparaître,
// et pendant ce trou le curseur est de nouveau sur la ligne -> l'aperçu au survol
// se rouvrait puis disparaissait (flicker). Le verrou tient jusqu'au prochain
// VRAI mouvement du curseur (le geste souris/menu est alors terminé), avec un
// garde-fou de temps au cas où la fenêtre n'arriverait jamais.
void StorageWindow::MarkDescPending() {
  const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
  desc_pending_ = true;
  desc_pending_x_ = mouse_pos.x;
  desc_pending_y_ = mouse_pos.y;
  desc_pending_tick_ = GetTickCount();
}

bool StorageWindow::DescPendingBlocksHover() {
  if (!desc_pending_) return false;
  constexpr float kMoveThreshold = 6.0f;   // px : ignore le micro-jitter de la souris
  constexpr uint32_t kMaxHoldMs = 1500;    // garde-fou : jamais bloqué indéfiniment
  const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
  const bool moved = std::fabs(mouse_pos.x - desc_pending_x_) +
                     std::fabs(mouse_pos.y - desc_pending_y_) > kMoveThreshold;
  if (moved || GetTickCount() - desc_pending_tick_ > kMaxHoldMs)
    desc_pending_ = false;
  return desc_pending_;
}

// ── Section « StorageWindow » du panneau Moonlight ───────────────────────────
// Déplacée depuis moonlight_ui/panel_interface.cc : ces widgets ne pilotent
// que l'état de CE plugin. MoonlightUi ne garde que l'appel et la décision
// de sauvegarder. Rend true si un réglage a changé.
bool StorageWindow::DrawSettings() {
  bool changed = false;
  // Fenêtre membre du groupe « Interface moderne » (tout-ImGui ou tout-natif, plus
  // de mixe) : `SetModernInterface` écrit `imgui_enabled_` avec les autres. Ce qui
  // suit ne dit que ce que la bascule change pour le storage.
  // 🔴 Plus de CASE ici (cf. skill_bar.cc et moonlight_ui.h) : l'interrupteur du
  // groupe est unique, en tête de « Interface de jeu ». On garde la DESCRIPTION.
  ImGui::TextDisabled("Fenêtre du groupe « Interface moderne »");
  SameLine(); HelpMarker(
      "ON : storage ImGui moderne (icônes, onglets, tri, drag-drop). La "
      "fenêtre native ne s'ouvre plus du tout.\nOFF : storage natif "
      "classique, aucun viewer.");

  ImGui::BeginDisabled(!imgui_enabled_);

  changed |= ro::RoCheckbox("Description au survol", &desc_tooltip());
  SameLine(); HelpMarker(
      "Survoler un item affiche un aperçu SIMPLIFIÉ (nom, illustration, "
      "texte) dans un panneau au skin RO, posé au curseur et effacé dès "
      "que la souris quitte la ligne.\n"
      "La description COMPLÈTE reste accessible au Ctrl + clic droit / "
      "menu contextuel.");

  changed |= ro::RoCheckbox("Onglets verticaux (à gauche)", &tabs_vertical());
  SameLine(); HelpMarker(
      "Dispose les catégories en liste verticale à gauche, comme la "
      "fenêtre native.\nOFF (défaut) : onglets horizontaux en haut.\n"
      "Décide aussi de l'orientation des onglets de storage, qui restent "
      "toujours PERPENDICULAIRES aux catégories.");

  // Sans onglets de catégorie, il n'y a plus de tuile à dessiner : la case est
  // grisée plutôt que laissée cliquable sans effet.
  ImGui::BeginDisabled(!show_type_tabs_);
  changed |= ro::RoCheckbox("Images d'onglet", &tab_images());
  SameLine(); HelpMarker(
      "ON : tuiles images du client — jeu tab_* en disposition verticale, "
      "tabh_* en horizontale (all/use/wea/ammo/card/fav/cash/cos/etc). Les "
      "catégories sans art propre réutilisent celui de leur famille et "
      "portent alors un sigle (Am, Cs, Et).\n"
      "OFF : onglets texte — TabBar classique en horizontal, libellé écrit "
      "à la verticale en vertical.");
  ImGui::EndDisabled();

  changed |= ro::RoCheckbox("Champ de filtre", &show_filter());
  SameLine(); HelpMarker(
      "Affiche la barre de recherche par nom au-dessus de la liste.\n"
      "Décoche pour gagner une ligne (le filtre est alors vidé).");

  changed |= ro::RoCheckbox("Filtres par type d'item", &show_type_tabs());
  SameLine(); HelpMarker(
      "Affiche les onglets de catégorie (Tout, Favoris, Consos, Armes...) et "
      "le combo « Sous-type » qui en dépend.\n"
      "Décoche pour une liste unique, sans filtre de type : la vue repasse à "
      "« Tout » (rien ne reste masqué derrière) et « Images d'onglet » n'a "
      "plus d'objet. « Onglets verticaux », lui, continue de décider de "
      "l'orientation des onglets de STORAGE.\n"
      "Les favoris restent marquables (Ctrl + clic gauche, menu contextuel) : "
      "seul l'onglet qui les isole disparaît.");

  changed |= ro::RoCheckbox("Onglets de storage", &show_storage_tabs());
  SameLine(); HelpMarker(
      "Affiche un onglet par entrepôt auquel vous avez accès (principal et "
      "alternatifs) : un clic bascule de l'un à l'autre sans repasser par "
      "@storage / @storagealtN.\n"
      "Les onglets se placent PERPENDICULAIREMENT à ceux des catégories — "
      "en colonne à gauche si les catégories sont horizontales, en rangée en "
      "haut si elles sont verticales.\n"
      "La liste vient du serveur : elle ne montre que ce que votre compte a le "
      "droit d'ouvrir, et n'apparaît qu'à partir de deux entrepôts.\n"
      "Chaque onglet porte son NUMÉRO par défaut ; clic droit dessus pour lui "
      "donner un nom à vous et une icône d'item (ou glissez-y un item du "
      "storage pour reprendre la sienne).");

  changed |= ro::RoCheckbox("Valeur estimée du storage", &show_total_value());
  SameLine(); HelpMarker(
      "Somme des prix de revente NPC (× quantité) des items AFFICHÉS "
      "— elle suit donc l'onglet, le sous-type et le filtre.");

  SeparatorText("Colonnes");
  changed |= ro::RoCheckbox("Index", &show_index_col());
  SameLine(); HelpMarker(
      "Index storage (slot) — un item récemment ajouté a un index élevé.");
  changed |= ro::RoCheckbox("ID d'item", &show_id_col());
  SameLine(); HelpMarker("Colonne avec l'id numérique de l'item.");
  changed |= ro::RoCheckbox("Slots", &show_slots_col());
  SameLine(); HelpMarker("Colonne avec le nombre de slots de carte.");
  changed |= ro::RoCheckbox("Prix de revente", &show_value_col());
  SameLine(); HelpMarker(
      "Colonne avec le prix de revente NPC × la quantité du stack.");

  ImGui::EndDisabled();

  // ── Tri serveur (mêmes combos que « Commands Settings ») ────────────────────
  // HORS du BeginDisabled ci-dessus : ce n'est pas un réglage du viewer mais un
  // réglage SERVEUR (@tri_storage / @tri_gstorage), qui vaut aussi pour la
  // fenêtre native. Il s'applique à la PROCHAINE ouverture du storage (le
  // serveur trie au moment où il envoie la liste).
  // Pas de `changed` : l'état vit dans MoonlightUi et le serveur en est la source
  // (aucun réglage yaml de CE plugin n'a bougé).
  SeparatorText("Tri serveur");
  ImGui::BeginDisabled(imgui_enabled_);
  if (auto* mu = Bourgeon::Instance().moonlight_ui()) {
    mu->DrawSortModeCombo(MoonlightUi::kSortStorage);
    mu->DrawSortModeCombo(MoonlightUi::kSortGuildStorage);
  }
  ImGui::EndDisabled();

  return changed;
}

void StorageWindow::OnRenderUI() {
  if (!open_ || !imgui_enabled_) return;

  if (need_pos_) {
    // FirstUseEver (pas Appearing) : ce n'est qu'un DÉFAUT pour la toute 1re
    // ouverture. Ensuite ImGui garde la position déplacée par le joueur (en
    // session + persistée dans imgui.ini via l'id stable ###bourgeon_storage) ;
    // les appels suivants sont des no-op tant qu'une position existe déjà.
    //
    // Ce défaut se lisait avant sur la fenêtre native, qui ne naît plus. On
    // reprend donc SA position de création (700, 85), rabattue dans l'écran pour
    // ne pas naître hors champ sur une petite résolution.
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float x = std::min(kSpawnX, std::max(0.0f, screen.x - kSpawnW));
    const float y = std::min(kSpawnY, std::max(0.0f, screen.y - kSpawnH));
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
  // Plancher de redimensionnement : sous ~200px la table, les onglets et le footer
  // n'ont plus de place (colonnes écrasées, strip d'onglets coupé).
  ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 420.0f),
                                      ImVec2(FLT_MAX, FLT_MAX));

  // Titre = nom du storage envoyé par le serveur (ZC_INVENTORY_START), ex.
  // "Storage" / "Guild Storage" / nom premium. Repli "Storage" si pas encore reçu.
  // L'id ImGui (###) reste stable -> position/taille persistent malgré le nom variable.
  char title[64];
  std::snprintf(title, sizeof(title), "%s###bourgeon_storage",
                storage_name_[0] ? storage_name_ : "Storage");
  // Bullet de la barre de titre = raccourci vers la config de CETTE fenêtre
  // (panneau Moonlight > Interface de jeu > Storage).
  ro::SetNextWindowTitleBullet("Options du storage");
  // Corps = couleur « fenêtre de liste » du skin (blanc pur par défaut), distincte
  // du corps général pour que la liste d'items se lise bien. Réglable dans
  // Moonlight > Interface de jeu > Skin RO.
  ro::SetNextWindowBodyColor(ro::ListBodyColorU32());
  const bool begun =
      ro::BeginRoWindow(title, &show_panel_, ImGuiWindowFlags_NoCollapse);
  if (ro::TitleBulletClicked())
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(MoonlightUi::kIfaceStorage);
  // Le X du viewer a été cliqué ce frame (show_panel_ était vrai à l'entrée, cf. le
  // early-return en tête) -> on FERME le storage côté serveur (CZ_CloseKafra). Le
  // serveur ferme la session -> native + viewer se ferment (open_ passe à false au
  // prochain OnTick). On remet show_panel_ à true pour que le viewer réapparaisse à
  // la prochaine ouverture (le masquage effectif vient de open_, pas de show_panel_).
  if (!show_panel_) {
    SendCloseStorage();
    show_panel_ = true;
  }
  if (!begun) {
    ro::EndRoWindow();
    return;
  }

  // Bandeau : pendant la composition d'un shop, TOUS les mouvements de cette
  // fenêtre sont bloqués (cf. VendingComposing). Les émetteurs refusent déjà,
  // mais un refus muet passerait pour un bug — on le dit une fois, en clair,
  // plutôt que de griser une trentaine de cases et de cibles de glisser.
  if (VendingComposing())
    ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                       "Shop en composition : les transferts sont figés.");

  // ── Onglets de STORAGE (opt-in) ─────────────────────────────────────────────
  // Ils basculent d'un entrepôt à l'autre — principal, alternatifs — sans passer
  // par @storage / @storagealtN. La liste ET les droits viennent du serveur
  // (ZC_BOURGEON_STORAGE_LIST) : le client ne connaît aucun id ni aucun nom à
  // l'avance. En dessous de DEUX entrées il n'y a aucun choix à offrir, donc rien
  // n'est dessiné — inutile de voler une ligne (ou une colonne) à la liste.
  //
  // ORIENTATION : toujours PERPENDICULAIRE aux onglets de catégorie. Deux
  // rangées d'onglets parallèles et d'apparence proche se confondent ; avec des
  // axes différents, on lit d'un coup d'œil laquelle change de storage et
  // laquelle filtre par type.
  //
  // Déclaré ICI, avant les émetteurs d'onglets : ceux des CATÉGORIES en ont
  // besoin aussi (leur rangée horizontale s'indente derrière le strip de
  // storage), et une lambda [&] ne peut capturer qu'une variable déjà nommée.
  const bool storage_tabs_on = show_storage_tabs_ && stg_tab_count_ > 1;

  // Disposition VERTICALE des catégories RÉELLEMENT émise. Les onglets de type
  // sont désactivables (setting « Filtres par type ») ; sans ce booléen, la
  // table réserverait la place d'un child jamais ouvert et le EndChild final
  // fermerait la fenêtre elle-même.
  const bool vertical_cats = tabs_vertical_ && show_type_tabs_;
  // Le strip de storage est-il posé à gauche de la table ? (Il l'est quand les
  // catégories sont horizontales — orientations perpendiculaires.)
  const bool storage_strip_left = storage_tabs_on && !tabs_vertical_;

  // 🔴 Style relevé AVANT tout push. Les strips d'onglets écrasent
  // `WindowPadding` (0,0) et `ItemSpacing` (-1,0) pour coller leurs tuiles — et
  // le menu contextuel d'un onglet est émis DEDANS, donc il héritait de ces
  // valeurs : contenu collé aux bords, lignes jointives. Il faut les vraies,
  // relevées ici plutôt que codées en dur, pour que le popup suive le skin.
  const ImVec2 popup_padding = ImGui::GetStyle().WindowPadding;
  const ImVec2 popup_spacing = ImGui::GetStyle().ItemSpacing;

  // Rect écran du viewer (pour tester le drop d'un drag natif dessus).
  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  win_x_ = wp.x; win_y_ = wp.y; win_w_ = ws.x; win_h_ = ws.y;
  win_valid_ = true;

  // Action en attente (menu contextuel ou fin d'un glisser partant d'ici) : 1 seul
  // = direct ; pile = prompt quantité. do_move applique le sens choisi. Les deux
  // sens sont SORTANTS — ce qui ENTRE dans le storage est envoyé par la fenêtre
  // d'où l'objet part (inventaire, cart), chacune émettant son propre paquet.
  auto do_move = [this](int amount) {
    switch (pend_action_) {
      case kPendStoToCart: SendStorageToCart(pend_index_, amount); break;
      default:             WithdrawItem(pend_index_, amount); break;  // kPendStoToInv
    }
  };
  if (pend_id_ != 0) {
    if (pend_open_prompt_) {
      ro::OpenQuantityPrompt(this);
      pend_open_prompt_ = false;
    } else if (pend_max_ <= 1) {
      do_move(1);  // 1 seul item -> direct
      pend_id_ = 0;
    }
  }
  // Dialogue « combien ? » PARTAGÉ (ui/qty_prompt) : habillé RO, identique dans
  // l'inventaire, le storage et le cart.
  {
    // Destination NOMMÉE des deux côtés : « Retirer » ne disait pas où l'objet
    // partait, alors que le cart est une destination tout aussi légitime — même
    // formulation que les entrées du menu contextuel.
    const char* verb =
        pend_action_ == kPendStoToCart ? "Vers le cart" : "Vers l'inventaire";
    bool cancelled = false;
    const int qty = ro::QuantityPrompt(this, verb, pend_max_, &cancelled);
    if (qty > 0) { do_move(qty); pend_id_ = 0; }
    else if (cancelled) pend_id_ = 0;
  }

  // Onglets de catégorie (filtre par type d'item). Changer d'onglet remet la
  // sous-catégorie à Tout (les clés diffèrent d'un onglet à l'autre).
  // L'onglet vient du yaml (persisté) : borné avant tout indexage de kStgCats.
  if (cur_tab_ < 0 || cur_tab_ >= kNumStgCats) cur_tab_ = 0;

  // Sélection d'un onglet (commune aux deux dispositions) : remet la sous-catégorie
  // à Tout (les clés diffèrent d'un onglet à l'autre) et persiste le nouvel onglet.
  auto select_tab = [this](int c) {
    if (cur_tab_ == c) return;
    cur_sub_ = -1;
    cur_tab_ = c;
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  };

  // Cible de dépôt d'un onglet (les DEUX dispositions) : lâcher un item du viewer
  // sur l'onglet « Favoris » l'y AJOUTE ; le lâcher sur n'importe quel AUTRE onglet
  // l'en RETIRE (même geste que l'inventaire natif / inventory_viewer). À appeler
  // juste après l'émission de l'onglet, tant qu'il est le « dernier item » ImGui.
  auto tab_drop_target = [this](int c) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("STG_ITEM")) {
      const int di = *static_cast<const int*>(pl->Data);
      if (di >= 0 && di < item_count_ && items_[di].id != 0) {
        // Ajout/retrait EXPLICITES (pas ToggleFavorite) : relâcher un favori sur
        // « Favoris » doit le laisser favori, pas le débrancher.
        if (kStgCats[c].fav) favorites_.insert(items_[di].id);
        else                 favorites_.erase(items_[di].id);
        if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
      }
    }
    ImGui::EndDragDropTarget();
  };

  // Le footer RO est épinglé en bas : la table (ou, en disposition VERTICALE, les
  // deux colonnes liste d'onglets + corps) doit lui laisser sa place.
  const float kFooterH = 21.0f;
  const float body_h = -(kFooterH + ImGui::GetStyle().ItemSpacing.y);

  // Rect (écran) de l'onglet ACTIF : sert au « pont » qui mange le bord entre le
  // strip et le contenu (souligne l'onglet actif), comme inventory_viewer.
  ImVec2 active_tab_min(0, 0), active_tab_max(0, 0);
  bool   have_active_tab = false;
  // Idem pour l'onglet de STORAGE actif, mais sur son bord BAS. Les deux pixels
  // d'air qui le séparent du tableau sont déjà blancs (le corps de la fenêtre
  // EST la couleur de liste, cf. SetNextWindowBodyColor) : il ne reste donc qu'à
  // manger la BORDURE HAUTE du tableau sous cet onglet pour que le blanc coule
  // sans rupture de l'un à l'autre.
  ImVec2 stg_tab_min(0, 0), stg_tab_max(0, 0);
  bool   have_stg_tab = false;
  // Le sélecteur est une rangée EN HAUT ou un strip À GAUCHE : le pont vise donc
  // la bordure haute du tableau dans un cas, sa bordure gauche dans l'autre.
  bool   stg_tab_is_strip = false;
  // Coin haut-gauche du tableau, relevé juste avant BeginTable : les onglets ne
  // le touchent pas (4 px d'air, plus la marge du child « corps » en disposition
  // verticale), le pont ne peut donc pas se déduire de leur seule position.
  float  table_top_y = 0.0f, table_left_x = 0.0f;

  // Strip VERTICAL : émis non pas ici mais juste avant la table (cf. appel plus bas),
  // pour que le haut des onglets s'aligne sur l'EN-TÊTE DU TABLEAU et suive donc le
  // filtre et/ou la valeur estimée quand ils sont affichés. Conséquence assumée : la
  // sélection d'onglet est lue avant d'être (re)dessinée, donc un clic d'onglet se
  // répercute sur la liste à la frame suivante — invisible à l'œil.
  auto emit_vertical_tabs = [&]() {
    // Disposition NATIVE : strip d'onglets IMAGES du client à gauche (tab_all/use/
    // cos/card/fav/etc, actif = <img>1.bmp, inactif = <img>2.bmp), contenu à droite.
    // Les catégories sans .bmp côté client retombent sur un libellé texte tourné.
    if (tab_images_) EnsureTabTextures();
    const float tabW = tab_images_ ? TabStripWidth() : TabStripWidthText();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // Pas de scrollbar (illisible dans un strip de ~20 px) mais la MOLETTE scrolle :
    // sur une fenêtre courte, les 10 onglets ne tiennent pas tous et les derniers
    // seraient sinon inatteignables.
    ImGui::BeginChild("storage_tabs", ImVec2(tabW, body_h), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, -1.0f));  // jointifs
    {
      ImDrawList* tdl = ImGui::GetWindowDrawList();
      for (int c = 0; c < kNumStgCats; ++c) {
        const bool sel = (cur_tab_ == c);
        ImGui::PushID(c);
        // Repli sur l'autre état si un seul des deux .bmp a pu être chargé.
        const BarTex& img =
            g_tab[c][sel ? 0 : 1].tex ? g_tab[c][sel ? 0 : 1] : g_tab[c][sel ? 1 : 0];
        if (tab_images_ && img.tex && img.w > 0 && img.h > 0) {
          const float ih = tabW * img.h / static_cast<float>(img.w);
          const ImVec2 p = ImGui::GetCursorScreenPos();
          if (ImGui::InvisibleButton("tab", ImVec2(tabW, ih))) select_tab(c);
          const ImVec2 pe(p.x + tabW, p.y + ih);
          // L'image active/inactive porte déjà la sélection -> pas de cadre ajouté.
          tdl->AddImage(reinterpret_cast<ImTextureID>(img.tex), p, pe,
                        ImVec2(0, 0), ImVec2(1, 1), ro::SkinImageTint());
          // Sigle des catégories qui partagent un art (Ar/Am/Cs/Mu/$/Et) : sans lui,
          // trois tuiles « équipement » d'affilée sont indistinguables.
          if (kStgCats[c].tag) {
            const ImVec2 ts = ImGui::CalcTextSize(kStgCats[c].tag);
            const ImVec2 tp((p.x + pe.x - ts.x) * 0.5f, (p.y + pe.y - ts.y) * 0.5f);
            tdl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(255, 255, 255, 200),
                         kStgCats[c].tag);  // liseré clair (lisibilité sur l'art)
            tdl->AddText(tp, IM_COL32(40, 40, 40, 255), kStgCats[c].tag);
          }
          if (!sel && ImGui::IsItemHovered())
            tdl->AddRectFilled(p, pe, IM_COL32(255, 255, 255, 45));  // survol
        } else {
          // Pas d'images : onglet TEXTE au libellé tourné à 90° (lecture bas->haut).
          // Hauteur du bouton = longueur du libellé -> le nom entier tient dans un
          // strip étroit, sans abréviation.
          const float ih = ImGui::CalcTextSize(kStgCats[c].label).x +
                           ImGui::GetStyle().FramePadding.y * 2.0f + 6.0f;
          const ImVec2 p = ImGui::GetCursorScreenPos();
          if (ImGui::InvisibleButton("tab", ImVec2(tabW-4.0f, ih))) select_tab(c);
          const ImVec2 pe(p.x + tabW-4.0f, p.y + ih);
          const bool hov = ImGui::IsItemHovered();
          // Couleurs prises au THÈME (ImGuiCol_Tab*) : les onglets suivent le skin
          // comme ceux du TabBar horizontal, au lieu d'un gris/blanc en dur.
          const ImU32 bg = ImGui::GetColorU32(
              sel ? ImGuiCol_TabSelected
                  : (hov ? ImGuiCol_TabHovered : ImGuiCol_Tab));
          // Coins ARRONDIS à gauche seulement : le bord droit reste franc et se
          // fond dans le contenu (c'est là que passe le pont de l'onglet actif).
          tdl->AddRectFilled(p, pe, bg, ImGui::GetStyle().TabRounding,
                             ImDrawFlags_RoundCornersLeft);
          // Zone autorisée = rect de l'onglet ∩ clip courant (le strip) : le libellé
          // ne peut ni sortir de son cadre ni déborder sur le footer quand les
          // onglets ne tiennent pas tous dans la hauteur disponible.
          const ImVec2 cmin(
              (std::max)(p.x, tdl->GetClipRectMin().x),
              (std::max)(p.y, tdl->GetClipRectMin().y));
          const ImVec2 cmax(
              (std::min)(pe.x, tdl->GetClipRectMax().x),
              (std::min)(pe.y, tdl->GetClipRectMax().y));
          if (cmax.x > cmin.x && cmax.y > cmin.y)
            AddTextVertical(tdl, ImVec2((p.x + pe.x-2.0f) * 0.5f, (p.y + pe.y) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_Text), kStgCats[c].label,
                            cmin, cmax);
        }
        // Glisser un item du viewer sur l'onglet : Favoris = ajoute, autre = retire.
        tab_drop_target(c);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(" %s ", kStgCats[c].label);
        if (sel) {
          active_tab_min = ImGui::GetItemRectMin();
          active_tab_max = ImGui::GetItemRectMax();
          have_active_tab = true;
        }
        ImGui::PopID();
      }
    }
    ImGui::PopStyleVar();  // ItemSpacing
    ImGui::EndChild();
    ImGui::PopStyleVar();  // WindowPadding
    // Onglets COLLÉS au contenu : SameLine sans espacement (le SameLine par défaut
    // insère ItemSpacing.x, ce qui laissait une bande vide entre le strip et la
    // table). C'est aussi ce qui permet au pont de l'onglet actif de mordre sur le
    // bord du tableau.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("storage_body", ImVec2(0.0f, body_h));
  };

  // Onglets HORIZONTAUX : émis eux aussi juste avant la table (même raison que le
  // strip vertical), pour que le filtre et la valeur estimée passent AU-DESSUS et
  // que la rangée d'onglets reste collée à l'en-tête du tableau.
  auto emit_horizontal_tabs = [&]() {
  // Le strip d'onglets de STORAGE, quand il est là, se pose à gauche de la table
  // et la décale d'autant. La rangée de catégories démarre donc à la même
  // abscisse qu'elle — symétrique exact de l'Indent de emit_storage_row, sans
  // quoi elle pend à gauche, au-dessus de rien.
  if (storage_strip_left) { EnsureTabTextures(); ImGui::Indent(StorageStripWidth()); }
  if (tab_images_) {
    // Disposition HORIZONTALE en IMAGES : jeu tabh_* (mêmes noms que le vertical
    // avec un « h » après « tab »). Rangée d'onglets jointifs, hauteur native de
    // l'art, largeur de chaque tuile déduite de son ratio.
    EnsureTabTextures();
    const float tabH = TabStripHeight();
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(-1.0f, 0.0f));  // jointifs
    for (int c = 0; c < kNumStgCats; ++c) {
      const bool sel = (cur_tab_ == c);
      ImGui::PushID(c);
      // Repli sur l'autre état si un seul des deux .bmp a pu être chargé.
      const BarTex& img =
          g_tabh[c][sel ? 0 : 1].tex ? g_tabh[c][sel ? 0 : 1] : g_tabh[c][sel ? 1 : 0];
      if (c) ImGui::SameLine();
      if (img.tex && img.w > 0 && img.h > 0) {
        const float iw = tabH * img.w / static_cast<float>(img.h);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("tabh", ImVec2(iw, tabH))) select_tab(c);
        const ImVec2 pe(p.x + iw, p.y + tabH);
        tdl->AddImage(reinterpret_cast<ImTextureID>(img.tex), p, pe,
                      ImVec2(0, 0), ImVec2(1, 1), ro::SkinImageTint());
        if (kStgCats[c].tag) {  // sigle des catégories qui partagent un art
          const ImVec2 ts = ImGui::CalcTextSize(kStgCats[c].tag);
          const ImVec2 tp((p.x + pe.x - ts.x) * 0.5f, (p.y + pe.y - ts.y) * 0.5f);
          tdl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(255, 255, 255, 200),
                       kStgCats[c].tag);
          tdl->AddText(tp, IM_COL32(40, 40, 40, 255), kStgCats[c].tag);
        }
        if (!sel && ImGui::IsItemHovered())
          tdl->AddRectFilled(p, pe, IM_COL32(255, 255, 255, 45));  // survol
      } else if (ImGui::Selectable(kStgCats[c].label, sel, 0,
                                   ImVec2(ImGui::CalcTextSize(kStgCats[c].label).x +
                                              ImGui::GetStyle().FramePadding.x * 2.0f,
                                          tabH))) {
        select_tab(c);
      }
      tab_drop_target(c);  // Favoris = ajoute, autre onglet = retire
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(" %s ", kStgCats[c].label);
      if (sel) {
        active_tab_min = ImGui::GetItemRectMin();
        active_tab_max = ImGui::GetItemRectMax();
        have_active_tab = true;
      }
      ImGui::PopID();
    }
    ImGui::PopStyleVar();  // ItemSpacing
  } else {
    // Disposition HORIZONTALE (défaut) : TabBar classique. L'onglet actif est
    // PERSISTANT (bourgeon_settings.yaml) : au 1er rendu de la session on force la
    // sélection sur l'onglet restauré (ImGui ne sérialise pas la sélection d'un
    // TabBar), ensuite c'est le joueur qui pilote.
    if (ImGui::BeginTabBar("storage_cats", ImGuiTabBarFlags_FittingPolicyScroll)) {
      for (int c = 0; c < kNumStgCats; ++c) {
        const ImGuiTabItemFlags tflags =
            (!tab_applied_ && c == cur_tab_) ? ImGuiTabItemFlags_SetSelected : 0;
        const bool tab_open = ImGui::BeginTabItem(kStgCats[c].label, nullptr, tflags);
        // Le « dernier item » ImGui est le BOUTON d'onglet, que celui-ci soit ouvert
        // ou non (BeginTabItem ne renvoie true que pour l'onglet SÉLECTIONNÉ) : la
        // cible de dépôt doit donc être posée ici, hors du if, sinon on ne pourrait
        // déposer que sur l'onglet déjà actif.
        tab_drop_target(c);
        if (tab_open) {
          select_tab(c);
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
      tab_applied_ = true;
    }
  }
  if (storage_strip_left) ImGui::Unindent(StorageStripWidth());
  };

  // Clic sur un onglet. Sans effet sur celui qui est déjà ouvert : le serveur
  // ignore aussi ce cas, mais autant ne pas envoyer le paquet — et surtout, les
  // commandes @storagealt, elles, FERMENT dans ce cas, ce qui serait un piège.
  auto select_storage = [this](uint8_t id) {
    if (switching_ || static_cast<int>(id) == cur_storage_id_) return;
    SendOpenStorage(id);
  };

  // Fond d'un onglet de storage : le BLANC de la liste (cf. SetNextWindowBodyColor
  // plus haut), pour TOUS. Ce n'est pas le fond qui dit la sélection — c'est le
  // bord du bas, ouvert sur le seul onglet actif, et son contenu en couleur.

  // Libellé ÉCRIT sur l'onglet. Par défaut le NUMÉRO du storage : les noms du
  // serveur (« Storage Alt 3 ») sont trop longs pour six onglets et se
  // ressemblent tous — un chiffre se lit d'un coup d'œil et tient partout. Le
  // nom complet reste en infobulle, et le joueur peut écrire ce qu'il veut à la
  // place (clic droit sur l'onglet).
  // `out` doit survivre à l'usage du retour : il porte le numéro quand il n'y a
  // pas de nom personnalisé.
  auto storage_label = [this](const StorageTab& tab, char* out, size_t cap) -> const char* {
    auto it = tab_custom_.find(tab.id);
    if (it != tab_custom_.end() && it->second.name[0]) return it->second.name;
    std::snprintf(out, cap, "%u", static_cast<unsigned>(tab.id));
    return out;
  };
  // Infobulle : le nom du SERVEUR, plus le nom choisi s'il y en a un. C'est la
  // seule chose qui dit vraiment quel entrepôt est sous le curseur.
  auto storage_tooltip = [this](const StorageTab& tab) {
    auto it = tab_custom_.find(tab.id);
    if (it != tab_custom_.end() && it->second.name[0])
      ImGui::SetTooltip(" %s \n(%s)", it->second.name, tab.name);
    else
      ImGui::SetTooltip(" %s ", tab.name);
  };
  // Icône d'item choisie pour cet onglet (0 = aucune -> libellé texte).
  auto storage_icon = [this](const StorageTab& tab) -> uint32_t {
    auto it = tab_custom_.find(tab.id);
    return it != tab_custom_.end() ? it->second.icon_id : 0;
  };
  auto save_settings = [] {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  };

  // Menu de personnalisation d'un onglet (clic DROIT). À appeler juste après le
  // bouton de l'onglet, tant qu'il est le « dernier item » ImGui. L'id ImGui de
  // l'onglet est déjà poussé par l'appelant : le popup est donc propre à l'onglet.
  auto storage_tab_config = [&](const StorageTab& tab) {
    // Glisser un item du viewer sur un onglet = lui donner SON icône. Il n'y a
    // pas de transfert storage -> storage dans le protocole, donc ce geste ne
    // peut rien vouloir dire d'autre ici.
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("STG_ITEM")) {
        const int di = *static_cast<const int*>(pl->Data);
        if (di >= 0 && di < item_count_ && items_[di].id != 0) {
          tab_custom_[tab.id].icon_id = items_[di].id;
          save_settings();
        }
      }
      ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("stgtab_cfg");
    // Marges rendues au popup (cf. popup_padding / popup_spacing) : celles du
    // strip d'onglets sont à zéro, et elles s'appliqueraient à lui.
    // `WindowPadding` n'est lu qu'à la CRÉATION de la fenêtre, `ItemSpacing` à
    // chaque item : les deux restent donc poussés jusqu'à EndPopup.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popup_padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, popup_spacing);
    if (!ImGui::BeginPopup("stgtab_cfg")) {
      ImGui::PopStyleVar(2);
      return;
    }
    TabCustom& custom = tab_custom_[tab.id];
    // Nom du SERVEUR toujours rappelé : c'est la seule façon de savoir quel
    // entrepôt on est en train de renommer une fois le nom remplacé.
    ImGui::TextDisabled("%s (id %u)", tab.name, static_cast<unsigned>(tab.id));
    ImGui::Separator();
    ImGui::SetNextItemWidth(180.0f);
    // Le hint montre le nom serveur : laisser vide, c'est le garder.
    if (ImGui::InputTextWithHint("Nom", tab.name, custom.name, sizeof(custom.name)))
      save_settings();
    int icon = static_cast<int>(custom.icon_id);
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputInt("Icône (id d'item)", &icon)) {
      custom.icon_id = icon > 0 ? static_cast<uint32_t>(icon) : 0;
      save_settings();
    }
    if (custom.icon_id) {
      const ro::IconTex ic = ro::ItemIcon(custom.icon_id);
      if (ic.tex && ic.w > 0 && ic.h > 0)
        ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(24.0f, 24.0f));
      else
        ImGui::TextDisabled("(icône introuvable pour cet id)");
    } else {
      ImGui::TextDisabled("Astuce : glissez un item du storage sur l'onglet.");
    }
    if (ro::RoButton("Réinitialiser", 110.0f, 20.0f)) {
      tab_custom_.erase(tab.id);
      save_settings();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);  // WindowPadding + ItemSpacing du popup
  };

  // RANGÉE HORIZONTALE (quand les catégories sont VERTICALES) : cadres tabh_sto*
  // du client, posés au-dessus du tableau et ALIGNÉS SUR SON BORD GAUCHE.
  // Contenu = icône d'item choisie par le joueur, sinon le numéro du storage ;
  // le nom entier est en infobulle.
  auto emit_storage_row = [&]() {
    EnsureTabTextures();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Le tableau ne commence pas au bord de la fenêtre : le strip de catégories
    // le précède. Les onglets démarrent donc à la même abscisse que lui, sinon
    // la rangée pend à gauche, en dehors de ce qu'elle pilote.
    if (vertical_cats)
      ImGui::Indent(tab_images_ ? TabStripWidth() : TabStripWidthText());
    // UN SEUL basculement pour toute la rangée : le callback coupe le lot de
    // draws, un par onglet le fragmenterait pour rien.
    dl->AddCallback(ImCbPointFilter, nullptr);
    // 🔴 Recouvrement de 1 px par le STYLE, comme les onglets de catégorie —
    // surtout pas par `SameLine(0, -1)` : quand l'offset est nul, ImGui remplace
    // tout espacement négatif par `style.ItemSpacing.x`, et les onglets se
    // retrouvaient écartés de l'espacement PAR DÉFAUT.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(-1.0f, 0.0f));
    for (int i = 0; i < stg_tab_count_; ++i) {
      const StorageTab& tab = stg_tabs_[i];
      const bool sel = (static_cast<int>(tab.id) == cur_storage_id_);
      char numbuf[8];
      const char* label = storage_label(tab, numbuf, sizeof(numbuf));
      if (i) ImGui::SameLine();  // jointifs : les cadres partagent leur bord
      ImGui::PushID(static_cast<int>(tab.id));
      const BarTex& art = StorageTabArt(g_stg_tabh, sel);
      const bool has_art = art.tex && art.w > 0 && art.h > 0;
      // Gabarit = taille NATIVE de l'art, jamais étirée (repli sur ses mesures
      // si les .bmp manquent, pour que la mise en page ne bouge pas).
      const ImVec2 sz = has_art ? ImVec2(static_cast<float>(art.w),
                                         static_cast<float>(art.h))
                                : ImVec2(27.0f, 25.0f);
      const ImVec2 cur = ImGui::GetCursorScreenPos();
      // Icône EN COULEUR sur l'onglet actif — et au survol d'un inactif, qui
      // annonce ainsi ce qu'on va activer. Sinon niveaux de gris : c'est le
      // contenu, pas le cadre, qui distingue l'onglet éteint de l'allumé.
      const bool lit =
          sel || ImGui::IsMouseHoveringRect(cur, ImVec2(cur.x + sz.x, cur.y + sz.y));
      const uint32_t icon_id = storage_icon(tab);
      const ro::IconTex ic = icon_id ? (lit ? ro::ItemIcon(icon_id) : GrayItemIcon(icon_id))
                                     : ro::IconTex{};
      const bool use_icon = ic.tex && ic.w > 0 && ic.h > 0;
      if (ImGui::InvisibleButton("stgsel", sz)) select_storage(tab.id);
      const bool hov = ImGui::IsItemHovered();
      // 🔴 GRILLE PIXEL : origine ENTIÈRE. La fenêtre peut être posée sur un
      // demi-pixel, et un blit dont le coin est fractionnaire échantillonne
      // entre deux texels — filtre POINT ou pas.
      const ImVec2 p(std::floor(cur.x), std::floor(cur.y));
      const ImVec2 pe(p.x + sz.x, p.y + sz.y);
      if (has_art)
        dl->AddImage(reinterpret_cast<ImTextureID>(art.tex), p, pe, ImVec2(0, 0),
                     ImVec2(1, 1), ro::SkinImageTint());
      else  // .bmp absents : un cadre minimal vaut mieux qu'un onglet invisible
        dl->AddRect(p, pe, kStgTabBorder, 2.0f, ImDrawFlags_RoundCornersTop, 1.0f);
      // Intérieur COMMUN aux deux états : l'inactif se ferme 2 px avant le bas,
      // on centre donc sur la plus petite des deux boîtes — le contenu ne saute
      // pas d'un pixel quand l'onglet devient actif.
      const ImVec2 in_min(p.x + 1.0f, p.y + 1.0f);
      const ImVec2 in_max(pe.x - 1.0f, pe.y - 1.0f - kStgArtClosedInset);
      dl->PushClipRect(in_min, in_max, true);
      if (use_icon) {
        // Taille NATIVE, centrée : l'icône (24) déborde d'un pixel ou deux du
        // cadre (23/25 utiles), et c'est sa marge transparente qui est rognée.
        // La redimensionner la rendrait floue pour gagner ces deux pixels.
        const ImVec2 c((in_min.x + in_max.x) * 0.5f, (in_min.y + in_max.y) * 0.5f);
        const ImVec2 ip(std::floor(c.x - ic.w * 0.5f), std::floor(c.y - ic.h * 0.5f));
        dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), ip,
                     ImVec2(ip.x + ic.w, ip.y + ic.h), ImVec2(0, 0), ImVec2(1, 1),
                     ro::SkinImageTint());
      } else {
        // Libellé : couleur de texte pour l'actif (et le survolé), gris sinon —
        // même règle que les icônes.
        const ImVec2 lbl = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(std::floor((in_min.x + in_max.x - lbl.x) * 0.5f),
                           std::floor((in_min.y + in_max.y - lbl.y) * 0.5f)),
                    lit ? ImGui::GetColorU32(ImGuiCol_Text)
                        : ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    label);
      }
      dl->PopClipRect();
      if (hov) storage_tooltip(tab);
      storage_tab_config(tab);
      if (sel) { stg_tab_min = p; stg_tab_max = pe; have_stg_tab = true; }
      ImGui::PopID();
    }
    // Rangée COLLÉE au tableau, GRATUITEMENT : l'ItemSpacing poussé pour le
    // recouvrement vaut (-1, 0), donc le dernier onglet a avancé le curseur de 0
    // — il est déjà au ras du bas des onglets. Rien à corriger ici : le faire
    // APRÈS le Pop remonterait la table de 4 px SUR les onglets.
    // (L'air nécessaire est déjà dans l'art, l'inactif s'y fermant 2 px avant le
    // bas, pendant que l'actif doit toucher la bordure du tableau pour que le
    // pont la prolonge.)
    ImGui::PopStyleVar();  // ItemSpacing (recouvrement de 1 px)
    if (vertical_cats)
      ImGui::Unindent(tab_images_ ? TabStripWidth() : TabStripWidthText());
  };

  // STRIP VERTICAL (quand les catégories sont HORIZONTALES) : cadres tab_sto* du
  // client, en colonne à gauche de la table. Ouvre la ligne qui contiendra la
  // table : l'appelant enchaîne directement sur BeginTable.
  auto emit_storage_strip = [&]() {
    EnsureTabTextures();
    // Largeur = taille NATIVE de l'art, jamais étirée (repli sur ses mesures si
    // les .bmp manquent, pour que la mise en page ne bouge pas).
    const BarTex& art_ref = g_stg_tab[0].tex ? g_stg_tab[0] : g_stg_tab[1];
    const float w = art_ref.tex && art_ref.w > 0 ? static_cast<float>(art_ref.w) : 23.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // Même parti pris que le strip de catégories : pas de scrollbar dans une
    // colonne de ~20 px, mais la molette scrolle si les onglets débordent.
    ImGui::BeginChild("stgsel_strip", ImVec2(w, body_h), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, -1.0f));  // jointifs
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Échantillonnage POINT pour tout le strip (cf. ImCbPointFilter) : l'état
    // ambiant est LINEAR, une icône de 24x24 y ressortirait baveuse.
    dl->AddCallback(ImCbPointFilter, nullptr);
    for (int i = 0; i < stg_tab_count_; ++i) {
      const StorageTab& tab = stg_tabs_[i];
      const bool sel = (static_cast<int>(tab.id) == cur_storage_id_);
      char numbuf[8];
      const char* label = storage_label(tab, numbuf, sizeof(numbuf));
      ImGui::PushID(static_cast<int>(tab.id));
      const BarTex& art = StorageTabArt(g_stg_tab, sel);
      const bool has_art = art.tex && art.w > 0 && art.h > 0;
      const ImVec2 sz = has_art ? ImVec2(static_cast<float>(art.w),
                                         static_cast<float>(art.h))
                                : ImVec2(w, 27.0f);
      const ImVec2 cur = ImGui::GetCursorScreenPos();
      // Couleur sur l'actif et sur le survolé, gris sinon — même règle que la
      // rangée horizontale. Le survol est testé avant l'émission du bouton, la
      // texture devant être choisie avant d'être dessinée.
      const bool lit =
          sel || ImGui::IsMouseHoveringRect(cur, ImVec2(cur.x + sz.x, cur.y + sz.y));
      const uint32_t icon_id = storage_icon(tab);
      const ro::IconTex ic = icon_id ? (lit ? ro::ItemIcon(icon_id) : GrayItemIcon(icon_id))
                                     : ro::IconTex{};
      const bool use_icon = ic.tex && ic.w > 0 && ic.h > 0;
      if (ImGui::InvisibleButton("stgsel", sz)) select_storage(tab.id);
      const bool hov = ImGui::IsItemHovered();
      // Grille pixel, comme la rangée horizontale (cf. emit_storage_row).
      const ImVec2 p(std::floor(cur.x), std::floor(cur.y));
      const ImVec2 pe(p.x + sz.x, p.y + sz.y);
      if (has_art)
        dl->AddImage(reinterpret_cast<ImTextureID>(art.tex), p, pe, ImVec2(0, 0),
                     ImVec2(1, 1), ro::SkinImageTint());
      else
        dl->AddRect(p, pe, kStgTabBorder, 2.0f, ImDrawFlags_RoundCornersLeft, 1.0f);
      // Intérieur COMMUN aux deux états : ici c'est le bord DROIT que l'inactif
      // ferme 2 px avant (l'actif s'ouvre par là, vers la table).
      const ImVec2 in_min(p.x + 1.0f, p.y + 1.0f);
      const ImVec2 in_max(pe.x - 1.0f - kStgArtClosedInset, pe.y - 1.0f);
      dl->PushClipRect(in_min, in_max, true);
      if (use_icon) {
        const ImVec2 c((in_min.x + in_max.x) * 0.5f, (in_min.y + in_max.y) * 0.5f);
        const ImVec2 ip(std::floor(c.x - ic.w * 0.5f), std::floor(c.y - ic.h * 0.5f));
        dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), ip,
                     ImVec2(ip.x + ic.w, ip.y + ic.h), ImVec2(0, 0), ImVec2(1, 1),
                     ro::SkinImageTint());
      } else {
        // Libellé À PLAT (plus tourné à 90°) : le cadre fait la taille d'une
        // icône, un nom écrit en hauteur y serait illisible — c'est le numéro du
        // storage qui tient, et l'infobulle qui porte le nom.
        const ImVec2 lbl = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(std::floor((in_min.x + in_max.x - lbl.x) * 0.5f),
                           std::floor((in_min.y + in_max.y - lbl.y) * 0.5f)),
                    lit ? ImGui::GetColorU32(ImGuiCol_Text)
                        : ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    label);
      }
      dl->PopClipRect();
      if (hov) storage_tooltip(tab);
      storage_tab_config(tab);
      if (sel) {
        stg_tab_min = p;
        stg_tab_max = pe;
        have_stg_tab = true;
        stg_tab_is_strip = true;  // pont HORIZONTAL (vers le bord gauche de la table)
      }
      ImGui::PopID();
    }
    ImGui::PopStyleVar();  // ItemSpacing
    ImGui::EndChild();
    ImGui::PopStyleVar();  // WindowPadding
    // Collé à la table (SameLine par défaut insérerait ItemSpacing.x).
    ImGui::SameLine(0.0f, 0.0f);
  };

  // meta serveur d'un item (subtype/equip) pour les sous-catégories.
  auto submeta = [this](uint32_t id) -> ItemMeta {
    auto it = meta_.find(id);
    return it != meta_.end() ? it->second : ItemMeta{};
  };

  // Barre de recherche (filtre par nom) — optionnelle (setting « Champ de filtre »).
  // Le "(?)" des raccourcis est sur la ligne du compteur, sous la table d'options.
  static ImGuiTextFilter filter;
  // Changement de storage : on repart d'une vue NEUVE. Un filtre tapé pour
  // l'entrepôt précédent masquerait tout dans le suivant, ce qui se lit comme
  // « mon storage est vide » ; et une sous-catégorie n'a de sens que dans un
  // onglet où elle existe encore.
  if (reset_view_) {
    filter.Clear();
    cur_sub_ = -1;
    reset_view_ = false;
  }
  if (show_filter_) {
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##storage_filter", "Filtrer...", filter.InputBuf,
                                 IM_ARRAYSIZE(filter.InputBuf)))
      filter.Build();
  } else if (filter.InputBuf[0]) {
    // Filtre masqué : on le vide pour ne pas cacher silencieusement des items.
    filter.Clear();
  }
  std::string desc = "Raccourcis storage\n\n"
                     "- Clic gauche sur un item : retrait (Maj = tout le stack ; 1 seul = direct ;\n"
                     "  pile = menu contextuel : Vers l'inventaire 1 / tout / quantité)\n"
                     "- Ctrl + clic gauche : (dé)marquer l'item comme favori (onglet Favoris)\n"
                     "- Clic droit : menu contextuel (dont Ajouter / Retirer des favoris)\n"
                     "- Ctrl + clic droit : description\n"
                     "- Alt / Maj + clic droit : retrait rapide du stack complet vers l'inventaire\n"
                     "- Glisser un item du viewer -> inventaire : retrait ; -> cart : storage vers cart\n"
                     "- Glisser un item d'inventaire / cart sur le viewer : dépôt / cart vers storage\n"
                     "- Glisser un item sur l'onglet Favoris : l'y ajoute ; sur un autre onglet : l'en retire\n"
                     "- Entrée : valide la quantité (défaut = stack entier)\n"
                     "- Survol d'un item : description (si activé dans Interface > Storage)\n"
                     "- Clic sur un en-tête de colonne : tri ; combo Sous-type : filtre fin\n"
                     "  (onglets de catégorie et sous-type : désactivables dans les options)\n"
                     "- Colonnes, filtre et survol : Moonlight > Interface de jeu > Storage\n"
                     "- Onglets de storage (option) : bascule vers un entrepôt alternatif\n"
                     "- Clic droit sur un onglet de storage : le renommer / lui donner une icône\n"
                     "- Glisser un item sur un onglet de storage : lui assigner SON icône\n"
                     "- Bouton Quitter / X : ferme le storage";

  // Onglet EFFECTIF : « Tout » (index 0) quand les filtres par type sont
  // désactivés. `cur_tab_` n'est PAS écrasé — le choix du joueur l'attend
  // intact s'il réaffiche les onglets. kStgCats[0].sub valant kSubNone, ceci
  // neutralise du même coup le combo « Sous-type » et son filtrage plus bas :
  // il n'y a qu'un seul endroit à tenir juste.
  const int active_tab = show_type_tabs_ ? cur_tab_ : 0;

  // Vue onglet+nom (avant sous-catégorie) : sert à connaître les sous-cats présentes.
  // Onglet Favoris : filtre par le set client (IsFavorite), pas par type d'item.
  const int  sub_dim = kStgCats[active_tab].sub;
  const bool fav_tab = kStgCats[active_tab].fav;
  std::vector<int> tabview;
  tabview.reserve(item_count_);
  for (int i = 0; i < item_count_; ++i) {
    const bool in_tab =
        fav_tab ? IsFavorite(items_[i].id)
                : ItemInTab(active_tab, items_[i].type, submeta(items_[i].id).equip);
    if (in_tab && filter.PassFilter(items_[i].name)) tabview.push_back(i);
  }

  // Combo de sous-catégorie (armes par type, armures/cartes par slot, munitions par
  // type) : ne liste que les sous-cats réellement présentes, triées par clé.
  std::vector<SubCat> subs;
  if (sub_dim != kSubNone) {
    for (int i : tabview) {
      const ItemMeta m = submeta(items_[i].id);
      const SubCat sc = SubCatOf(sub_dim, m.subtype, m.equip);
      if (!sc.label) continue;
      bool seen = false;
      for (const auto& s : subs) if (s.key == sc.key) { seen = true; break; }
      if (!seen) {
        size_t p = subs.size();
        while (p > 0 && subs[p - 1].key > sc.key) --p;
        subs.insert(subs.begin() + p, sc);
      }
    }
    // Label courant (repli Tout si la clé sélectionnée n'existe plus).
    const char* cur_label = "Tout";
    if (cur_sub_ != -1) {
      bool found = false;
      for (const auto& s : subs) if (s.key == cur_sub_) { cur_label = s.label; found = true; break; }
      if (!found) cur_sub_ = -1;
    }
    ImGui::TextUnformatted("Sous-type");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ro::RoBeginCombo("##storage_subcat", cur_label)) {
      if (ImGui::Selectable("Tout", cur_sub_ == -1)) cur_sub_ = -1;
      for (const auto& s : subs)
        if (ImGui::Selectable(s.label, cur_sub_ == s.key)) cur_sub_ = s.key;
      ro::RoEndCombo();
    }
  }

  // Vue finale = tabview filtrée par la sous-catégorie choisie.
  std::vector<int> view;
  view.reserve(tabview.size());
  for (int i : tabview) {
    if (sub_dim == kSubNone || cur_sub_ == -1) { view.push_back(i); continue; }
    const ItemMeta m = submeta(items_[i].id);
    if (SubCatOf(sub_dim, m.subtype, m.equip).key == cur_sub_) view.push_back(i);
  }

  // Valeur = prix de vente NPC (reçu du serveur) * quantité.
  auto price = [this](uint32_t id) -> long long {
    auto it = prices_.find(id);
    return it != prices_.end() ? static_cast<long long>(it->second) : 0;
  };
  long long total_val = 0;
  for (int i : view) total_val += price(items_[i].id) * items_[i].amount;

  // Valeur estimée : LIGNE À ELLE SEULE. Surtout pas de SameLine ici — le widget
  // précédent est le combo « Sous-type » ou le champ de filtre, tous deux en
  // largeur pleine (SetNextItemWidth(-1)), et le texte se retrouvait dessous.
  // Compteurs et "(?)" sont dans le footer.
  if (show_total_value_) ImGui::Text("Valeur estimée: %lldz", total_val);

  // Bascule en vol : la liste est vide À DESSEIN (cf. OnTick, on ne lit plus le
  // modèle) et le storage demandé peut mettre un aller-retour char-server à
  // arriver. Sans cette ligne, l'écran dirait « storage vide ».
  if (switching_) ImGui::TextDisabled("Chargement du storage...");

  // Catégories VERTICALES -> la rangée de storages est HORIZONTALE. Elle est
  // émise ICI et pas en tête de fenêtre : au plus PRÈS de la table, juste
  // au-dessus du strip de catégories. Posée tout en haut, elle flottait à trois
  // lignes de la liste qu'elle pilote (filtre, sous-type et valeur estimée
  // s'intercalaient) et on ne la rattachait plus à rien.
  if (storage_tabs_on && tabs_vertical_) emit_storage_row();

  // Onglets de catégorie : ICI, une fois le filtre et la valeur estimée émis,
  // donc collés à l'en-tête du tableau et non plus en tête de fenêtre. En
  // vertical, ceci ouvre aussi le child « corps » qui contient la table — d'où
  // `vertical_cats`, qui doit rester VRAI aux trois autres endroits qui le
  // testent (taille de table, EndChild, pont de l'onglet actif) et FAUX quand
  // les onglets ne sont pas émis du tout.
  if (vertical_cats)          emit_vertical_tabs();
  else if (show_type_tabs_)   emit_horizontal_tabs();

  // Catégories HORIZONTALES -> le sélecteur de storage est le strip VERTICAL, à
  // gauche de la table (il ouvre la ligne, la table se pose juste à sa droite).
  if (storage_tabs_on && !tabs_vertical_) emit_storage_strip();

  // Item survolé ce frame (0 = aucun) : pilote l'aperçu de description dessiné
  // après la fenêtre. `hover_idx` sert à retrouver cartes/options du stack.
  uint32_t hover_id = 0;
  int      hover_idx = -1;

  // Ordre courant des colonnes : [Index], Item, [ID], [Slots], Qté, Prix revente.
  // Les index de tri sont calculés dynamiquement (les colonnes optionnelles décalent).
  const int ncols = 2 + (show_index_col_ ? 1 : 0) + (show_id_col_ ? 1 : 0) +
                    (show_slots_col_ ? 1 : 0) + (show_value_col_ ? 1 : 0);
  int colc = 0;
  const int kColIdx   = show_index_col_ ? colc++ : -1;  // Index
  ++colc;                                               // Item -> tri par nom (else)
  const int kColId    = show_id_col_ ? colc++ : -1;      // ID
  const int kColSlots = show_slots_col_ ? colc++ : -1;   // Slots
  const int kColQte   = colc++;                          // Qté
  const int kColVal   = show_value_col_ ? colc++ : -1;   // Prix revente
  const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_Sortable |
                             ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_SizingStretchProp;
  // En disposition verticale, c'est le child « corps » qui a déjà réservé la place
  // du footer : la table prend simplement toute sa hauteur.
  const ImVec2 table_size =
      vertical_cats ? ImVec2(0.0f, 0.0f) : ImVec2(0.0f, body_h);
  table_top_y  = ImGui::GetCursorScreenPos().y;
  table_left_x = ImGui::GetCursorScreenPos().x;
  if (ImGui::BeginTable("storage_items", ncols, tf, table_size)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    if (show_index_col_)
      ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed |
                                         ImGuiTableColumnFlags_PreferSortDescending,
                              54.0f);
    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch |
                                        ImGuiTableColumnFlags_DefaultSort);
    if (show_id_col_)
      ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    if (show_slots_col_)
      ImGui::TableSetupColumn("Slots", ImGuiTableColumnFlags_WidthFixed |
                                           ImGuiTableColumnFlags_PreferSortDescending,
                              24.0f);
    ImGui::TableSetupColumn("Qté", ImGuiTableColumnFlags_WidthFixed |
                                       ImGuiTableColumnFlags_PreferSortDescending,
                            36.0f);
    if (show_value_col_)
      ImGui::TableSetupColumn("Prix revente", ImGuiTableColumnFlags_WidthFixed |
                                          ImGuiTableColumnFlags_PreferSortDescending,
                              72.0f);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) { // tri demandé
      if (sort->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& sp = sort->Specs[0];
        const bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
        std::sort(view.begin(), view.end(), [&](int a, int b) {
          int c;
          if (sp.ColumnIndex == kColQte) {
            c = (items_[a].amount < items_[b].amount) ? -1
                : (items_[a].amount > items_[b].amount) ? 1 : 0;
          } else if (sp.ColumnIndex == kColVal) {
            const long long va = price(items_[a].id) * items_[a].amount;
            const long long vb = price(items_[b].id) * items_[b].amount;
            c = (va < vb) ? -1 : (va > vb) ? 1 : 0;
          } else if (sp.ColumnIndex == kColIdx) {
            c = (items_[a].index < items_[b].index) ? -1
                : (items_[a].index > items_[b].index) ? 1 : 0;
          } else if (sp.ColumnIndex == kColId) {
            c = (items_[a].id < items_[b].id) ? -1
                : (items_[a].id > items_[b].id) ? 1 : 0;
          } else if (sp.ColumnIndex == kColSlots) {
            const int sa = submeta(items_[a].id).slots;
            const int sb = submeta(items_[b].id).slots;
            c = (sa < sb) ? -1 : (sa > sb) ? 1 : 0;
          } else {
            c = _stricmp(items_[a].name, items_[b].name);
          }
          return asc ? c < 0 : c > 0;
        });
      }
    }

    constexpr float kIcon = 22.0f;  // hauteur d'affichage de l'icône
    for (int idx : view) {
      ImGui::TableNextRow();
      // ── Colonne Idx (optionnelle) : index storage (slot) ──
      if (show_index_col_) {
        ImGui::TableNextColumn();
        ImGui::Text("%d", items_[idx].index);
      }
      // ── Colonne Item : icône + nom cliquable (clic-droit = description) ──
      ImGui::TableNextColumn();
      const ro::IconTex ic = ro::ItemIcon(items_[idx].id, items_[idx].identified);
      const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
      if (ic.tex && ic.w > 0 && ic.h > 0) {
        const float w = kIcon * static_cast<float>(ic.w) / ic.h;
        // Cassé = icône teintée du rouge natif (cf. itemcell::kDamagedShadow).
        // ImageWithBg et non Image : depuis ImGui 1.91.9 c'est elle qui porte
        // le paramètre tint_col.
        const ImVec4 tint = items_[idx].damaged
                                ? ImGui::ColorConvertU32ToFloat4(itemcell::kDamagedShadow)
                                : ImVec4(1, 1, 1, 1);
        ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(w, kIcon),
                           ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
      } else {
        ImGui::Dummy(ImVec2(kIcon, kIcon));  // garde l'alignement si pas d'icône
      }
      // Marqueur favori : petite étoile dorée en coin haut-gauche de l'icône.
      if (IsFavorite(items_[idx].id))
        DrawFavStar(ImGui::GetWindowDrawList(), icon_pos.x + 5.0f, icon_pos.y + 5.0f,
                    5.0f, IM_COL32(255, 205, 40, 255), IM_COL32(60, 40, 0, 220));
      ImGui::SameLine();
      ImGui::PushID(idx);
      // Équipement cassé : l'ombre rouge du natif sous le nom. Soumise AVANT le
      // Selectable, elle reste sous son texte ; le fond de survol la recouvre le
      // temps du hover (ordre de dessin), défaut cosmétique accepté.
      if (items_[idx].damaged && items_[idx].name[0]) {
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(ImVec2(rp.x + 1.0f, rp.y + 1.0f),
                                            itemcell::kDamagedShadow,
                                            items_[idx].name);
      }
      // Clic GAUCHE : Ctrl -> (dé)favori ; Shift -> tout retirer (hotkey native) ;
      // 1 seul item -> retrait direct ; sinon (pile) -> menu de choix de quantité.
      if (ImGui::Selectable(items_[idx].name[0] ? items_[idx].name : "(?)")) {
        if (ImGui::GetIO().KeyCtrl)
          ToggleFavorite(items_[idx].id);
        else if (ImGui::GetIO().KeyShift || items_[idx].amount <= 1)
          WithdrawItem(items_[idx].index, items_[idx].amount);
        else
          ImGui::OpenPopup("ctx");
      }
      // Description au SURVOL (option « Description au survol ») : on retient la
      // ligne survolée ; l'ouverture/fermeture de la VRAIE fenêtre de description
      // (celle du clic droit) se fait après la table, sur CHANGEMENT de ligne.
      // Pas pendant un drag (bouton tenu / payload ImGui actif) : la fenêtre
      // masquerait la cible du drop.
      if (show_desc_tooltip_ &&
          ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) &&
          !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
          ImGui::GetDragDropPayload() == nullptr && !DescPendingBlocksHover()) {
        hover_id = items_[idx].id;
        hover_idx = idx;
      }
      // Source de DRAG : glisser un item du viewer -> relâché sur l'inventaire
      // natif = retrait (le fantôme suit le curseur). Le drop est traité en fin
      // de OnRenderUI (MouseOverInventory).
      // Marge interne du fantôme : la grille pousse WindowPadding à 0 (tuiles
      // jointives), et la tooltip de drag d'ImGui hérite de ce style au Begin ->
      // on la surcharge le temps du bloc pour aérer l'icône + le nom.
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        drag_active_ = true;
        drag_index_ = items_[idx].index;
        drag_amount_ = items_[idx].amount;
        ImGui::SetDragDropPayload("STG_ITEM", &idx, sizeof(idx));
        if (ic.tex && ic.w > 0 && ic.h > 0) {
          const float w = kIcon * static_cast<float>(ic.w) / ic.h;
          ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(w, kIcon));
          ImGui::SameLine();
        }
        ImGui::TextUnformatted(items_[idx].name[0] ? items_[idx].name : "(?)");
        ImGui::EndDragDropSource();
      }
      ImGui::PopStyleVar();  // WindowPadding (marge du fantôme de drag)
      // Clic DROIT : Ctrl -> description directe ; Alt/Maj -> retrait rapide du
      // stack COMPLET vers l'inventaire ; sinon -> menu contextuel.
      if (IsLastItemRightClicked()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
          MarkDescPending();  // bloque l'aperçu au survol jusqu'à la fenêtre de desc
          POINT pt;
          if (GetCursorPos(&pt)) OpenItemDesc(items_[idx].index, pt.x, pt.y);
        } else if (io.KeyAlt || io.KeyShift) {
          WithdrawItem(items_[idx].index, items_[idx].amount);
        } else {
          ImGui::OpenPopup("ctx");
        }
      }
      if (ImGui::BeginPopup("ctx")) {
        // `name` porte DÉJÀ « [N] » : le suffixe est cuit à l'extraction, avec
        // garde anti-doublon (cf. le bloc « Suffixe [N] » d'OnTick).
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        itemcell::NameText(items_[idx].name[0] ? items_[idx].name : "(?)",
                           items_[idx].damaged != 0);
        ImGui::PopStyleColor();
        ImGui::Separator();
        if (ImGui::MenuItem("Description")) {
          // Le menu se ferme AVANT que la fenêtre de description n'apparaisse :
          // sans ce verrou, l'aperçu au survol se rouvre entre les deux (flicker).
          MarkDescPending();
          POINT pt;
          if (GetCursorPos(&pt)) OpenItemDesc(items_[idx].index, pt.x, pt.y);
        }
        if (ImGui::MenuItem(IsFavorite(items_[idx].id) ? "Retirer des favoris"
                                                       : "Ajouter aux favoris"))
          ToggleFavorite(items_[idx].id);
        ImGui::Separator();
        const int amt = items_[idx].amount;
        const int index = items_[idx].index;
        // Destination NOMMÉE : « Retirer » seul ne disait pas où l'objet partait,
        // alors que le cart est une destination tout aussi légitime (et la seule
        // que le menu ne proposait pas du tout).
        if (ImGui::MenuItem("Vers l'inventaire (1)")) WithdrawItem(index, 1);
        if (amt > 1) {
          char lbl[48];
          std::snprintf(lbl, sizeof(lbl), "Vers l'inventaire (tout : %d)", amt);
          if (ImGui::MenuItem(lbl)) WithdrawItem(index, amt);
          // Quantité libre : on ARME le prompt partagé (ui/qty_prompt) au lieu de
          // saisir dans le menu — un champ + un bouton en plein menu contextuel,
          // c'était le seul endroit du storage resté en widgets ImGui bruts.
          if (ImGui::MenuItem("Vers l'inventaire...")) {
            pend_id_ = index;
            pend_index_ = index;
            pend_max_ = amt;
            pend_action_ = kPendStoToInv;
            pend_open_prompt_ = true;
          }
        }
        // Cart ouvert : même offre que pour l'inventaire. Le serveur AUTORISE
        // storage <-> cart pendant que le storage est ouvert (CZ 0x0128/0x0129,
        // hors pc_cant_act2) — contrairement à inventaire <-> cart.
        if (CartOpen()) {
          ImGui::Separator();
          if (ImGui::MenuItem("Vers le cart (1)")) SendStorageToCart(index, 1);
          if (amt > 1) {
            char lbl[48];
            std::snprintf(lbl, sizeof(lbl), "Vers le cart (tout : %d)", amt);
            if (ImGui::MenuItem(lbl)) SendStorageToCart(index, amt);
            if (ImGui::MenuItem("Vers le cart...")) {
              pend_id_ = index;
              pend_index_ = index;
              pend_max_ = amt;
              pend_action_ = kPendStoToCart;
              pend_open_prompt_ = true;
            }
          }
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
      // ── Colonne ID (optionnelle) ──
      if (show_id_col_) {
        ImGui::TableNextColumn();
        ImGui::Text("%u", items_[idx].id);
      }
      // ── Colonne Slots (optionnelle) : nb de slots de carte ──
      if (show_slots_col_) {
        ImGui::TableNextColumn();
        const int sl = submeta(items_[idx].id).slots;
        if (sl > 0) ImGui::Text("%d", sl);
        else ImGui::TextDisabled("-");
      }
      // ── Colonne Qte ──
      ImGui::TableNextColumn();
      ImGui::Text("%d", items_[idx].amount);
      // ── Colonne Valeur (prix de vente NPC * quantité, optionnelle) ──
      if (show_value_col_) {
        ImGui::TableNextColumn();
        const long long val = price(items_[idx].id) * items_[idx].amount;
        if (val > 0) ImGui::Text("%lldz", val);
        else ImGui::TextDisabled("-");
      }
    }
    ImGui::EndTable();
  }

  // Pont de l'onglet de STORAGE actif : on mange la bordure HAUTE du tableau sur
  // sa seule largeur. Le blanc de l'onglet, celui des deux pixels d'air et celui
  // de la liste deviennent alors une seule surface — l'onglet ouvre le contenu
  // au lieu d'être posé dessus. Les autres restent fermés par la bordure.
  // 🔴 DESSINÉ ICI, après la table mais AVANT EndChild : en disposition verticale
  // la table vit dans le child « corps », dont la draw-list passe APRÈS celle de
  // la fenêtre. Tracé après EndChild — là où le pont des catégories l'est, parce
  // que son voisin ne peint rien à cet endroit — il finirait SOUS la bordure.
  if (have_stg_tab) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 body = ro::ListBodyColorU32();
    if (stg_tab_is_strip)
      dl->AddRectFilled(ImVec2(stg_tab_max.x - 1.0f, stg_tab_min.y + 1.0f),
                        ImVec2(table_left_x + 1.0f, stg_tab_max.y - 1.0f), body);
    else
      dl->AddRectFilled(ImVec2(stg_tab_min.x + 1.0f, table_top_y - 1.0f),
                        ImVec2(stg_tab_max.x - 1.0f, table_top_y + 1.0f), body);
  }

  // Fin du child « corps » de la disposition verticale (ouvert au moment des onglets).
  if (vertical_cats) ImGui::EndChild();

  // L'onglet actif « mange » le bord entre le strip et le contenu : petit pont sur
  // son bord droit -> passage continu, ce qui souligne la sélection (recette
  // inventory_viewer). VERTICAL SEULEMENT : en horizontal, ce qui suit la rangée
  // n'est pas un conteneur bordé mais la ligne « Sous-type » / le filtre, et le pont
  // débordait dessus. L'art actif/inactif suffit à marquer la sélection.
  if (have_active_tab && vertical_cats) {
    // Mode texte : couleur de l'onglet actif du thème (le fond est peint par nous).
    // Mode images : corps de l'art actif — blanc, sauf Favoris (gris-bleu).
    const ImU32 pont =
        !tab_images_ ? ImGui::GetColorU32(ImGuiCol_TabSelected)
        : kStgCats[cur_tab_].fav ? IM_COL32(0xD1, 0xDC, 0xE8, 255)
                                 : IM_COL32(255, 255, 255, 255);
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(active_tab_max.x - 1.0f, active_tab_min.y + 1.0f),
        ImVec2(active_tab_max.x + 2.0f, active_tab_max.y - 1.0f), pont);
  }

  // Description au SURVOL : simple mémorisation de la ligne survolée. L'aperçu est
  // un panneau RO SIMPLIFIÉ dessiné après la fenêtre (cf. plus bas) — pas la fenêtre
  // native, qu'il faudrait ouvrir/fermer et qui alourdit l'écran au moindre survol.
  hover_desc_id_ = hover_id;
  hover_desc_idx_ = hover_idx;

  // Compteur UNIQUE (plus de doublon en tête) : occupation du storage + nombre
  // d'items réellement affichés (onglet + sous-type + filtre).
  char cnt[48];
  std::snprintf(cnt, sizeof(cnt), "%d/%d  (%d affichés)", used_, max_,
                static_cast<int>(view.size()));

  // ── Footer btnbar (épinglé en bas de la fenêtre) : icône + compteur + Quitter.
  // Le grip de resize (dessiné par BeginRoWindow) tombe dans le coin bas-droit,
  // donc dans ce footer. La table a réservé kFooterH au-dessus.
  {
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const float fx0 = wp.x + st.WindowPadding.x;
    const float fx1 = wp.x + ws.x - st.WindowPadding.x;
    const float fy1 = wp.y + ws.y - st.WindowPadding.y;
    const float fy0 = fy1 - kFooterH;
    ro::DrawBar(fx0, fy0, fx1, fy1);
    const float iw = ro::DrawIconNum(fx0 + 6.0f, fy0 + (kFooterH - 14.0f) * 0.5f);
    const ImVec2 tsz = ImGui::CalcTextSize(cnt);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(fx0 + 6.0f + iw + 4.0f, fy0 + (kFooterH - tsz.y) * 0.5f),
        ImGui::GetColorU32(ImGuiCol_Text), cnt);
    // Bouton Quitter (RO) aligné à DROITE du footer -> ferme le storage (envoie
    // CZ_CloseKafra). Marge à droite pour ne pas recouvrir le grip de resize du coin.
    const float bw = 48.0f;
    ImGui::SetCursorScreenPos(ImVec2(fx1 - bw - 18.0f, fy0));
    HelpMarker(desc.c_str());
    SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
    if (ro::RoButton("Quitter", bw - 4.0f, kFooterH - 4.0f)) SendCloseStorage();
  }

  // DRAG d'un item storage : suit le curseur ; au relâché, la CIBLE décide du sens :
  //   - lâché sur l'INVENTAIRE -> retrait (storage -> inventaire)
  //   - lâché sur le CART       -> storage -> cart
  // 1 seul = direct ; pile = prompt quantité (comme le dépôt).
  if (drag_active_) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType("STG_ITEM")) {
      const ImVec2 m = ImGui::GetMousePos();
      drag_mx_ = m.x; drag_my_ = m.y;
    } else {  // drag terminé ce frame
      int action = -1;
      if (drag_index_ > 0) {
        // Lâcher DANS le storage = rangement interne, rien à router. Testé EN
        // PREMIER pour qu'une fenêtre posée dessous ne capte pas le drop (même
        // garde que l'inventaire et le cart).
        const bool over_self = drag_mx_ >= win_x_ && drag_my_ >= win_y_ &&
                               drag_mx_ < win_x_ + win_w_ && drag_my_ < win_y_ + win_h_;
        if (over_self) {
          // rien
        }
        else if (MouseOverInventory(drag_mx_, drag_my_)) action = kPendStoToInv;
        else if (MouseOverCart(drag_mx_, drag_my_))      action = kPendStoToCart;
      }
      if (action != -1) {
        pend_id_ = drag_index_;
        pend_index_ = drag_index_;
        pend_max_ = drag_amount_ > 0 ? drag_amount_ : 1;
        pend_action_ = action;
        pend_open_prompt_ = (pend_max_ > 1);
      }
      drag_active_ = false;
    }
  }

  // (Le redessin de l'icône d'un drag NATIF survolant le viewer a disparu avec le
  // reste du pont natif : le jeu rendait cette icône sous l'overlay ImGui, mais
  // plus aucun drag natif ne peut atteindre cette fenêtre.)

  ro::EndRoWindow();

  // ── Aperçu de description au SURVOL : TOOLTIP habillé RO ───────────────────
  // Un vrai tooltip, PAS une fenêtre : ImGui ordonne les fenêtres par focus, donc
  // une fenêtre ordinaire — même soumise après celle du viewer — passait DERRIÈRE
  // lui. Les tooltips, eux, sont placés dans la couche avant et sont toujours au
  // premier plan (et ImGui les recadre tout seul près des bords de l'écran).
  // Le cadre sysbox du client est peint à la main derrière le contenu.
  if (show_desc_tooltip_ && hover_desc_id_ != 0) {
    constexpr float kW = 330.0f;  // largeur max (wrap du texte)
    const float edge = ro::DescPanelEdge();
    // Fond BLANC ARRONDI peint par ImGui lui-même (comme la vraie fenêtre de desc),
    // et bordure ImGui SUPPRIMÉE : c'était elle, le liseré sombre à angles droits —
    // elle vient du style GLOBAL, les PushStyleVar du skin ayant déjà été dépilés
    // par EndRoWindow. Le cadre visible doit être l'art sysbox, rien d'autre.
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));   // sur fond clair
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);  // = BeginRoDescWindow
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(edge, edge));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(kW, ImGui::GetIO().DisplaySize.y * 0.8f));
    ImGui::BeginTooltip();
    // Canaux : le contenu part sur le canal 1, le cadre sur le 0 -> le cadre est
    // rendu DERRIÈRE alors qu'on ne connaît la taille de la fenêtre qu'après coup.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    // Cartes/options du STACK survolé (données d'instance) : la DB ne les connaît
    // pas, on les passe depuis l'item extrait. Index revalidé (items_ est
    // reconstruit à chaque tick).
    itemdesc::SimpleOpt sopts[5];
    const uint32_t* pcards = nullptr;
    const char* hname = nullptr;
    int ncards = 0, nopts = 0, hrefine = 0;
    bool hdamaged = false;
    if (hover_desc_idx_ >= 0 && hover_desc_idx_ < item_count_) {
      const Item& hit = items_[hover_desc_idx_];
      pcards = hit.cards;
      ncards = 4;
      nopts = hit.opt_count;
      hrefine = hit.refine;
      hdamaged = hit.damaged != 0;
      hname = hit.name;  // nom décoré (BuildDisplayName) + « [N] » ajouté par OnTick
      for (int k = 0; k < nopts && k < 5; ++k) {
        sopts[k].index = hit.opts[k].index;
        sopts[k].value = hit.opts[k].value;
        sopts[k].param = hit.opts[k].param;
      }
    }
    itemdesc::RenderSimpleDesc(hover_desc_id_, kW - 2.0f * edge, pcards, ncards,
                               sopts, nopts, hrefine, hname, hdamaged);
    dl->ChannelsSetCurrent(0);
    // Art sysbox SANS son fond (fill_bg=false) : le fond blanc arrondi est déjà
    // peint par ImGui, et celui de DrawDescPanelFrame est à angles droits — il
    // écraserait les coins ronds.
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ro::DrawDescPanelFrame(dl, wp.x, wp.y, wp.x + ws.x, wp.y + ws.y, false);
    dl->ChannelsMerge();
    ImGui::EndTooltip();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
  }
}
