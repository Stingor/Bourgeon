#include "features/item_cell.h"
#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
#include "ragnarok/item_info.h"  // rag::itemlist : le layout du noeud
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
#include "features/windows/chat_window.h"       // AppendItemLink (Maj+clic = lien d'objet)
#include "features/windows/inventory_viewer.h"  // PointOverViewer (dépôt vers le viewer inventaire)
#include "features/windows/item_desc_window.h"  // itemdesc::RenderSimpleDesc (aperçu au survol)
#include "features/moonlight_ui/moonlight_ui.h"      // OpenInterfaceSection + HelpMarker
#include "features/windows/storage_window.h"    // PointOverViewer (dépôt vers le viewer storage)
#include "features/windows/vending_window.h"    // IsComposing (échoppe en cours -> chariot figé)
#include "utils/i18n.h"                         // i18n::Tr (libellés traduisibles)
#include "d3d9/d3d9_hook.h"            // Overlay_DeviceEpoch
#include "imgui.h"
#include "ui/qty_prompt.h"             // ro::QuantityPrompt (dialogue « combien ? »)
#include "ui/ro_imgui.h"               // skin RO (BeginRoWindow / RoCheckbox / …)
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "features/windows/viewer_probes.h"  // etat des fenetres voisines
#include "ui/item_grid_chrome.h"  // ro::grid : le decor commun aux grilles
#include "ui/ro_widgets.h"

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
// Placement et taille par défaut du viewer, à la toute 1re ouverture seulement
// (avant, ils étaient lus sur la fenêtre native, qui ne naît plus).
constexpr float kSpawnX = 420.0f, kSpawnY = 160.0f;
constexpr float kSpawnW = 300.0f, kSpawnH = 300.0f;

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

// Dispatcher (CMode) : FUN_00a75340(0x1213338) -> objet mode actif (0 hors jeu).
// Son vtbl+0x18 = CMode::SendMsg. Commandes de transfert RE'ées sur la fenêtre
// cart elle-même (UICartWnd_OnRButtonDown branche ALT / OnMsg case 38).

// Autres fenêtres, pour router un transfert (cible ouverte ou non) :
// uiwnd::kInventoryWndSlot / kStorageWndSlot et leurs vtables.

// Lit un pointeur de fenêtre valide depuis un slot (vtable vérifiée). SEH.
uint8_t* ReadValidWnd(uintptr_t slot, uintptr_t expected_vtable) {
  return uiwnd::WndAtSlot(slot, expected_vtable);
}

// La fenêtre cart ouverte, ou nullptr. Ne sert plus qu'au FILET de OnTick, qui la
// détruit : elle ne naît plus qu'au moment d'une demande du joueur, aussitôt
// interceptée. La vtable est vérifiée car un id ne garantit pas la classe si un
// portage de client renumérote les fenêtres.
uint8_t* CartWnd() {
  return uiwnd::WndOfClass(uiwnd::kUICartWnd, uiwnd::kCartWndVTable);
}

// Envoie une commande UI native (transfert) via le dispatcher.
void SendCmd(int cmd, int index, int amount) {
  if (amount <= 0) return;
  // Garde-fou : l'UI grise déjà ces actions pendant une composition d'échoppe,
  // mais un raccourci (double-clic, Alt+clic droit) ne passe pas par un widget
  // désactivé. Le serveur jetterait la requête sans un mot ; autant ne pas la
  // faire partir.
  if (viewers::VendingComposing()) return;
  __try {
    void* d = rag::ActiveModeSafe();
    if (d) rag::ModeSendMsg(d, cmd, index, amount, 0, 0);
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
    uiwnd::OnMsg(wnd, uiwnd::kMsgUiAction, uiwnd::kActionClose, 0, 0, 0);
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
  itemcell::DeferDescFromIndex(rag::kCartListAddr, index, mx, my);
}

// Maj + clic gauche : le lien de l'objet dans la barre de saisie du chat. Le
// natif le fait DEPUIS LE CHARIOT AUSSI (`UICartWnd_OnLButtonDown 0x0094b460`,
// même branche `GetAsyncKeyState(VK_SHIFT)` que l'inventaire) ; le viewer ImGui
// n'avait jamais repris ce geste.
void PostItemLinkToChat(int index) {
  void* info = itemcell::FindInfoByIndex(rag::kCartListAddr, index);
  if (!info) return;
  if (auto* chat = Bourgeon::Instance().chat_window()) chat->AppendItemLink(info);
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
// 🔴 Les libellés se traduisent AU POINT D'USAGE, jamais ici : cette table est un
// agrégat statique, construit au chargement de la DLL, bien avant que le
// catalogue de langue n'existe. `i18n::Tr` y serait évalué une fois, en français,
// et n'en sortirait plus jamais. Voir la bande d'onglets, qui traduit à chacun de
// ses trois usages — dont la MESURE du texte.
// « Conso », « Equip », « Ammo », « Etc » restent tels quels : ce sont les
// abréviations du jeu, pas du français.
const Cat kCats[] = {
    {"Tout",   nullptr, 0, "tab_all"},
    {"Conso",  kUse,    4, "tab_use"},
    {"Equip",  kEquip,  9, "tab_cos"},
    {"Ammo",   kAmmo,   4, "tab_ammo"},
    {"Etc",    kEtc,    2, "tab_etc"},
    {"Cartes", kCard,   1, "tab_card"},
};
constexpr int kNumCats = 6;

// ── Assets natifs : barre 3-slice + fond de tuile + icônes du footer ───────────
// Préfixe CP949 pris sur les strings de l'exe (jamais reconstruit à la main).

using BarTex = ro::GameTexture;
BarTex g_tab[kNumCats][2];   // onglets VERTICAUX   [cat][0=actif, 1=inactif]
BarTex g_tabh[kNumCats][2];  // onglets HORIZONTAUX (jeu tabh_*)
bool   g_assets_tried = false;

void LoadAssets() {
  if (g_assets_tried) return;
  g_assets_tried = true;
  char path[160];
  for (int c = 0; c < kNumCats; ++c) {
    const char* base = kCats[c].img;
    if (!base) continue;
    char nm[48];
    std::snprintf(nm, sizeof(nm), "%s1.bmp", base);
    ro::grid::BasicInterfacePath(nm, path, sizeof(path)); g_tab[c][0] = ro::TextureFromGameFile(path);
    std::snprintf(nm, sizeof(nm), "%s2.bmp", base);
    ro::grid::BasicInterfacePath(nm, path, sizeof(path)); g_tab[c][1] = ro::TextureFromGameFile(path);
    char hbase[40];
    std::snprintf(hbase, sizeof(hbase), "tabh%s", base + 3);  // saute « tab »
    std::snprintf(nm, sizeof(nm), "%s1.bmp", hbase);
    ro::grid::BasicInterfacePath(nm, path, sizeof(path)); g_tabh[c][0] = ro::TextureFromGameFile(path);
    std::snprintf(nm, sizeof(nm), "%s2.bmp", hbase);
    ro::grid::BasicInterfacePath(nm, path, sizeof(path)); g_tabh[c][1] = ro::TextureFromGameFile(path);
  }
}

// Teinte des AddImage = luminosité + opacité du skin RO (les images du jeu sont
// dessinées en draw-list brut, elles échapperaient sinon à ces réglages).

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

// Vide les caches de textures quand le device D3D a été reset/recréé (handles morts).
unsigned g_tex_epoch = 0;
void MaybeFlushTextures() {
  const unsigned e = Overlay_DeviceEpoch();
  if (e == g_tex_epoch) return;
  g_tex_epoch = e;
  for (auto& row : g_tab)  for (auto& b : row) b = BarTex{};
  for (auto& row : g_tabh) for (auto& b : row) b = BarTex{};
  g_assets_tried = false;
}

// Aperçu de description RO au survol (le même que l'inventaire) : tooltip
// couche-avant, fond blanc arrondi + cadre sysbox peint derrière via un split de
// canaux. Appelé À L'EXTÉRIEUR de toute fenêtre (il crée son popup).

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════

// La fenêtre native du cart vient de naître : c'est une DEMANDE du joueur. On la
// masque sur-le-champ (sans quoi une frame native passe à l'écran) et on bascule
// le viewer ; OnTick la détruira — le natif la manipule encore ici.
void CartViewer::HandleNativeCreation(void* win) {
  HandleNativeToggle(win, uiwnd::kCartWndVTable);
}

// Remplit items_/item_count_ depuis le MODÈLE SESSION du cart (0x015fbae0), donc
// marche fenêtre native cachée. POD-only sous SEH ; le nom complet passe par
// itemcell::BuildDisplayName, qui résout seul son contexte natif.
void CartViewer::Extract() {
  item_count_ = itemcell::ExtractList(rag::kCartListAddr, items_, kMaxItems);
}

void CartViewer::OnTick() {
  // `open_` n'est plus déduit de la présence de la native : elle ne vit plus.
  // Il est posé par HandleNativeCreation (la demande du joueur) et levé par elle.
  const bool mode_changed = (imgui_enabled_ != prev_imgui_enabled_);
  prev_imgui_enabled_ = imgui_enabled_;
  if (!imgui_enabled_) {
    // Retour au natif : le viewer s'efface, la native reprend son service à la
    // prochaine demande (elle n'existe plus, donc le client la recréera).
    open_ = false;
    win_rect_.Invalidate();
    drag_active_ = false;
    return;
  }
  if (Bourgeon::Instance().IsMapLoading()) return;
  // 🔴 DÉTRUIRE, pas masquer : toute bascule du client fait « ferme si elle
  // existe, sinon crée » (cf. reference_native_window_toggle_router). Une native
  // seulement masquée existe, donc la demande suivante la fermerait sans repasser
  // par MakeWindow — un appui sur deux serait avalé — et elle garderait le
  // clavier. Ce filet couvre aussi la bascule de mode alors qu'elle est ouverte,
  // et la reconstruction du HUD.
  if (CartWnd()) {
    // Sa présence PROUVE que le joueur avait le cart ouvert : on adopte l'état
    // avant de la détruire, sinon activer le mode moderne le ferait disparaître.
    if (mode_changed && !open_) { open_ = true; show_panel_ = true; need_pos_ = true; }
    __try {
      uiwnd::CloseWindow(uiwnd::kUICartWnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  if (open_) Extract();
  // Aperçu de description : purgé dès que le viewer ne dessine plus, sinon il
  // resterait affiché sans rien pour l'effacer.
  if (!open_) {
    hover_desc_id_ = 0; hover_desc_idx_ = -1;
    win_rect_.Invalidate(); drag_active_ = false;
  }
}

void CartViewer::OnRenderUI() {
  // Pas dessinee ce frame => elle n a plus de rect : un depot lache sur sa
  // derniere position connue ne doit pas lui etre route.
  if (!open_ || !imgui_enabled_) { win_rect_.Invalidate(); return; }
  MaybeFlushTextures();  // device reset/TDR -> lâche les handles morts

  if (need_pos_) {
    // FirstUseEver : simple DÉFAUT de première ouverture ; ensuite ImGui garde la
    // position déplacée par le joueur. Ce défaut se lisait sur la fenêtre native,
    // qui ne naît plus — on le fixe, rabattu dans l'écran sur petite résolution.
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(
        ImVec2(std::min(kSpawnX, std::max(0.0f, screen.x - kSpawnW)),
               std::min(kSpawnY, std::max(0.0f, screen.y - kSpawnH))),
        ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(kSpawnW, kSpawnH), ImGuiCond_FirstUseEver);
  if (ro::grid::Snap().valid && !lock_size_) {
    const float minGrid = 5.0f * (ro::grid::Snap().cell + ro::grid::Snap().gap) - ro::grid::Snap().gap;  // min 5 tuiles
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(ro::grid::Snap().chromew + minGrid, ro::grid::Snap().chromeh + minGrid),
        ImVec2(10000.0f, 10000.0f), ro::grid::SnapWindowSize);
  } else if (ro::grid::Snap().valid && win_rect_.valid()) {
    // Taille VERROUILLÉE : le callback de snap ne tourne plus (il n'agit que pendant
    // un redimensionnement), donc la fenêtre resterait sur une hauteur quelconque —
    // dernière ligne coupée. On la recale une fois sur le palier le plus proche.
    const float step = ro::grid::Snap().cell + ro::grid::Snap().gap;
    int cols = static_cast<int>((win_rect_.w() - ro::grid::Snap().chromew + ro::grid::Snap().gap) / step + 0.5f);
    int rows = static_cast<int>((win_rect_.h() - ro::grid::Snap().chromeh + ro::grid::Snap().gap) / step + 0.5f);
    if (cols < 5) cols = 5;
    if (rows < 5) rows = 5;
    const ImVec2 snapped(ro::grid::Snap().chromew + cols * step - ro::grid::Snap().gap,
                         ro::grid::Snap().chromeh + rows * step - ro::grid::Snap().gap);
    if (std::fabs(snapped.x - win_rect_.w()) > 0.5f ||
        std::fabs(snapped.y - win_rect_.h()) > 0.5f)
      ImGui::SetNextWindowSize(snapped, ImGuiCond_Always);
  }

  // Bullet de la barre de titre = raccourci vers la config de CETTE fenêtre.
  ro::SetNextWindowTitleBullet(i18n::Tr("Options du cart"));
  const bool begun = ro::BeginRoWindow(
      i18n::Tr("Cart###bourgeon_cart"), &show_panel_,
      lock_size_ ? ImGuiWindowFlags_NoResize : 0);
  if (ro::TitleBulletClicked())
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(MoonlightUi::kIfaceCart);
  // X du viewer : l'état d'ouverture est le NÔTRE maintenant, il n'y a plus de
  // fenêtre native à fermer. Réarme show_panel_ pour la prochaine ouverture.
  if (!show_panel_) { open_ = false; show_panel_ = true; }
  // Repliee ou clippee : ImGui ne l a pas dessinee, son rect deplie ne vaut
  // plus rien comme cible de depot.
  if (!begun) { win_rect_.Invalidate(); ro::EndRoWindow(); return; }

  // Pendant la composition d'un shop, le serveur refuse TOUT mouvement touchant
  // le cart : autant l'annoncer une fois en clair, en plus des entrées grisées.
  if (viewers::VendingComposing())
    ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                       i18n::Tr("Shop en composition : les transferts sont figés."));

  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  win_rect_.Capture(wp.x, wp.y, ws.x, ws.y);

  // ── Action en attente (transfert d'une pile) -> prompt quantité ──
  auto do_move = [this](int amount) {
    switch (pend_action_) {
      case kPendToBody:    SendCmd(rag::kCmdCartToBody, pend_index_, amount); break;
      case kPendToStorage: SendCmd(rag::kCmdCartToStorage, pend_index_, amount); break;
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
    const char* verb = pend_action_ == kPendToStorage ? i18n::Tr("Vers le storage")
                                                      : i18n::Tr("Vers l'inventaire");
    bool cancelled = false;
    const int qty = ro::QuantityPrompt(this, verb, pend_max_, &cancelled);
    if (qty > 0) { do_move(qty); pend_id_ = 0; }
    else if (cancelled) pend_id_ = 0;
  }

  // Aide raccourcis : le texte est construit ici, le « (?) » est émis dans le FOOTER
  // pour ne pas manger une ligne au-dessus de la grille.
  // Les littéraux adjacents sont concaténés par le compilateur : c'est donc UNE
  // seule clé de catalogue, avec ses retours à la ligne.
  const char* kShortcuts =
      i18n::Tr("Raccourcis cart\n\n"
      "- Double-clic gauche : retirer vers l'inventaire\n"
      "- Clic droit : menu contextuel\n"
      "- Ctrl + clic droit : description\n"
      "- Alt + clic droit : transfert rapide (storage si ouvert, sinon inventaire)\n"
      "- Glisser : lâcher sur l'inventaire ou le storage pour y transférer");
  static ImGuiTextFilter filter;
  if (show_filter_) {
    ImGui::SetNextItemWidth(-1.0f);
    // 1er argument = l'ID du champ, JAMAIS traduit ; seul l'indice l'est.
    if (ImGui::InputTextWithHint("##cart_filter", i18n::Tr("Filtrer..."), filter.InputBuf,
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
            // ⚠ MESURER LE TEXTE AFFICHÉ, donc le traduit : mesurer le français
            // et dessiner l'anglais donnerait un onglet à la mauvaise largeur.
            : ImVec2(ImGui::CalcTextSize(i18n::Tr(kCats[c].label)).x +
                         style.FramePadding.x * 2.0f, tabH);
        if (ImGui::Selectable(i18n::Tr(kCats[c].label), sel, 0, sz)) cur_tab_ = c;
        if (sel) {
          activeTabMin = ImGui::GetItemRectMin();
          activeTabMax = ImGui::GetItemRectMax();
          haveActiveTab = true;
        }
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(" %s ", i18n::Tr(kCats[c].label));
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
    // Tuiles de 32 px (taille native du client), jointives, À L'ÉCHELLE de
    // l'interface — même règle que la grille d'inventaire, dont ceci est le
    // jumeau.
    const float cell = ro::Px(32.0f), gap = 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const float availw = ImGui::GetContentRegionAvail().x;  // exclut déjà la scrollbar
    const float availh = ImGui::GetContentRegionAvail().y;
    // Mesure du chrome (fenêtre - zone grille) pour le snap de resize (frame +1).
    ro::grid::Snap().cell = cell; ro::grid::Snap().gap = gap;
    ro::grid::Snap().chromew = mainW - availw;
    ro::grid::Snap().chromeh = mainH - availh;
    ro::grid::Snap().valid = true;
    int cols = static_cast<int>((availw + gap) / (cell + gap));
    if (cols < 1) cols = 1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {  // Fond itemwin_mid PAVÉ, ALIGNÉ sur la grille d'items.
      const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();  // = 1re tuile
      const ImVec2 gmn = ImGui::GetWindowPos();
      const ImVec2 gsz = ImGui::GetWindowSize();
      ro::grid::DrawTiledBg(dl, ro::grid::Assets().tile, gridOrigin, gmn, ImVec2(gmn.x + gsz.x, gmn.y + gsz.y));
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
          if (it.amount > 1) ImGui::TextDisabled(i18n::Tr(" Quantité : %d "), it.amount);
          ImGui::EndTooltip();
        }
        // Double-clic = retirer vers l'inventaire. La native n'a PAS de double-clic
        // (slot vtable par défaut) ; c'est un confort du viewer, symétrique du
        // double-clic « utiliser/équiper » de l'inventaire.
        // Entrepôt ouvert : le serveur refuse cart -> inventaire, on bascule donc
        // vers le storage — exactement l'arbitrage du natif (ALT + clic droit
        // envoie au storage dès qu'il est ouvert, sinon à l'inventaire).
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
          SendCmd(viewers::StorageOpen() ? rag::kCmdCartToStorage : rag::kCmdCartToBody, it.index,
                  it.amount);
      }

      // Raccourcis (miroir de UICartWnd_OnRButtonDown) :
      //   Maj  + clic GAUCHE = lien de l'objet dans la saisie du chat ;
      //   Ctrl + clic DROIT = description directe ;
      //   Alt  + clic DROIT = transfert rapide (storage ouvert -> 0x4f, sinon 0x4d) ;
      //   clic DROIT seul   = menu contextuel.
      const ImGuiIO& mods = ImGui::GetIO();
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && mods.KeyShift)
        PostItemLinkToChat(it.index);
      if (IsLastItemRightClicked()) {
        if (mods.KeyCtrl) {
          POINT pt; if (GetCursorPos(&pt)) OpenItemDesc(it.index, pt.x, pt.y);
        } else if (mods.KeyAlt) {
          if (viewers::StorageOpen())        SendCmd(rag::kCmdCartToStorage, it.index, it.amount);
          else if (viewers::InventoryOpen()) SendCmd(rag::kCmdCartToBody, it.index, it.amount);
        } else {
          ImGui::OpenPopup("ctx");
        }
      }

      // Source de drag (inventaire / storage selon la cible du relâché).
      // Marge interne du fantôme : la grille pousse WindowPadding à 0 (tuiles
      // jointives), et la tooltip de drag d'ImGui hérite de ce style au Begin ->
      // on la surcharge le temps du bloc pour aérer l'icône + le nom.
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
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
        if (viewers::VendingComposing())
          ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                             i18n::Tr("Shop en composition : le cart est figé"));
        else if (viewers::StorageOpen() && viewers::MouseOverInventory(drag_mouse.x, drag_mouse.y))
          ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                             i18n::Tr("Storage ouvert : vers l'inventaire impossible"));
        ImGui::EndDragDropSource();
      }
      ImGui::PopStyleVar();  // WindowPadding (marge du fantôme de drag)

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
        if (ImGui::MenuItem(i18n::Tr("Description"))) {
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
        const bool vending_lock = viewers::VendingComposing();
        const bool storage_open = viewers::StorageOpen();
        const bool to_body_off = storage_open || vending_lock;
        // Retrait vers l'inventaire : une PILE ouvre le prompt de quantité.
        if (it.amount <= 1) {
          if (ImGui::MenuItem(i18n::Tr("Vers l'inventaire"), nullptr, false, !to_body_off))
            SendCmd(rag::kCmdCartToBody, it.index, 1);
        } else if (ImGui::MenuItem(i18n::Tr("Vers l'inventaire..."), nullptr, false, !to_body_off)) {
          pend_id_ = it.index; pend_index_ = it.index; pend_max_ = it.amount;
          pend_action_ = kPendToBody; pend_open_prompt_ = true;
        }
        if (to_body_off && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip(
              vending_lock
                  ? i18n::Tr("Impossible pendant la composition d'un shop (règle du\n"
                             "serveur). Ouvrez ou annulez le shop d'abord.")
                  : i18n::Tr("Impossible tant que le storage est ouvert (règle du serveur).\n"
                             "Fermez le storage, ou faites transiter l'objet par le storage."));
        if (storage_open) {
          if (it.amount <= 1) {
            if (ImGui::MenuItem(i18n::Tr("Vers le storage"), nullptr, false, !vending_lock))
              SendCmd(rag::kCmdCartToStorage, it.index, 1);
          } else if (ImGui::MenuItem(i18n::Tr("Vers le storage..."), nullptr, false, !vending_lock)) {
            pend_id_ = it.index; pend_index_ = it.index; pend_max_ = it.amount;
            pend_action_ = kPendToStorage; pend_open_prompt_ = true;
          }
        }
        // alootid : ramassage auto par ID (même entrée que l'inventaire).
        if (auto* mui = Bourgeon::Instance().moonlight_ui()) {
          ImGui::Separator();
          const bool inAloot = mui->IsAlootId(it.id);
          if (ImGui::MenuItem(inAloot ? i18n::Tr("Retirer de l'alootid")
                                      : i18n::Tr("Ajouter à l'alootid"))) {
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
        const bool over_self = win_rect_.Contains(drag_mx_, drag_my_);
        if (!over_self) {
          if (viewers::MouseOverStorage(drag_mx_, drag_my_))        action = kPendToStorage;
          // Entrepôt ouvert => le serveur refuse cart -> inventaire (storage_flag,
          // cf. le menu contextuel) : on n'arme RIEN, plutôt que d'ouvrir un prompt
          // de quantité dont la validation partirait à la poubelle.
          else if (viewers::MouseOverInventory(drag_mx_, drag_my_) && !viewers::StorageOpen())
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
    ro::grid::DrawFooterBar(dl, fx0, fy0, fx1, footerH);
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
    x += ro::grid::DrawFooterIcon(dl, ro::grid::Assets().weight, x, cy1) + 3.0f;
    std::snprintf(buf, sizeof(buf), "%d/%d", fv.weight, fv.maxweight);
    dl->AddText(ImVec2(x, cy1 - lineH * 0.5f),
                (fv.maxweight > 0 && fv.weight >= fv.maxweight) ? colOver : colText, buf);

    x = fx0 + 6.0f;
    x += ro::grid::DrawFooterIcon(dl, ro::grid::Assets().num, x, cy2) + 3.0f;
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
  ImGui::TextDisabled(i18n::Tr("Fenêtre du groupe « Interface moderne »"));
  SameLine(); HelpMarker(
      i18n::Tr("ON : cart ImGui moderne (grille d'icônes, onglets, recherche, "
      "double-clic pour retirer, clic droit, glisser vers l'inventaire ou le "
      "storage) et la fenêtre native est cachée.\n"
      "OFF (défaut) : cart natif classique, aucun viewer."));

  ImGui::BeginDisabled(!imgui_enabled_);

  changed |= ro::RoCheckbox(i18n::Tr("Description au survol"), &desc_tooltip());
  SameLine(); HelpMarker(
      i18n::Tr("Survoler un item affiche un aperçu SIMPLIFIÉ (nom, illustration, texte, "
      "cartes et options) dans un panneau au skin RO, à la place du petit "
      "tooltip nom + quantité.\n"
      "La description COMPLÈTE reste accessible au Ctrl + clic droit / menu "
      "contextuel."));

  changed |= ro::RoCheckbox(i18n::Tr("Champ de filtre"), &show_filter());
  SameLine(); HelpMarker(
      i18n::Tr("Affiche la barre de recherche par nom au-dessus de la grille.\n"
      "Décoche pour gagner une ligne (le filtre est alors vidé)."));

  changed |= ro::RoCheckbox(i18n::Tr("Onglets verticaux (à gauche)"), &tabs_vertical());
  SameLine(); HelpMarker(
      i18n::Tr("ON (défaut) : onglets en colonne à gauche de la grille (images tab_*).\n"
      "OFF : rangée horizontale au-dessus de la grille (images tabh_*)."));

  changed |= ro::RoCheckbox(i18n::Tr("Verrouiller la taille"), &lock_size());
  SameLine(); HelpMarker(
      i18n::Tr("La fenêtre ne peut plus être redimensionnée (elle reste déplaçable)."));

  ImGui::EndDisabled();

  // ── Tri serveur (même combo que « Commands Settings ») ──────────────────────
  // HORS du BeginDisabled : réglage SERVEUR (@tri_cart), valable aussi pour la
  // fenêtre native. Le serveur trie et renvoie la liste ; le viewer l'affiche dans
  // l'ordre reçu. Pas de `changed` : l'état vit dans MoonlightUi.
  SeparatorText(i18n::Tr("Tri serveur"));
  if (auto* mu = Bourgeon::Instance().moonlight_ui())
    mu->DrawSortModeCombo(MoonlightUi::kSortCart);

  return changed;
}
