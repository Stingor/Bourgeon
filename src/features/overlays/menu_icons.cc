#include "ui/game_texture.h"
#include "features/overlays/menu_icons.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"

#include "ragnarok/msgstring.h"  // msgstr:: (libellés natifs du client)
#include "ragnarok/uiwnd.h"
#include "ragnarok/user_hotkey.h"    // raccourcis du CLIENT (infobulles)
#include "features/hotkey_actions.h"  // raccourcis de BOURGEON (idem + dispatch)
#include "features/hotkey_util.h"     // hotkeys::Label (mise en forme d'un combo)

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"
#include "imgui.h"
#include "features/systems/bourgeon_opcodes.h"
#include "features/moonlight_ui/moonlight_ui.h"  // shared AlignGrid (snap)
#include "ui/window_clamp.h"  // ClampWindowPosToScreen (icônes déplacées à la main)
#include "utils/log_console.h"
#include "utils/i18n.h"
#include "ragnarok/game_settings.h"  // gamesettings::kFlagGetRawAddr / kFlagSetRawAddr
#include "utils/memory_patch.h"  // mem::PatchValue

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {
// ── Game texture loader (conventions per status_tweaks.cc) ─────────────────

// UITexture field offsets: +0x114 width, +0x118 height, +0x11c BGRA32 top-down.

// ── Native menu-icon window draw hook (hide the native grid) ───────────────
// GridClear replaces the native grid's DrawContent (UIMenuIconWnd_RebuildNodes):
// it empties the window's render-node list (window+4 = std::list head, window+8 =
// size) and does NOT rebuild from the icon map (+0xb4, which stays populated for
// dispatch). Unlike a plain no-op, this also drops the ALREADY-built nodes, so
// the grid vanishes immediately instead of lingering until the next relayout
// (map reload). The orphaned nodes leak once per enable (~25 * 0x18 bytes —
// negligible; the game rebuilds them when the replacement is disabled).
constexpr uintptr_t kGridDrawSlot = 0x01028200;  // UIMenuIconWnd vtbl +0x50
constexpr uintptr_t kGridDrawOrig = 0x00814150;  // UIMenuIconWnd_RebuildNodes
void __fastcall GridClear(void* self, void* /*edx*/) {
  __try {
    char* w = reinterpret_cast<char*>(self);
    int* head = *reinterpret_cast<int**>(w + 4);  // std::list sentinel node
    if (!head) return;
    head[0] = reinterpret_cast<int>(head);         // next = self (empty list)
    head[1] = reinterpret_cast<int>(head);         // prev = self
    *reinterpret_cast<int*>(w + 8) = 0;            // size = 0
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Command dispatch ───────────────────────────────────────────────────────
// A native icon click routes (OnLButtonDown -> FUN_00a38b40 table lookup ->
// tail-jump) into the menu-icon window's command handler FUN_00814a70, invoked
// as (this=window, 0, action=6, cmdId, 0, 0, 0). We call that handler DIRECTLY:
// it only needs the window + cmd id, so we avoid synthesising a click — the old
// path poked the window-manager's active-window slot (mgr+0x19c) and the icon's
// +0xac then restored them, which corrupted the game's active-window tracking
// and crashed when a window was opened/closed repeatedly (spam).
constexpr uintptr_t kCmdHandler    = 0x00814a70;  // UIMenuIconWnd command handler
using CmdHandlerFn = void  (__thiscall*)(void*, int, int, int, int, int, int);

// La carte du monde (et les UI plein écran) remplacent tout le HUD : la grille
// d'icônes native cesse de se dessiner, donc nos icônes ImGui doivent se cacher
// et refuser les clics. C'est uiwnd::IsHudReplaced() qui porte ce test — il
// était copié ici et dans basic_info.cc, à l'identique.

// ── Functional-icon test: native grid only shows ids >= 0x178 per the WARP
// visibility table @0x814064 (byte 1 = shown); classic ids < 0x178 always show.
constexpr uintptr_t kVisTable = 0x00814064;  // indexed by (cmdId - 0x178), 0..0xDF

bool IconShown(int cmd_id) {
  if (cmd_id < 0x178) return true;
  const int idx = cmd_id - 0x178;
  if (idx < 0 || idx > 0xDF) return true;
  return *reinterpret_cast<uint8_t*>(kVisTable + idx) == 1;
}

// ── Signalement « nouveau » (le N rouge du courrier) ───────────────────────
// Le natif ne peint pas une pastille par-dessus l'icône : `BuildIconList`
// @0x00812fb0 crée une SECONDE UIMenuIcon (bitmaps bt_<name>_new.bmp /
// _new_press.bmp) pour les cinq commandes qui en ont une — status 0xC0,
// item 0xC2, skill 0xC4, achievement 0x1D9, mail 0x1DC — puis
// `RebuildNodes` @0x00814150 montre l'une OU l'autre.
// Ce qui décide, c'est une std::list<int> des commandes à signaler portée par
// la fenêtre elle-même : sentinelle à +0xC4, taille à +0xC8, nœuds
// {next, prev, valeur}. Elle est alimentée par son propre OnMsg (action 6,
// commande 286, arg = id d'icône, arg suivant = 1 allumer / 0 éteindre) —
// c'est ce qu'appellent les handlers RODEX : allumage à l'arrivée d'un
// courrier non lu, extinction dans 0x00cfb6b0 quand le compteur de non-lus
// (g_RodexMgr+0x18) retombe à zéro.
// On LIT cette liste au lieu de rejouer la règle métier : le signalement suit
// donc exactement le natif, pour les cinq icônes et sans code par icône. Elle
// reste tenue à jour alors même que la grille native est vidée (GridClear ne
// touche qu'à la liste de nœuds de RENDU, +0x4).
constexpr int kOffBadgeList = 0xC4;  // std::list<int> : sentinelle
constexpr int kOffBadgeSize = 0xC8;  // taille de cette liste

// Relève les commandes signalées dans `out` (au plus `cap`). Renvoie le nombre
// lu, 0 si la fenêtre est absente ou la liste illisible. Isolée du reste pour
// garder le __try loin de tout objet à destructeur.
int ReadBadgeCmdIds(void* wnd, int* out, int cap) {
  if (!wnd) return 0;
  int n = 0;
  __try {
    char* w = reinterpret_cast<char*>(wnd);
    int* head = *reinterpret_cast<int**>(w + kOffBadgeList);
    const int count = *reinterpret_cast<int*>(w + kOffBadgeSize);
    if (!head || count <= 0 || count > cap) return 0;
    int* node = reinterpret_cast<int*>(head[0]);  // premier élément
    while (n < count && node && node != head) {
      out[n++] = node[2];                         // nœud : next, prev, valeur
      node = reinterpret_cast<int*>(node[0]);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
  return n;
}

// Charge \<dir><name>.bmp par le loader du jeu. `dir` porte le dossier ET le
// préfixe de nom, parce que les deux changent d'une famille d'icônes à l'autre
// (« menu_icon\bt_ » pour la grille, « basic_interface\ » pour le bouton du
// cash shop) — c'est TOUT ce que cette fonction ajoute.
//
// ⚠ Elle portait sa propre copie du décodage : lecture des trois champs de la
// CTexture, garde-fou de dimensions, boucle de color-key magenta, téléversement.
// `ro::TextureFromGameFile` fait exactement cela, et passe par le chemin
// indépendant du moteur (surface DirectDraw en DX7, texture D3D9 en DX9 — sans
// quoi les icônes sont invisibles dans le rendu qui ne correspond pas).
void* LoadIconTexture(const char* dir, const char* name, int* out_w, int* out_h) {
  const std::string path =
      std::string(ro::uipath::kUiRoot) + "\\" + dir + name + ".bmp";
  const ro::GameTexture t = ro::TextureFromGameFile(path.c_str());
  if (!t.tex) return nullptr;
  if (out_w) *out_w = t.w;
  if (out_h) *out_h = t.h;
  return t.tex;
}

// Dossier + préfixe de nom des bitmaps de la grille, sous 유저인터페이스\.
constexpr char kGridIconDir[] = "menu_icon\\bt_";

// Charge un état d'icône À LA DEMANDE, et renonce après trois échecs. Le suffixe
// est celui du natif : "" (normal), "_press", "_new", "_new_press".
//
// 🔴 Le renoncement n'est pas une optimisation de confort : la plupart des
// icônes n'ont ni bitmap « _new » ni bitmap « _press », et une recherche GRF par
// FRAME pour un fichier absent se paierait cher. Le compteur est remis à zéro
// quand le device change d'epoch (les textures sont mortes, il faut retenter) et,
// pour le badge, quand le natif l'éteint puis le rallume.
void EnsureBitmap(MenuIcons::Bitmap& bmp, const char* dir, const char* name,
                  const char* suffix) {
  if (bmp.tex || bmp.fail >= 3) return;
  char file[64];
  std::snprintf(file, sizeof(file), "%s%s", name, suffix);
  bmp.tex = LoadIconTexture(dir, file, &bmp.w, &bmp.h);
  if (!bmp.tex) ++bmp.fail;
}

// name, cmd_id, tooltip msg_id (from UIMenuIcon_SetHelpTextByCmdId @0x00814550),
// puis l'index de commande de la catégorie Interface dont la TOUCHE ouvre la
// même fenêtre (-1 = le client n'en propose pas).
//
// ── D'où vient cette dernière colonne ────────────────────────────────────────
// De `data\luafiles514\lua files\hotkey.lub`, table `HOTKEY_2` : le client y
// range les commandes de la catégorie Interface DANS L'ORDRE, et c'est ce rang
// (0-based) que `GetHotKey(catégorie+1, index)` attend. Recoupé sur le
// `SaveData\UserKeys.lua` d'un compte réel, qui n'écrit que les écarts et les
// désigne par ce même rang : `[3] = { EXE = "Quest Log" }` tombe bien sur
// `MSI_HK_QUESTWND_ONOFF`, `[4]` sur `MSI_HK_FRIENDWND_ONOFF`, `[21]` sur
// `MSI_HK_SNAPSHOT` — et `USERKEY_1[9] = "Hotkey 2-1"` sur `MSI_HK_SKILLBAR2_1`.
//
// ⚠ TÉMOIN NÉGATIF : le GRF porte AUSSI un `hotkey_v2.lub` où l'ordre des tables
// diffère (`HOTKEY_2` y est la seconde barre de compétences, l'Interface passe en
// `HOTKEY_3`). Ce n'est PAS celui que ce client suit — le `UserKeys.lua` écrit
// par le jeu le prouve, il range l'Interface en `USERKEY_2`. Prendre la mauvaise
// table décalerait chaque raccourci d'une fenêtre à l'autre, en silence.
//
// 🔴 On ne recopie ici que le RANG. Le nom de touche, lui, est relu chez le jeu à
// chaque survol : il est layout-aware (« A » sur AZERTY, « Q » sur QWERTY) et
// suit le remappage du joueur, ce qu'une table écrite à la main ne ferait pas.
struct IconDef { const char* name; int cmd_id; int msg_id; int hk_cmd; };
const IconDef kIconTable[] = {
    {"status", 0xC0, 0x69, 47},   {"option", 0xC1, 0x109, -1}, {"item", 0xC2, 0x6A, 9},
    {"equip", 0xC3, 0x68, 1},     {"skill", 0xC4, 0x11B, 2},   {"party", 0xC7, 0x67, 5},
    {"map", 0xDB, 0xB2B, 8},      {"quest", 0x169, 0x525, 3},  {"keyboard", 0x172, 0xC38, -1},
    {"guild", 0x175, 0xC3E, 10},  {"battle", 0x178, 0x7D3, 30}, {"booking", 0x17B, 0x1103, 34},
    {"rec", 0x18F, 0x92E, 38},    {"navigation", 0x1AE, 0x931, 42}, {"bank", 0x1CD, 0xC3F, 46},
    {"achievement", 0x1D9, 0xA56, 49}, {"mail", 0x1DC, 0xC40, 51}, {"tip", 0x1FF, 0xCA7, 54},
    {"shop", 0x200, 0xC41, 37},   {"sns", 0x206, 0xB1D, 53},   {"attendance", 0x21C, 0xD91, -1},
    {"adventurerAgency", 0x220, 0xDBA, -1}, {"repute", 0x237, 0xEF3, 60},
    {"adventureguide", 0x245, 0xFD5, -1},   {"probability", 0x24B, 0x1017, -1},
};

// ── Les icônes de BOURGEON ────────────────────────────────────────────────────
// Des boutons que le client ne connaît pas, posés dans la même grille pour
// hériter d'un coup du mode édition, de l'aimantage, du clamp et de la
// persistance par nom. Deux choses seulement les distinguent d'une icône du jeu,
// et elles tiennent dans les champs d'`Icon` : `wnd_id == 0` (le clic ne part
// chez aucune fenêtre native) et `action_id` (il part au catalogue de raccourcis,
// exactement là où va la touche du joueur — un seul point d'entrée, donc jamais
// deux comportements à tenir d'accord).
//
// L'art vit dans `menu_icon\` comme celui du jeu, aux mêmes conventions de nom
// (`bt_atlas.bmp`, `bt_atlas_press.bmp`) : il est livré dans le dossier `data\`
// du client, que son VFS lit AVANT les GRF.
struct BourgeonIconDef {
  const char* name;
  const char* action_id;
  const char* label_fr;
};
const BourgeonIconDef kBourgeonIcons[] = {
    {"atlas", "tool_craft_atlas", "Atlas des recettes"},
};

// Le raccourci qui ouvre CETTE icône, prêt à afficher — chaîne vide s'il n'y en
// a pas. Les deux systèmes de raccourcis du projet y répondent chacun pour les
// siens (cf. le pavé de `hotkey_actions.h` sur le partage) : le catalogue de
// Bourgeon pour ses propres boutons, les tables du client pour ses fenêtres.
//
// 🔴 RELU À CHAQUE SURVOL, jamais mis en cache : le joueur peut remapper sans
// quitter la carte, et le nom rendu par le jeu est layout-aware.
void HotkeyLabel(const MenuIcons::Icon& ic, char* out, int cap) {
  if (cap <= 0) return;
  out[0] = '\0';
  if (ic.action_id) {
    const int index = hotkeys::IndexOf(ic.action_id);
    if (index < 0) return;
    const hotkeys::Binding& b = hotkeys::BindingAt(index);
    if (b.vk == 0) return;  // aucune touche : l'infobulle n'en parle pas
    hotkeys::Label(b.vk, b.ctrl, b.alt, b.shift, out, cap);
    return;
  }
  if (ic.hk_cmd < 0) return;
  userhotkey::Binding b;
  if (!userhotkey::ReadBindingForCommand(userhotkey::kInterface, ic.hk_cmd, &b))
    return;
  if (!b.assigned) return;
  std::snprintf(out, cap, "%s", b.key_name);
}

// ── Le bouton « cash shop » posé près de la minimap ────────────────────────
// Ce n'est PAS une icône de la grille : c'est une fenêtre native à elle seule,
// `uiwnd::kUInCash_CallWnd`, avec sa vtable. Son seul créateur est
// `GameMode_OnEnterMapSetup` @0x00c6bbad, qui la fabrique à CHAQUE entrée de
// carte (sauf sur `new_event.rsw`) ; le case 190 de MakeWindow @0x00a3d3f7 la
// dimensionne 43x43 et la pose en (largeur_écran - 187, 16), c'est-à-dire au
// coin haut-droit, contre la minimap. Elle ne dessine rien elle-même (son
// DrawContent est un stub) : son unique enfant est un bouton bitmap
// `\basic_interface\NC_CashShop.bmp` dont le clic part en
// `GameMode::SendMsg(cmd 323)` -> CZ_SE_CASHSHOP_OPEN2 0x0B6D.
//
// Le client sait déjà la masquer, et c'est ce geste-là qu'on rejoue : la
// commande de chat `/cashshop` (`Chat_HandleChatMessage` case 218 @0x00c7c83b)
// BASCULE l'option de jeu TT_SHOW_CASHSHOP_BTN_ON_OFF — l'index 218 de la table
// des commandes @0x01008120 — puis applique `UIWindow_SetVisible(190, valeur)`.
// On impose une valeur au lieu de basculer, sans quoi la case à cocher et le
// jeu pourraient diverger ; pour le reste c'est la MÊME option, donc taper
// `/cashshop` en jeu met la case à jour toute seule.
//
// 🔴 Rien à persister de notre côté, et rien à réappliquer : l'option vit dans
// la table OptionInfo du client, que `OptionInfo_SaveToFile` (appelée à la
// fermeture propre du jeu) écrit dans `SaveData\OptionInfo.lua` ; et la queue
// commune du case 190 @0x00a3d473 refait `SetVisible(OptionInfo_GetValue(218))`
// à chaque création, donc le réglage survit seul aux changements de carte.
//
// ── Et pourquoi il est DANS `icons_` ──────────────────────────────────────
// Pour être déplaçable comme les 25 autres, il fallait choisir : bouger la
// fenêtre native, ou la masquer et la redessiner. C'est le second, parce que
// c'est déjà toute l'architecture de ce fichier — l'entrée gagne d'un coup le
// mode édition, l'aimantage aux voisines, le clamp à l'écran et la persistance
// par nom, sans une ligne de plus. Deux différences seulement avec une icône de
// grille, portées par les champs `dir` et `wnd_id` de `Icon` : son art vit dans
// `basic_interface\` et son clic va à SA fenêtre (OnMsg action 6, commande 192)
// au lieu du handler de la grille.
//
// 🔴 Le natif recrée sa fenêtre VISIBLE à chaque entrée de carte (cf. plus haut)
// : la masquer une fois ne suffit pas, d'où `SyncCashShopButton` rejouée depuis
// `OnTick`. Elle est le SEUL endroit qui décide qui du natif ou de notre copie
// se voit — règle : le natif ne se montre que si l'option du client est allumée
// ET que notre remplacement ne prend pas le relais.
constexpr int kTtShowCashShopBtn = 218;  // TT_SHOW_CASHSHOP_BTN_ON_OFF
using OptionInfoGetFn = uint8_t(__cdecl*)(unsigned int);
using OptionInfoSetFn = int(__cdecl*)(unsigned int, char);

// L'icône côté Bourgeon. `NC_CashShop.bmp` sert les TROIS états du bouton natif
// (normal / survol / pressé) : un seul bitmap à charger.
constexpr char kCashShopIconDir[]  = "basic_interface\\";
constexpr char kCashShopIconName[] = "NC_CashShop";
constexpr int  kCashShopCmdId = 192;    // commande du bouton enfant (0xC0)
// Infobulle : **MSI_CASHSHOP**, le nom que le serveur donne à sa boutique — soit
// « Vote Shop » sur moonlight, où la monnaie n'est pas de l'argent mais les votes
// des joueurs. Le lire au lieu de l'écrire en dur, c'est suivre d'office une
// prochaine retouche de la table, et c'est déjà ce que fait l'icône « shop » de
// la grille. ⚠ Ne PAS prendre `MSI_OUTSIDE_CASHSHOP_BTN_TOOLTIP` (3582), dont le
// nom promet pourtant l'infobulle de CE bouton : le natif ne s'en sert nulle part
// et la ligne du serveur n'a jamais été remplie — elle vaut « Title », comme sa
// voisine 3580 vaut « Material ».
constexpr int  kCashShopMsgId = 0xC41;  // MSI_CASHSHOP
// Sa touche : `MSI_HK_CASHSHOPWND`, rang 37 de la catégorie Interface — la même
// que l'icône « shop » de la grille, les deux ouvrant la même boutique.
constexpr int  kCashShopHotkeyCmd = 37;
// Position par défaut du natif, recopiée de MakeWindow case 190 : elle ne sert
// que le tout premier lancement, et seulement si la fenêtre n'est pas encore née
// au moment où on construit la liste — sinon on lit la vraie, sur l'objet vivant.
constexpr int kCashShopDefaultRightMargin = 187;
constexpr int kCashShopDefaultY           = 16;

// Le natif rend l'octet de poids faible d'EAX : l'option est un booléen.
bool CashShopButtonShown() {
  __try {
    return reinterpret_cast<OptionInfoGetFn>(gamesettings::kFlagGetRawAddr)(
               kTtShowCashShopBtn) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Écrit l'option, et RIEN d'autre : c'est `SyncCashShopButton` qui en tire la
// visibilité du natif comme de notre copie. Marche même hors carte, où la
// fenêtre n'existe pas encore — la prochaine création appliquera l'option ; là
// où la commande native, elle, sort sans rien faire sur un FindWindow(190) nul.
void SetCashShopButtonShown(bool shown) {
  __try {
    reinterpret_cast<OptionInfoSetFn>(gamesettings::kFlagSetRawAddr)(kTtShowCashShopBtn,
                                                           shown ? 1 : 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Ask the server to clif_refresh us (reuses the CZ 0x0F04 settings packet with a
// REFRESH id). The client receives the resulting ZC_NPCACK_MAPMOVE (0x91) in its
// recv loop and re-composites the UI — this drops the stale native menu-icon
// "ghost" that lingers after GridClear empties the node list (the already-
// composited pixels aren't re-blitted until a relayout). @refresh proved this
// safe in-game. Sent from OnTick (never mid-Present). Server side: moonlight
// clif_parse_bourgeon_setting case BOURGEON_SETTING_REFRESH -> clif_refresh(sd).
constexpr uint16_t kCzBourgeonSetting      = bopcodes::kSetting;  // 0x0F04 CZ_BOURGEON_SETTING
constexpr uint16_t kBourgeonSettingRefresh = 25;  // matches moonlight e_bourgeon_setting
void RequestServerRefresh() {
  uint8_t buf[10];
  *reinterpret_cast<uint16_t*>(buf)     = kCzBourgeonSetting;
  *reinterpret_cast<uint16_t*>(buf + 2) = 10;
  *reinterpret_cast<uint16_t*>(buf + 4) = kBourgeonSettingRefresh;
  *reinterpret_cast<uint32_t*>(buf + 6) = 0;
  Bourgeon::Instance().SendPacket(buf, sizeof(buf));
}
}  // namespace

void MenuIcons::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
}

void MenuIcons::BuildIconList() {
  icons_.clear();
  // Applique la position enregistrée. `allow_hidden` est faux pour le bouton du
  // cash shop : sa visibilité appartient à l'option du client, pas à notre YAML.
  const auto apply_saved = [this](Icon& ic, bool allow_hidden) {
    const auto it = saved_.find(ic.name);
    if (it == saved_.end() || !it->second.valid) return;
    if (it->second.x >= 0) ic.x = it->second.x;
    if (it->second.y >= 0) ic.y = it->second.y;
    if (allow_hidden) ic.hidden = it->second.hidden;
  };

  int shown = 0;
  for (const auto& d : kIconTable) {
    if (!IconShown(d.cmd_id)) continue;  // skip native-hidden / non-functional
    Icon ic;
    ic.name = d.name;
    ic.dir = kGridIconDir;
    ic.wnd_id = uiwnd::kMenuIconWndId;
    ic.cmd_id = d.cmd_id;
    ic.msg_id = d.msg_id;
    ic.hk_cmd = d.hk_cmd;
    ic.x = 24 + (shown % 6) * 40;   // default grid
    ic.y = 160 + (shown / 6) * 40;
    apply_saved(ic, true);
    icons_.push_back(ic);
    ++shown;
  }

  // Les boutons de Bourgeon, à la suite de la grille et sur le même pas : ils
  // sont déplaçables comme les autres, et le joueur les retrouve donc là où il
  // s'attend à trouver une icône de menu.
  for (const auto& d : kBourgeonIcons) {
    Icon ic;
    ic.name = d.name;
    ic.dir = kGridIconDir;
    ic.wnd_id = 0;            // aucune fenêtre native : le clic va au catalogue
    ic.cmd_id = 0;
    ic.msg_id = 0;            // pas de ligne dans la table de messages du client
    ic.action_id = d.action_id;
    ic.label_fr = d.label_fr;
    ic.x = 24 + (shown % 6) * 40;
    ic.y = 160 + (shown / 6) * 40;
    apply_saved(ic, true);
    icons_.push_back(ic);
    ++shown;
  }

  // Le bouton du cash shop, à la suite : même liste, donc même mode édition,
  // même aimantage et même persistance. Sa position par défaut est celle que le
  // CLIENT donne à sa fenêtre, relevée sur l'objet vivant plutôt que recopiée du
  // désassemblage — le repli n'existe que pour le cas où elle ne serait pas
  // encore née (voir kCashShopDefault*).
  {
    Icon ic;
    ic.name = kCashShopIconName;
    ic.dir = kCashShopIconDir;
    ic.wnd_id = uiwnd::kUInCash_CallWnd;
    ic.cmd_id = kCashShopCmdId;
    ic.msg_id = kCashShopMsgId;
    ic.hk_cmd = kCashShopHotkeyCmd;
    ic.x = static_cast<int>(ImGui::GetIO().DisplaySize.x) -
           kCashShopDefaultRightMargin;
    ic.y = kCashShopDefaultY;
    if (void* w = uiwnd::SafeFindWindow(uiwnd::kUInCash_CallWnd)) {
      ic.x = uiwnd::PosX(w);
      ic.y = uiwnd::PosY(w);
    }
    apply_saved(ic, false);
    icons_.push_back(ic);
  }

  icons_built_ = true;
  SyncCashShopButton();  // pose `hidden` du bouton dès la construction
}

// Recopie dans les icônes le signalement « nouveau » tenu par la fenêtre native.
// Appelée depuis OnTick (phase update, ~100 ms) : un badge n'a pas besoin de la
// fréquence d'affichage, et on évite un FindWindow par frame.
void MenuIcons::RefreshBadges() {
  constexpr int kMaxFlagged = 32;
  int flagged[kMaxFlagged];
  const int n = ReadBadgeCmdIds(uiwnd::SafeFindWindow(uiwnd::kMenuIconWndId), flagged,
                                kMaxFlagged);
  for (Icon& ic : icons_) {
    // 🔴 Seules les icônes DE LA GRILLE : la liste ne porte que des commandes de
    // la grille, et le bouton du cash shop partage la commande 0xC0 avec
    // « status » — il s'allumerait à sa place, puis chercherait en vain un
    // bitmap `NC_CashShop_new.bmp` qui n'existe pas.
    if (ic.wnd_id != uiwnd::kMenuIconWndId) { ic.badge = false; continue; }
    bool on = false;
    for (int i = 0; i < n && !on; ++i) on = (flagged[i] == ic.cmd_id);
    if (!on) {  // réarme le chargement pour le prochain allumage
      ic.badge_normal.fail = 0;
      ic.badge_pressed.fail = 0;
    }
    ic.badge = on;
  }
}

// Qui, du bouton natif ou de notre copie ImGui, se montre. Rejouée depuis
// OnTick parce que le client RECRÉE sa fenêtre visible à chaque entrée de carte.
void MenuIcons::SyncCashShopButton() {
  const bool on = CashShopButtonShown();
  // « Remplacé » veut dire remplacé POUR DE BON : notre copie n'existe à l'écran
  // qu'une fois son bitmap chargé. Tant qu'il ne l'est pas — première frame, ou
  // texture perdue à une remise à zéro du device — on laisse le natif, plutôt
  // que de risquer plus aucun bouton du tout.
  bool replaced = false;
  for (Icon& ic : icons_) {
    if (ic.wnd_id != uiwnd::kUInCash_CallWnd) continue;
    ic.hidden = !on;
    replaced = enabled_ && ic.normal.tex != nullptr;
  }
  uiwnd::SafeSetVisible(uiwnd::SafeFindWindow(uiwnd::kUInCash_CallWnd), on && !replaced);
}

float MenuIcons::SnapIcon(float v, float ext, int self, bool y_axis) const {
  constexpr float kSnap = 10.0f;  // px magnetism radius
  float best = v, best_dist = kSnap;
  for (int j = 0; j < static_cast<int>(icons_.size()); ++j) {
    if (j == self || icons_[j].hidden) continue;
    const float opos = y_axis ? static_cast<float>(icons_[j].y)
                              : static_cast<float>(icons_[j].x);
    const float oext = y_axis ? static_cast<float>(icons_[j].normal.h)
                              : static_cast<float>(icons_[j].normal.w);
    const float cands[4] = {opos, opos + oext - ext, opos + oext, opos - ext};
    for (float c : cands) {
      float d = c - v;
      if (d < 0.0f) d = -d;
      if (d < best_dist) { best_dist = d; best = c; }
    }
  }
  return best;
}

void MenuIcons::HideNativeGrid(bool hide) {
  if (hide && !grid_hidden_) {
    if (*reinterpret_cast<uintptr_t*>(kGridDrawSlot) == kGridDrawOrig) {
      mem::PatchValue<void*>(kGridDrawSlot, reinterpret_cast<void*>(&GridClear));
      grid_hidden_ = true;
      // Drop the already-built render nodes now so the native grid disappears
      // immediately (otherwise it lingers until the next relayout / map reload).
      if (void* wnd = uiwnd::FindWindow(uiwnd::kMenuIconWndId))
        GridClear(wnd, nullptr);
      // GridClear empties the node list but the already-composited pixels linger
      // until a relayout — ask the server to clif_refresh so the client
      // re-composites and the ghost vanishes (drained in OnTick, never mid-Present).
      pending_refresh_ = true;
    }
  } else if (!hide && grid_hidden_) {
    mem::PatchValue<void*>(kGridDrawSlot, reinterpret_cast<void*>(kGridDrawOrig));
    grid_hidden_ = false;
    // Symmetric to enable: refresh so the restored native grid re-composites and
    // reappears immediately (otherwise it waits for the next natural relayout).
    pending_refresh_ = true;
  }
}

void MenuIcons::DispatchCommand(int wnd_id, int cmd_id, const char* action_id) {
  // Bouton de Bourgeon : il n'y a aucune fenêtre native à saisir, et surtout
  // aucune raison d'en inventer une. On passe par le catalogue de raccourcis,
  // c'est-à-dire par le MÊME point d'entrée que la touche du joueur — clic et
  // clavier ne peuvent donc pas diverger.
  if (action_id) {
    hotkeys::Invoke(action_id);
    return;
  }
  void* wnd = uiwnd::SafeFindWindow(wnd_id);
  if (!wnd) {
    LogDiag("[MenuIcons] window 0x{:X} not found for cmd 0x{:X}", wnd_id, cmd_id);
    return;
  }
  if (wnd_id == uiwnd::kMenuIconWndId) {
    // action 6 = button-click command; the handler reads the cmd id from arg3.
    reinterpret_cast<CmdHandlerFn>(kCmdHandler)(wnd, 0, 6, cmd_id, 0, 0, 0);
    return;
  }
  // Les icônes qui SONT une fenêtre native (le bouton du cash shop) : leur
  // propre OnMsg est déjà le handler du clic. On le lui passe tel quel plutôt
  // que de refaire son travail — c'est lui qui connaît, par exemple, la branche
  // Steam/Stove (cmd 331) au lieu du CZ 0x0B6D ordinaire.
  uiwnd::OnMsg(wnd, 6, cmd_id);
}

void MenuIcons::FlushPending() {
  // `pending_action_` et non `pending_cmd_` seul : un bouton de Bourgeon n'a pas
  // de commande native, sa commande VAUT 0, et le test d'origine l'aurait
  // silencieusement jeté.
  if (pending_cmd_ == 0 && !pending_action_) return;
  const int wnd = pending_wnd_, cmd = pending_cmd_;
  const char* action = pending_action_;
  pending_wnd_    = 0;
  pending_cmd_    = 0;
  pending_action_ = nullptr;
  // Driven from the game's input phase (ProcessInput, every frame) so the
  // command runs with native click timing/context — never from the Present hook,
  // and never while the HUD is replaced (world map, etc.).
  if (enabled_ && in_game_ && !uiwnd::IsHudReplaced())
    DispatchCommand(wnd, cmd, action);
}

// Fallback only: OnTick is throttled to ~100ms, so ProcessInput (which runs
// first each frame) normally drains pending_cmd_ before this ever sees it.
void MenuIcons::OnTick() {
  // Drain a queued server-refresh request here (update phase, never mid-Present)
  // so the client re-composites and the stale native-grid ghost vanishes.
  if (pending_refresh_ && in_game_) {
    pending_refresh_ = false;
    RequestServerRefresh();
  }
  if (enabled_ && in_game_ && icons_built_) RefreshBadges();
  // Hors du test `enabled_` : c'est aussi ce qui REND le bouton natif quand on
  // cesse de le remplacer. Et le client le recrée visible à chaque entrée de
  // carte, donc il faut y repasser régulièrement, pas seulement aux bascules.
  if (in_game_) SyncCashShopButton();
  FlushPending();
}

// ── Section « MenuIcons » du panneau Moonlight ──────────────────────────
// Déplacée depuis moonlight_ui/panel_interface.cc : ces widgets ne pilotent
// que l'état de CE plugin. MoonlightUi ne garde que l'appel et la décision
// de sauvegarder. Rend true si un réglage a changé.
bool MenuIcons::DrawSettings() {
  bool changed = false;
  SeparatorText(i18n::Tr("Réglages généraux"));
  changed |= ro::RoCheckbox(i18n::Tr("Rendre les icônes déplaçables"), &enabled_);
  SameLine(); HelpMarker(i18n::Tr("Cache la grille native et recrée les icônes fonctionnelles."));

  ImGui::BeginDisabled(!enabled_);

  changed |= ro::RoCheckbox(i18n::Tr("Mode édition (glisser pour déplacer)"), &edit_mode_);
  SameLine(); HelpMarker(
      i18n::Tr("En mode édition : glisse chaque icône pour la repositionner.\n"
      "Aimantage aux autres icônes et à la grille d'alignement.\n"
      "Désactive le mode pour cliquer les icônes normalement."));

  // Per-icon show/hide. icons() is populated once in-game.
  SeparatorText(i18n::Tr("Icônes"));
  // `icon_list` et non `icons` : la locale masquerait l'accesseur icons() dont
  // elle est issue (elle le faisait, via mi->icons() une fois le préfixe retiré).
  auto& icon_list = icons();
  if (icon_list.empty()) {
    ImGui::TextDisabled(i18n::Tr("(disponible une fois en jeu)"));
  } else {
    for (auto& ic : icon_list) {
      // Le bouton du cash shop est bien dans la liste (il se déplace comme les
      // autres) mais PAS ici : sa visibilité a déjà sa case plus bas, celle qui
      // écrit l'option du client. Deux cases pour le même réglage, ce serait
      // une de trop. Les boutons de Bourgeon, eux, restent : leur visibilité
      // n'appartient qu'à nous, et c'est ici qu'on la règle.
      if (ic.wnd_id == uiwnd::kUInCash_CallWnd) continue;
      bool shown = !ic.hidden;
      // Le NOM et non la commande : celle des boutons de Bourgeon vaut 0, ils
      // partageraient donc tous le même identifiant ImGui.
      ImGui::PushID(ic.name);
      // Les icônes du client gardent leur nom de bitmap, celui que le joueur
      // reconnaît dans le dossier ; les nôtres portent leur libellé traduit,
      // qui est le seul nom sous lequel elles existent.
      const char* label = ic.label_fr ? i18n::Tr(ic.label_fr) : ic.name;
      if (ro::RoCheckbox(label, &shown)) {
        ic.hidden = !shown;
        saved_[ic.name] = {ic.x, ic.y, ic.hidden, true};
        changed = true;
      }
      ImGui::PopID();
    }
  }

  ImGui::EndDisabled();

  // ── Boutons natifs posés hors de la grille ────────────────────────────────
  // Hors du BeginDisabled ci-dessus À DESSEIN : ce bouton-là est une fenêtre du
  // client, il reste réglable que notre grille ImGui remplace la native ou non.
  // « Vote Shop » et non « cash shop » : le client l'appelle ainsi parce que sa
  // monnaie est de l'argent, mais sur moonlight elle n'est faite que des votes
  // des joueurs pour le serveur. Le nom natif ne survit que dans les commentaires
  // et les noms de symboles, jamais à l'écran.
  SeparatorText(i18n::Tr("Boutons natifs"));
  bool cash_btn = CashShopButtonShown();
  if (ro::RoCheckbox(i18n::Tr("Bouton du Vote Shop"), &cash_btn)) {
    SetCashShopButtonShown(cash_btn);
    SyncCashShopButton();  // sans attendre le tick : effet immédiat à l'écran
  }
  SameLine(); HelpMarker(
      i18n::Tr("Le bouton carré posé près de la minimap, qui ouvre le Vote Shop.\n"
      "Exactement le réglage de la commande /cashshop : c'est le client qui le\n"
      "retient, dans SaveData\\OptionInfo.lua.\n"
      "Icônes déplaçables activées, il se déplace comme les autres icônes."));

  return changed;
}

void MenuIcons::OnRenderUI() {
  if (!enabled_ || !in_game_) {
    HideNativeGrid(false);  // restore native grid when disabled
    return;
  }
  if (Bourgeon::Instance().client().session().aid() == 0) return;

  HideNativeGrid(true);
  if (!icons_built_) BuildIconList();

  // A full-screen UI (world map, etc.) replaces the HUD: hide our icons too
  // (clicks are also rejected in OnTick).

  // Textures D3DPOOL_DEFAULT : mortes après reset/recréation du device -> on nulle
  // les handles cachés pour forcer le rechargement paresseux (sinon draw = crash).
  {
    static unsigned s_epoch = 0;
    const unsigned e = Overlay_DeviceEpoch();
    if (e != s_epoch) {
      for (Icon& ic : icons_) {
        ic.normal = Bitmap();
        ic.pressed = Bitmap();
        ic.badge_normal = Bitmap();
        ic.badge_pressed = Bitmap();
      }
      s_epoch = e;
    }
  }

  for (int i = 0; i < static_cast<int>(icons_.size()); ++i) {
    Icon& ic = icons_[i];
    if (ic.hidden) continue;  // user-hidden via the MoonlightUi list
    EnsureBitmap(ic.normal, ic.dir, ic.name, "");  // lazy/retry
    if (!ic.normal.tex) continue;

    // L'état ENFONCÉ, chargé une fois pour toutes dès la première frame : à la
    // différence du badge, il ne dépend d'aucun événement, et l'attendre du
    // premier clic ferait manquer le retour visuel de ce clic-là.
    EnsureBitmap(ic.pressed, ic.dir, ic.name, "_press");
    // Icône signalée : on charge son bitmap « _new » (celui qui porte le N) à la
    // première frame où le natif l'allume — et son état enfoncé avec, pour la
    // même raison que ci-dessus.
    if (ic.badge) {
      EnsureBitmap(ic.badge_normal, ic.dir, ic.name, "_new");
      EnsureBitmap(ic.badge_pressed, ic.dir, ic.name, "_new_press");
    }
    // Le bitmap « _new » déborde vers le haut (le natif le pose 6 px plus haut
    // que l'icône normale, même x). On aligne les BAS sur la hauteur MESURÉE
    // plutôt que de recopier ce 6 : la fenêtre grandit d'autant, sinon ImGui
    // rognerait le badge à son bord. (ic.x, ic.y) reste l'ancre de l'icône
    // normale, donc la position enregistrée ne bouge pas quand le badge s'allume.
    const bool  badge  = ic.badge && ic.badge_normal.tex != nullptr;
    const int   draw_w = badge ? ic.badge_normal.w : ic.normal.w;
    const int   draw_h = badge ? ic.badge_normal.h : ic.normal.h;
    const float over_y = static_cast<float>(draw_h - ic.normal.h);  // débordement vers le haut

    // 🔴 La fenêtre porteuse est identifiée par le NOM du bitmap, qui est déjà la
    // clé de persistance de l'icône et le seul champ unique à coup sûr : la
    // commande ne l'est PAS (le bouton du cash shop et l'icône « status » ont
    // toutes deux 0xC0) et les boutons de Bourgeon n'en ont aucune. Deux fenêtres
    // ImGui du même nom n'en font qu'UNE — la seconde s'attribuerait la position
    // et la zone cliquable de la première.
    char id[64];
    std::snprintf(id, sizeof(id), "##micon_%s", ic.name);
    // Pin the window to the stored position every frame (NoMove) and drive any
    // drag ourselves, so the drawn icon and its hit-rect never desync.
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(ic.x),
                                   static_cast<float>(ic.y) - over_y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(draw_w),
                                    static_cast<float>(draw_h)), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags f =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    ImGui::Begin(id, nullptr, f);
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + draw_w, p0.y + draw_h);
    // Coin haut-gauche de l'icône elle-même : décalé du débordement du badge.
    // C'est LUI qui sert au glisser et à l'aimantage, qui raisonnent tous sur
    // (ic.x, ic.y) et sur la taille de l'icône normale.
    const ImVec2 icon_p0(p0.x, p0.y + over_y);

    // Frameless: a transparent full-window button (interaction) + the bitmap
    // drawn directly, so there's no ImGui button frame around the icon.
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    const bool clicked = ImGui::InvisibleButton(
        "b", ImVec2(static_cast<float>(draw_w), static_cast<float>(draw_h)));
    const bool hovered = ImGui::IsItemHovered();
    // Enfoncé = le bouton de la souris tenu SUR l'icône, ce qu'ImGui appelle
    // « actif ». En mode édition, l'icône est active pendant tout le glisser :
    // l'état enfoncé y serait un mensonge (rien ne s'ouvrira), et le surlignage
    // jaune dit déjà ce qui se passe.
    const bool held = !edit_mode_ && ImGui::IsItemActive();
    // Le bitmap à peindre. 🔴 Chaque état a un REPLI vers l'état voisin qui
    // existe, jamais rien : une icône sans « _press » (le bouton du cash shop, à
    // qui un seul bitmap sert les trois états) doit rester dessinée quand on
    // appuie dessus, pas disparaître.
    const MenuIcons::Bitmap& drawn =
        badge ? (held && ic.badge_pressed.tex ? ic.badge_pressed : ic.badge_normal)
              : (held && ic.pressed.tex ? ic.pressed : ic.normal);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage((ImTextureID)(uintptr_t)drawn.tex, p0, p1);

    if (edit_mode_) {
      const ImVec2 mp = ImGui::GetIO().MousePos;
      if (ImGui::IsItemActivated()) {
        dragging_   = i;
        drag_off_x_ = mp.x - icon_p0.x;
        drag_off_y_ = mp.y - icon_p0.y;
      }
      if (dragging_ == i && ImGui::IsItemActive()) {
        float nx = mp.x - drag_off_x_, ny = mp.y - drag_off_y_;
        nx = SnapIcon(nx, static_cast<float>(ic.normal.w), i, false);  // magnetic to icons
        ny = SnapIcon(ny, static_cast<float>(ic.normal.h), i, true);
        if (auto* mui = Bourgeon::Instance().moonlight_ui()) {  // shared grid snap
          const ImVec2 ds = ImGui::GetIO().DisplaySize;
          nx = mui->grid_.SnapAxis(nx, ds.x);
          ny = mui->grid_.SnapAxis(ny, ds.y);
        }
        // L'icône reste entièrement dans l'écran de jeu. APRÈS les deux aimantations
        // (icônes voisines + grille), qui peuvent elles-mêmes pousser dehors ; le clamp
        // global de ui/window_clamp.h ne peut rien ici, la fenêtre étant réépinglée à
        // (ic.x,ic.y) en Cond_Always chaque frame.
        const ImVec2 in_screen = ro::ClampWindowPosToScreen(
            ImVec2(nx, ny),
            ImVec2(static_cast<float>(ic.normal.w), static_cast<float>(ic.normal.h)));
        nx = in_screen.x;
        ny = in_screen.y;
        ic.x = static_cast<int>(nx + 0.5f);
        ic.y = static_cast<int>(ny + 0.5f);
      }
      if (dragging_ == i && ImGui::IsItemDeactivated()) {
        dragging_ = -1;
        saved_[ic.name] = {ic.x, ic.y, ic.hidden, true};
        geometry_dirty_ = true;  // MoonlightUi persists once on release
      }
      // Edit-mode highlight so the draggable cells are visible.
      dl->AddRectFilled(p0, p1, IM_COL32(255, 220, 80, hovered ? 70 : 35));
      dl->AddRect(p0, p1, IM_COL32(255, 220, 80, 220));
    } else {
      if (clicked) {  // dispatched from OnTick/input phase
        pending_wnd_    = ic.wnd_id;
        pending_cmd_    = ic.cmd_id;
        pending_action_ = ic.action_id;
      }
      if (hovered) {
        // Utf8 et non Cp949 : cette infobulle est dessinee par ImGui, pas par
        // le moteur de texte natif. Les boutons de Bourgeon n'ont pas de ligne
        // dans la table du client : c'est notre catalogue i18n qui les nomme.
        const char* tip = ic.msg_id ? msgstr::Utf8(ic.msg_id)
                                    : (ic.label_fr ? i18n::Tr(ic.label_fr) : "");
        char key[80] = {0};
        HotkeyLabel(ic, key, sizeof(key));
        if (tip && *tip) {
          if (key[0]) ImGui::SetTooltip("%s  (%s)", tip, key);
          else        ImGui::SetTooltip("%s", tip);
        } else if (key[0]) {
          ImGui::SetTooltip("(%s)", key);
        }
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }
}
