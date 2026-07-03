#include "plugins/item_desc_tweaks.h"

#include <Windows.h>
#include <shellapi.h>  // ShellExecuteA (ouvrir les liens <URL>)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "shell32.lib")

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "imgui.h"
#include "plugins/bourgeon_opcodes.h"
#include "plugins/moonlight_ui.h"  // API autolootid (bouton +/- réintégré)
#include "utils/log_console.h"

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
constexpr uintptr_t kOffPosX    = 0x1c;   // int  x écran
constexpr uintptr_t kOffPosY    = 0x20;   // int  y écran

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
constexpr uintptr_t kGetBaseName  = 0x006a2b50;  // ItemSkillInfo_GetBaseName(info,out,&cap,flag)
constexpr uintptr_t kBuildName    = 0x008a0570;  // ItemSkillInfo_BuildDisplayName (titre COMPLET)
constexpr uintptr_t kGameFree     = 0x00dbbc7f;  // free() du jeu (pour le vector alloué côté jeu)
constexpr uintptr_t kGameMalloc   = 0x00dbbc4f;  // malloc() du jeu (pairé avec game_free)
constexpr uintptr_t kCloseWindow  = 0x00a2e770;  // UIWindowMgr_Close(mgr,edx,id)
constexpr uintptr_t kUIWindowMgr  = 0x0131f4e8;

// Navigation (routage <NAVI>). ABI capturée en live (bp sur 0x00b314f0) :
// __thiscall(this=navMgr, std::string map BYVAL 0x18o, int type, int flags,
// int a24, int x, int y, int a30). Pour un lien navi item : type=field3,
// x=field1, y=field2 ; flags=1, a24=1, a30=0 (constantes observées).
constexpr uintptr_t kNaviRoute = 0x00b314f0;  // CNavigation::SearchRoute
constexpr uintptr_t kNaviMgr   = 0x015c3090;  // &DAT_015c3090 (nav manager)

// Lua : appel d'un global via wrapper varargs. __cdecl(luaStatePtr, std::string
// funcName BYVAL, const char* fmt "d>s", <args in> , <ptrs out>). Renvoie 1 si OK.
// (RE 0x00a9a7d0 : fmt 'd'=int in / 's'=char* out ; '>' sépare in/out.)
constexpr uintptr_t kLuaCall  = 0x00a9a7d0;  // Lua_CallGlobal_va
constexpr uintptr_t kLuaState = 0x015ffd78;  // &g_UILuaState
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

// Recette texture (identique à skill_bar_tweaks / menu_icons).
constexpr uintptr_t kTexMgr  = 0x00a90350;
constexpr uintptr_t kMakeKey = 0x00a9f030;
constexpr uintptr_t kLoadTex = 0x00a8d4a0;
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;

using HideNative_t   = void  (__fastcall*)(void*, void*, int);
using GetDescLines_t = char*** (__fastcall*)(void*);            // -> &vector : [0]=begin,[1]=end
using GetBaseName_t  = size_t(__thiscall*)(void*, char*, size_t*, char);
using GameFree_t     = void  (__cdecl*)(void*);
// std::vector<int> MSVC (begin/last/end) — grossi par l'allocateur du JEU.
struct GVec { int* first; int* last; int* end; };
static_assert(sizeof(GVec) == 12, "GVec = std::vector layout");
// BuildDisplayName(this=wnd, info, &colorOut, &offsetsVec, &bufptr, &cap, &hlptr, f7, f8)
using BuildName_t = int(__thiscall*)(void*, void*, int*, GVec*, char**, size_t*,
                                     char**, char, char);
using CloseWin_t     = char  (__fastcall*)(void*, void*, int);
// OnMsg des fenêtres desc (vtable+0x94) : __thiscall(this, p1, msg, p3, p4, p5, p6).
using DescOnMsg_t    = int   (__thiscall*)(void*, int, int, int, int, int, int);
constexpr int kVfOnMsg        = 0x94;   // slot vtable OnMsg
constexpr int kMsgButton      = 6;      // message "commande bouton"
constexpr int kCmdProbability = 0x157;  // « View Probability Info » -> wnd 0x271c
// Éligibilité du bouton Probabilité : l'item a-t-il un enregistrement dans la DB
// de probabilité (DAT_01255108, chargée depuis packageitem.lub/simplecashshop..).
// ItemProbabilityDB_Fetch(mgr, out, id) __thiscall : out[0]=record, out[1].byte=found.
constexpr uintptr_t kProbDbPtr = 0x01255108;  // ptr vers le mgr (lazy-new)
constexpr uintptr_t kProbFetch = 0x0069f480;  // ItemProbabilityDB_Fetch
using ProbFetch_t = void(__thiscall*)(void*, int*, int);
using TexMgr_t       = void* (__cdecl*)();
using MakeKey_t      = void* (__cdecl*)(const char*);
using LoadTex_t      = void* (__fastcall*)(void*, void*, void*);
// std::string MSVC en mode HEAP (nom Lua de 16 car. = pas de SSO) : ptr + size + cap.
struct LuaStr { const char* ptr; char pad[12]; uint32_t size; uint32_t cap; };
static_assert(sizeof(LuaStr) == 0x18, "LuaStr = std::string MSVC (0x18)");
using LuaCall_t = char(__cdecl*)(void*, LuaStr, const char*, int, char**);
using GameMalloc_t = void* (__cdecl*)(size_t);

// Repro de la fenêtre SKILL (0x2e) : desc lue des rich-text box natifs (safe,
// pas de Lua/hook). Masquage du natif via OnTick UNIQUEMENT (PAS de hook sur
// 0x008ca900 : il crashait le chemin du msg 0x3d) -> léger flicker toléré.
constexpr bool     kSkillWindowEnabled = true;

// ── Couche serveur (ACTIVE — requiert le map-server rebuild avec 0x0F0B/0x0F0C) ─
constexpr bool     kEnableServerFetch = true;
// Zone custom SÛRE (>0x0C35, hors table client ET plage rAthena) : dispatch via
// le reader-hook (cf. RagConnection::RegisterRecvOpcode). Vérifié flag=-1.
// Source unique : plugins/bourgeon_opcodes.h
constexpr uint16_t kOpcodeReqTechData = bopcodes::kReqTechData;  // 0x0F0B client -> serveur
constexpr uint16_t kOpcodeTechData    = bopcodes::kTechData;     // 0x0F0C serveur -> client

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
  __try {
    auto* wnd = *reinterpret_cast<uint8_t**>(slot);
    if (wnd == nullptr) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(wnd) != expected_vtable) return nullptr;
    return wnd;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// c_str d'une std::string MSVC embarquée à base (SSO : heap si cap>0xf).
const char* MsvcStr(const uint8_t* base, uint32_t cap) {
  return (cap > 0xf) ? *reinterpret_cast<const char* const*>(base)
                     : reinterpret_cast<const char*>(base);
}

// Résout le .bmp de collection en pixels bruts BGRA (appels natifs, POD only).
// SEH ne peut pas contenir d'objets C++ (C2712) -> la conversion est faite hors
// __try par l'appelant.
struct RawTex { const uint8_t* bgra; int w; int h; };
bool GetRawTex(const char* path, RawTex* out) {
  __try {
    void* mgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
    if (!mgr) return false;
    void* key = reinterpret_cast<MakeKey_t>(kMakeKey)(path);
    if (!key) return false;
    void* t = reinterpret_cast<LoadTex_t>(kLoadTex)(mgr, nullptr, key);
    if (!t) return false;
    const int w = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexW);
    const int h = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexH);
    const uint8_t* bgra =
        *reinterpret_cast<const uint8_t**>(static_cast<char*>(t) + kTexPix);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !bgra) return false;
    out->bgra = bgra; out->w = w; out->h = h;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Charge le .bmp de collection (chemin déjà complet) en texture ImGui + dims.
IconTex LoadCollectionIcon(const char* path) {
  RawTex rt{};
  if (!GetRawTex(path, &rt)) return {};
  std::vector<uint8_t> argb(static_cast<size_t>(rt.w) * rt.h * 4);
  for (int i = 0; i < rt.w * rt.h; ++i) {
    const uint8_t b = rt.bgra[i * 4], g = rt.bgra[i * 4 + 1],
                  r = rt.bgra[i * 4 + 2];
    const bool ck = (r == 0xFF && g == 0 && b == 0xFF);  // colorkey magenta
    argb[i * 4] = b; argb[i * 4 + 1] = g; argb[i * 4 + 2] = r;
    argb[i * 4 + 3] = ck ? 0 : 0xFF;
  }
  return {Overlay_CreateTextureARGB(argb.data(), rt.w, rt.h), rt.w, rt.h};
}

// Données d'un item extraites sous SEH (POD only) pour sortir avant tout ImGui.
constexpr int kMaxLines = 64;
constexpr int kLineLen  = 192;
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
};

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
      reinterpret_cast<BuildName_t>(kBuildName)(
          wnd, wnd + kItemStruct, &colorOut, &off, &bufptr, &ncap, &hlptr, 0, 0);
      size_t n = 0;
      while (n < sizeof(e->name) - 1 && nbuf[n]) { e->name[n] = nbuf[n]; ++n; }
      e->name[n] = '\0';
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
            e->hl_col   = IM_COL32((colorOut >> 16) & 0xff,
                                   (colorOut >> 8) & 0xff, colorOut & 0xff, 255);
          }
        }
      }
      if (off.first) reinterpret_cast<GameFree_t>(kGameFree)(off.first);
    }
    // Repli : nom de base si BuildDisplayName n'a rien produit.
    if (e->name[0] == '\0') {
      size_t cap = sizeof(e->name);
      reinterpret_cast<GetBaseName_t>(kGetBaseName)(
          wnd + kItemStruct, e->name, &cap, 0);
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
      }
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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
  __try {
    uint8_t* body = *reinterpret_cast<uint8_t**>(wnd + kSkillBoxBody);
    uint8_t* head = *reinterpret_cast<uint8_t**>(wnd + kSkillBoxHead);
    ReadRichTextLines(body, e);
    ReadRichTextLines(head, e);
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

// Dessine le titre avec le segment [hs,he) coloré en hlcol (reste en def).
void DrawTitle(const char* name, int hs, int he, ImU32 hlcol, ImU32 def) {
  if (!name || !name[0]) { ImGui::TextUnformatted("(?)"); return; }
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

void SelectableColoredText(const char* id, const char lines[][kLineLen],
                           int count, ImU32 default_col) {
  ImDrawList* dl   = ImGui::GetWindowDrawList();
  ImFont*     font = ImGui::GetFont();
  const float fsz    = ImGui::GetFontSize();
  const float lineH  = ImGui::GetTextLineHeightWithSpacing();
  const float spaceW = ImGui::CalcTextSize(" ").x;
  float wrap = ImGui::GetContentRegionAvail().x;
  if (wrap < 1.0f) wrap = 1.0f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();

  const ImU32 kLinkUrl  = IM_COL32(64, 132, 224, 255);  // liens <URL>  (bleu)
  const ImU32 kLinkNavi = IM_COL32(60, 175, 90, 255);   // liens <NAVI> (vert)

  struct Box { float x0, x1, y; int row; int idx; ImU32 col; int link; };
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
  auto putVisible = [&](char c, float w, ImU32 col) {
    boxes.push_back({p0.x + penX, p0.x + penX + w, p0.y + penY, row,
                     static_cast<int>(buf.size()), col, curLink});
    buf.push_back(c);
    penX += w;
  };

  auto tag = [](const char* p, const char* t) {
    return std::strncmp(p, t, std::strlen(t)) == 0;
  };

  for (int li = 0; li < count; ++li) {
    const char* s = lines[li];
    ImU32 cur = default_col;
    bool  inHref = false;   // on lit l'URL entre <INFO> et </INFO> (non affichée)
    std::string word;
    auto flushWord = [&](ImU32 c) {
      if (word.empty()) return;
      ImU32 useCol = c;
      if (curLink >= 0)
        useCol = (linkKind[curLink] == 1) ? kLinkNavi : kLinkUrl;
      const float ww = ImGui::CalcTextSize(word.c_str()).x;
      if (penX > 0.0f) {
        if (penX + spaceW + ww > wrap) {       // wrap doux -> espace logique
          buf.push_back(' ');
          penX = 0.0f; penY += lineH; ++row;
        } else {
          putVisible(' ', spaceW, useCol);     // espace visible entre mots
        }
      }
      for (char ch : word) {
        const char tmp[2] = {ch, '\0'};
        putVisible(ch, ImGui::CalcTextSize(tmp).x, useCol);
      }
      word.clear();
    };
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
      if (*p == ' ') { flushWord(cur); ++p; }
      else           { word.push_back(*p); ++p; }
    }
    flushWord(cur);
    curLink = -1;
    buf.push_back('\n');            // fin de ligne (dans le buffer, non visible)
    penX = 0.0f; penY += lineH; ++row;
  }
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
        result = (m.x < (bx.x0 + bx.x1) * 0.5f) ? bx.idx : bx.idx + 1;
        break;
      }
      result = bx.idx + 1;  // après ce glyphe (continue -> fin de ligne)
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
        ImGui::SetTooltip("Aller a : %s (%d, %d)", mapn, nx, ny);
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
    dl->AddText(font, fsz, ImVec2(bx.x0, bx.y), bx.col, c, c + 1);
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

// Déclenche une commande bouton (msg 6) sur une fenêtre desc via son OnMsg. SEH.
// Utilisé pour rejouer le bouton natif « Probabilité » (ouvre la fenêtre 0x271c).
void CallDescButton(uint8_t* wnd, int cmd) {
  if (!wnd) return;
  __try {
    void** vt = *reinterpret_cast<void***>(wnd);
    auto onmsg = reinterpret_cast<DescOnMsg_t>(vt[kVfOnMsg / 4]);
    onmsg(wnd, 0, kMsgButton, cmd, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Résout (cache + chargement) l'icône de collection d'un item extrait.
IconTex ResolveIcon(uint32_t id, const ItemExtract& e) {
  if (!e.has_icon) return {};
  auto it = g_icon_cache.find(id);
  if (it != g_icon_cache.end()) return it->second;
  IconTex tex = LoadCollectionIcon(e.iconpath);
  g_icon_cache[id] = tex;  // met en cache même null (miss connu)
  return tex;
}

}  // namespace

ItemDescTweaks::ItemDescTweaks() {
  if (kEnableServerFetch) {
    Bourgeon::Instance().RegisterRecvOpcode(kOpcodeTechData);
  }
}

namespace {
// Cache le rendu natif d'UNE fenêtre desc lue depuis son slot (vtable validée).
void HideDescSlot(uintptr_t slot, uintptr_t vtable) {
  if (uint8_t* wnd = ReadValidWnd(slot, vtable))
    __try { reinterpret_cast<HideNative_t>(kHideNative)(wnd, nullptr, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
}  // namespace

void ItemDescTweaks::HideNativeDescWindows() {
  if (!show_item_panel_) return;
  // Cache la fenêtre item (0xc) ET la fenêtre de comparaison (0xea) — cette
  // dernière est créée dans le même OnMsg 0x18, donc déjà présente ici.
  HideDescSlot(kItemWndSlot, kItemVTable);
  HideDescSlot(kCompareWndSlot, kCompareVTable);
}

void ItemDescTweaks::HideNativeSkillWindow() {
  if (!kSkillWindowEnabled || !show_skill_panel_) return;
  HideDescSlot(kSkillWndSlot, kSkillVTable);
}

namespace {
// Lit une fenêtre au layout ITEM (0xc ou 0xea : id = chaîne décimale @+0xe4).
void ReadItemLayoutWindow(uintptr_t slot, uintptr_t vtable,
                          ItemDescTweaks::DescWindow* out) {
  *out = ItemDescTweaks::DescWindow{};
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
        out->x        = *reinterpret_cast<int*>(wnd + kOffPosX);
        out->y        = *reinterpret_cast<int*>(wnd + kOffPosY);
        out->w        = *reinterpret_cast<int*>(wnd + kOffWidth);
        out->h        = *reinterpret_cast<int*>(wnd + kOffHeight);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { *out = ItemDescTweaks::DescWindow{}; }
  }
}
}  // namespace

void ItemDescTweaks::OnTick() {
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
        skill_.x        = *reinterpret_cast<int*>(wnd + kOffPosX);
        skill_.y        = *reinterpret_cast<int*>(wnd + kOffPosY);
        skill_.w        = *reinterpret_cast<int*>(wnd + kOffWidth);
        skill_.h        = *reinterpret_cast<int*>(wnd + kOffHeight);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { skill_ = DescWindow{}; }
  }
  // Repro skill désactivée (kSkillWindowEnabled) : on NE cache PAS le natif.
  if (kSkillWindowEnabled && show_skill_panel_ && skill_.open)
    HideDescSlot(kSkillWndSlot, kSkillVTable);

  // Front montant d'ouverture -> pose la fenêtre reproduite près du curseur.
  // Aucune requête serveur automatique : c'est le joueur qui les déclenche via
  // les boutons du panneau (cf. RenderTechBlock).
  if (item_.open && !item_was_open_) {
    POINT pt;
    if (GetCursorPos(&pt)) {
      item_spawn_x_ = pt.x + 12;
      item_spawn_y_ = pt.y + 12;
      item_need_pos_ = true;
    }
  }
  item_was_open_ = item_.open;
  if (skill_.open && !skill_was_open_) {
    POINT pt;
    if (GetCursorPos(&pt)) {
      skill_spawn_x_ = pt.x + 12;
      skill_spawn_y_ = pt.y + 12;
      skill_need_pos_ = true;
    }
  }
  skill_was_open_ = skill_.open;
}

void ItemDescTweaks::RequestTechData(uint32_t id, bool is_skill, uint8_t scope) {
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

void ItemDescTweaks::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                  uint16_t len) {
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
void ItemDescTweaks::RenderDropTable(const TechData& td, const char* table_id,
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
    ImGui::TextDisabled("%u coffre(s) au tresor exclu(s).", td.treasure_excluded);
  if (td.truncated)
    ImGui::TextDisabled("... autres sources : voir la base de donnees.");
}

// Panneau d'infos techniques : boutons on-demand (aucune requête par défaut).
void ItemDescTweaks::RenderTechBlock(const DescWindow& w) {
  if (!kEnableServerFetch) return;
  const bool panel_on = w.is_skill ? show_skill_panel_ : show_item_panel_;
  if (!panel_on) return;

  ImGui::Separator();
  ImGui::PushID(static_cast<int>(w.id));  // IDs uniques (colonnes équipé/comparé)

  // En-tête pliable : replié par défaut (donc AUCUNE requête au départ). Le
  // déplier lance la requête (guardée par le TTL) et renvoie l'entrée cache une
  // fois prête ; le replier cache les résultats (les données restent en cache).
  auto open_section = [&](uint8_t scope, const char* header_label)
      -> const TechData* {
    if (!ImGui::CollapsingHeader(header_label))
      return nullptr;  // replié : rien affiché, pas de requête
    RequestTechData(w.id, w.is_skill, scope);
    const uint32_t key = CacheKey(w.id, w.is_skill, scope);
    auto it = cache_.find(key);
    const FetchState st =
        (it != cache_.end()) ? it->second.state : FetchState::kNone;
    if (st == FetchState::kPending)
      ImGui::TextDisabled("Chargement...");
    else if (st == FetchState::kFailed)
      ImGui::TextDisabled("echec de la requete");
    return (st == FetchState::kReady && it != cache_.end()) ? &it->second
                                                            : nullptr;
  };

  if (w.is_skill) {
    // ── SKILL : en-tête pliable -> table de cast par niveau ─────────────────
    if (const TechData* td =
            open_section(kScopeNormal, "Informations techniques du sort")) {
      if (td->levels.empty()) {
        ImGui::TextDisabled("Aucune donnee de cast.");
      } else {
        const ImGuiTableFlags tf = ImGuiTableFlags_Borders |
                                   ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_SizingFixedFit;
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,
                              IM_COL32(206, 198, 172, 255));
        if (ImGui::BeginTable("tech_skill", 5, tf)) {
          ImGui::TableSetupColumn("Niv");
          ImGui::TableSetupColumn("Cast");
          ImGui::TableSetupColumn("Fixe");
          ImGui::TableSetupColumn("Cooldown");
          ImGui::TableSetupColumn("Delai");
          ImGui::TableHeadersRow();
          auto sec = [](int32_t ms) { return ms / 1000.0f; };
          for (size_t i = 0; i < td->levels.size(); ++i) {
            const SkillLv& s = td->levels[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", (int)i + 1);
            ImGui::TableNextColumn(); ImGui::Text("%.2fs", sec(s.cast_var));
            ImGui::TableNextColumn(); ImGui::Text("%.2fs", sec(s.cast_fixed));
            ImGui::TableNextColumn(); ImGui::Text("%.2fs", sec(s.cooldown));
            ImGui::TableNextColumn(); ImGui::Text("%.2fs", sec(s.delay));
          }
          ImGui::EndTable();
        }
        ImGui::PopStyleColor();
      }
    }
    ImGui::PopID();
    return;
  }

  // ── ITEM : deux en-têtes pliables, split par boss-type du mob ─────────────
  // Monstres = mobs non-boss (une seule mécanique : drop normal, pas de colonne
  // Type). MVP/boss = mobs boss-type (drops normaux + MVP rewards -> colonne Type).
  if (const TechData* td =
          open_section(kScopeNormal, "Quels monstres droppent cet item ?"))
    RenderDropTable(*td, "tech_drops_normal",
                    CacheKey(w.id, false, kScopeNormal), /*show_type=*/false);
  ImGui::Spacing();
  if (const TechData* td =
          open_section(kScopeMvp, "Quels boss / MVP droppent cet item ?"))
    RenderDropTable(*td, "tech_drops_mvp", CacheKey(w.id, false, kScopeMvp),
                    /*show_type=*/true);

  ImGui::PopID();
}

// Reproduit la fenêtre de description d'ITEM en ImGui (Option A). Le pointeur
// natif est relu FRAIS + validé ici (indépendant de OnTick). SEH sur les
// lectures/appels natifs ; le rendu ImGui reste hors __try (objets C++).
void ItemDescTweaks::RenderItemWindow() {
  uint8_t* iwnd = ReadValidWnd(kItemWndSlot, kItemVTable);
  if (!iwnd) return;
  // Fenêtre de comparaison (équipé) éventuelle (id 0xea).
  uint8_t* cwnd = ReadValidWnd(kCompareWndSlot, kCompareVTable);

  // Extraction SEH (POD only), puis résolution icône hors SEH.
  ItemExtract ie{}; ExtractItem(iwnd, &ie);
  IconTex iicon = ResolveIcon(item_.id, ie);

  const bool has_cmp = (cwnd != nullptr && compare_.open);
  ItemExtract ce{}; IconTex cicon;
  if (has_cmp) { ExtractItem(cwnd, &ce); cicon = ResolveIcon(compare_.id, ce); }

  // ── Rendu ImGui (fond clair : le texte RO est conçu pour un fond pâle) ─────
  // Redimensionnable entre (500,300) et (1800,900) ; taille par défaut au 1er
  // affichage, puis ImGui mémorise la taille choisie par l'utilisateur.
  ImGui::SetNextWindowSizeConstraints(ImVec2(500.0f, 300.0f),
                                      ImVec2(1800.0f, 900.0f));
  ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
  if (item_need_pos_) {
    ImGui::SetNextWindowPos(
        ImVec2(static_cast<float>(item_spawn_x_),
               static_cast<float>(item_spawn_y_)),
        ImGuiCond_Always);
    item_need_pos_ = false;
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
    if (icon.tex && icon.w > 0 && icon.h > 0) {
      // Taille native, ratio préservé, plafonnée (évite la déformation 48x48).
      const float kMax = 72.0f;
      float w = static_cast<float>(icon.w), h = static_cast<float>(icon.h);
      const float big = (w > h) ? w : h;
      if (big > kMax) { const float s = kMax / big; w *= s; h *= s; }
      ImGui::Image(reinterpret_cast<ImTextureID>(icon.tex), ImVec2(w, h));
      ImGui::SameLine();
    }
    ImGui::BeginGroup();
    // Nom coloré : uniquement en COMPARAISON (le titre de fenêtre = "Comparaison").
    // En mode simple le nom est déjà dans la barre de titre -> pas de doublon.
    if (header)
      DrawTitle(e.name, e.hl_start, e.hl_end, e.hl_col, IM_COL32(0, 0, 0, 255));

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
      char vid[64];
      std::snprintf(vid, sizeof(vid), "%s_vid", selId);
      SelectableColoredText(vid, e.lines + skip_db, 1, black);
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
    ImGui::EndGroup();

    ImGui::Separator();
    SelectableColoredText(selId, e.lines + skip, e.line_count - skip, black);
    RenderTechBlock(snap);
  };

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoFocusOnAppearing;
  char title[160];
  if (has_cmp)
    std::snprintf(title, sizeof(title), "Comparaison###itemdesc_item");
  else
    std::snprintf(title, sizeof(title), "%s###itemdesc_item",
                  ie.name[0] ? ie.name : "Description");

  bool open = true;
  const bool visible = ImGui::Begin(title, &open, flags);
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
  if (visible) {
    if (has_cmp) {
      // Deux colonnes redimensionnables qui se partagent la largeur (le texte
      // se wrap dans chacune). Ordre NATIF : ÉQUIPÉ à gauche, sélection à droite.
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
  }
  ImGui::End();
  ImGui::PopStyleColor(4);

  // X ImGui -> ferme les fenêtres natives (item 0xc + comparaison 0xea).
  if (!open) {
    __try {
      reinterpret_cast<CloseWin_t>(kCloseWindow)(
          reinterpret_cast<void*>(kUIWindowMgr), nullptr, 0xc);
      reinterpret_cast<CloseWin_t>(kCloseWindow)(
          reinterpret_cast<void*>(kUIWindowMgr), nullptr, 0xea);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
}

// Reproduit la fenêtre de description de SKILL (classe 0x2e) en ImGui : nom+SP
// (this+0xec) + description via Lua GetSkillDescript (markup rendu comme l'item).
void ItemDescTweaks::RenderSkillWindow() {
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
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(skill_spawn_x_),
                                   static_cast<float>(skill_spawn_y_)),
                            ImGuiCond_Always);
    skill_need_pos_ = false;
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

  bool open = true;
  const bool visible = ImGui::Begin(title, &open, flags);
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
  if (visible) {
    // Nom+SP déjà dans la barre de titre, et l'ID/Max Level sont déjà dans la
    // description native (box head) -> on n'affiche QUE la description ici pour
    // éviter les répétitions.
    SelectableColoredText("##seltext_skill", s_e.lines, s_e.line_count,
                          IM_COL32(0, 0, 0, 255));
    RenderTechBlock(skill_);
  }
  ImGui::End();
  ImGui::PopStyleColor(4);

  if (!open) {
    __try {
      reinterpret_cast<CloseWin_t>(kCloseWindow)(
          reinterpret_cast<void*>(kUIWindowMgr), nullptr, 0x2e);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
}

void ItemDescTweaks::OnRenderUI() {
  // ITEM (0xc/0xea) + SKILL (0x2e) reproduits en ImGui (Option A).
  if (show_item_panel_  && item_.open)  RenderItemWindow();
  if (kSkillWindowEnabled && show_skill_panel_ && skill_.open)
    RenderSkillWindow();
}
