#include "ragnarok/globals.h"
#include "features/windows/navigation_window.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "features/windows/chat_window.h"  // AppendNaviLink (partage <NAVIL>)
#include "features/windows/monster_info_window.h"  // fiche du monstre visé
#include "imgui.h"
#include "ragnarok/uiwnd.h"  // détruire les natives qu'on remplace
#include "ui/game_texture.h"  // miniatures de carte (mêmes bitmaps que le radar)
#include "ui/mob_sprite.h"  // sprite du monstre par son id de classe
#include "ui/ro_imgui.h"  // skin RO (BeginRoWindow / RoButton / RoCheckbox)
#include "utils/i18n.h"

// (Pas de `using namespace mui;` ici : ce panneau n'emploie que les primitives
// de `ro::` et l'API ImGui brute, et `ui/ro_widgets.h` n'est pas inclus.)

// ── Constantes RE (client 20250716, base 0x400000) ───────────────────────────
// Détail complet et mesures live : docs/navigation_re.md. On ne recopie ici que
// ce qui sert, avec la référence de section en regard.
namespace {

// L'objet moteur. STATIQUE : ce n'est pas un pointeur VERS un objet, c'est
// l'objet lui-même — d'où l'absence de déréférencement partout ci-dessous.
constexpr uintptr_t kNavigation = 0x015C3090;

// ── Champs de CNavigation (§3.1) ─────────────────────────────────────────────
// 🔴 Tout ce que le décompilé appelle `byte_15C42xx` / `dword_15C43xx` est un
// champ de CET objet, pas un global : `0x015C4348 - 0x015C3090 = 0x12B8`. C'est
// pourquoi il n'y a qu'une seule adresse à connaître ici.
constexpr int kOffMapsBegin   = 0x0000;  // std::vector<CNaviNode*> des cartes
constexpr int kOffMapsEnd     = 0x0004;
// 🔴 La CIBLE, rangée à part des étapes. `SelectResult` écrit ici le bloc
// `this[1087..1091]` : type, nœud, x, y, puis le NOM de la carte visée en
// `0x110C`. Il le faut, parce que les « étapes » énumérées par le moteur sont
// les LIENS du chemin — chacun nommé par la carte dont il part. La carte
// d'arrivée n'étant la source d'aucun lien, elle manque à l'appel : l'itinéraire
// s'affichait « … > pay_fild07 » sans jamais nommer `pay_fild10`.
constexpr int kOffTargetName  = 0x110C;  // std::string du nom de carte visé
constexpr int kOffRouteState  = 0x1258;  // 2 = itinéraire en cours
constexpr int kOffFollowState = 0x125C;  // 0 arrêté · 1 en route · 2 en attente
constexpr int kOffFilter      = 0x1260;  // 0 tout · 1 carte · 2 NPC · 3 monstre
constexpr int kOffTerm        = 0x1264;  // std::string du terme cherché
constexpr int kOffOptService  = 0x127C;  // les trois options d'itinéraire (§4.3)
constexpr int kOffOptAirship  = 0x127D;
constexpr int kOffOptScroll   = 0x127E;
constexpr int kOffGroupsBegin = 0x12B8;  // std::vector<std::vector<int>>
constexpr int kOffGroupsEnd   = 0x12BC;
constexpr int kOffSelGroup    = 0x12C4;  // index de groupe sélectionné (-1 = aucun)
constexpr int kOffSelMember   = 0x12C8;  // index du membre dans le groupe

// ── Fonctions natives (§3.3) ─────────────────────────────────────────────────
constexpr uintptr_t kFnSearch        = 0x00B34970;  // CNavigation::Search
constexpr uintptr_t kFnGetResult     = 0x00B2E700;  // (out40, index)
constexpr uintptr_t kFnSearchRoute   = 0x00B314F0;  // l'API publique « va là »
constexpr uintptr_t kFnSelectResult  = 0x00B35F80;  // pose (ou EFFACE) la cible
constexpr uintptr_t kFnStepCount     = 0x00B39660;
constexpr uintptr_t kFnGetStep       = 0x00B2EE00;
constexpr uintptr_t kFnNodeName      = 0x00B26CF0;  // CNaviNode::GetName (interne)
constexpr uintptr_t kFnShareToChat   = 0x005AB550;  // pose la balise dans le chat
constexpr uintptr_t kFnSetFocusedWnd = 0x00A4B760;  // __thiscall(mgr, fenetre)
constexpr uintptr_t kUiWindowMgr     = 0x0131F4E8;
constexpr uintptr_t kNewChatWndPtr   = 0x0131F6B0;  // g_pNewChatWnd
constexpr int       kChatInputChild  = 47;          // +0xBC : la barre de saisie
constexpr uintptr_t kFnMapDisplayName = 0x00B26B00;  // nom AFFICHÉ d'une carte
constexpr uintptr_t kStdStringAssign   = 0x004F1940;  // __thiscall(this, src, len)
constexpr uintptr_t kStdStringDtor     = 0x004F08F0;  // __thiscall(this)
constexpr uintptr_t kStdStringCtorCStr = 0x004E5330;  // __thiscall(this, cstr)

// ── Le CONTENU d'une carte : ses PNJ et ses spawns ──────────────────────────
//
// Un `CNaviNode` porte trois vecteurs consécutifs (ctor 0x00B24AC0). Leur rôle
// se lit dans `sub_B26E50` (0x00B26E50) — la fonction qui verse le contenu d'une
// carte dans les résultats — laquelle les parcourt sous le MÊME code de filtre
// que nos pastilles :
//
//   +0x44 / +0x48  vecteur des PNJ      (filtre 2)
//   +0x50 / +0x54  vecteur des SPAWNS   (filtre 3)
//   +0x5C / +0x60  le troisième, non employé ici
//
// 🔴 On LIT ces vecteurs, on n'appelle pas `sub_B26E50` : elle écrirait dans le
// vecteur de résultats du moteur, que la fenêtre native et notre miroir se
// partagent. Lire ne peut rien casser, et nos groupes sont construits de toute
// façon — c'est le même travail sans le risque.
//
// Le nœud lui-même vient de `CNavigation::FindMapByName`, qui balaie le vecteur
// de cartes en comparant les noms.
constexpr int       kOffNodeNpcsBegin  = 0x44;
constexpr int       kOffNodeMobsBegin  = 0x50;
constexpr uintptr_t kFnFindMapByName   = 0x00B20CE0;  // __thiscall(nav, name)

// ── L'icône de TRACE du guidage (la fenêtre native 306) ─────────────────────
//
// Le guidage sème au sol une texture animée le long du chemin, et le client en
// offre HUIT jeux au choix. Toute la mécanique tient en trois éléments, mesurés
// sur le handler `msg 40` de la 203 (`0x005AB450`, qui borne son argument par
// `n - 1 <= 7` — d'où 1..8) :
//
//   · `+0x1344` porte le NUMÉRO choisi. Le setter (0x00B34AE0) ne fait QUE
//     l'écrire — un `this[1233] = n` et rien d'autre ;
//   · `0x00B37D80` reconstruit ensuite les HUIT chemins d'animation, dans huit
//     `std::string` consécutives à `+0x1180` (24 octets chacune) : le gabarit
//     est `navi_grid%d_%d.tga`, rempli avec (numéro choisi, image 0..7). C'est
//     lui qui fait apparaître le changement au sol ;
//   · les vignettes du sélecteur sont
//     `navigation_interface3\btn_roadIocn_select%d_normal.bmp` — noter la
//     coquille du client, « Iocn » et non « Icon », qu'il faut recopier telle
//     quelle sous peine de ne rien trouver.
//
// 🔴 LES DEUX APPELS VONT ENSEMBLE. Écrire `+0x1344` sans rejouer 0x00B37D80
// change le numéro affiché et RIEN au sol : les chemins déjà construits restent
// ceux de l'ancienne icône. C'est le piège de ce champ.
//
// ⚠ Le natif ne PERSISTE ce choix nulle part : ses deux fonctions n'ont qu'un
// appelant chacune, le handler de la fenêtre 306, et aucun n'écrit de fichier.
// Le joueur le repose donc à chaque session — un défaut qu'on corrige en le
// rangeant dans nos réglages. (Vérifié depuis : `+0x1344` n'a que QUATRE sites,
// le constructeur, ce setter, la reconstruction des chemins et une écriture de
// registre — laquelle n'est appelée de nulle part.)
constexpr int       kOffRouteIcon        = 0x1344;
constexpr uintptr_t kFnSetRouteIcon      = 0x00B34AE0;  // __thiscall(nav, n)
constexpr uintptr_t kFnRebuildRoutePaths = 0x00B37D80;  // __thiscall(nav)
constexpr int       kRouteIconCount      = 8;

// ── LE TRACÉ EXACT, CELLULE PAR CELLULE (§10 de la doc) ───────────────────
//
// 🔴 Ce qu'on croyait absent est ici. `CNavigation_BuildCellPath` (0x00B2FC30)
// lance l'A★ de déplacement du client (`Pathfind_AStarSearch`, 0x00A777B0) sur
// la .gat de la carte COURANTE et range sa sortie dans un `std::vector` du
// moteur. Le renderer au sol (0x00B31C40, appelé chaque frame par la scène 3D et
// non par une fenêtre) ne fait que relire ce vecteur.
//
// Élément de 16 octets, mesuré sur `Pathfind_ReconstructPath` (0x00A77660) :
//   +0  int x       cellule
//   +4  int y
//   +8  int dir     0..7 · 0=+Y 1=-X+Y 2=-X 3=-X-Y 4=-Y 5=+X-Y 6=+X 7=+X+Y
//   +12 int t_ms    temps de marche cumulé (inexploitable ici : le guidage
//                   passe une vitesse de 1, pas celle du personnage)
// La reconstruction remonte la chaîne des parents depuis l'ARRIVÉE en écrivant
// à l'envers : la suite est donc ordonnée DÉPART → ARRIVÉE.
//
// 🔴 `dir` est la direction qui MÈNE À cette cellule. Le natif pose donc en `k`
// la flèche de `k+1` — sans quoi la dernière case pointerait dans le vide.
//
// Un TROISIÈME vecteur existe à +0x1294, les mêmes cellules déjà projetées en
// pixels d'un carré 128 × 128 : c'est la preuve que le natif dessine bien
// l'itinéraire sur SA minimap. On ne s'en sert pas — quantifié à 128 pixels, il
// s'effondre dès qu'on zoome, alors que les cellules restent exactes.
constexpr int kOffCellPathBegin = 0x1164;  // vector<PathCell 16o> : begin
constexpr int kOffCellPathEnd   = 0x1168;  // … end
constexpr int kOffTrailActive   = 0x117C;  // bool : la trace au sol est posée
constexpr size_t kPathCellSize   = 16;
constexpr size_t kPathCellOffDir = 8;
// Plafond de sécurité sur le nombre de cellules PARCOURUES. Une diagonale de la
// plus grande carte du jeu en fait moins de mille ; au-delà, c'est que le
// vecteur a été lu pendant une réallocation et non qu'il est long.
constexpr size_t kPathCellMax    = 4096;

// L'ARRÊT du guidage, tel que le natif le fait (bouton d'annulation de la 203,
// 0x005AA21F) : deux drapeaux à zéro puis `CNavigation::ClearRoute`.
// ⚠ Le natif passe `full = 1`, ce qui détruit AUSSI le vecteur de résultats de
// recherche (+0x10F0) — sa listbox se recharge, la nôtre non. On passe donc 0 :
// l'itinéraire (cartes, cellules, points minimap), le drapeau de trace et le
// rendu au sol sont nettoyés, la recherche du joueur reste à l'écran.
constexpr int       kOffGuideFlag = 0x1254;  // lu par la carte du monde
constexpr uintptr_t kFnClearRoute = 0x00B2F080;  // __thiscall(nav, bool full)

// ── Les quatre fenêtres natives que ce panneau remplace (§2 de la doc) ──────
// La principale et ses trois satellites. Le natif éclate la tâche sur les
// quatre, dont deux ne suivent même pas la principale quand on la déplace —
// elles lisent sa position À LA CRÉATION et ne sont jamais repositionnées.
constexpr int kNativeWndMain  = 203;  // UINavigationV4Wnd
constexpr int kNativeWndHelp  = 229;  // UINavigationHelpWnd
constexpr int kNativeWndIcon  = 306;  // UINavigationroadiconWnd
constexpr int kNativeWndRoute = 314;  // UINavigationRuideWnd

// Slots virtuels d'un CNavi_Object (le NPC ou le monstre d'un résultat), tels
// que les emploient Navi_FormatResultLabel et Navi_FormatMemberLabel :
constexpr int kVtDisplayName = 5;  // +0x14 : son nom affiché
constexpr int kVtObjectPos   = 7;  // +0x1C : sa cellule {x, y} (CNavi_Object_GetPos)
constexpr int kVtMapNode     = 8;  // +0x20 : le NŒUD CARTE qui le porte

// ── Champs d'un CNavi_Object (ctor 0x00B24BF0, 0x7C octets) ──────────────────
// 🔴 Les deux derniers CHANGENT DE SENS selon le type, exactement comme les deux
// dernières colonnes du `.lub` : pour un NPC ce sont ses coordonnées, pour un
// monstre son niveau et ses statistiques empaquetées.
constexpr int kObjSubtype  = 0x08;  // 101/102 (NPC) · 300/301 (spawn)
constexpr int kObjPacked   = 0x0C;  // bas 16 = sprite · haut 16 = quantité
constexpr int kObjLevelOrX = 0x44;
constexpr int kObjStatsOrY = 0x48;

// Sous-types, tels que les écrivent `write_npc` / `write_spawn` du serveur.
// Sentinelle du 4ᵉ champ de `Navi_Npc` : ce NPC est un PORTAIL de warp, pas un
// personnage. Écrite par `write_npc` (`vd.look[LOOK_BASE] == JT_WARPNPC`), et
// testée telle quelle par `queryNavi_NpcInfo`. ⚠ Elle dépasse 16 bits — d'où
// l'interdiction de masquer ce champ pour un NPC.
constexpr int kWarpPortalSprite = 99999;

constexpr int kSubShop = 102;  // boutique (NPCTYPE_SHOP / CASHSHOP)
constexpr int kSubMvp  = 301;  // `mexp != 0` côté serveur — c'est un MVP

// Types de résultat, tels que le moteur les range dans le champ `type` (§3.5).
// 🔴 `0` ET `1` sont tous deux des CARTES — c'est ce que fait
// Navi_FormatResultLabel, qui traite `type <= 1` par le nom de nœud carte. Le
// `1` a d'abord été pris pour un séparateur : l'exclure vidait les groupes de
// leur en-tête, puisque c'est LUI qui porte la carte à laquelle les NPC et les
// monstres du groupe sont rattachés.
constexpr int kTypeMapPoint = 0;  // un point précis sur une carte
constexpr int kTypeMap      = 1;  // la carte elle-même (en-tête de groupe)
constexpr int kTypeNpc      = 2;
constexpr int kTypeMob      = 3;

// Valeurs de la barre de filtres. `kShowAll` vaut -1 et non 0 : « Tout » et
// « cartes » ne peuvent pas partager une valeur, sinon les deux pastilles
// s'allument ensemble — c'était le cas quand « cartes » valait `kTypeMapPoint`.
constexpr int kShowAll  = -1;
constexpr int kShowMaps = 0;   // regroupe les types 0 et 1
constexpr int kShowNpc  = kTypeNpc;
constexpr int kShowMob  = kTypeMob;

// Sentinelle « la carte entière » : le moteur la pose à la place des coordonnées
// quand la cible est la carte elle-même et non un point précis.
constexpr int kWholeMap = -1000;

// ── Le masque des trois options d'itinéraire (§4.3), TOUJOURS au complet ──────
// bit 0 = services Kafra · bit 1 = avion · bit 2 = scrolls.
// 🔴 Aucune n'est réglable, et c'est délibéré. Sur Moonlight, « services Kafra »
// désigne le **Warp Agent** : gratuit, présent partout, et seul moyen
// d'atteindre ce qui n'est pas relié à pied — le laisser éteint ne donnerait au
// joueur qu'un « aucun chemin » incompréhensible. L'avion et les scrolls sont
// désuets ici, et `navi_scroll_krpri.lub` est de toute façon vide (`{"NULL"}`).
// Une case qu'il ne faut jamais décocher n'est pas un réglage, c'est un piège.
constexpr char kAllRouteOptions = 1 | 2 | 4;

// ── Le `type` passé à SearchRoute — À NE PAS CONFONDRE avec les `kType*` ──────
// Les valeurs coïncident, le sens non : les `kType*` ci-dessus classent un
// RÉSULTAT de recherche, ceux-ci disent au moteur COMMENT résoudre la CIBLE.
//
// 🔴🔴 `kGoWithCoords` n'est pas « la carte, éventuellement précisée ». Dans
// `CNavigation_PrepareDestination` (`0x00B39030`), sa branche charge la `.gat`
// de la carte visée puis vérifie que la cellule est PRATICABLE
// (`sub_A784C0(x, y)`). L'envoyer avec `(0, 0)` — un coin de mur sur toute carte
// de RO — fait rendre `-97` à `BuildRoute`, donc `0` à `SearchRoute`, et le
// bouton « Y aller » ne fait RIEN, sans le moindre message. C'est exactement le
// bug qu'on a vécu. `kGoMapOnly` est la seule forme correcte quand on n'a pas de
// coordonnées : sa branche ne regarde jamais x/y. Le serveur applique la même
// règle (`clif_navigateTo` : type 0 si `x > 0 && y > 0`, type 1 sinon).
constexpr int kGoWithCoords = 0;  // vise une CELLULE — elle doit être praticable
constexpr int kGoMapOnly    = 1;  // vise la carte, coordonnées ignorées
constexpr int kGoNpcNode    = 2;  // vise un nœud NPC du graphe (retrouvé par x/y)
constexpr int kGoMobId      = 3;  // vise un monstre par son id

inline uint8_t* Nav() { return reinterpret_cast<uint8_t*>(kNavigation); }

// ── Le résultat brut du moteur : 40 octets (§3.5) ────────────────────────────
// 🔴 Le 2ᵉ champ n'est PAS un identifiant : c'est un POINTEUR vers l'objet
// (nœud carte pour un type 0/1, CNavi_Object pour un NPC ou un monstre). C'est
// ce que montrent Navi_FormatResultLabel et Navi_FormatMemberLabel, qui
// l'appellent par vtable. Le prendre pour un entier donnait une fenêtre qui
// affiche juste et navigue faux.
#pragma pack(push, 1)
struct NativeResult {
  int32_t type;
  void*   object;
  int32_t x;
  int32_t y;
  uint8_t name[24];  // std::string MSVC construite par l'appelé
};
#pragma pack(pop)
static_assert(sizeof(NativeResult) == 40, "le moteur range ses résultats par 40 octets");

// La std::string passée PAR VALEUR à SearchRoute. POD de 24 octets : MSVC la
// recopie bit à bit sur la pile, exactement comme le fait le client au site
// d'appel de la commande de chat (§9.2).
// 🔴 C'est l'APPELÉ qui la détruit (il finit sur un std_string_dtor de son
// paramètre) : ne jamais la libérer de notre côté, ce serait un double free.
struct ByValueString {
  uint8_t raw[24];
};

using Search_t       = char(__thiscall*)(void*);
using GetResult_t    = void*(__thiscall*)(void*, NativeResult*, int);
using SelectResult_t = char(__thiscall*)(void*);
using ClearRoute_t   = int(__thiscall*)(void*, char);
using StepCount_t    = int(__thiscall*)(void*);
using GetStep_t      = void*(__thiscall*)(void*, int);
using NodeName_t     = const char*(__thiscall*)(void*);
using MapDisplay_t   = const char*(__thiscall*)(void*);
using ToMapNode_t    = void*(__thiscall*)(void*);
using StrAssign_t    = void*(__thiscall*)(void*, const void*, size_t);
using StrDtor_t      = void(__thiscall*)(void*);
using StrCtorCStr_t  = void*(__thiscall*)(void*, const char*);
// (std::string map, int type, char flags, char hideWindow, int x, int y, int mob)
using SearchRoute_t  = char(__thiscall*)(void*, ByValueString, int, char, char,
                                         int, int, int);
// __stdcall(std::string map /* par valeur */, int x, int y) : compose la balise
// <NAVIL> + map + base62(x) + base62(y) + </NAVIL> et la POSE dans la barre de
// saisie du chat, en la dépliant au besoin. L'appelé détruit la chaîne.
using ShareToChat_t  = void(__stdcall*)(ByValueString, int, int);
using SetFocusedWnd_t = void(__thiscall*)(void*, void*);

// ── Enveloppes protégées ─────────────────────────────────────────────────────
// 🔴 Aucune de ces fonctions ne manipule d'objet C++ : MSVC refuse `__try` dans
// une fonction qui demande un déroulement d'objets (C2712). C'est la raison de
// ce découpage — la conversion en std::string se fait chez l'appelant.

bool SafeReadInt(int offset, int32_t* out) {
  __try {
    *out = *reinterpret_cast<int32_t*>(Nav() + offset);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeReadByte(int offset, bool* out) {
  __try {
    *out = *(Nav() + offset) != 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeWriteOptions(bool service, bool airship, bool scroll) {
  __try {
    *(Nav() + kOffOptService) = service ? 1 : 0;
    *(Nav() + kOffOptAirship) = airship ? 1 : 0;
    *(Nav() + kOffOptScroll)  = scroll ? 1 : 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int SafeMapCount() {
  __try {
    const auto begin = *reinterpret_cast<uintptr_t*>(Nav() + kOffMapsBegin);
    const auto end   = *reinterpret_cast<uintptr_t*>(Nav() + kOffMapsEnd);
    if (!begin || end < begin) return 0;
    return static_cast<int>((end - begin) / 4);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool SafeGroupBounds(const uint8_t** begin, const uint8_t** end) {
  __try {
    *begin = *reinterpret_cast<const uint8_t**>(Nav() + kOffGroupsBegin);
    *end   = *reinterpret_cast<const uint8_t**>(Nav() + kOffGroupsEnd);
    return *begin && *end && *end >= *begin;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeGroupMembers(const uint8_t* slot, const int32_t** begin,
                      const int32_t** end) {
  __try {
    *begin = *reinterpret_cast<const int32_t* const*>(slot);
    *end   = *reinterpret_cast<const int32_t* const*>(slot + 4);
    return *begin && *end && *end >= *begin;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Remplit `out` ET recopie les deux libellés dans des tampons à nous, puis rend
// la std::string que l'appelé a construite. À la sortie, plus rien ne pointe
// dans le moteur : l'appelant peut construire ses std::string tranquillement.
// Ce qu'on tire du nœud d'un NPC / monstre, en plus de ses deux libellés.
struct ObjectFacts {
  int subtype = 0;
  int level   = 0;
  int amount  = 0;
  int stats   = 0;
  int x       = 0;
  int y       = 0;
  // Les 16 bits bas de `+0x0C` : l'id de CLASSE du sprite, celui-là même que
  // `ro::LoadMobSprite` attend. Le natif ne s'en sert que pour son icône de
  // liste ; il ouvre la porte à montrer la bête elle-même.
  int sprite_class = 0;
};

// Lit un `CNavi_Object` du vecteur d'une carte — le pendant de `SafeGetResult`
// quand on tient déjà l'objet au lieu de passer par une recherche.
//
// `is_mob` décide de la lecture des deux derniers champs : ils portent le NIVEAU
// et les stats sur un monstre, les COORDONNÉES sur un PNJ. Ce n'est pas une
// commodité, c'est le format du `.lub` (cf. `kObjLevelOrX`) — se tromper ici
// affiche des coordonnées à la place d'un niveau, sans rien qui le signale.
bool SafeReadObject(void* object, bool is_mob, char* label, size_t label_size,
                    ObjectFacts* facts) {
  label[0] = '\0';
  *facts   = ObjectFacts{};
  __try {
    if (!object) return false;
    auto** vtable = *reinterpret_cast<void***>(object);
    const char* name =
        reinterpret_cast<NodeName_t>(vtable[kVtDisplayName])(object);
    if (name) std::strncpy(label, name, label_size - 1);

    const auto* fields = static_cast<const uint8_t*>(object);
    const int32_t packed = *reinterpret_cast<const int32_t*>(fields + kObjPacked);
    facts->subtype = *reinterpret_cast<const int32_t*>(fields + kObjSubtype);
    if (is_mob) {
      facts->amount       = (packed >> 16) & 0xFFFF;
      facts->sprite_class = packed & 0xFFFF;
      facts->level = *reinterpret_cast<const int32_t*>(fields + kObjLevelOrX);
      facts->stats = *reinterpret_cast<const int32_t*>(fields + kObjStatsOrY);
    } else {
      // 🔴 PAS de masque 16 bits sur un PNJ : un portail de warp y vaut 99999,
      // que masquer transformerait en un sprite pris au hasard.
      facts->sprite_class = packed;
      facts->x = *reinterpret_cast<const int32_t*>(fields + kObjLevelOrX);
      facts->y = *reinterpret_cast<const int32_t*>(fields + kObjStatsOrY);
    }
    return label[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Le nœud d'une carte, par son nom INTERNE. nullptr si le graphe ne la connaît
// pas.
// 🔴 Elle prend un `const char*` NU, pas une `std::string` par valeur —
// contrairement à `SearchRoute` et à `ShareToChat`, ses voisines immédiates. Sa
// comparaison est un `strcmp` sur le nom interne du nœud (`sub_B25750`).
// Lui passer une string par valeur empile 24 octets là où elle en attend 4 : la
// pile part, et l'appel rend n'importe quoi — le symptôme observé était un
// bouton qui « ne fait rien ».
void* SafeFindMapNode(const char* map_name) {
  __try {
    if (!map_name || !*map_name) return nullptr;
    using FindMap_t = void*(__thiscall*)(void*, const char*);
    return reinterpret_cast<FindMap_t>(kFnFindMapByName)(Nav(), map_name);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Les objets d'un des deux vecteurs du nœud. `out` reçoit les pointeurs bruts ;
// c'est l'appelant qui les lit, sous SON `__try`.
int SafeNodeObjects(void* node, int begin_offset, void** out, int max_out) {
  int count = 0;
  __try {
    if (!node) return 0;
    const auto* fields = static_cast<const uint8_t*>(node);
    auto* const* begin =
        *reinterpret_cast<void** const*>(fields + begin_offset);
    auto* const* end =
        *reinterpret_cast<void** const*>(fields + begin_offset + 4);
    if (!begin || !end || end < begin) return 0;
    for (auto* const* it = begin; it != end && count < max_out; ++it)
      if (*it) out[count++] = *it;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return count; }
  return count;
}

bool SafeGetResult(int index, NativeResult* out, char* label, size_t label_size,
                   char* map_name, size_t map_size, ObjectFacts* facts) {
  label[0]    = '\0';
  map_name[0] = '\0';
  *facts = ObjectFacts{};
  __try {
    std::memset(out, 0, sizeof(*out));
    reinterpret_cast<GetResult_t>(kFnGetResult)(Nav(), out, index);

    void* object = out->object;
    if (object) {
      auto** vtable = *reinterpret_cast<void***>(object);
      if (out->type == kTypeNpc || out->type == kTypeMob) {
        // NPC / monstre : son nom par le slot +0x14, sa carte par le slot +0x20.
        const char* name =
            reinterpret_cast<NodeName_t>(vtable[kVtDisplayName])(object);
        if (name) std::strncpy(label, name, label_size - 1);
        void* map_node =
            reinterpret_cast<ToMapNode_t>(vtable[kVtMapNode])(object);
        if (map_node) {
          const char* m = reinterpret_cast<NodeName_t>(kFnNodeName)(map_node);
          if (m) std::strncpy(map_name, m, map_size - 1);
        }
        const auto* fields = static_cast<const uint8_t*>(object);
        const int32_t packed =
            *reinterpret_cast<const int32_t*>(fields + kObjPacked);
        facts->subtype = *reinterpret_cast<const int32_t*>(fields + kObjSubtype);
        // 🔴 `+0x0C` ne s'empaquette que pour un MONSTRE (`quantité << 16 |
        // sprite`, cf. write_spawn). Pour un NPC, c'est le 4ᵉ champ du `.lub`
        // TEL QUEL, et il vaut **99999** sur un portail de warp — masquer en
        // 16 bits donnerait 34463, un sprite pris au hasard dans le bestiaire.
        if (out->type == kTypeMob) {
          facts->amount       = (packed >> 16) & 0xFFFF;
          facts->sprite_class = packed & 0xFFFF;
        } else {
          facts->sprite_class = packed;
        }
        if (out->type == kTypeMob) {
          facts->level = *reinterpret_cast<const int32_t*>(fields + kObjLevelOrX);
          facts->stats = *reinterpret_cast<const int32_t*>(fields + kObjStatsOrY);
        } else {
          // Les coordonnées viennent du NŒUD, pas du résultat : c'est ce que
          // fait le natif (slot +0x1C), et le résultat ne les porte que pour les
          // points de carte.
          facts->x = *reinterpret_cast<const int32_t*>(fields + kObjLevelOrX);
          facts->y = *reinterpret_cast<const int32_t*>(fields + kObjStatsOrY);
        }
      } else {
        // Carte : le nom INTERNE est celui qu'attend SearchRoute, le nom AFFICHÉ
        // est celui qu'on montre au joueur.
        const char* internal = reinterpret_cast<NodeName_t>(kFnNodeName)(object);
        if (internal) std::strncpy(map_name, internal, map_size - 1);
        const char* shown =
            reinterpret_cast<MapDisplay_t>(kFnMapDisplayName)(object);
        std::strncpy(label, shown && *shown ? shown : (internal ? internal : ""),
                     label_size - 1);
      }
    }
    // La chaîne du résultat appartient au client : on la rend avec SON
    // destructeur, les deux CRT n'ayant pas le même tas.
    reinterpret_cast<StrDtor_t>(kStdStringDtor)(out->name);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeRunSearch(int filter, const char* term, size_t term_len) {
  __try {
    *reinterpret_cast<int32_t*>(Nav() + kOffFilter) = filter;
    reinterpret_cast<StrAssign_t>(kStdStringAssign)(Nav() + kOffTerm, term,
                                                    term_len);
    reinterpret_cast<Search_t>(kFnSearch)(Nav());
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Le chemin exact de la commande de chat du client (§9.1) : une std::string par
// valeur, puis type / flags / hideWindow / x / y / mob.
// `hideWindow = 1` empêche le moteur d'ouvrir la fenêtre NATIVE derrière nous —
// c'est ce qui rend la coexistence supportable.
// Rend le VERDICT du moteur, pas seulement « ça n'a pas planté » : la native
// renvoie 1 quand elle a construit un itinéraire et 0 quand le pathfinder n'a
// trouvé aucun chemin. Sans cette valeur on ne pourrait pas distinguer « pas de
// route » d'une route vide en cours de calcul, et c'est précisément le cas que
// le natif ne sait pas expliquer au joueur.
// -1 = l'appel lui-même a échoué (exception).
int SafeGoTo(const char* map, int type, char flags, int x, int y, int mob_id) {
  __try {
    ByValueString packed{};
    reinterpret_cast<StrCtorCStr_t>(kStdStringCtorCStr)(&packed, map);
    return reinterpret_cast<SearchRoute_t>(kFnSearchRoute)(
               Nav(), packed, type, flags, /*hideWindow=*/1, x, y, mob_id)
               ? 1
               : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Le « Share » du natif (cmd 354). Il n'envoie RIEN au serveur : il pré-remplit
// la saisie du chat avec une balise cliquable, à charge du joueur de la valider.
bool SafeShareToChat(const char* map, int x, int y) {
  __try {
    // 🔴 LE PIÈGE. La fonction native écrit dans la fenêtre FOCALISÉE
    // (`mgr + 416`) : elle a été écrite pour être appelée depuis l'OnMsg de la
    // fenêtre 203, qui a forcément le focus à ce moment-là. Appelée depuis un
    // clic ImGui, ce champ vaut **0** — et son `OnMsg(0, ...)` déréférence un
    // pointeur nul. Le symptôme n'est pas un plantage mais un bouton qui « ne
    // fait rien », l'exception étant avalée par ce `__except`.
    // On donne donc le focus à la barre de saisie AVANT d'appeler : la native
    // retrouve alors exactement l'état qu'elle attend, déplie la barre si besoin
    // et y écrit sa balise.
    void* chat = *reinterpret_cast<void**>(kNewChatWndPtr);
    if (!chat) return false;
    void* input = reinterpret_cast<void**>(chat)[kChatInputChild];
    if (!input) return false;
    reinterpret_cast<SetFocusedWnd_t>(kFnSetFocusedWnd)(
        reinterpret_cast<void*>(kUiWindowMgr), input);

    ByValueString packed{};
    reinterpret_cast<StrCtorCStr_t>(kStdStringCtorCStr)(&packed, map);
    reinterpret_cast<ShareToChat_t>(kFnShareToChat)(packed, x, y);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Pose l'icône de trace, LES DEUX APPELS (cf. le bloc de constantes : écrire le
// numéro seul ne change rien au sol).
bool SafeSetRouteIcon(int icon) {
  if (icon < 1 || icon > kRouteIconCount) return false;
  __try {
    reinterpret_cast<void(__thiscall*)(void*, int)>(kFnSetRouteIcon)(Nav(), icon);
    reinterpret_cast<void(__thiscall*)(void*)>(kFnRebuildRoutePaths)(Nav());
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int SafeRouteIcon() {
  __try {
    return *reinterpret_cast<const int32_t*>(Nav() + kOffRouteIcon);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// L'arrêt du guidage, tel que le pratique le `cmd 284` du natif : remettre
// l'état de suivi à zéro, puis appeler SelectResult avec des index hors bornes.
// 🔴 SelectResult (0x00B35F80) n'est PAS « stop » malgré les apparences : c'est
// sa branche d'index invalide qui efface la cible et nettoie l'affichage.
bool SafeStopGuidance() {
  __try {
    // D'abord la CIBLE : remettre la sélection à -1 puis rappeler SelectResult
    // efface le nom de destination (+0x110C), donc notre bandeau.
    *reinterpret_cast<int32_t*>(Nav() + kOffFollowState) = 0;
    *reinterpret_cast<int32_t*>(Nav() + kOffSelGroup)    = -1;
    *reinterpret_cast<int32_t*>(Nav() + kOffSelMember)   = 0;
    reinterpret_cast<SelectResult_t>(kFnSelectResult)(Nav());
    // Puis l'ITINÉRAIRE lui-même. Sans cet appel, effacer la cible ne suffisait
    // pas : le chemin en cellules survivait, le drapeau +0x117C restait à 1, et
    // le client continuait à semer ses traces au sol pour une destination que
    // le joueur venait d'abandonner.
    *reinterpret_cast<uint8_t*>(Nav() + kOffGuideFlag) = 0;
    reinterpret_cast<ClearRoute_t>(kFnClearRoute)(Nav(), 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Le drapeau de trace, remis à zéro. Le moteur le rallume lui-même quand il a
// calculé un chemin : l'éteindre avant une tentative en fait le témoin de cette
// tentative-là, et de rien d'autre.
void SafeClearTrailFlag() {
  __try {
    *reinterpret_cast<uint8_t*>(Nav() + kOffTrailActive) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Y a-t-il une trace au sol posée ? Sonde minimale — le drapeau du moteur et un
// vecteur non vide — pour ne pas parcourir tout le chemin à chaque frame juste
// pour savoir s'il existe.
bool SafeTrailActive() {
  __try {
    if (*reinterpret_cast<const uint8_t*>(Nav() + kOffTrailActive) == 0) return false;
    const uint8_t* begin =
        *reinterpret_cast<const uint8_t* const*>(Nav() + kOffCellPathBegin);
    const uint8_t* end =
        *reinterpret_cast<const uint8_t* const*>(Nav() + kOffCellPathEnd);
    return begin != nullptr && end > begin;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Le chemin en CELLULES, décimé aux changements de direction. Rend le nombre de
// points écrits, 0 si aucun guidage n'est actif sur cette carte.
//
// 🔴 Lecture SOUS `__try` de bout en bout : ces bornes sont un `std::vector` du
// client, que le moteur peut réallouer entre deux frames (recalcul à l'entrée
// de carte, ou quand le joueur s'écarte de plus de dix cellules).
size_t SafeReadCellPath(NavigationWindow::PathPoint* out, size_t max) {
  if (!out || max == 0) return 0;
  __try {
    if (*reinterpret_cast<const uint8_t*>(Nav() + kOffTrailActive) == 0) return 0;
    const uint8_t* begin =
        *reinterpret_cast<const uint8_t* const*>(Nav() + kOffCellPathBegin);
    const uint8_t* end =
        *reinterpret_cast<const uint8_t* const*>(Nav() + kOffCellPathEnd);
    if (!begin || end <= begin) return 0;
    const size_t bytes = static_cast<size_t>(end - begin);
    if (bytes % kPathCellSize != 0) return 0;
    const size_t count = bytes / kPathCellSize;
    if (count < 2 || count > kPathCellMax) return 0;

    size_t n        = 0;
    int    last_dir = -1;
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* cell = begin + i * kPathCellSize;
      const int dir = *reinterpret_cast<const int32_t*>(cell + kPathCellOffDir);
      // On ne garde que les COINS : le premier point, le dernier, et ceux où la
      // direction change. Entre deux coins la trajectoire est une droite, que la
      // ligne brisée restitue au pixel près — et un chemin de huit cents
      // cellules tient alors en une trentaine de points.
      const bool keep = (i == 0) || (i == count - 1) || (dir != last_dir);
      last_dir = dir;
      if (!keep) continue;
      if (n >= max) break;  // tronqué : on garde la portion PROCHE du joueur
      out[n].x   = *reinterpret_cast<const int32_t*>(cell);
      out[n].y   = *reinterpret_cast<const int32_t*>(cell + 4);
      out[n].dir = dir;
      ++n;
    }
    return n < 2 ? 0 : n;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Lit une `std::string` MSVC posée à `offset` dans le moteur. Layout : 16 octets
// de tampon interne, la taille en +16, la CAPACITÉ en +20 — et c'est la capacité
// qui dit où sont les octets : >= 16 signifie « alloué sur le tas », donc les
// 4 premiers octets sont un pointeur, pas du texte.
bool SafeReadStdString(int offset, char* out, size_t out_size) {
  out[0] = '\0';
  __try {
    const uint8_t* str = Nav() + offset;
    const uint32_t size     = *reinterpret_cast<const uint32_t*>(str + 16);
    const uint32_t capacity = *reinterpret_cast<const uint32_t*>(str + 20);
    if (size == 0 || size >= out_size) return false;
    const char* text = capacity >= 16
                           ? *reinterpret_cast<const char* const*>(str)
                           : reinterpret_cast<const char*>(str);
    if (!text) return false;
    std::memcpy(out, text, size);
    out[size] = '\0';
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int SafeStepCount() {
  __try {
    int32_t state = *reinterpret_cast<int32_t*>(Nav() + kOffRouteState);
    if (state != 2) return 0;  // pas d'itinéraire publié
    return reinterpret_cast<StepCount_t>(kFnStepCount)(Nav());
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Nom de carte de l'étape `index`. L'objet rendu par GetStep expose son nœud par
// le slot +0x20, et c'est CE nœud qui porte le nom (§3.6).
bool SafeStepName(int index, char* out, size_t out_size) {
  out[0] = '\0';
  __try {
    void* step = reinterpret_cast<GetStep_t>(kFnGetStep)(Nav(), index);
    if (!step) return false;
    auto** vtable = *reinterpret_cast<void***>(step);
    void* node = reinterpret_cast<ToMapNode_t>(vtable[kVtMapNode])(step);
    if (!node) return false;
    const char* name = reinterpret_cast<NodeName_t>(kFnNodeName)(node);
    if (!name) return false;
    std::strncpy(out, name, out_size - 1);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// La CELLULE d'une étape : le point de sortie à rejoindre sur cette carte-là.
//
// Par le slot virtuel `+0x1C` (`CNavi_Object_GetPos` 0x00B26CD0), qui rend un
// couple {x, y} lu en `+0x44`/`+0x48`. C'est bien ce que le natif emploie pour
// placer sa flèche de destination (branche « type 2 ou 6 » de 0x00B37F50).
//
// 🔴 Ces deux champs CHANGENT DE SENS selon l'objet : sur un MONSTRE ils portent
// le niveau et les stats empaquetées (cf. `kObjLevelOrX`). Une étape d'itinéraire
// est un point de passage — un warp — donc jamais un monstre ; c'est ce qui rend
// la lecture légitime ici, et c'est aussi pourquoi elle ne doit PAS être
// réemployée sur un résultat de recherche quelconque.
//
// La convention de retour est celle de MSVC pour une struct rendue par valeur :
// l'appelé reçoit un tampon caché et rend son adresse.
bool SafeStepPos(int index, int* out_x, int* out_y) {
  *out_x = 0;
  *out_y = 0;
  __try {
    void* step = reinterpret_cast<GetStep_t>(kFnGetStep)(Nav(), index);
    if (!step) return false;
    auto** vtable = *reinterpret_cast<void***>(step);
    using GetPos_t = int32_t*(__thiscall*)(void*, int32_t*);
    int32_t point[2] = {0, 0};
    const int32_t* got =
        reinterpret_cast<GetPos_t>(vtable[kVtObjectPos])(step, point);
    if (!got) return false;
    *out_x = got[0];
    *out_y = got[1];
    // (0, 0) = le moteur n'a pas de point pour cette étape : ne rien dessiner
    // vaut mieux qu'un marqueur au coin de la carte.
    return *out_x > 0 && *out_y > 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Élément / taille / race, décodés du champ empaqueté d'un monstre (le serveur
// l'écrit ainsi dans `write_spawn`). Les noms restent en anglais : ce sont les
// termes du jeu, ceux que le joueur lit partout ailleurs.
const char* ElementName(int element) {
  static const char* kNames[] = {"Neutral", "Water", "Earth",  "Fire",  "Wind",
                                 "Poison",  "Holy",  "Shadow", "Ghost", "Undead"};
  return element >= 0 && element < 10 ? kNames[element] : "?";
}
const char* SizeName(int size) {
  static const char* kNames[] = {"Small", "Medium", "Large"};
  return size >= 0 && size < 3 ? kNames[size] : "?";
}
const char* RaceName(int race) {
  static const char* kNames[] = {"Formless", "Undead", "Brute",      "Plant",
                                 "Insect",   "Fish",   "Demon",      "Demi-Human",
                                 "Angel",    "Dragon"};
  return race >= 0 && race < 10 ? kNames[race] : "?";
}

// Racine des bitmaps d'interface du client, en CP949 (« 유저인터페이스 ») — le
// même littéral que la minimap, d'où viennent aussi les miniatures de carte.
// ⚠ En ÉCHAPPEMENTS HEXA, jamais en caractères : ce fichier est en UTF-8, or le
// client attend du CP949 — coller les glyphes donnerait deux octets par
// caractère et un chemin introuvable, en silence.
constexpr char kUiRoot[] = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA";

// Miniature d'une carte, en infobulle. Le natif réserve un cadre fixe à droite
// de sa fenêtre pour cette image ; la mettre au survol donne la même information
// sans immobiliser un tiers de la largeur.
// `CachedTextureFromGameFile` mémorise le résultat PAR CHEMIN, échec compris —
// une carte sans bitmap ne relance donc pas un chargement à chaque frame — et
// son cache se vide seul au reset de device.
// Dessine le plan de la carte, inscrit dans un carré de `side` sans déformer.
// Ce sont les bitmaps du radar, servis par le cache de textures du jeu.
void MapThumbnail(const char* map_name, float side) {
  if (!map_name || !*map_name) return;
  char path[192];
  _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\map\\%s.bmp", kUiRoot,
              map_name);
  const ro::GameTexture tex = ro::CachedTextureFromGameFile(path);
  if (!tex.tex) {
    ImGui::TextDisabled("%s", i18n::Tr("(pas de miniature)"));
    return;
  }
  const float w = static_cast<float>(tex.w), h = static_cast<float>(tex.h);
  const float scale = w > h ? side / w : side / h;  // garder les proportions
  ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex.tex)),
               ImVec2(w * scale, h * scale));
}

void MapThumbnailTooltip(const char* map_name) {
  if (!map_name || !*map_name) return;
  ImGui::BeginTooltip();
  ImGui::TextUnformatted(map_name);
  MapThumbnail(map_name, ro::Px(160.0f));
  ImGui::EndTooltip();
}

// Le marqueur MVP. 🔴 En texte coloré il était illisible : le corps d'une
// fenêtre RO est CLAIR (beige), et un doré posé dessus disparaît. On peint donc
// une PASTILLE — fond doré soutenu, texte presque noir — qui garde son contraste
// sur le skin comme sur une ligne survolée.
void MvpBadge() {
  static const char kLabel[] = "MVP";
  const ImVec2 text  = ImGui::CalcTextSize(kLabel);
  const float  pad_x = ImGui::GetStyle().FramePadding.x * 0.7f;
  const ImVec2 p0    = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + text.x + pad_x * 2.0f, p0.y + text.y);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(198, 146, 12, 255), 3.0f);
  dl->AddText(ImVec2(p0.x + pad_x, p0.y), IM_COL32(28, 20, 0, 255), kLabel);
  // Réserver la place, sinon le texte suivant se dessinerait par-dessus.
  ImGui::Dummy(ImVec2(p1.x - p0.x, p1.y - p0.y));
}

// Les libellés du graphe viennent des `.lub` générés par le serveur : ils sont
// en pratique ASCII, mais on passe quand même par la conversion CP949 — règle du
// projet pour toute chaîne venue du client, et sans coût sur de l'ASCII pur.
std::string ToUtf8(const char* client_text) {
  if (!client_text || !*client_text) return std::string();
  // ⚠ ro::LocalToUtf8 et NON Cp949ToUtf8 — même règle que la MsgStringTable et
  // les noms d'items. Ces chaînes sortent des `.lub` de navigation, que le
  // client lit dans SA code-page : 949 en Corée, mais **1252 en Europe**, ce
  // qu'est Moonlight. Or les noms viennent des scripts serveur, eux aussi en
  // CP1252. Décodé en 949, le `à` de « Retour à Gonryun » (0xE0) passe pour un
  // octet de tête, avale l'espace qui suit, et toute la séquence tombe sur le
  // caractère de remplacement : « Retour ?Gonryun ».
  // Le tampon rendu est thread-local et ROTATIF : la copie dans la std::string
  // est ce qui le met hors de portée.
  const char* utf8 = ro::LocalToUtf8(client_text);
  return utf8 ? std::string(utf8) : std::string(client_text);
}

}  // namespace

NavigationWindow::NavigationWindow() = default;

void NavigationWindow::Toggle() {
  open_ = !open_;
  if (open_) {
    need_pos_ = true;
    // Repartir de l'état RÉEL du moteur : la native a pu chercher entre-temps,
    // et il n'y a qu'un seul jeu de résultats pour les deux fenêtres.
    RefreshResults();
    // On IMPOSE nos options au moteur plutôt que de les lui demander : elles
    // viennent des réglages du joueur et lui survivent d'une session à l'autre,
    // alors que le moteur repart toujours de zéro.
    ReadOptions();
  }
}

// ── Tick : c'est ICI que le natif est appelé, jamais dans la frame ───────────
void NavigationWindow::OnTick() {
  if (!imgui_enabled_) return;
  if (!Bourgeon::Instance().IsGameActive()) {
    // Hors du monde, le graphe n'est pas interrogeable et nos miroirs seraient
    // des reliques : mieux vaut un panneau vide qu'un panneau qui ment.
    groups_.clear();
    route_.clear();
    graph_ready_ = false;
    return;
  }

  // ── Les natives que l'on remplace, DÉTRUITES ──────────────────────────────
  // Elles ont été masquées à la naissance (HandleNativeCreation) ; on les
  // supprime ici, hors de MakeWindow, dont l'appelant déréférence encore le
  // retour. Une native masquée mais vivante avalerait un appui sur deux et
  // volerait le clavier.
  //
  // ⚠ AVANT la garde `graph_ready_` : sans données de navigation le panneau ne
  // sert à rien, mais la native, elle, naîtrait quand même — et resterait
  // masquée à l'écran, à voler le clavier.
  for (const int native_id : {kNativeWndMain, kNativeWndRoute, kNativeWndIcon,
                              kNativeWndHelp}) {
    if (uiwnd::FindWindow(native_id) != nullptr) uiwnd::CloseWindow(native_id);
  }

  map_count_   = SafeMapCount();
  graph_ready_ = map_count_ > 0;
  if (!graph_ready_) return;

  PumpIntents();

  // 🔴 L'itinéraire est relu MÊME PANNEAU FERMÉ, et ce n'est pas du zèle : la
  // minimap y lit le point de sortie de la carte courante, et c'est justement
  // fermé qu'on la regarde. Le miroir servait auparavant au seul volet
  // d'itinéraire, d'où sa place après la garde.
  //
  // Le coût est celui d'un `GetStepCount` et de deux appels virtuels par étape,
  // une fois par tick (~100 ms) et seulement quand une route existe — le compte
  // est nul le reste du temps.
  RefreshRoute();

  if (!open_) return;
  ReadOptions();
}

void NavigationWindow::PumpIntents() {
  // Les options d'abord : le pathfinder les lit au moment du calcul, elles
  // doivent donc être posées AVANT l'intention « aller ».
  // Reposees a CHAQUE tick, sans condition : le moteur les remet a zero et
  // `SearchRoute` les reecrit depuis son propre masque des qu'un itineraire est
  // demande ailleurs (fenetre native, lien de chat, `navigateto` scripte).
  SafeWriteOptions(true, true, true);

  if (dirty_) {
    RunSearch();
    dirty_ = false;
    RefreshResults();
  }

  if (stop_armed_) {
    SafeStopGuidance();
    stop_armed_ = false;
    // Sinon le bandeau « aucun chemin » d'une tentative précédente reviendrait
    // hanter l'écran dès que l'itinéraire redevient vide.
    no_route_ = false;
    RefreshRoute();
  }

  if (share_.armed) {
    // 🔴 L'interface moderne d'abord. Quand la chatbox ImGui est active, la
    // fenêtre de chat NATIVE est DÉTRUITE, pas masquée : le pointeur
    // `g_pNewChatWnd` est nul, et la native de partage — qui écrit dans la
    // fenêtre focalisée — n'a plus rien où écrire. Elle échouait donc en
    // silence, exception avalée par son `__except`. `AppendNaviLink` compose la
    // MÊME balise `<NAVIL>` et l'insère dans notre barre ; elle rend false
    // quand la chatbox ImGui n'est pas de la partie, et c'est seulement là que
    // le chemin natif garde un sens.
    ChatWindow* chat = Bourgeon::Instance().chat_window();
    if (!chat || !chat->AppendNaviLink(share_.map.c_str(), share_.x, share_.y))
      SafeShareToChat(share_.map.c_str(), share_.x, share_.y);
    share_ = GoIntent{};
  }

  // L'icône de trace, reposée quand NOTRE réglage et le moteur divergent. Ce
  // n'est pas seulement l'écho d'un clic : le natif ne persiste pas ce choix, et
  // le moteur repart donc de sa valeur d'usine à chaque session. Comparer plutôt
  // que d'écrire à chaque tick évite de reconstruire huit `std::string` du
  // client soixante fois par seconde pour rien.
  //
  // ⚠ Borné ICI, une fois : un réglage relu d'un fichier édité à la main peut
  // valoir n'importe quoi, et `SafeSetRouteIcon` refuserait alors en silence à
  // chaque tick — le moteur ne rejoindrait jamais notre valeur, donc la
  // comparaison redéclencherait indéfiniment.
  if (route_icon_ < 1 || route_icon_ > kRouteIconCount) route_icon_ = 1;
  if (route_icon_armed_ || SafeRouteIcon() != route_icon_) {
    SafeSetRouteIcon(route_icon_);
    route_icon_armed_ = false;
  }

  if (go_.armed) {
    const char flags = kAllRouteOptions;
    // 🔴 Ardoise propre AVANT l'appel. Le moteur remet lui-même `+0x117C` à 0 en
    // entrant dans son recalcul — mais seulement s'il y arrive : un refus en
    // amont (cellule impraticable, `BuildRoute` rend -97) laisse la trace de
    // l'itinéraire PRÉCÉDENT allumée, et elle témoignerait alors pour une
    // tentative qui n'a jamais eu lieu.
    SafeClearTrailFlag();
    int verdict =
        SafeGoTo(go_.map.c_str(), go_.type, flags, go_.x, go_.y, go_.mob_id);
    // Repli : viser une cellule, c'est parier qu'elle est praticable. Les
    // coordonnées d'un NPC viennent du `.lub` et peuvent tomber sur une case
    // bloquée (décor, comptoir, PNJ posé sur un mur). Plutôt que de renvoyer le
    // joueur à un « aucun chemin » mensonger — la carte, elle, est joignable —
    // on retombe sur la destination large, ce que le natif ne fait jamais.
    // ⚠ Les DEUX témoins doivent dire non. Le verdict seul ne suffit pas :
    // il vaut 0 pour une destination sur la carte courante, pourtant réussie —
    // élargir alors à la carte entière détruirait le guidage précis qu'on vient
    // d'obtenir vers le PNJ.
    if (verdict == 0 && !SafeTrailActive() && go_.type == kGoWithCoords)
      SafeGoTo(go_.map.c_str(), kGoMapOnly, flags, 0, 0, 0);
    // On garde la demande telle quelle : c'est elle qu'on rejouera si le joueur
    // accepte d'élargir les moyens de transport. La rejouer à l'identique évite
    // de redemander au joueur de resélectionner sa cible.
    last_go_ = go_;
    go_ = GoIntent{};
    RefreshRoute();
    // 🔴🔴 UNE DESTINATION SUR LA CARTE COURANTE N'A AUCUNE ÉTAPE, ET CE N'EST
    // PAS UN ÉCHEC. Les étapes d'un itinéraire sont les LIENS à franchir entre
    // cartes ; quand il n'y en a pas à franchir, `GetStepCount` rend 0 — alors
    // que `CNavigation_RefreshOnMapEnter` a parfaitement calculé le chemin en
    // cellules et allumé sa trace. Le bandeau rouge s'affichait donc par-dessus
    // une trace au sol et un tracé de minimap bien vivants : le bug rapporté sur
    // gonryun, un PNJ de la carte où l'on se trouve déjà.
    //
    // Le verdict de la native ne rattrape rien : on savait déjà qu'elle peut
    // rendre 1 sans publier d'itinéraire, et rien ne dit qu'elle rende 1 ici.
    //
    // On tranche donc sur l'ÉTAT DU MOTEUR, et sur lui seul — trois lectures
    // fraîches, aucune dérivée de nos miroirs :
    //  · le drapeau `+0x117C`, celui-là même qui autorise le rendu au sol ; il
    //    vient d'être remis à zéro juste avant l'appel, il ne témoigne donc que
    //    de CETTE tentative ;
    //  · l'état de suivi ;
    //  · le nombre d'étapes.
    // 🔴 Surtout PAS `route_` : `RefreshRoute` y ajoute le nom de la carte visée
    // lu en `+0x110C`, qu'un échec ne remet pas à zéro — une cible périmée
    // suffirait à masquer le bandeau.
    //
    // Le vrai échec reste fréquent et mérite son bandeau : hors des cartes
    // reliées à pied, la seule liaison est le Warp Agent, déclaré en type 204 —
    // que le pathfinder REFUSE tant que « services Kafra » est éteint.
    no_route_ = !SafeTrailActive() && !following_ && SafeStepCount() == 0;
  }

}

void NavigationWindow::RunSearch() {
  // 🔴 On interroge TOUJOURS le moteur en mode « tout » (filtre 0), et on filtre
  // à l'affichage. Deux raisons : changer de pastille devient instantané (aucune
  // relance du moteur), et on ne dépend pas du filtre natif, dont la valeur
  // « carte » construit des groupes que sa propre seconde passe laisse vides.
  SafeRunSearch(0, pending_term_.c_str(), pending_term_.size());
}

void NavigationWindow::ReadOptions() {
  // 🔴 On NE relit PLUS les trois options depuis le moteur — c'est NOUS la
  // source de vérité. Le moteur les remet à zéro au démarrage, et surtout
  // `SearchRoute` les RÉÉCRIT depuis son masque à chaque appel : n'importe quel
  // itinéraire déclenché ailleurs (la native, un `navigateto` scripté) décochait
  // donc les cases du joueur sous ses yeux, et « services Kafra » — sans lequel
  // aucune destination hors des cartes reliées à pied n'est atteignable —
  // retombait silencieusement à zéro entre deux recherches.
  // Elles sont persistées dans les réglages et reposées dans le moteur à
  // l'ouverture du panneau.
  //
  // ⚠ Cette relecture est un simple accès mémoire, JAMAIS un appel natif :
  // `Toggle` peut venir d'une frame ImGui. La vérité sur `following_` est posée
  // par `RefreshRoute`, qui tourne à chaque tick sans attendre l'ouverture du
  // panneau ; on la relit ici pour que l'ouverture n'affiche pas un tick de
  // retard.
  int32_t follow = 0;
  if (SafeReadInt(kOffFollowState, &follow)) following_ = follow != 0;
}

// Recopie le vecteur de GROUPES du moteur. Un groupe est un `std::vector<int>`
// d'indices dans le tableau des résultats bruts : c'est la hiérarchie à deux
// niveaux que le natif étale sur ses deux listbox (§3.5).
void NavigationWindow::RefreshResults() {
  groups_.clear();
  // Les index deviendraient des reliques : le détail montrerait un monstre qui
  // n'est plus dans les résultats affichés.
  sel_group_ = -1;
  sel_entry_ = -1;
  if (!graph_ready_) return;

  const uint8_t* gbegin = nullptr;
  const uint8_t* gend   = nullptr;
  if (!SafeGroupBounds(&gbegin, &gend)) return;

  const int group_count = static_cast<int>((gend - gbegin) / 12);
  for (int g = 0; g < group_count; ++g) {
    const int32_t* ibegin = nullptr;
    const int32_t* iend   = nullptr;
    if (!SafeGroupMembers(gbegin + 12 * g, &ibegin, &iend)) continue;

    Group group;
    for (const int32_t* it = ibegin; it != iend; ++it) {
      NativeResult raw{};
      ObjectFacts  facts;
      char label[128];
      char map_name[64];
      if (!SafeGetResult(*it, &raw, label, sizeof(label), map_name,
                         sizeof(map_name), &facts))
        continue;
      Entry entry;
      entry.type    = raw.type;
      entry.subtype = facts.subtype;
      entry.level   = facts.level;
      entry.amount  = facts.amount;
      entry.stats   = facts.stats;
      entry.sprite_class = facts.sprite_class;
      entry.is_mvp  = facts.subtype == kSubMvp;
      entry.is_shop = facts.subtype == kSubShop;
      // Pour un NPC, les coordonnées fiables sont celles du nœud ; pour un point
      // de carte, seul le résultat les porte.
      entry.x = raw.type == kTypeNpc ? facts.x : raw.x;
      entry.y = raw.type == kTypeNpc ? facts.y : raw.y;
      entry.name = ToUtf8(label);
      entry.map  = ToUtf8(map_name);
      if (entry.name.empty()) entry.name = entry.map;
      // 🔴 Le bornage par carte se fait ICI, sur NOTRE miroir, jamais sur le
      // moteur : son vecteur de résultats est partagé avec la fenêtre native, et
      // le tronquer lui ferait afficher notre liste. Insensible à la casse — le
      // nom vient du graphe d'un côté et du global du client de l'autre, et rien
      // ne garantit qu'ils s'accordent dessus.
      if (!search_map_.empty() &&
          _stricmp(entry.map.c_str(), search_map_.c_str()) != 0)
        continue;
      if (group.entries.empty()) {
        group.name = entry.name;
        group.type = entry.type;
      }
      group.entries.push_back(std::move(entry));
    }
    if (!group.entries.empty()) groups_.push_back(std::move(group));
  }
}

void NavigationWindow::RefreshRoute() {
  // L'état de suivi d'abord, et ICI plutôt que dans `ReadOptions` : celui-ci ne
  // tourne que panneau ouvert, si bien que `following_` avait un tick de retard
  // au moment où `PumpIntents` en tirait un verdict.
  int32_t follow = 0;
  following_ = SafeReadInt(kOffFollowState, &follow) && follow != 0;

  route_.clear();
  const int steps = SafeStepCount();
  for (int i = 0; i < steps && i < 64; ++i) {
    char name[64];
    if (!SafeStepName(i, name, sizeof(name))) continue;
    RouteStep step;
    step.map     = ToUtf8(name);
    step.has_pos = SafeStepPos(i, &step.x, &step.y);
    route_.push_back(std::move(step));
  }

  // 🔴 La destination, que le moteur ne compte PAS parmi les étapes : chacune
  // nomme la carte d'où PART un lien, et la carte d'arrivée n'est la source
  // d'aucun lien. Sans ce complément, un trajet vers `pay_fild10` s'affichait
  // « gonryun > alberta > pay_fild03 > pay_fild07 » — il désignait le chemin
  // sans jamais nommer le but.
  // On la lit dans le moteur plutôt que dans notre propre demande : le joueur a
  // pu lancer l'itinéraire autrement (fenêtre native, lien de chat, script
  // serveur), et c'est le moteur qui sait où l'on va vraiment.
  char target[64];
  if (SafeReadStdString(kOffTargetName, target, sizeof(target))) {
    std::string dest = ToUtf8(target);
    // Une cible déjà en fin de liste ne se répète pas : c'est le cas quand on
    // navigue vers un point de la carte où l'on arrive.
    if (!dest.empty() && (route_.empty() || route_.back().map != dest)) {
      RouteStep last;
      last.map = std::move(dest);
      // Pas de `has_pos` : la destination n'est la source d'aucun lien, donc le
      // moteur ne lui associe pas de point de sortie. La minimap n'a rien à y
      // marquer — on y est déjà arrivé.
      route_.push_back(std::move(last));
    }
  }
}

// ── Rendu ────────────────────────────────────────────────────────────────────
void NavigationWindow::OnRenderUI() {
  if (!imgui_enabled_ || !open_) return;

  // 🔴 `FirstUseEver` pour la POSITION aussi, pas `Always`. Avec `Always`, toute
  // frame où `need_pos_` était armé RAMENAIT la fenêtre en haut à gauche — et
  // les boutons qui appellent `OpenSearch` ou `ShowMapContents` l'armaient,
  // depuis l'intérieur de la fenêtre. Elle sautait donc sous le curseur au
  // premier clic, et sa position n'était jamais conservée.
  //
  // Avec `FirstUseEver`, ImGui pose ces valeurs à la première apparition puis
  // laisse la position vivre sa vie — et la persiste sous l'identifiant stable
  // `###bourgeon_navigation`, donc elle survit d'une ouverture à l'autre comme
  // d'une session à l'autre.
  if (need_pos_) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + ro::Px(40.0f), vp->WorkPos.y + ro::Px(90.0f)),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(ro::Px(430.0f), ro::Px(470.0f)),
                             ImGuiCond_FirstUseEver);
    // Au premier plan : la fenêtre vient d'être demandée (raccourci, lien de
    // chat, fiche de monstre), elle ne doit pas s'ouvrir sous une autre.
    ImGui::SetNextWindowFocus();
    need_pos_ = false;
  }

  bool keep_open = true;
  const bool begun = ro::BeginRoWindow(
      i18n::Tr("Navigation###bourgeon_navigation"), &keep_open);
  if (!keep_open) open_ = false;
  if (!begun) {
    ro::EndRoWindow();
    return;
  }

  if (!graph_ready_) {
    ImGui::TextUnformatted(
        i18n::Tr("Les données de navigation ne sont pas chargées."));
    ro::EndRoWindow();
    return;
  }
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
      ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
    // Diagnostic discret : le nombre de cartes du graphe dit d'un coup d'œil si
    // les .lub chargés sont ceux qu'on croit (1301 pour la génération courante).
    ImGui::SetTooltip(i18n::Tr("%d cartes dans le graphe de navigation."),
                      map_count_);
  }

  // ── Recherche ──────────────────────────────────────────────────────────────
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::InputTextWithHint(
          "##navi_term", i18n::Tr("Chercher une carte, un NPC, un monstre..."),
          input_, sizeof(input_))) {
    // Recherche incrémentale : le graphe est EN MÉMOIRE, relancer à la frappe ne
    // coûte aucun aller-retour serveur. C'est ce que le natif ne fait pas — il
    // attend le clic sur la loupe ou la touche Entrée.
    pending_term_ = input_;
    dirty_        = !pending_term_.empty();
    // 🔴 Taper LIBÈRE le bornage. Il vient d'un lien de chat, donc d'une question
    // précise (« ce Warp Agent-là ») ; dès que le joueur écrit sa propre
    // recherche, la question est la sienne, et un bornage hérité lui cacherait
    // des résultats sans lui dire pourquoi.
    search_map_.clear();
    if (pending_term_.empty()) groups_.clear();
  }

  // ── Filtre en pastilles (le natif : un combo à quatre entrées) ──────────────
  // ⚠ « Maps » n'est PAS traduit, et c'est délibéré : en français « carte » est un
  // FAUX AMI — le catalogue rend « Cartes » par « Cards », qui désigne les
  // cartes-items du jeu. « Map » est le terme du jeu pour un lieu, dans les deux
  // langues (convention : les termes de jeu restent en anglais).
  struct Pill { const char* label; int value; };
  const Pill pills[] = {{i18n::Tr("Tout"), kShowAll},
                        {"Maps", kShowMaps},
                        {"NPC", kShowNpc},
                        {i18n::Tr("Monstres"), kShowMob}};
  for (int i = 0; i < 4; ++i) {
    if (i) ImGui::SameLine();
    if (ro::RoToggleButton(pills[i].label, filter_ == pills[i].value))
      filter_ = pills[i].value;  // filtre d'AFFICHAGE : aucune relance du moteur
  }

  // ── L'aide, à la place de la fenêtre 229 ──────────────────────────────────
  // Celle du client est un pavé de 310 × 200 qui explique la MÉCANIQUE (types de
  // liaisons, options d'itinéraire) plutôt que l'usage — illisible pour qui veut
  // juste aller quelque part. On la remplace par ce qu'une aide doit être ici :
  // trois phrases sur ce qu'on peut faire, au survol du point d'interrogation,
  // plus les invites de gestes posées au fil des volets.
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(i18n::Tr(
        "Tapez un nom de carte, de PNJ ou de monstre : la recherche se relance à "
        "la frappe, sans rien demander au serveur.\n\n"
        "Choisissez un résultat pour voir ses détails, puis « Y aller » pour être "
        "guidé. Le chemin passe par le Warp Agent quand la marche ne suffit pas.\n\n"
        "Clic droit sur un résultat : le menu. Maj+clic : le lien dans le chat."));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }

  // ── « Ici », en un bouton ──────────────────────────────────────────────────
  // La question la plus courante devant une navigation n'est pas « où est
  // Prontera » mais « qu'est-ce qu'il y a ICI » — quels PNJ, quels monstres, sur
  // la carte où je me trouve. Le natif ne sait pas y répondre : il faut savoir
  // comment sa carte s'appelle et le taper.
  //
  // La recherche part sur le nom AFFICHÉ, jamais sur le nom interne : c'est ce
  // que le moteur compare dans ses résultats.
  {
    char current_map[64];
    const bool have_map = rag::CurrentMapName(current_map, sizeof(current_map));
    ImGui::BeginDisabled(!have_map);
    // 🔴 Le nom INTERNE, jamais le nom affiché — mesuré, pas supposé. La
    // recherche du moteur (`sub_B27330`) compare le terme à la concaténation
    // « nom_interne + espace + nom_affiché » de chaque carte, et son analyseur
    // découpe la saisie en TOKENS sur les espaces. Or :
    //  · le nom affiché que rend le client (« Gonryun, the Hermit Land
    //    (Kunlun) ») vient de sa DB de cartes, pas de `Navi_Data_Map` que le
    //    moteur emploie — les deux tables sont traduites séparément ;
    //  · même s'il coïncidait, ses virgules et ses parenthèses partiraient dans
    //    les tokens et ne correspondraient à rien.
    // Symptôme observé : Gonryun ne se trouvait pas elle-même. Le nom interne,
    // lui, est TOUJOURS dans la chaîne comparée.
    if (ro::RoButton(i18n::Tr("Rechercher la map actuelle")) && have_map)
      OpenSearch(current_map, /*monsters_only=*/false);
    ImGui::EndDisabled();
    if (have_map && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s  (%s)", MapLabel(current_map).c_str(), current_map);

    // ── Le CONTENU de la carte, qui n'est pas une recherche ─────────────────
    // Chercher le nom d'une carte rend la carte ; ça ne dit pas ce qu'il y a
    // DESSUS. Ce bouton-ci liste ses PNJ et ses spawns, ce que le moteur ne sait
    // pas faire — sa recherche compare un terme à des noms, jamais une carte à
    // son contenu. Deux boutons parce que ce sont deux questions.
    ImGui::SameLine();
    ImGui::BeginDisabled(!have_map);
    if (ro::RoButton(i18n::Tr("Voir ce qu'il y a ici")) && have_map)
      ShowMapContents(current_map);
    ImGui::EndDisabled();
    if (have_map && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", i18n::Tr("Les PNJ et les monstres déclarés sur cette carte."));
  }

  // ── Le BORNAGE, quand un lien de chat en a posé un ─────────────────────
  // 🔴 Une liste tronquée sans explication est un bug aux yeux du joueur : il
  // cherche « Warp Agent », en voit un seul, et croit que les autres n'existent
  // pas. On DIT donc pourquoi, et on offre de lever le bornage sur place — c'est
  // aussi la seule façon de passer de « celui-ci » à « tous les autres ».
  if (!search_map_.empty()) {
    ImGui::TextDisabled("%s", i18n::Tr("Limité à cette carte :"));
    ImGui::SameLine();
    ImGui::TextUnformatted(MapLabel(search_map_.c_str()).c_str());
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Partout"))) {
      search_map_.clear();
      // Il faut REJOUER la recherche : le bornage est appliqué à la recopie du
      // moteur, et rien ne recopie tant qu'on ne le lui demande pas.
      dirty_ = !pending_term_.empty();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s",
                        i18n::Tr("Chercher ce nom sur toutes les cartes."));
  }

  ImGui::Separator();

  // ── Deux volets : la liste à gauche, le détail à droite ────────────────────
  // Le natif étale la même tâche sur quatre fenêtres, dont deux ne suivent même
  // pas la principale quand on la déplace. Ici tout tient dans un seul panneau,
  // et la liste reste sobre : c'est le volet de droite qui porte les faits et
  // les actions, au lieu d'un chapelet de boutons sur chaque ligne.
  const float footer   = ro::Px(120.0f);  // itinéraire + options
  const float detail_w = ro::Px(220.0f);
  if (ImGui::BeginChild("##navi_results", ImVec2(-detail_w, -footer), true))
    DrawResultsPane();
  ImGui::EndChild();

  ImGui::SameLine();
  if (ImGui::BeginChild("##navi_detail", ImVec2(0.0f, -footer), true))
    DrawDetailPane();
  ImGui::EndChild();

  DrawRoute();
  ro::EndRoWindow();
}

void NavigationWindow::OpenSearch(const char* term_utf8, bool monsters_only,
                                  const char* map_filter) {
  if (!term_utf8 || !*term_utf8) return;
  // ⚠ Le placement par defaut n'est redemande QUE si la fenetre etait fermee.
  // Plusieurs boutons du panneau appellent cette methode : la reposer a chaque
  // fois la faisait sauter en haut a gauche sous le curseur du joueur.
  if (!open_) need_pos_ = true;
  open_ = true;
  std::snprintf(input_, sizeof(input_), "%s", term_utf8);
  pending_term_ = input_;
  dirty_        = true;   // consommée par PumpIntents, hors frame
  filter_       = monsters_only ? kShowMob : kShowAll;
  search_map_   = (map_filter != nullptr) ? map_filter : "";
  // La sélection d'avant appartient aux résultats d'avant.
  sel_group_ = -1;
  sel_entry_ = -1;
}

void NavigationWindow::HandleNativeCreation(void* win, int window_id) {
  if (!win || !imgui_enabled_) return;
  __try {
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + uiwnd::kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

  // Seule la principale porte une intention du joueur ; les satellites ne
  // naissent que sur ordre de celle-ci, et n'ont donc rien à basculer.
  if (window_id != kNativeWndMain) return;
  // Reconstruction d'interface au changement de carte : personne n'a rien
  // demandé, on ne touche pas à l'état du panneau.
  if (Bourgeon::Instance().IsMapLoading()) return;
  // C'est NOUS qui portons la bascule : la native étant détruite, le client ne
  // la voit jamais exister et redemande une création à chaque appui.
  if (open_) { open_ = false; return; }
  open_     = true;
  need_pos_ = true;
}

void NavigationWindow::ShowMapContents(const char* map_name) {
  if (!map_name || !*map_name) return;
  if (!open_) need_pos_ = true;  // meme regle que OpenSearch : ne pas la deplacer
  open_ = true;
  // 🔴 La recherche en cours est ANNULÉE, pas complétée : `dirty_` armé
  // relancerait le moteur au tick suivant et écraserait ce qu'on vient de
  // construire. On vide aussi la saisie, pour que l'écran ne montre pas un
  // terme qui ne correspond à rien de ce qui est affiché.
  dirty_ = false;
  pending_term_.clear();
  input_[0] = '\0';
  filter_     = kShowAll;
  // Le contenu d'une carte n'est pas une recherche bornée : on construit la
  // liste nous-mêmes, `RefreshResults` ne repassera pas dessus.
  search_map_.clear();
  sel_group_ = -1;
  sel_entry_ = -1;
  groups_.clear();

  void* node = SafeFindMapNode(map_name);
  if (node == nullptr) return;

  // Deux groupes, dans l'ordre où on les cherche : les PNJ (à qui l'on parle),
  // puis les monstres (que l'on chasse).
  struct Bucket {
    int  offset;
    int  type;
    bool is_mob;
  };
  const Bucket buckets[] = {{kOffNodeNpcsBegin, kTypeNpc, false},
                            {kOffNodeMobsBegin, kTypeMob, true}};

  for (const Bucket& bucket : buckets) {
    // Plafond DUR : une carte de ville dépasse la centaine de PNJ, et une liste
    // sans borne pourrait grossir avec les données du serveur.
    static constexpr int kMaxPerBucket = 512;
    void* objects[kMaxPerBucket];
    const int count =
        SafeNodeObjects(node, bucket.offset, objects, kMaxPerBucket);
    if (count <= 0) continue;

    Group group;
    group.type = bucket.type;
    group.name = bucket.is_mob ? i18n::Tr("Monstres") : "NPC";
    for (int i = 0; i < count; ++i) {
      char label[128];
      ObjectFacts facts;
      if (!SafeReadObject(objects[i], bucket.is_mob, label, sizeof(label), &facts))
        continue;
      Entry entry;
      entry.type         = bucket.type;
      entry.name         = ToUtf8(label);
      entry.map          = map_name;
      entry.subtype      = facts.subtype;
      entry.level        = facts.level;
      entry.amount       = facts.amount;
      entry.stats        = facts.stats;
      entry.sprite_class = facts.sprite_class;
      entry.x            = facts.x;
      entry.y            = facts.y;
      entry.is_mvp       = facts.subtype == kSubMvp;
      entry.is_shop      = facts.subtype == kSubShop;
      group.entries.push_back(std::move(entry));
    }
    if (!group.entries.empty()) groups_.push_back(std::move(group));
  }
}

bool NavigationWindow::RouteExitOnMap(const char* map_name, int* out_x,
                                      int* out_y) const {
  if (!map_name || !*map_name || !out_x || !out_y) return false;
  for (const RouteStep& step : route_) {
    if (!step.has_pos) continue;
    // Comparaison insensible à la casse : le nom vient d'un côté du moteur
    // (`CNaviNode::GetName`) et de l'autre du global du client, et rien ne
    // garantit qu'ils s'accordent sur la casse.
    if (_stricmp(step.map.c_str(), map_name) != 0) continue;
    *out_x = step.x;
    *out_y = step.y;
    return true;
  }
  return false;
}

size_t NavigationWindow::RouteCellPath(PathPoint* out, size_t max) const {
  return SafeReadCellPath(out, max);
}

bool NavigationWindow::IsGuidanceActive() const {
  // Trois témoins, parce qu'ils ne s'allument pas ensemble :
  //  · `route_` porte les étapes d'un itinéraire MULTI-CARTES ; il est vide quand
  //    la destination est sur la carte courante — le cas le plus fréquent, et
  //    précisément celui où le bouton d'arrêt manquait ;
  //  · `following_` reflète +0x125C, l'état de suivi du moteur ;
  //  · la trace au sol elle-même, dernier recours : un guidage posé par un
  //    `navigateto` scripté la sème avant même que notre miroir ait tourné.
  return following_ || !route_.empty() || SafeTrailActive();
}

void NavigationWindow::GoTo(const char* map_name, int x, int y) {
  if (!map_name || !*map_name) return;
  const bool precise = x > 0 && y > 0;
  go_.armed  = true;
  go_.map    = map_name;
  go_.type   = precise ? kGoWithCoords : kGoMapOnly;
  go_.x      = precise ? x : 0;
  go_.y      = precise ? y : 0;
  go_.mob_id = 0;
}

void NavigationWindow::DrawMapThumbnail(const char* map_name, float side) {
  MapThumbnail(map_name, side);
}

std::string NavigationWindow::MapLabel(const char* map_name) {
  if (!map_name || !*map_name) return std::string();
  char shown[96];
  if (rag::MapDisplayName(map_name, shown, sizeof(shown)) && shown[0] != '\0')
    return ro::LocalToUtf8(shown);  // la DB du client est dans SA code-page
  return map_name;
}

const NavigationWindow::Entry* NavigationWindow::Selection() const {
  if (sel_group_ < 0 || sel_group_ >= static_cast<int>(groups_.size()))
    return nullptr;
  const Group& group = groups_[sel_group_];
  if (sel_entry_ < 0 || sel_entry_ >= static_cast<int>(group.entries.size()))
    return nullptr;
  return &group.entries[sel_entry_];
}

links::Target NavigationWindow::TargetOf(const Entry& entry) const {
  if (entry.type == kTypeMob) {
    // 🔴 Par la classe de SPRITE, pas par un id de mob_db : le `.lub` porte
    // `vd.look[LOOK_BASE]` (cf. `write_spawn`) et le client n'a pas mob_db pour
    // faire la correspondance. C'est le serveur qui la fait, et `FromMobView`
    // marque le lien pour que la demande parte avec le bon drapeau.
    return links::FromMobView(static_cast<uint32_t>(entry.sprite_class),
                              entry.is_mvp ? 2 : 0, entry.name.c_str());
  }
  if (entry.type == kTypeNpc) {
    // Un PNJ n'a ni fiche ni identité stable hors de ces données : tout ce qu'on
    // peut en partager, c'est OÙ LE CHERCHER. D'où une recherche et non un lieu
    // — ses coordonnées ne valent que pour CET exemplaire, et beaucoup de PNJ
    // sont posés en plusieurs endroits sous le même nom.
    // 🔴 Avec SA carte : le nom seul désigne un RÔLE, pas un exemplaire. Un
    // « Warp Agent » partagé sans contexte renvoie les trente-huit du serveur,
    // et le lecteur ne peut pas deviner lequel on lui montrait.
    return links::FromNaviSearch(links::NaviKind::kNpc, entry.name.c_str(),
                                 entry.map.c_str());
  }
  return links::FromNavi(entry.map.c_str(), 0, 0);
}

void NavigationWindow::DrawResultsPane() {
  if (groups_.empty()) {
    ImGui::TextDisabled("%s", pending_term_.empty()
                                  ? i18n::Tr("Tapez un nom pour chercher.")
                                  : i18n::Tr("Aucun résultat."));
  } else {
    // Les gestes MODIFIÉS ne se devinent pas : rien à l'écran ne les annonce, et
    // une fonction qu'on ne peut pas découvrir n'existe pas.
    ImGui::TextDisabled("%s",
                        i18n::Tr("Clic droit : menu · Maj+clic : lien dans le chat"));
  }
  for (int g = 0; g < static_cast<int>(groups_.size()); ++g) {
    const Group& group = groups_[g];
    // Filtre d'affichage. « Maps » couvre les DEUX types de carte (0 et 1).
    const bool shown =
        filter_ == kShowAll ||
        (filter_ == kShowMaps ? group.type <= kTypeMap : group.type == filter_);
    if (!shown) continue;

    const char* kind = group.type <= kTypeMap  ? "map"
                       : group.type == kTypeNpc ? "NPC"
                                                : i18n::Tr("monstre");
    // Le natif affiche `"[%d]%s"` — l'identifiant brut. On montre plutôt la
    // NATURE et le nombre d'endroits, qui est ce que le joueur cherche.
    char header[192];
    std::snprintf(header, sizeof(header), "%s  (%s, %d)###navi_g%d",
                  group.name.c_str(), kind,
                  static_cast<int>(group.entries.size()), g);
    const bool group_has_mvp =
        std::any_of(group.entries.begin(), group.entries.end(),
                    [](const Entry& e) { return e.is_mvp; });
    const bool opened = ImGui::TreeNode(header);
    // 🔴 La miniature n'a de sens que si le groupe désigne UNE seule carte. Un
    // monstre est groupé par NOM et peut vivre sur plusieurs cartes : montrer
    // celle du premier membre serait un mensonge tranquille — « Greatest
    // General » annoncerait `pay_dun03` alors qu'il est sur trois cartes.
    const bool one_map =
        !group.entries.empty() &&
        std::all_of(group.entries.begin(), group.entries.end(),
                    [&group](const Entry& e) {
                      return e.map == group.entries.front().map;
                    });
    if (ImGui::IsItemHovered() && one_map)
      MapThumbnailTooltip(group.entries.front().map.c_str());
    if (group_has_mvp) {
      // Repérable sans déplier : c'est tout l'intérêt de croiser la navigation
      // avec le suivi des MVP (le tracker sait QUAND, la navigation sait OÙ).
      ImGui::SameLine();
      MvpBadge();
    }
    if (!opened) continue;

    for (size_t e = 0; e < group.entries.size(); ++e) {
      const Entry& entry = group.entries[e];
      ImGui::PushID(static_cast<int>(e));
      const bool whole_map = entry.x == kWholeMap && entry.y == kWholeMap;
      // Un libelle COURT, et le volet de droite dit le reste : c'est ce
      // qui garde la liste lisible. Le natif ecrit « [12]Yoyo » -- son
      // identifiant brut colle au nom -- et n'offre aucun moyen d'en savoir
      // plus.
      char label[224];
      if (entry.type == kTypeMob) {
        // 🔴 Un monstre n'a PAS de coordonnees : les deux derniers champs de
        // son noeud portent son niveau et ses statistiques. Les afficher comme
        // un « (x, y) » etait le piege.
        std::snprintf(label, sizeof(label), "%s  ·  %s %d###e%d",
                      entry.name.c_str(), i18n::Tr("niv."), entry.level,
                      static_cast<int>(e));
      } else if (entry.type <= kTypeMap || whole_map) {
        std::snprintf(label, sizeof(label), "%s###e%d",
                      entry.map.empty() ? entry.name.c_str()
                                        : entry.map.c_str(),
                      static_cast<int>(e));
      } else {
        // 🔴 Le NOM du NPC, pas celui de la carte : le groupe EST deja la
        // carte, la repeter a chaque ligne masquait la seule information que
        // le joueur cherche.
        std::snprintf(label, sizeof(label), "%s###e%d", entry.name.c_str(),
                      static_cast<int>(e));
      }

      const bool is_sel =
          sel_group_ == g && sel_entry_ == static_cast<int>(e);
      if (ImGui::Selectable(label, is_sel)) {
        sel_group_ = g;
        sel_entry_ = static_cast<int>(e);
      }
      // ── La ligne est aussi un LIEN ────────────────────────────────────────
      // Le clic GAUCHE reste la sélection : c'est le métier du widget, et le
      // volet de détail en dépend. On n'ajoute donc que les gestes MODIFIÉS —
      // clic droit pour le menu, Maj+clic pour poser le lien dans le chat —
      // exactement la règle du .h de links:: pour un widget qui a déjà un rôle
      // au clic simple. D'où `links::Hit` plutôt que `links::Gestures`, qui
      // gouvernerait les trois boutons et volerait la sélection.
      //
      // ⚠ La cible n'est construite QUE sur la ligne survolée. Elle n'est pas
      // gratuite — celle d'une carte résout son nom affiché par un appel natif
      // et une allocation — et la bâtir pour chaque ligne à chaque frame ferait
      // payer une liste de cinquante résultats soixante fois par seconde pour
      // une information dont on n'a besoin que sous le curseur.
      if (ImGui::IsItemHovered()) {
        const links::Target row = TargetOf(entry);
        switch (links::Hit(row, true)) {
          case links::Gesture::kChatLink: links::PostToChat(row); break;
          case links::Gesture::kMenu:
            row_menu_      = row;
            row_menu_open_ = true;  // ouvert hors de l'arbre (piles d'ID)
            break;
          default: break;  // le clic gauche appartient au Selectable
        }
      }
      if (entry.is_mvp) {
        // Reperable sans ouvrir le detail : c'est tout l'interet de croiser la
        // navigation avec le suivi des MVP.
        ImGui::SameLine();
        MvpBadge();
      }
      ImGui::PopID();
    }
    ImGui::TreePop();
  }

  // Le menu, ouvert et dessiné HORS de l'arbre : `ImGui::TreeNode` et `PushID`
  // empilent des identifiants, et un popup ouvert sous cette pile ne serait pas
  // retrouvé par le `BeginPopup` d'après. Même détour que la table des drops de
  // la fiche de monstre.
  if (row_menu_open_) {
    row_menu_open_ = false;
    ImGui::OpenPopup("##navi_row_menu");
  }
  links::DrawMenu("##navi_row_menu", row_menu_);
}

// ── Volet de DÉTAIL ──────────────────────────────────────────────────────────
// Tout ce que le `.lub` sait de la cible, et que le natif garde pour lui : sa
// liste se contente de « [12]Yoyo », et pour les monstres d'une vague tranche de
// densité (« nombreux », « peu »). On a le niveau exact, le nombre d'exemplaires
// du spawn, l'élément et son niveau, la taille, la race — et le plan de la
// carte, que le natif n'affiche que dans un cadre séparé.
void NavigationWindow::DrawDetailPane() {
  const Entry* sel = Selection();
  if (!sel) {
    ImGui::TextDisabled("%s", i18n::Tr("Choisissez un résultat."));
    return;
  }
  const Entry& entry     = *sel;
  const bool   whole_map = entry.x == kWholeMap && entry.y == kWholeMap;

  ImGui::TextWrapped("%s", entry.name.c_str());
  if (entry.is_mvp) MvpBadge();
  if (entry.is_shop) ImGui::TextDisabled("%s", i18n::Tr("boutique"));
  ImGui::Separator();

  if (!entry.map.empty()) ImGui::TextDisabled("%s", entry.map.c_str());

  // Ce qu'on montre en grand dépend de ce qu'on regarde. Pour un monstre, c'est
  // la BÊTE qu'on cherche à reconnaître — sa carte, on la lit dans le libellé
  // juste au-dessus, et son plan reste à un survol. Pour une carte ou un PNJ, le
  // plan est au contraire l'information principale.
  // Vaut aussi pour un NPC : son nœud porte la même classe de sprite, et
  // `ro::LoadMobSprite` la résout par `jobName.lub`, qui couvre les deux —
  // « monstre » est le nom de la fonction, pas sa limite.
  // ⚠ Sauf `kWarpPortalSprite` : un portail n'est pas un personnage, il n'a rien
  // à montrer. C'est la même sentinelle que teste `queryNavi_NpcInfo`.
  const bool has_sprite = (entry.type == kTypeMob || entry.type == kTypeNpc) &&
                          entry.sprite_class > 0 &&
                          entry.sprite_class != kWarpPortalSprite;
  const bool show_mob_sprite = has_sprite &&
                               ro::LoadMobSprite(entry.sprite_class, &mob_sprite_) &&
                               !mob_sprite_.is_model;

  if (show_mob_sprite) {
    const float side = ro::Px(150.0f);
    const ImVec2 p0  = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + side, p0.y + side);
    // `allow_upscale` reste à false : la taille RÉELLE du sprite renseigne sur
    // le gabarit de la bête, un Poring gonflé à la taille d'un Baphomet
    // mentirait.
    ro::DrawMobSprite(ImGui::GetWindowDrawList(), mob_sprite_, p0, p1,
                      static_cast<float>(ImGui::GetTime()));
    // Cliquable : la navigation sait OÙ, la fiche sait QUOI. `by_view = true`
    // parce que le `.lub` porte une classe de SPRITE et non un id de mob_db —
    // `write_spawn` écrit `vd.look[LOOK_BASE]`. C'est le serveur qui fait la
    // correspondance inverse, le client n'ayant pas mob_db.
    // Le clic n'a de sens que sur un MONSTRE : il n'existe pas de fiche pour un
    // NPC, dont le sprite n'est là que pour le reconnaître.
    const bool clickable = entry.type == kTypeMob;
    ImGui::InvisibleButton("##navi_sprite", ImVec2(side, side));
    if (clickable && ImGui::IsItemClicked()) {
      if (auto* info = Bourgeon::Instance().monster_info())
        info->Open(static_cast<uint32_t>(entry.sprite_class), /*by_view=*/true);
    }
    // Le survol garde le PLAN — c'est lui qui répond à « où est-ce ? », et le
    // sprite a pris sa place dans le corps du volet. L'invite de clic s'y ajoute
    // plutôt que de la remplacer : les deux tiennent dans une seule infobulle.
    if (ImGui::IsItemHovered()) {
      if (clickable) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      ImGui::BeginTooltip();
      if (!entry.map.empty()) {
        ImGui::TextUnformatted(entry.map.c_str());
        MapThumbnail(entry.map.c_str(), ro::Px(160.0f));
      }
      if (clickable)
        ImGui::TextDisabled("%s", i18n::Tr("Ouvrir la fiche du monstre"));
      ImGui::EndTooltip();
    }
  } else if (!entry.map.empty()) {
    // 🔴 Y compris pour les classes rendues par un MODÈLE 3D (Emperium,
    // gardiens, coffres) : elles n'ont aucun `.spr`, et le dire « pas de
    // sprite » serait faux. On retombe simplement sur le plan.
    MapThumbnail(entry.map.c_str(), ro::Px(150.0f));
  }

  if (entry.type == kTypeMob) {
    // 🔴 Un monstre n'a AUCUNE position : le fichier n'en donne pas. Ses deux
    // derniers champs portent le niveau et des statistiques empaquetées.
    ImGui::Text("%s %d", i18n::Tr("niv."), entry.level);
    ImGui::Text("%s : %d", i18n::Tr("Exemplaires"), entry.amount);
    if (entry.stats) {
      const int element = (entry.stats >> 16) & 0xFFFF;
      ImGui::Text("%s %d", ElementName(element % 20), element / 20);
      ImGui::TextUnformatted(SizeName((entry.stats >> 8) & 0xFF));
      ImGui::TextUnformatted(RaceName(entry.stats & 0xFF));
    }
  } else if (entry.type == kTypeNpc && !whole_map) {
    ImGui::Text("(%d, %d)", entry.x, entry.y);
  }

  ImGui::Separator();

  const bool can_go = !entry.map.empty();
  ImGui::BeginDisabled(!can_go);
  if (ro::RoButton(i18n::Tr("Y aller"))) {
    go_.armed = true;
    go_.map   = entry.map;
    // Un NPC a des coordonnées exploitables ; un monstre n'en a pas de fixes, on
    // vise donc sa carte — ce que fait le natif aussi.
    // 🔴 Des coordonnées ne suffisent pas : encore faut-il qu'elles soient
    // PLAUSIBLES. Le moteur les traduit en cellule et refuse tout ce qui n'est
    // pas praticable, `(0, 0)` en tête — d'où l'exigence du `> 0`, qui est aussi
    // le test exact du serveur dans `clif_navigateTo`.
    const bool precise =
        entry.type == kTypeNpc && !whole_map && entry.x > 0 && entry.y > 0;
    go_.type   = precise ? kGoWithCoords : kGoMapOnly;
    go_.x      = precise ? entry.x : 0;
    go_.y      = precise ? entry.y : 0;
    go_.mob_id = 0;
  }
  ImGui::EndDisabled();

  // « Partager » : le geste du bouton Share natif — poser dans la barre de chat
  // une balise <NAVIL> cliquable, que les autres joueurs suivront d'un clic.
  // Rien n'est envoyé : le joueur relit et valide lui-même.
  ImGui::BeginDisabled(!can_go);
  if (ro::RoButton(i18n::Tr("Partager"))) {
    share_.armed = true;
    share_.map   = entry.map;
    share_.x     = whole_map ? 0 : entry.x;
    share_.y     = whole_map ? 0 : entry.y;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s",
                      i18n::Tr("Écrit un lien dans votre barre de chat ; à vous de l'envoyer."));
}

void NavigationWindow::DrawRoute() {
  // ── Itinéraire ─────────────────────────────────────────────────────────────
  if (route_.empty() && no_route_) {
    // On distingue « rien demandé » de « demandé mais sans trajet » : le second
    // cas est un vrai renseignement, que le natif laisse deviner — il se contente
    // d'un message système noyé dans le chat, sans jamais nommer la cause.
    //
    // 🔴 La cause est presque toujours la même. Le graphe de navigation ne
    // connaît que les warps ; un lieu qu'on n'atteint qu'en PARLANT à un PNJ y
    // est un îlot. Ces liaisons n'existent dans les données que sous le type
    // 204, et le pathfinder REFUSE ce type tant que « services Kafra » est
    // éteint. Le joueur, lui, en conclut que sa destination n'existe pas.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.15f, 0.10f, 1.0f));
    ImGui::TextWrapped("%s", i18n::Tr("Aucun chemin jusqu'à cette destination."));
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "%s",
        i18n::Tr("Aucune liaison connue ne mène jusque-là."));
  } else if (route_.empty()) {
    // 🔴 « Pas d'étapes » ne veut PAS dire « pas de guidage » : un itinéraire qui
    // se termine sur la carte courante n'a aucune étape à énumérer, et c'est le
    // cas ordinaire. Le bouton d'arrêt vivait dans l'autre branche — il était
    // donc introuvable exactement quand on en avait le plus besoin.
    ImGui::TextDisabled("%s", IsGuidanceActive()
                                  ? i18n::Tr("Guidage en cours, sans étape à afficher.")
                                  : i18n::Tr("Aucun itinéraire en cours."));
  } else {
    std::string line;
    for (size_t i = 0; i < route_.size(); ++i) {
      if (i) line += "  >  ";
      line += route_[i].map;
    }
    ImGui::TextWrapped("%s", line.c_str());
  }
  if (IsGuidanceActive()) {
    if (ro::RoButton(i18n::Tr("Arrêter le guidage"))) stop_armed_ = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s",
                        i18n::Tr("Efface l'itinéraire et les traces au sol."));
    ImGui::SameLine();
  }
  DrawRouteIconPicker();
}

// Le choix de la TRACE au sol. Le natif lui consacre une fenêtre entière (306,
// 158 × 102), ouverte par un bouton de la barre de titre et posée à un décalage
// FIXE de la principale — donc à côté de la plaque dès qu'on déplace celle-ci.
// Ici : un bouton qui porte l'icône courante et déroule les huit autres.
void NavigationWindow::DrawRouteIconPicker() {
  const float side = ro::Px(24.0f);
  if (RouteIconButton(route_icon_, side, "##navi_icon_cur"))
    ImGui::OpenPopup("##navi_icon_pick");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::Tr("Trace du guidage au sol"));

  if (ImGui::BeginPopup("##navi_icon_pick")) {
    // Quatre par rangée, comme le natif : ses huit vignettes tiennent en 4 × 2.
    for (int icon = 1; icon <= kRouteIconCount; ++icon) {
      char id[32];
      std::snprintf(id, sizeof(id), "##navi_icon%d", icon);
      if (RouteIconButton(icon, ro::Px(32.0f), id)) {
        route_icon_       = icon;
        route_icon_armed_ = true;  // l'appel natif part au tick
        ImGui::CloseCurrentPopup();
      }
      if (icon % 4 != 0) ImGui::SameLine();
    }
    ImGui::EndPopup();
  }
}

// Une vignette du sélecteur, en bouton. Rend true au clic.
// Repli en bouton TEXTE si le bitmap manque (GRF allégé, skin remplacé) : un
// numéro reste utilisable, un trou dans une rangée ne l'est pas.
bool NavigationWindow::RouteIconButton(int icon, float side, const char* id) {
  char path[192];
  _snprintf_s(path, sizeof(path), _TRUNCATE,
              "%s\\navigation_interface3\\btn_roadIocn_select%d_normal.bmp",
              kUiRoot, icon);
  const ro::GameTexture tex = ro::CachedTextureFromGameFile(path);
  ImGui::PushID(id);
  bool clicked = false;
  if (tex.tex != nullptr) {
    // Le cadre marque la sélection : sans lui, rien ne dit laquelle des huit est
    // active une fois le popup ouvert.
    const bool current = icon == route_icon_;
    ImGui::PushStyleColor(ImGuiCol_Button,
                          current ? ImVec4(0.85f, 0.72f, 0.35f, 0.55f)
                                  : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    clicked = ImGui::ImageButton(
        id, static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex.tex)),
        ImVec2(side, side));
    ImGui::PopStyleColor();
  } else {
    char label[16];
    std::snprintf(label, sizeof(label), "%d", icon);
    clicked = ImGui::Button(label, ImVec2(side, side));
  }
  ImGui::PopID();
  return clicked;
}

