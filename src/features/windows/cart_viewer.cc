#include "features/item_cell.h"
#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
#include "features/windows/cart_viewer.h"

#include "ui/game_texture.h"
// Icônes d'item : ro::ItemIcon (chargement, colorkey magenta et invalidation au
// reset de device sont partagés avec les autres viewers).
#include "ui/icon_cache.h"
#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <cstdio>
#include <cmath>    // std::fabs (recalage de la taille verrouillée sur les tuiles)
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bourgeon.h"                  // Bourgeon::Instance()
#include "features/windows/inventory_viewer.h"  // PointOverViewer (dépôt vers le viewer inventaire)
#include "features/windows/item_desc_window.h"  // itemdesc::RenderSimpleDesc (aperçu au survol)
#include "features/moonlight_ui/moonlight_ui.h"      // OpenInterfaceSection + HelpMarker
#include "features/windows/storage_window.h"    // PointOverViewer (dépôt vers le viewer storage)
#include "features/windows/vending_window.h"    // IsComposing (échoppe en cours -> chariot figé)
#include "d3d9/d3d9_hook.h"            // Overlay_DeviceEpoch
#include "imgui.h"
#include "ui/qty_prompt.h"             // ro::QuantityPrompt (dialogue « combien ? »)
#include "ui/ro_imgui.h"               // skin RO (BeginRoWindow / RoCheckbox / …)

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Constantes RE (client 20250716, base 0x400000) ─────────────────────────────
// RE 2026-07-27 (IDA) : UICartWnd_OnMsg 0x009576a0, UICartWnd_DrawContent 0x00948610,
// UICartWnd_OnRButtonDown 0x0094faa0, Cart_GetCount 0x00d5ce50, Cart_CopyItemAt
// 0x00d5c000. Cf. project_cart_window_imgui_todo.
namespace {

// Fenêtre cart native : id 40 (0x28), vtable 0x0103d538.
// ⚠ On la retrouve par le GESTIONNAIRE (uiwnd::FindWindow), PAS par un global.
// 0x0131f6a0, hérité d'une RE live et recopié dans inventory_viewer/storage_window,
// n'a AUCUNE référence dans le binaire (xrefs IDA 2026-07-27) : il ne porte pas la
// fenêtre cart. C'est ce qui donnait « ni le viewer ni la native » au premier
// essai — le hook de création masquait la native pendant que le viewer, aveugle,
// ne dessinait rien. Le client DÉTRUIT ses fenêtres à la fermeture
// (SaveWindowRect -> QueueDestroyWindow), donc FindWindow non-nul == ouverte.
constexpr uintptr_t kCartVTable = 0x0103d538;
constexpr int kWinCart    = 0x28;
constexpr int kOffWidth   = 0x14;
constexpr int kOffHeight  = 0x18;

// Modèle SESSION du cart (indépendant de la fenêtre => marche natif caché).
// RE : Cart_GetCount = *(g_session+0x1724) ; Cart_CopyItemAt parcourt la std::list
// @ g_session+0x1720. g_session = 0x015fa3c0 (inventaire = +0x16f0, storage = +0x1718).
constexpr uintptr_t kCartListHead = 0x015fbae0;  // sentinelle std::list (head)
constexpr int kNodeNext = 0x00;  // nœud : next
constexpr int kNodeInfo = 0x08;  // nœud : value = ItemSkillInfo
constexpr int kNodeAmt  = 0x18;  // nœud : quantité (= info+0x10)
// Champs DANS l'ItemSkillInfo (= node+0x08), identiques à l'inventaire :
constexpr int kInfoType   = 0x00;
constexpr int kInfoIndex  = 0x04;  // index CHARIOT (arg des commandes de transfert)
constexpr int kInfoIdStr  = 0x2c;  // std::string id (le jeu fait atoi dessus)
constexpr int kInfoIdCap  = 0x40;  // capacité SSO de la std::string id
constexpr int kInfoIdent  = 0x5c;  // byte : item identifié ?
constexpr int kInfoDamaged = 0x5d; // byte : équipement CASSÉ (rendu rouge, cf. itemcell)
constexpr int kInfoRefine = 0x60;

// Compteurs du footer NATIF (RE UICartWnd_DrawContent) : nb d'items et poids, avec
// leur max. Le natif passe le texte en rouge dès que cur >= max — on fait pareil.
constexpr uintptr_t kCartNumItems  = 0x015fb2d4;
constexpr uintptr_t kCartMaxItems  = 0x015fb2d8;
constexpr uintptr_t kCartWeight    = 0x015fb2dc;
constexpr uintptr_t kCartMaxWeight = 0x015fb2e0;

// Nom d'affichage (refine/cartes/enchant), comme l'inventaire et le storage.

// Construit le nom d'affichage d'un item sous SEH ISOLÉ : un item dont
// BuildDisplayName plante ne doit pas avorter TOUTE l'énumération (leçon de
// l'inventaire, où un seul item fautif faisait disparaître la moitié de la liste).

// ── Helpers vtable / fenêtres ────────────────────────────────────────────────

constexpr int kMsgUiAction = 0x06;   // OnMsg : action de contrôle…
constexpr int kActionClose = 0xc9;   // …201 = fermeture (RE UICartWnd_OnMsg case 6)

// Dispatcher (CMode) : FUN_00a75340(0x1213338) -> objet mode actif (0 hors jeu).
// Son vtbl+0x18 = CMode::SendMsg. Commandes de transfert RE'ées sur la fenêtre
// cart elle-même (UICartWnd_OnRButtonDown branche ALT / OnMsg case 38).
constexpr int kVfDispCmd     = 0x18;
constexpr int kCmdCartToBody    = 0x4d;  // cart -> inventaire
constexpr int kCmdCartToStorage = 0x4f;  // cart -> storage (storage ouvert)
using GetMode_t = void*(__fastcall*)(int);
using DispCmd_t = void(__thiscall*)(void*, int, int, int, int, int);

// Autres fenêtres, pour router un transfert (cible ouverte ou non) :
// uiwnd::kInventoryWndSlot / kStorageWndSlot et leurs vtables.

// Lit un pointeur de fenêtre valide depuis un slot (vtable vérifiée). SEH.
uint8_t* ReadValidWnd(uintptr_t slot, uintptr_t expected_vtable) {
  return uiwnd::WndAtSlot(slot, expected_vtable);
}

// ⚠ Une fenêtre native CACHÉE (uiwnd::kOffVisible = 0) n'est PAS une cible de dépôt : en
// « Interface moderne » les natives inventaire/storage/cart sont masquées mais
// gardent leur rect, et sans ce test leur emplacement fantôme capturait les drops
// faits sur les viewers ImGui posés par-dessus (un lâcher sur l'inventaire partait
// au storage). Le flag porte exactement ce sens côté client : hors rendu ET hors
// hit-test.
bool MouseOverWnd(uintptr_t slot, uintptr_t vt, float x, float y) {
  uint8_t* w = ReadValidWnd(slot, vt);
  if (!w) return false;
  __try {
    if (*reinterpret_cast<int*>(w + uiwnd::kOffVisible) == 0) return false;
    const int wx = *reinterpret_cast<int*>(w + uiwnd::kOffPosX);
    const int wy = *reinterpret_cast<int*>(w + uiwnd::kOffPosY);
    const int ww = *reinterpret_cast<int*>(w + kOffWidth);
    const int wh = *reinterpret_cast<int*>(w + kOffHeight);
    return x >= wx && y >= wy && x < wx + ww && y < wy + wh;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// La fenêtre cart ouverte, ou nullptr. Passe par le gestionnaire (cf. la note
// sur kCartVTable) et vérifie quand même la vtable : un id ne garantit pas la
// classe si un portage de client renumérote les fenêtres.
uint8_t* CartWnd() {
  __try {
    auto* w = reinterpret_cast<uint8_t*>(uiwnd::FindWindow(kWinCart));
    if (!w) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(w) != kCartVTable) return nullptr;
    return w;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool InventoryOpen() { return ReadValidWnd(uiwnd::kInventoryWndSlot, uiwnd::kInventoryWndVTable) != nullptr; }
bool StorageOpen()   { return ReadValidWnd(uiwnd::kStorageWndSlot, uiwnd::kStorageWndVTable) != nullptr; }

// Composition d'échoppe en cours. Le serveur lève alors `sd->state.prevend` et
// REFUSE EN SILENCE tout mouvement touchant le chariot : pc_getitemfromcart /
// pc_putitemtocart et storage_storageaddfromcart / storage_storagegettocart le
// testent explicitement, et pc_cant_act2() l'inclut. Autrement dit, TOUT ce que
// cette fenêtre sait faire est mort tant qu'une échoppe se compose.
bool VendingComposing() {
  auto* vending = Bourgeon::Instance().vending_window();
  return vending && vending->IsComposing();
}

// Cibles de dépôt : la fenêtre NATIVE ou, si elle est remplacée par son viewer
// ImGui (native cachée => rect invalide), le rect du viewer.
bool OverInventory(float x, float y) {
  if (auto* iv = Bourgeon::Instance().inventory_viewer())
    if (iv->PointOverViewer(static_cast<int>(x), static_cast<int>(y))) return true;
  return MouseOverWnd(uiwnd::kInventoryWndSlot, uiwnd::kInventoryWndVTable, x, y);
}
bool OverStorage(float x, float y) {
  if (auto* st = Bourgeon::Instance().storage_window())
    if (st->PointOverViewer(static_cast<int>(x), static_cast<int>(y))) return true;
  return MouseOverWnd(uiwnd::kStorageWndSlot, uiwnd::kStorageWndVTable, x, y);
}

// Objet mode courant (dispatcher), ou nullptr hors d'un mode jouable. SEH-gardé.
void* Dispatcher() {
  __try {
    return reinterpret_cast<GetMode_t>(rag::kModeMgrGetActiveAddr)(static_cast<int>(rag::kModeMgrAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Envoie une commande UI native (transfert) via le dispatcher.
void SendCmd(int cmd, int index, int amount) {
  if (amount <= 0) return;
  // Garde-fou : l'UI grise déjà ces actions pendant une composition d'échoppe,
  // mais un raccourci (double-clic, Alt+clic droit) ne passe pas par un widget
  // désactivé. Le serveur jetterait la requête sans un mot ; autant ne pas la
  // faire partir.
  if (VendingComposing()) return;
  __try {
    void* d = Dispatcher();
    if (d) uiwnd::Vf<DispCmd_t>(d, kVfDispCmd)(d, cmd, index, amount, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Ferme le cart comme le X natif : OnMsg(6, 201) -> UIWindowMgr_SaveWindowRect(40)
// (RE UICartWnd_OnMsg case 6). On rejoue le chemin natif au lieu d'appeler nous-mêmes
// une fonction du gestionnaire : aucune convention d'appel à deviner — c'est ce qui
// avait planté sur la fermeture de l'inventaire (cf. project_inventory_viewer_wip).
void CloseCart() {
  uint8_t* wnd = CartWnd();
  if (!wnd) return;
  __try {
    uiwnd::OnMsg(wnd, kMsgUiAction, kActionClose, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Description (clic droit) : on retrouve le nœud CHARIOT par son INDEX cart et on
// passe son ItemSkillInfo — celui que le serveur a rempli — à la fenêtre 0xc, qui
// rend alors cartes, refine et enchantements. item_desc_window détecte cette
// fenêtre et lui substitue sa version enrichie.
//
// ⚠ Par INDEX, jamais par id : deux exemplaires du même objet (même id, refines
// ou cartes différents) sont deux nœuds distincts, et une recherche par id
// rendrait toujours le PREMIER pour toutes leurs cases.
//
// DIFFÉRÉE au relâchement du bouton (itemcell::FlushDeferredDesc) : ouverte dès
// le clic, un appui PROLONGÉ faisait passer la description DERRIÈRE nous.
void OpenItemDesc(int index, int mx, int my) {
  itemcell::DeferDescFromIndex(kCartListHead, index, mx, my);
}

// Lecture SEH (POD only) des compteurs du footer -> hors OnRenderUI, qui contient
// des objets C++ (string/vector) et ne peut donc pas héberger de __try (C2712).
struct FooterVals { int num = 0, maxnum = 0, weight = 0, maxweight = 0; };
FooterVals ReadFooterVals() {
  FooterVals v;
  __try {
    v.num       = *reinterpret_cast<int*>(kCartNumItems);
    v.maxnum    = *reinterpret_cast<int*>(kCartMaxItems);
    v.weight    = *reinterpret_cast<int*>(kCartWeight);
    v.maxweight = *reinterpret_cast<int*>(kCartMaxWeight);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return v;
}

// ── Onglets de catégorie (mêmes familles de types que l'inventaire) ───────────
// Pas d'onglet « Favoris » : le flag favori (info+0x74) est propre à l'inventaire,
// le cart n'en a pas.
struct Cat { const char* label; const int* types; int n; const char* img; };
const int kUse[]   = {0, 1, 2, 0x12};
const int kEquip[] = {4, 5, 8, 9, 0xb, 0xc, 0xd, 0xe, 0xf};
const int kEtc[]   = {3, 7};
const int kAmmo[]  = {10, 0x10, 0x11, 0x13};
const int kCard[]  = {6};
const Cat kCats[] = {
    {"Tout",   nullptr, 0, "tab_all"},
    {"Conso",  kUse,    4, "tab_use"},
    {"Equip",  kEquip,  9, "tab_cos"},
    {"Ammo",   kAmmo,   4, "tab_ammo"},
    {"Etc",    kEtc,    2, "tab_etc"},
    {"Cartes", kCard,   1, "tab_card"},
};
constexpr int kNumCats = 6;

// Convertit une texture jeu en ImTextureID ImGui.
inline ImTextureID TexId(void* t) { return reinterpret_cast<ImTextureID>(t); }

// ── Assets natifs : barre 3-slice + fond de tuile + icônes du footer ───────────
// Préfixe CP949 pris sur les strings de l'exe (jamais reconstruit à la main).
constexpr uintptr_t kBtnbarPath     = 0x010357b8;  // …\basic_interface\btnbar_left.bmp
constexpr uintptr_t kIconWeightPath = 0x0103db00;  // …\inventory\icon_weight.bmp
constexpr uintptr_t kIconNumPath    = 0x0103dad4;  // …\inventory\icon_num.bmp

using BarTex = ro::GameTexture;
BarTex g_bar[3];      // btnbar 3-slice : 0=left, 1=mid, 2=right
BarTex g_tile;        // itemwin_mid.bmp : fond de tuile d'item (32px)
BarTex g_ico_weight;  // icon_weight.bmp (footer)
BarTex g_ico_num;     // icon_num.bmp (footer)
BarTex g_tab[kNumCats][2];   // onglets VERTICAUX   [cat][0=actif, 1=inactif]
BarTex g_tabh[kNumCats][2];  // onglets HORIZONTAUX (jeu tabh_*)
bool   g_assets_tried = false;

// `<préfixe basic_interface\> + <file>`, préfixe repris de la string exe du btnbar.
void BasicInterfacePath(const char* file, char* out, size_t out_sz) {
  const char* base = reinterpret_cast<const char*>(kBtnbarPath);
  const char* slash = std::strrchr(base, '\\');
  const size_t n = slash ? static_cast<size_t>(slash - base + 1) : 0;
  if (n && n < out_sz) std::memcpy(out, base, n);
  std::snprintf(out + n, out_sz - n, "%s", file);
}

void LoadAssets() {
  if (g_assets_tried) return;
  g_assets_tried = true;
  char path[160];
  const char* names[3] = {"btnbar_left3.bmp", "btnbar_mid3.bmp", "btnbar_right3.bmp"};
  for (int i = 0; i < 3; ++i) {
    BasicInterfacePath(names[i], path, sizeof(path));
    g_bar[i] = ro::TextureFromGameFile(path);
  }
  BasicInterfacePath("itemwin_mid.bmp", path, sizeof(path));
  g_tile = ro::TextureFromGameFile(path);
  g_ico_weight = ro::TextureFromGameFile(reinterpret_cast<const char*>(kIconWeightPath));
  g_ico_num    = ro::TextureFromGameFile(reinterpret_cast<const char*>(kIconNumPath));
  for (int c = 0; c < kNumCats; ++c) {
    const char* base = kCats[c].img;
    if (!base) continue;
    char nm[48];
    std::snprintf(nm, sizeof(nm), "%s1.bmp", base);
    BasicInterfacePath(nm, path, sizeof(path)); g_tab[c][0] = ro::TextureFromGameFile(path);
    std::snprintf(nm, sizeof(nm), "%s2.bmp", base);
    BasicInterfacePath(nm, path, sizeof(path)); g_tab[c][1] = ro::TextureFromGameFile(path);
    char hbase[40];
    std::snprintf(hbase, sizeof(hbase), "tabh%s", base + 3);  // saute « tab »
    std::snprintf(nm, sizeof(nm), "%s1.bmp", hbase);
    BasicInterfacePath(nm, path, sizeof(path)); g_tabh[c][0] = ro::TextureFromGameFile(path);
    std::snprintf(nm, sizeof(nm), "%s2.bmp", hbase);
    BasicInterfacePath(nm, path, sizeof(path)); g_tabh[c][1] = ro::TextureFromGameFile(path);
  }
}

// Teinte des AddImage = luminosité + opacité du skin RO (les images du jeu sont
// dessinées en draw-list brut, elles échapperaient sinon à ces réglages).

// Barre 3-slice du footer dans [x0..x1] à y0 (hauteur barH). Repli rect plein RO.
void DrawFooterBar(ImDrawList* dl, float x0, float y0, float x1, float barH) {
  const float y1 = y0 + barH;
  const float lw = g_bar[0].w > 0 ? static_cast<float>(g_bar[0].w) : 0.0f;
  const float rw = g_bar[2].w > 0 ? static_cast<float>(g_bar[2].w) : 0.0f;
  if (g_bar[1].tex) {
    const ImU32 t = ro::SkinImageTint();
    if (g_bar[0].tex) dl->AddImage(TexId(g_bar[0].tex), ImVec2(x0, y0), ImVec2(x0 + lw, y1), ImVec2(0, 0), ImVec2(1, 1), t);
    dl->AddImage(TexId(g_bar[1].tex), ImVec2(x0 + lw, y0), ImVec2(x1 - rw, y1), ImVec2(0, 0), ImVec2(1, 1), t);  // étiré
    if (g_bar[2].tex) dl->AddImage(TexId(g_bar[2].tex), ImVec2(x1 - rw, y0), ImVec2(x1, y1), ImVec2(0, 0), ImVec2(1, 1), t);
  } else {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), 2.0f);
  }
}

// Icône BarTex centrée verticalement à (x, cy) ; renvoie sa largeur (0 si absente).
float DrawFooterIcon(ImDrawList* dl, const BarTex& ic, float x, float cy) {
  if (!ic.tex || ic.w <= 0 || ic.h <= 0) return 0.0f;
  dl->AddImage(TexId(ic.tex), ImVec2(x, cy - ic.h * 0.5f),
               ImVec2(x + ic.w, cy + ic.h * 0.5f), ImVec2(0, 0), ImVec2(1, 1), ro::SkinImageTint());
  return static_cast<float>(ic.w);
}

// Largeur du strip d'onglets VERTICAL / hauteur de la rangée HORIZONTALE : la
// dimension TRANSVERSE (jamais étirée), l'autre se déduit du ratio de l'image.
float TabStripWidth() {
  float w = 0.0f;
  for (int c = 0; c < kNumCats; ++c)
    for (int s = 0; s < 2; ++s)
      if (g_tab[c][s].w > w) w = static_cast<float>(g_tab[c][s].w);
  return w > 0.0f ? w : 22.0f;
}
float TabStripHeightH() {
  float h = 0.0f;
  for (int c = 0; c < kNumCats; ++c)
    for (int s = 0; s < 2; ++s)
      if (g_tabh[c][s].h > h) h = static_cast<float>(g_tabh[c][s].h);
  return h > 0.0f ? h : 22.0f;
}

// itemwin_mid PAVÉ (répété à sa taille native) dans [mn..mx], aligné sur `origin`
// (la 1re tuile) pour que le fond et les items partagent la même marge.
void DrawTiledBg(ImDrawList* dl, const BarTex& tile, ImVec2 origin, ImVec2 mn, ImVec2 mx) {
  if (!tile.tex || tile.w <= 0 || tile.h <= 0) {
    dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGuiCol_FrameBg));
    return;
  }
  const float tw = static_cast<float>(tile.w), th = static_cast<float>(tile.h);
  auto floorTo = [](float v, float o, float step) {
    int n = static_cast<int>((v - o) / step);
    if (o + n * step > v) --n;
    return o + n * step;
  };
  const float sx = floorTo(mn.x, origin.x, tw);
  const float sy = floorTo(mn.y, origin.y, th);
  dl->PushClipRect(mn, mx, true);
  for (float y = sy; y < mx.y; y += th)
    for (float x = sx; x < mx.x; x += tw)
      dl->AddImage(TexId(tile.tex), ImVec2(x, y), ImVec2(x + tw, y + th),
                   ImVec2(0, 0), ImVec2(1, 1), ro::SkinImageTint());
  dl->PopClipRect();
}

// ── Resize par PALIER de tuile (même mécanique que l'inventaire) ──────────────
// La fenêtre saute d'une COLONNE / LIGNE de tuiles : jamais de colonne partielle.
struct SnapState { float cell = 32, gap = 0, chromew = 0, chromeh = 0; bool valid = false; };
SnapState g_snap;
void SnapWindowSize(ImGuiSizeCallbackData* d) {
  const SnapState& s = g_snap;
  const float step = s.cell + s.gap;
  int cols = static_cast<int>((d->DesiredSize.x - s.chromew + s.gap) / step + 0.5f);
  int rows = static_cast<int>((d->DesiredSize.y - s.chromeh + s.gap) / step + 0.5f);
  if (cols < 5) cols = 5;
  if (rows < 5) rows = 5;
  d->DesiredSize.x = s.chromew + cols * step - s.gap;
  d->DesiredSize.y = s.chromeh + rows * step - s.gap;
}

// Vide les caches de textures quand le device D3D a été reset/recréé (handles morts).
unsigned g_tex_epoch = 0;
void MaybeFlushTextures() {
  const unsigned e = Overlay_DeviceEpoch();
  if (e == g_tex_epoch) return;
  g_tex_epoch = e;
  for (auto& b : g_bar) b = BarTex{};
  g_tile = g_ico_weight = g_ico_num = BarTex{};
  for (auto& row : g_tab)  for (auto& b : row) b = BarTex{};
  for (auto& row : g_tabh) for (auto& b : row) b = BarTex{};
  g_assets_tried = false;
}

// Aperçu de description RO au survol (le même que l'inventaire) : tooltip
// couche-avant, fond blanc arrondi + cadre sysbox peint derrière via un split de
// canaux. Appelé À L'EXTÉRIEUR de toute fenêtre (il crée son popup).

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════

// Cache la fenêtre native DÈS sa création (avant le 1er rendu) -> zéro flicker.
void CartViewer::HideNativeAtCreation(void* win) {
  if (!win || !imgui_enabled_) return;
  __try {
    if (*reinterpret_cast<uintptr_t*>(win) != kCartVTable) return;
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + uiwnd::kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Remplit items_/item_count_ depuis le MODÈLE SESSION du cart (0x015fbae0), donc
// marche fenêtre native cachée. POD-only sous SEH ; le nom complet passe par
// itemcell::BuildDisplayName, avec la fenêtre cart native comme contexte.
void CartViewer::Extract() {
  item_count_ = 0;
  void* wnd = CartWnd();  // contexte `this` de BuildDisplayName (peut être nullptr)
  uint8_t* head = nullptr;
  uint8_t* node = nullptr;
  __try {
    head = *reinterpret_cast<uint8_t**>(kCartListHead);
    if (head) node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
  if (!head) return;

  int guard = 0;
  while (node && node != head && item_count_ < kMaxItems && guard < kMaxItems) {
    // Pointeur SUIVANT lu D'ABORD sous garde : un nœud corrompu arrête net (pas de
    // boucle infinie ni de faute) sans perdre les items déjà lus.
    uint8_t* next = nullptr;
    __try { next = *reinterpret_cast<uint8_t**>(node + kNodeNext); }
    __except (EXCEPTION_EXECUTE_HANDLER) { break; }

    // Extraction PAR ITEM sous SEH ISOLÉ -> une faute est confinée à l'item (sauté),
    // l'énumération continue (même politique que l'inventaire).
    __try {
      Item& it = items_[item_count_];
      uint8_t* info = node + kNodeInfo;
      const uint32_t idcap = *reinterpret_cast<uint32_t*>(info + kInfoIdCap);
      const char* ids = (idcap > 0xf) ? *reinterpret_cast<char**>(info + kInfoIdStr)
                                      : reinterpret_cast<const char*>(info + kInfoIdStr);
      it.id = ids ? static_cast<uint32_t>(atoi(ids)) : 0;
      it.identified = *reinterpret_cast<uint8_t*>(info + kInfoIdent);
      it.damaged = *reinterpret_cast<uint8_t*>(info + kInfoDamaged);
      it.amount = *reinterpret_cast<int*>(node + kNodeAmt);
      it.index  = *reinterpret_cast<int*>(info + kInfoIndex);
      it.refine = *reinterpret_cast<int*>(info + kInfoRefine);
      it.type   = *reinterpret_cast<int*>(info + kInfoType);
      for (int k = 0; k < 4; ++k)
        it.cards[k] = *reinterpret_cast<uint32_t*>(info + 0x1c + k * 4);
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
      itemcell::BuildDisplayName(wnd, info, it.name, sizeof(it.name));
      it.total_slots = itemcell::SlotCount(info);
      ++item_count_;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    node = next;
    ++guard;
  }
}

void CartViewer::OnTick() {
  open_ = false;
  uint8_t* wnd = CartWnd();
  if (wnd) {
    __try {
      if (!was_open_) {
        spawn_x_ = *reinterpret_cast<int*>(wnd + uiwnd::kOffPosX);
        spawn_y_ = *reinterpret_cast<int*>(wnd + uiwnd::kOffPosY);
        need_pos_ = true;
      }
      // Master switch : ON -> native masquée (hors rendu ET hit-test) + viewer ;
      // OFF -> native seule, aucun viewer. Forcé chaque tick (le natif remet à 1).
      *reinterpret_cast<int*>(wnd + uiwnd::kOffVisible) = imgui_enabled_ ? 0 : 1;
      open_ = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { open_ = false; }
    if (open_) Extract();
  }
  // Aperçu de description : purgé dès que le viewer ne dessine plus, sinon il
  // resterait affiché sans rien pour l'effacer.
  if (!open_ || !imgui_enabled_) { hover_desc_id_ = 0; hover_desc_idx_ = -1; }
  if (!open_) { win_valid_ = false; drag_active_ = false; }
  was_open_ = open_;
}

void CartViewer::OnRenderUI() {
  if (!open_ || !imgui_enabled_) return;
  MaybeFlushTextures();  // device reset/TDR -> lâche les handles morts

  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(spawn_x_),
                                   static_cast<float>(spawn_y_)),
                            ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(300, 300), ImGuiCond_FirstUseEver);
  if (g_snap.valid && !lock_size_) {
    const float minGrid = 5.0f * (g_snap.cell + g_snap.gap) - g_snap.gap;  // min 5 tuiles
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(g_snap.chromew + minGrid, g_snap.chromeh + minGrid),
        ImVec2(10000.0f, 10000.0f), SnapWindowSize);
  } else if (g_snap.valid && win_valid_) {
    // Taille VERROUILLÉE : le callback de snap ne tourne plus (il n'agit que pendant
    // un redimensionnement), donc la fenêtre resterait sur une hauteur quelconque —
    // dernière ligne coupée. On la recale une fois sur le palier le plus proche.
    const float step = g_snap.cell + g_snap.gap;
    int cols = static_cast<int>((win_w_ - g_snap.chromew + g_snap.gap) / step + 0.5f);
    int rows = static_cast<int>((win_h_ - g_snap.chromeh + g_snap.gap) / step + 0.5f);
    if (cols < 5) cols = 5;
    if (rows < 5) rows = 5;
    const ImVec2 snapped(g_snap.chromew + cols * step - g_snap.gap,
                         g_snap.chromeh + rows * step - g_snap.gap);
    if (std::fabs(snapped.x - win_w_) > 0.5f || std::fabs(snapped.y - win_h_) > 0.5f)
      ImGui::SetNextWindowSize(snapped, ImGuiCond_Always);
  }

  // Bullet de la barre de titre = raccourci vers la config de CETTE fenêtre.
  ro::SetNextWindowTitleBullet("Options du cart");
  const bool begun = ro::BeginRoWindow(
      "Cart###bourgeon_cart", &show_panel_,
      lock_size_ ? ImGuiWindowFlags_NoResize : 0);
  if (ro::TitleBulletClicked())
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(MoonlightUi::kIfaceCart);
  // X du viewer -> ferme le cart natif (client-side). Réarme show_panel_.
  if (!show_panel_) { CloseCart(); show_panel_ = true; }
  if (!begun) { ro::EndRoWindow(); return; }

  // Pendant la composition d'un shop, le serveur refuse TOUT mouvement touchant
  // le cart : autant l'annoncer une fois en clair, en plus des entrées grisées.
  if (VendingComposing())
    ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                       "Shop en composition : les transferts sont figés.");

  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  win_x_ = wp.x; win_y_ = wp.y; win_w_ = ws.x; win_h_ = ws.y;
  win_valid_ = true;

  // ── Action en attente (transfert d'une pile) -> prompt quantité ──
  auto do_move = [this](int amount) {
    switch (pend_action_) {
      case kPendToBody:    SendCmd(kCmdCartToBody, pend_index_, amount); break;
      case kPendToStorage: SendCmd(kCmdCartToStorage, pend_index_, amount); break;
      default: break;
    }
  };
  if (pend_id_ != 0) {
    if (pend_open_prompt_) { ro::OpenQuantityPrompt(this); pend_open_prompt_ = false; }
    else if (pend_max_ <= 1) { do_move(1); pend_id_ = 0; }
  }
  // Dialogue « combien ? » PARTAGÉ (ui/qty_prompt) : habillé RO, identique dans
  // l'inventaire, le storage et le cart.
  {
    const char* verb = pend_action_ == kPendToStorage ? "Vers le storage"
                                                      : "Vers l'inventaire";
    bool cancelled = false;
    const int qty = ro::QuantityPrompt(this, verb, pend_max_, &cancelled);
    if (qty > 0) { do_move(qty); pend_id_ = 0; }
    else if (cancelled) pend_id_ = 0;
  }

  // Aide raccourcis : le texte est construit ici, le « (?) » est émis dans le FOOTER
  // pour ne pas manger une ligne au-dessus de la grille.
  const char* kShortcuts =
      "Raccourcis cart\n\n"
      "- Double-clic gauche : retirer vers l'inventaire\n"
      "- Clic droit : menu contextuel\n"
      "- Ctrl + clic droit : description\n"
      "- Alt + clic droit : transfert rapide (storage si ouvert, sinon inventaire)\n"
      "- Glisser : lâcher sur l'inventaire ou le storage pour y transférer";
  static ImGuiTextFilter filter;
  if (show_filter_) {
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##cart_filter", "Filtrer...", filter.InputBuf,
                                 IM_ARRAYSIZE(filter.InputBuf)))
      filter.Build();
  } else if (filter.InputBuf[0]) {
    // Filtre masqué : on le vide, sinon il continuerait de cacher des items sans
    // que rien à l'écran ne l'explique.
    filter.Clear();
  }

  // ── Dimensions communes (footer réservé + snap) ──
  LoadAssets();
  const float lineH = ImGui::GetTextLineHeight();
  const float footerH = 2.0f * lineH + 6.0f;  // 2 lignes compactes (barre étirée dessous)
  const ImGuiStyle& style = ImGui::GetStyle();
  const float mainW = ImGui::GetWindowWidth();
  const float mainH = ImGui::GetWindowHeight();
  const float childH = -(footerH + style.ItemSpacing.y);

  // ── Onglets IMAGES (jeu tab_* en vertical, tabh_* en horizontal ; actif =
  //    <img>1.bmp, inactif = <img>2.bmp). Sans image chargée -> libellé texte.
  const bool vtabs = tabs_vertical_;
  const float tabW = TabStripWidth();
  const float tabH = TabStripHeightH();
  ImVec2 activeTabMin(0, 0), activeTabMax(0, 0);
  bool haveActiveTab = false;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("cart_tabs", vtabs ? ImVec2(tabW, childH) : ImVec2(0.0f, tabH),
                    false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                      vtabs ? ImVec2(0.0f, -1.0f) : ImVec2(-1.0f, 0.0f));
  {
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    for (int c = 0; c < kNumCats; ++c) {
      const bool sel = (cur_tab_ == c);
      ImGui::PushID(c);
      const BarTex (&set)[2] = vtabs ? g_tab[c] : g_tabh[c];
      const BarTex& img = set[sel ? 0 : 1].tex ? set[sel ? 0 : 1] : set[sel ? 1 : 0];
      if (!vtabs && c) ImGui::SameLine();
      if (img.tex && img.w > 0 && img.h > 0) {
        // Dimension TRANSVERSE fixée, l'autre déduite du ratio : jamais d'étirement.
        const float iw = vtabs ? tabW : tabH * img.w / static_cast<float>(img.h);
        const float ih = vtabs ? tabW * img.h / static_cast<float>(img.w) : tabH;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("tab", ImVec2(iw, ih))) cur_tab_ = c;
        const ImVec2 pe(p.x + iw, p.y + ih);
        tdl->AddImage(TexId(img.tex), p, pe, ImVec2(0, 0), ImVec2(1, 1), ro::SkinImageTint());
        if (!sel && ImGui::IsItemHovered())
          tdl->AddRectFilled(p, pe, IM_COL32(255, 255, 255, 45));  // survol : éclaircir
        if (sel) { activeTabMin = p; activeTabMax = pe; haveActiveTab = true; }
      } else {
        const ImVec2 sz = vtabs
            ? ImVec2(tabW, 0.0f)
            : ImVec2(ImGui::CalcTextSize(kCats[c].label).x +
                         style.FramePadding.x * 2.0f, tabH);
        if (ImGui::Selectable(kCats[c].label, sel, 0, sz)) cur_tab_ = c;
        if (sel) {
          activeTabMin = ImGui::GetItemRectMin();
          activeTabMax = ImGui::GetItemRectMax();
          haveActiveTab = true;
        }
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(" %s ", kCats[c].label);
      ImGui::PopID();
    }
  }
  ImGui::PopStyleVar();  // ItemSpacing
  ImGui::EndChild();
  ImGui::PopStyleVar();  // WindowPadding (strip)

  // ── Vue filtrée (onglet courant + recherche) ──
  // AUCUN tri : on garde l'ordre de la liste session, comme la fenêtre native
  // (UICartWnd_OnMsg case 23 ne trie pas) -> le tri serveur « Tri Cart » /
  // @tri_cart est reflété tel quel.
  auto in_tab = [](int tab, const Item& it) -> bool {
    const Cat& c = kCats[tab];
    if (!c.types) return true;  // Tout
    for (int i = 0; i < c.n; ++i) if (c.types[i] == it.type) return true;
    return false;
  };
  std::vector<int> view;
  view.reserve(item_count_);
  for (int i = 0; i < item_count_; ++i)
    if (in_tab(cur_tab_, items_[i]) && filter.PassFilter(items_[i].name))
      view.push_back(i);

  // ── Grille de tuiles 32px (fond itemwin_mid) : à DROITE des onglets en
  //    disposition verticale, EN DESSOUS en horizontale. Défile. ──
  if (vtabs) {
    ImGui::SameLine(0.0f, 0.0f);
  } else {
    // Collée à la rangée d'onglets : on reprend l'ItemSpacing vertical que le child
    // du strip vient d'insérer.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - style.ItemSpacing.y);
  }
  // Style du MENU CONTEXTUEL, mémorisé AVANT les push de la grille : celle-ci
  // tourne en WindowPadding 0 / ItemSpacing jointif (tuiles collées), et un popup
  // ouvert dans ce scope en hérite — entrées serrées, sans marge. Le menu du
  // storage, lui, est rendu au style normal de la fenêtre : c'est cette référence
  // qu'on repousse autour du popup (cf. plus bas).
  const ImVec2 menu_pad = style.WindowPadding;
  const ImVec2 menu_spacing = style.ItemSpacing;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("cartgrid", ImVec2(0.0f, childH), true,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
  {
    const float cell = 32.0f, gap = 0.0f;  // tuiles natives 32px, jointives
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const float availw = ImGui::GetContentRegionAvail().x;  // exclut déjà la scrollbar
    const float availh = ImGui::GetContentRegionAvail().y;
    // Mesure du chrome (fenêtre - zone grille) pour le snap de resize (frame +1).
    g_snap.cell = cell; g_snap.gap = gap;
    g_snap.chromew = mainW - availw;
    g_snap.chromeh = mainH - availh;
    g_snap.valid = true;
    int cols = static_cast<int>((availw + gap) / (cell + gap));
    if (cols < 1) cols = 1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {  // Fond itemwin_mid PAVÉ, ALIGNÉ sur la grille d'items.
      const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();  // = 1re tuile
      const ImVec2 gmn = ImGui::GetWindowPos();
      const ImVec2 gsz = ImGui::GetWindowSize();
      DrawTiledBg(dl, g_tile, gridOrigin, gmn, ImVec2(gmn.x + gsz.x, gmn.y + gsz.y));
    }

    // Aperçu de description au survol : recalculé à chaque frame (0 = aucune case).
    hover_desc_id_ = 0;
    hover_desc_idx_ = -1;
    const int nitems = static_cast<int>(view.size());
    for (int k = 0; k < nitems; ++k) {
      if (k % cols != 0) ImGui::SameLine();
      const int idx = view[k];
      const Item& it = items_[idx];
      // ID semé par l'index CHARIOT stable (pas la position volatile) : si la liste
      // est renumérotée pendant un glisser, le drag reste collé au bon item.
      ImGui::PushID(it.index);
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton("cell", ImVec2(cell, cell));
      const bool hovered = ImGui::IsItemHovered();
      const ImVec2 p1 = ImVec2(p0.x + cell, p0.y + cell);

      if (hovered)  // survol : léger éclaircissement (le HeaderHovered du skin est BLEU)
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 55), 0.0f);

      // Tuile de grille (icône centrée + badge coin) : brique partagée,
      // cf. features/item_cell.h — même rendu ici et dans le chariot.
      const ro::IconTex ic = ro::ItemIcon(it.id, it.identified);
      itemcell::DrawTile(dl, p0, p1, cell, ic, it.refine, it.amount,
                         it.damaged != 0);

      if (hovered) {
        if (show_desc_tooltip_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            ImGui::GetDragDropPayload() == nullptr) {
          hover_desc_id_ = it.id;
          hover_desc_idx_ = idx;
        } else if (!show_desc_tooltip_) {
          ImGui::BeginTooltip();
          char lbl[96], padded[100];
          std::snprintf(padded, sizeof(padded), " %s ",
                        itemcell::Label(lbl, sizeof(lbl), it.name,
                                        it.total_slots));
          itemcell::NameText(padded, it.damaged != 0);
          if (it.amount > 1) ImGui::TextDisabled(" Quantité : %d ", it.amount);
          ImGui::EndTooltip();
        }
        // Double-clic = retirer vers l'inventaire. La native n'a PAS de double-clic
        // (slot vtable par défaut) ; c'est un confort du viewer, symétrique du
        // double-clic « utiliser/équiper » de l'inventaire.
        // Entrepôt ouvert : le serveur refuse cart -> inventaire, on bascule donc
        // vers le storage — exactement l'arbitrage du natif (ALT + clic droit
        // envoie au storage dès qu'il est ouvert, sinon à l'inventaire).
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
          SendCmd(StorageOpen() ? kCmdCartToStorage : kCmdCartToBody, it.index,
                  it.amount);
      }

      // Raccourcis (miroir de UICartWnd_OnRButtonDown) :
      //   Ctrl + clic DROIT = description directe ;
      //   Alt  + clic DROIT = transfert rapide (storage ouvert -> 0x4f, sinon 0x4d) ;
      //   clic DROIT seul   = menu contextuel.
      const ImGuiIO& mods = ImGui::GetIO();
      if (IsLastItemRightClicked()) {
        if (mods.KeyCtrl) {
          POINT pt; if (GetCursorPos(&pt)) OpenItemDesc(it.index, pt.x, pt.y);
        } else if (mods.KeyAlt) {
          if (StorageOpen())        SendCmd(kCmdCartToStorage, it.index, it.amount);
          else if (InventoryOpen()) SendCmd(kCmdCartToBody, it.index, it.amount);
        } else {
          ImGui::OpenPopup("ctx");
        }
      }

      // Source de drag (inventaire / storage selon la cible du relâché).
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        drag_active_ = true;
        drag_index_ = it.index; drag_amount_ = it.amount;
        ImGui::SetDragDropPayload("CART_ITEM", &idx, sizeof(idx));
        if (ic.tex) { ImGui::Image(TexId(ic.tex), ImVec2(24, 24)); ImGui::SameLine(); }
        ImGui::TextUnformatted(it.name[0] ? it.name : "(?)");
        // Survol d'une cible que le serveur refusera : on le dit PENDANT le glisser,
        // seul moment où l'on peut encore renoncer (entrepôt ouvert = pas de
        // cart -> inventaire).
        const ImVec2 drag_mouse = ImGui::GetMousePos();
        if (VendingComposing())
          ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                             "Shop en composition : le cart est figé");
        else if (StorageOpen() && OverInventory(drag_mouse.x, drag_mouse.y))
          ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                             "Storage ouvert : vers l'inventaire impossible");
        ImGui::EndDragDropSource();
      }

      // Menu au style NORMAL de la fenêtre (marges + espacement), pas au style
      // jointif de la grille dans laquelle il est ouvert.
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, menu_pad);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, menu_spacing);
      if (ImGui::BeginPopup("ctx")) {
        char lbl[96];
        // En-tête grisé, avec l'ombre rouge du natif si l'item est cassé.
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        itemcell::NameText(itemcell::Label(lbl, sizeof(lbl), it.name,
                                           it.total_slots),
                           it.damaged != 0);
        ImGui::PopStyleColor();
        ImGui::Separator();
        if (ImGui::MenuItem("Description")) {
          POINT pt; if (GetCursorPos(&pt)) OpenItemDesc(it.index, pt.x, pt.y);
        }
        ImGui::Separator();
        // ⚠ RÈGLE SERVEUR : tant que l'entrepôt est ouvert, le serveur REFUSE tout
        // mouvement cart <-> inventaire — clif_parse_GetItemFromCart (CZ 0x0127)
        // passe par pc_cant_act2(), qui inclut state.storage_flag. Le paquet part
        // mais est jeté en silence. On grise donc l'entrée en le disant, plutôt que
        // de laisser un clic sans effet (le natif fait la même chose autrement :
        // son ALT+clic droit bascule sur « vers le storage » dès qu'il est ouvert).
        // Même nature que `storage_open` : une règle SERVEUR qu'on rend visible
        // au lieu de laisser un clic sans effet.
        const bool vending_lock = VendingComposing();
        const bool storage_open = StorageOpen();
        const bool to_body_off = storage_open || vending_lock;
        // Retrait vers l'inventaire : une PILE ouvre le prompt de quantité.
        if (it.amount <= 1) {
          if (ImGui::MenuItem("Vers l'inventaire", nullptr, false, !to_body_off))
            SendCmd(kCmdCartToBody, it.index, 1);
        } else if (ImGui::MenuItem("Vers l'inventaire...", nullptr, false, !to_body_off)) {
          pend_id_ = it.index; pend_index_ = it.index; pend_max_ = it.amount;
          pend_action_ = kPendToBody; pend_open_prompt_ = true;
        }
        if (to_body_off && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip(
              vending_lock
                  ? "Impossible pendant la composition d'un shop (règle du\n"
                    "serveur). Ouvrez ou annulez le shop d'abord."
                  : "Impossible tant que le storage est ouvert (règle du serveur).\n"
                    "Fermez le storage, ou faites transiter l'objet par le storage.");
        if (storage_open) {
          if (it.amount <= 1) {
            if (ImGui::MenuItem("Vers le storage", nullptr, false, !vending_lock))
              SendCmd(kCmdCartToStorage, it.index, 1);
          } else if (ImGui::MenuItem("Vers le storage...", nullptr, false, !vending_lock)) {
            pend_id_ = it.index; pend_index_ = it.index; pend_max_ = it.amount;
            pend_action_ = kPendToStorage; pend_open_prompt_ = true;
          }
        }
        // alootid : ramassage auto par ID (même entrée que l'inventaire).
        if (auto* mui = Bourgeon::Instance().moonlight_ui()) {
          ImGui::Separator();
          const bool inAloot = mui->IsAlootId(it.id);
          if (ImGui::MenuItem(inAloot ? "Retirer de l'alootid" : "Ajouter à l'alootid")) {
            if (inAloot) mui->RemoveAlootId(it.id); else mui->AddAlootId(it.id);
          }
        }
        ImGui::EndPopup();
      }
      ImGui::PopStyleVar(2);  // WindowPadding + ItemSpacing du menu

      ImGui::PopID();
    }
    ImGui::PopStyleVar();  // ItemSpacing
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();  // WindowPadding (grille)

  // Onglet actif « mange » le bord entre le strip et la grille : petit pont sur le
  // bord qui touche la grille (droit en vertical, bas en horizontal) -> passage
  // blanc continu qui souligne l'onglet actif.
  if (haveActiveTab) {
    const ImU32 pont = IM_COL32(255, 255, 255, 255);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (vtabs)
      dl->AddRectFilled(ImVec2(activeTabMax.x - 1.0f, activeTabMin.y + 1.0f),
                        ImVec2(activeTabMax.x + 2.0f, activeTabMax.y - 1.0f), pont);
    else
      dl->AddRectFilled(ImVec2(activeTabMin.x + 1.0f, activeTabMax.y - 1.0f),
                        ImVec2(activeTabMax.x - 1.0f, activeTabMax.y + 2.0f), pont);
  }

  // ── Drag terminé : router selon la cible (inventaire / storage) ──
  if (drag_active_) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType("CART_ITEM")) {
      const ImVec2 m = ImGui::GetMousePos();
      drag_mx_ = m.x; drag_my_ = m.y;
    } else {  // relâché ce frame
      int action = -1;
      if (drag_index_ > 0) {
        const bool over_self = drag_mx_ >= win_x_ && drag_my_ >= win_y_ &&
                               drag_mx_ < win_x_ + win_w_ && drag_my_ < win_y_ + win_h_;
        if (!over_self) {
          if (OverStorage(drag_mx_, drag_my_))        action = kPendToStorage;
          // Entrepôt ouvert => le serveur refuse cart -> inventaire (storage_flag,
          // cf. le menu contextuel) : on n'arme RIEN, plutôt que d'ouvrir un prompt
          // de quantité dont la validation partirait à la poubelle.
          else if (OverInventory(drag_mx_, drag_my_) && !StorageOpen())
            action = kPendToBody;
        }
      }
      if (action != -1) {
        pend_id_ = drag_index_; pend_index_ = drag_index_;
        pend_max_ = drag_amount_ > 0 ? drag_amount_ : 1;
        pend_action_ = action;
        pend_open_prompt_ = (pend_max_ > 1);
      }
      drag_active_ = false;
    }
  }

  // ── Footer : barre 3-slice portant le poids (ligne 1) et le compteur d'items
  //    (ligne 2), avec les icônes NATIVES (icon_weight / icon_num) et le même code
  //    couleur que le natif (rouge dès que la valeur atteint son max, RE
  //    UICartWnd_DrawContent).
  //    GÉOMÉTRIE IDENTIQUE au footer de l'inventaire (mêmes marges, même origine) :
  //    la version précédente n'ôtait qu'une DEMI marge de fenêtre, donc la barre
  //    remontait dans la grille et rognait sa dernière ligne, et le « ? » collé au
  //    bord droit tombait sur la poignée de redimensionnement. On reprend donc à
  //    l'identique : marges WindowPadding pleines, bas de barre calé sur le bas de
  //    la zone client, et gripM px réservés à droite pour la poignée.
  const FooterVals fv = ReadFooterVals();
  {
    const ImVec2 fwp = ImGui::GetWindowPos(), fws = ImGui::GetWindowSize();
    const float fx0 = fwp.x + style.WindowPadding.x;
    const float fx1 = fwp.x + fws.x - style.WindowPadding.x;
    const float fy1 = fwp.y + fws.y - style.WindowPadding.y;
    const float fy0 = fy1 - footerH;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawFooterBar(dl, fx0, fy0, fx1, footerH);
    const float cy1 = fy0 + footerH * 0.25f;  // centre ligne 1
    const float cy2 = fy0 + footerH * 0.75f;  // centre ligne 2
    const float gripM = 16.0f;                // marge de la poignée de resize
    const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colOver = IM_COL32(230, 60, 60, 255);
    char buf[64];

    // Ligne 1 = POIDS, ligne 2 = COMPTEUR : l'ordre de l'inventaire, pour que les
    // deux fenêtres se lisent pareil quand elles sont ouvertes côte à côte. (Le
    // natif du cart met les deux sur une seule ligne, compteur d'abord — il n'y
    // a donc pas d'ordre natif à respecter sur deux lignes.)
    float x = fx0 + 6.0f;
    x += DrawFooterIcon(dl, g_ico_weight, x, cy1) + 3.0f;
    std::snprintf(buf, sizeof(buf), "%d/%d", fv.weight, fv.maxweight);
    dl->AddText(ImVec2(x, cy1 - lineH * 0.5f),
                (fv.maxweight > 0 && fv.weight >= fv.maxweight) ? colOver : colText, buf);

    x = fx0 + 6.0f;
    x += DrawFooterIcon(dl, g_ico_num, x, cy2) + 3.0f;
    std::snprintf(buf, sizeof(buf), "%d/%d", fv.num, fv.maxnum);
    dl->AddText(ImVec2(x, cy2 - lineH * 0.5f),
                (fv.maxnum > 0 && fv.num >= fv.maxnum) ? colOver : colText, buf);

    // « ? » des raccourcis : dans le footer, sur la ligne du bas et à gauche de la
    // marge de poignée — jamais au-dessus d'elle, sinon il avale le clic de
    // redimensionnement. Même widget que l'inventaire (HelpMarker).
    const float hw = ImGui::CalcTextSize("(?)").x;
    ImGui::SetCursorScreenPos(ImVec2(fx1 - gripM - hw, cy2 - lineH * 0.5f));
    HelpMarker(kShortcuts);
  }

  ro::EndRoWindow();

  // Aperçu de description : dessiné APRÈS la fenêtre (c'est un tooltip, il doit
  // passer AU-DESSUS d'elle) et hors de tout Begin/End.
  if (hover_desc_id_ != 0 && hover_desc_idx_ >= 0 && hover_desc_idx_ < item_count_) {
    const Item& hit = items_[hover_desc_idx_];
    itemdesc::SimpleOpt sopts[5];
    for (int i = 0; i < hit.opt_count && i < 5; ++i) {
      sopts[i].index = hit.opts[i].index;
      sopts[i].value = hit.opts[i].value;
      sopts[i].param = hit.opts[i].param;
    }
    itemcell::DrawTooltip(hit.id, hit.cards, 4, sopts, hit.opt_count, hit.refine, hit.name,
                          hit.damaged != 0);
  }
}

// ── Section « Cart » du panneau Moonlight ──────────────────────────────────
bool CartViewer::DrawSettings() {
  bool changed = false;
  // Fenêtre membre du groupe « Interface moderne » (tout-ImGui ou tout-natif, plus
  // de mixe) : `SetModernInterface` écrit `imgui_enabled_` avec les autres. Ce qui
  // suit ne dit que ce que la bascule change pour le cart.
  // 🔴 Plus de CASE ici (cf. skill_bar.cc et moonlight_ui.h) : l'interrupteur du
  // groupe est unique, en tête de « Interface de jeu ». On garde la DESCRIPTION.
  ImGui::TextDisabled("Fenêtre du groupe « Interface moderne »");
  SameLine(); HelpMarker(
      "ON : cart ImGui moderne (grille d'icônes, onglets, recherche, "
      "double-clic pour retirer, clic droit, glisser vers l'inventaire ou le "
      "storage) et la fenêtre native est cachée.\n"
      "OFF (défaut) : cart natif classique, aucun viewer.");

  ImGui::BeginDisabled(!imgui_enabled_);

  changed |= ro::RoCheckbox("Description au survol", &desc_tooltip());
  SameLine(); HelpMarker(
      "Survoler un item affiche un aperçu SIMPLIFIÉ (nom, illustration, texte, "
      "cartes et options) dans un panneau au skin RO, à la place du petit "
      "tooltip nom + quantité.\n"
      "La description COMPLÈTE reste accessible au Ctrl + clic droit / menu "
      "contextuel.");

  changed |= ro::RoCheckbox("Champ de filtre", &show_filter());
  SameLine(); HelpMarker(
      "Affiche la barre de recherche par nom au-dessus de la grille.\n"
      "Décoche pour gagner une ligne (le filtre est alors vidé).");

  changed |= ro::RoCheckbox("Onglets verticaux (à gauche)", &tabs_vertical());
  SameLine(); HelpMarker(
      "ON (défaut) : onglets en colonne à gauche de la grille (images tab_*).\n"
      "OFF : rangée horizontale au-dessus de la grille (images tabh_*).");

  changed |= ro::RoCheckbox("Verrouiller la taille", &lock_size());
  SameLine(); HelpMarker(
      "La fenêtre ne peut plus être redimensionnée (elle reste déplaçable).");

  ImGui::EndDisabled();

  // ── Tri serveur (même combo que « Commands Settings ») ──────────────────────
  // HORS du BeginDisabled : réglage SERVEUR (@tri_cart), valable aussi pour la
  // fenêtre native. Le serveur trie et renvoie la liste ; le viewer l'affiche dans
  // l'ordre reçu. Pas de `changed` : l'état vit dans MoonlightUi.
  SeparatorText("Tri serveur");
  if (auto* mu = Bourgeon::Instance().moonlight_ui())
    mu->DrawSortModeCombo(MoonlightUi::kSortCart);

  return changed;
}
