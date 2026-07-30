#include "ragnarok/lua.h"
#include "ragnarok/item_db.h"
#include "ui/game_texture.h"
#include "features/windows/item_desc_window.h"
#include "ui/ro_widgets.h"

#include "ragnarok/uiwnd.h"
#include <Windows.h>
#include <shellapi.h>  // ShellExecuteA (ouvrir les liens <URL>)
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

#include "bourgeon.h"
#include "features/systems/bug_report.h"  // BugReport::ItemContext/SkillContext
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "imgui.h"
#include "ui/imgui_escape.h"
#include "ui/ro_imgui.h"           // BeginRoDescWindow (cadre desc)
#include "features/overlays/basic_info.h"    // aperçu équipement (RenderItemPreviewTooltip)
#include "features/systems/bourgeon_opcodes.h"
#include "features/moonlight_ui/moonlight_ui.h"  // API autolootid (bouton +/- réintégré)
#include "utils/log_console.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Constantes RE (cf. mémoire project_item_skill_desc_window_re, CORRECTION
//    LIVE 2026-07-01 : DEUX fenêtres distinctes, pas une unifiée) ─────────────
namespace {

// Slots manager (g_UIWindowMgr = 0x0131f4e8) : non-nul ⇒ fenêtre ouverte, remis
// à 0 à la fermeture (signal FIABLE). Relire FRAIS chaque tick.
constexpr uintptr_t kItemWndSlot  = 0x0131f700;  // mgr+0x218 : ITEM desc (classe 0xc)
constexpr uintptr_t kSkillWndSlot = 0x0131f718;  // mgr+0x230 : SKILL desc (classe 0x2e)

// vtables attendues (valider avant de déréférencer les offsets spécifiques).
constexpr uintptr_t kItemVTable   = 0x01032aac;  // classe 0xc
constexpr uintptr_t kSkillVTable  = 0x01032e0c;  // classe 0x2e

// Offsets communs (base UIWindow, identiques aux 2 classes).
constexpr uintptr_t kOffWidth   = 0x14;   // int  largeur
constexpr uintptr_t kOffHeight  = 0x18;   // int  hauteur

// Fenêtre de COMPARAISON d'équipement (id 0xea) : 2e instance parallèle à 0xc.
// MÊME layout que l'item (id string @+0xe4, struct +0xb8, icône +0x1c4, DrawContent
// partagé 0x008b4200) mais vtable + slot manager distincts. Créée SYNCHRONEMENT
// dans OnMsg 0x18 de 0xc (relais 0xea) => on la lit/cache dans la foulée.
constexpr uintptr_t kCompareWndSlot = 0x0131f708;  // mgr+0x220 : COMPARE desc (id 0xea)
constexpr uintptr_t kCompareVTable  = 0x01032c5c;  // classe 0xea

// Offsets d'id, SPÉCIFIQUES à chaque classe.
constexpr uintptr_t kItemIdStr  = 0xe4;   // (0xc/0xea) std::string id (SSO)
constexpr uintptr_t kItemIdCap  = 0xf8;   // (0xc/0xea) capacité de la std::string id
constexpr uintptr_t kSkillIdInt = 0x104;  // (0x2e) int id BRUT

// Offsets fenêtre item (0xc) pour la reproduction ImGui.
constexpr uintptr_t kItemStruct   = 0xb8;   // ItemSkillInfo (arg des accesseurs)
constexpr uintptr_t kItemIconPath = 0x1c4;  // std::string chemin icône collection (SSO)
constexpr uintptr_t kItemIconLen  = 0x1d4;  // longueur de la std::string chemin
constexpr uintptr_t kItemIconCap  = 0x1d8;  // capacité de la std::string chemin

// Fonctions natives (base 0x400000, no-ASLR).
constexpr uintptr_t kHideNative   = 0x009030c0;  // UIWnd_SetVisible(this,edx,vis) : cache sans détruire
constexpr uintptr_t kGetDescLines = 0x006a2a70;  // ItemSkillDB_GetDescLines(info) -> &vector<char*>
constexpr uintptr_t kGetResName   = 0x006a4bc0;  // ItemSkillDB_GetResName(info) -> resname C-str (icône)
// DB de description (map id->record). Lookup(id,&db) -> record, ou nil(&itemdb::kNilAddr).
// nom d'item affiché = *(char**)(record+4) (cf. GetBaseName). Sert à résoudre les
// noms de cartes/enchants par id.
// Construction standalone d'une ItemSkillInfo (pour ouvrir/lire la desc d'une
// carte par id) : ctor puis SetId. La fenêtre 0xc courante reçoit OnMsg 0x18.
// Chargement paresseux d'une desc par id (RE live 2026-07-05) : dans CE client,
// TOUTES les descs (item ET skill) vivent dans rec+0x0c, lu par GetDescLines quand
// info+0x5c==1 (rec+0x20, le champ "item", est toujours vide). EnsureLoaded parse
// rec+0x0c dans le cache (comme OnMsg 0x18) avant lecture — indispensable pour une
// carte compound jamais ouverte seule.
constexpr uintptr_t kInfoFlag     = 0x5c;        // info+0x5c : 1 => GetDescLines lit rec+0x0c
// Icône (item\) + illustration (cardBmp\) d'une carte. resname icône = GetResName
// (rec+0x08 quand +0x5c=1) ; resname illustration = GetCardResName (DB carte
// 0x01255128, *record). Préfixes CP949 : cardBmp lu du jeu (null-term) ; item
// hardcodé (l'occurrence mémoire 0x01024250 enchaîne un autre chemin, pas null-term).
constexpr uintptr_t kGetCardResName = 0x006a2970;  // __cdecl(id) -> record ; *record = resname
constexpr uintptr_t kCardBmpPrefix  = 0x01036648;  // "유저인터페이스\cardBmp\" (CP949, null-term)
// Fiche « nil » renvoyée par GetCardResName quand l'id est ABSENT de la DB carte
// (FUN_006a0ab0 : lower_bound échoue -> &LAB_01255180). Son resname est "sorry"
// (=> cardBmp\sorry.bmp, le placeholder "now printing"). record == cette adresse
// est donc le test FIABLE « n'est PAS une carte » (non-carte => pas d'illustration,
// on garde l'icône collection). Confirmé live 2026-07-08.
constexpr uintptr_t kCardNilRecord  = 0x01255180;

// Offsets DANS l'ItemSkillInfo (base = wnd+kItemStruct), remplis par
// BuildFromItemRecord : 4 slots cartes/enchants + random options.
constexpr uintptr_t kInfoCards    = 0x1c;   // card[0..3] : 4 x uint32 id (0 = vide)
constexpr uintptr_t kInfoOptCount = 0x98;   // int : nombre de random options
constexpr uintptr_t kInfoOpts     = 0x9c;   // entrées 5o : [index:2][value:2][param:1]
constexpr int       kMaxCards     = 4;
constexpr int       kMaxOpts      = 5;
constexpr uintptr_t kGameFree     = 0x00dbbc7f;  // free() du jeu (pour le vector alloué côté jeu)
constexpr uintptr_t kGameMalloc   = 0x00dbbc4f;  // malloc() du jeu (pairé avec game_free)

// Navigation (routage <NAVI>). ABI capturée en live (bp sur 0x00b314f0) :
// __thiscall(this=navMgr, std::string map BYVAL 0x18o, int type, int flags,
// int a24, int x, int y, int a30). Pour un lien navi item : type=field3,
// x=field1, y=field2 ; flags=1, a24=1, a30=0 (constantes observées).
constexpr uintptr_t kNaviRoute = 0x00b314f0;  // CNavigation::SearchRoute
constexpr uintptr_t kNaviMgr   = 0x015c3090;  // &DAT_015c3090 (nav manager)

// Lua : appel d'un global via wrapper varargs. __cdecl(luaStatePtr, std::string
// funcName BYVAL, const char* fmt "d>s", <args in> , <ptrs out>). Renvoie 1 si OK.
// (RE 0x00a9a7d0 : fmt 'd'=int in / 's'=char* out ; '>' sépare in/out.)
// API C Lua BRUTE (Lua 5.1, __cdecl) extraite du wrapper Lua_CallGlobal_va : permet
// d'appeler un global avec un nom STATIQUE const char* (pas de std::string BYVAL
// que le wrapper détruit => pas de heap/free/crash d'allocateur). Sert à
// GetVarOptionName(id) pour les noms de random options (dynamique = suit le lub).
constexpr uintptr_t kSkillIntStr = 0xec;     // (0x2e) std::string nom + coût SP (SSO/heap)
// Fenêtre skill (0x2e) : 2 rich-text box enfants portant les lignes de desc.
// this+0x108 (ptr box "haut" = id/max level) et this+0xb8 (ptr box "corps").
// Chaque box : std::vector<std::string> à box+0x88 (begin) / +0x8c (end),
// élément = std::string 0x18o (SSO/heap). Lignes BRUTES (markup ^RRGGBB/<..>).
constexpr uintptr_t kSkillBoxBody = 0xb8;    // ptr box corps
constexpr uintptr_t kSkillBoxHead = 0x108;   // ptr box id/max level
constexpr uintptr_t kRichLinesBegin = 0x88;  // vector<std::string> begin
constexpr uintptr_t kRichLinesEnd   = 0x8c;  // vector<std::string> end
constexpr int       kStdStringSize  = 0x18;  // taille d'un std::string MSVC élément

// Recette texture (identique à skill_bar / menu_icons).
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;

using HideNative_t   = void  (__fastcall*)(void*, void*, int);
using GetDescLines_t = char*** (__fastcall*)(void*);            // -> &vector : [0]=begin,[1]=end
using GetBaseName_t  = size_t(__thiscall*)(void*, char*, size_t*, char);
using DescLookup_t   = void* (__cdecl*)(int, void*);  // ItemSkillDescDB_Lookup
using InfoCtor_t     = void  (__fastcall*)(void*);       // ItemSkillInfo_ctor
using InfoSetId_t    = void  (__thiscall*)(void*, int);  // ItemSkillInfo_SetId
using EnsureLoaded_t = char  (__thiscall*)(void*, int);  // ItemSkillDescDB_EnsureLoaded
using GetResName_t   = char* (__fastcall*)(void*);       // ItemSkillDB_GetResName -> resname C-str
using CardResName_t  = void* (__cdecl*)(int);            // GetCardResName -> record (*record=resname)
using GameFree_t     = void  (__cdecl*)(void*);
// std::vector<int> MSVC (begin/last/end) — grossi par l'allocateur du JEU.
struct GVec { int* first; int* last; int* end; };
static_assert(sizeof(GVec) == 12, "GVec = std::vector layout");
// BuildDisplayName(this=wnd, info, &colorOut, &offsetsVec, &bufptr, &cap, &hlptr, f7, f8)
using BuildName_t = int(__thiscall*)(void*, void*, int*, GVec*, char**, size_t*,
                                     char**, char, char);
// OnMsg des fenêtres desc (vtable+0x94) : __thiscall(this, p1, msg, p3, p4, p5, p6).
using DescOnMsg_t    = int   (__thiscall*)(void*, int, int, int, int, int, int);
constexpr int kMsgButton      = 6;      // message "commande bouton"
constexpr int kCmdProbability = 0x157;  // « View Probability Info » -> wnd 0x271c
// Éligibilité du bouton Probabilité : l'item a-t-il un enregistrement dans la DB
// de probabilité (DAT_01255108, chargée depuis packageitem.lub/simplecashshop..).
// ItemProbabilityDB_Fetch(mgr, out, id) __thiscall : out[0]=record, out[1].byte=found.
constexpr uintptr_t kProbDbPtr = 0x01255108;  // ptr vers le mgr (lazy-new)
constexpr uintptr_t kProbFetch = 0x0069f480;  // ItemProbabilityDB_Fetch
using ProbFetch_t = void(__thiscall*)(void*, int*, int);
// Boutons « Read » / « Auto Read » des LIVRES (enfants natifs +0x1b8/+0x1bc de la
// fenêtre desc, libellés msg 0x50e/0x50f). Le cmd ouvre book\<id>.txt via
// ResFileStream puis MakeWindow(0x6a) + OnMsg 0x5e (RE 2026-07-27).
constexpr int kCmdReadBook     = 0x11f;  // « Read »
constexpr int kCmdAutoReadBook = 0x120;  // « Auto Read » (tourne les pages seul)
// Les 2 gates du natif (mêmes que le repositionnement des boutons à y=6, sinon
// y=-94 = hors cadre) : l'item est un LIVRE et le joueur le POSSÈDE.
// BookItemDB_Contains(id) : appartenance à g_BookItemNameMap (0x01255120, chargée
// depuis data\BookItemNameTable.txt). Inventory_OwnsItemById(id) : présent dans
// l'inventaire avec une quantité > 0.
constexpr uintptr_t kBookDbContains = 0x006a5db0;  // BookItemDB_Contains
constexpr uintptr_t kOwnsItemById   = 0x00c6b1e0;  // Inventory_OwnsItemById
using BookContains_t = char(__cdecl*)(int);
using OwnsItem_t     = char(__stdcall*)(int);

// ── Fenêtre LIVRE (UIBookWnd, id 0x6a) ───────────────────────────────────────
// Celle qu'ouvrent les boutons ci-dessus. Reproduite en ImGui (Option A : native
// cachée, données relues). Offsets VÉRIFIÉS live (x32, « Potion Creation Guide »,
// 2026-07-27). Le manager la range dans sa map générique (pas de slot fixe) ->
// détection par uiwnd::FindWindow(0x6a) + contrôle de vtable.
constexpr int       kBookWndId  = 0x6a;
constexpr uintptr_t kBookVTable = 0x0103517c;
constexpr int kBookTitle     = 0xbc;   // char[] INLINE : nom du livre (= titre natif)
constexpr int kBookLineTotal = 0x120;  // nb total de lignes déjà wrappées par le client
constexpr int kBookPageCount = 0x124;  // nb de pages
constexpr int kBookPage      = 0x128;  // page courante (1-based)
constexpr int kBookAutoFlag  = 0x134;  // byte : lecture auto EN COURS
constexpr int kBookBgR       = 0x138;  // couleur de FOND R/G/B (en-tête %RRGGBB du .txt)
constexpr int kBookBgG       = 0x13c;
constexpr int kBookBgB       = 0x140;
constexpr int kBookLines     = 0x144;  // std::vector<std::string> : begin (end à +0x148)
                                       // (élément = kStdStringSize, cf. plus haut)
constexpr int kBookLinesPerPage = 19;  // pagination native (OnMsg case 92)
// Commandes du case 6 de la fenêtre livre + message de saut de page.
constexpr int kCmdBookClose = 0xc9;   // sauve le SIGNET (msg 356) puis ferme
constexpr int kCmdBookPrev  = 0x162;  // page précédente
constexpr int kCmdBookNext  = 0x163;  // page suivante
constexpr int kMsgBookGoto  = 92;     // aller à la page param_3 (0 = signet registre)
using TexMgr_t       = void* (__cdecl*)();
using MakeKey_t      = void* (__cdecl*)(const char*);
using LoadTex_t      = void* (__fastcall*)(void*, void*, void*);
// std::string MSVC en mode HEAP (nom Lua de 16 car. = pas de SSO) : ptr + size + cap.
struct LuaStr { const char* ptr; char pad[12]; uint32_t size; uint32_t cap; };
static_assert(sizeof(LuaStr) == 0x18, "LuaStr = std::string MSVC (0x18)");
using LuaToLStr_t    = const char*(__cdecl*)(void*, int, size_t*);
using LuaSetTop_t    = void       (__cdecl*)(void*, int);
using GameMalloc_t = void* (__cdecl*)(size_t);

// Repro de la fenêtre SKILL (0x2e) : desc lue des rich-text box natifs (safe,
// pas de Lua/hook). Masquage du natif via OnTick UNIQUEMENT (PAS de hook sur
// 0x008ca900 : il crashait le chemin du msg 0x3d) -> léger flicker toléré.
constexpr bool     kSkillWindowEnabled = true;

// ── Couche serveur (ACTIVE — requiert le map-server rebuild avec 0x0F0B/0x0F0C) ─
constexpr bool     kEnableServerFetch = true;
// Zone custom SÛRE (>0x0C35, hors table client ET plage rAthena) : dispatch via
// le reader-hook (cf. RagConnection::RegisterRecvOpcode). Vérifié flag=-1.
// Source unique : features/systems/bourgeon_opcodes.h
constexpr uint16_t kOpcodeReqTechData = bopcodes::kReqTechData;  // 0x0F0B client -> serveur
constexpr uint16_t kOpcodeTechData    = bopcodes::kTechData;     // 0x0F0C serveur -> client
constexpr uint16_t kOpcodeReqDamage   = bopcodes::kReqDamage;    // 0x0F0D client -> serveur
constexpr uint16_t kOpcodeDamage      = bopcodes::kDamage;       // 0x0F0E serveur -> client
constexpr uint16_t kOpcodeReqItemScript = bopcodes::kReqItemScript;  // 0x0F11 client -> serveur
constexpr uint16_t kOpcodeItemScript    = bopcodes::kItemScript;     // 0x0F12 serveur -> client

// Les taux de drop dépendent de facteurs DYNAMIQUES côté serveur (event_drop,
// VIP, SC_ITEMBOOST, bonus de gear) : on ne peut pas cacher indéfiniment. On
// ré-interroge donc au bout de kTechTtlMs, ET à chaque réouverture de la
// fenêtre (invalidation du cache sur front montant). kTechPendingMs = délai
// avant de renvoyer une requête restée sans réponse.
constexpr uint32_t kTechTtlMs     = 15000;  // fraîcheur d'une réponse
constexpr uint32_t kTechPendingMs = 3000;   // timeout d'une requête en vol

// Icône de collection : texture + dimensions natives (pour préserver le ratio).
struct IconTex { void* tex = nullptr; int w = 0; int h = 0; };
// Cache : id -> IconTex (tex null = miss connu).
std::unordered_map<uint32_t, IconTex> g_icon_cache;

// Lit un pointeur de fenêtre valide depuis un slot manager, en vérifiant sa
// vtable. Renvoie null si slot vide ou vtable inattendue. SEH-gardé.
uint8_t* ReadValidWnd(uintptr_t slot, uintptr_t expected_vtable) {
  return uiwnd::WndAtSlot(slot, expected_vtable);
}

// c_str d'une std::string MSVC embarquée à base (SSO : heap si cap>0xf).
const char* MsvcStr(const uint8_t* base, uint32_t cap) {
  return (cap > 0xf) ? *reinterpret_cast<const char* const*>(base)
                     : reinterpret_cast<const char*>(base);
}

// Résout le .bmp de collection en pixels bruts BGRA (appels natifs, POD only).
// SEH ne peut pas contenir d'objets C++ (C2712) -> la conversion est faite hors
// __try par l'appelant.
struct RawTex { const uint8_t* bgra; int w; int h; void* ctex; };
bool GetRawTex(const char* path, RawTex* out) {
  __try {
    void* mgr = reinterpret_cast<TexMgr_t>(ro::texmgr::kGet)();
    if (!mgr) return false;
    void* key = reinterpret_cast<MakeKey_t>(ro::texmgr::kMakeKey)(path);
    if (!key) return false;
    void* t = reinterpret_cast<LoadTex_t>(ro::texmgr::kLoad)(mgr, nullptr, key);
    if (!t) return false;
    const int w = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexW);
    const int h = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexH);
    const uint8_t* bgra =
        *reinterpret_cast<const uint8_t**>(static_cast<char*>(t) + kTexPix);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !bgra) return false;
    out->bgra = bgra; out->w = w; out->h = h; out->ctex = t;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Charge le .bmp de collection (chemin déjà complet) en texture ImGui + dims.
IconTex LoadCollectionIcon(const char* path) {
  RawTex rt{};
  if (!GetRawTex(path, &rt)) return {};
  std::vector<uint8_t> argb(static_cast<size_t>(rt.w) * rt.h * 4);
  for (int i = 0; i < rt.w * rt.h; ++i) {
    const int b = rt.bgra[i * 4], g = rt.bgra[i * 4 + 1],
              r = rt.bgra[i * 4 + 2];
    // Colorkey magenta TOLÉRANT : attrape aussi la frange anti-aliasée
    // (near-magenta) visible dans les angles. Magenta = R et B élevés + proches,
    // G faible (distingue du rouge/bleu/violet pur).
    const int drb = (r > b) ? r - b : b - r;
    const bool ck = (r >= 0xC8 && b >= 0xC8 && g <= 0x38 && drb <= 0x30);
    argb[i * 4] = static_cast<uint8_t>(b);
    argb[i * 4 + 1] = static_cast<uint8_t>(g);
    argb[i * 4 + 2] = static_cast<uint8_t>(r);
    argb[i * 4 + 3] = ck ? 0 : 0xFF;
  }
  return {Overlay_CreateTextureARGB(argb.data(), rt.w, rt.h), rt.w, rt.h};
}

// Données d'un item extraites sous SEH (POD only) pour sortir avant tout ImGui.
constexpr int kMaxLines = 64;
constexpr int kLineLen  = 192;
// Une random option d'instance : type + valeur + param (offsets +0x9c du struct).
struct RdmOpt {
  int16_t index = 0;
  int16_t value = 0;
  uint8_t param = 0;
};

struct ItemExtract {
  char name[128];
  char iconpath[300];
  bool has_icon;
  int  line_count;
  char lines[kMaxLines][kLineLen];
  // Segment coloré (enchant/inconnu) du titre : [hl_start,hl_end) en hl_col.
  int      hl_start = -1;
  int      hl_end = -1;
  uint32_t hl_col = 0;  // ImU32 (0 = aucun)
  // Ombre du nom : le natif (DrawName) dessine une ombre 0x5050fa (COLORREF BGR ->
  // RGB 250,80,80 = ROUGE) sous le nom quand info+0x5d != 0 (ex. arme CASSÉE). Le
  // "nom rouge" d'un item cassé = CETTE ombre, pas la couleur du texte (RE live
  // 2026-07-08). 0 = aucune ombre.
  uint32_t name_shadow = 0;  // ImU32
  // Instance : cartes/enchants (4 slots, id + nom résolu) + random options.
  uint32_t cards[kMaxCards] = {0, 0, 0, 0};
  char     card_names[kMaxCards][64] = {};
  int      card_slots = 0;  // nb d'emplacements de carte de l'item (record+0x30)
  int      view_id = 0;     // info+0x70 (viewID sprite ; 0 = aucun aperçu)
  int      emplacement = 0; // info+0x8 (bitmask emplacement, pour le slot d'aperçu)
  int      opt_count = 0;
  RdmOpt   opts[kMaxOpts] = {};
};

// Desc d'une carte/enchant chargée par id (tooltip au survol). Lignes BRUTES
// (markup ^RRGGBB/<URL>..). Chargée une fois, mise en cache par id.
struct CardDesc {
  char name[64] = {};
  int  card_slots = 0;  // record+0x30 : nb d'emplacements de carte (0 = non sloté)
  int  line_count = 0;
  char lines[kMaxLines][kLineLen] = {};
  char icon_path[288]   = {};  // 유저인터페이스\item\<resname>.bmp (petite icône liste)
  char coll_path[288]   = {};  // 유저인터페이스\collection\<resname>.bmp (art de preview)
  char illust_path[288] = {};  // 유저인터페이스\cardBmp\<resname>.bmp (illustration tooltip)
};

// ── Encodage : du texte du CLIENT vers l'UTF-8 d'ImGui ──────────────────────
// Ces trois helpers sont ICI, avant les extracteurs, parce que TOUT texte lu
// du natif y passe : nom d'objet, lignes de description, noms de cartes
// serties, nom et description de skill, pages de livre.

// Longueur d'une séquence UTF-8 d'après son octet de tête (1 = ASCII, ou octet
// isolé/invalide qu'on laisse passer tel quel).
inline int Utf8SeqLen(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xe0) == 0xc0) return 2;
  if ((c & 0xf0) == 0xe0) return 3;
  if ((c & 0xf8) == 0xf0) return 4;
  return 1;
}

// Une chaîne est-elle purement ASCII ? Alors la convertir ne changerait pas un
// octet, et on s'épargne l'aller-retour UTF-16. C'est le cas de l'immense
// majorité des textes de ce client.
bool IsPureAscii(const char* s) {
  for (const char* p = s; *p; ++p)
    if (static_cast<unsigned char>(*p) >= 0x80) return false;
  return true;
}

// Convertit SUR PLACE un texte du jeu vers l'UTF-8 attendu par ImGui.
//
// Sans ça, tout octet >= 0x80 — les accents et les « … » des .txt de livre, mais
// aussi les descriptions et les noms restés en coréen dans la DB — est lu comme
// de l'UTF-8 invalide et sort en losanges.
//
// ⚠ La conversion ALLONGE : un caractère coréen fait 2 octets dans la code-page
// du client et 3 en UTF-8. Quand le résultat ne tient pas, on coupe sur une
// FRONTIÈRE de caractère — une coupe au milieu d'une séquence UTF-8 rendrait
// exactement les losanges qu'on cherche à supprimer.
void LocalizeInPlace(char* buf, size_t buf_size) {
  if (!buf || buf_size == 0 || IsPureAscii(buf)) return;
  const char* utf8 = ro::LocalToUtf8(buf);
  size_t n = 0;
  while (utf8[n]) {
    const int seq = Utf8SeqLen(static_cast<unsigned char>(utf8[n]));
    if (n + static_cast<size_t>(seq) > buf_size - 1) break;
    n += static_cast<size_t>(seq);
  }
  std::memcpy(buf, utf8, n);
  buf[n] = 0;
}

void LocalizeLines(char lines[][kLineLen], int count) {
  for (int i = 0; i < count; ++i) LocalizeInPlace(lines[i], kLineLen);
}

// Remplit ItemExtract depuis la fenêtre item (POD only, SEH-gardé).
bool ExtractItem(uint8_t* wnd, ItemExtract* e) {
  __try {
    // Titre COMPLET (raffinement +N / [slots] / cartes / enchant) via
    // BuildDisplayName — écrit la concat des segments dans un buffer local ; le
    // vector d'offsets est alloué par le JEU -> libéré par le free du JEU.
    {
      char  nbuf[256]; nbuf[0] = '\0';
      char* bufptr  = nbuf;
      size_t ncap   = sizeof(nbuf);
      int    colorOut = 0;
      char*  hlptr  = nullptr;
      GVec   off = {nullptr, nullptr, nullptr};
      reinterpret_cast<BuildName_t>(itemdb::kBuildDisplayNameAddr)(
          wnd, wnd + kItemStruct, &colorOut, &off, &bufptr, &ncap, &hlptr, 0, 0);
      size_t n = 0;
      while (n < sizeof(e->name) - 1 && nbuf[n]) { e->name[n] = nbuf[n]; ++n; }
      e->name[n] = '\0';
      // Le nom reste ICI en octets client : sa conversion en UTF-8 se fait plus
      // bas, une fois hs/he calculés, parce que ce sont des index d'OCTETS dans
      // nbuf — les convertir d'abord les décalerait sur tout nom non-ASCII.
      // Segment coloré (enchant/inconnu) : hlptr = début, fin = 1er offset > début.
      if (hlptr && colorOut != 0) {
        int hs = static_cast<int>(hlptr - nbuf);
        int he = static_cast<int>(bufptr - nbuf);
        for (int* p = off.first; p && p != off.last; ++p) {
          const int idx = static_cast<int>(reinterpret_cast<char*>(
                              static_cast<uintptr_t>(static_cast<uint32_t>(*p))) - nbuf);
          if (idx > hs) { he = idx; break; }
        }
        if (hs >= 0 && hs < static_cast<int>(n)) {
          if (he > static_cast<int>(n)) he = static_cast<int>(n);
          if (he > hs) {
            e->hl_start = hs;
            e->hl_end   = he;
            // colorOut est un COLORREF Win32 (BGR : octet BAS = ROUGE), pas du RGB
            // — le natif le passe tel quel à SetTextColor. Lire R dans l'octet bas,
            // B dans l'octet haut (sinon rouge/bleu inversés : le nom « rouge » des
            // items forgés/signés à nom non résolu s'affichait bleu). RE 2026-07-08.
            e->hl_col   = IM_COL32(colorOut & 0xff,
                                   (colorOut >> 8) & 0xff,
                                   (colorOut >> 16) & 0xff, 255);
          }
        }
      }
      if (off.first) reinterpret_cast<GameFree_t>(kGameFree)(off.first);
    }
    // Ombre ROUGE du nom : le natif (DrawName 0x008972c0) dessine l'ombre du nom en
    // 0x5050fa (COLORREF BGR -> RGB 250,80,80 = ROUGE) quand info+0x5d != 0 (branche
    // « else » : ni segment coloré, ni grade). C'est CE qui fait paraître le nom
    // d'une arme CASSÉE « rouge » (ombre rouge sous le texte noir), pas la couleur
    // du texte. On lit le même flag et on reproduit l'ombre — 0 sinon.
    e->name_shadow =
        (*(uint8_t*)(wnd + kItemStruct + 0x5d) != 0) ? IM_COL32(0xFA, 0x50, 0x50, 255)
                                                     : 0u;
    // Repli : nom de base si BuildDisplayName n'a rien produit.
    if (e->name[0] == '\0') {
      size_t cap = sizeof(e->name);
      reinterpret_cast<GetBaseName_t>(itemdb::kBaseNameFallbackAddr)(
          wnd + kItemStruct, e->name, &cap, 0);
    }
    // Le nom passe en UTF-8 MAINTENANT, et les bornes du segment coloré avec
    // lui : hl_start/hl_end comptent des OCTETS, et un préfixe non-ASCII n'en
    // fait plus le même nombre une fois converti. On reconvertit donc chaque
    // préfixe pour lire sa longueur d'arrivée. Nom ASCII — le cas courant — :
    // rien ne bouge, et la conversion n'a même pas lieu.
    if (!IsPureAscii(e->name)) {
      char prefix[sizeof(e->name)];
      if (e->hl_start > 0) {
        const size_t k = static_cast<size_t>(e->hl_start);
        std::memcpy(prefix, e->name, k);
        prefix[k] = '\0';
        e->hl_start = static_cast<int>(std::strlen(ro::LocalToUtf8(prefix)));
      }
      if (e->hl_end > 0) {
        const size_t k = static_cast<size_t>(e->hl_end);
        std::memcpy(prefix, e->name, k);
        prefix[k] = '\0';
        e->hl_end = static_cast<int>(std::strlen(ro::LocalToUtf8(prefix)));
      }
      LocalizeInPlace(e->name, sizeof(e->name));
    }

    const uint32_t iconcap = *reinterpret_cast<uint32_t*>(wnd + kItemIconCap);
    const uint32_t iconlen = *reinterpret_cast<uint32_t*>(wnd + kItemIconLen);
    if (iconlen > 0) {
      const char* path = MsvcStr(wnd + kItemIconPath, iconcap);
      std::strncpy(e->iconpath, path ? path : "", sizeof(e->iconpath) - 1);
      e->has_icon = e->iconpath[0] != '\0';
    }

    char*** vec =
        reinterpret_cast<GetDescLines_t>(kGetDescLines)(wnd + kItemStruct);
    if (vec) {
      char** first = vec[0];
      char** last  = vec[1];
      const ptrdiff_t n = last - first;
      if (n > 0 && n < 4096) {
        for (char** it = first; it < last && e->line_count < kMaxLines; ++it) {
          const char* line = *it;
          std::strncpy(e->lines[e->line_count], line ? line : "", kLineLen - 1);
          ++e->line_count;
        }
        LocalizeLines(e->lines, e->line_count);
      }
    }

    // Instance : cartes/enchants (4 slots) — id à info+0x1c+i*4, nom résolu via
    // la DB desc (Lookup(id) -> rec+4). id 0 = slot vide.
    uint8_t* info = wnd + kItemStruct;
    // ⚠️ Item FORGÉ/CRÉÉ (card[0] = marqueur 0xFF/0xFE, cf. ItemInfo_IsForgedOrCreated
    // 0x006a5e30) : les slots de carte contiennent les DONNÉES DU CRÉATEUR (charid
    // scindé, star crumbs, élément), PAS des cartes. On ne les lit donc PAS comme
    // cartes (sinon #255 = 0xFF, #18928 = un bout de charid s'affichent), et on
    // masque emplacements + suffixe [N]. Détection : card[0] PETIT NON NUL (<= 500 ;
    // les vraies cartes/enchants ont un id > 500). card[0]==0 = slots VIDES d'un item
    // normal (ex. Rapier) -> on garde l'affichage des emplacements vides + [N].
    const uint32_t card0 = *reinterpret_cast<uint32_t*>(info + kInfoCards);
    const bool forged = (card0 != 0 && card0 <= 500);
    for (int i = 0; i < kMaxCards; ++i) {
      const uint32_t cid =
          forged ? 0u : *reinterpret_cast<uint32_t*>(info + kInfoCards + i * 4);
      e->cards[i] = cid;
      e->card_names[i][0] = '\0';
      if (cid != 0) {
        void* rec = reinterpret_cast<DescLookup_t>(itemdb::kLookupAddr)(
            static_cast<int>(cid), reinterpret_cast<void*>(itemdb::kTableAddr));
        if (rec && rec != reinterpret_cast<void*>(itemdb::kNilAddr)) {
          const char* nm = *reinterpret_cast<char**>(
              reinterpret_cast<char*>(rec) + 4);
          if (nm) {
            std::strncpy(e->card_names[i], nm, sizeof(e->card_names[i]) - 1);
            e->card_names[i][sizeof(e->card_names[i]) - 1] = '\0';
            LocalizeInPlace(e->card_names[i], sizeof(e->card_names[i]));
          }
        }
      }
    }

    // Nombre d'emplacements de carte de l'item : record+0x30 de la DB desc (le
    // natif le formate " [%d]" via FUN_006a4c40 ; 0 = non-sloté). Guard 1..4.
    e->card_slots = 0;
    {
      const char* idstr =
          MsvcStr(info + 0x2c, *reinterpret_cast<uint32_t*>(info + 0x40));
      const int item_id = idstr ? atoi(idstr) : 0;
      if (!forged && item_id > 0) {  // forgé -> pas d'emplacements ni de suffixe [N]
        void* rec = reinterpret_cast<DescLookup_t>(itemdb::kLookupAddr)(
            item_id, reinterpret_cast<void*>(itemdb::kTableAddr));
        if (rec && rec != reinterpret_cast<void*>(itemdb::kNilAddr)) {
          const int sc =
              *reinterpret_cast<int*>(reinterpret_cast<char*>(rec) + 0x30);
          if (sc > 0 && sc <= kMaxCards) e->card_slots = sc;
        }
      }
    }

    // viewID (info+0x70) + emplacement (info+0x8) : pour l'aperçu 3D au survol.
    e->view_id     = *reinterpret_cast<int*>(info + 0x70);
    e->emplacement = *reinterpret_cast<int*>(info + 0x8);

    // Suffixe " [N]" dans le nom (comme le natif FUN_006a4c40), si l'item a des
    // emplacements. e->name est déjà le nom complet construit par BuildDisplayName.
    if (e->card_slots > 0) {
      const size_t nl = strnlen(e->name, sizeof(e->name));
      std::snprintf(e->name + nl, sizeof(e->name) - nl, " [%d]", e->card_slots);
    }

    // Random options : count à info+0x98, entrées 5o à info+0x9c+i*5.
    int oc = *reinterpret_cast<int*>(info + kInfoOptCount);
    if (oc < 0) oc = 0;
    if (oc > kMaxOpts) oc = kMaxOpts;
    e->opt_count = oc;
    for (int i = 0; i < oc; ++i) {
      uint8_t* ob = info + kInfoOpts + i * 5;
      e->opts[i].index = *reinterpret_cast<int16_t*>(ob + 0);
      e->opts[i].value = *reinterpret_cast<int16_t*>(ob + 2);
      e->opts[i].param = *(ob + 4);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Insère une fermeture de couleur ^000000 en tête de `line` (buffer de kLineLen) :
// marque le début d'un nouveau FLUX de couleur, la couleur laissée ouverte par les
// lignes précédentes ne déborde donc pas dessus.
inline void PrefixColorReset(char* line) {
  const size_t len = strnlen(line, kLineLen);
  if (len + 7 >= kLineLen) return;  // pas la place : on laisse tel quel
  std::memmove(line + 7, line, len + 1);
  std::memcpy(line, "^000000", 7);
}

// Lit les lignes d'un rich-text box natif (std::vector<std::string> @ box+0x88)
// et les ajoute à e->lines (lignes BRUTES avec markup ^RRGGBB/<..>). SEH (POD).
void ReadRichTextLines(uint8_t* box, ItemExtract* e) {
  if (!box) return;
  __try {
    uint8_t* first = *reinterpret_cast<uint8_t**>(box + kRichLinesBegin);
    uint8_t* last  = *reinterpret_cast<uint8_t**>(box + kRichLinesEnd);
    if (!first || last <= first) return;
    const ptrdiff_t bytes = last - first;
    if (bytes <= 0 || bytes > kStdStringSize * 512) return;  // garde
    for (uint8_t* el = first;
         el + kStdStringSize <= last && e->line_count < kMaxLines;
         el += kStdStringSize) {
      const uint32_t cap = *reinterpret_cast<uint32_t*>(el + 0x14);
      const char* s = MsvcStr(el, cap);  // SSO si cap<=0xf, sinon heap
      char* dst = e->lines[e->line_count];
      int i = 0;
      while (i < kLineLen - 1 && s && s[i]) { dst[i] = s[i]; ++i; }
      dst[i] = '\0';
      ++e->line_count;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Lit le nom+SP (this+0xec) sous SEH (POD only).
void ReadSkillName(uint8_t* wnd, char* out, int outsz) {
  out[0] = '\0';
  __try {
    const uint32_t cap = *reinterpret_cast<uint32_t*>(wnd + kSkillIntStr + 0x14);
    const char* nm = MsvcStr(wnd + kSkillIntStr, cap);
    int i = 0;
    while (i < outsz - 1 && nm && nm[i]) { out[i] = nm[i]; ++i; }
    out[i] = '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// Remplit ItemExtract pour un SKILL : nom+SP (+0xec) + description lue DIRECTEMENT
// des rich-text box natifs (déjà remplis par le jeu, aucune Lua/hook -> pas de
// crash). Ordre : box corps (+0xb8) puis box id/max-level (+0x108). SEH via les
// helpers. (Le paramètre id n'est plus utilisé mais conservé pour la signature.)
void ExtractSkill(uint8_t* wnd, uint32_t /*id*/, ItemExtract* e) {
  *e = ItemExtract{};
  ReadSkillName(wnd, e->name, sizeof(e->name));
  LocalizeInPlace(e->name, sizeof(e->name));
  __try {
    uint8_t* body = *reinterpret_cast<uint8_t**>(wnd + kSkillBoxBody);
    uint8_t* head = *reinterpret_cast<uint8_t**>(wnd + kSkillBoxHead);
    ReadRichTextLines(body, e);
    // Chaque box est un flux de couleur INDÉPENDANT : une couleur laissée ouverte par
    // la box corps ne doit pas déborder sur la box id/max-level (qui est rendue à la
    // suite dans le même bloc ImGui) -> fermeture explicite sur sa 1re ligne.
    const int head_first = e->line_count;
    ReadRichTextLines(head, e);
    if (e->line_count > head_first) PrefixColorReset(e->lines[head_first]);
    LocalizeLines(e->lines, e->line_count);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// 2 hex -> octet ; -1 si non-hexa.
inline int Hex2(const char* p) {
  auto v = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  const int hi = v(p[0]), lo = v(p[1]);
  return (hi < 0 || lo < 0) ? -1 : (hi << 4 | lo);
}

// std::string MSVC (SSO) telle que passée PAR VALEUR à la fonction de routage
// (layout confirmé par capture live : buf[16] + size + cap = 0x18 octets).
struct RoStr { char buf[16]; uint32_t size; uint32_t cap; };
static_assert(sizeof(RoStr) == 0x18, "RoStr doit matcher std::string MSVC (0x18)");
using NaviRoute_t =
    char(__thiscall*)(void*, RoStr, int, int, int, int, int, int);

// Déclenche le routage de navigation vers (map, x, y). SEH (POD only).
void StartNavigation(const char* map, int x, int y, int type) {
  __try {
    RoStr s;
    std::memset(&s, 0, sizeof(s));
    size_t n = 0;
    while (n < 15 && map[n]) { s.buf[n] = map[n]; ++n; }
    s.size = static_cast<uint32_t>(n);
    s.cap  = 15;  // SSO
    // flags=1, a24=1, a30=0 : constantes observées pour un lien navi item.
    reinterpret_cast<NaviRoute_t>(kNaviRoute)(
        reinterpret_cast<void*>(kNaviMgr), s, type, 1, 1, x, y, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Pivot ImGui (0..1) de l'ancrage souris de la desc. L'ancrage = la DIRECTION dans
// laquelle la fenêtre s'étend depuis le curseur (ex. « bas-droite » = la fenêtre
// descend à droite du curseur = son coin HAUT-GAUCHE est posé sur le curseur =
// pivot 0,0). Le pivot est donc le coin OPPOSÉ à la direction nommée.
static ImVec2 DescAnchorPivot(int a) {
  switch (a) {
    case 1:  return ImVec2(0.0f, 1.0f);  // haut-droite  -> fenêtre en haut à droite
    case 2:  return ImVec2(1.0f, 0.0f);  // bas-gauche
    case 3:  return ImVec2(0.0f, 0.0f);  // bas-droite (classique : descend à droite)
    case 4:  return ImVec2(0.5f, 0.5f);  // centre (sur le curseur)
    default: return ImVec2(1.0f, 1.0f);  // haut-gauche
  }
}

// Dessine le titre avec le segment [hs,he) coloré en hlcol (reste en def). `shadow`
// != 0 => ombre du nom entier décalée +1,+1 SOUS le texte (natif : 0x5050fa rouge
// quand info+0x5d!=0, ex. arme cassée).
void DrawTitle(const char* name, int hs, int he, ImU32 hlcol, ImU32 def,
               ImU32 shadow = 0) {
  if (!name || !name[0]) { ImGui::TextUnformatted("(?)"); return; }
  if (shadow) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 1.0f, p.y + 1.0f), shadow, name);
  }
  const int len = static_cast<int>(std::strlen(name));
  if (hs < 0 || he <= hs || hs >= len) {
    ImGui::PushStyleColor(ImGuiCol_Text, def);
    ImGui::TextUnformatted(name);
    ImGui::PopStyleColor();
    return;
  }
  if (he > len) he = len;
  bool first = true;
  auto span = [&](int a, int b, ImU32 c) {
    if (b <= a) return;
    if (!first) ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextUnformatted(name + a, name + b);
    ImGui::PopStyleColor();
    first = false;
  };
  span(0, hs, def);
  span(hs, he, hlcol);
  span(he, len, def);
}

// ── Bloc de texte coloré SÉLECTIONNABLE + copiable (Ctrl+C) ────────────────
// ImGui ne sait pas sélectionner du texte coloré : on écrit un mini moteur.
// Layout caractère par caractère (tokens ^RRGGBB + wrap mot à mot), hit-test
// souris -> index dans un buffer texte plat, surbrillance de la sélection, et
// Ctrl+C / Ctrl+A. État de sélection persistant par bloc (clé = ImGuiID).
struct SelState { int anchor = -1, head = -1; };
std::unordered_map<ImGuiID, SelState> g_sel;
ImGuiID g_active_sel = 0;  // dernier bloc où une sélection a démarré

// Longueur VISIBLE d'une ligne (ignore les tokens couleur ^RRGGBB) + dernier caractère
// visible. Sert au reflow skill : détecter les lignes « pleines » (== colonne de wrap
// natif -> continuation coupée) et distinguer une fin de phrase d'une coupure.
inline void AnalyzeDescLine(const char* s, int* out_len, char* out_last) {
  int n = 0;
  char last = '\0';
  for (const char* p = s; *p;) {
    if (p[0] == '^' && Hex2(p + 1) >= 0 && Hex2(p + 3) >= 0 && Hex2(p + 5) >= 0) {
      p += 7;  // token ^RRGGBB : non visible
      continue;
    }
    last = *p;
    ++n;
    ++p;
  }
  if (out_len) *out_len = n;
  if (out_last) *out_last = last;
}

// Premier caractère VISIBLE d'une ligne (ignore les tokens couleur ^RRGGBB), '\0'
// si la ligne n'a aucun texte. Sert au reflow skill : le wrap natif coupe à la
// largeur PIXEL, donc EN PLEIN MOT -> la ligne de continuation reprend le mot coupé
// et commence par une lettre minuscule. Un nouveau bloc écrit par l'auteur du .lub
// commence par tout autre chose (« [Lv 2] », « Comments: », ligne vide).
inline char FirstVisibleChar(const char* s) {
  for (const char* p = s; *p;) {
    if (p[0] == '^' && Hex2(p + 1) >= 0 && Hex2(p + 3) >= 0 && Hex2(p + 5) >= 0) {
      p += 7;  // token couleur : non visible
      continue;
    }
    return *p;
  }
  return '\0';
}


// reflow=true (skills) : les lignes viennent des rich-text box natifs DÉJÀ wrappés par
// le jeu, qui coupe en plein mot (il compte les tokens ^RRGGBB dans la largeur) -> on
// fusionne les lignes « pleines » (== colonne de wrap) non terminées par une ponctuation
// de phrase avec la suivante, puis on re-wrappe à la largeur ImGui. reflow=false
// (items/cartes) : lignes DB brutes déjà propres -> saut de ligne forcé par ligne.
void SelectableColoredText(const char* id, const char lines[][kLineLen],
                           int count, ImU32 default_col,
                           float wrap_override = 0.0f, bool reflow = false) {
  ImDrawList* dl   = ImGui::GetWindowDrawList();
  ImFont*     font = ImGui::GetFont();
  const float fsz    = ImGui::GetFontSize();
  const float lineH  = ImGui::GetTextLineHeightWithSpacing();
  const float spaceW = ImGui::CalcTextSize(" ").x;
  float wrap = wrap_override > 0.0f ? wrap_override
                                    : ImGui::GetContentRegionAvail().x;
  if (wrap < 1.0f) wrap = 1.0f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();

  const ImU32 kLinkUrl  = IM_COL32(64, 132, 224, 255);  // liens <URL>  (bleu)
  const ImU32 kLinkNavi = IM_COL32(60, 175, 90, 255);   // liens <NAVI> (vert)

  // Une « boîte » = UN glyphe affiché. `len` = sa taille en octets dans `buf` : un
  // caractère non-ASCII (UTF-8 multi-octets) reste un seul glyphe, sinon il serait
  // dessiné et sélectionné octet par octet (= losanges).
  struct Box { float x0, x1, y; int row; int idx; int len; ImU32 col; int link; };
  static std::vector<Box> boxes;   // réutilisés (UI mono-thread)
  static std::string buf;
  static std::vector<std::string> hrefs;  // charge des liens (index = Box.link)
  static std::vector<int> linkKind;       // 0 = URL, 1 = NAVI (index = Box.link)
  boxes.clear();
  buf.clear();
  hrefs.clear();
  linkKind.clear();

  float penX = 0.0f, penY = 0.0f;
  int   row = 0;
  int   curLink = -1;              // lien courant (index dans hrefs) ou -1
  auto putVisible = [&](const char* s, int len, float w, ImU32 col) {
    boxes.push_back({p0.x + penX, p0.x + penX + w, p0.y + penY, row,
                     static_cast<int>(buf.size()), len, col, curLink});
    buf.append(s, static_cast<size_t>(len));
    penX += w;
  };

  auto tag = [](const char* p, const char* t) {
    return std::strncmp(p, t, std::strlen(t)) == 0;
  };

  // Colonne de wrap (largeur visible max parmi les lignes) : une ligne « pleine »
  // (== cette colonne) a été coupée par le wrap natif -> à re-fusionner (reflow skill).
  int wrapCol = 0;
  for (int li = 0; li < count; ++li) {
    int v = 0;
    AnalyzeDescLine(lines[li], &v, nullptr);
    if (v > wrapCol) wrapCol = v;
  }

  // État PERSISTANT à travers une fusion de lignes (lien/mot en cours) : reset seulement
  // en vraie fin de paragraphe (sinon la continuation perdrait son lien ou couperait le
  // mot fusionné). `cur` (couleur) est persistant à travers TOUTES les lignes du bloc :
  // le rich-text natif traite la source comme un FLUX, un ^RRGGBB ouvert reste actif sur
  // les lignes suivantes jusqu'à ^000000 (les .lub écrivent la couleur une seule fois, en
  // tête de paragraphe, et la ferment plusieurs lignes plus bas).
  ImU32       cur = default_col;
  bool        inHref = false;   // on lit l'URL entre <INFO> et </INFO> (non affichée)
  std::string word;
  // Un flush est déclenché aussi bien par un vrai espace que par un token ^RRGGBB /
  // une balise EN PLEIN MOT (« Knight^777777s »). Seul le premier doit produire un
  // séparateur : sans ce drapeau, chaque token couleur intercalé injectait un espace
  // parasite (« Knight s »). Posé UNIQUEMENT à la consommation d'un ' ' de la source.
  bool        pendingSpace = false;
  auto flushWord = [&](ImU32 c) {
    if (word.empty()) return;   // early-out : ne consomme pas `pendingSpace`
    ImU32 useCol = c;
    if (curLink >= 0)
      useCol = (linkKind[curLink] == 1) ? kLinkNavi : kLinkUrl;
    const float ww = ImGui::CalcTextSize(word.c_str()).x;
    if (penX > 0.0f) {
      // Le mot doit wrapper même sans séparateur en attente (suite d'un mot coupé par
      // un token couleur qui déborde) -> le test de largeur est indépendant.
      const float sep = pendingSpace ? spaceW : 0.0f;
      if (penX + sep + ww > wrap) {           // wrap doux -> espace logique
        if (pendingSpace) buf.push_back(' ');
        penX = 0.0f; penY += lineH; ++row;
      } else if (pendingSpace) {
        putVisible(" ", 1, spaceW, useCol);   // espace visible entre mots
      }
    }
    pendingSpace = false;
    // Découpe par CARACTÈRE (séquence UTF-8), pas par octet.
    for (size_t i = 0; i < word.size();) {
      int len = Utf8SeqLen(static_cast<unsigned char>(word[i]));
      if (i + static_cast<size_t>(len) > word.size())
        len = static_cast<int>(word.size() - i);
      const char* seq = word.c_str() + i;
      putVisible(seq, len,
                 ImGui::CalcTextSize(seq, seq + len).x, useCol);
      i += static_cast<size_t>(len);
    }
    word.clear();
  };

  for (int li = 0; li < count; ++li) {
    const char* s = lines[li];
    for (const char* p = s; *p;) {
      // Balises RO d'hyperlien : <URL>affichage<INFO>href</INFO></URL> et
      // <NAVI>affichage<INFO>map,x,y,...</INFO></NAVI>.
      if (tag(p, "<URL>"))   { flushWord(cur); curLink = static_cast<int>(hrefs.size()); hrefs.emplace_back(); linkKind.push_back(0); p += 5; continue; }
      if (tag(p, "</URL>"))  { flushWord(cur); curLink = -1; p += 6; continue; }
      if (tag(p, "<NAVI>"))  { flushWord(cur); curLink = static_cast<int>(hrefs.size()); hrefs.emplace_back(); linkKind.push_back(1); p += 6; continue; }
      if (tag(p, "</NAVI>")) { flushWord(cur); curLink = -1; p += 7; continue; }
      if (tag(p, "<INFO>"))  { flushWord(cur); inHref = true;  p += 6; continue; }
      if (tag(p, "</INFO>")) { inHref = false; p += 7; continue; }
      if (inHref) { if (curLink >= 0) hrefs[curLink].push_back(*p); ++p; continue; }

      if (p[0] == '^') {
        const int r = Hex2(p + 1), g = Hex2(p + 3), b = Hex2(p + 5);
        if (r >= 0 && g >= 0 && b >= 0) {
          flushWord(cur);
          cur = (r == 0 && g == 0 && b == 0) ? default_col
                                             : IM_COL32(r, g, b, 255);
          p += 7;
          continue;
        }
      }
      if (*p == ' ') { flushWord(cur); pendingSpace = true; ++p; }
      else           { word.push_back(*p); ++p; }
    }
    // Ligne PLEINE (== colonne de wrap) NON terminée par une ponctuation de phrase et
    // suivie d'au moins une ligne = coupure de continuation du wrap natif -> on NE coupe
    // PAS : le mot en cours + la couleur continuent sur la ligne suivante, l'ensemble est
    // re-wrappé à la largeur ImGui (répare « Sle|ep »). Sinon = vraie fin de paragraphe.
    int vlen = 0; char last = '\0';
    AnalyzeDescLine(s, &vlen, &last);
    const bool sentence_end = (last == '.' || last == '!' || last == '?');
    // La seule longueur ne suffit PAS à distinguer une coupure du wrap natif d'une
    // ligne d'auteur qui atteint fortuitement la même colonne : dans « [Lv 1]: 300%
    // damage, 0.8 sec After Cast Delay », chaque niveau fait pile la longueur max du
    // bloc et se retrouvait recollé au suivant. On exige donc la SIGNATURE d'une
    // coupure en plein mot : la ligne finit sur un caractère de mot ET la suivante
    // reprend en minuscule (suite du mot). Un « [Lv 2] », un « Comments: » ou une
    // ligne vide gardent leur saut de ligne.
    const bool word_cut = (last >= 'a' && last <= 'z') ||
                          (last >= 'A' && last <= 'Z') ||
                          (last >= '0' && last <= '9');
    const char next_first =
        (li + 1 < count) ? FirstVisibleChar(lines[li + 1]) : '\0';
    const bool word_continues = (next_first >= 'a' && next_first <= 'z');
    const bool merge = reflow && (li + 1 < count) && wrapCol >= 40 &&
                       vlen >= wrapCol && !sentence_end && word_cut &&
                       word_continues;
    if (merge) continue;          // fusionne : garde word/cur/curLink/inHref/penX
    flushWord(cur);
    curLink = -1;
    inHref  = false;
    // `cur` N'EST PAS réinitialisé ici : la couleur en cours se propage à la ligne
    // suivante, comme dans le rich-text natif (fermeture explicite par ^000000).
    buf.push_back('\n');            // fin de ligne (dans le buffer, non visible)
    penX = 0.0f; penY += lineH; ++row;
    pendingSpace = false;           // le '\n' fait office de séparateur
  }
  flushWord(cur);   // garde-fou : résidu si la dernière ligne était « pleine »
  float totalH = penY;
  if (totalH < lineH) totalH = lineH;

  // Zone interactive (capture souris) couvrant tout le bloc.
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, ImVec2(wrap, totalH),
                         ImGuiButtonFlags_MouseButtonLeft);
  const ImGuiID wid     = ImGui::GetItemID();
  const bool    hovered = ImGui::IsItemHovered();
  const bool    active  = ImGui::IsItemActive();

  // Hit-test souris -> index dans buf.
  auto hitTest = [&](ImVec2 m) -> int {
    if (boxes.empty()) return 0;
    int r = static_cast<int>((m.y - p0.y) / lineH);
    if (r < 0) r = 0;
    int result = static_cast<int>(buf.size());
    bool rowHas = false;
    for (const Box& bx : boxes) {
      if (bx.row != r) continue;
      rowHas = true;
      if (m.x < bx.x0) { result = bx.idx; break; }
      if (m.x <= bx.x1) {
        result = (m.x < (bx.x0 + bx.x1) * 0.5f) ? bx.idx : bx.idx + bx.len;
        break;
      }
      result = bx.idx + bx.len;  // après ce glyphe (continue -> fin de ligne)
    }
    if (!rowHas) {  // ligne vide : cale sur le 1er glyphe d'une ligne >= r
      for (const Box& bx : boxes)
        if (bx.row >= r) { result = bx.idx; break; }
    }
    return result;
  };

  // Index du lien sous la souris (ou -1).
  auto linkAt = [&](ImVec2 m) -> int {
    const int r = static_cast<int>((m.y - p0.y) / lineH);
    if (r < 0) return -1;
    for (const Box& bx : boxes) {
      if (bx.row != r) continue;
      if (m.x >= bx.x0 && m.x <= bx.x1 && bx.link >= 0 &&
          bx.link < static_cast<int>(hrefs.size()))
        return bx.link;
    }
    return -1;
  };

  SelState& st = g_sel[wid];
  if (active) {
    const int idx = hitTest(ImGui::GetIO().MousePos);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { st.anchor = idx; st.head = idx; }
    else st.head = idx;
    g_active_sel = wid;
  }
  // Clamp (le contenu peut avoir changé depuis la dernière frame).
  const int bufLen = static_cast<int>(buf.size());
  if (st.anchor > bufLen) st.anchor = bufLen;
  if (st.head   > bufLen) st.head   = bufLen;
  int lo = (st.anchor < st.head) ? st.anchor : st.head;
  int hi = (st.anchor < st.head) ? st.head : st.anchor;
  if (st.anchor < 0 || st.head < 0) { lo = hi = 0; }

  // Ctrl+A (tout sélectionner) / Ctrl+C (copier) sur le bloc actif.
  const ImGuiIO& io = ImGui::GetIO();
  if ((active || g_active_sel == wid) && io.KeyCtrl) {
    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
      st.anchor = 0; st.head = bufLen; lo = 0; hi = bufLen;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_C, false) && hi > lo) {
      ImGui::SetClipboardText(buf.substr(lo, hi - lo).c_str());
    }
  }

  // Liens : curseur main au survol ; clic simple (pas un drag de sélection).
  //  - <URL>  : ouvre l'URL http(s) dans le navigateur.
  //  - <NAVI> : tooltip destination (routage natif = TODO, cf. FUN_00b314f0).
  if (hovered) {
    const int lk = linkAt(io.MousePos);
    if (lk >= 0 && lk < static_cast<int>(hrefs.size())) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      const std::string& payload = hrefs[lk];
      const bool clicked = ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                           st.anchor == st.head;
      if (linkKind[lk] == 0) {  // URL
        if (clicked && std::strncmp(payload.c_str(), "http", 4) == 0)
          ShellExecuteA(nullptr, "open", payload.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
      } else {                  // NAVI : "map,f1=x,f2=y,f3=type,..."
        char mapn[64] = {0};
        int nx = 0, ny = 0, ntype = 0;
        const char* s = payload.c_str();
        const char* c1 = std::strchr(s, ',');
        if (c1) {
          const int mlen = static_cast<int>(c1 - s) < 63 ? static_cast<int>(c1 - s) : 63;
          std::memcpy(mapn, s, static_cast<size_t>(mlen));
          nx = std::atoi(c1 + 1);
          const char* c2 = std::strchr(c1 + 1, ',');
          if (c2) {
            ny = std::atoi(c2 + 1);
            const char* c3 = std::strchr(c2 + 1, ',');
            if (c3) ntype = std::atoi(c3 + 1);
          }
        }
        // Texte blanc forcé : la couleur de texte poussée (noir, pour le fond
        // clair de la fenêtre) rendrait le tooltip invisible sur fond sombre.
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        ImGui::SetTooltip("Aller à : %s (%d, %d)", mapn, nx, ny);
        ImGui::PopStyleColor();
        if (clicked && mapn[0])
          StartNavigation(mapn, nx, ny, ntype);  // routage natif (ABI capturée live)
      }
    }
  }

  // Surbrillance de la sélection (sous le texte).
  if (hi > lo) {
    const ImU32 selc = IM_COL32(60, 110, 220, 90);
    for (const Box& bx : boxes)
      if (bx.idx >= lo && bx.idx < hi)
        dl->AddRectFilled(ImVec2(bx.x0, bx.y), ImVec2(bx.x1, bx.y + lineH), selc);
  }
  // Texte (par glyphe, couleur d'origine) + soulignement des liens.
  for (const Box& bx : boxes) {
    const char* c = &buf[static_cast<size_t>(bx.idx)];
    dl->AddText(font, fsz, ImVec2(bx.x0, bx.y), bx.col, c, c + bx.len);
    if (bx.link >= 0)
      dl->AddLine(ImVec2(bx.x0, bx.y + fsz), ImVec2(bx.x1, bx.y + fsz), bx.col);
  }
}

// L'item a-t-il des données de probabilité (=> bouton Probabilité éligible) ?
// SEH (POD only). false si la DB n'est pas encore allouée / id absent.
bool ItemHasProbability(uint32_t id) {
  bool found = false;
  __try {
    void* mgr = *reinterpret_cast<void**>(kProbDbPtr);
    if (!mgr) return false;
    int out[2] = {0, 0};
    reinterpret_cast<ProbFetch_t>(kProbFetch)(mgr, out, static_cast<int>(id));
    found = (out[1] & 0xff) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { found = false; }
  return found;
}

// L'item est-il un LIVRE lisible ET possédé (=> boutons « Lire » éligibles) ?
// Reproduit à l'identique les 2 gates natifs. Le test de possession parcourt
// l'inventaire (recherche linéaire + copie d'ItemSkillInfo) : trop lourd pour
// chaque frame -> résultat mémorisé ~500 ms pour l'item courant (assez court pour
// suivre un drop/consommation du livre). SEH (POD only).
bool ItemIsReadableBook(uint32_t id) {
  static uint32_t s_id = 0;
  static uint32_t s_checked_at = 0;
  static bool     s_readable = false;
  const uint32_t now = GetTickCount();
  if (id == s_id && (now - s_checked_at) < 500) return s_readable;
  bool readable = false;
  __try {
    const int item_id = static_cast<int>(id);
    if (reinterpret_cast<BookContains_t>(kBookDbContains)(item_id))
      readable = reinterpret_cast<OwnsItem_t>(kOwnsItemById)(item_id) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { readable = false; }
  s_id = id;
  s_checked_at = now;
  s_readable = readable;
  return readable;
}

// ── Fenêtre LIVRE : accès natif ──────────────────────────────────────────────
// La fenêtre livre si elle est ouverte, sinon null. Contrairement aux descs, elle
// n'a pas de slot manager dédié (FindWindow tombe sur la map générique). SEH.
uint8_t* FindBookWindow() {
  __try {
    auto* wnd = static_cast<uint8_t*>(uiwnd::FindWindow(kBookWndId));
    if (wnd == nullptr) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(wnd) != kBookVTable) return nullptr;
    return wnd;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Cache le rendu natif de la fenêtre livre sans la détruire (elle reste la source
// des données et le destinataire des commandes de page). SEH.
void HideBookWindow(uint8_t* wnd) {
  if (!wnd) return;
  __try { reinterpret_cast<HideNative_t>(kHideNative)(wnd, nullptr, 0); }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Rend la fenêtre au client. Sans ça, désactiver le panneau moderne pendant
// qu'un livre est ouvert le laisserait invisible (on l'avait masqué, et le natif
// ne se ré-affiche qu'au chargement d'une page). SEH.
void ShowBookWindow(uint8_t* wnd) {
  if (!wnd) return;
  __try { reinterpret_cast<HideNative_t>(kHideNative)(wnd, nullptr, 1); }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Envoie un message à la fenêtre livre. Le natif se les envoie avec param_1 = this
// (cf. case 94 : `OnMsg(a1, a1, 92, page, …)`) -> on fait pareil. SEH.
void CallBookMsg(uint8_t* wnd, int msg, int p3) {
  if (!wnd) return;
  __try {
    void** vt = *reinterpret_cast<void***>(wnd);
    auto onmsg = reinterpret_cast<DescOnMsg_t>(vt[uiwnd::kVfOnMsg / 4]);
    onmsg(wnd, static_cast<int>(reinterpret_cast<uintptr_t>(wnd)), msg, p3, 0, 0,
          0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Une page de livre extraite du natif : ses lignes (déjà wrappées par le client,
// markup ^RRGGBB inclus) + la couleur de fond déclarée par le fichier.
struct BookExtract {
  int      page = 1;
  int      pages = 1;
  int      line_count = 0;
  char     lines[kBookLinesPerPage][kLineLen] = {};
  uint32_t bg = IM_COL32(250, 240, 230, 255);
};

// Remplit BookExtract depuis la fenêtre native. La page courante est relue à
// CHAQUE frame (pas depuis le snapshot d'OnTick, qui est throttlé : après un
// changement de page l'affichage doit suivre immédiatement). Le vector natif
// porte TOUTES les lignes du livre ; la page N couvre [19*(N-1), 19*N).
// POD only (SEH).
bool ExtractBookPage(uint8_t* wnd, BookExtract* e) {
  __try {
    int page = *reinterpret_cast<int*>(wnd + kBookPage);
    if (page < 1) page = 1;
    e->page  = page;
    e->pages = *reinterpret_cast<int*>(wnd + kBookPageCount);
    if (e->pages < 1) e->pages = 1;
    e->bg = IM_COL32(*reinterpret_cast<int*>(wnd + kBookBgR) & 0xff,
                     *reinterpret_cast<int*>(wnd + kBookBgG) & 0xff,
                     *reinterpret_cast<int*>(wnd + kBookBgB) & 0xff, 255);
    auto* first = *reinterpret_cast<uint8_t**>(wnd + kBookLines);
    auto* last  = *reinterpret_cast<uint8_t**>(wnd + kBookLines + 4);
    if (!first || !last || last < first) return false;
    const ptrdiff_t capacity = (last - first) / kStdStringSize;
    if (capacity <= 0 || capacity > 100000) return false;
    // Le compteur natif (+0x120) fait foi : il exclut la ligne d'en-tête %RRGGBB
    // que OnMsg retire du vecteur.
    int total = *reinterpret_cast<int*>(wnd + kBookLineTotal);
    if (total > static_cast<int>(capacity)) total = static_cast<int>(capacity);
    const int start = (e->page - 1) * kBookLinesPerPage;
    for (int i = 0; i < kBookLinesPerPage; ++i) {
      const int idx = start + i;
      if (idx < 0 || idx >= total) break;
      const uint8_t* str = first + idx * kStdStringSize;
      const uint32_t cap = *reinterpret_cast<const uint32_t*>(str + 0x14);
      const char* txt = MsvcStr(str, cap);
      std::strncpy(e->lines[e->line_count], txt ? txt : "", kLineLen - 1);
      ++e->line_count;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Déclenche une commande bouton (msg 6) sur une fenêtre desc via son OnMsg. SEH.
// Utilisé pour rejouer le bouton natif « Probabilité » (ouvre la fenêtre 0x271c).
void CallDescButton(uint8_t* wnd, int cmd) {
  if (!wnd) return;
  __try {
    void** vt = *reinterpret_cast<void***>(wnd);
    auto onmsg = reinterpret_cast<DescOnMsg_t>(vt[uiwnd::kVfOnMsg / 4]);
    onmsg(wnd, 0, kMsgButton, cmd, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Cache des descs de cartes/enchants (clé = id). Chargé une fois via LoadCardDesc.
std::unordered_map<uint32_t, CardDesc> g_card_desc_cache;
// Caches texture séparés (clé = id) : petite icône item, art de collection,
// illustration cardBmp. Trois images DIFFÉRENTES pour un même item — la vignette
// des descriptions les essaie dans l'ordre inverse (cf. RenderSimpleDesc).
std::unordered_map<uint32_t, IconTex> g_card_icon_cache;
std::unordered_map<uint32_t, IconTex> g_card_collection_cache;
std::unordered_map<uint32_t, IconTex> g_card_illust_cache;

// Toutes les textures ci-dessus vivent en D3DPOOL_DEFAULT : mortes après un
// reset/recréation du device. On jette les 3 caches quand l'epoch change, sinon
// AddImage() sur un handle mort plante dans ddraw. (g_card_desc_cache = data pure,
// pas de texture -> conservé.) À appeler en tête de chaque résolveur d'icône.
void IconCachesGuard() {
  static unsigned s_epoch = 0;
  const unsigned e = Overlay_DeviceEpoch();
  if (e == s_epoch) return;
  g_icon_cache.clear();
  g_card_icon_cache.clear();
  g_card_collection_cache.clear();
  g_card_illust_cache.clear();
  s_epoch = e;
}
// Préfixes CP949 (2 littéraux concaténés pour éviter que \xba avale le \ suivant).
// L'item et sa collection portent le MÊME resname, dans deux dossiers : « item\ »
// pour la petite icône d'inventaire, « collection\ » pour l'art de preview.
static const char kItemIconPrefix[] =
    "\xc0\xaf\xc0\xfa\xc0\xce\xc5\xcd\xc6\xe4\xc0\xcc\xbd\xba" "\\item\\";
static const char kCollectionPrefix[] =
    "\xc0\xaf\xc0\xfa\xc0\xce\xc5\xcd\xc6\xe4\xc0\xcc\xbd\xba" "\\collection\\";

// Charge la desc d'une carte/enchant par id. Toutes les descs de ce client vivent
// dans rec+0x0c (RE 2026-07-05), lues par GetDescLines quand info+0x5c==1. On
// EnsureLoad d'abord (parse rec+0x0c dans le cache, comme OnMsg 0x18) — sinon une
// carte compound jamais ouverte seule aurait rec+0x0c vide. SEH (fonctions jeu).
void LoadCardDesc(uint32_t id, CardDesc* cd) {
  __try {
    // 1) Parse paresseux -> peuple rec+0x0c.
    void* cache = *reinterpret_cast<void**>(itemdb::kEnsureCachePtr);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(itemdb::kEnsureLoadedAddr)(cache, static_cast<int>(id));

    // 2) Nom affiché = record+4 (comme GetBaseName).
    void* rec = reinterpret_cast<DescLookup_t>(itemdb::kLookupAddr)(
        static_cast<int>(id), reinterpret_cast<void*>(itemdb::kTableAddr));
    if (rec && rec != reinterpret_cast<void*>(itemdb::kNilAddr)) {
      const char* nm = *reinterpret_cast<char**>(reinterpret_cast<char*>(rec) + 4);
      if (nm) {
        std::strncpy(cd->name, nm, sizeof(cd->name) - 1);
        LocalizeInPlace(cd->name, sizeof(cd->name));
      }
      // 2bis) Nombre d'emplacements de carte = record+0x30 (le natif le formate
      // « [%d] » via FUN_006a4c40 ; 0 = non sloté). Même source que le titre de la
      // fenêtre de description complète (cf. ExtractItem). Guard 1..kMaxCards.
      const int slots = *reinterpret_cast<int*>(reinterpret_cast<char*>(rec) + 0x30);
      if (slots > 0 && slots <= kMaxCards) cd->card_slots = slots;
    }

    // 3) Lignes de desc : info standalone (+0x5c=1) -> GetDescLines -> rec+0x0c.
    uint8_t info[0x100];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(itemdb::kInfoCtorAddr)(info);            // init les std::string
    reinterpret_cast<InfoSetId_t>(itemdb::kInfoSetIdAddr)(info, static_cast<int>(id));  // -> info+0x2c
    *(info + kInfoFlag) = 1;                                  // => lit rec+0x0c
    char*** vec = reinterpret_cast<GetDescLines_t>(kGetDescLines)(info);
    if (vec) {
      char** first = vec[0];
      char** last  = vec[1];
      const ptrdiff_t n = last - first;
      if (n > 0 && n < 4096) {
        for (char** it = first; it < last && cd->line_count < kMaxLines; ++it) {
          const char* line = *it;
          std::strncpy(cd->lines[cd->line_count], line ? line : "", kLineLen - 1);
          ++cd->line_count;
        }
        LocalizeLines(cd->lines, cd->line_count);
      }
    }

    // 4) Chemins images (mêmes fonctions natives que le jeu) : petite icône item
    // (GetResName -> rec+0x08 avec +0x5c=1) + illustration cardBmp (GetCardResName).
    const char* irn = reinterpret_cast<GetResName_t>(kGetResName)(info);
    if (irn && irn[0]) {
      std::snprintf(cd->icon_path, sizeof(cd->icon_path), "%s%s.bmp",
                    kItemIconPrefix, irn);
      // Même resname, autre dossier : l'art de preview. Beaucoup d'items n'en
      // ont pas — le chemin est bâti quand même, c'est le chargement qui échoue
      // et fait retomber la vignette sur la petite icône.
      std::snprintf(cd->coll_path, sizeof(cd->coll_path), "%s%s.bmp",
                    kCollectionPrefix, irn);
    }
    void* crec = reinterpret_cast<CardResName_t>(kGetCardResName)(static_cast<int>(id));
    // crec == la fiche nil (kCardNilRecord, resname "sorry") => id absent de la DB
    // carte = NON-carte : on laisse illust_path vide pour garder l'icône collection
    // (sinon on chargeait cardBmp\sorry.bmp = le placeholder "now printing").
    if (crec && crec != reinterpret_cast<void*>(kCardNilRecord)) {
      const char* crn = *reinterpret_cast<char**>(crec);
      if (crn && crn[0])
        std::snprintf(cd->illust_path, sizeof(cd->illust_path), "%s%s.bmp",
                      reinterpret_cast<const char*>(kCardBmpPrefix), crn);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Desc cache d'une carte (charge au 1er survol). Jamais nullptr.
const CardDesc* GetCardDesc(uint32_t id) {
  auto it = g_card_desc_cache.find(id);
  if (it != g_card_desc_cache.end()) return &it->second;
  CardDesc cd;
  LoadCardDesc(id, &cd);
  return &g_card_desc_cache.emplace(id, cd).first->second;
}

// Petite icône item d'une carte (chargée au 1er accès, cache par id ; tex null OK).
IconTex GetCardIcon(uint32_t id) {
  IconCachesGuard();
  auto it = g_card_icon_cache.find(id);
  if (it != g_card_icon_cache.end()) return it->second;
  const CardDesc* cd = GetCardDesc(id);
  IconTex t = cd->icon_path[0] ? LoadCollectionIcon(cd->icon_path) : IconTex{};
  g_card_icon_cache[id] = t;
  return t;
}

// Art de COLLECTION de l'item (chargé au 1er accès, cache par id). C'est l'image
// large que montre le cash shop ; la plupart des items d'équipement en ont une,
// les consommables rarement — d'où la tex vide, mémorisée, quand elle manque.
IconTex GetCardCollection(uint32_t id) {
  IconCachesGuard();
  auto it = g_card_collection_cache.find(id);
  if (it != g_card_collection_cache.end()) return it->second;
  const CardDesc* cd = GetCardDesc(id);
  IconTex t = cd->coll_path[0] ? LoadCollectionIcon(cd->coll_path) : IconTex{};
  g_card_collection_cache[id] = t;
  return t;
}

// Illustration cardBmp d'une carte (chargée au 1er accès, cache par id). AUTO-gatée :
// LoadCardDesc laisse illust_path vide pour les non-cartes (fiche nil kCardNilRecord),
// donc renvoie une tex vide pour tout ce qui n'est pas dans la DB carte -> l'appelant
// peut l'appeler inconditionnellement (les non-cartes gardent leur icône collection).
IconTex GetCardIllust(uint32_t id) {
  IconCachesGuard();
  auto it = g_card_illust_cache.find(id);
  if (it != g_card_illust_cache.end()) return it->second;
  const CardDesc* cd = GetCardDesc(id);
  IconTex t = cd->illust_path[0] ? LoadCollectionIcon(cd->illust_path) : IconTex{};
  g_card_illust_cache[id] = t;
  return t;
}

// Contenu PARTAGÉ (nom + illustration + description) d'une carte/enchant, dessiné
// tel quel par le tooltip de survol ET la fenêtre épinglée (clic droit). Saute la
// ligne 0 (lien DB <URL>ItemID..</URL>) = bruit. `sctext_id` = id du bloc de texte
// sélectionnable (unique par appelant) ; `wrap` = largeur de wrap (0 = région dispo).
void RenderCardDescBody(uint32_t id, const char* sctext_id, float wrap) {
  const CardDesc* cd = GetCardDesc(id);
  const ImVec4 hdr(0.30f, 0.24f, 0.10f, 1.0f);  // brun (comme "Cartes / Enchants")
  if (cd->name[0]) ImGui::TextColored(hdr, "%s (#%u)", cd->name, id);
  else             ImGui::TextColored(hdr, "#%u", id);

  // Ligne 0 = lien DB <URL>ItemID..</URL> : bruit -> sautée.
  const int skip = (cd->line_count > 0 && std::strstr(cd->lines[0], "<URL>") &&
                    std::strstr(cd->lines[0], "ItemID")) ? 1 : 0;
  const bool has_desc = cd->line_count > skip;

  // Illustration cardBmp (gauche) + desc (droite), comme la fenêtre native "view".
  IconTex illust = GetCardIllust(id);
  if (illust.tex || has_desc) ImGui::Separator();
  if (illust.tex && illust.w > 0 && illust.h > 0) {
    ImGui::Image(reinterpret_cast<ImTextureID>(illust.tex),
                 ImVec2(static_cast<float>(illust.w), static_cast<float>(illust.h)));
    if (has_desc) ImGui::SameLine(0, 8);
  }
  if (has_desc) {
    ImGui::BeginGroup();
    SelectableColoredText(sctext_id, cd->lines + skip, cd->line_count - skip,
                          IM_COL32(0, 0, 0, 255), wrap);
    ImGui::EndGroup();
  } else if (!illust.tex) {
    ImGui::TextDisabled("(pas de description)");
  }
}

// Tooltip au survol d'une carte/enchant : nom + desc (markup coloré) + rappel des
// actions clic gauche/droit.
void RenderCardTooltip(uint32_t id) {
  // Fond crème identique à la fenêtre desc, mais alpha conservé (~240) pour garder
  // la translucidité d'un tooltip. Texte noir (fond clair) comme la fenêtre.
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(245, 243, 232, 240));
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
  ImGui::BeginTooltip();
  RenderCardDescBody(id, "##cardtip", 340.0f);
  ImGui::Separator();
  ImGui::TextDisabled(
      "Clic gauche : base de données   \xc2\xb7   "
      "Clic droit : ouvrir la description complète");
  ImGui::EndTooltip();
  ImGui::PopStyleColor(2);
}

// Résout le nom localisé d'une random option via le getter Lua natif
// GetVarOptionName(id), appelé par l'API C Lua BRUTE (nom STATIQUE const char* =>
// pas de std::string BYVAL/heap/free, donc pas le crash d'allocateur du wrapper
// varargs). 100% DYNAMIQUE : lit la table Lua VIVANTE du client (NameTable_VAR),
// suit toutes les MàJ. Puis sprintf(valeur). SEH-gardé ; false si le lua_State
// n'est pas prêt (repli brut « Option #id » côté appelant).
bool ResolveOptName(int id, int value, char* out, size_t cap) {
  char lua_fmt[128] = {0};
  __try {
    // 0x015ffd78 -> M (ptr passé byval au wrapper) -> *M = vrai lua_State. Le
    // client fait `push [0x015ffd78]` puis `lua_getfield(*param_1,..)` : DOUBLE
    // déréférencement (confirmé x32 : *M a tt=8 LUA_TTHREAD).
    void* L = lua::State();
    if (L) {
      lua::GetField(L, lua::kGlobalsIndex,
                                                    "GetVarOptionName");
      lua::PushNumber(L, static_cast<double>(id));
      if (lua::PCall(L, 1, 1, 0) == 0) {
        const char* s =
            lua::ToLString(L, -1, nullptr);
        if (s && s[0]) std::strncpy(lua_fmt, s, sizeof(lua_fmt) - 1);
      }
      lua::SetTop(L, -2);       // pop résultat/erreur
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { lua_fmt[0] = '\0'; }
  if (!lua_fmt[0]) return false;
#pragma warning(push)
#pragma warning(disable : 4774)  // format non littéral (chaîne client de confiance %d/%%)
  std::snprintf(out, cap, lua_fmt, value);
#pragma warning(pop)
  return out[0] != '\0';
}

// Cache des noms d'options résolus (clé = id<<32 | valeur). "" = échec connu.
std::unordered_map<uint64_t, std::string> g_opt_name_cache;
const char* GetOptName(int id, int value) {
  const uint64_t key =
      (static_cast<uint64_t>(static_cast<uint32_t>(id)) << 32) |
      static_cast<uint32_t>(value);
  auto it = g_opt_name_cache.find(key);
  if (it == g_opt_name_cache.end()) {
    char buf[128] = {0};
    const bool ok = ResolveOptName(id, value, buf, sizeof(buf));
    it = g_opt_name_cache.emplace(key, ok ? buf : "").first;
  }
  return it->second.empty() ? nullptr : it->second.c_str();
}

// Clic GAUCHE sur une carte : ouvre la base de données de l'item (par id). Format
// confirmé en live : c'est exactement l'URL du lien natif <URL>..<INFO>url</INFO>
// de la ligne 0 de la desc (page=itemdb&itemid=<id>).
void OpenCardDbLink(uint32_t cardId) {
  char url[256];
  std::snprintf(url, sizeof(url),
                "https://moonlight-destiny.fr/index.php?page=itemdb&itemid=%u",
                cardId);
  ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// Résout (cache + chargement) l'icône de collection d'un item extrait.
IconTex ResolveIcon(uint32_t id, const ItemExtract& e) {
  if (!e.has_icon) return {};
  IconCachesGuard();
  auto it = g_icon_cache.find(id);
  if (it != g_icon_cache.end()) return it->second;
  IconTex tex = LoadCollectionIcon(e.iconpath);
  g_icon_cache[id] = tex;  // met en cache même null (miss connu)
  return tex;
}

}  // namespace

ItemDescWindow::ItemDescWindow() {
  if (kEnableServerFetch) {
    Bourgeon::Instance().RegisterRecvOpcode(kOpcodeTechData);
    Bourgeon::Instance().RegisterRecvOpcode(kOpcodeDamage);
    Bourgeon::Instance().RegisterRecvOpcode(kOpcodeItemScript);
  }
}

namespace {
// Cache le rendu natif d'UNE fenêtre desc lue depuis son slot (vtable validée).
void HideDescSlot(uintptr_t slot, uintptr_t vtable) {
  if (uint8_t* wnd = ReadValidWnd(slot, vtable))
    __try { reinterpret_cast<HideNative_t>(kHideNative)(wnd, nullptr, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Ferme une fenêtre du manager par id, SEH ISOLÉE dans une fonction libre : permet
// aux appelants (RenderItemWindow/RenderSkillWindow) d'utiliser des objets C++ à
// déroulement (temporaires std::string du bouton bug-report) sans C2712.
void SafeCloseWindowId(int id) {
  __try {
    uiwnd::CloseWindow(id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Ouvre (ou navigue) la fenêtre de description native (classe 0xc) vers l'item/carte
// `id`, comme un clic sur un lien de carte natif. Reproduit exactement le chemin de
// UIInventoryWnd_OnRButtonDown : MakeWindow(0xc) puis OnMsg 0x18 avec une
// ItemSkillInfo. Le handler natif (UIItemSkillDescWnd_OnMsg case 0x18) copie l'info
// dans wnd+0xb8 PUIS re-résout nom/desc/icône depuis la DB via atoi(resname) — une
// info minimale (ctor + SetId + flag +0x5c=1, comme LoadCardDesc) suffit donc. Le
// plugin reproduit ensuite la desc complète en ImGui (le natif reste caché). Comme
// la fenêtre 0xc est déjà ouverte, MakeWindow renvoie la MÊME instance -> l'item
// courant est remplacé par la carte. SEH (fonctions jeu, POD only).
void OpenCardDescWindow(uint32_t id) {
  __try {
    uint8_t info[0x100];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(itemdb::kInfoCtorAddr)(info);            // init les std::string
    reinterpret_cast<InfoSetId_t>(itemdb::kInfoSetIdAddr)(info, static_cast<int>(id));  // -> +0x2c
    *(info + kInfoFlag) = 1;                                  // => desc lues de rec+0x0c
    void* mgr = uiwnd::Mgr();        // objet manager embarqué
    void* wnd = uiwnd::MakeWindow(0xc);
    if (wnd) {
      void** vt = *reinterpret_cast<void***>(wnd);
      auto onmsg = reinterpret_cast<DescOnMsg_t>(vt[uiwnd::kVfOnMsg / 4]);
      onmsg(wnd, 0, itemdb::kItemDescMsgSet, static_cast<int>(reinterpret_cast<uintptr_t>(info)),
            0, 0, 0);
    }
    // Pas de dtor sur `info` : toutes ses std::string sont en SSO (id décimal court),
    // aucune allocation heap à libérer — identique à LoadCardDesc.
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
}  // namespace

void ItemDescWindow::HideNativeDescWindows() {
  if (!show_item_panel_) return;
  // Cache la fenêtre item (0xc) ET la fenêtre de comparaison (0xea) — cette
  // dernière est créée dans le même OnMsg 0x18, donc déjà présente ici.
  HideDescSlot(kItemWndSlot, kItemVTable);
  HideDescSlot(kCompareWndSlot, kCompareVTable);
}

void ItemDescWindow::HideNativeSkillWindow() {
  if (!kSkillWindowEnabled || !show_skill_panel_) return;
  HideDescSlot(kSkillWndSlot, kSkillVTable);
}

namespace {
// Lit une fenêtre au layout ITEM (0xc ou 0xea : id = chaîne décimale @+0xe4).
void ReadItemLayoutWindow(uintptr_t slot, uintptr_t vtable,
                          ItemDescWindow::DescWindow* out) {
  *out = ItemDescWindow::DescWindow{};
  if (uint8_t* wnd = ReadValidWnd(slot, vtable)) {
    __try {
      const char* idstr;
      if (*reinterpret_cast<uint32_t*>(wnd + kItemIdCap) > 0xf)
        idstr = *reinterpret_cast<char**>(wnd + kItemIdStr);
      else
        idstr = reinterpret_cast<const char*>(wnd + kItemIdStr);
      const long id = (idstr != nullptr) ? std::atol(idstr) : 0;
      if (id > 0) {
        out->open     = true;
        out->is_skill = false;
        out->id       = static_cast<uint32_t>(id);
        out->x        = *reinterpret_cast<int*>(wnd + uiwnd::kOffPosX);
        out->y        = *reinterpret_cast<int*>(wnd + uiwnd::kOffPosY);
        out->w        = *reinterpret_cast<int*>(wnd + kOffWidth);
        out->h        = *reinterpret_cast<int*>(wnd + kOffHeight);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { *out = ItemDescWindow::DescWindow{}; }
  }
}
}  // namespace

void ItemDescWindow::OnTick() {
  // Clic droit sur une carte au frame précédent -> ouvre sa desc complète MAINTENANT
  // (hors rendu ImGui). Navigue la fenêtre 0xc vers la carte ; les lectures de slot
  // ci-dessous prennent aussitôt le nouvel id.
  if (pending_card_open_ != 0) {
    OpenCardDescWindow(pending_card_open_);
    pending_card_open_ = 0;
  }

  // ── Fenêtres ITEM (0xc) + COMPARAISON équipé (0xea) : Option A ─────────────
  ReadItemLayoutWindow(kItemWndSlot, kItemVTable, &item_);
  ReadItemLayoutWindow(kCompareWndSlot, kCompareVTable, &compare_);
  // Cache le rendu natif à CHAQUE tick (filet de sécurité ; le hook OnMsg le
  // fait déjà sans flicker à l'ouverture). On garde les objets vivants pour les
  // données et on redessine en ImGui (à EndScene).
  HideNativeDescWindows();

  // ── Fenêtre SKILL (classe 0x2e) : détecte + cache le rendu natif (Option A).
  skill_ = DescWindow{};
  if (uint8_t* wnd = ReadValidWnd(kSkillWndSlot, kSkillVTable)) {
    __try {
      const int id = *reinterpret_cast<int*>(wnd + kSkillIdInt);
      if (id > 0) {
        skill_.open     = true;
        skill_.is_skill = true;
        skill_.id       = static_cast<uint32_t>(id);
        skill_.x        = *reinterpret_cast<int*>(wnd + uiwnd::kOffPosX);
        skill_.y        = *reinterpret_cast<int*>(wnd + uiwnd::kOffPosY);
        skill_.w        = *reinterpret_cast<int*>(wnd + kOffWidth);
        skill_.h        = *reinterpret_cast<int*>(wnd + kOffHeight);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { skill_ = DescWindow{}; }
  }
  // Repro skill désactivée (kSkillWindowEnabled) : on NE cache PAS le natif.
  if (kSkillWindowEnabled && show_skill_panel_ && skill_.open)
    HideDescSlot(kSkillWndSlot, kSkillVTable);

  // ── Fenêtre LIVRE (0x6a), ouverte par les boutons « Lire » / « Lecture auto »
  // ⚠️ En mode LECTURE AUTO le natif cache LUI-MÊME la fenêtre (OnMsg case 93 :
  // SetVisible(0)) et récite le livre ligne par ligne dans le chat, via son slot
  // d'update (qui tourne donc bien fenêtre cachée). Il n'y a alors rien à
  // reproduire : on laisse faire et on n'affiche pas de panneau.
  const bool book_was = book_.open;
  book_ = BookWindow{};
  if (uint8_t* bwnd = FindBookWindow()) {
    __try {
      book_.auto_read = *reinterpret_cast<uint8_t*>(bwnd + kBookAutoFlag) != 0;
      book_.page      = *reinterpret_cast<int*>(bwnd + kBookPage);
      book_.pages     = *reinterpret_cast<int*>(bwnd + kBookPageCount);
      std::strncpy(book_.title, reinterpret_cast<const char*>(bwnd + kBookTitle),
                   sizeof(book_.title) - 1);
      book_.open = !book_.auto_read;
    } __except (EXCEPTION_EXECUTE_HANDLER) { book_ = BookWindow{}; }
    if (show_book_panel_ && book_.open) HideBookWindow(bwnd);
    // Panneau moderne coupé : on rend la fenêtre au client (sauf en lecture auto,
    // où le natif la veut masquée).
    else if (!show_book_panel_ && !book_.auto_read) ShowBookWindow(bwnd);
  }
  // Front montant -> pose la fenêtre reproduite près du curseur (même réglage
  // d'ancrage que les descs).
  if (book_.open && !book_was) {
    // ⚠️ Le client NE rouvre PAS un livre au début : il restaure un SIGNET de page
    // stocké par personnage dans le registre (HKLM\…\Gravity Soft\Ragnarok\BookMark\
    // <serveur>\<perso>\<nom du livre>, écrit à la fermeture ; défaut 1 si absent).
    // C'est ce qui fait « démarrer page 2 » après une lecture précédente. Option
    // (défaut ON) : revenir à la première page à chaque ouverture.
    if (book_reset_page_ && book_.page > 1) {
      if (uint8_t* bwnd = FindBookWindow()) {
        CallBookMsg(bwnd, kMsgBookGoto, 1);
        if (show_book_panel_) HideBookWindow(bwnd);  // msg 92 ré-affiche la native
        book_.page = 1;
      }
    }
    book_need_raise_ = true;  // hors du if : le z-order ne dépend pas du curseur
    POINT pt;
    if (GetCursorPos(&pt)) {
      book_spawn_x_  = pt.x + desc_offset_x_;
      book_spawn_y_  = pt.y + desc_offset_y_;
      book_need_pos_ = true;
    }
  }

  // Front montant d'ouverture -> pose la fenêtre reproduite près du curseur.
  // Aucune requête serveur automatique : elles partent quand le joueur active
  // un onglet data (cf. RenderTechTabs).
  if (item_.open && !item_was_open_) {
    POINT pt;
    if (GetCursorPos(&pt)) {
      item_spawn_x_ = pt.x + desc_offset_x_;
      item_spawn_y_ = pt.y + desc_offset_y_;
      item_need_pos_ = true;
    }
  }
  // Remontée au 1er plan : à l'ouverture ET sur un changement d'objet dans une desc
  // DÉJÀ ouverte — clic droit sur un autre item pendant qu'elle est affichée. Ce
  // second cas n'a aucun front montant à offrir : sans lui, la desc changeait de
  // contenu en restant enterrée sous la fenêtre d'où venait le clic (elle, focus).
  // Le hook OnMsg 0x18 (RaiseItemWindow) pose déjà le drapeau, sans attendre le
  // prochain tick ; ce test-ci reste le filet si le détour n'a pas pu être posé.
  if (item_.open && (!item_was_open_ || item_.id != item_last_id_))
    item_need_raise_ = true;
  item_last_id_  = item_.open ? item_.id : 0;
  item_was_open_ = item_.open;
  if (skill_.open && !skill_was_open_) {
    POINT pt;
    if (GetCursorPos(&pt)) {
      skill_spawn_x_ = pt.x + desc_offset_x_;
      skill_spawn_y_ = pt.y + desc_offset_y_;
      skill_need_pos_ = true;
    }
  }
  // Même règle pour le skill (pas de hook côté 0x2e : c'est le polling qui voit
  // l'ouverture comme le passage d'une compétence à l'autre).
  if (skill_.open && (!skill_was_open_ || skill_.id != skill_last_id_))
    skill_need_raise_ = true;
  skill_last_id_  = skill_.open ? skill_.id : 0;
  skill_was_open_ = skill_.open;
}

void ItemDescWindow::RequestTechData(uint32_t id, bool is_skill, uint8_t scope) {
  if (!kEnableServerFetch) return;
  auto& entry = cache_[CacheKey(id, is_skill, scope)];
  const uint32_t now = GetTickCount();
  // Réponse encore fraîche : on ne réinterroge pas (le TTL borne la staleness
  // des taux dynamiques). GetTickCount peut wrapper (~49j) -> la soustraction
  // non-signée reste correcte.
  if (entry.state == FetchState::kReady &&
      (now - entry.requested_tick) < kTechTtlMs)
    return;
  // Requête déjà en vol et pas encore expirée : on attend.
  if (entry.state == FetchState::kPending &&
      (now - entry.requested_tick) < kTechPendingMs)
    return;

  entry.is_skill = is_skill;
  uint8_t pkt[10];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpcodeReqTechData;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(sizeof(pkt));
  *reinterpret_cast<uint32_t*>(pkt + 4) = id;
  pkt[8] = is_skill ? 1 : 0;
  pkt[9] = scope;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  entry.state          = FetchState::kPending;
  entry.requested_tick = now;
}

void ItemDescWindow::RequestDamage(uint32_t skill_id, uint32_t target_mob_id) {
  if (!kEnableServerFetch) return;
  auto& e = dmg_cache_[skill_id];
  const uint32_t now = GetTickCount();
  if (e.state == FetchState::kReady && (now - e.requested_tick) < kTechTtlMs)
    return;
  if (e.state == FetchState::kPending && (now - e.requested_tick) < kTechPendingMs)
    return;

  uint8_t pkt[14];  // [op:2][len:2][skill_id:4][skill_lv:2][target_mob_id:4]
  *reinterpret_cast<uint16_t*>(pkt + 0)  = kOpcodeReqDamage;
  *reinterpret_cast<uint16_t*>(pkt + 2)  = static_cast<uint16_t>(sizeof(pkt));
  *reinterpret_cast<uint32_t*>(pkt + 4)  = skill_id;
  *reinterpret_cast<uint16_t*>(pkt + 8)  = 0;  // niveau 0 = niveau appris
  *reinterpret_cast<uint32_t*>(pkt + 10) = target_mob_id;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  e.state          = FetchState::kPending;
  e.requested_tick = now;
  e.target         = target_mob_id;
}

void ItemDescWindow::RequestItemScript(uint32_t id) {
  if (!kEnableServerFetch) return;
  auto& e = script_cache_[id];
  const uint32_t now = GetTickCount();
  // Le script d'un item ne dépend d'aucun facteur dynamique : une fois « prêt »,
  // on ne réinterroge jamais (cache définitif pour la session).
  if (e.state == FetchState::kReady) return;
  if (e.state == FetchState::kPending && (now - e.requested_tick) < kTechPendingMs)
    return;

  uint8_t pkt[8];  // [op:2][len:2][id:4]
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpcodeReqItemScript;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(sizeof(pkt));
  *reinterpret_cast<uint32_t*>(pkt + 4) = id;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  e.state          = FetchState::kPending;
  e.requested_tick = now;
}

void ItemDescWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                  uint16_t len) {
  // Réponse script + combos (0x0F12). Payload (après [id:4][status:1] déjà retiré
  // du header par le reader-hook -> data pointe sur [id:4]) :
  //   [id:4][status:1] puis si status==0 : 3 chaînes préfixées 16 bits (script/
  //   equip/unequip), [combo_count:1], puis par combo [member_count:1] ×
  //   [member_id:4][namelen:1][name] et [script_len:2][script].
  if (opcode == kOpcodeItemScript) {
    if (len < 5) return;  // [id:4][status:1]
    const uint32_t id     = *reinterpret_cast<const uint32_t*>(data);
    const uint8_t  status = data[4];
    auto& e = script_cache_[id];
    e.main.clear(); e.equip.clear(); e.unequip.clear(); e.combos.clear();
    e.status = status;
    const uint8_t* p   = data + 5;
    const uint8_t* end = data + len;
    // Lit une chaîne préfixée d'une longueur 16 bits (bornée par `end`).
    auto read_str = [&](std::string* out) -> bool {
      if (end - p < 2) return false;
      const uint16_t n = *reinterpret_cast<const uint16_t*>(p); p += 2;
      if (end - p < n) return false;
      out->assign(reinterpret_cast<const char*>(p), n); p += n;
      return true;
    };
    if (status == 0) {
      if (read_str(&e.main) && read_str(&e.equip) && read_str(&e.unequip) &&
          p < end) {
        const uint8_t combo_count = *p++;
        for (uint8_t ci = 0; ci < combo_count; ++ci) {
          if (p >= end) break;
          ComboInfo ci_info;
          const uint8_t mcount = *p++;
          bool ok = true;
          for (uint8_t mi = 0; mi < mcount; ++mi) {
            if (end - p < 5) { ok = false; break; }  // [id:4][namelen:1]
            const uint32_t mid = *reinterpret_cast<const uint32_t*>(p); p += 4;
            const uint8_t  nl  = *p++;
            if (end - p < nl) { ok = false; break; }
            std::string nm(reinterpret_cast<const char*>(p), nl); p += nl;
            ci_info.members.emplace_back(mid, std::move(nm));
          }
          if (!ok || !read_str(&ci_info.script)) break;
          e.combos.push_back(std::move(ci_info));
        }
      }
    }
    e.state = FetchState::kReady;
    return;
  }
  // Réponse d'estimation de dégâts (0x0F0E).
  if (opcode == kOpcodeDamage) {
    // [id:4][lv:2][target:4][status:1][atk:1][hits:2][min/max/avg:8*3] puis
    // [namelen:1][name] (nom du monstre, optionnel).
    if (len < 38) return;
    const uint32_t sid = *reinterpret_cast<const uint32_t*>(data);
    auto& e = dmg_cache_[sid];
    e.skill_lv = *reinterpret_cast<const uint16_t*>(data + 4);
    e.target   = *reinterpret_cast<const uint32_t*>(data + 6);
    e.status   = data[10];
    e.atk_type = data[11];
    e.hits     = *reinterpret_cast<const uint16_t*>(data + 12);
    std::memcpy(&e.dmg_min, data + 14, 8);
    std::memcpy(&e.dmg_max, data + 22, 8);
    std::memcpy(&e.dmg_avg, data + 30, 8);
    e.target_name.clear();
    if (len >= 39) {  // nom de monstre présent
      const uint8_t nl = data[38];
      if (nl && len >= 39u + nl)
        e.target_name.assign(reinterpret_cast<const char*>(data + 39), nl);
    }
    e.state = FetchState::kReady;
    return;
  }
  if (opcode != kOpcodeTechData) return;
  if (len < 6) return;  // besoin d'au moins [id:4][is_skill:1][scope:1]
  const uint32_t id       = *reinterpret_cast<const uint32_t*>(data);
  const bool     is_skill = data[4] != 0;
  const uint8_t  scope    = data[5];

  auto& entry = cache_[CacheKey(id, is_skill, scope)];
  entry.is_skill          = is_skill;
  entry.drops.clear();
  entry.levels.clear();
  entry.truncated         = false;
  entry.treasure_excluded = 0;
  entry.max_lv            = 0;

  // Parseur borné : `p` avance dans le payload, `end` = garde-fou.
  const uint8_t* p   = data + 6;
  const uint8_t* end = data + len;

  if (!is_skill) {
    // [count:1][truncated:1][treasure_excluded:1] puis count ×
    // [mob_id:4][rate:4][boss:1][namelen:1][name].
    if (end - p < 3) { entry.state = FetchState::kReady; return; }
    const uint8_t count     = p[0];
    entry.truncated         = p[1] != 0;
    entry.treasure_excluded = p[2];
    p += 3;
    for (uint8_t i = 0; i < count; ++i) {
      if (end - p < 11) break;  // [mob:4][rate:4][boss:1][src:1][namelen:1]
      DropSrc d;
      d.mob_id = *reinterpret_cast<const uint32_t*>(p); p += 4;
      d.rate   = *reinterpret_cast<const uint32_t*>(p); p += 4;
      d.boss   = *p; p += 1;
      d.src    = *p; p += 1;
      const uint8_t namelen = *p; p += 1;
      if (end - p < namelen) break;
      d.name.assign(reinterpret_cast<const char*>(p), namelen);
      p += namelen;
      entry.drops.push_back(std::move(d));
    }
  } else {
    // [max_lv:1] puis max_lv × [cast_var:4][cast_fixed:4][cooldown:4][delay:4].
    if (end - p < 1) { entry.state = FetchState::kReady; return; }
    const uint8_t maxlv = *p; p += 1;
    entry.max_lv = maxlv;
    for (uint8_t lv = 0; lv < maxlv; ++lv) {
      if (end - p < 16) break;
      SkillLv s;
      s.cast_var   = *reinterpret_cast<const int32_t*>(p); p += 4;
      s.cast_fixed = *reinterpret_cast<const int32_t*>(p); p += 4;
      s.cooldown   = *reinterpret_cast<const int32_t*>(p); p += 4;
      s.delay      = *reinterpret_cast<const int32_t*>(p); p += 4;
      entry.levels.push_back(s);
    }
  }
  entry.state = FetchState::kReady;
}

// Rend une table de sources (filtre + tri + liens bestiaire). show_type ajoute
// une colonne mécanisme (drop normal / MVP reward).
void ItemDescWindow::RenderDropTable(const TechData& td, const char* table_id,
                                     uint32_t filter_key, bool show_type) {
  if (td.drops.empty()) {
    ImGui::TextDisabled("Aucune source.");
    return;
  }
  // Filtre texte (par nom de monstre), persistant entre frames par (id,scope).
  static std::unordered_map<uint32_t, ImGuiTextFilter> s_filters;
  ImGuiTextFilter& filter = s_filters[filter_key];
  // Label ID unique par table (les 2 sections partagent le même texte visible
  // -> sans ceci, ImGui signale un conflit d'ID entre les deux InputText).
  char flabel[80];
  _snprintf_s(flabel, sizeof(flabel), _TRUNCATE, "Filtrer (monstre)##%s",
              table_id);
  ImGui::SetNextItemWidth(180.0f);
  filter.Draw(flabel);

  std::vector<const DropSrc*> view;
  view.reserve(td.drops.size());
  for (const DropSrc& d : td.drops)
    if (filter.PassFilter(d.name.c_str())) view.push_back(&d);

  const ImGuiTableFlags tf =
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
      ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
  ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, IM_COL32(206, 198, 172, 255));
  if (ImGui::BeginTable(table_id, show_type ? 3 : 2, tf)) {
    ImGui::TableSetupColumn("Monstre", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Taux", ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_PreferSortDescending |
                                        ImGuiTableColumnFlags_DefaultSort,
                            70.0f);
    if (show_type)
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 105.0f);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
      if (sort->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& sp = sort->Specs[0];
        const bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
        std::sort(view.begin(), view.end(),
                  [&](const DropSrc* a, const DropSrc* b) {
                    int c;
                    if (sp.ColumnIndex == 1) {
                      c = (a->rate < b->rate) ? -1 : (a->rate > b->rate ? 1 : 0);
                    } else if (sp.ColumnIndex == 2) {  // Type (mécanisme)
                      if (a->src != b->src)
                        c = (a->src < b->src) ? -1 : 1;
                      else
                        c = (a->rate < b->rate) ? 1 : (a->rate > b->rate ? -1 : 0);
                    } else {
                      c = _stricmp(a->name.c_str(), b->name.c_str());
                    }
                    return asc ? c < 0 : c > 0;
                  });
      }
    }

    for (const DropSrc* d : view) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      // Badge type de boss avant le nom.
      if (d->boss == 2) {
        ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f), "[MVP]");
        ImGui::SameLine();
      } else if (d->boss == 1) {
        ImGui::TextColored(ImVec4(0.80f, 0.55f, 0.10f, 1.0f), "[Mini]");
        ImGui::SameLine();
      }
      // Nom cliquable → bestiaire du site (lien externe).
      const ImVec4 kLink(0.10f, 0.30f, 0.85f, 1.0f);
      ImGui::TextColored(kLink, "%s (%u)", d->name.c_str(), d->mob_id);
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y),
                                            ImVec2(mx.x, mx.y),
                                            ImGui::GetColorU32(kLink));
      }
      if (ImGui::IsItemClicked()) {
        char url[192];
        _snprintf_s(url, sizeof(url), _TRUNCATE,
                    "https://moonlight-destiny.fr/index.php?page=bestiary&mobid=%u",
                    d->mob_id);
        ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
      }
      ImGui::TableNextColumn();
      ImGui::Text("%.2f%%", d->rate / 100.0f);  // rate en 0.01% (10000 = 100%)
      if (show_type) {
        ImGui::TableNextColumn();
        if (d->src == 1)
          ImGui::TextColored(ImVec4(0.80f, 0.55f, 0.10f, 1.0f), "MVP reward");
        else
          ImGui::TextDisabled("Drop normal");
      }
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleColor();  // TableHeaderBg
  if (td.treasure_excluded > 0)
    ImGui::TextDisabled("%u coffre(s) au trésor exclu(s).", td.treasure_excluded);
  if (td.truncated)
    ImGui::TextDisabled("... autres sources : voir la base de données.");
  // Les spawns d'instance (donjons instanciés) ne sont pas encore pris en
  // compte par le scan serveur : ces sources peuvent manquer dans la liste.
  ImGui::TextColored(ImVec4(0.70f, 0.55f, 0.20f, 1.0f),
                     "Note : les spawns d'instance ne sont pas encore "
                     "comptés/scannés.");
}

// ── Rendu « éditeur » d'un script rAthena (pretty-print + coloration) ────────
namespace {

// Reformate un script rAthena pour la lisibilité : un statement par ligne
// (découpe sur « ; »), espace après les virgules, espaces multiples compactés.
// Les CHAÎNES "..." et COMMENTAIRES // /* */ sont copiés VERBATIM (on ne coupe
// jamais dedans — les scripts autobonus embarquent tout un sous-script entre
// guillemets). Pas d'indentation de blocs (les vrais { } de haut niveau sont
// rares dans les scripts d'item ; ils vivent surtout dans des chaînes).
std::string PrettyPrintScript(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 64);
  bool in_str = false, in_line_c = false, in_block_c = false;
  bool at_line_start = true;  // pour absorber les espaces en début de ligne
  const size_t n = in.size();
  for (size_t i = 0; i < n; ++i) {
    const char c = in[i];
    const char nx = (i + 1 < n) ? in[i + 1] : '\0';
    if (in_line_c) {
      if (c == '\n') { in_line_c = false; out.push_back('\n'); at_line_start = true; }
      else out.push_back(c);
      continue;
    }
    if (in_block_c) {
      out.push_back(c);
      if (c == '*' && nx == '/') { out.push_back('/'); ++i; in_block_c = false; }
      continue;
    }
    if (in_str) {
      out.push_back(c);
      if (c == '\\' && nx) { out.push_back(nx); ++i; }
      else if (c == '"') in_str = false;
      continue;
    }
    if (c == '/' && nx == '/') { in_line_c = true; out += "//"; ++i; at_line_start = false; continue; }
    if (c == '/' && nx == '*') { in_block_c = true; out += "/*"; ++i; at_line_start = false; continue; }
    if (c == '"') { in_str = true; out.push_back(c); at_line_start = false; continue; }
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      if (!at_line_start && !out.empty() && out.back() != ' ') out.push_back(' ');
      continue;
    }
    if (c == ';') {
      while (!out.empty() && out.back() == ' ') out.pop_back();
      out.push_back(';');
      out.push_back('\n');
      at_line_start = true;
      continue;
    }
    if (c == ',') {
      while (!out.empty() && out.back() == ' ') out.pop_back();
      out += ", ";
      at_line_start = false;
      continue;
    }
    out.push_back(c);
    at_line_start = false;
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

inline bool IdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.' ||
         c == '@' || c == '$' || c == '#' || c == '\'';
}
inline bool IdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' ||
         c == '@' || c == '$' || c == '#' || c == '\'';
}
// Mot-clé de contrôle (bleu) ?
bool IsControlKw(const char* b, size_t len) {
  static const char* kw[] = {"if", "else", "for", "while", "do", "switch",
                             "case", "default", "break", "continue", "return",
                             "function"};
  for (const char* k : kw)
    if (std::strlen(k) == len && std::strncmp(b, k, len) == 0) return true;
  return false;
}
// Commande de script hors appel-fonction (jaune) ?
bool IsCmdKw(const char* b, size_t len) {
  static const char* kw[] = {"set",  "mes",  "next",     "close",   "close2",
                             "end",  "menu", "goto",     "callfunc", "callsub",
                             "warp", "getarg"};
  for (const char* k : kw)
    if (std::strlen(k) == len && std::strncmp(b, k, len) == 0) return true;
  return false;
}

// Dessine un script rAthena dans un bloc « éditeur » à fond sombre fixe (palette
// type VS Code — lisible quel que soit le thème de la fenêtre). Pretty-print +
// coloration lexicale (mots-clés, commandes bonus/appels, params bXxx, constantes
// MAJUSCULE, nombres, chaînes, commentaires). Wrap doux + bouton « Copier ».
void DrawScriptCode(const char* box_id, const std::string& raw) {
  const std::string code = PrettyPrintScript(raw);
  char clbl[48];
  std::snprintf(clbl, sizeof(clbl), "Copier##%s", box_id);
  if (ImGui::SmallButton(clbl)) ImGui::SetClipboardText(code.c_str());

  // Palette (VS Code Dark+).
  const ImU32 kDef  = IM_COL32(212, 212, 212, 255);
  const ImU32 kCom  = IM_COL32(106, 153, 85, 255);   // commentaire
  const ImU32 kStr  = IM_COL32(206, 145, 120, 255);  // chaîne
  const ImU32 kNum  = IM_COL32(181, 206, 168, 255);  // nombre
  const ImU32 kCtl  = IM_COL32(86, 156, 214, 255);   // if/else/for…
  const ImU32 kCmd  = IM_COL32(220, 220, 170, 255);  // bonus/appels/commandes
  const ImU32 kParm = IM_COL32(156, 220, 254, 255);  // bStr, bBaseAtk…
  const ImU32 kCst  = IM_COL32(78, 201, 176, 255);   // Constantes (MAJ)

  ImDrawList* dl   = ImGui::GetWindowDrawList();
  ImFont*     font = ImGui::GetFont();
  const float fsz   = ImGui::GetFontSize();
  const float lineH = ImGui::GetTextLineHeightWithSpacing();
  const float padX = 6.0f, padY = 5.0f;
  float avail = ImGui::GetContentRegionAvail().x;
  if (avail < 60.0f) avail = 60.0f;
  const float innerWrap = avail - padX * 2.0f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float x0 = p0.x + padX, y0 = p0.y + padY;

  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);  // texte au-dessus du fond

  float penX = 0.0f, penY = 0.0f;
  auto tok = [&](const char* b, const char* e, ImU32 col) {
    const ImVec2 sz = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, b, e);
    if (penX > 0.0f && penX + sz.x > innerWrap) {  // wrap doux + indent continuation
      penX = 12.0f;
      penY += lineH;
    }
    dl->AddText(font, fsz, ImVec2(x0 + penX, y0 + penY), col, b, e);
    penX += sz.x;
  };

  const char* s = code.c_str();
  const size_t n = code.size();
  for (size_t i = 0; i < n;) {
    const char c = s[i];
    if (c == '\n') { penX = 0.0f; penY += lineH; ++i; continue; }
    if (c == ' ' || c == '\t') {
      size_t j = i;
      while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j;
      tok(s + i, s + j, kDef);
      i = j;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {  // commentaire ligne
      size_t j = i;
      while (j < n && s[j] != '\n') ++j;
      tok(s + i, s + j, kCom);
      i = j;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {  // commentaire bloc
      size_t j = i + 2;
      while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) ++j;
      j = (j + 2 < n) ? j + 2 : n;
      tok(s + i, s + j, kCom);
      i = j;
      continue;
    }
    if (c == '"') {  // chaîne
      size_t j = i + 1;
      while (j < n && s[j] != '"') { if (s[j] == '\\' && j + 1 < n) ++j; ++j; }
      if (j < n) ++j;
      tok(s + i, s + j, kStr);
      i = j;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {  // nombre (dec/hex)
      size_t j = i;
      while (j < n && (std::isalnum(static_cast<unsigned char>(s[j])) ||
                       s[j] == 'x' || s[j] == 'X'))
        ++j;
      tok(s + i, s + j, kNum);
      i = j;
      continue;
    }
    if (IdentStart(c)) {  // identifiant -> classification
      size_t j = i;
      while (j < n && IdentChar(s[j])) ++j;
      const size_t len = j - i;
      ImU32 col = kDef;
      if (IsControlKw(s + i, len)) {
        col = kCtl;
      } else {
        size_t k = j;  // appel de fonction ? (« ident( »)
        while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
        const bool is_call = (k < n && s[k] == '(');
        const bool is_bonus =
            (len >= 5 && std::strncmp(s + i, "bonus", 5) == 0) ||
            (len >= 9 && std::strncmp(s + i, "autobonus", 9) == 0);
        if (is_call || is_bonus || IsCmdKw(s + i, len))
          col = kCmd;
        else if (len >= 2 && s[i] == 'b' &&
                 std::isupper(static_cast<unsigned char>(s[i + 1])))
          col = kParm;  // bStr, bBaseAtk, bAtkRate…
        else if (std::isupper(static_cast<unsigned char>(c)))
          col = kCst;   // Class_Boss, RC_*, Ele_*, EQI_*…
      }
      tok(s + i, s + j, col);
      i = j;
      continue;
    }
    // ponctuation / opérateur
    tok(s + i, s + i + 1, kDef);
    ++i;
  }

  const float totalH = penY + lineH;
  dl->ChannelsSetCurrent(0);  // fond derrière le texte
  dl->AddRectFilled(p0, ImVec2(p0.x + avail, y0 + totalH + padY),
                    IM_COL32(30, 30, 32, 255), 4.0f);
  dl->ChannelsMerge();

  ImGui::Dummy(ImVec2(avail, totalH + padY * 2.0f));
}

}  // namespace

// ── API partagée : contenu de description simplifié ─────────────────────────
// Même corps que le survol d'une carte/enchant (nom + illustration si c'en est une
// + lignes colorées), sans le rappel d'actions propre au panneau « Cartes ». La
// desc vient de la DB native via GetCardDesc/LoadCardDesc, qui vaut pour N'IMPORTE
// quel id d'item, pas seulement les cartes.
namespace itemdesc {

// Hauteur max de l'illustration dans l'aperçu. Les cardBmp font ~300x460 : à leur
// taille native (ce que fait la fenêtre complète) elles écrasent le texte et
// remplissent l'écran. On borne la HAUTEUR et on déduit la largeur du ratio.
constexpr float kSimpleIllustH = 110.0f;

void RenderSimpleDesc(uint32_t id, float wrap, const uint32_t* cards,
                      int card_count, const SimpleOpt* opts, int opt_count, int refine,
                      const char* display_name) {
  if (id == 0) return;
  const CardDesc* cd = GetCardDesc(id);
  const ImVec4 hdr(0.30f, 0.24f, 0.10f, 1.0f);  // brun (comme la fenêtre desc)

  // Item FORGÉ/CRÉÉ : card[0] PETIT NON NUL (<= 500) = données du forgeron (charid
  // scindé, star crumbs, élément), PAS des cartes. Même critère que la fenêtre de
  // description complète (cf. ExtractItem) : ni section « Cartes », ni suffixe [N].
  const uint32_t card0 = (cards && card_count > 0) ? cards[0] : 0u;
  const bool forged = (card0 != 0 && card0 <= 500);

  // Titre = celui de la fenêtre de description COMPLÈTE : nom décoré par le
  // name-builder natif (refine « +7 », préfixes/suffixes des cartes serties, forge)
  // que l'appelant a composé depuis SON ItemSkillInfo, puis le nombre
  // d'emplacements « [N] » — que BuildDisplayName ne compose pas, le natif le
  // formate à part. Sans nom décoré : repli sur le nom de base de la DB + « +N ».
  char title[160];
  if (display_name && display_name[0]) {
    std::snprintf(title, sizeof(title), "%s", display_name);
  } else if (cd->name[0]) {
    if (refine > 0) std::snprintf(title, sizeof(title), "+%d %s", refine, cd->name);
    else            std::snprintf(title, sizeof(title), "%s", cd->name);
  } else {
    std::snprintf(title, sizeof(title), "#%u", id);
  }
  // « [N] » seulement si le nom n'en porte pas déjà un : le viewer storage ajoute
  // le sien depuis la meta serveur, et un nom d'enchant peut contenir un crochet —
  // sinon on doublerait le suffixe.
  if (!forged && cd->card_slots > 0 && std::strchr(title, '[') == nullptr) {
    const size_t len = strnlen(title, sizeof(title));
    std::snprintf(title + len, sizeof(title) - len, " [%d]", cd->card_slots);
  }
  // Titre WRAPPÉ : un nom décoré (« +7 Vadon's Sword of Rage [4] ») dépasse
  // largement la largeur du tooltip, qui est bornée — sans wrap il déborderait du
  // cadre au lieu de passer à la ligne.
  if (wrap > 0.0f) ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
  ImGui::TextColored(hdr, "%s", title);
  if (wrap > 0.0f) ImGui::PopTextWrapPos();

  // ── L'id de l'objet ──────────────────────────────────────────────────────
  // Même formulation que la fenêtre de description COMPLÈTE, qui écrit « ID : N »
  // en grisé quand elle n'a pas le lien database. Ici on saute toujours ce lien
  // (cf. juste en dessous) : sans cette ligne, l'aperçu au survol serait le seul
  // endroit de l'UI où l'id n'apparaît plus. Posée AVANT le corps, elle précède
  // donc la ligne « ViewID : N », qui ouvre celui-ci.
  ImGui::TextDisabled("ID : %u", id);

  // Ligne 0 = lien DB <URL>ItemID..</URL> : bruit -> sautée.
  const int skip = (cd->line_count > 0 && std::strstr(cd->lines[0], "<URL>") &&
                    std::strstr(cd->lines[0], "ItemID")) ? 1 : 0;
  const bool has_desc = cd->line_count > skip;

  // Vignette, par ordre de préférence : illustration de carte RÉDUITE si c'en est
  // une, sinon l'art de COLLECTION (l'image large du client, la même que le cash
  // shop), sinon la petite icône d'inventaire. La collection manquait ici : un
  // costume s'affichait en vignette 24x24 alors que le client en a l'art complet.
  IconTex img = GetCardIllust(id);
  if (!img.tex) img = GetCardCollection(id);
  if (!img.tex) img = GetCardIcon(id);
  if (img.tex || has_desc) ImGui::Separator();
  // `wrap` = largeur TOTALE dispo. Le texte étant posé À DROITE de la vignette, il
  // faut lui retirer la largeur de celle-ci + l'espacement, sinon il déborde du
  // panneau et se fait couper à droite.
  float text_wrap = wrap;
  if (img.tex && img.w > 0 && img.h > 0) {
    const float h = (std::min)(kSimpleIllustH, static_cast<float>(img.h));
    const float w = h * img.w / static_cast<float>(img.h);
    ImGui::Image(reinterpret_cast<ImTextureID>(img.tex), ImVec2(w, h));
    if (has_desc) {
      ImGui::SameLine(0, 8);
      if (text_wrap > 0.0f) text_wrap -= w + 8.0f;
    }
  }
  if (has_desc) {
    ImGui::BeginGroup();
    SelectableColoredText("##simpledesc", cd->lines + skip, cd->line_count - skip,
                          IM_COL32(0, 0, 0, 255), text_wrap);
    ImGui::EndGroup();
  } else if (!img.tex) {
    ImGui::TextDisabled("(pas de description)");
  }

  // ── Cartes / enchants insérés (données d'INSTANCE, pas de la DB) ───────────
  int ncards = 0;
  if (!forged)
    for (int i = 0; i < card_count; ++i)
      if (cards && cards[i]) ++ncards;
  if (ncards > 0) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.30f, 0.24f, 0.10f, 1.0f), "Cartes / Enchants");
    for (int i = 0; i < card_count; ++i) {
      const uint32_t cid = cards ? cards[i] : 0;
      if (!cid) continue;
      const IconTex ic = GetCardIcon(cid);
      if (ic.tex && ic.w > 0 && ic.h > 0) {
        const float h = ImGui::GetTextLineHeight();
        ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex),
                     ImVec2(h * ic.w / static_cast<float>(ic.h), h));
        ImGui::SameLine(0, 4);
      }
      const CardDesc* ccd = GetCardDesc(cid);
      if (ccd->name[0]) ImGui::TextUnformatted(ccd->name);
      else              ImGui::Text("#%u", cid);
    }
  }

  // ── Random options d'instance (nom localisé résolu par le Lua du client) ───
  int nopts = 0;
  for (int i = 0; i < opt_count; ++i)
    if (opts && opts[i].index) ++nopts;
  if (nopts > 0) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.30f, 0.24f, 0.10f, 1.0f), "Options");
    for (int i = 0; i < opt_count; ++i) {
      if (!opts || !opts[i].index) continue;
      const char* nm = GetOptName(opts[i].index, opts[i].value);
      if (nm && nm[0]) ImGui::TextUnformatted(nm);
      else ImGui::Text("Option #%d : %d", opts[i].index, opts[i].value);
    }
  }
}

}  // namespace itemdesc

// Onglets d'infos techniques, émis DANS le TabBar de la fenêtre (après l'onglet
// Description). L'onglet Description étant actif par défaut, aucune requête n'est
// lancée tant que le joueur ne sélectionne pas un onglet data.
void ItemDescWindow::RenderTechTabs(const DescWindow& w) {
  if (!kEnableServerFetch) return;
  const bool panel_on = w.is_skill ? show_skill_panel_ : show_item_panel_;
  if (!panel_on) return;

  // Corps commun (drops item / cast skill) : requête quand l'onglet est actif,
  // renvoie l'entrée cache une fois prête. Bouton « Rafraîchir » = force un
  // recalcul immédiat (utile après un changement de stats/stuff).
  auto tech_body = [&](uint8_t scope) -> const TechData* {
    const uint32_t key = CacheKey(w.id, w.is_skill, scope);
    char rlbl[40];
    std::snprintf(rlbl, sizeof(rlbl), "Rafraichir##tech%u", key);
    if (ImGui::SmallButton(rlbl)) cache_.erase(key);  // force refetch
    RequestTechData(w.id, w.is_skill, scope);
    auto it = cache_.find(key);
    const FetchState st =
        (it != cache_.end()) ? it->second.state : FetchState::kNone;
    if (st == FetchState::kPending)
      ImGui::TextDisabled("Chargement...");
    else if (st == FetchState::kFailed)
      ImGui::TextDisabled("échec de la requête");
    return (st == FetchState::kReady && it != cache_.end()) ? &it->second
                                                            : nullptr;
  };

  if (w.is_skill) {
    // ── Onglet « Infos techniques » : table de cast par niveau ──────────────
    if (ImGui::BeginTabItem("Infos techniques")) {
      const TechData* td = tech_body(kScopeNormal);
      if (td && td->levels.empty()) {
        ImGui::TextDisabled("Aucune donnée de cast.");
      } else if (td) {
        const ImGuiTableFlags tf = ImGuiTableFlags_Borders |
                                   ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_SizingFixedFit;
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,
                              IM_COL32(206, 198, 172, 255));
        if (ImGui::BeginTable("tech_skill", 4, tf)) {
          ImGui::TableSetupColumn("Niv");
          ImGui::TableSetupColumn("Cast");
          ImGui::TableSetupColumn("Cooldown");
          ImGui::TableSetupColumn("Delai");
          ImGui::TableHeadersRow();
          auto sec = [](int32_t ms) { return ms / 1000.0f; };
          for (size_t i = 0; i < td->levels.size(); ++i) {
            const SkillLv& s = td->levels[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", (int)i + 1);
            ImGui::TableNextColumn(); ImGui::Text("%.2fs", sec(s.cast_var));
            ImGui::TableNextColumn(); ImGui::Text("%.2fs", sec(s.cooldown));
            ImGui::TableNextColumn(); ImGui::Text("%.2fs", sec(s.delay));
          }
          ImGui::EndTable();
        }
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Valeurs effectives : tes stats + gear + buffs actifs.");
      }
      ImGui::EndTabItem();
    }

    // ── Onglet « Dégâts » : cible réglable (mob id, ou soi-même PvP) ────────
    if (ImGui::BeginTabItem("Dégâts")) {
      // Miroir PvP : cible = toi-même (ta def/élément/gear encaissent le sort).
      const bool self_changed =
          ImGui::Checkbox("Contre moi-même (PvP)", &dmg_target_self_);
      ImGui::SetNextItemWidth(120.0f);
      if (dmg_target_self_) ImGui::BeginDisabled();
      ImGui::InputInt("Cible : ID monstre (0 = neutre)", &dmg_target_input_,
                      0, 0);
      if (dmg_target_self_) ImGui::EndDisabled();
      if (dmg_target_input_ < 0) dmg_target_input_ = 0;
      ImGui::SameLine();
      const bool calc = ImGui::Button("Calculer");

      // Cible envoyée : 0xFFFFFFFF = soi-même, sinon l'id saisi (0 = neutre).
      const uint32_t req_target =
          dmg_target_self_ ? 0xFFFFFFFFu
                           : static_cast<uint32_t>(dmg_target_input_);

      auto it = dmg_cache_.find(w.id);
      const bool have =
          (it != dmg_cache_.end()) && it->second.state != FetchState::kNone;
      if (calc || self_changed || !have) {
        if (calc || self_changed) dmg_cache_.erase(w.id);
        RequestDamage(w.id, req_target);
        it = dmg_cache_.find(w.id);
      }

      const FetchState st =
          (it != dmg_cache_.end()) ? it->second.state : FetchState::kNone;
      if (st == FetchState::kPending) {
        ImGui::TextDisabled("Calcul en cours...");
      } else if (st == FetchState::kReady && it != dmg_cache_.end()) {
        const DamageEst& d = it->second;
        if (d.status == 1) {
          ImGui::TextDisabled("Sort non offensif (aucun dégât).");
        } else if (d.status == 2) {
          ImGui::TextDisabled("Monstre introuvable (ID invalide ?).");
        } else if (d.status != 0) {
          ImGui::TextDisabled("Estimation indisponible.");
        } else {
          const char* atk = (d.atk_type == 1) ? "Physique"
                          : (d.atk_type == 2) ? "Magique"
                                              : "Divers";
          if (d.target == 0xFFFFFFFFu)
            ImGui::TextDisabled("Cible : toi-même (miroir PvP)");
          else if (d.target == 0)
            ImGui::TextDisabled("Cible : neutre 0 def (dégâts bruts)");
          else if (!d.target_name.empty())
            ImGui::Text("Cible : %s (#%u)", d.target_name.c_str(), d.target);
          else
            ImGui::Text("Cible : monstre #%u", d.target);
          ImGui::Text("Niveau %u  -  %s", d.skill_lv, atk);
          ImGui::Text("%lld - %lld  (moy. %lld)",
                      static_cast<long long>(d.dmg_min),
                      static_cast<long long>(d.dmg_max),
                      static_cast<long long>(d.dmg_avg));
          if (d.hits > 1) {
            ImGui::SameLine();
            ImGui::Text("x%u coups", d.hits);
          }
          ImGui::TextDisabled("Inclut ton stuff / buffs. Neutre = avant def/elem.");
        }
      }
      ImGui::EndTabItem();
    }
    return;
  }

  // ── ITEM : onglets « Monstres » (non-boss) et « Boss / MVP » ──────────────
  if (ImGui::BeginTabItem("Monstres")) {
    if (const TechData* td = tech_body(kScopeNormal))
      RenderDropTable(*td, "tech_drops_normal",
                      CacheKey(w.id, false, kScopeNormal), /*show_type=*/false);
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Boss / MVP")) {
    if (const TechData* td = tech_body(kScopeMvp))
      RenderDropTable(*td, "tech_drops_mvp", CacheKey(w.id, false, kScopeMvp),
                      /*show_type=*/true);
    ImGui::EndTabItem();
  }

  // Script + combos partagent la même requête serveur (0x0F11) mise en cache par
  // id. On la déclenche dès qu'un des deux onglets est actif, et on renvoie
  // l'entrée prête (ou null tant qu'elle ne l'est pas).
  auto script_body = [&]() -> const ScriptData* {
    RequestItemScript(w.id);
    auto it = script_cache_.find(w.id);
    const FetchState st =
        (it != script_cache_.end()) ? it->second.state : FetchState::kNone;
    if (st == FetchState::kPending) {
      ImGui::TextDisabled("Chargement...");
      return nullptr;
    }
    if (st != FetchState::kReady || it == script_cache_.end()) return nullptr;
    if (it->second.status != 0) {
      ImGui::TextDisabled("Script indisponible (item introuvable serveur).");
      return nullptr;
    }
    return &it->second;
  };

  // Bloc « éditeur » : pretty-print + coloration syntaxique rAthena, fond sombre
  // fixe, bouton « Copier » (cf. DrawScriptCode).
  auto draw_code = [](const char* box_id, const std::string& text) {
    DrawScriptCode(box_id, text);
  };

  // ── Onglet « Script » : source brute (principal / équip / déséquip) ────────
  if (ImGui::BeginTabItem("Script")) {
    if (const ScriptData* sd = script_body()) {
      if (sd->main.empty() && sd->equip.empty() && sd->unequip.empty()) {
        ImGui::TextDisabled("Cet item n'a aucun script.");
      } else {
        if (!sd->main.empty()) {
          ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.35f, 1.0f), "Script");
          draw_code("##script_main", sd->main);
        }
        if (!sd->equip.empty()) {
          ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.35f, 1.0f), "EquipScript");
          draw_code("##script_equip", sd->equip);
        }
        if (!sd->unequip.empty()) {
          ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.35f, 1.0f), "UnEquipScript");
          draw_code("##script_unequip", sd->unequip);
        }
      }
    }
    ImGui::EndTabItem();
  }

  // ── Onglet « Combos » : combos auxquels l'item participe + leur script ─────
  if (ImGui::BeginTabItem("Combos")) {
    if (const ScriptData* sd = script_body()) {
      if (sd->combos.empty()) {
        ImGui::TextDisabled("Cet item ne fait partie d'aucun combo.");
      } else {
        // Rend un membre de combo comme LIEN vers l'itemdb du site : bleu (or pour
        // l'item courant), soulignement + curseur main au survol, aperçu de
        // description au survol (le MÊME tooltip RO que l'inventaire/storage, via
        // RenderCardTooltip), clic gauche -> base de données du site, clic droit ->
        // description complète dans la fenêtre desc (différée au prochain tick).
        auto draw_member = [&](const auto& mem) {
          char lbl[96];
          if (!mem.second.empty())
            std::snprintf(lbl, sizeof(lbl), "%s", mem.second.c_str());
          else
            std::snprintf(lbl, sizeof(lbl), "#%u", mem.first);
          const bool self = (mem.first == w.id);
          const ImU32 col = self ? IM_COL32(230, 200, 90, 255)    // item courant : or
                                 : IM_COL32(26, 77, 217, 255);    // lien : bleu
          ImGui::PushStyleColor(ImGuiCol_Text, col);
          ImGui::TextUnformatted(lbl);
          ImGui::PopStyleColor();
          if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y),
                                                ImVec2(mx.x, mx.y), col);
            RenderCardTooltip(mem.first);   // aperçu RO partagé (nom + desc + illust)
          }
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            OpenCardDbLink(mem.first);      // base de données du site (page itemdb)
          if (IsLastItemRightClicked())
            pending_card_open_ = mem.first; // desc complète au prochain OnTick
        };
        for (size_t i = 0; i < sd->combos.size(); ++i) {
          const ComboInfo& c = sd->combos[i];
          ImGui::PushID(static_cast<int>(i));
          ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.55f, 1.0f), "Combo %zu",
                             i + 1);
          // Membres cliquables joints par « + » ; wrap manuel (ImGui ne wrappe pas
          // une série de widgets : on décide du SameLine selon la largeur dispo).
          const float visible_x2 =
              ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
          for (size_t m = 0; m < c.members.size(); ++m) {
            if (m) {
              ImGui::SameLine(0, 0);
              ImGui::TextUnformatted(" + ");
              // le prochain membre tient-il sur la ligne courante ?
              char nbuf[96];
              if (!c.members[m].second.empty())
                std::snprintf(nbuf, sizeof(nbuf), "%s",
                              c.members[m].second.c_str());
              else
                std::snprintf(nbuf, sizeof(nbuf), "#%u", c.members[m].first);
              if (ImGui::GetItemRectMax().x + ImGui::CalcTextSize(nbuf).x <
                  visible_x2)
                ImGui::SameLine(0, 0);
            }
            draw_member(c.members[m]);
          }
          if (!c.script.empty()) {
            char bid[24];
            std::snprintf(bid, sizeof(bid), "##combo_scr%zu", i);
            draw_code(bid, c.script);
          } else {
            ImGui::TextDisabled("(pas de script)");
          }
          if (i + 1 < sd->combos.size()) ImGui::Separator();
          ImGui::PopID();
        }
      }
    }
    ImGui::EndTabItem();
  }
}

// Émet le bouton de rapport de bug de la fenêtre de description. ISOLÉ dans une
// fonction SANS __try : RenderItemWindow/RenderSkillWindow contiennent un __try, et
// MSVC (C2712) interdit tout objet à destructeur (ici std::string via *Context)
// dans une fonction qui utilise __try. On confine donc les std::string ici.
static void EmitDescBugButton(uint32_t id, const char* name, bool is_skill,
                              const char* imgui_id) {
  auto* br = Bourgeon::Instance().bug_report();
  if (!br || !br->enabled()) return;  // opt-out : pas de bouton ni de marge basse
  const char* nm = name ? name : "";
  ImGui::Spacing();
  br->Button(is_skill ? BugReport::SkillContext(id, nm)
                      : BugReport::ItemContext(id, nm),
             imgui_id);
  // Marge basse OBLIGATOIRE : la fenêtre desc a un cadre décoratif (~14px) en bas
  // + un clip à -10px. Sans réserve, le bouton (dernier item) est tronqué par le
  // cadre même défilé tout en bas. 18px > 14px => il dégage entièrement au-dessus.
  ImGui::Dummy(ImVec2(0.0f, 18.0f));
}

// Reproduit la fenêtre de description d'ITEM en ImGui (Option A). Le pointeur
// natif est relu FRAIS + validé ici (indépendant de OnTick). SEH sur les
// lectures/appels natifs ; le rendu ImGui reste hors __try (objets C++).
void ItemDescWindow::RenderItemWindow() {
  uint8_t* iwnd = ReadValidWnd(kItemWndSlot, kItemVTable);
  if (!iwnd) return;
  // Fenêtre de comparaison (équipé) éventuelle (id 0xea).
  uint8_t* cwnd = ReadValidWnd(kCompareWndSlot, kCompareVTable);

  // Extraction SEH (POD only), puis résolution icône hors SEH.
  ItemExtract ie{}; ExtractItem(iwnd, &ie);
  IconTex iicon = ResolveIcon(item_.id, ie);

  const bool has_cmp_data = (cwnd != nullptr && compare_.open);
  const bool has_cmp = has_cmp_data && cmp_show_equipped_;  // toggle par-vue
  ItemExtract ce{}; IconTex cicon;
  if (has_cmp) { ExtractItem(cwnd, &ce); cicon = ResolveIcon(compare_.id, ce); }

  // ── Rendu ImGui (fond clair : le texte RO est conçu pour un fond pâle) ─────
  // Largeur mini DYNAMIQUE : large (500) seulement quand la comparaison 2 colonnes
  // est affichée ; en vue simple, 320 -> la fenêtre peut être étroite (avant, le
  // 500 bloquait même un item seul). Redimensionnable, ImGui mémorise la taille.
  const float min_w = has_cmp ? 500.0f : 320.0f;
  ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, 300.0f),
                                      ImVec2(1800.0f, 900.0f));
  ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
  if (item_need_pos_) {
    // Mode « près de la souris » : on force la position au curseur. Mode « dernière
    // position » : on NE touche PAS la position (ImGui réutilise celle mémorisée
    // via le ###id).
    if (desc_spawn_at_cursor_)
      ImGui::SetNextWindowPos(
          ImVec2(static_cast<float>(item_spawn_x_),
                 static_cast<float>(item_spawn_y_)),
          ImGuiCond_Always, DescAnchorPivot(desc_anchor_));
    item_need_pos_ = false;
  }
  // Remontée au 1er plan, DÉCOUPLÉE du placement (elle était conditionnée au bloc
  // ci-dessus, donc perdue dès que GetCursorPos échouait, et jamais demandée quand
  // la desc déjà ouverte ne faisait que changer d'objet). Le drapeau est posé par
  // OnTick (ouverture / changement d'objet) et, plus tôt et plus sûrement, par le
  // hook OnMsg 0x18 — le seul point qui voit TOUTES les ouvertures, y compris
  // celles déclenchées par nos propres fenêtres (inventaire, storage, chariot,
  // boutique, fiche de perso…), d'où la desc sortait sinon DERRIÈRE la fenêtre
  // d'où venait le clic.
  const bool raising = item_need_raise_;
  if (raising) {
    ImGui::SetNextWindowFocus();
    item_need_raise_ = false;
  }
  ImGui::SetNextWindowBgAlpha(1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(245, 243, 232, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBg,       IM_COL32(120, 110, 90, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(120, 110, 90, 255));
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));

  const ImU32 black = IM_COL32(0, 0, 0, 255);
  // Dessine une colonne (icône + titre + ID + description + bloc technique).
  auto draw_col = [&](const char* selId, const char* header, IconTex icon,
                      const ItemExtract& e, const DescWindow& snap,
                      uint8_t* wnd) {
    if (header) { ImGui::TextDisabled("%s", header); ImGui::Separator(); }
    // VRAIE carte -> illustration cardBmp (réduite) + mouseover pleine taille.
    // Sinon -> icône collection ; survol d'un équipement à viewID -> aperçu du perso
    // (comme le survol du « ViewID : N »). GetCardIllust est auto-gatée (renvoie vide
    // pour tout ce qui n'est pas dans la DB carte), donc appel inconditionnel : les
    // non-cartes (ex. 617 boîte) gardent leur icône collection.
    IconTex cill = GetCardIllust(snap.id);
    const bool is_card = cill.tex && cill.w > 0 && cill.h > 0;
    const IconTex& ic = is_card ? cill : icon;
    if (ic.tex && ic.w > 0 && ic.h > 0) {
      // Taille native, ratio préservé, plafonnée (évite la déformation 48x48).
      const float kMax = 120.0f;
      float w = static_cast<float>(ic.w), h = static_cast<float>(ic.h);
      const float big = (w > h) ? w : h;
      if (big > kMax) { const float s = kMax / big; w *= s; h *= s; }
      ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(w, h));
      if (ImGui::IsItemHovered()) {
        if (is_card) {  // mouseover illustration carte pleine taille
          ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
          ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
          ImGui::BeginTooltip();
          ImGui::Image(reinterpret_cast<ImTextureID>(cill.tex),
                       ImVec2(static_cast<float>(cill.w),
                              static_cast<float>(cill.h)));
          ImGui::EndTooltip();
          ImGui::PopStyleColor(2);
        } else {  // équipement à viewID (ou costume à hat effect) -> aperçu du perso
          auto* bi = Bourgeon::Instance().basic_info();
          if (bi) {
            const int ord = bi->ItemToHatOrdinal(static_cast<int>(snap.id));
            const bool can_sprite = e.view_id != 0 && bi->CanPreview(e.emplacement);
            if (can_sprite || ord != 0) {
              ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
              bi->RenderItemPreviewTooltip(e.view_id, e.emplacement, ord);
            }
          }
        }
      }
      ImGui::SameLine();
    }
    ImGui::BeginGroup();
    // Nom coloré dans le corps : TOUJOURS en comparaison (header). En mode simple,
    // seulement quand le nom porte un segment coloré (forgé/signé = nom du forgeron
    // rouge/bleu) OU une ombre rouge (item CASSÉ, info+0x5d!=0) -> on montre ce que
    // la barre de titre ne peut pas rendre ; un item normal garde juste son nom
    // dans la barre de titre.
    if (header || e.hl_start >= 0 || e.name_shadow)
      DrawTitle(e.name, e.hl_start, e.hl_end, e.hl_col, IM_COL32(0, 0, 0, 255),
                e.name_shadow);

    // La 1ère ligne de desc item est TOUJOURS le lien database (<URL>ItemID..),
    // qui affiche déjà l'ID -> on l'utilise À LA PLACE de la ligne "ID : N"
    // (redondante) et on la SKIP du corps. Repli sur "ID : N" si pas de lien.
    const int skip_db =
        (e.line_count > 0 && std::strstr(e.lines[0], "<URL>")) ? 1 : 0;
    if (skip_db) {
      char dbid[64];
      std::snprintf(dbid, sizeof(dbid), "%s_db", selId);
      SelectableColoredText(dbid, e.lines, 1, black);
    } else {
      ImGui::TextDisabled("ID : %u", snap.id);
    }
    // La ligne "ViewID : N" (juste après le lien DB) est remontée SOUS l'ItemID.
    const bool has_vid =
        (e.line_count > skip_db && std::strstr(e.lines[skip_db], "ViewID"));
    if (has_vid) {
      auto* bi = Bourgeon::Instance().basic_info();
      const bool previewable =
          e.view_id != 0 && bi && bi->CanPreview(e.emplacement);
      if (previewable) {
        // ViewID en lien bleu (signale l'interaction) + hint molette. Survol ->
        // aperçu du perso portant l'item (sprites capturés, cf. BasicInfo).
        ImGui::TextColored(ImVec4(0.16f, 0.42f, 0.88f, 1.0f), "ViewID : %d",
                           e.view_id);
        const bool hov = ImGui::IsItemHovered();
        ImGui::SameLine(0, 6);
        ImGui::TextDisabled("(molette : tourner)");
        if (hov) {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          // + hat effect superposé si l'item en a un (costume viewid AVEC hateffect).
          bi->RenderItemPreviewTooltip(e.view_id, e.emplacement,
                                       bi->ItemToHatOrdinal(static_cast<int>(snap.id)));
        }
      } else {
        char vid[64];
        std::snprintf(vid, sizeof(vid), "%s_vid", selId);
        SelectableColoredText(vid, e.lines + skip_db, 1, black);
      }
    }
    const int skip = skip_db + (has_vid ? 1 : 0);

    // Bouton +/- alootid (réintégré depuis l'overlay autonome) via MoonlightUi.
    if (auto* mui = Bourgeon::Instance().moonlight_ui()) {
      const bool in = mui->IsAlootId(snap.id);
      char blbl[48];
      std::snprintf(blbl, sizeof(blbl), "%s alootid%s", in ? "-" : "+", selId);
      ImGui::PushStyleColor(ImGuiCol_Button,
                            in ? ImVec4(0.65f, 0.18f, 0.18f, 1.0f)
                               : ImVec4(0.18f, 0.48f, 0.18f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
      if (ImGui::SmallButton(blbl)) {
        if (in) mui->RemoveAlootId(snap.id);
        else    mui->AddAlootId(snap.id);
      }
      ImGui::PopStyleColor(2);
    }

    // Bouton « Probabilité » (rejoue le natif « View Probability Info » -> ouvre
    // la fenêtre 0x271c des taux refine/enchant pour cet item).
    if (ItemHasProbability(snap.id)) {
      char pb[48];
      std::snprintf(pb, sizeof(pb), "Probabilités%s", selId);
      if (ImGui::SmallButton(pb)) CallDescButton(wnd, kCmdProbability);
    }

    // Boutons « Lire » / « Lecture auto » des LIVRES (rejouent les 2 boutons
    // natifs +0x1b8/+0x1bc : ouvrent book\<id>.txt dans la fenêtre livre 0x6a).
    // ⚠️ Cette fenêtre-là reste NATIVE : elle se compose avant ImGui, donc elle
    // passera SOUS la desc si les deux se recouvrent (repro ImGui = TODO).
    if (ItemIsReadableBook(snap.id)) {
      char rb[48];
      std::snprintf(rb, sizeof(rb), "Lire%s", selId);
      if (ImGui::SmallButton(rb)) {
        CallDescButton(wnd, kCmdReadBook);
        // La fenêtre livre vient d'être créée + affichée : on la cache DANS LA
        // MÊME frame (avant EndScene) -> zéro flicker, comme le hook OnMsg des
        // descs. Le prochain OnTick prendra le relais.
        if (show_book_panel_) HideBookWindow(FindBookWindow());
      }
      ImGui::SameLine();
      std::snprintf(rb, sizeof(rb), "Lecture auto%s", selId);
      // Lecture auto = le personnage récite le livre dans le chat (le client
      // n'affiche alors AUCUNE fenêtre) — pas de panneau à reproduire.
      if (ImGui::SmallButton(rb)) CallDescButton(wnd, kCmdAutoReadBook);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Le personnage lit le livre à voix haute, ligne par "
                          "ligne, dans le chat.");
    }
    ImGui::EndGroup();

    ImGui::Separator();
    SelectableColoredText(selId, e.lines + skip, e.line_count - skip, black);
    // NB : cartes/enchants + options aléatoires ne sont PLUS dessinés ici. Ils
    // sont sortis dans des panneaux SATELLITES (DrawInstancePanels), ancrés à
    // l'extérieur, SOUS la fenêtre principale (façon fenêtres natives).
  };

  // Regroupement de FOCUS : la desc + ses panneaux satellites remontent ENSEMBLE au
  // premier plan quand on interagit avec l'un d'eux (fenêtres séparées => sinon les
  // panneaux passent SOUS les autres fenêtres). Noms des panneaux collectés au rendu
  // ; sat_foc = true si un panneau est focus cette frame. Réordonnancement après.
  // Tableau POD (le SEH de fermeture est désormais isolé dans SafeCloseWindowId, donc
  // les objets C++ à déroulement sont permis ici ; on garde ce tableau simple).
  char sat_names[8][64];
  int  sat_name_count = 0;
  bool sat_foc = false;

  // Panneaux SATELLITES (cartes + options) : fenêtres ImGui SKINNÉES RO séparées
  // (cadre sysbox ro::BeginRoDescPanel = même look que la desc), ancrées sous la
  // desc. Fenêtres séparées (AUCUN dessin sur la draw list parente) => curseur et
  // rendu SÛRS. Boîtes d'options bordées à la main (AddRect, arrondies). Cartes =
  // vrais Selectable (survol/clic gérés par ImGui). `tl` = coin haut-gauche ;
  // renvoie le Y du bas. Le z-order est géré par le regroupement de focus (les
  // panneaux remontent avec la desc), pas par un dessin sur la draw list parente
  // (qui avait cassé curseur+skin le 2026-07-08).
  auto draw_satellites = [&](const ItemExtract& e, const char* tag,
                             ImVec2 tl) -> float {
    // Rangées = jusqu'au DERNIER slot occupé (index+1), au minimum le nb
    // d'emplacements. ⚠️ PAS le COMPTE de cartes non nulles : avec un trou (ex.
    // carte en 0, enchants en 2 et 3, slot 1 vide) le compte = 3 sauterait le
    // slot 3. On prend donc le plus haut index occupé.
    int last_slot = -1;
    for (int i = 0; i < kMaxCards; ++i) if (e.cards[i] != 0) last_slot = i;
    int slot_rows = (e.card_slots > last_slot + 1) ? e.card_slots : last_slot + 1;
    if (slot_rows > kMaxCards) slot_rows = kMaxCards;
    if (e.opt_count <= 0 && slot_rows <= 0) return tl.y;

    const ImVec4 brown(0.30f, 0.24f, 0.10f, 1.0f);
    const ImGuiWindowFlags wf =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    const float lh = ImGui::GetTextLineHeight();
    char buf[160];
    float y = tl.y;

    // Panneau SKINNÉ RO (cadre sysbox, comme la fenêtre desc) ; marge +2px gérée
    // dans BeginRoDescPanel (WindowPadding e+2). `y` capturé par ref (déplacé
    // entre les panneaux).
    auto begin_panel = [&](const char* id) -> bool {
      ImGui::SetNextWindowPos(ImVec2(tl.x, y));
      const bool o = ro::BeginRoDescPanel(id, wf);
      if (sat_name_count < 8) {                           // pour le regroupement focus
        std::strncpy(sat_names[sat_name_count], id, 63);
        sat_names[sat_name_count][63] = '\0';
        ++sat_name_count;
      }
      if (ImGui::IsWindowFocused()) sat_foc = true;
      return o;
    };
    auto end_panel = [&]() { ro::EndRoDescPanel(); };

    // ── OPTIONS : boîte bordée arrondie par option (bordure dessinée main) ──────
    if (e.opt_count > 0) {
      char wid[64];
      std::snprintf(wid, sizeof(wid), "##optpanel_%s", tag);
      if (begin_panel(wid)) {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        const ImU32 tcol = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 bcol = IM_COL32(0xC2, 0xC2, 0xC2, 255);
        const float bpx = 6.0f, bpy = 2.0f, gap = 3.0f;
        const int n = (e.opt_count < kMaxOpts) ? e.opt_count : kMaxOpts;
        float boxw = 40.0f;
        for (int i = 0; i < n; ++i) {
          const char* on = GetOptName(e.opts[i].index, e.opts[i].value);
          if (!on)
            std::snprintf(buf, sizeof(buf), "Option #%d : valeur %d (param %u)",
                          (int)e.opts[i].index, (int)e.opts[i].value,
                          (unsigned)e.opts[i].param);
          const float w = ImGui::CalcTextSize(on ? on : buf).x;
          if (w > boxw) boxw = w;
        }
        boxw += bpx * 2.0f;
        const float bh = lh + bpy * 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, gap));
        for (int i = 0; i < n; ++i) {
          const char* on = GetOptName(e.opts[i].index, e.opts[i].value);
          if (!on)
            std::snprintf(buf, sizeof(buf), "Option #%d : valeur %d (param %u)",
                          (int)e.opts[i].index, (int)e.opts[i].value,
                          (unsigned)e.opts[i].param);
          const ImVec2 p0 = ImGui::GetCursorScreenPos();
          const ImVec2 p1(p0.x + boxw, p0.y + bh);
          wdl->AddRect(p0, p1, bcol, 4.0f, 0, 1.0f);         // bordure 1px arrondie
          wdl->AddText(ImVec2(p0.x + bpx, p0.y + bpy), tcol, on ? on : buf);
          ImGui::Dummy(ImVec2(boxw, bh));
        }
        ImGui::PopStyleVar(1);
        y = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y + 4.0f;
      }
      end_panel();
    }

    // ── CARTES : titre + une ligne (icône + nom cliquable) par emplacement ──────
    if (slot_rows > 0) {
      char wid[64];
      std::snprintf(wid, sizeof(wid), "##cardpanel_%s", tag);
      if (begin_panel(wid)) {
        if (e.card_slots > 0)
          ImGui::TextColored(brown, "Cartes / Enchants (%d emplacement%s)",
                             e.card_slots, e.card_slots > 1 ? "s" : "");
        else
          ImGui::TextColored(brown, "Cartes / Enchants");
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
        for (int i = 0; i < slot_rows; ++i) {
          if (e.cards[i] == 0) {  // emplacement vide de l'item
            ImGui::TextDisabled("[Emplacement vide]");
            continue;
          }
          char clbl[160];
          if (e.card_names[i][0])
            std::snprintf(clbl, sizeof(clbl), "%s (#%u)##sc%d_%s",
                          e.card_names[i], e.cards[i], i, tag);
          else
            std::snprintf(clbl, sizeof(clbl), "#%u##sc%d_%s", e.cards[i], i, tag);
          IconTex cicon = GetCardIcon(e.cards[i]);
          if (cicon.tex && cicon.h > 0) {
            const float th = ImGui::GetTextLineHeight();
            ImGui::Image(reinterpret_cast<ImTextureID>(cicon.tex),
                         ImVec2(th * cicon.w / cicon.h, th));
            ImGui::SameLine(0, 2);
          }
          ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(26, 77, 217, 255));  // lien
          ImGui::Selectable(clbl, false, ImGuiSelectableFlags_AllowDoubleClick);
          ImGui::PopStyleColor();
          if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            RenderCardTooltip(e.cards[i]);
          }
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            OpenCardDbLink(e.cards[i]);
          // Clic droit -> ouvre la description COMPLÈTE de la carte/enchant dans la
          // fenêtre desc (remplace l'item courant, comme un lien de carte natif). On
          // diffère l'appel natif au prochain OnTick (hors rendu ImGui).
          if (IsLastItemRightClicked())
            pending_card_open_ = e.cards[i];
        }
        ImGui::PopStyleVar(1);
        y = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y + 4.0f;
      }
      end_panel();
    }
    return y;
  };

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoFocusOnAppearing;
  char title[160];
  if (has_cmp)
    std::snprintf(title, sizeof(title), "Comparaison###itemdesc_item");
  else
    // Item cassé (name_shadow) -> suffixe " - Broken" dans le titre.
    std::snprintf(title, sizeof(title), "%s%s###itemdesc_item",
                  ie.name[0] ? ie.name : "Description",
                  ie.name_shadow ? " - Broken" : "");

  // Coins arrondis (comme moonlight_ui) : fenêtre moins « rude ».
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  bool open = true;
  // Ombre ROUGE du titre pour un item cassé, uniquement hors comparaison (le titre
  // de comparaison = "Comparaison", pas de nom d'item).
  const unsigned int title_shadow = has_cmp ? 0u : ie.name_shadow;
  const bool visible = ro::BeginRoDescWindow(title, &open, flags, title_shadow);
  // Anti hors-écran : après le placement (curseur/mémoire) et le resize, on
  // ramène la fenêtre entièrement dans l'écran (bords droit/bas puis haut/gauche).
  {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const ImVec2 pos  = ImGui::GetWindowPos();
    const ImVec2 sz   = ImGui::GetWindowSize();
    float nx = pos.x, ny = pos.y;
    if (nx + sz.x > disp.x) nx = disp.x - sz.x;
    if (ny + sz.y > disp.y) ny = disp.y - sz.y;
    if (nx < 0.0f) nx = 0.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (nx != pos.x || ny != pos.y)
      ImGui::SetWindowPos(ImVec2(nx, ny));
  }
  // Le texte desc est dessiné via la draw list (SelectableColoredText) => clippé au
  // CORPS de la fenêtre (qui inclut le cadre sysbox 14px du bas), PAS au content
  // region -> WindowPadding ne le contraint pas. On CLIPPE donc le contenu au-dessus
  // du cadre du bas pour qu'il ne rende pas dedans (le curseur natif RO n'est pas
  // affecté : la scrollbar est peinte après, hors de ce clip).
  {
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    ImGui::PushClipRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y - 10.0f), true);
  }
  if (visible && ImGui::BeginTabBar("##itemtabs")) {
    if (ImGui::BeginTabItem("Description")) {
      // Toggle comparaison : replie/déplie la colonne « Équipé » (visible seulement
      // quand la donnée de comparaison native existe).
      if (has_cmp_data) {
        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, (float)(int)(style.FramePadding.y * 0.60f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, (float)(int)(style.ItemSpacing.y * 0.60f)));
        // Persiste immédiatement l'état (la checkbox vit dans la fenêtre desc, pas
        // dans le panneau de réglages qui déclenche le save autrement).
        if (ro::RoCheckbox("Comparer", &cmp_show_equipped_))
          if (auto* mui = Bourgeon::Instance().moonlight_ui()) mui->SaveSettings();
        ImGui::PopStyleVar(2);
        ImGui::Separator();
      }
      if (has_cmp) {
        // Deux colonnes redimensionnables qui se partagent la largeur (le texte
        // se wrap). Ordre NATIF : ÉQUIPÉ à gauche, sélection à droite.
        if (ImGui::BeginTable("##cmp", 2,
                              ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchSame)) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          draw_col("##seltext_eq", "Equipé", cicon, ce, compare_, cwnd);
          ImGui::TableNextColumn();
          draw_col("##seltext_obj", "Objet", iicon, ie, item_, iwnd);
          ImGui::EndTable();
        }
      } else {
        draw_col("##seltext_single", nullptr, iicon, ie, item_, iwnd);
      }
      ImGui::EndTabItem();
    }
    // Onglets drops de l'objet principal (item_). En comparaison, ce sont les
    // sources de l'objet évalué, pas de l'équipé.
    RenderTechTabs(item_);
    ImGui::EndTabBar();
  }
  // Rapport de bug contextuel : id + nom de l'objet joints automatiquement.
  // (Émis hors de cette fonction : elle contient un __try -> C2712 sinon.)
  EmitDescBugButton(item_.id, ie.name, /*is_skill=*/false, "itemdesc_bug");
  ImGui::PopClipRect();  // fin du clip « contenu au-dessus du cadre du bas »
  // Ancre des satellites = rect de la fenêtre principale (capturé AVANT End, après
  // le clamp anti-hors-écran). Les panneaux (fenêtres séparées) sont dessinés
  // APRÈS End. En comparaison : équipé sous la moitié gauche, objet sous la droite.
  const ImVec2 main_pos  = ImGui::GetWindowPos();
  const ImVec2 main_size = ImGui::GetWindowSize();
  // Focus de la desc (racine + enfants), capturé pendant qu'elle est courante.
  const bool desc_foc =
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  ro::EndRoDescWindow();
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(3);

  if (visible) {
    const float top = main_pos.y + main_size.y + 4.0f;
    if (has_cmp) {
      draw_satellites(ce, "eq", ImVec2(main_pos.x, top));
      draw_satellites(ie, "obj", ImVec2(main_pos.x + main_size.x * 0.5f, top));
    } else {
      draw_satellites(ie, "obj", ImVec2(main_pos.x, top));
    }
    // Regroupement de focus : si la desc OU un panneau vient de GAGNER le focus
    // (transition), on remonte tout le groupe au 1er plan ensemble (panneaux puis
    // desc => desc au-dessus, panneaux juste en dessous, le tout au-dessus des
    // autres fenêtres). Uniquement sur la transition -> aucune bagarre de focus par
    // frame. Corrige « les panneaux passent sous l'inventaire ».
    // `raising` : même réordonnancement sur une ouverture. Le SetNextWindowFocus de
    // la desc ne concerne QU'ELLE ; les satellites, eux, apparaissent à cette frame
    // et ImGui les empile au-dessus (fenêtres neuves) -> sans ce passage, les
    // panneaux cartes/options recouvraient la desc qu'ils accompagnent.
    static bool s_grp_foc_prev = false;
    const bool grp_foc = desc_foc || sat_foc;
    if ((grp_foc && !s_grp_foc_prev) || raising) {
      for (int i = 0; i < sat_name_count; ++i) ImGui::SetWindowFocus(sat_names[i]);
      ImGui::SetWindowFocus(title);
    }
    s_grp_foc_prev = grp_foc;
  }

  // X ImGui -> ferme les fenêtres natives (item 0xc + comparaison 0xea). SEH isolée
  // dans SafeCloseWindowId (sinon C2712 : __try + temporaires C++ dans cette fonction).
  if (!open) {
    SafeCloseWindowId(0xc);
    SafeCloseWindowId(0xea);
  }
}

// Reproduit la fenêtre de description de SKILL (classe 0x2e) en ImGui : nom+SP
// (this+0xec) + description via Lua GetSkillDescript (markup rendu comme l'item).
void ItemDescWindow::RenderSkillWindow() {
  uint8_t* wnd = ReadValidWnd(kSkillWndSlot, kSkillVTable);
  if (!wnd) return;

  // Données mises en cache par id (évite un appel Lua par frame).
  static uint32_t s_id = 0;
  static ItemExtract s_e{};
  if (skill_.id != s_id) {
    ExtractSkill(wnd, skill_.id, &s_e);
    s_id = skill_.id;
  }

  ImGui::SetNextWindowSizeConstraints(ImVec2(500.0f, 300.0f),
                                      ImVec2(1800.0f, 900.0f));
  ImGui::SetNextWindowSize(ImVec2(520.0f, 360.0f), ImGuiCond_FirstUseEver);
  if (skill_need_pos_) {
    if (desc_spawn_at_cursor_)  // sinon : dernière position mémorisée par ImGui
      ImGui::SetNextWindowPos(ImVec2(static_cast<float>(skill_spawn_x_),
                                     static_cast<float>(skill_spawn_y_)),
                              ImGuiCond_Always, DescAnchorPivot(desc_anchor_));
    skill_need_pos_ = false;
  }
  // Au 1er plan à chaque ouverture, y compris un changement de skill dans une desc
  // déjà ouverte (clic droit sur une autre icône du grimoire). Découplé du
  // placement, cf. RenderItemWindow.
  if (skill_need_raise_) {
    ImGui::SetNextWindowFocus();
    skill_need_raise_ = false;
  }
  ImGui::SetNextWindowBgAlpha(1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(245, 243, 232, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBg,       IM_COL32(120, 110, 90, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(120, 110, 90, 255));
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoFocusOnAppearing;
  char title[160];
  std::snprintf(title, sizeof(title), "%s###itemdesc_skill",
                s_e.name[0] ? s_e.name : "Skill");

  // Coins arrondis (comme moonlight_ui).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  bool open = true;
  const bool visible = ro::BeginRoDescWindow(title, &open, flags);
  {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const ImVec2 pos  = ImGui::GetWindowPos();
    const ImVec2 sz   = ImGui::GetWindowSize();
    float nx = pos.x, ny = pos.y;
    if (nx + sz.x > disp.x) nx = disp.x - sz.x;
    if (ny + sz.y > disp.y) ny = disp.y - sz.y;
    if (nx < 0.0f) nx = 0.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (nx != pos.x || ny != pos.y) ImGui::SetWindowPos(ImVec2(nx, ny));
  }
  if (visible && ImGui::BeginTabBar("##skilltabs")) {
    if (ImGui::BeginTabItem("Description")) {
      // Nom+SP déjà dans la barre de titre, ID/Max Level déjà dans la desc
      // native (box head) -> on n'affiche QUE la description ici.
      // reflow=true : les lignes viennent des rich-text box natifs déjà wrappés (le jeu
      // coupe en plein mot en comptant les tokens ^RRGGBB) -> on re-fusionne + re-wrap.
      SelectableColoredText("##seltext_skill", s_e.lines, s_e.line_count,
                            IM_COL32(0, 0, 0, 255), 0.0f, /*reflow=*/true);
      ImGui::EndTabItem();
    }
    RenderTechTabs(skill_);  // onglets Infos techniques / Dégâts
    ImGui::EndTabBar();
  }
  // Rapport de bug contextuel : id + nom de la compétence joints.
  EmitDescBugButton(skill_.id, s_e.name, /*is_skill=*/true, "skilldesc_bug");
  ro::EndRoDescWindow();
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(3);

  if (!open) SafeCloseWindowId(0x2e);
}

// Reproduit la fenêtre LIVRE (UIBookWnd 0x6a) en ImGui. Même principe que la desc :
// la fenêtre native reste vivante (elle charge book\<id>.txt, wrappe le texte,
// tient la pagination et le signet) mais son rendu est masqué, et on redessine la
// page courante à EndScene, donc AU-DESSUS de l'overlay.
void ItemDescWindow::RenderBookWindow() {
  uint8_t* wnd = FindBookWindow();
  if (!wnd) return;

  BookExtract be{};
  ExtractBookPage(wnd, &be);
  // Les .txt de livre sont écrits dans la code-page du client (« … », accents) :
  // conversion en UTF-8, sinon ImGui affiche un losange à leur place.
  LocalizeLines(be.lines, be.line_count);

  ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 240.0f),
                                      ImVec2(1800.0f, 1000.0f));
  ImGui::SetNextWindowSize(ImVec2(620.0f, 460.0f), ImGuiCond_FirstUseEver);
  if (book_need_pos_) {
    if (desc_spawn_at_cursor_)  // sinon : dernière position mémorisée par ImGui
      ImGui::SetNextWindowPos(ImVec2(static_cast<float>(book_spawn_x_),
                                     static_cast<float>(book_spawn_y_)),
                              ImGuiCond_Always, DescAnchorPivot(desc_anchor_));
    book_need_pos_ = false;
  }
  // Au 1er plan à l'ouverture, découplé du placement (cf. RenderItemWindow) : le
  // livre s'ouvre depuis le bouton « Lire » de la desc, qui a le focus.
  if (book_need_raise_) {
    ImGui::SetNextWindowFocus();
    book_need_raise_ = false;
  }
  ImGui::SetNextWindowBgAlpha(1.0f);
  // Fond = la couleur déclarée par le livre lui-même (en-tête %RRGGBB du .txt,
  // lue dans la fenêtre native) — le texte RO est écrit pour ce fond pâle.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, be.bg);
  ImGui::PushStyleColor(ImGuiCol_TitleBg,       IM_COL32(120, 110, 90, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(120, 110, 90, 255));
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);

  char title[160];
  std::snprintf(title, sizeof(title), "%s ( %d / %d )###itemdesc_book",
                book_.title[0] ? book_.title : "Livre", be.page, be.pages);

  bool open = true;
  const bool visible = ro::BeginRoDescWindow(
      title, &open,
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing);
  {  // clamp anti-hors-écran (comme les autres fenêtres reproduites)
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const ImVec2 pos  = ImGui::GetWindowPos();
    const ImVec2 sz   = ImGui::GetWindowSize();
    float nx = pos.x, ny = pos.y;
    if (nx + sz.x > disp.x) nx = disp.x - sz.x;
    if (ny + sz.y > disp.y) ny = disp.y - sz.y;
    if (nx < 0.0f) nx = 0.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (nx != pos.x || ny != pos.y) ImGui::SetWindowPos(ImVec2(nx, ny));
  }
  // Commande de page demandée cette frame (jouée après End : le natif re-affiche
  // sa fenêtre en changeant de page, on la recache dans la FOULÉE = zéro flicker).
  int  page_cmd  = 0;    // commande case 6 (précédent/suivant)
  int  goto_page = 0;    // saut direct (msg 92)
  bool wheel_ok  = false;  // molette libre (pas de scroll à consommer)
  if (visible) {
    const bool first = (be.page <= 1);
    const bool last  = (be.page >= be.pages);
    ImGui::BeginDisabled(first);
    if (ImGui::SmallButton("<< Début")) goto_page = 1;
    ImGui::SameLine();
    if (ImGui::SmallButton("< Précédente")) page_cmd = kCmdBookPrev;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("Page %d / %d", be.page, be.pages);
    ImGui::SameLine();
    ImGui::BeginDisabled(last);
    if (ImGui::SmallButton("Suivante >")) page_cmd = kCmdBookNext;
    ImGui::EndDisabled();
    ImGui::Separator();

    // Lignes DÉJÀ wrappées par le client (à la largeur de la fenêtre native) : on
    // les re-fusionne et re-wrappe à la nôtre (reflow), comme la desc de skill.
    SelectableColoredText("##seltext_book", be.lines, be.line_count,
                          IM_COL32(0, 0, 0, 255), 0.0f, /*reflow=*/true);
    // Molette = tourner les pages, MAIS seulement si la fenêtre n'a rien à faire
    // défiler (sinon on volerait le scroll d'une page trop haute pour le cadre).
    wheel_ok = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
               ImGui::GetScrollMaxY() <= 0.0f;
  }
  ro::EndRoDescWindow();
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(3);

  if (wheel_ok && page_cmd == 0 && goto_page == 0) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel < 0.0f && be.page < be.pages) page_cmd = kCmdBookNext;
    else if (wheel > 0.0f && be.page > 1)   page_cmd = kCmdBookPrev;
  }

  if (page_cmd != 0) {
    CallDescButton(wnd, page_cmd);   // case 6 : le natif change de page ET se ré-affiche
    HideBookWindow(wnd);             // -> recaché dans la même frame
  } else if (goto_page != 0) {
    CallBookMsg(wnd, kMsgBookGoto, goto_page);
    HideBookWindow(wnd);
  }
  // X ImGui -> commande de fermeture NATIVE (elle sauve le signet de page dans le
  // registre avant de fermer, comme le bouton natif).
  if (!open) CallDescButton(wnd, kCmdBookClose);
}

// ── Section « ItemDescWindow » du panneau Moonlight ──────────────────────────
// Déplacée depuis moonlight_ui/panel_interface.cc : ces widgets ne pilotent
// que l'état de CE plugin. MoonlightUi ne garde que l'appel et la décision
// de sauvegarder. Rend true si un réglage a changé.
bool ItemDescWindow::DrawSettings() {
  bool changed = false;
  TextUnformatted("Descriptions modernes des items et skills.");

  changed |= ro::RoCheckbox("Panneau technique des items", &show_item_panel());
  SameLine(); HelpMarker(
      "Affiche le panneau enrichi description d'un ITEM.\n"
      "Clic droit item");

  changed |= ro::RoCheckbox("Panneau technique des skills", &show_skill_panel());
  SameLine(); HelpMarker(
      "Affiche le panneau enrichi à côté de la description d'un SKILL.\n"
      "Clic droit dans le grimoire");

  changed |= ro::RoCheckbox("Fenêtre de livre moderne", &show_book_panel());
  SameLine(); HelpMarker(
      "Redessine la fenêtre de lecture des livres en ImGui (texte "
      "sélectionnable, pages à la molette).\n"
      "OFF : fenêtre native, qui s'affiche SOUS l'interface moderne.");

  changed |= ro::RoCheckbox("Livres : ouvrir à la première page",
                            &book_reset_page());
  SameLine(); HelpMarker(
      "ON : un livre s'ouvre toujours page 1.\n"
      "OFF : comportement d'origine du client, qui reprend à la dernière page "
      "lue (signet enregistré par personnage dans le registre Windows).");

  changed |= ro::RoCheckbox("Ouvrir près de la souris", &desc_spawn_at_cursor());
  SameLine(); HelpMarker(
      "ON : la description apparaît près du curseur à chaque ouverture.\n"
      "OFF : elle réapparaît à sa dernière position connue.");

  if (desc_spawn_at_cursor()) {
    Indent();
      const char* kAnchors[] = {"Haut-gauche", "Haut-droite", "Bas-gauche","Bas-droite", "Centre"};
      changed |= ro::RoCombo("Ancrage", &desc_anchor(), kAnchors, 5);
      changed |= WheelSliderInt("Offset X", &desc_offset_x(), -400, 400, "%d px");
      changed |= WheelSliderInt("Offset Y", &desc_offset_y(), -400, 400, "%d px");
      SameLine(); HelpMarker("Décalage depuis le curseur (molette au survol pour ajuster).");
    Unindent();
  }
  return changed;
}

void ItemDescWindow::OnRenderUI() {
  // ITEM (0xc/0xea) + SKILL (0x2e) reproduits en ImGui (Option A).
  if (show_item_panel_  && item_.open)  RenderItemWindow();
  if (kSkillWindowEnabled && show_skill_panel_ && skill_.open)
    RenderSkillWindow();
  // Fenêtre LIVRE (0x6a) : ouverte depuis le bouton « Lire » de la desc.
  if (show_book_panel_ && book_.open) RenderBookWindow();
}
