#include "features/overlays/minimap.h"
#include "ragnarok/actor.h"  // rag::actor : les offsets de CActorSprite

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "bourgeon.h"
#include "features/windows/navigation_window.h"  // le point de sortie de l'itinéraire
#include "d3d9/d3d9_hook.h"  // Overlay_SetTextureFilter
#include "features/moonlight_ui/moonlight_ui.h"
#include "ragnarok/globals.h"
#include "ragnarok/navigation.h"
#include "ui/game_texture.h"
#include "ragnarok/uiwnd.h"  // FindWindow / CloseWindow / MakeWindow
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"  // mui::WheelSliderInt, HelpMarker, SeparatorText…
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"
#include "utils/log_console.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ragnarok/stl_node.h"  // rag::treenode : le nœud du conteneur

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ===========================================================================
// Minimap ImGui — étape 1 : le bitmap de la carte + le point du joueur.
// Le RE complet du radar natif est dans docs/minimap_re.md.
// ===========================================================================

namespace {

// ── Adresses du client 20250716 ──────────────────────────────────────────────

// (Le global du nom de carte courante vit désormais dans `rag::CurrentMapName`
// — la navigation en a eu besoin à son tour. Même adresse, même retrait
// d'extension : on demande toujours le MÊME fichier que la grande carte native.)

// CGameMode -> monde -> { acteur du joueur, info de carte }.
// Relevés dans Scene_WorldPosToCellXY (0x00c6ac10) et sub_C6AEF0 (0x00c6aef0),
// qui les enchaînent exactement ainsi.
constexpr int kGm_World       = 0x0cc;
constexpr int kWorld_OwnActor = 0x2c;
constexpr int kWorld_MapInfo  = 0x30;
constexpr int kMapInfo_Width  = 0x110;  // int, largeur en CELLULES
constexpr int kMapInfo_Height = 0x114;  // int, hauteur en CELLULES
constexpr int kMapInfo_Cell   = 0x118;  // int, taille d'une cellule en unités monde

// Le bouton du radar natif dont on emprunte l'art. Ses cinq boutons suivent tous
// le même gabarit `minimap\i_<nom>_<état>.bmp`, les états valant 1 = normal,
// 2 = survol, 3 = enfoncé (`UIMinimapZoomWnd_CreateControls` 0x008a9220).
//
// ⚠ `viewon` ouvre la carte du MONDE chez le natif ; on n'en garde que l'image.
// C'est de l'art emprunté, pas une commande reprise — d'où l'infobulle, qui est
// la seule chose qui dit au joueur ce que ce bouton fait CHEZ NOUS.
constexpr char kCfgButtonName[] = "viewon";

// Curseur RO « main » : la valeur que le hook curseur pose dans *(CursorMgr+0x50)
// pour la frame en cours. Le natif la montre sur tout ce qui se clique — nos
// boutons dessinés à la main doivent la demander eux-mêmes, `ro::RoButton` le
// fait déjà pour les siens.
constexpr int kRoCursorHand = 2;

// Les deux écrans natifs qu'on ne remplace pas encore sont offerts depuis le
// menu : `uiwnd::kUIRoMapWnd` et `uiwnd::kUINavigationV4Wnd`. Comment
// l'identifiant de la navigation a été MESURÉ — après une déduction fausse — est
// écrit avec lui, dans le foyer.

// Plafond de points du TRACÉ d'itinéraire. Ce ne sont pas les cellules du
// chemin mais ses COINS : `RouteCellPath` décime aux changements de direction,
// et un chemin de plusieurs centaines de cellules en tient une trentaine. 256
// laisse donc une marge confortable même pour un labyrinthe.
constexpr size_t kMaxPathPoints = 256;

// ── Le veto du dessin natif ──────────────────────────────────────────────────
//
// 🔴 Fermer la fenêtre 14 au battement de frame NE SUFFIT PAS, et aucun réglage
// de cadence n'y changera rien. Le client recrée son radar au MILIEU de sa
// frame (traitement des paquets d'entrée de carte) et le dessine plus loin dans
// CETTE MÊME frame ; notre battement, lui, passe au DÉBUT de la frame et est
// donc déjà passé. Il restait une frame pleine de radar natif à chaque
// téléport — parfaitement visible au sortir d'un écran de chargement.
//
// Le seul point qui soit à coup sûr APRÈS toute création et AVANT tout dessin,
// c'est le site d'appel du dessin lui-même :
//   `GameMode_InGame_ProcessFrame+0x542 : call GameMode_DrawMiniMap`.
// Sauter cet appel revient EXACTEMENT à ce que fait le client quand la fenêtre
// 14 n'existe pas — la première chose que teste `GameMode_DrawMiniMap` est
// `if (!g_MinimapZoomWnd) return`. Le veto ne prive donc le jeu de rien d'autre
// que d'un dessin, et il emporte AUSSI tous les marqueurs : ils sont posés
// depuis cette fonction, et de nulle part ailleurs (relevé des xrefs de
// `GameMode_DrawMiniMapMarker`).
//
// ⚠ Le détour se pose sur le SITE D'APPEL et pas sur la fonction : le prologue
// de `GameMode_DrawMiniMap` installe un cadre SEH (`push -1` / `push handler` /
// `mov eax, fs:0`), que le JMP-hook ne sait pas relayer.
constexpr uintptr_t kDrawMiniMapCall  = 0x00c74fc2;  // call GameMode_DrawMiniMap
constexpr uintptr_t kDrawMiniMapAfter = 0x00c74fc7;  // l'instruction suivante
// Les cinq octets attendus là : `call rel32` vers 0x00c66ab0. Vérifiés avant de
// poser le détour, parce que l'exe livré porte des correctifs WARP que l'IDB ne
// montre pas — écrire un JMP par-dessus autre chose qu'un appel de 5 octets ne
// pardonnerait pas.
constexpr uint8_t kDrawMiniMapCallBytes[5] = {0xE8, 0xE9, 0x1A, 0xFF, 0xFF};

// ── État du module ───────────────────────────────────────────────────────────

MinimapConfig g_cfg;
bool g_in_game = false;
bool g_needs_save = false;

// Plafond de marqueurs par carte. Ce n'est pas une contrainte de format — le
// yaml n'en a pas — mais de LISIBILITÉ : au-delà, la carte et la liste du menu
// deviennent illisibles, et c'est le moment de se demander ce qu'on cherche.
constexpr int kMaxMemosPerMap = 32;

// « Engagée » : Maj est maintenue au-dessus de la minimap, ou l'un de nos deux
// menus est ouvert. C'est ce qui montre le bouton de réglages et, en mode
// traverser les clics, rend la fenêtre interactive le temps du geste.
bool g_shift_engaged = false;
bool g_menu_open = false;

// Temps de survol, en secondes, avant que le rappel « Maj » ne s'affiche.
// Calé sur le délai des infobulles d'ImGui : assez long pour qu'un curseur qui
// ne fait que traverser la carte n'en déclenche aucune.
constexpr float kHintHoverDelay = 0.6f;

// Cellule visée par le dernier clic droit, en attente dans le menu d'ajout.
int  g_memo_cell_x = 0;
int  g_memo_cell_y = 0;
char g_memo_name[64] = {};

// Couleurs d'un popup qui doit se lire comme un corps de fenêtre RO.
//
// 🔴 Sans elles, `ro::RoButton` peint son libellé avec le `ImGuiCol_Text`
// ambiant — le blanc d'ImGui — sur l'art clair du bouton : illisible. Le même
// piège guette `TextDisabled`, qui porte le « (?) » des aides. Posées AVANT
// `BeginPopup` et retirées qu'il s'ouvre ou non, sinon la pile de styles se
// déséquilibre les frames où il est fermé.
void PushRoPopupColors() {
  const ro::RoSkinConfig& skin = ro::SkinConfig();
  ImGui::PushStyleColor(ImGuiCol_PopupBg,
                        ImVec4(skin.body_col[0], skin.body_col[1],
                               skin.body_col[2], skin.body_col[3]));
  ImGui::PushStyleColor(ImGuiCol_Border,
                        ImVec4(skin.border_col[0], skin.border_col[1],
                               skin.border_col[2], skin.border_col[3]));
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImVec4(skin.body_text[0], skin.body_text[1],
                               skin.body_text[2], skin.body_text[3]));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled,
                        ImVec4(skin.body_text[0], skin.body_text[1],
                               skin.body_text[2], 0.65f));
}
inline void PopRoPopupColors() { ImGui::PopStyleColor(4); }

// Nom de carte de repli, posé par OnModeSwitch. Il porte l'extension `.rsw` que
// le client ajoute à son nom de fichier de scène ; on la retire à l'usage.
char g_map_fallback[64] = {};

// Un champ à un offset : la lecture est celle de tout le monde (globals.h),
// et le `using` garde les points d'appel de ce fichier tels quels.
using rag::Read;

// Nom LISIBLE d'un lieu — « PvP : Room Copass » pour `pvp_n_3-5`. C'est le
// premier `%s` du titre de la grande carte native.
//
// La RE a DÉMÉNAGÉ dans `rag::MapDisplayName` (ragnarok/globals.h) le jour où un
// second appelant est apparu — les liens de navigation du chat, qui doivent
// afficher le nom que le joueur lit ailleurs. Rien n'a changé de comportement,
// seulement le lieu de l'adresse.
bool MapDisplayName(const char* map_no_ext, char* out, size_t cap) {
  return rag::MapDisplayName(map_no_ext, out, cap);
}

// Nom de la carte courante, sans extension.
//
// DEUX sources, dans cet ordre :
//   1. le `map_name` que nous passe OnModeSwitch — notre propre plomberie, donc
//      la source dont on répond ; on lui retire son extension (`.rsw`) ;
//   2. à défaut — module chargé sans qu'un changement de mode ait eu lieu — le
//      global que le client injecte lui-même dans son gabarit
//      `유저인터페이스\map\%s.bmp` (UIMiniMapWnd_DrawContent @0x00962441).
//
// ⚠ Ce global (2) a été relevé au DÉSASSEMBLAGE, pas mesuré en jeu : il n'est
// volontairement que le recours. S'il s'avère porter autre chose, le chemin
// normal reste (1) et rien ne change à l'écran.
bool CurrentMapName(char* out, size_t cap) {
  out[0] = '\0';

  if (g_map_fallback[0] != '\0') {
    strncpy_s(out, cap, g_map_fallback, _TRUNCATE);
    if (char* dot = strrchr(out, '.')) *dot = '\0';
    if (out[0] != '\0') return true;
  }

  // Le global du client, second choix — la RE a déménagé dans rag:: le jour où
  // la navigation en a eu besoin elle aussi.
  return rag::CurrentMapName(out, cap);
}

// ── Les trois cartes de marqueurs de CGameMode ───────────────────────────────
// Chacune est une `std::map` EMBARQUÉE dans l'objet : l'offset donne le nœud
// sentinelle (`_Myhead`), et la taille suit immédiatement. C'est ce que fait
// `GameMode_DrawMiniMapPartyGuildQuestMarkers` (0x00c66060), qui itère depuis
// `*(gm+0x1B4)` et pousse `*(gm+0x1B8)` comme longueur de liste.
constexpr int kGm_PartyMap = 0x1b4;  // std::map<GID, {int x, int y, D3DCOLOR}>
constexpr int kGm_GuildMap = 0x1bc;  // idem
// Les repères posés par le SERVEUR : `ZC_COMPASS` (0x0144), ce que la commande
// de script `viewpoint` envoie. std::map<id, {int x, int y, int permanent,
// D3DCOLOR, DWORD tick}> ; `sub_C65FE0` (0x00c65fe0) en est l'`erase` par id,
// ce qui confirme la clé à +0x10.
constexpr int kGm_ViewpointMap = 0x1c4;
constexpr int kGm_QuestMap = 0x1f0;  // std::map<clé, {s16 x, y, type, sous-type}>

// Le marqueur de BOSS — ce que révèle un Convex Mirror. Trois champs nus, pas
// une liste : le client n'en suit qu'un à la fois.
constexpr int kGm_BossX    = 0x5c4;  // int, cellule
constexpr int kGm_BossY    = 0x5c8;  // int, cellule
constexpr int kGm_BossKnown = 0x5cc; // byte : 0 = aucun boss connu

// La valeur du nœud est une `pair` dont la CLÉ vient en tête (le GID) : les
// champs utiles sont donc derrière elle, et s'écrivent en DÉCALAGE de la valeur
// plutôt qu'en absolu — le lien reste visible.
constexpr int kVal = rag::treenode::kValue;
constexpr int kPos_X      = kVal + 0x4;  // groupe/guilde : int ; quête : s16
constexpr int kPos_Y      = kVal + 0x8;  // groupe/guilde : int
constexpr int kPos_Color  = kVal + 0xc;  // groupe/guilde : D3DCOLOR (ARGB)
constexpr int kQuest_Y    = kVal + 0x6;  // quête : s16
constexpr int kQuest_Type = 0x18;  // quête : s16
constexpr int kQuest_Sub  = 0x1a;  // quête : s16, le %d de quest_%d.bmp
// Viewpoint : x et y sont aux mêmes offsets que groupe/guilde, la couleur est
// DÉCALÉE parce qu'un champ de plus s'intercale.
constexpr int kVp_Permanent = 0x1c;  // int : 0 = expire au bout de 15 s
constexpr int kVp_Color     = 0x20;  // D3DCOLOR
constexpr int kVp_Tick      = 0x24;  // DWORD, timeGetTime à la pose

// Durée de vie d'un viewpoint NON permanent, et cadence de clignotement : les
// deux valeurs du natif (`GameMode_DrawMiniMap`), reprises telles quelles.
constexpr unsigned kVpLifetimeMs = 15000;
constexpr unsigned kVpBlinkMs    = 1000;

constexpr int kMaxMarkers = 64;

// Quelle famille d'arbre on parcourt : les trois n'ont pas la même charge utile
// derrière la clé.
enum class TreeKind { kPosColor, kQuest, kViewpoint };

struct Marker {
  int   cell_x = 0;
  int   cell_y = 0;
  ImU32 color = 0;
  int   variant = 0;    // quêtes : le sous-type, qui choisit le bitmap
  unsigned tick = 0;    // viewpoints : la pose, pour le clignotement
};

// Une D3DCOLOR du client (ARGB) en couleur ImGui (ABGR) : rouge et bleu
// échangés, l'alpha à sa place.
inline ImU32 ArgbToImU32(uint32_t argb) {
  return IM_COL32((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff,
                  (argb >> 24) & 0xff);
}

// Parcours en profondeur d'un `std::_Tree`, borné par la capacité de sortie ET
// par une pile fixe : un arbre corrompu ne doit pas faire tourner le client en
// rond. L'ordre est sans importance — on ne fait qu'empiler des points à
// dessiner. POD uniquement, pour rester compatible avec le SEH.
int CollectTree(void* gm, int map_off, TreeKind kind, Marker* out, int cap) {
  int n = 0;
  __try {
    if (!gm) return 0;
    uint8_t* head = Read<uint8_t*>(gm, map_off);
    if (!head) return 0;
    uint8_t* stack[64];
    int sp = 0;
    uint8_t* root = Read<uint8_t*>(head, rag::treenode::kParent);
    if (root && root != head) stack[sp++] = root;

    while (sp > 0 && n < cap) {
      uint8_t* node = stack[--sp];
      if (!node || Read<uint8_t>(node, rag::treenode::kIsNil) != 0) continue;

      Marker& m = out[n];
      bool keep = true;
      if (kind == TreeKind::kQuest) {
        m.cell_x = Read<int16_t>(node, kPos_X);
        m.cell_y = Read<int16_t>(node, kQuest_Y);
        m.variant = Read<int16_t>(node, kQuest_Sub);
        m.color = IM_COL32_WHITE;
        // Sous-type nul = entrée vide : c'est le test du natif (`if (v24)`)
        // avant de composer son nom de bitmap.
        keep = (m.variant != 0);
      } else if (kind == TreeKind::kViewpoint) {
        m.cell_x = Read<int>(node, kPos_X);
        m.cell_y = Read<int>(node, kPos_Y);
        m.color = ArgbToImU32(Read<uint32_t>(node, kVp_Color));
        m.tick = Read<uint32_t>(node, kVp_Tick);
        // 🔴 On FILTRE les périmés, on ne les RETIRE pas : la liste appartient
        // au client, et c'est son propre dessin qui fait le ménage. Y toucher
        // depuis notre rendu casserait son itération en cours.
        const bool permanent = Read<int>(node, kVp_Permanent) != 0;
        keep = permanent || (GetTickCount() - m.tick) < kVpLifetimeMs;
      } else {
        m.cell_x = Read<int>(node, kPos_X);
        m.cell_y = Read<int>(node, kPos_Y);
        m.color = ArgbToImU32(Read<uint32_t>(node, kPos_Color));
      }
      if (keep) ++n;

      uint8_t* l = Read<uint8_t*>(node, rag::treenode::kLeft);
      uint8_t* r = Read<uint8_t*>(node, rag::treenode::kRight);
      if (sp < 62) {
        if (l && Read<uint8_t>(l, rag::treenode::kIsNil) == 0) stack[sp++] = l;
        if (r && Read<uint8_t>(r, rag::treenode::kIsNil) == 0) stack[sp++] = r;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
  return n;
}

// ── Objets de carte : le CTownInfoMgr du client ──────────────────────────────
//
// 🔴 On LIT sa table, on ne relit pas `Towninfo.lub`. Le manager a déjà fait le
// travail que refaire coûterait cher et divergerait : choix de `SystemEN\` ou
// `System\`, fusion de `Towninfo_C.lub` par-dessus (`F_ROTP`), et lecture par le
// VFS — donc le fichier peut vivre dans un GRF sans exister sur le disque.
// Et surtout : c'est une lecture mémoire. `sub_A81E00`, l'accesseur natif,
// CONSTRUIT deux `std::string` dans le tampon de sortie ; l'appeler nous
// mettrait à gérer l'allocateur du client. Ici, rien n'est alloué.
//
// Forme : std::map<std::string /*carte*/, std::vector<Rec60>>.
constexpr uintptr_t kTownInfoMgrPtr = 0x0159c08c;  // CTownInfoMgr* — à DÉRÉFÉRENCER
constexpr int kTown_TreeHead  = 0x08;  // nœud sentinelle de la map
constexpr int kTownNode_Key   = 0x10;  // std::string : le nom de carte
constexpr int kTownNode_First = 0x28;  // vecteur d'enregistrements : _Myfirst
constexpr int kTownNode_Last  = 0x2c;  // _Mylast
constexpr int kTownRecSize    = 60;
constexpr int kTownRec_Type   = 0x00;  // int (0..9 ; 8 = warp, 9 = quête)
constexpr int kTownRec_Name   = 0x04;  // std::string : le libellé affiché
constexpr int kTownRec_X      = 0x1c;  // int, cellule
constexpr int kTownRec_Y      = 0x20;  // int, cellule
constexpr int kTownRec_Bitmap = 0x24;  // std::string : chemin CP949 de l'icône

// std::string de MSVC (32 bits) : seize octets qui portent SOIT le texte court,
// SOIT un pointeur, puis la taille et la capacité. C'est la CAPACITÉ qui
// tranche — au-delà de 15, le texte est ailleurs.
// Copie une std::string du client. LECTURE SEULE : on ne touche ni sa taille ni
// sa capacité, donc aucun risque côté allocateur.
bool ReadClientString(const void* field, char* out, int cap) {
  rag::clientstr::CopyTruncating(field, out, cap);
  // Le contrat d'origine était « j'ai copié au moins un caractère ». Un tampon
  // resté vide dit exactement la même chose, et les trois appelants s'en servent
  // comme d'un « ai-je quelque chose à lire ? ».
  return out[0] != '\0';
}

struct TownIcon {
  int  cell_x = 0;
  int  cell_y = 0;
  int  type = 0;
  char bmp[96] = {};
  char name[64] = {};  // libellé affiché — l'infobulle du bouton natif
};

// ── Routage de navigation ────────────────────────────────────────────────────

// Lance l'itinéraire vers (carte, x, y), comme le fait le clic sur une icône du
// radar natif.
//
// ⚠ Jumeau de `StartNavigation` dans features/windows/item_desc_window.cc, et
// volontairement PAS factorisé : les deux passent des constantes DIFFÉRENTES —
// un lien `<NAVI>` d'objet route avec (type du lien, flags 1, a30 0), le chemin
// de la minimap avec (type 0, flags 5, a30 1002), relevés chacun sur leur site
// natif. Une fonction commune ne serait qu'un passe-plat à huit paramètres, et
// masquerait que ces valeurs ne sont pas interchangeables.
//
// 🔴 Le nom de carte doit être SANS extension et tenir en 15 caractères : on
// reste en SSO, donc rien n'est alloué et le client n'a rien à libérer.

// Les objets déclarés pour `map_name`. Le nom de carte est la CLÉ de la map ; on
// parcourt l'arbre et on compare, plutôt que d'appeler le `find` natif — un
// `find` prendrait la clé par valeur, et nous voilà de nouveau à fabriquer une
// std::string pour le client.
int CollectTownIcons(const char* map_name, TownIcon* out, int cap) {
  int n = 0;
  __try {
    if (!map_name || !map_name[0]) return 0;
    void* mgr = *reinterpret_cast<void**>(kTownInfoMgrPtr);
    if (!mgr) return 0;
    uint8_t* head = Read<uint8_t*>(mgr, kTown_TreeHead);
    if (!head) return 0;

    uint8_t* stack[64];
    int sp = 0;
    uint8_t* root = Read<uint8_t*>(head, rag::treenode::kParent);
    if (root && root != head) stack[sp++] = root;

    while (sp > 0 && n < cap) {
      uint8_t* node = stack[--sp];
      if (!node || Read<uint8_t>(node, rag::treenode::kIsNil) != 0) continue;

      char key[64];
      if (ReadClientString(node + kTownNode_Key, key, sizeof(key)) &&
          _stricmp(key, map_name) == 0) {
        uint8_t* first = Read<uint8_t*>(node, kTownNode_First);
        uint8_t* last  = Read<uint8_t*>(node, kTownNode_Last);
        if (first && last && last > first) {
          const int count = static_cast<int>((last - first) / kTownRecSize);
          for (int i = 0; i < count && n < cap; ++i) {
            const uint8_t* rec = first + i * kTownRecSize;
            TownIcon& t = out[n];
            t.type   = Read<int>(rec, kTownRec_Type);
            t.cell_x = Read<int>(rec, kTownRec_X);
            t.cell_y = Read<int>(rec, kTownRec_Y);
            // Le chemin porté par l'ENREGISTREMENT, pas une table à nous : le
            // client le pose depuis son propre tableau de dix icônes
            // (0x0159C090, indexé par le type), et un patch qui le change nous
            // suivra sans qu'on ait à le savoir.
            // Le libellé peut manquer sans que l'entrée soit invalide : c'est le
            // BITMAP qui décide, comme chez le natif.
            ReadClientString(rec + kTownRec_Name, t.name, sizeof(t.name));
            if (ReadClientString(rec + kTownRec_Bitmap, t.bmp, sizeof(t.bmp)))
              ++n;
          }
        }
        break;  // une seule entrée par carte
      }

      uint8_t* l = Read<uint8_t*>(node, rag::treenode::kLeft);
      uint8_t* r = Read<uint8_t*>(node, rag::treenode::kRight);
      if (sp < 62) {
        if (l && Read<uint8_t>(l, rag::treenode::kIsNil) == 0) stack[sp++] = l;
        if (r && Read<uint8_t>(r, rag::treenode::kIsNil) == 0) stack[sp++] = r;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
  return n;
}

// Cellule du boss révélé (Convex Mirror), false si aucun n'est connu.
//
// 🔴 Le drapeau reste ARMÉ quand le boss est MORT : le serveur annonce alors
// la tombe (l'heure de réapparition) SANS position, et le client garde 0,0 dans
// ses deux champs — d'où l'icône planquée au coin bas-gauche pendant toute
// l'attente. On refuse cette cellule : 0,0 est le coin de la carte, jamais une
// case où un boss se tient.
//
// ⚠ Fonction à part, et pas un `__try` posé dans le rendu : MSVC refuse le SEH
// dans une fonction qui doit dérouler des objets C++ (C2712), et `OnRenderUI` en
// manipule à la pelle. C'est la même contrainte qui a donné leur forme aux
// autres lecteurs de ce fichier.
bool ReadBossCell(int* out_x, int* out_y) {
  __try {
    void* gm = rag::ActiveModeSafe();
    if (!gm || Read<uint8_t>(gm, kGm_BossKnown) == 0) return false;
    const int bx = Read<int>(gm, kGm_BossX);
    const int by = Read<int>(gm, kGm_BossY);
    // Tombe active : aucune position à donner, on masque au lieu de coller
    // l'icône dans le coin.
    if (bx <= 0 && by <= 0) return false;
    *out_x = bx;
    *out_y = by;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Ce que le rendu doit savoir de la carte et du joueur, relu à chaque frame.
struct Snapshot {
  bool  ok = false;
  int   cells_w = 0, cells_h = 0;  // taille de la carte, en cellules
  float cell_x = 0.0f;             // cellule CONTINUE du joueur (pas d'arrondi)
  float cell_y = 0.0f;
  float angle = 0.0f;              // orientation de la flèche, en degrés
};

bool ReadSnapshot(Snapshot* out) {
  __try {
    void* gm = rag::ActiveModeSafe();
    if (!gm) return false;
    void* world = Read<void*>(gm, kGm_World);
    if (!world) return false;
    void* info  = Read<void*>(world, kWorld_MapInfo);
    void* actor = Read<void*>(world, kWorld_OwnActor);
    if (!info || !actor) return false;

    const int w    = Read<int>(info, kMapInfo_Width);
    const int h    = Read<int>(info, kMapInfo_Height);
    const int cell = Read<int>(info, kMapInfo_Cell);
    if (w <= 0 || h <= 0 || cell <= 0) return false;

    // Arithmétique de Scene_WorldPosToCellXY, moins son `floor` final : c'est ce
    // qui fait glisser le point au lieu de le faire sauter d'une cellule à
    // l'autre. 🔴 La division par deux est ENTIÈRE dans le natif — la refaire en
    // flottant décalerait d'une demi-cellule sur toute carte de taille impaire.
    out->cells_w = w;
    out->cells_h = h;
    out->cell_x = static_cast<float>(w / 2) +
                  Read<float>(actor, rag::actor::kPosX) / static_cast<float>(cell);
    out->cell_y = static_cast<float>(h / 2) +
                  Read<float>(actor, rag::actor::kPosZ) / static_cast<float>(cell);
    out->angle = Read<float>(actor, rag::actor::kFacing);
    out->ok = true;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Filtre d'échantillonnage. 🔴 Posé dans les DEUX cas, jamais seulement pour le
// mode « non par défaut » : l'état ambiant d'une draw list dépend de ce qui a
// été dessiné avant elle (feedback_imgui_texture_filter_ambient). Et on restaure
// en POINT explicite, pas avec ImDrawCallback_ResetRenderState, qui rendrait la
// main en LINEAR aux blits de skin suivants.
void ImCb_MapFilter(const ImDrawList*, const ImDrawCmd*) {
  Overlay_SetTextureFilter(g_cfg.smooth);
}
void ImCb_RestorePoint(const ImDrawList*, const ImDrawCmd*) {
  Overlay_SetTextureFilter(false);
}

// Les quatre coins d'un quad pivoté de `deg` degrés, rotation d'écran USUELLE
// (sens horaire, Y vers le bas) :
//     x = cx + u·cos(θ) − v·sin(θ)
//     y = cy + u·sin(θ) + v·cos(θ)
//
// 🔴 NE PAS y remettre la formule de coins interne du moteur. `sub_A74E90`
// (0x00a74e90) applique `x = cx + u·sinθ − v·cosθ ; y = cy + u·cosθ + v·sinθ`,
// ce qui vaut une rotation de **90° − θ** et non de θ. Transposée telle quelle
// ici, elle MIROITE la flèche : l'échange θ ↔ 90−θ est une réflexion dans
// l'espace des angles.
//
// Le symptôme l'a prouvé au chiffre près (constaté en jeu, 2026-08-15) : la
// flèche ne pointait juste qu'au NORD-EST et au SUD-OUEST. Avec la formule du
// moteur elle vise (−cos A, −sin A), avec celle-ci (sin A, cos A) ; les deux ne
// coïncident que si −cos A = sin A, soit A = 135° et A = 315° — exactement ces
// deux directions-là, et aucune autre. Un demi-tour, lui, n'aurait laissé
// AUCUNE direction correcte : c'est ce qui distingue un miroir d'une inversion.
//
// L'angle reste celui que le natif passe (θ = 180 − angle de l'acteur) : c'est
// la CONVENTION DE ROTATION qui était en cause, pas la valeur.
//
// Sortie dans l'ordre haut-gauche, haut-droit, bas-droit, bas-gauche : celui
// qu'attend ImDrawList::AddImageQuad.
void RotatedQuad(float cx, float cy, float hw, float hh, float deg,
                 ImVec2 out[4]) {
  const float th = deg * 3.14159265358979f / 180.0f;
  const float s = sinf(th);
  const float c = cosf(th);
  const float u[4] = {-hw, +hw, +hw, -hw};
  const float v[4] = {-hh, -hh, +hh, +hh};
  for (int i = 0; i < 4; ++i)
    out[i] = ImVec2(cx + u[i] * c - v[i] * s, cy + u[i] * s + v[i] * c);
}

// Côté minimal de la zone de carte — c'est aussi, à un pixel près, la taille du
// radar natif (128×128, cf. docs/minimap_re.md). En dessous, la flèche du joueur
// mange l'image et plus rien n'est lisible.
constexpr float kMinSide = 128.0f;

// Bornes et pas du zoom : ceux des boutons + / − du radar natif
// (`UIMinimapZoomWnd_OnMsg` commandes 216 et 217). Repris tels quels plutôt
// qu'inventés — le joueur retrouve l'amplitude qu'il connaît.
constexpr float kZoomMin  = 1.0f;
constexpr float kZoomMax  = 4.0f;
constexpr float kZoomStep = 4.0f / 3.0f;

// Fenêtre [0,1] de largeur `span` centrée sur `c`, ramenée dans les bornes SANS
// se rétrécir : c'est ce que fait le natif quand le joueur s'approche d'un bord
// de carte — la vue glisse le long du bord au lieu de se réduire.
void CenteredSpan(float c, float span, float* lo, float* hi) {
  if (span >= 1.0f) { *lo = 0.0f; *hi = 1.0f; return; }
  *lo = c - span * 0.5f;
  *hi = c + span * 0.5f;
  if (*lo < 0.0f) { *lo = 0.0f; *hi = span; }
  if (*hi > 1.0f) { *hi = 1.0f; *lo = 1.0f - span; }
}

// Part de l'écran que la fenêtre ne dépassera jamais. 🔴 Ce n'est pas un
// confort : une fenêtre plus grande que l'écran met sa poignée de
// redimensionnement HORS D'ATTEINTE, et l'utilisateur n'a plus aucun geste pour
// la réduire — l'état est piégeant, et il se persiste.
constexpr float kMaxScreenFrac = 0.9f;

// Ce que la contrainte de taille doit savoir de l'habillage de la fenêtre : les
// marges qu'ImGui ajoute autour du contenu, la hauteur non-carte (la ligne
// « carte,x,y »), et le plafond calculé depuis l'écran.
struct SquareFit {
  float pad_x = 0.0f;    // 2 × WindowPadding.x
  float pad_y = 0.0f;    // 2 × WindowPadding.y
  float extra_h = 0.0f;  // ligne de coordonnées, espacement compris
  float max_side = 0.0f; // côté maximal de la zone de carte
};

// Contrainte de redimensionnement : la ZONE DE CARTE reste carrée.
//
// C'est le carré qu'on garde, pas la fenêtre : celle-ci porte en plus ses marges
// et la ligne de coordonnées, et vouloir une FENÊTRE carrée déformerait la carte
// dès qu'on affiche les coordonnées.
//
// 🔴 On prend la MOYENNE des deux côtés impliqués plutôt que l'un des deux. La
// poignée de coin bouge les deux axes à la fois, donc n'importe lequel suffirait
// — mais ImGui laisse aussi tirer les BORDS, et un axe unique rendrait le bord
// perpendiculaire inerte. La moyenne répond aux deux, à demi-vitesse sur un bord
// seul : un geste qui répond mollement se corrige, un geste mort déroute.
// 🔴 Le plafond est appliqué ICI, pas seulement dans le rectangle de contrainte.
// ImGui borne la taille sur min/max AVANT d'appeler ce callback, puis retient ce
// que le callback rend : un callback qui recalcule ses deux axes ressortirait de
// la borne juste après elle. C'est ce qui a laissé une fenêtre dépasser l'écran,
// poignée comprise.
void SizeCbSquare(ImGuiSizeCallbackData* data) {
  const SquareFit* fit = static_cast<const SquareFit*>(data->UserData);
  const float from_w = data->DesiredSize.x - fit->pad_x;
  const float from_h = data->DesiredSize.y - fit->pad_y - fit->extra_h;
  float side = (from_w + from_h) * 0.5f;
  if (side > fit->max_side) side = fit->max_side;
  if (side < kMinSide) side = kMinSide;
  data->DesiredSize.x = side + fit->pad_x;
  data->DesiredSize.y = side + fit->pad_y + fit->extra_h;
}

inline ImU32 RgbToImU32(int rgb, int alpha = 255) {
  return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, alpha);
}
// ── Détour du dessin natif ───────────────────────────────────────────────────

// Trampoline rendu par le HookManager : l'appel volé, RELOGÉ (`DetourCopyInstruction`
// réécrit le rel32), suivi d'un saut vers l'instruction d'après. On y SAUTE, on
// ne l'appelle pas — il ne revient pas ici mais dans le jeu.
void* g_tramp_draw_minimap = nullptr;

// Le radar natif doit-il disparaître de cette frame ? Appelée depuis le stub,
// donc en plein dessin du jeu : hors frame ImGui, et sans le moindre objet à
// dérouler — c'est ce qui autorise le `__try` de `SafeFindWindow` (C2712).
bool NativeRadarVetoed() {
  if (!g_in_game || !g_cfg.enabled || !g_cfg.replace_native) return false;
  // Et le cadre avec. Le gestionnaire de fenêtres rend les siennes APRÈS ce
  // point : masquer ici retire dans la MÊME frame le texte des coordonnées et
  // les cinq boutons que porte la fenêtre 14 — le reliquat visible que le veto
  // du quad, à lui seul, laissait passer. Sa DESTRUCTION, elle, attend le
  // battement de frame suivant : `CloseWindow` est une commande du client, et
  // on ne l'émet pas au milieu de son rendu.
  uiwnd::SafeSetVisible(uiwnd::SafeFindWindow(uiwnd::kUIMinimapZoomWnd), false);
  return true;
}

__declspec(naked) void DrawMiniMapStub() {
  __asm {
    pushad
    call NativeRadarVetoed
    test al, al
    popad                 // POPAD ne touche pas aux drapeaux : le test tient
    jz   draw
    // ⚠ Adresse en DUR, et pas la constante nommée juste au-dessus : dans l'asm
    // inline MSVC, un identifiant C++ désigne un EMPLACEMENT mémoire —
    // `jmp kDrawMiniMapAfter` sauterait à son CONTENU.
    mov  eax, 0C74FC7h    // = kDrawMiniMapAfter, l'instruction après l'appel
    jmp  eax              // l'appel n'a pas lieu ; eax est volatil pour l'appelant
  draw:
    jmp  [g_tramp_draw_minimap]
  }
}

}  // namespace

// ── Cycle de vie ─────────────────────────────────────────────────────────────

Minimap::Minimap() {
  const uint8_t* site = reinterpret_cast<const uint8_t*>(kDrawMiniMapCall);
  if (memcmp(site, kDrawMiniMapCallBytes, sizeof(kDrawMiniMapCallBytes)) != 0) {
    LogError(
        "Minimap : 0x{:x} ne porte pas l'appel attendu au radar natif — détour "
        "NON posé, le radar natif clignotera à chaque téléport.",
        kDrawMiniMapCall);
    return;
  }
  g_tramp_draw_minimap = hooking::HookManager::Instance().SetHook(
      hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kDrawMiniMapCall),
      reinterpret_cast<uint8_t*>(&DrawMiniMapStub));
}

// ── Événements ───────────────────────────────────────────────────────────────

void Minimap::OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) {
  g_in_game = (mode_type == ModeMgr::ModeType::kGame);
  if (map_name && map_name[0] != '\0')
    strncpy_s(g_map_fallback, sizeof(g_map_fallback), map_name, _TRUNCATE);
  else
    g_map_fallback[0] = '\0';
}

void Minimap::OnGameFramePulse() {
  if (!g_in_game) return;

  // Bascule demandée par le menu. Vidée ICI et pas au clic : `MakeWindow` et
  // `CloseWindow` sont des commandes du client, et les émettre depuis notre
  // frame ImGui fige le jeu sans un mot.
  if (pending_toggle_wnd_ != 0) {
    const int id = pending_toggle_wnd_;
    pending_toggle_wnd_ = 0;
    if (uiwnd::SafeFindWindow(id))
      uiwnd::SafeCloseWindow(id);
    else
      uiwnd::MakeWindow(id);
  }

  // 🔴 FERMER, pas masquer. `GameMode_DrawMiniMap` (0x00c66ab0) est gardée par
  // `if (g_MinimapZoomWnd)` — le POINTEUR de la fenêtre 14, pas son drapeau de
  // visibilité (+0x28). La masquer ferait donc disparaître son cadre et ses cinq
  // boutons tout en laissant le quad de carte se dessiner : le pire des deux.
  // Seule sa destruction retire l'ensemble.
  //
  // Et c'est à rejouer à CHAQUE frame, pas une fois : le client recrée sa
  // fenêtre à chaque entrée de carte, comme il le fait pour le bouton du cash
  // shop.
  //
  // ⚠ Ce rattrapage-ci arrive forcément UNE FRAME APRÈS la recréation — il est
  // en tête de frame, la recréation a lieu au milieu de la précédente. Ce n'est
  // donc PAS lui qui empêche le clignotement au téléport : c'est le veto posé
  // sur le site d'appel du dessin (cf. `kDrawMiniMapCall`), qui masque la
  // fenêtre et saute le radar dans la frame même où elle réapparaît. Ici, on ne
  // fait que la détruire pour de bon, sans urgence.
  const bool want_native_gone = g_cfg.enabled && g_cfg.replace_native;
  void* native = uiwnd::SafeFindWindow(uiwnd::kUIMinimapZoomWnd);
  if (want_native_gone) {
    if (native) {
      uiwnd::SafeCloseWindow(uiwnd::kUIMinimapZoomWnd);
      native_closed_ = true;
    }
  } else if (native_closed_ && !native) {
    uiwnd::MakeWindow(uiwnd::kUIMinimapZoomWnd);
    native_closed_ = false;
  }
}

void Minimap::OnRenderUI() {
  if (!g_cfg.enabled || !g_in_game) return;

  char map[64];
  const bool have_map = CurrentMapName(map, sizeof(map));

  Snapshot snap;
  ReadSnapshot(&snap);

  // La texture est MÉMORISÉE par chemin, échec compris : une carte sans bitmap
  // ne relance pas un chargement à chaque frame. Le cache se vide seul au reset
  // de device, donc rien à surveiller ici.
  ro::GameTexture tex;
  if (have_map) {
    char path[192];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\map\\%s.bmp", ro::uipath::kUiRoot, map);
    tex = ro::CachedTextureFromGameFile(path);
  }

  // La flèche du joueur : le MÊME fichier que le natif. Même mémorisation, donc
  // une carte qui ne l'aurait pas ne relance rien à chaque frame — et le point
  // plein prend le relais.
  char arrow_path[192];
  _snprintf_s(arrow_path, sizeof(arrow_path), _TRUNCATE, "%s\\map\\map_arrow.bmp",
              ro::uipath::kUiRoot);
  const ro::GameTexture arrow = ro::CachedTextureFromGameFile(arrow_path);

  const ImGuiStyle& style = ImGui::GetStyle();
  SquareFit fit;
  fit.pad_x = style.WindowPadding.x * 2.0f;
  fit.pad_y = style.WindowPadding.y * 2.0f;
  fit.extra_h = g_cfg.show_coords ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;

  // ── Nom lisible du lieu ──────────────────────────────────────────────────
  // Résolu une fois par CARTE, pas par frame : la résolution construit une
  // std::string et interroge la DB du client.
  static char s_name_for[64] = {};
  static char s_display[192] = {};
  if (have_map && strncmp(s_name_for, map, sizeof(s_name_for)) != 0) {
    strncpy_s(s_name_for, sizeof(s_name_for), map, _TRUNCATE);
    char local[128];
    if (MapDisplayName(map, local, sizeof(local)))
      strncpy_s(s_display, sizeof(s_display), ro::LocalToUtf8(local), _TRUNCATE);
    else
      s_display[0] = '\0';
  }
  const bool has_display = g_cfg.show_map_name && s_display[0] != '\0';

  // La hauteur du nom entre dans la contrainte AVANT `Begin` : replié sur la
  // largeur de la carte il peut prendre deux lignes ou plus, et la fenêtre doit
  // déjà être assez haute pour les porter — sinon elle rognerait son propre
  // texte. Mesuré sur la largeur que la carte AURA, d'où le pré-calcul du côté.
  float provisional = static_cast<float>(g_cfg.size);
  if (provisional < kMinSide) provisional = kMinSide;
  if (has_display) {
    fit.extra_h += ImGui::CalcTextSize(s_display, nullptr, false, provisional).y +
                   style.ItemSpacing.y;
  }

  // Plafond : la FENÊTRE entière tient dans 90 % de l'écran, sur les deux axes.
  // On raisonne donc sur le côté de la zone de carte une fois l'habillage
  // déduit, pas sur la taille brute — sinon la ligne de coordonnées ferait
  // dépasser la hauteur de sa propre hauteur.
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  const float max_w = screen.x * kMaxScreenFrac - fit.pad_x;
  const float max_h = screen.y * kMaxScreenFrac - fit.pad_y - fit.extra_h;
  fit.max_side = (max_w < max_h) ? max_w : max_h;
  // Sur un écran assez petit pour que 90 % ne tiennent pas le minimum, c'est le
  // minimum qui gagne : mieux vaut déborder un peu que rendre la carte illisible.
  if (fit.max_side < kMinSide) fit.max_side = kMinSide;

  float side = static_cast<float>(g_cfg.size);
  if (side < kMinSide) side = kMinSide;
  if (side > fit.max_side) side = fit.max_side;

  // ── Engagement à la touche Maj ──────────────────────────────────────────
  // 🔴 Le survol NE PEUT PAS passer par `ImGui::IsWindowHovered()` : en mode
  // traverser les clics la fenêtre porte `NoInputs`, et ImGui ne teste alors
  // JAMAIS son rectangle — la question « la souris est-elle dessus ? » ne lui
  // est même pas posée. On la tranche nous-mêmes, sur le rectangle qu'on tient
  // déjà à jour pour la persistance de la position.
  const ImGuiIO& io = ImGui::GetIO();
  const float win_x0 = static_cast<float>(g_cfg.pos_x);
  const float win_y0 = static_cast<float>(g_cfg.pos_y);
  const float win_x1 = win_x0 + side + fit.pad_x;
  const float win_y1 = win_y0 + side + fit.pad_y + fit.extra_h;
  const bool mouse_over = io.MousePos.x >= win_x0 && io.MousePos.x < win_x1 &&
                          io.MousePos.y >= win_y0 && io.MousePos.y < win_y1;

  if (io.KeyShift && mouse_over) {
    g_shift_engaged = true;
  } else if (!ImGui::IsAnyItemActive()) {
    // On ne LÂCHE pas tant qu'un geste est en cours : relâcher Maj au milieu
    // d'un glissement ou d'un redimensionnement remettrait `NoInputs` et
    // couperait le geste net.
    g_shift_engaged = false;
  }

  // Un menu ouvert maintient l'engagement : nos deux popups sont construits
  // sous cette condition, la perdre les refermerait sous le curseur.
  const bool engaged = g_shift_engaged || g_menu_open;
  const bool interactive = !g_cfg.locked || engaged;

  // Le rappel du geste. En mode traverser les clics, RIEN à l'écran ne dit que
  // la minimap redevient saisissable : ni bouton, ni bordure, ni curseur — c'est
  // tout l'intérêt du mode, et c'est aussi ce qui le rend indevinable.
  //
  // Il n'apparaît qu'après un temps de survol, et il ne peut pas gêner : sans
  // Maj la souris traverse, donc la bulle ne recouvre jamais que ce que le
  // joueur regardait déjà. Pousser la bulle dès l'effleurement, en revanche,
  // ferait clignoter un pavé de texte à chaque fois qu'on passe devant la carte
  // pour cliquer derrière.
  //
  // 🔴 `SetTooltip` et pas `Tooltip` du toolkit : celui-ci se gouverne à
  // `IsLastItemHovered()`, et il n'y a ICI aucun item — la fenêtre porte
  // `NoInputs`, c'est notre propre test de rectangle qui tranche le survol.
  static float s_hint_hover = 0.0f;
  if (g_cfg.locked && mouse_over && !engaged) {
    s_hint_hover += io.DeltaTime;
    if (s_hint_hover >= kHintHoverDelay) {
      ImGui::SetTooltip(
          "%s", i18n::Tr("Maintenir Maj pour saisir la minimap\n"
                         "Sans Maj la souris la traverse : ni déplacement, ni "
                         "poignées, ni molette, ni bouton de réglages."));
    }
  } else {
    s_hint_hover = 0.0f;
  }

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoScrollWithMouse |
                           ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoTitleBar;
  if (!interactive)
    flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
             ImGuiWindowFlags_NoResize;

  ImGui::SetNextWindowPos(ImVec2(static_cast<float>(g_cfg.pos_x),
                                 static_cast<float>(g_cfg.pos_y)),
                          interactive ? ImGuiCond_Once : ImGuiCond_Always);
  // La taille n'est imposée qu'à la PREMIÈRE frame ; ensuite c'est la poignée
  // qui commande. La contrainte, elle, est réappliquée à chaque frame — c'est ce
  // qui refait tenir la fenêtre quand on montre ou cache les coordonnées, sans
  // attendre le prochain geste.
  ImGui::SetNextWindowSize(ImVec2(side + fit.pad_x, side + fit.pad_y + fit.extra_h),
                           ImGuiCond_Once);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(kMinSide + fit.pad_x, kMinSide + fit.pad_y + fit.extra_h),
      ImVec2(fit.max_side + fit.pad_x, fit.max_side + fit.pad_y + fit.extra_h),
      SizeCbSquare, &fit);
  ImGui::SetNextWindowBgAlpha(g_cfg.bg_alpha / 100.0f);

  // Les DEUX poignées d'ImGui (bas-droit et bas-gauche) doivent se voir : la
  // couleur du thème est trop discrète par-dessus une image de carte, et une
  // poignée invisible n'existe pas pour le joueur. Sombre plutôt que claire —
  // les cartes de RO sont majoritairement pâles. Poussé pour CETTE fenêtre
  // seulement : le réglage d'ImGui est global, pas ce style-ci.
  const int grip_pushes = 3;
  ImGui::PushStyleColor(ImGuiCol_ResizeGrip, IM_COL32(30, 34, 44, 130));
  ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, IM_COL32(30, 34, 44, 200));
  ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, IM_COL32(30, 34, 44, 255));

  // Nom entièrement masqué (`##`) mais STABLE : il porte l'identité de la
  // fenêtre, pas un libellé. Le faire varier avec la carte lui ferait perdre sa
  // position à chaque déplacement.
  if (ImGui::Begin("##bourgeon_minimap", nullptr, flags)) {
    // On persiste déplacement et redimensionnement une fois le geste posé — la
    // vidange plus bas attend `!IsAnyItemActive()`, et ImGui garde un item actif
    // pendant toute la durée d'un glissement de fenêtre comme d'une poignée.
    if (interactive) {
      const ImVec2 wp = ImGui::GetWindowPos();
      if (static_cast<int>(wp.x) != g_cfg.pos_x ||
          static_cast<int>(wp.y) != g_cfg.pos_y) {
        g_cfg.pos_x = static_cast<int>(wp.x);
        g_cfg.pos_y = static_cast<int>(wp.y);
        g_needs_save = true;
      }
    }

    // 🔴 Le côté effectif vient de la FENÊTRE, pas du réglage : c'est la poignée
    // qui commande, la contrainte a déjà rendu la zone carrée, et la relire ici
    // est ce qui rend le redimensionnement persistant sans second chemin.
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail >= kMinSide && avail <= fit.max_side) side = avail;
    if (static_cast<int>(side) != g_cfg.size) {
      g_cfg.size = static_cast<int>(side);
      g_needs_save = true;
    }

    // Le carré de dessin est réservé d'abord : la carte peut manquer, la
    // fenêtre ne doit pas se rétracter pour autant (elle est déplaçable, et une
    // fenêtre qui change de taille sous le curseur est impossible à saisir).
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(side, side));
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Molette = zoom ───────────────────────────────────────────────────────
    // 🔴 Jamais `io.MouseWheel` en direct : une zone qui prend la molette au
    // survol vole le défilement de ce qui la contient. `RegionWheel` est la
    // variante pour une zone hit-testée à la main (on dessine à l'ImDrawList,
    // il n'y a pas d'« item » ImGui à interroger) ; elle CONSOMME le cran.
    if (interactive) {
      const bool over_map =
          ImGui::IsWindowHovered() &&
          ImGui::IsMouseHoveringRect(origin,
                                     ImVec2(origin.x + side, origin.y + side));
      const float notches = RegionWheel("bourgeon_minimap", over_map);
      if (notches != 0.0f) {
        // Puissance plutôt qu'une boucle de crans : un pavé tactile envoie des
        // fractions, qu'un compte entier arrondirait à zéro — la molette
        // semblerait morte sur ce matériel-là.
        float z = g_cfg.zoom * powf(kZoomStep, notches);
        if (z < kZoomMin) z = kZoomMin;
        if (z > kZoomMax) z = kZoomMax;
        if (z != g_cfg.zoom) {
          g_cfg.zoom = z;
          g_needs_save = true;
        }
      }
    }
    float zoom = g_cfg.zoom;
    if (!(zoom >= kZoomMin)) zoom = kZoomMin;  // attrape aussi un NaN du YAML
    if (zoom > kZoomMax) zoom = kZoomMax;

    if (tex.tex && tex.w > 0 && tex.h > 0) {
      // ── Cadrage ──────────────────────────────────────────────────────────
      // u part de la gauche ; v part du HAUT, et la ligne 0 du bitmap est la
      // cellule de plus grand Y — d'où l'inversion, celle du natif.
      float cu = 0.5f, cv = 0.5f;
      if (snap.ok) {
        cu = snap.cell_x / static_cast<float>(snap.cells_w);
        cv = (static_cast<float>(snap.cells_h) - snap.cell_y) /
             static_cast<float>(snap.cells_h);
        if (cu < 0.0f) cu = 0.0f;
        if (cu > 1.0f) cu = 1.0f;
        if (cv < 0.0f) cv = 0.0f;
        if (cv > 1.0f) cv = 1.0f;
      }
      // Modèle du natif : on regarde une fenêtre de 1/zoom de la carte, centrée
      // sur le joueur. Zoom 1 ⇒ portée 1 ⇒ la carte entière, et le centrage
      // n'a plus d'effet. Sans position connue, on montre tout.
      const float span = snap.ok ? 1.0f / zoom : 1.0f;
      float u0, u1, v0, v1;
      CenteredSpan(cu, span, &u0, &u1);
      CenteredSpan(cv, span, &v0, &v1);

      // On garde le rapport de forme de la PORTION VISIBLE du bitmap, pas du
      // bitmap entier : les deux coïncident tant que les deux portées sont
      // égales, mais elles divergent quand une seule bute sur un bord.
      //
      // ⚠ Ce n'est PAS ce qui empêche les cartes rectangulaires d'être
      // déformées — rien ne les en empêche, et c'est voulu. Mesuré sur les 877
      // bitmaps du client (2026-08-16) : 823 font exactement 512×512, quelle que
      // soit la forme de la carte. Le contenu est donc ÉTIRÉ pour remplir un
      // canevas carré, et c'est cet étirement que `u = cellX/largeur` défait.
      // Le calcul ci-dessous ne sert qu'au cas du zoom en bord de carte.
      const float sub_w = tex.w * (u1 - u0);
      const float sub_h = tex.h * (v1 - v0);
      const float scale = (sub_w >= sub_h) ? side / sub_w : side / sub_h;
      const float draw_w = sub_w * scale;
      const float draw_h = sub_h * scale;
      const ImVec2 p0(origin.x + (side - draw_w) * 0.5f,
                      origin.y + (side - draw_h) * 0.5f);
      const ImVec2 p1(p0.x + draw_w, p0.y + draw_h);

      int map_a = g_cfg.map_alpha;
      if (map_a < 0) map_a = 0;
      if (map_a > 100) map_a = 100;
      const ImU32 tint = IM_COL32(255, 255, 255, (map_a * 255) / 100);

      dl->AddCallback(ImCb_MapFilter, nullptr);
      dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex.tex)),
                   p0, p1, ImVec2(u0, v0), ImVec2(u1, v1), tint);
      if (snap.ok) {
        float half = static_cast<float>(g_cfg.marker_px);
        if (half < 2.0f) half = 2.0f;

        // Cellule -> point à l'écran, DANS la portion visible. Rend false quand
        // la cellule est hors cadrage : au zoom 1 ça n'arrive jamais, mais dès
        // qu'on rapproche, un coéquipier à l'autre bout de la carte sort du
        // champ et ne doit pas être écrasé contre le bord.
        auto CellToScreen = [&](int cx, int cy, ImVec2* at_out) -> bool {
          const float fu = static_cast<float>(cx) / static_cast<float>(snap.cells_w);
          const float fv = (static_cast<float>(snap.cells_h) - static_cast<float>(cy)) /
                           static_cast<float>(snap.cells_h);
          if (fu < u0 || fu > u1 || fv < v0 || fv > v1) return false;
          const float su = (u1 > u0) ? (fu - u0) / (u1 - u0) : 0.5f;
          const float sv = (v1 > v0) ? (fv - v0) / (v1 - v0) : 0.5f;
          *at_out = ImVec2(p0.x + su * draw_w, p0.y + sv * draw_h);
          return true;
        };

        // ── PNJ et commodités de la carte ─────────────────────────────────
        // En premier : c'est le fond d'information, tout le reste passe dessus.
        if (g_cfg.show_town && have_map) {
          TownIcon town[kMaxMarkers];
          const int nt = CollectTownIcons(map, town, kMaxMarkers);
          const float th = (half * 0.9f < 5.0f) ? 5.0f : half * 0.9f;
          const ImVec2 saved_cur = ImGui::GetCursorScreenPos();
          for (int i = 0; i < nt; ++i) {
            ImVec2 at;
            if (!CellToScreen(town[i].cell_x, town[i].cell_y, &at)) continue;
            const ro::GameTexture tt = ro::CachedTextureFromGameFile(town[i].bmp);
            if (!tt.tex) continue;
            dl->AddImage(
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tt.tex)),
                ImVec2(at.x - th, at.y - th), ImVec2(at.x + th, at.y + th));

            // 🔴 Un VRAI item ImGui par icône, pas un `IsMouseHoveringRect` : le
            // corps de la fenêtre est déplaçable, et un simple test de survol
            // laisserait le clic démarrer un glissement en même temps qu'il
            // ouvre l'itinéraire. L'item consomme le clic ; c'est aussi ce que
            // fait le natif, dont les icônes sont de vrais boutons.
            if (interactive) {
              ImGui::SetCursorScreenPos(ImVec2(at.x - th, at.y - th));
              ImGui::PushID(i);
              ImGui::InvisibleButton("##mm_npc", ImVec2(th * 2.0f, th * 2.0f));
              if (ImGui::IsItemHovered()) {
                ro::SetHoverCursor(kRoCursorHand);
                if (town[i].name[0])
                  ImGui::SetTooltip("%s", ro::LocalToUtf8(town[i].name));
              }
              if (ImGui::IsItemClicked())
                navi::RouteFromMinimap(map, town[i].cell_x, town[i].cell_y);
              ImGui::PopID();
            }
          }
          ImGui::SetCursorScreenPos(saved_cur);
        }

        // ── Marqueurs du joueur ───────────────────────────────────────────
        // Clic droit sur la carte pour en poser un : le bouton DROIT ne déplace
        // pas la fenêtre chez ImGui, donc un simple test de survol suffit ici —
        // contrairement aux icônes de PNJ, où le clic gauche entrait en concurrence
        // avec le glissement.
        if (interactive && ImGui::IsWindowHovered() &&
            ImGui::IsMouseHoveringRect(p0, p1) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          const ImVec2 mouse = ImGui::GetIO().MousePos;
          const float su = (draw_w > 0.0f) ? (mouse.x - p0.x) / draw_w : 0.0f;
          const float sv = (draw_h > 0.0f) ? (mouse.y - p0.y) / draw_h : 0.0f;
          // L'inverse EXACT de CellToScreen, cadrage du zoom compris.
          const float fu = u0 + su * (u1 - u0);
          const float fv = v0 + sv * (v1 - v0);
          g_memo_cell_x = static_cast<int>(fu * static_cast<float>(snap.cells_w));
          g_memo_cell_y = static_cast<int>(static_cast<float>(snap.cells_h) -
                                           fv * static_cast<float>(snap.cells_h));
          g_memo_name[0] = '\0';
          ImGui::OpenPopup("##mm_memo_popup");
        }

        std::vector<MinimapMemo>* mine = nullptr;
        if (have_map) {
          auto it = memos_.find(map);
          if (it != memos_.end()) mine = &it->second;
        }
        if (mine && !mine->empty()) {
          // 🔴 Le suffixe d'ÉTAT est obligatoire. Le natif ne nomme jamais ces
          // fichiers en entier : `UIMiniMapWnd_CreateBitmapButton` (0x00895750)
          // reçoit la seule base « minimap\memopoint », y colle le préfixe
          // 유저인터페이스\, puis `_1` / `_2` / `_3` (normal / survol / enfoncé)
          // et enfin `.bmp`. Sans `_1`, le chargement échoue — c'est la même
          // convention que `i_viewon_1.bmp` du bouton de réglages.
          char mp[192];
          _snprintf_s(mp, sizeof(mp), _TRUNCATE, "%s\\minimap\\memopoint_1.bmp",
                      ro::uipath::kUiRoot);
          const ro::GameTexture mt = ro::CachedTextureFromGameFile(mp);
          const float mh = (half * 0.8f < 4.0f) ? 4.0f : half * 0.8f;
          const ImVec2 saved_cur = ImGui::GetCursorScreenPos();
          for (size_t i = 0; i < mine->size(); ++i) {
            const MinimapMemo& memo = (*mine)[i];
            ImVec2 at;
            if (!CellToScreen(memo.x, memo.y, &at)) continue;
            if (mt.tex) {
              dl->AddImage(
                  static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(mt.tex)),
                  ImVec2(at.x - mh, at.y - mh), ImVec2(at.x + mh, at.y + mh));
            } else {
              // L'art du client manque : un losange plutôt que rien, distinct
              // des carrés du groupe et des pastilles de guilde.
              dl->AddQuadFilled(ImVec2(at.x, at.y - mh), ImVec2(at.x + mh, at.y),
                                ImVec2(at.x, at.y + mh), ImVec2(at.x - mh, at.y),
                                IM_COL32(255, 210, 60, 235));
            }
            if (interactive) {
              ImGui::SetCursorScreenPos(ImVec2(at.x - mh, at.y - mh));
              ImGui::PushID(static_cast<int>(i));
              ImGui::InvisibleButton("##mm_memo_pt", ImVec2(mh * 2.0f, mh * 2.0f));
              if (ImGui::IsItemHovered()) {
                ro::SetHoverCursor(kRoCursorHand);
                ImGui::SetTooltip("%s\n%d, %d",
                                  memo.name.empty() ? i18n::Tr("(sans nom)")
                                                    : memo.name.c_str(),
                                  memo.x, memo.y);
              }
              ImGui::PopID();
            }
          }
          ImGui::SetCursorScreenPos(saved_cur);
        }

        // ── Quêtes ────────────────────────────────────────────────────────
        // Le natif compose son bitmap avec le SOUS-type de l'entrée, pas son
        // type : `\basic_interface\quest_%d.bmp`.
        if (g_cfg.show_quests) {
          Marker quests[kMaxMarkers];
          const int nq = CollectTree(rag::ActiveModeSafe(), kGm_QuestMap, TreeKind::kQuest,
                                     quests, kMaxMarkers);
          const float qh = (half * 0.8f < 4.0f) ? 4.0f : half * 0.8f;
          for (int i = 0; i < nq; ++i) {
            ImVec2 at;
            if (!CellToScreen(quests[i].cell_x, quests[i].cell_y, &at)) continue;
            char qp[192];
            _snprintf_s(qp, sizeof(qp), _TRUNCATE,
                        "%s\\basic_interface\\quest_%d.bmp", ro::uipath::kUiRoot,
                        quests[i].variant);
            const ro::GameTexture qt = ro::CachedTextureFromGameFile(qp);
            if (!qt.tex) continue;
            dl->AddImage(
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(qt.tex)),
                ImVec2(at.x - qh, at.y - qh), ImVec2(at.x + qh, at.y + qh));
          }
        }

        // ── Groupe et guilde ──────────────────────────────────────────────
        // Les deux formes du natif : carré plein bordé pour le groupe,
        // pastille cernée pour la guilde. La couleur vient de l'entrée, c'est
        // le serveur qui la choisit — on ne la réinvente pas.
        const float mk = (half * 0.5f < 2.0f) ? 2.0f : half * 0.5f;
        if (g_cfg.show_guild) {
          Marker gm_[kMaxMarkers];
          const int ng = CollectTree(rag::ActiveModeSafe(), kGm_GuildMap, TreeKind::kPosColor,
                                     gm_, kMaxMarkers);
          for (int i = 0; i < ng; ++i) {
            ImVec2 at;
            if (!CellToScreen(gm_[i].cell_x, gm_[i].cell_y, &at)) continue;
            dl->AddCircleFilled(at, mk + 1.0f, IM_COL32(255, 255, 255, 170));
            dl->AddCircleFilled(at, mk, gm_[i].color);
          }
        }
        if (g_cfg.show_party) {
          Marker pm[kMaxMarkers];
          const int np = CollectTree(rag::ActiveModeSafe(), kGm_PartyMap, TreeKind::kPosColor,
                                     pm, kMaxMarkers);
          for (int i = 0; i < np; ++i) {
            ImVec2 at;
            if (!CellToScreen(pm[i].cell_x, pm[i].cell_y, &at)) continue;
            dl->AddRectFilled(ImVec2(at.x - mk - 1.0f, at.y - mk - 1.0f),
                              ImVec2(at.x + mk + 1.0f, at.y + mk + 1.0f),
                              IM_COL32(255, 255, 255, 150));
            dl->AddRectFilled(ImVec2(at.x - mk, at.y - mk),
                              ImVec2(at.x + mk, at.y + mk), pm[i].color);
          }
        }

        // ── Repères du serveur (`viewpoint` → ZC_COMPASS 0x0144) ──────────
        // Croix clignotante, comme le natif : 500 ms visible sur 1000, la phase
        // partant de l'horodatage de pose — deux repères posés à des instants
        // différents clignotent donc en décalé, ce qui les distingue.
        if (g_cfg.show_viewpoints) {
          Marker vps[kMaxMarkers];
          const int nv = CollectTree(rag::ActiveModeSafe(), kGm_ViewpointMap,
                                     TreeKind::kViewpoint, vps, kMaxMarkers);
          const float vh = (half * 0.22f < 1.0f) ? 1.0f : half * 0.22f;
          const unsigned now = GetTickCount();
          for (int i = 0; i < nv; ++i) {
            if ((now - vps[i].tick) % kVpBlinkMs >= kVpBlinkMs / 2) continue;
            ImVec2 at;
            if (!CellToScreen(vps[i].cell_x, vps[i].cell_y, &at)) continue;
            // La croix du natif : une barre horizontale 8×2 et une verticale
            // 2×8, en demi-étendues — d'où le facteur 4 sur un seul axe.
            dl->AddRectFilled(ImVec2(at.x - vh * 4.0f, at.y - vh),
                              ImVec2(at.x + vh * 4.0f, at.y + vh),
                              vps[i].color);
            dl->AddRectFilled(ImVec2(at.x - vh, at.y - vh * 4.0f),
                              ImVec2(at.x + vh, at.y + vh * 4.0f),
                              vps[i].color);
          }
        }

        // ── Boss (Convex Mirror) ──────────────────────────────────────────
        // 🔴 COLLÉ AU BORD, pas omis. C'est tout l'intérêt de l'objet : quand le
        // boss est hors du cadrage, le natif ramène son icône sur le bord pour
        // en donner la DIRECTION. Et il garde son bitmap en le faisant — le
        // remplacement par une flèche de quête ne vaut que pour les autres
        // genres de marqueurs.
        if (g_cfg.show_boss) {
          int bx = 0, by = 0;
          if (ReadBossCell(&bx, &by)) {
            char bmp[192];
            _snprintf_s(bmp, sizeof(bmp), _TRUNCATE, "%s\\map\\bossmonster.bmp",
                        ro::uipath::kUiRoot);
            const ro::GameTexture bt = ro::CachedTextureFromGameFile(bmp);
            if (bt.tex) {
              // Position NON bornée, puis ramenée dans le cadre : c'est ce qui
              // distingue « collé au bord » de « omis ».
              const float fu = static_cast<float>(bx) /
                               static_cast<float>(snap.cells_w);
              const float fv = (static_cast<float>(snap.cells_h) -
                                static_cast<float>(by)) /
                               static_cast<float>(snap.cells_h);
              const float su = (u1 > u0) ? (fu - u0) / (u1 - u0) : 0.5f;
              const float sv = (v1 > v0) ? (fv - v0) / (v1 - v0) : 0.5f;
              const float bh2 = (half * 0.9f < 5.0f) ? 5.0f : half * 0.9f;
              float ax = p0.x + su * draw_w;
              float ay = p0.y + sv * draw_h;
              if (ax < p0.x + bh2) ax = p0.x + bh2;
              if (ax > p1.x - bh2) ax = p1.x - bh2;
              if (ay < p0.y + bh2) ay = p0.y + bh2;
              if (ay > p1.y - bh2) ay = p1.y - bh2;
              dl->AddImage(
                  static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(bt.tex)),
                  ImVec2(ax - bh2, ay - bh2), ImVec2(ax + bh2, ay + bh2));
            }
          }
        }

        // Position du joueur DANS la portion affichée, pas dans la carte.
        const float mu = (u1 > u0) ? (cu - u0) / (u1 - u0) : 0.5f;
        const float mv = (v1 > v0) ? (cv - v0) / (v1 - v0) : 0.5f;
        const ImVec2 at(p0.x + mu * draw_w, p0.y + mv * draw_h);

        // ── L'ITINÉRAIRE : LE TRACÉ EXACT, CELLULE PAR CELLULE ───────────
        //
        // 🔴 Correction d'une affirmation antérieure de ce fichier : le chemin
        // à l'intérieur de la carte EXISTE bien sous forme de liste, et le
        // moteur la tient toute prête. `CNavigation` fait tourner l'A★ de
        // déplacement du client sur la .gat et garde sa sortie — c'est cette
        // suite de cellules que le natif sème au sol en `navi_grid`. Voir
        // `NavigationWindow::RouteCellPath` et docs/navigation_re.md §10.
        //
        // Le natif la projette déjà sur SA minimap, mais quantifiée à un carré
        // de 128 pixels : à ce prix le tracé se ratatine dès qu'on zoome. On
        // repart donc des cellules et on applique NOTRE cadrage.
        //
        // C'est aussi ce qui manquait vraiment au natif : ses traces au sol ne
        // se voient qu'une fois dessus, donc elles aident à SUIVRE un chemin,
        // jamais à le TROUVER. Ici il est visible en entier.
        //
        // Dessiné APRÈS tous les marqueurs et AVANT le joueur : la ligne doit
        // passer par-dessus les icônes de ville pour rester lisible, mais rien
        // ne doit recouvrir la flèche du joueur.
        if (have_map) {
          auto* nav = Bourgeon::Instance().navigation_window();
          if (nav != nullptr && nav->imgui_enabled_) {
            // Ambre, la couleur du guidage dans le reste de Bourgeon. Doublée
            // d'un liséré sombre : une ligne claire seule disparaît sur les
            // cartes de ville, qui le sont aussi.
            const ImU32 kRouteDark = IM_COL32(0x20, 0x18, 0x08, 0xB0);
            const ImU32 kRouteCol  = IM_COL32(0xFF, 0xC8, 0x4A, 0xE0);
            const float eh = (half * 0.5f < 3.0f) ? 3.0f : half * 0.5f;

            // Cellule -> écran SANS test de cadrage, contrairement à
            // `CellToScreen`. Une ligne brisée ne se découpe pas point par
            // point : on la laisse sortir du cadre et c'est la clip rect de la
            // draw list qui la taille, ce qui garde aux segments d'entrée et de
            // sortie leur pente exacte.
            auto CellToScreenRaw = [&](int cx, int cy) -> ImVec2 {
              const float fu = static_cast<float>(cx) /
                               static_cast<float>(snap.cells_w);
              const float fv = (static_cast<float>(snap.cells_h) -
                                static_cast<float>(cy)) /
                               static_cast<float>(snap.cells_h);
              const float su = (u1 > u0) ? (fu - u0) / (u1 - u0) : 0.5f;
              const float sv = (v1 > v0) ? (fv - v0) / (v1 - v0) : 0.5f;
              return ImVec2(p0.x + su * draw_w, p0.y + sv * draw_h);
            };

            // Un point d'arrivée hors cadrage garde ainsi sa DIRECTION au lieu
            // de disparaître — c'est justement au zoom qu'on en a besoin.
            auto ClampToFrame = [&](ImVec2 raw) {
              float ex = raw.x, ey = raw.y;
              if (ex < p0.x + eh) ex = p0.x + eh;
              if (ex > p1.x - eh) ex = p1.x - eh;
              if (ey < p0.y + eh) ey = p0.y + eh;
              if (ey > p1.y - eh) ey = p1.y - eh;
              return ImVec2(ex, ey);
            };
            // L'arrivée : un losange, forme qu'aucun autre marqueur de la
            // minimap n'emploie (les mémos sont des ronds, les viewpoints des
            // croix, les quêtes et le boss des bitmaps).
            auto DrawGoal = [&](ImVec2 c) {
              const ImVec2 diamond[4] = {ImVec2(c.x, c.y - eh), ImVec2(c.x + eh, c.y),
                                         ImVec2(c.x, c.y + eh), ImVec2(c.x - eh, c.y)};
              dl->AddConvexPolyFilled(diamond, 4, kRouteCol);
              dl->AddPolyline(diamond, 4, kRouteDark, ImDrawFlags_Closed, 1.5f);
            };

            // Statiques : la fonction de rendu est déjà profonde et ces deux
            // tableaux pèsent 5 Kio. Sans risque, une frame ImGui étant seule
            // sur son fil.
            static NavigationWindow::PathPoint s_cells[kMaxPathPoints];
            static ImVec2                      s_pts[kMaxPathPoints];
            const size_t np = nav->RouteCellPath(s_cells, kMaxPathPoints);

            if (np >= 2) {
              for (size_t k = 0; k < np; ++k)
                s_pts[k] = CellToScreenRaw(s_cells[k].x, s_cells[k].y);
              dl->PushClipRect(p0, p1, true);
              dl->AddPolyline(s_pts, static_cast<int>(np), kRouteDark,
                              ImDrawFlags_None, 3.0f);
              dl->AddPolyline(s_pts, static_cast<int>(np), kRouteCol,
                              ImDrawFlags_None, 1.5f);
              dl->PopClipRect();
              DrawGoal(ClampToFrame(s_pts[np - 1]));
            } else {
              // Repli : pas de chemin en cellules, mais une étape publiée pour
              // cette carte. Le moteur ne recalcule le chemin local qu'à
              // l'entrée de carte et quand le joueur s'en écarte — entre le clic
              // et le premier pas il n'y a encore rien à tracer, et une
              // DIRECTION vaut mieux que rien.
              int exit_x = 0, exit_y = 0;
              if (nav->RouteExitOnMap(map, &exit_x, &exit_y)) {
                const ImVec2 goal = ClampToFrame(CellToScreenRaw(exit_x, exit_y));
                dl->AddLine(at, goal, kRouteDark, 3.0f);
                dl->AddLine(at, goal, kRouteCol, 1.5f);
                DrawGoal(goal);
              }
            }
          }
        }

        // ── Le joueur, EN DERNIER : rien ne doit le recouvrir ──────────────
        // Nom distinct de la teinte de la carte juste au-dessus : deux `tint`
        // imbriqués passeraient en avertissement de masquage.
        const ImU32 marker_col = RgbToImU32(g_cfg.marker_tint);

        if (arrow.tex) {
          const float deg = 180.0f - snap.angle;
          const ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 0.0f);
          const ImVec2 uv2(1.0f, 1.0f), uv3(0.0f, 1.0f);
          ImVec2 q[4];

          // Ombre portée, telle que la pose le natif : son rectangle est élargi
          // de 2 à gauche et en haut, de 2 et 4 à droite et en bas — soit un
          // demi-quad plus grand de (2, 3) et un centre descendu d'un pixel.
          // C'est elle qui rend la flèche lisible sur une carte claire.
          RotatedQuad(at.x, at.y + 1.0f, half + 2.0f, half + 3.0f, deg, q);
          dl->AddImageQuad(
              static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(arrow.tex)),
              q[0], q[1], q[2], q[3], uv0, uv1, uv2, uv3,
              IM_COL32(0x33, 0x33, 0x33, 0x77));

          RotatedQuad(at.x, at.y, half, half, deg, q);
          dl->AddImageQuad(
              static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(arrow.tex)),
              q[0], q[1], q[2], q[3], uv0, uv1, uv2, uv3, marker_col);
        } else {
          // Repli si le bitmap manque : un point cerné de sombre, lisible sur
          // fond clair comme sur fond foncé. Pas de bascule de filtre à poser —
          // un aplat n'échantillonne qu'un texel blanc.
          dl->AddCircleFilled(at, half + 1.0f, IM_COL32(0, 0, 0, 200));
          dl->AddCircleFilled(at, half, marker_col);
        }
      }

      dl->AddCallback(ImCb_RestorePoint, nullptr);
      dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 160));
    } else {
      // Deux cas distincts, pas un gabarit commun : « carte inconnue » n'a pas
      // de `%s` à remplir, et lui passer `map` quand même serait un format qui
      // ment sur ses arguments.
      char line[128];
      if (have_map)
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%s\n%s",
                    i18n::Tr("Pas d'image pour"), map);
      else
        strncpy_s(line, sizeof(line), i18n::Tr("Carte inconnue"), _TRUNCATE);
      const ImVec2 sz = ImGui::CalcTextSize(line);
      dl->AddText(ImVec2(origin.x + (side - sz.x) * 0.5f,
                         origin.y + (side - sz.y) * 0.5f),
                  ImGui::GetColorU32(ImGuiCol_TextDisabled), line);
    }

    // « carte,x,y » — la forme sous laquelle un joueur de RO lit et annonce une
    // position, et le seul endroit où le nom de la carte apparaît maintenant
    // qu'il n'y a plus de barre de titre.
    // ── Bouton de réglages, art emprunté au radar natif ──────────────────────
    // 🔴 Coin HAUT-gauche, et les deux coins du bas sont laissés libres : ImGui
    // y place SES DEUX poignées de redimensionnement (bas-droit puis bas-gauche,
    // dès que `io.ConfigWindowsResizeFromEdges` est vrai — c'est son défaut). Le
    // bouton était au bas-gauche, il couvrait la seconde poignée et la rendait
    // inutilisable ; leur position n'étant pas réglable côté ImGui, c'est au
    // bouton de céder.
    // Visible dès que la fenêtre est CLIQUABLE — donc en permanence en mode
    // déverrouillé, où elle l'est déjà. Le maintien de Maj ne le fait apparaître
    // que dans l'autre mode, celui où la souris traverse : là, un bouton visible
    // en permanence serait mort au clic.
    if (interactive) {
      char bp[192];
      _snprintf_s(bp, sizeof(bp), _TRUNCATE, "%s\\minimap\\i_%s_1.bmp", ro::uipath::kUiRoot,
                  kCfgButtonName);
      const ro::GameTexture b_norm = ro::CachedTextureFromGameFile(bp);
      if (b_norm.tex && b_norm.w > 0 && b_norm.h > 0) {
        const float bw = static_cast<float>(b_norm.w);
        const float bh = static_cast<float>(b_norm.h);
        const ImVec2 bpos(origin.x + 2.0f, origin.y + 2.0f);

        const ImVec2 saved = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(bpos);
        ImGui::InvisibleButton("##mm_cfg", ImVec2(bw, bh));
        const bool b_hover = ImGui::IsItemHovered();
        const bool b_down = ImGui::IsItemActive();
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("##mm_cfg_popup");
        if (b_hover) {
          ro::SetHoverCursor(kRoCursorHand);
          ImGui::SetTooltip("%s", i18n::Tr("Réglages de la minimap"));
        }
        ImGui::SetCursorScreenPos(saved);

        // État survolé / enfoncé : les mêmes trois images que le natif. Si l'un
        // des deux manque, on retombe sur l'image normale plutôt que sur un trou.
        const ro::GameTexture* face = &b_norm;
        ro::GameTexture alt;
        if (b_down || b_hover) {
          _snprintf_s(bp, sizeof(bp), _TRUNCATE, "%s\\minimap\\i_%s_%d.bmp",
                      ro::uipath::kUiRoot, kCfgButtonName, b_down ? 3 : 2);
          alt = ro::CachedTextureFromGameFile(bp);
          if (alt.tex) face = &alt;
        }
        dl->AddImage(
            static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(face->tex)),
            bpos, ImVec2(bpos.x + bw, bpos.y + bh));
      }

      // Le menu porte le MÊME contenu que la section du panneau Moonlight —
      // appelé, pas recopié : deux listes de réglages divergent au premier ajout.
      //
      // 🔴 Et il doit porter les MÊMES COULEURS, sans quoi le contenu commun ne
      // se rend pas pareil. `ro::RoButton` peint son libellé avec `ImGuiCol_Text`
      // AMBIANT : sur un popup ImGui d'origine, ce blanc tombait sur l'art clair
      // du bouton et « Réinitialiser » devenait illisible. On repose donc le
      // corps RO — fond clair, texte sombre — exactement comme `RoBeginCombo`.
      // Les couleurs sont posées AVANT `BeginPopup` et retirées qu'il s'ouvre ou
      // non, sinon la pile de styles se déséquilibre les frames où il est fermé.
    }

    // ── Les deux menus ───────────────────────────────────────────────────────
    // Construits sous `interactive`, PAS sous `engaged` : en mode déverrouillé
    // le clic droit pose un marqueur sans qu'on ait à maintenir Maj, et un menu
    // déjà ouvert doit survivre au relâchement de la touche. C'est aussi eux qui
    // entretiennent `g_menu_open`, lequel prolonge l'engagement.
    bool menu_open_now = false;
    if (interactive) {
      // ── Menu des marqueurs (clic droit sur la carte) ────────────────────
      PushRoPopupColors();
      if (ImGui::BeginPopup("##mm_memo_popup")) {
        menu_open_now = true;
        ImGui::Text("%s %d, %d", i18n::Tr("Marqueur en"), g_memo_cell_x,
                    g_memo_cell_y);

        std::vector<MinimapMemo>* list = nullptr;
        if (have_map) {
          auto it = memos_.find(map);
          if (it != memos_.end()) list = &it->second;
        }
        const int used = list ? static_cast<int>(list->size()) : 0;
        const bool full = used >= kMaxMemosPerMap;

        ImGui::SetNextItemWidth(180.0f);
        // Entrée = valider : poser un marqueur est un geste rapide, obliger à
        // viser un bouton après avoir tapé le nom le rendrait pénible.
        const bool entered = ImGui::InputText(
            "##mm_memo_name", g_memo_name, sizeof(g_memo_name),
            ImGuiInputTextFlags_EnterReturnsTrue);
        SameLine();
        ImGui::BeginDisabled(full || !have_map);
        const bool add = ro::RoButton(i18n::Tr("Ajouter"));
        ImGui::EndDisabled();
        if ((add || entered) && !full && have_map && g_memo_name[0]) {
          MinimapMemo memo;
          memo.x = g_memo_cell_x;
          memo.y = g_memo_cell_y;
          memo.name = g_memo_name;
          memos_[map].push_back(memo);
          memos_dirty_ = true;
          g_memo_name[0] = '\0';
          ImGui::CloseCurrentPopup();
        }
        if (full)
          ImGui::TextDisabled("%s", i18n::Tr("Maximum atteint pour cette carte."));

        if (list && !list->empty()) {
          ImGui::Separator();
          for (size_t i = 0; i < list->size();) {
            ImGui::PushID(static_cast<int>(i));
            const bool del = ro::RoSmallButton("X");
            SameLine();
            ImGui::Text("%d, %d  %s", (*list)[i].x, (*list)[i].y,
                        (*list)[i].name.c_str());
            ImGui::PopID();
            if (del) {
              list->erase(list->begin() + static_cast<long>(i));
              memos_dirty_ = true;
            } else {
              ++i;
            }
          }
          // Une carte vidée de ses marqueurs ne doit pas laisser d'entrée
          // derrière elle : le yaml resterait constellé de listes vides.
          if (list->empty()) memos_.erase(map);
        }
        ImGui::EndPopup();
      }
      PopRoPopupColors();

      PushRoPopupColors();
      if (ImGui::BeginPopup("##mm_cfg_popup")) {
        menu_open_now = true;

        // « Général » : les écrans natifs qu'on n'a pas encore repris. Ils se
        // BASCULENT, comme le fait le bouton `viewon` du radar d'origine —
        // recliquer referme, plutôt que d'empiler des ouvertures.
        if (ImGui::MenuItem(i18n::Tr("Carte du monde")))
          pending_toggle_wnd_ = uiwnd::kUIRoMapWnd;
        if (ImGui::MenuItem(i18n::Tr("Navigation")))
          pending_toggle_wnd_ = uiwnd::kUINavigationV4Wnd;

        ImGui::Separator();
        // Le sous-menu est contraint en largeur : `DrawSettings` porte des
        // curseurs et un sélecteur de couleur, qu'un menu en taille automatique
        // étirerait sur la moitié de l'écran.
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, 0.0f),
                                            ImVec2(440.0f, screen.y * 0.8f));
        if (ImGui::BeginMenu(i18n::Tr("Réglages"))) {
          DrawSettings();
          ImGui::EndMenu();
        }
        ImGui::EndPopup();
      }
      PopRoPopupColors();
    }
    g_menu_open = menu_open_now;

    // Le nom lisible, REPLIÉ sur la largeur de la carte : un lieu au nom long
    // passe à la ligne au lieu d'être coupé, et reste lisible quelle que soit la
    // taille choisie pour la minimap. Sa hauteur est déjà entrée dans la
    // contrainte de fenêtre, plus haut — sans quoi la fenêtre rognerait son
    // propre texte.
    if (has_display) {
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + side);
      ImGui::TextUnformatted(s_display);
      ImGui::PopTextWrapPos();
    }

    if (g_cfg.show_coords) {
      if (have_map && snap.ok)
        ImGui::Text("%s,%d,%d", map, static_cast<int>(snap.cell_x),
                    static_cast<int>(snap.cell_y));
      else if (have_map)
        ImGui::TextDisabled("%s", map);
      else
        ImGui::TextDisabled("-");
    }
  }
  ImGui::End();
  ImGui::PopStyleColor(grip_pushes);

  if ((g_needs_save || memos_dirty_) && !ImGui::IsAnyItemActive()) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    g_needs_save = false;
    memos_dirty_ = false;
  }
}

// ── Réglages ─────────────────────────────────────────────────────────────────

void Minimap::DrawSettings() {
  // Plus de case d'activation : la minimap suit le groupe « Interface moderne »
  // depuis le 2026-08-18 (SetModernInterface écrit g_cfg.enabled — le tester
  // revient à tester le groupe). Les réglages fins restent, grisés hors groupe.
  ImGui::TextDisabled(
      "%s", i18n::Tr("Suivent l'interface moderne — l'interrupteur est en tête "
                     "d'« Interface de jeu »."));

  ImGui::BeginDisabled(!g_cfg.enabled);

  g_needs_save |= ro::RoCheckbox(i18n::Tr("Remplacer le radar d'origine"),
                                 &g_cfg.replace_native);
  SameLine();
  HelpMarker(i18n::Tr(
      "Coché : le radar du client est retiré, celui-ci le remplace.\n"
      "Décoché : les deux coexistent, de quoi les comparer.\n\n"
      "Retirer le radar retire aussi ses cinq boutons — la grande carte et la\n"
      "carte du monde restent accessibles par les icônes du menu."));

  g_needs_save |= ro::RoCheckbox(i18n::Tr("Traverser les clics"), &g_cfg.locked);
  SameLine();
  HelpMarker(i18n::Tr(
      "Décoché : la carte est cliquable — la glisser la déplace, les poignées\n"
      "des deux coins du bas la redimensionnent en gardant son carré, la\n"
      "molette zoome, et le bouton de réglages reste affiché.\n\n"
      "Coché : la souris passe au travers, la carte n'est plus qu'un décor.\n"
      "Maintenir Maj au-dessus la réveille alors le temps du geste, bouton\n"
      "de réglages compris."));

  g_needs_save |= WheelSliderInt(i18n::Tr("Opacité du fond"), &g_cfg.bg_alpha, 0, 100, "%d%%");
  g_needs_save |= WheelSliderInt(i18n::Tr("Opacité de la carte"), &g_cfg.map_alpha, 10, 100, "%d%%");
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Lisser l'image"), &g_cfg.smooth);
  SameLine();
  HelpMarker(i18n::Tr("La carte est un petit bitmap très agrandi : lissée elle "
                      "est plus douce, nette elle garde ses pixels."));

  SeparatorText(i18n::Tr("Personnage"));
  g_needs_save |= WheelSliderInt(i18n::Tr("Taille de la flèche"), &g_cfg.marker_px,
                                 2, 24, "%d px");
  // 🔴 QUATRE flottants, pas trois : ColorEdit4WithAlphaBar appelle
  // ImGui::ColorEdit4, qui LIT la composante alpha même si on n'en fait rien.
  float mc[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  ro::RgbToF3(g_cfg.marker_tint, mc);
  if (ColorEdit4WithAlphaBar(i18n::Tr("Teinte de la flèche"), mc)) {
    g_cfg.marker_tint = ro::F3ToRgb(mc);
    g_needs_save = true;
  }
  SameLine();
  HelpMarker(i18n::Tr("Blanc = la flèche du client telle quelle.\n"
                      "Toute autre couleur la teinte."));
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Afficher « carte,x,y »"), &g_cfg.show_coords);
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Afficher le nom détaillé du lieu"),
                                 &g_cfg.show_map_name);
  SameLine();
  HelpMarker(i18n::Tr(
      "Le nom lisible que la grande carte du client met en titre —\n"
      "« PvP : Room Copass » pour « pvp_n_3-5 ».\n"
      "Plus long que la minimap, il passe à la ligne."));

  SeparatorText(i18n::Tr("Marqueurs"));
  g_needs_save |= ro::RoCheckbox(i18n::Tr("PNJ et commodités"), &g_cfg.show_town);
  SameLine();
  HelpMarker(i18n::Tr("Kafra, guides, marchands, forge, auberge, salon de coiffure,\n"
                      "portails — la liste que le client charge pour cette carte."));
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Membres du groupe"), &g_cfg.show_party);
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Membres de la guilde"), &g_cfg.show_guild);
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Quêtes"), &g_cfg.show_quests);
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Boss (Convex Mirror)"), &g_cfg.show_boss);
  SameLine();
  HelpMarker(i18n::Tr("Le boss révélé par un Convex Mirror.\n"
                      "Hors du cadrage, l'icône se colle au bord de la carte\n"
                      "pour en donner la direction ; elle disparaît tant que le\n"
                      "boss est mort (le client n'a plus de position à donner)."));
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Repères du serveur"),
                                 &g_cfg.show_viewpoints);
  SameLine();
  HelpMarker(i18n::Tr("Les croix clignotantes que posent les NPC pour indiquer\n"
                      "un lieu (commande de script « viewpoint »)."));
  SameLine();
  HelpMarker(i18n::Tr(
      "Les positions et les couleurs viennent du client, comme pour son propre\n"
      "radar : un marqueur n'apparaît que si le serveur a envoyé la position."));

  if (ro::RoButton(i18n::Tr("Réinitialiser"))) {
    g_cfg = MinimapConfig{};
    g_cfg.enabled = true;  // on la garde allumée après une remise à zéro
    g_needs_save = true;
  }

  ImGui::EndDisabled();
}

MinimapConfig& Minimap::config() { return g_cfg; }
