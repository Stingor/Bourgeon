#include "plugins/char_select.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"      // Overlay_CreateTextureARGB / Overlay_DeviceEpoch
#include "imgui.h"
#include "plugins/basic_info.h"  // RenderDoll : moteur de capture sprite partagé
#include "plugins/moonlight_auth.h"
#include "ui/ro_imgui.h"
#include "utils/hooking/hook_manager.h"  // détour Net_OnDeleteCharReserveAck
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

// DX7/DX9 : le proxy ddraw pose ce flag ; il sélectionne l'offset du handle texture
// natif dans CTexture (0x128 DX7 / 0x12c DX9). Défini dans proxy_idirectdraw.cc.
extern bool g_imgui_dx7_active;

namespace {

// ── Accès natif (client 20250716, base 0x400000) ──────────────────────────────
// Dispatcher CMode : *(0x0121333c) -> objet ; vtbl+0x18, cmd 8 = get CHARACTER_INFO
// par slot (renvoie nullptr si slot vide). Convention __thiscall confirmée dans
// character_sheet.cc (SendConfigToggle). Carte des champs = docs/charselect_re.md.
constexpr uintptr_t kUICmdDisp = 0x0121333c;
constexpr int       kVfDispCmd = 0x18;
constexpr int       kCmdGetChar = 8;

// ── Loader de texture natif (fond banquet, BMP côté client) ───────────────────
// Même chemin que les icônes d'items (character_sheet.cc) : UITextureMgr_Get ->
// MakeKey(path) -> LoadTex ; la texture expose largeur/hauteur/pixels BGRA.
constexpr uintptr_t kTexMgrGet  = 0x00a90350;  // UITextureMgr_Get() __cdecl -> mgr
constexpr uintptr_t kTexMakeKey = 0x00a9f030;  // MakeKey(path) __cdecl -> key
constexpr uintptr_t kTexLoad    = 0x00a8d4a0;  // LoadTex(mgr, edx, key) __fastcall
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;
// Chemin VFS du décor : à déposer dans le GRF/data du client (via le patcher).
// Préfixe 유저인터페이스\ (CP949) = racine UI où le loader résout à coup sûr, comme
// les .bmp d'items. Fichier 24/32 bits ; dessiné étiré plein écran (toute taille
// convient — plus petit = moins de VRAM).
const char kHallBmpPath[] =
    "lobby_hall.bmp";

// ── Fenêtre native de création (ouverte par le contrôle « créer » 0x1A0) ──────
constexpr uintptr_t kFindWindow    = 0x00a47b90;  // __thiscall(mgr, id) -> wnd|null
constexpr int       kMakeCharWndId = 0xC8;        // UIMakeCharWnd (MakeWindow 0xC8)

// ── Quitter l'écran : retour au login / fermeture du jeu ─────────────────────
// Le « Cancel » natif du char-select (UINewSelectCharWnd_OnMsg 0x0079d610, ctrl 185)
// fait, après une msgbox de confirmation, l'un des deux selon un flag client
// (g_CanReturnToLoginScreen 0x01602328) :
//   flag=1 -> CLoginMode_SendMsg(mode, 10011) = CRagConnection_OnDisconnect + état 3
//             (0x00d2a130 case 10011) = RETOUR à l'écran de connexion ;
//   sinon  -> SendMsg(mode, 2), qui retombe sur CMode::SendMsg de base 0x00a763c0 :
//             arrêt des sous-systèmes + mode+0x14 = 0 -> la boucle principale sort
//             = QUITTER le jeu (exactement ce que fait le bouton « Exit » de
//             UILoginWnd_OnMsg 0x008848d0 ctrl 221).
// On expose les DEUX en ImGui, sans passer par le ctrl 185 : sa msgbox native est
// supprimée sous notre UI (Detour_ShowModal renvoie 185 != 187) -> le natif
// conclurait « annulé » et ne ferait rien. On envoie donc la commande de mode
// nous-mêmes, après notre propre confirmation ImGui.
constexpr int kCmdBackToLogin = 10011;  // 0x271B
constexpr int kCmdQuitGame    = 2;
// g_UIWindowMgr + id de la fenêtre native du char-select. FindWindow(0x115) non nul
// = l'écran char-select est VIVANT dans le manager (contrairement au cache
// mgr+0x3d4 = kCharSelWndPtr, jamais remis à zéro à la destruction). Sert à savoir
// quand le natif a effectivement quitté l'écran après notre commande.
constexpr uintptr_t kUIWindowMgr  = 0x0131f4e8;
constexpr int       kCharSelWndId = 0x115;  // 277 = UINewSelectCharWnd

// La fenêtre native du char-select existe-t-elle encore ?
bool NativeCharSelectAlive() {
  __try {
    using FindWindow_t = void*(__thiscall*)(void*, int);
    return reinterpret_cast<FindWindow_t>(kFindWindow)(
               reinterpret_cast<void*>(kUIWindowMgr), kCharSelWndId) != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ── Entrée en jeu : séquence NATIVE (cf. EnterGame) ──────────────────────────
// g_CharSelect_SelectedSlot : octet lu par le handler du bouton OK ET relu plus
// tard par Net_OnNotifyZoneSvr_EnterGame 0x00d23180 (seed du cache local).
constexpr uintptr_t kSelectedSlot    = 0x015F8262;
// Coiffures à la CRÉATION : on rend les SPR nous-mêmes et le serveur
// (char_make_new_char) NE valide PAS hair_style -> on peut exposer TOUT ce que data.grf
// contient (~80), bien au-delà des 23 du make-char natif (qui n'affiche que des BMP
// img_hairStyleNN.bmp). Grille 4 colonnes, ordre ascendant (id affiché = id envoyé).
// Un id sans sprite -> case vide (DrawHairIcon ne dessine rien). Couleurs 0..250.
constexpr int kMaxHairStyle = 80;
constexpr int kHairGridCols = 8;  // 8 col. x 10 lignes -> les 80 tiennent sans scroll
constexpr int kMaxHairColor = 250;
// Début du picking couleur. Toutes affichées (0..kMaxHairColor). ⚠ head_0..head_6 du
// GRF sont mal faites (quasi plates) — à corriger côté contenu ; mises à kMinHairColor
// > 0 si on veut les retirer du picking.
constexpr int kMinHairColor = 0;
// UINewSelectCharWnd : id de fenêtre 0x115, pointeur caché à mgr+0x3d4.
constexpr uintptr_t kCharSelWndPtr   = 0x0131F8BC;  // g_pCharSelectWnd
constexpr uintptr_t kCharSelWndVtbl  = 0x0101D424;  // garde anti-pointeur périmé
constexpr uintptr_t kCharSelOnMsg    = 0x0079D610;  // vtbl+0x94, RET 0x18

// Comptes de slots renseignés par HC_ACCEPT_ENTER2 (serveur). Capacité = somme.
constexpr uintptr_t kNormalSlots   = 0x015ffd60;
constexpr uintptr_t kPremiumSlots  = 0x015ffd64;
constexpr uintptr_t kBillingSlots  = 0x015ffd68;
constexpr uintptr_t kCreatableSlots = 0x015ffd6c;

// Offsets dans CHARACTER_INFO (175 o) — voir docs/charselect_re.md.
namespace ci {
constexpr int kGid       = 0x00;  // u32
constexpr int kZeny      = 0x0c;  // i32
constexpr int kJobLevel  = 0x18;  // i32
constexpr int kHp        = 0x32;  // i64
constexpr int kMaxHp     = 0x3a;  // i64
constexpr int kSp        = 0x42;  // i64
constexpr int kMaxSp     = 0x4a;  // i64
constexpr int kJob       = 0x54;  // i16
constexpr int kBaseLevel = 0x5c;  // i16
constexpr int kName      = 0x6c;  // char[24]
constexpr int kStr       = 0x84;  // u8 x6 (str/agi/vit/int/dex/luk)
constexpr int kSlot      = 0x8a;  // u8
constexpr int kMap       = 0x8e;  // char[16]
constexpr int kDelDate   = 0x9e;  // i32 (délai restant avant suppression ; 0=aucune)
constexpr int kSex       = 0xae;  // u8 (0=F,1=M,99=compte)
// Apparence (paperdoll) — exactement les champs lus par RenderSlots 0x0079d170.
constexpr int kHair      = 0x56;  // u16
constexpr int kBody      = 0x58;  // u16 body style
constexpr int kHeadLow   = 0x60;  // u16 head_bottom
constexpr int kHeadTop   = 0x64;  // u16 head_top
constexpr int kHeadMid   = 0x66;  // u16 head_mid
constexpr int kHairCol   = 0x68;  // u16
constexpr int kClothesCol = 0x6a;  // u16
constexpr int kGarment   = 0xa2;  // u32 robe
// Compteurs de coupons (RE UINewSelectCharWnd_OnMsg 0x0079d610 : lus >0 pour le badge
// via sub_D239E0(45/47), et +0xa6 relu dans le paquet moveslot case 433).
constexpr int kMoveCnt   = 0xa6;  // u32 changements de SLOT restants (coupon 12786)
constexpr int kRenameCnt = 0xaa;  // u32 renommages restants (coupon 12790)
}  // namespace ci

// Sexe du COMPTE (g_Account_Sex) : CHARACTER_INFO+0xae vaut 99 (« sexe du compte »)
// pour les serveurs modernes. Même résolution que le natif, aux DEUX endroits où il
// la fait : UINewSelectCharWnd_RenderSlots 0x0079d170 (avant Actor_Init) et
// UINewSelectCharWnd_OnMsg 0x0079d737 (Own_SetSex avant l'entrée en jeu).
// Lu en OCTET : le natif ne consomme que le low byte (cast (char) avant Actor_Init).
constexpr uintptr_t kAccountSex = 0x015FB23C;
// Account ID (g_Account_Aid) — requis par CH_REQ_IS_VALID_CHARNAME 0x028d (rename).
constexpr uintptr_t kAccountAid = 0x015FB9A4;

using DispCmd_t = void*(__thiscall*)(void*, int, int, int, int, int);

// ── Détour de Net_OnDeleteCharReserveAck 0x00d21210 ──────────────────────────
// Handler natif de HC_DELETE_CHAR3_RESERVED (réponse ZC 0x0828 à « Programmer
// suppression »). __fastcall(ecx=mode, edx, pktbuf) ; RET 4 ; UN SEUL appelant.
// pktbuf = 0x015e8198 (buffer recv global) : result à +6, char_id à +2, date à +10.
//   result 1 = succès (le natif écrit la date d'expiration dans CHARACTER_INFO+0x9e)
//   result 2 = no-op silencieux
//   4=guilde 5=groupe 3=db/introuvable 6=échoppe 0=déjà en file -> le natif ouvrirait
//     une MSGBOX MODALE bloquante (UIWndMgr_ShowMessageBoxModal 0x00a31a30), cachée
//     sous notre UI plein écran qui capte l'input -> elle traînerait.
// On la SUPPRIME chirurgicalement : si NOTRE char-select couvre (g_cover_active) ET
// que c'est un échec, on NE rappelle PAS l'original -> la box n'est jamais créée (le
// chemin d'échec ne fait rien d'autre) et on remonte le code au plugin. Sur succès/
// no-op, ou si le natif est visible (pas notre UI), on laisse tourner l'original.
using DelAckFn = void(__fastcall*)(void*, void*, void*);
DelAckFn g_orig_del_ack = nullptr;
bool g_cover_active = false;          // notre scène char-select couvre-t-elle le natif ?
int g_del_reject_result = 0;         // dernier code d'échec
unsigned long g_del_reject_seq = 0;  // ++ à chaque échec (le plugin détecte le delta)

void __fastcall Detour_DeleteAck(void* mode, void* edx, void* pktbuf) {
  int result = 1;
  __try {
    result = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(pktbuf) + 6);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    result = 1;  // doute -> laisser le natif faire, ne rien casser
  }
  if (result == 1 || result == 2 || !g_cover_active) {
    if (g_orig_del_ack) g_orig_del_ack(mode, edx, pktbuf);
    return;
  }
  g_del_reject_result = result;  // échec + notre UI couvre : msgbox native SUPPRIMÉE
  ++g_del_reject_seq;
}

void InstallDeleteAckDetour() {
  static bool done = false;
  if (done) return;
  done = true;
  g_orig_del_ack = reinterpret_cast<DelAckFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(0x00d21210),
          reinterpret_cast<uint8_t*>(&Detour_DeleteAck)));
  if (!g_orig_del_ack)
    LogError("[CharSelect] détour delete-ack 0x00d21210 NON installé : la msgbox "
             "native de refus pourra apparaître/traîner sous l'UI.");
}

// ── Détour de UIWndMgr_ShowMessageBoxModal 0x00a31a30 ────────────────────────
// Cette fonction native lance sa PROPRE pompe de messages (modale BLOQUANTE) : tant
// que l'utilisateur n'a pas cliqué OK, elle ne rend pas la main. Sous notre char-select
// ImGui plein écran (qui capte tout l'input), l'OK n'est jamais cliqué -> BLOCAGE total
// (ImGui figé). Ça arrive p.ex. sur HC_REFUSE_MAKECHAR 0x006e (nom déjà pris), dont le
// handler est INLINE dans LoginCharMode_RecvDispatch (pas détournable isolément) et
// appelle cette modale (id msg 0x76d/0x76e). Plutôt que détourner chaque handler, on
// garde CE point de passage commun : si NOTRE UI couvre (g_cover_active), on SUPPRIME la
// modale (retour no-op 185, comme le early-return "déjà en jeu" du natif) — le feedback
// est déjà rendu en ImGui (create échec / delete refus). Sinon (char-select natif visible)
// on laisse tourner l'original. __thiscall : ecx=mgr, 9 args pile.
using ShowModalFn = int(__fastcall*)(void*, void*, char*, int, int*, int, int, char*,
                                     int, int, int*);
ShowModalFn g_orig_show_modal = nullptr;
// ++ à chaque modale native SUPPRIMÉE sous notre UI. Le popup de création s'en sert pour
// conclure « échec » (nom pris) SANS attendre le timeout (le refus 0x6e est hors recv).
unsigned long g_modal_suppressed_seq = 0;
// Texte (code-page client) de la DERNIÈRE modale supprimée : permet d'afficher la vraie
// raison du serveur (nom pris/invalide/guilde…) au lieu d'un message générique.
char g_suppressed_modal_msg[256] = {0};

int __fastcall Detour_ShowModal(void* ecx, void* edx, char* msg, int p2, int* p3, int p4,
                                int p5, char* p6, int p7, int p8, int* p9) {
  if (g_cover_active) {
    __try {
      if (msg) {
        std::strncpy(g_suppressed_modal_msg, msg, sizeof(g_suppressed_modal_msg) - 1);
        g_suppressed_modal_msg[sizeof(g_suppressed_modal_msg) - 1] = '\0';
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_suppressed_modal_msg[0] = '\0';
    }
    ++g_modal_suppressed_seq;
    return 185;  // notre UI couvre -> modale supprimée (no-op)
  }
  if (g_orig_show_modal)
    return g_orig_show_modal(ecx, edx, msg, p2, p3, p4, p5, p6, p7, p8, p9);
  return 185;
}

void InstallShowModalDetour() {
  static bool done = false;
  if (done) return;
  done = true;
  g_orig_show_modal = reinterpret_cast<ShowModalFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(0x00a31a30),
          reinterpret_cast<uint8_t*>(&Detour_ShowModal)));
  if (!g_orig_show_modal)
    LogError("[CharSelect] détour ShowMessageBoxModal 0x00a31a30 NON installé : une "
             "modale native (nom pris, etc.) pourra bloquer sous l'UI.");
}

// Message de refus pour un code result serveur (cf. Net_OnDeleteCharReserveAck).
const char* DeleteRejectMsg(int result) {
  switch (result) {
    case 4:
      return "Suppression refusée : ce personnage est dans une GUILDE. "
             "Quitte-la d'abord, puis réessaie.";
    case 5:
      return "Suppression refusée : ce personnage est dans un GROUPE. "
             "Quitte-le d'abord, puis réessaie.";
    case 6:
      return "Suppression impossible : ce personnage tient une échoppe.";
    case 3:
      return "Suppression impossible : erreur base de données ou personnage "
             "introuvable.";
    case 0:
      return "La suppression de ce personnage est déjà programmée.";
    default:
      return "Suppression refusée par le serveur.";
  }
}

// Code-page EFFECTIVE du client (949 Corée / 1252 Europe / 0 = CP_ACP), posée par
// FUN_00a72440 selon g_ServiceType. Le natif rend les noms via
// MultiByteToWideChar(CodePage_0159b818) + TextOutW (UIText_GdiTextOut 0x00547600).
constexpr uintptr_t kClientCodePage = 0x0159b818;

// Convertit un texte du jeu (noms, maps) vers UTF-8 pour ImGui, en MIROIR du natif :
// on lit la code-page effective du client au lieu de coder 949 en dur, donc correct
// quel que soit le servicetype de la connexion. Sans ça, ImGui interprète les octets
// code-page comme de l'UTF-8 invalide -> « ????? ».
// Buffer rotatif : consommer le résultat tout de suite, ne pas le stocker.
const char* LocalToUtf8(const char* s) {
  static char bufs[4][256];
  static int idx = 0;
  char* out = bufs[idx];
  idx = (idx + 1) & 3;
  out[0] = '\0';
  if (!s || !*s) return out;
  UINT cp;
  __try {
    cp = *reinterpret_cast<const UINT*>(kClientCodePage);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    cp = CP_ACP;
  }
  wchar_t wide[128];
  const int wlen = MultiByteToWideChar(cp, 0, s, -1, wide, 128);
  if (wlen <= 0) {  // code-page refusée -> repli sur les octets bruts
    lstrcpynA(out, s, sizeof(bufs[0]));
    return out;
  }
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, sizeof(bufs[0]), nullptr, nullptr);
  return out;
}

// Inverse de LocalToUtf8 : une saisie ImGui (UTF-8) -> code-page du client, pour
// l'ENVOYER sur le fil (ex. nom à la création). Sans ça, un nom accentué partirait en
// octets UTF-8 et s'afficherait « mojibake » (le natif rend les noms en code-page).
// Buffer rotatif : consommer tout de suite.
const char* Utf8ToLocal(const char* s) {
  static char bufs[2][256];
  static int idx = 0;
  char* out = bufs[idx];
  idx = (idx + 1) & 1;
  out[0] = '\0';
  if (!s || !*s) return out;
  UINT cp;
  __try {
    cp = *reinterpret_cast<const UINT*>(kClientCodePage);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    cp = CP_ACP;
  }
  wchar_t wide[128];
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, wide, 128);
  if (wlen <= 0) {  // UTF-8 invalide -> repli sur les octets bruts
    lstrcpynA(out, s, sizeof(bufs[0]));
    return out;
  }
  WideCharToMultiByte(cp, 0, wide, -1, out, sizeof(bufs[0]), nullptr, nullptr);
  return out;
}

// ── Icônes de coiffure : rendu DIRECT du .spr (comme le make-char natif) ─────────
// Recette RE (agent IDA + capture x32dbg sur make_character_ver2) : le natif ne fait
// PAS de doll — il charge 인간족\머리통\<genre>\<id>_<genre>.spr/.act via le loader
// ressource, prend une frame et la dessine. On réplique EXACTEMENT le mécanisme de
// login_parade (spr+act -> Act_GetFrame -> plus grande cellule -> atlas -> texture DX9
// -> AddImage). Léger, budget/cache gérés par l'atlas natif. Cf. login_parade.cc.
namespace hairicon {
// Format-strings NATIVES (CP949, adresses confirmées) : "%d" = id de coiffure brut.
constexpr uintptr_t kFmtSprF = 0x0108F9BC;  // 인간족\머리통\여\%d_여.spr (femelle)
constexpr uintptr_t kFmtActF = 0x0108F9A0;  // …\여\%d_여.act
constexpr uintptr_t kFmtSprM = 0x0108F9F4;  // 인간족\머리통\남\%d_남.spr (mâle)
constexpr uintptr_t kFmtActM = 0x0108F9D8;  // …\남\%d_남.act
constexpr uintptr_t kTexMgrGet      = 0x00a90350;  // __cdecl() -> mgr
constexpr uintptr_t kMakeKey        = 0x00a9f030;  // __cdecl(path) -> key
constexpr uintptr_t kLoadRes        = 0x00a8d4a0;  // __fastcall(mgr, edx, key) -> CSprite*/CAction*
constexpr uintptr_t kActGetFrame    = 0x0070f4b0;  // __fastcall(act, edx, action, frame) -> frame*
constexpr uintptr_t kAtlasGetCached = 0x00566b70;  // __fastcall(atlas, edx, cell, pal, geom) -> ctex
constexpr uintptr_t kAtlasBuild     = 0x005663d0;
constexpr uintptr_t kSceneCtxPtr    = 0x012515f8;  // *ptr + 0xc0 = atlas de sprites
constexpr int       kCTexDX9Handle  = 0x12c;
constexpr int       kCTexDX7Handle  = 0x128;
using TexMgrGetFn   = void* (__cdecl*)();
using MakeKeyFn     = void* (__cdecl*)(const char*);
using LoadResFn     = void* (__fastcall*)(void*, void*, void*);
using ActGetFrameFn = void* (__fastcall*)(void*, void*, unsigned, unsigned);
using AtlasFn       = void* (__fastcall*)(void*, void*, void*, int, int*);

struct Entry { int hair = -1, sex = -1; void* spr = nullptr; void* act = nullptr; };
constexpr int kCacheN = 64;   // >= 43 styles : la grille tient sans thrash
Entry g_cache[kCacheN];
int   g_next = 0;

// Charge (ou récupère) le sprite+action d'une coiffure. SEH : ressource manquante /
// GRF -> spr/act nul (l'appelant met un placeholder), jamais de crash.
Entry* Ensure(int hair, int sex) {
  for (int i = 0; i < kCacheN; ++i)
    if (g_cache[i].hair == hair && g_cache[i].sex == sex) return &g_cache[i];
  char sp[160], ap[160];
  std::snprintf(sp, sizeof(sp),
                reinterpret_cast<const char*>(sex ? kFmtSprM : kFmtSprF), hair);
  std::snprintf(ap, sizeof(ap),
                reinterpret_cast<const char*>(sex ? kFmtActM : kFmtActF), hair);
  void* spr = nullptr;
  void* act = nullptr;
  __try {
    void* mgr = reinterpret_cast<TexMgrGetFn>(kTexMgrGet)();
    spr = reinterpret_cast<LoadResFn>(kLoadRes)(
        mgr, nullptr, reinterpret_cast<MakeKeyFn>(kMakeKey)(sp));
    act = reinterpret_cast<LoadResFn>(kLoadRes)(
        mgr, nullptr, reinterpret_cast<MakeKeyFn>(kMakeKey)(ap));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    spr = nullptr;
    act = nullptr;
  }
  Entry& e = g_cache[g_next];
  g_next = (g_next + 1) % kCacheN;
  e.hair = hair;
  e.sex = sex;
  e.spr = spr;
  e.act = act;
  return &e;
}

// Résout la plus grande cellule de la frame 0 (action 0 = idle sud, de face) en
// texture atlas + UV. Ré-résolu chaque frame (le handle de page survit au device
// reset). Copie fidèle de login_parade::ResolveQuad, restreinte à la frame idle.
bool Resolve(Entry* e, void** out_tex, ImVec2* uv0, ImVec2* uv1, float* cw, float* ch,
             bool dx7, void* pal_override = nullptr) {
  if (!e || !e->spr || !e->act) return false;
  const int handle_off = dx7 ? kCTexDX7Handle : kCTexDX9Handle;
  bool ok = false;
  __try {
    void* frame =
        reinterpret_cast<ActGetFrameFn>(kActGetFrame)(e->act, nullptr, 0, 0);
    if (!frame) return false;
    char* fr = reinterpret_cast<char*>(frame);
    char* lbegin = *reinterpret_cast<char**>(fr + 0x20);
    char* lend = *reinterpret_cast<char**>(fr + 0x24);
    const int nlayers = static_cast<int>((lend - lbegin) / 0x24);
    if (nlayers <= 0 || nlayers > 64) return false;
    char* spr = reinterpret_cast<char*>(e->spr);
    void* atlas = reinterpret_cast<void*>(
        *reinterpret_cast<uintptr_t*>(kSceneCtxPtr) + 0xc0);
    void* best = nullptr;
    ImVec2 b0, b1;
    float bw = 0.0f, bh = 0.0f;
    long best_area = -1;
    for (int i = 0; i < nlayers; ++i) {
      int* L = reinterpret_cast<int*>(lbegin + i * 0x24);
      const int sprNo = L[2], sprType = L[8];
      if (sprNo < 0 || sprType >= 2) continue;
      const int cellBase = *reinterpret_cast<int*>(spr + 0x510 + sprType * 0xc);
      const int cellEnd = *reinterpret_cast<int*>(spr + 0x514 + sprType * 0xc);
      if (static_cast<unsigned>(sprNo) >=
          static_cast<unsigned>((cellEnd - cellBase) >> 2))
        continue;
      short* cell = *reinterpret_cast<short**>(cellBase + sprNo * 4);
      if (!cell) continue;
      // Palette de l'atlas : celle EMBARQUÉE du sprite (spr+0x110) par défaut, ou une
      // palette de COULEUR fournie (head_<N>.pal, cf. ColorPalette) pour recolorer.
      const int palette = pal_override
          ? static_cast<int>(reinterpret_cast<uintptr_t>(pal_override))
          : static_cast<int>(reinterpret_cast<uintptr_t>(spr + 0x110));
      int geom[12] = {0};
      void* ctex = reinterpret_cast<AtlasFn>(kAtlasGetCached)(atlas, nullptr, cell,
                                                             palette, geom);
      if (!ctex)
        ctex = reinterpret_cast<AtlasFn>(kAtlasBuild)(atlas, nullptr, cell, palette,
                                                     geom);
      if (!ctex) continue;
      const long area = static_cast<long>(cell[0]) * static_cast<long>(cell[1]);
      if (area > best_area) {
        best_area = area;
        best = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctex) + handle_off);
        bw = static_cast<float>(cell[0]);
        bh = static_cast<float>(cell[1]);
        b0 = ImVec2(*reinterpret_cast<float*>(&geom[3]),
                    *reinterpret_cast<float*>(&geom[4]));
        b1 = ImVec2(*reinterpret_cast<float*>(&geom[5]),
                    *reinterpret_cast<float*>(&geom[6]));
      }
    }
    if (best) {
      *out_tex = best;
      *uv0 = b0;
      *uv1 = b1;
      *cw = bw;
      *ch = bh;
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return ok;
}

// ── Palette de COULEUR de cheveux N ─────────────────────────────────────────────
// Fichier GRF `palette\머리\head_<N>.pal` (머리 = octets CP949 B8 D3 B8 AE), une palette
// 256×RGBA par couleur. Chargé via le MÊME resource manager que les .spr/.act
// (kMakeKey/kLoadRes) -> objet CPaletteRes dont la table est à +0x110 (RE
// CPaletteRes_Load 0x00725b60 : lit 0x400 o dans this+0x110). On renvoie l'adresse de
// cette table, à passer comme `palette` à l'atlas (exactement comme spr+0x110) pour
// RECOLORER la coupe. ⚠ Chemin bâti au snprintf, ZÉRO std::string / builder natif —
// c'était l'interop std::string qui plantait (edi corrompu ; cf. mémoire charselect).
// Cache par couleur (pointeur de ressource, comme hairicon::Ensure met en cache spr/act).
struct PalEntry { int color = -1; void* pal = nullptr; };
constexpr int kPalCacheN = 64;
PalEntry g_palcache[kPalCacheN];
int g_palnext = 0;

void* ColorPalette(int color) {
  if (color < 0) return nullptr;
  for (int i = 0; i < kPalCacheN; ++i)
    if (g_palcache[i].color == color) return g_palcache[i].pal;

  void* paldata = nullptr;
  __try {
    char path[64];
    // "palette\" + 머리(B8 D3 B8 AE) + "\head_<N>.pal"
    std::snprintf(path, sizeof(path), "palette\\\xB8\xD3\xB8\xAE\\head_%d.pal", color);
    void* mgr = reinterpret_cast<TexMgrGetFn>(kTexMgrGet)();
    void* res = reinterpret_cast<LoadResFn>(kLoadRes)(
        mgr, nullptr, reinterpret_cast<MakeKeyFn>(kMakeKey)(path));
    // ⚠ CPaletteRes garde le BRUT (RGBA) à +0x110 et la palette CONVERTIE (format
    // 16-bit du moteur, via sub_566770 à la charge) à +0x510. L'atlas/le sprite
    // utilisent la CONVERTIE (spr+0x110 d'un CSprite EST déjà convertie). On pointe
    // donc +0x510, sinon couleurs en bouillie.
    if (res) paldata = reinterpret_cast<char*>(res) + 0x510;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    paldata = nullptr;
  }

  PalEntry& e = g_palcache[g_palnext];
  g_palnext = (g_palnext + 1) % kPalCacheN;
  e.color = color;
  e.pal = paldata;
  return paldata;
}
}  // namespace hairicon

template <typename T>
T Read(const void* base, int off) {
  T v{};
  std::memcpy(&v, reinterpret_cast<const char*>(base) + off, sizeof(T));
  return v;
}

int ReadCount(uintptr_t addr) {
  __try {
    return *reinterpret_cast<const int*>(addr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// Nom lisible d'un job — getter NATIF `Job_GetDisplayNameOrResName` 0x00d5bb40 :
//   char* __thiscall(void* this = 0x015fa3c0 (adresse LITTÉRALE, pas un pointeur à
//                    déréférencer), unsigned classId, int sex)   // sex 0=F, 1=M
// C'est exactement ce qu'appelle le char-select NATIF (FUN_0079f150 @0x0079f1d7)
// avec CHARACTER_INFO+0x54 (job) et +0xae (sexe) — on fait donc pareil.
// La table vient des Lua DataInfo (PCJobNameTable, chargée au boot par
// GameInfo_LoadClassNameTables 0x00d63b20) : le retour est NON-owned (ne pas
// libérer) et encodé dans la code-page client -> passer par LocalToUtf8().
// Renvoie nullptr tant que la table n'est pas chargée (le natif renvoie "").
// ⚠ Ne jamais passer sex = -1 ici : ce mode lit la session in-game, absente au
// char-select ; on passe le sexe du personnage.
const char* JobName(int job, int sex) {
  if (job < 0) return nullptr;
  // __fastcall(ecx=this, edx=inutilisé, …) : strictement équivalent au __thiscall
  // (EDX n'est pas lu) et c'est la convention déjà employée dans basic_info.cc.
  using GetJobName_t = const char*(__fastcall*)(void*, void*, unsigned, int);
  const char* n = nullptr;
  __try {
    n = reinterpret_cast<GetJobName_t>(0x00d5bb40)(
        reinterpret_cast<void*>(0x015fa3c0), nullptr,
        static_cast<unsigned>(job), sex);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
  return (n && *n) ? n : nullptr;
}

// Répertoire de l'exe (pour la config yaml).
std::string SettingsPath() {
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string p(buf);
  const auto s = p.find_last_of("\\/");
  if (s != std::string::npos) p.resize(s + 1);
  return p + "bourgeon_settings.yaml";
}

// ── Table des PLACES (scène banquet) ──────────────────────────────────────────
// L'utilisateur a numéroté le décor (1..25) : le slot i occupe la place n°(i+1).
// Coords NORMALISÉES [0..1] sur le fond (indépendantes de la résolution : on
// dessine le fond étiré plein écran, donc place écran = (nx*disp.x, ny*disp.y)).
// `chair` = point où poser les PIEDS du perso (assis/debout sur le siège) ; `scale`
// = hauteur du pantin en fraction de la hauteur d'écran (perspective : trônes au
// fond = petits, bancs du 1er plan = grands). Valeurs extraites à l'œil de l'image
// numérotée -> AJUSTABLES en jeu via l'éditeur de sièges (staff), qui journalise la
// table prête à recoller ici. Ordre = 1 grand trône, 2 petit trône, 3-5 petites
// tables, 6-14 rangée haute, 15/16 bouts, 17-25 rangée basse.
struct Seat { float nx, ny, scale; };
Seat g_seats[] = {
    {0.460f, 0.295f, 0.115f},  // 1  grand trône
    {0.545f, 0.304f, 0.115f},  // 2  petit trône
    {0.414f, 0.453f, 0.115f},  // 3  petite table gauche
    {0.497f, 0.453f, 0.115f},  // 4  petite table milieu
    {0.579f, 0.453f, 0.115f},  // 5  petite table droite
    {0.142f, 0.589f, 0.115f},  // 6  rangée haute
    {0.232f, 0.589f, 0.115f},  // 7
    {0.321f, 0.589f, 0.115f},  // 8
    {0.408f, 0.589f, 0.115f},  // 9
    {0.498f, 0.589f, 0.115f},  // 10
    {0.586f, 0.589f, 0.115f},  // 11
    {0.674f, 0.589f, 0.115f},  // 12
    {0.763f, 0.589f, 0.115f},  // 13
    {0.852f, 0.589f, 0.115f},  // 14
    {0.041f, 0.677f, 0.115f},  // 15 bout gauche
    {0.954f, 0.679f, 0.115f},  // 16 bout droit
    {0.111f, 0.837f, 0.115f},  // 17 rangée basse
    {0.207f, 0.837f, 0.115f},  // 18
    {0.305f, 0.837f, 0.115f},  // 19
    {0.401f, 0.837f, 0.115f},  // 20
    {0.497f, 0.837f, 0.115f},  // 21
    {0.594f, 0.837f, 0.115f},  // 22
    {0.689f, 0.837f, 0.115f},  // 23
    {0.782f, 0.837f, 0.115f},  // 24
    {0.880f, 0.837f, 0.115f},  // 25
};
constexpr int kSeatCount = static_cast<int>(sizeof(g_seats) / sizeof(g_seats[0]));

// ── Pagination des bancs ──────────────────────────────────────────────────────
// MAX_CHARS serveur (45, bientôt 60) dépasse les 25 sièges de la table. La table
// d'HONNEUR (5 premiers sièges = trônes + petites tables) reste FIXE sur toutes les
// pages (toujours les persos 0-4) ; les 20 BANCS restants sont PAGINÉS :
//   page 0 : bancs = persos 5..24    (+ les 5 fixes -> persos 0..24)
//   page 1 : bancs = persos 25..44   (+ les 5 fixes -> 0..4 puis 25..44)
constexpr int kHeadSeats = 5;                        // trônes + petites tables (fixes)
constexpr int kRowSeats  = kSeatCount - kHeadSeats;  // bancs paginés (20)

// Slot de personnage affiché au siège `seat` pour la page `page`.
int CharForSeat(int seat, int page) {
  return (seat < kHeadSeats) ? seat : (seat + page * kRowSeats);
}
// Nombre de pages pour `cap` slots (seuls les bancs portent la pagination).
int NumPages(int cap) {
  if (cap <= kHeadSeats) return 1;
  return (cap - kHeadSeats + kRowSeats - 1) / kRowSeats;  // ceil
}

// ── Éditeur de layout (staff) : poignées déplaçables + dump des coordonnées ─────
// Toute position qui passe par Anchor() devient, en mode édition, une petite
// poignée qu'on glisse à la souris ; « Dump layout » journalise tous ces points
// (et la table des sièges) en bloc prêt à figer en dur. Sert à caler les petits
// décalages qu'on ne peut pas calculer d'avance. Enregistrement idempotent par nom
// (les fractions survivent aux changements de résolution).
struct AnchorPt { const char* name; float nx, ny; };
AnchorPt g_anchors[16];
int g_anchor_count = 0;

// Point de layout `name` (défaut def_nx/def_ny en FRACTIONS d'écran). Renvoie sa
// position ÉCRAN courante (px). En mode édition, dessine + déplace la poignée.
ImVec2 Anchor(const char* name, float def_nx, float def_ny, bool edit) {
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  AnchorPt* a = nullptr;
  for (int i = 0; i < g_anchor_count; ++i)
    if (std::strcmp(g_anchors[i].name, name) == 0) { a = &g_anchors[i]; break; }
  if (!a && g_anchor_count < static_cast<int>(sizeof(g_anchors) / sizeof(g_anchors[0]))) {
    a = &g_anchors[g_anchor_count++];
    a->name = name;
    a->nx = def_nx;
    a->ny = def_ny;
  }
  if (!a) return ImVec2(def_nx * disp.x, def_ny * disp.y);
  ImVec2 c(a->nx * disp.x, a->ny * disp.y);
  if (edit) {
    ImGui::PushID(name);
    ImGui::SetCursorScreenPos(ImVec2(c.x - 9.0f, c.y - 9.0f));
    ImGui::InvisibleButton("##anc", ImVec2(18.0f, 18.0f));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      a->nx += ImGui::GetIO().MouseDelta.x / disp.x;
      a->ny += ImGui::GetIO().MouseDelta.y / disp.y;
      c = ImVec2(a->nx * disp.x, a->ny * disp.y);
    }
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddCircleFilled(c, 5.0f,
                        ImGui::IsItemActive() ? IM_COL32(255, 255, 150, 255)
                                              : IM_COL32(120, 200, 255, 230));
    fg->AddText(ImVec2(c.x + 8.0f, c.y - 6.0f), IM_COL32(150, 210, 255, 255), name);
    ImGui::PopID();
  }
  return c;
}

// Récupère (SEH, POD only) les dimensions + le pointeur des pixels BGRA de la
// texture native du décor. ⚠ AUCUN objet C++ ici : un std::vector/std::string dans
// un __try déclenche C2712 (« __try dans une fonction nécessitant un déroulement
// d'objet »). D'où la scission avec la conversion C++ ci-dessous.
const uint8_t* FetchHallBgra(int* out_w, int* out_h) {
  __try {
    using TexMgr_t  = void*(__cdecl*)();
    using MakeKey_t = void*(__cdecl*)(const char*);
    using LoadTex_t = void*(__fastcall*)(void*, void*, void*);
    void* mgr = reinterpret_cast<TexMgr_t>(kTexMgrGet)();
    if (!mgr) return nullptr;
    void* key = reinterpret_cast<MakeKey_t>(kTexMakeKey)(kHallBmpPath);
    if (!key) return nullptr;
    void* t = reinterpret_cast<LoadTex_t>(kTexLoad)(mgr, nullptr, key);
    if (!t) return nullptr;
    const int w = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexW);
    const int h = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexH);
    const uint8_t* bgra =
        *reinterpret_cast<const uint8_t**>(static_cast<char*>(t) + kTexPix);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !bgra) return nullptr;
    *out_w = w;
    *out_h = h;
    return bgra;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// Recopie (SEH, POD only) le buffer natif BGRA vers ARGB opaque. Sous SEH car
// `bgra` est de la mémoire native (le loader garantit w*h*4, mais on se protège).
bool CopyBgraToArgb(uint8_t* dst, const uint8_t* bgra, int w, int h) {
  __try {
    for (int i = 0; i < w * h; ++i) {
      dst[i * 4 + 0] = bgra[i * 4 + 0];
      dst[i * 4 + 1] = bgra[i * 4 + 1];
      dst[i * 4 + 2] = bgra[i * 4 + 2];
      dst[i * 4 + 3] = 0xFF;  // fond plein écran : toujours opaque
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ── Fond banquet : BMP client -> texture ImGui (cache, invalidé au reset device) ─
// Chargé une fois via le loader natif, converti BGRA->ARGB pour Overlay. Renvoie 0
// tant que le .bmp est absent (le décor tombe alors sur un fond sombre uni).
void* LoadHallTexture() {
  static void* s_tex = nullptr;
  static unsigned s_epoch = 0xffffffff;
  static bool s_tried = false;  // ne pas re-tenter en boucle si le .bmp manque
  const unsigned e = Overlay_DeviceEpoch();
  if (e != s_epoch) {  // device (re)créé -> l'ancienne texture est morte
    s_tex = nullptr;
    s_tried = false;
    s_epoch = e;
  }
  if (s_tex || s_tried) return s_tex;
  s_tried = true;
  int w = 0, h = 0;
  const uint8_t* bgra = FetchHallBgra(&w, &h);
  if (!bgra) return s_tex;  // .bmp absent/invalide -> fond uni
  // Conversion + upload en C++ normal : le std::vector a un destructeur, donc HORS
  // __try (sinon C2712). La recopie lisant `bgra` reste, elle, SEH-gardée.
  std::vector<uint8_t> argb(static_cast<size_t>(w) * h * 4);
  if (!CopyBgraToArgb(argb.data(), bgra, w, h)) return s_tex;
  s_tex = Overlay_CreateTextureARGB(argb.data(), w, h);
  return s_tex;
}

}  // namespace

CharSelect::CharSelect(MoonlightAuth* auth) : auth_(auth) {
  std::ifstream f(SettingsPath());
  if (f) {
    try {
      const YAML::Node root = YAML::Load(f);
      const YAML::Node cs = root["char_select"];
      if (cs) {
        enabled_ = cs["imgui"].as<bool>(true);  // défaut activé ; opt-out explicite
        force_ = cs["force"].as<bool>(false);
      }
    } catch (const std::exception& e) {
      LogError("[CharSelect] config illisible: {}", e.what());
    }
  }
  LogDiag("[CharSelect] {}{}",
          enabled_ ? "activé (défaut)" : "désactivé (opt-out yaml)",
          force_ ? " [force: gate Moonlight ignoré]" : "");
  // Détour du handler de réponse « delete reserve » : supprime la msgbox native de
  // refus (guilde/groupe…) qui traînerait sous notre UI, et capte le code exact.
  InstallDeleteAckDetour();
  // Détour de la modale native bloquante : supprime toute msgbox (nom pris à la
  // création, etc.) qui figerait ImGui tant qu'on couvre le natif.
  InstallShowModalDetour();
}

int CharSelect::SlotCapacity() const {
  const int sum = ReadCount(kNormalSlots) + ReadCount(kPremiumSlots) +
                  ReadCount(kBillingSlots);
  const int creatable = ReadCount(kCreatableSlots);
  int cap = sum > creatable ? sum : creatable;
  if (cap < 0) cap = 0;
  if (cap > 128) cap = 128;  // garde-fou (MAX_CHARS serveur monte à 60)
  return cap;
}

bool CharSelect::ReadSlot(int slot, CharView* out) const {
  *out = CharView{};
  out->slot = slot;
  __try {
    void* d = *reinterpret_cast<void**>(kUICmdDisp);
    if (!d) return false;
    auto fn = reinterpret_cast<DispCmd_t>(
        (*reinterpret_cast<uintptr_t**>(d))[kVfDispCmd / 4]);
    void* c = fn(d, kCmdGetChar, slot, 0, 0, 0);
    if (!c) return false;  // slot vide

    out->occupied = true;
    out->gid = Read<uint32_t>(c, ci::kGid);
    std::memcpy(out->name, reinterpret_cast<char*>(c) + ci::kName, 24);
    out->name[24] = '\0';
    out->base_level = Read<int16_t>(c, ci::kBaseLevel);
    out->job_level = Read<int32_t>(c, ci::kJobLevel);
    out->job = Read<int16_t>(c, ci::kJob);
    out->hp = Read<int64_t>(c, ci::kHp);
    out->max_hp = Read<int64_t>(c, ci::kMaxHp);
    out->sp = Read<int64_t>(c, ci::kSp);
    out->max_sp = Read<int64_t>(c, ci::kMaxSp);
    out->zeny = Read<int32_t>(c, ci::kZeny);
    std::memcpy(out->map, reinterpret_cast<char*>(c) + ci::kMap, 16);
    out->map[16] = '\0';
    // Coupe l'extension ".gat" pour l'affichage.
    if (char* dot = std::strstr(out->map, ".")) *dot = '\0';
    const unsigned char* s = reinterpret_cast<unsigned char*>(c) + ci::kStr;
    out->str = s[0]; out->agi = s[1]; out->vit = s[2];
    out->intel = s[3]; out->dex = s[4]; out->luk = s[5];
    out->sex = Read<uint8_t>(c, ci::kSex);
    // 99 = « sexe du compte » -> g_Account_Sex, comme le natif (cf. kAccountSex).
    out->sex_eff = (out->sex == 99) ? *reinterpret_cast<const uint8_t*>(kAccountSex)
                                    : out->sex;
    out->del_rev_date = Read<int32_t>(c, ci::kDelDate);
    // Apparence (paperdoll) — u16 non signés (les view ids dépassent 32767).
    out->hair          = Read<uint16_t>(c, ci::kHair);
    out->body          = Read<uint16_t>(c, ci::kBody);
    out->head_low      = Read<uint16_t>(c, ci::kHeadLow);
    out->head_top      = Read<uint16_t>(c, ci::kHeadTop);
    out->head_mid      = Read<uint16_t>(c, ci::kHeadMid);
    out->hair_color    = Read<uint16_t>(c, ci::kHairCol);
    out->clothes_color = Read<uint16_t>(c, ci::kClothesCol);
    out->garment       = static_cast<int>(Read<uint32_t>(c, ci::kGarment));
    out->moves_avail   = static_cast<int>(Read<uint32_t>(c, ci::kMoveCnt));
    out->rename_avail  = static_cast<int>(Read<uint32_t>(c, ci::kRenameCnt));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *out = CharView{};
    out->slot = slot;
    return false;
  }
}

void CharSelect::EnterGame(int slot) {
  if (entering_) return;  // une seule entrée par passage à l'écran (edge-trigger)
  // ⚠ NE PAS envoyer CH_SELECT_CHAR (0x0066) à la main. Le bouton « OK » natif fait
  // bien plus que d'émettre un paquet, et il ne l'émet même pas lui-même :
  //
  //   UINewSelectCharWnd_OnMsg 0x0079d610 (vtbl+0x94, msg=6, ctrl=0xB8) :
  //     lit g_CharSelect_SelectedSlot -> SendMsg(mode,8,slot) = CHARACTER_INFO*
  //     -> refuse si +0x9e (suppression en cours)
  //     -> Own_SetCharName(0x015fa3c0, ci+0x6c)   <<< LE NOM DU PERSO
  //     -> Own_SetSex(0x015fa3c0, ci+0xae == 99 ? g_Account_Sex : ci+0xae)
  //     -> SendMsg(mode, 0, 0x2712) -> mode+0xc = 9
  //   ... et c'est l'ÉTAT 9 (CLoginMode_OnStateEnter 0x00d24080) qui construit et
  //   envoie [0x0066][slot]. L'envoyer soi-même = DOUBLE envoi.
  //
  // Le nom du perso est stocké OBFUSQUÉ (XOR) à 0x015fab54 ; le chat le relit via
  // Own_GetCharName et compose "%s : %s" (CZ_REQUEST_CHAT 0x00f3). Sans cette
  // initialisation, rAthena rejette le message (« sent a message using an incorrect
  // name ») et force un relog. C'était le bug du char-select ImGui.
  //
  // g_CharSelect_SelectedSlot doit être posé AVANT et RESTER en place : le handler
  // HC_NOTIFY_ZONESVR (Net_OnNotifyZoneSvr_EnterGame 0x00d23180) le RELIT ensuite
  // pour semer le cache local (g_Own_* / g_OwnLook_*) via cmd 0x2719 — sans lui,
  // un slot != 0 sèmerait l'état du MAUVAIS personnage.
  __try {
    *reinterpret_cast<uint8_t*>(kSelectedSlot) = static_cast<uint8_t>(slot);
    void* wnd = *reinterpret_cast<void**>(kCharSelWndPtr);
    // Garde de vtable : le cache mgr+0x3d4 n'est jamais remis à zéro à la
    // destruction -> il peut pendouiller hors de cet écran.
    if (!wnd || *reinterpret_cast<uintptr_t*>(wnd) != kCharSelWndVtbl) {
      LogError("[CharSelect] fenêtre native absente/invalide -> entrée en jeu "
               "impossible (slot {}). Utilise « Mode Classique ».", slot);
      return;
    }
    // ⚠ RET 0x18 = 6 args pile (même piège que UILoginWnd_OnMsg : un typedef à
    // 5 args corromprait ESP).
    using WndOnMsg_t = int(__thiscall*)(void*, int, int, int, int, int, int);
    reinterpret_cast<WndOnMsg_t>(kCharSelOnMsg)(wnd, 0, 6, 0xB8, 0, 0, 0);
    entering_ = true;
    enter_tick_ = GetTickCount();
    LogDiag("[CharSelect] entrée en jeu slot={} (séquence native OnMsg 0xB8)", slot);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogError("[CharSelect] exception pendant l'entrée en jeu (slot {})", slot);
  }
}

void CharSelect::DrawDollAt(const CharView& v, float cx, float chair_y,
                           float box_h) {
  // Paperdoll composé (corps + coiffes + garment, palettes) rendu par le moteur de
  // capture PARTAGÉ de basic_info : le hook de capture (Actor_SubmitSpriteQuad
  // 0x00a1b7c0) est global, on passe donc par RenderDoll, seul chemin AUTONOME
  // (apparence en paramètre, ni session en jeu ni UIWindow requises).
  // Pieds ancrés en (cx, chair_y), corps centré en X, box de hauteur box_h vers le
  // haut. RenderDoll prend des coords ÉCRAN (cf. l'ancienne grille).
  //
  // Échelle sprite par JOB : le client natif rétrécit les classes BÉBÉ
  // (Actor_GetJobSpriteScale 0x00d7fd30 -> 0.75 bébé 1re classe, 0.80/0.82 les
  // autres, 1.0 non-bébé). Ce getter est DÉJÀ appliqué à notre capture (via
  // Actor_DrawSprites 0x7ac820), mais le fit hauteur de RenderDoll le neutralise
  // (il ré-étire le sprite bébé pour remplir la box). On RÉ-APPLIQUE donc le même
  // facteur à la box : l'échelle interne `s` retombe sur celle d'un adulte et le
  // bébé rend bien `js` fois plus petit, pieds toujours ancrés au banc.
  using JobScaleFn = float(__stdcall*)(int);
  float js = reinterpret_cast<JobScaleFn>(0x00d7fd30)(v.job);
  if (!(js > 0.05f && js <= 1.0f)) js = 1.0f;  // garde-fou (job inconnu)
  const float dh = box_h * js;
  const float w = dh * 0.62f;  // aspect sprite RO
  const float x = cx - w * 0.5f;
  const float y = chair_y - dh;

  bool drawn = false;
  if (BasicInfoTweaks* bi = Bourgeon::Instance().basic_info()) {
    BasicInfoTweaks::DollLook look;
    look.sex           = v.sex_eff;  // 99 déjà résolu en sexe de compte
    look.job           = v.job;
    look.body          = v.body;
    look.hair          = v.hair;
    look.hair_color    = v.hair_color;
    look.clothes_color = v.clothes_color;
    look.head_low      = v.head_low;
    look.head_top      = v.head_top;
    look.head_mid      = v.head_mid;
    look.garment       = v.garment;
    // Perso en attente de suppression : sprite teinté ROUGE PULSANT (multiplication
    // au dessin, pas de re-capture). Les canaux vert/bleu descendent (rouge fort)
    // puis remontent, en boucle douce (cosinus). 0xFFFFFFFF = pas de teinte sinon.
    uint32_t tint = 0xFFFFFFFFu;
    if (v.del_rev_date > 0) {
      const float ph = (GetTickCount() % 1000) / 1000.0f;          // 0..1 sur 1 s
      const float pulse = 0.5f - 0.5f * std::cos(ph * 6.2831853f);  // 0..1..0 doux
      const int gb = 70 + static_cast<int>((1.0f - pulse) * 150.0f);  // 70 fort..220
      tint = IM_COL32(255, gb, gb, 255);
    }
    // dir 0 = de face ; anim 2 = ASSIS (les convives sont attablés). La pose assise
    // pose aussi le perso SUR le banc (son point d'assise, pas ses pieds, arrive à
    // chair_y) -> corrige le « flottement » de la pose debout.
    drawn = bi->RenderDoll(look, x, y, w, dh, /*dir=*/0, /*anim=*/2, tint);
  }
  if (!drawn) {
    // Placeholder tant que la capture n'est pas prête (budget par frame, jusqu'à 25
    // pantins) : une silhouette discrète, même encombrement -> zéro saut de scène.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, chair_y),
                      IM_COL32(40, 44, 58, 120), 4.0f);
    char t[24];
    std::snprintf(t, sizeof(t), "job %d", v.job);
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(cx - ts.x * 0.5f, y + dh * 0.5f),
                IM_COL32(150, 160, 190, 170), t);
  }
}

void CharSelect::DrawCreateDoll(float x, float y, float w, float h) {
  // Aperçu de CRÉATION : un Novice (job/body 0) avec les cheveux/couleur/sexe choisis,
  // debout de face. Même moteur de capture partagé que DrawDollAt ; la capture est
  // re-clé par apparence -> changer un curseur met l'aperçu à jour à la frame suivante.
  bool drawn = false;
  if (BasicInfoTweaks* bi = Bourgeon::Instance().basic_info()) {
    BasicInfoTweaks::DollLook look;  // tout à 0 par défaut = Novice sans équipement
    look.sex        = create_sex_;
    look.hair       = create_hair_;
    look.hair_color = create_hair_color_;
    drawn = bi->RenderDoll(look, x, y, w, h, create_dir_, /*anim=*/0);  // debout
  }
  if (!drawn) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                      IM_COL32(40, 44, 58, 120), 4.0f);
    const char* t = "...";
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, y + h * 0.5f),
                IM_COL32(150, 160, 190, 170), t);
  }
}

void CharSelect::DrawHairIcon(int hair, float x, float y, float sz, int color_override) {
  // Icône de coiffure = frame du .spr natif (sexe courant), ajustée à la case en
  // gardant le ratio, centrée. Rien dessiné tant que le sprite n'est pas prêt.
  // `color_override` >= 0 : recolore la coupe avec la palette head_<N>.pal (swatches).
  // Remap legacy des 12 coiffures historiques : id de coiffure -> n° de fichier .spr.
  // Le rendu JEU applique cette table (préservée par le patch WARP Allow65kHairs pour les
  // index < 43, identité au-delà) ; on la RÉPLIQUE ici pour que l'icône = le doll = le
  // rendu en jeu (WYSIWYG). ⚠ On n'envoie JAMAIS ce fichier : create_hair_ (id brut) part
  // au serveur, qui le stocke et laisse le client ré-appliquer la table. Vérifié à l'œil
  // (1↔2, 3→7/6→3/7→6, 4↔5, 11↔12 ; 8/9/10 et 13+ identité). Cf. mémoire charselect_imgui.
  static const int kHairFile[13] = {0, 2, 1, 7, 5, 4, 3, 6, 8, 9, 10, 12, 11};
  const int file = (hair >= 1 && hair <= 12) ? kHairFile[hair] : hair;

  void* tex = nullptr;
  ImVec2 uv0, uv1;
  float cw = 0.0f, ch = 0.0f;
  hairicon::Entry* e = hairicon::Ensure(file, create_sex_);
  void* pal_override =
      (color_override >= 0) ? hairicon::ColorPalette(color_override) : nullptr;
  if (hairicon::Resolve(e, &tex, &uv0, &uv1, &cw, &ch, g_imgui_dx7_active, pal_override) &&
      tex && cw >= 1.0f && ch >= 1.0f) {
    const float s = (sz / cw < sz / ch) ? sz / cw : sz / ch;
    const float w = cw * s, h = ch * s;
    ImGui::GetWindowDrawList()->AddImage(
        reinterpret_cast<ImTextureID>(tex),
        ImVec2(x + (sz - w) * 0.5f, y + (sz - h) * 0.5f),
        ImVec2(x + (sz + w) * 0.5f, y + (sz + h) * 0.5f), uv0, uv1);
  }
}

void CharSelect::DriveNativeCtrl(int ctrl, int slot) {
  // Pose le slot sélectionné (le natif le relit) puis pilote UINewSelectCharWnd
  // OnMsg (msg 6 / ctrl). Aucun paquet fabriqué ici : le natif construit tout.
  __try {
    *reinterpret_cast<uint8_t*>(kSelectedSlot) = static_cast<uint8_t>(slot);
    void* wnd = *reinterpret_cast<void**>(kCharSelWndPtr);
    if (!wnd || *reinterpret_cast<uintptr_t*>(wnd) != kCharSelWndVtbl) {
      LogError("[CharSelect] fenêtre native absente/invalide -> ctrl 0x{:x} ignoré "
               "(slot {})", ctrl, slot);
      return;
    }
    // RET 0x18 = 6 args pile (cf. EnterGame) : typedef à 6 args obligatoire.
    using WndOnMsg_t = int(__thiscall*)(void*, int, int, int, int, int, int);
    reinterpret_cast<WndOnMsg_t>(kCharSelOnMsg)(wnd, 0, 6, ctrl, 0, 0, 0);
    LogDiag("[CharSelect] ctrl natif 0x{:x} (slot {})", ctrl, slot);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogError("[CharSelect] exception ctrl 0x{:x} (slot {})", ctrl, slot);
  }
}

void CharSelect::DriveModeCmd(int cmd) {
  // Envoie une commande au MODE courant (dispatcher *(0x0121333c), vtbl+0x18) — le
  // même point d'entrée que ReadSlot (cmd 8) et que le natif quand il quitte l'écran.
  // 5 args pile : DispCmd_t est le typedef partagé (aucun ne sert ici, tous à 0).
  __try {
    void* d = *reinterpret_cast<void**>(kUICmdDisp);
    if (!d) {
      LogError("[CharSelect] dispatcher de mode absent -> commande {} ignorée", cmd);
      return;
    }
    auto fn = reinterpret_cast<DispCmd_t>(
        (*reinterpret_cast<uintptr_t**>(d))[kVfDispCmd / 4]);
    fn(d, cmd, 0, 0, 0, 0);
    LogDiag("[CharSelect] commande de mode {} envoyée", cmd);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogError("[CharSelect] exception sur la commande de mode {}", cmd);
  }
}

void CharSelect::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  // Réarme à chaque arrivée sur l'écran login/char-select.
  if (mode_type == ModeMgr::ModeType::kLogin) {
    active_ = false;
    selected_ = -1;
    page_ = 0;
    native_fallback_ = false;
    native_op_ = false;
    op_prev_nfilled_ = -1;
    entering_ = false;
    quitting_ = false;
    left_ = false;
    del_reject_until_ = 0;
    del_reject_seq_seen_ = g_del_reject_seq;  // ne pas ressortir un refus d'avant
  }
}

void CharSelect::OnRenderLoginUI() {
  // Le détour delete-ack lit g_cover_active : il ne supprime la msgbox native que
  // lorsqu'on couvre effectivement l'écran (posé à true juste avant le Begin plein
  // écran). Remis à false ici -> tout retour anticipé (login, repli natif, op native)
  // laisse le natif gérer sa propre boîte.
  g_cover_active = false;
  if (!enabled_ || native_fallback_) return;
  // Règle produit : char-select ImGui réservé au parcours de login Moonlight.
  // Login natif / « Login classique » => on laisse le char-select NATIF. Le flag
  // `force` (dev) court-circuite ce gate pour tester via un login natif.
  if (!force_ && (auth_ == nullptr || !auth_->DroveMoonlightLogin())) return;
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  if (disp.x <= 0.0f || disp.y <= 0.0f) return;  // garde minimize

  // ── Écran quitté (« Revenir au login ») : on ne dessine plus rien ────────────
  // On ne peut pas se fier aux CHARACTER_INFO (encore lisibles un moment après la
  // déconnexion) ni à OnModeSwitch (un re-login ne change pas de MODE). On se réarme
  // donc sur la fenêtre NATIVE du char-select (id 0x115) : absente = on est à l'écran
  // de connexion, présente = un nouveau char-select s'est ouvert, on reprend la main.
  if (left_) {
    if (!NativeCharSelectAlive()) {
      active_ = false;
      return;
    }
    left_ = false;
    selected_ = -1;
    page_ = 0;
  }

  // Détection char-select : au moins un slot chargé. (Sur l'écran de login pur,
  // aucun perso n'est chargé -> on ne dessine rien.) La détection robuste par
  // fenêtre native viendra avec le masquage du natif.
  const int cap = SlotCapacity();
  if (cap <= 0) { active_ = false; return; }

  // Lit tous les slots (occupés + vides).
  static CharView views[128];
  int nfilled = 0;
  for (int i = 0; i < cap && i < 128; ++i) {
    if (ReadSlot(i, &views[i])) ++nfilled;
  }
  if (nfilled == 0) { active_ = false; return; }  // pas encore au char-select
  active_ = true;

  // Refus de « Programmer suppression » remonté par le détour (code EXACT, msgbox
  // native déjà supprimée) : un nouveau n° de séquence -> on arme le bandeau ~6 s.
  // Le SUCCÈS ne passe pas par ici : le natif a écrit del_rev_date, le perso se
  // marque « Suppression programmée » tout seul.
  if (g_del_reject_seq != del_reject_seq_seen_) {
    del_reject_seq_seen_ = g_del_reject_seq;
    del_reject_reason_ = g_del_reject_result;
    del_reject_until_ = GetTickCount() + 6000;
  }

  // ── Opération native éphémère (création / suppression définitive) ────────────
  // On s'est DÉCOUVERT pour laisser le dialogue natif (fenêtre 0xC8 ou confirm)
  // agir. On se re-couvre dès que la liste change (créa +1 / suppr -1) ou sur clic.
  // Aucune couverture ni capture d'input ici -> le natif est visible et utilisable.
  if (native_op_) {
    if (op_prev_nfilled_ < 0) op_prev_nfilled_ = nfilled;
    if (nfilled != op_prev_nfilled_) {  // opération terminée -> retour à la table
      native_op_ = false;
      op_prev_nfilled_ = -1;
      selected_ = -1;  // relaissera l'autofocus se replacer
    } else {
      ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.85f);
      ImGui::Begin("##charsel_return", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings);
      ImGui::TextUnformatted("Gestion native en cours…");
      if (ImGui::Button("Revenir à la table")) {
        native_op_ = false;
        op_prev_nfilled_ = -1;
      }
      ImGui::End();
    }
    return;
  }

  // La fenêtre plein écran occupe tout l'écran : un seul curseur (celui redessiné
  // par ImGui), partout. Posé chaque frame (le latch expire seul au retour au jeu).
  ro::SetFullscreenCursorActive();

  // Autofocus : à l'arrivée, présélectionne le 1er personnage -> Entrée joue
  // immédiatement, sans clic préalable.
  if (selected_ < 0) {
    for (int i = 0; i < cap && i < 128; ++i) {
      if (views[i].occupied) { selected_ = i; break; }
    }
  }

  // ── Couverture plein écran ──────────────────────────────────────────────────
  // Une fenêtre ImGui PLEIN ÉCRAN couvre ENTIÈREMENT le char-select natif ;
  // combinée à la capture clavier+souris (le hook WndProc avale l'input natif
  // quand WantCapture* est vrai), le natif est invisible ET inatteignable. Bien
  // plus simple que de masquer son rendu 3D (ce n'est pas une UIWindow isolable).
  ImGui::SetNextFrameWantCaptureKeyboard(true);
  ImGui::SetNextFrameWantCaptureMouse(true);

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(disp, ImGuiCond_Always);
  // Fond transparent : le décor banquet (ou un dégradé sombre de repli) est peint
  // par-dessus, plein cadre, dans la draw-list de la fenêtre.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags fs =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBringToFrontOnFocus;
  g_cover_active = true;  // on couvre le natif -> le détour supprime sa msgbox de refus
  ImGui::Begin("##charselect_full", nullptr, fs);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // ── Décor plein écran ────────────────────────────────────────────────────────
  if (void* hall = LoadHallTexture()) {
    dl->AddImage(reinterpret_cast<ImTextureID>(hall), ImVec2(0, 0), disp);
  } else {
    // Repli si le .bmp n'est pas déployé : dégradé sombre (haut -> bas).
    dl->AddRectFilledMultiColor(ImVec2(0, 0), disp, IM_COL32(24, 22, 30, 255),
                                IM_COL32(24, 22, 30, 255), IM_COL32(12, 11, 16, 255),
                                IM_COL32(12, 11, 16, 255));
  }
  // Léger voile bas pour asseoir titre/barre d'action sur le décor.
  dl->AddRectFilledMultiColor(ImVec2(0, disp.y - 96.0f), disp,
                              IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                              IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150));

  // ── Transitions (entrée en jeu / sortie de l'écran) : fondu au noir ─────────
  // Une fois l'entrée déclenchée (EnterGame -> OnMsg 0xB8), le natif VIDE les
  // CHARACTER_INFO pour semer l'état en jeu. Si on continuait à dessiner les sièges,
  // on verrait pendant ~½ s les dolls s'effacer et le slot retomber sur ses valeurs
  // par défaut (job 0 / sex 0 = Novice femelle). On masque cette fenêtre derrière un
  // fondu au noir + « Entrée en jeu… » : on ne relit ni ne redessine plus les slots.
  // Même traitement pour la FERMETURE du jeu (le temps que la boucle principale
  // sorte). Le retour au login, lui, n'a pas besoin de fondu : le formulaire
  // Moonlight reprend l'écran dès la frame suivante (cf. left_).
  if (entering_ || quitting_) {
    const unsigned long since = entering_ ? enter_tick_ : quit_tick_;
    const unsigned long el = GetTickCount() - since;
    float a = static_cast<float>(el) / 260.0f;  // fondu sur ~260 ms
    if (a > 1.0f) a = 1.0f;
    const int av = static_cast<int>(a * 255.0f);
    dl->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, av));
    const char* t = entering_ ? "Entrée en jeu…" : "Fermeture du jeu…";
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2((disp.x - ts.x) * 0.5f, disp.y * 0.5f - ts.y * 0.5f),
                IM_COL32(235, 230, 220, av), t);
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();  // WindowBg
    return;
  }

  auto can_enter_slot = [&](int i) {
    return i >= 0 && i < cap && i < 128 && views[i].occupied &&
           views[i].del_rev_date == 0;
  };
  const bool creatable_all = (ReadCount(kCreatableSlots) > 0);

  const ImGuiIO& io = ImGui::GetIO();
  // Un popup modal (ex. confirmation de suppression) est ouvert ? BeginPopupModal
  // bloque déjà la SOURIS, mais Enter/flèches passent par IsKeyPressed (global) ->
  // on les neutralise ici pour ne pas entrer en jeu / naviguer derrière le modal.
  const bool any_popup = ImGui::IsPopupOpen(
      nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

  // Enter joue le perso — SAUF si un champ texte a le focus (saisie de l'email) ou
  // qu'un popup est ouvert, sinon on entrerait en jeu en tapant.
  const bool enter_key = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
  if (enter_key && !io.WantTextInput && !any_popup && can_enter_slot(selected_))
    EnterGame(selected_);

  // ── Sièges (position = g_seats[i]) ; slot de perso = CharForSeat(i, page) ─────
  // Dessinés du fond vers l'avant (ordre de la table) : les sièges du 1er plan
  // passent par-dessus, et leur bouton invisible, soumis en dernier, gagne les
  // clics en cas de recouvrement. Les 5 sièges d'honneur restent sur les persos
  // 0-4 ; les 20 bancs affichent la page courante (cf. kHeadSeats/CharForSeat).
  const int n_pages = NumPages(cap);
  if (page_ >= n_pages) page_ = n_pages - 1;
  if (page_ < 0) page_ = 0;

  // ── Navigation aux flèches entre les persos visibles ─────────────────────────
  // Disposition 2D (banquet) -> navigation DIRECTIONNELLE : la flèche saute au perso
  // le mieux placé dans sa direction (projection positive), en favorisant le faible
  // écart latéral (même rangée pour ←/→, même colonne pour ↑/↓). Sans sélection
  // visible, une flèche sélectionne le 1er perso. Enter joue (géré plus bas).
  if (!seat_edit_ && !io.WantTextInput && !any_popup) {
    int dx = 0, dy = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) dx = 1;
    else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) dx = -1;
    else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) dy = 1;
    else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) dy = -1;
    if (dx != 0 || dy != 0) {
      // Position ÉCRAN du perso courant (siège dont le slot == selected_).
      float curx = 0.0f, cury = 0.0f;
      bool have_cur = false;
      for (int i = 0; i < kSeatCount && !have_cur; ++i) {
        const int cj = CharForSeat(i, page_);
        if (cj == selected_ && cj < cap && cj < 128 && views[cj].occupied) {
          curx = g_seats[i].nx * disp.x;
          cury = g_seats[i].ny * disp.y;
          have_cur = true;
        }
      }
      int best = -1, first_occ = -1;
      float best_score = 1e30f;
      for (int i = 0; i < kSeatCount; ++i) {
        const int cj = CharForSeat(i, page_);
        if (cj >= cap || cj >= 128 || !views[cj].occupied) continue;
        if (first_occ < 0) first_occ = cj;
        if (!have_cur || cj == selected_) continue;
        const float vx = g_seats[i].nx * disp.x - curx;
        const float vy = g_seats[i].ny * disp.y - cury;
        const float proj = vx * dx + vy * dy;           // avance dans la direction
        if (proj <= 1.0f) continue;                       // pas dans la direction
        const float perp = std::fabs(vx * dy - vy * dx);  // écart latéral
        const float score = proj + 2.5f * perp;           // favorise l'alignement
        if (score < best_score) { best_score = score; best = cj; }
      }
      if (best >= 0) selected_ = best;
      else if (!have_cur && first_occ >= 0) selected_ = first_occ;  // entrée dans la liste
    }
  }

  for (int i = 0; i < kSeatCount; ++i) {
    const int ci = CharForSeat(i, page_);     // slot de perso affiché à ce siège
    const bool has_slot = (ci < cap && ci < 128);
    if (!has_slot && !seat_edit_) continue;   // pas de slot -> siège nu
    CharView empty_view;                       // siège sans slot (mode édition)
    const CharView& v = has_slot ? views[ci] : empty_view;
    const bool empty = !v.occupied;
    if (empty && !creatable_all && !seat_edit_) continue;  // vide non créable

    const Seat& st = g_seats[i];
    const float cx = st.nx * disp.x;
    const float chair_y = st.ny * disp.y;
    const float box_h = st.scale * disp.y;
    const float w = box_h * 0.62f;
    const ImVec2 tl(cx - w * 0.5f, chair_y - box_h);
    const ImVec2 br(cx + w * 0.5f, chair_y + disp.y * 0.02f);

    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(tl);
    ImGui::InvisibleButton("seat", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    // Éditeur de sièges (staff) : glisser pour caler, molette pour la taille.
    if (seat_edit_) {
      if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        g_seats[i].nx += io.MouseDelta.x / disp.x;
        g_seats[i].ny += io.MouseDelta.y / disp.y;
      }
      if (hovered && io.MouseWheel != 0.0f)
        g_seats[i].scale =
            (std::max)(0.05f, g_seats[i].scale + io.MouseWheel * 0.005f);
      dl->AddRect(tl, br, IM_COL32(255, 210, 90, 220), 3.0f, 0, 1.5f);
      char lbl[8];
      std::snprintf(lbl, sizeof(lbl), "%d", i + 1);
      dl->AddText(ImVec2(cx - 4, tl.y - 14), IM_COL32(255, 220, 120, 255), lbl);
    }

    if (empty) {
      // Siège libre créable : marqueur « + ». Clic -> création native.
      const ImU32 col = hovered ? IM_COL32(255, 230, 150, 230)
                                : IM_COL32(210, 200, 180, 150);
      const float r = w * 0.30f;
      dl->AddCircle(ImVec2(cx, chair_y - box_h * 0.5f), r, col, 24, 2.0f);
      dl->AddLine(ImVec2(cx - r * 0.5f, chair_y - box_h * 0.5f),
                  ImVec2(cx + r * 0.5f, chair_y - box_h * 0.5f), col, 2.0f);
      dl->AddLine(ImVec2(cx, chair_y - box_h * 0.5f - r * 0.5f),
                  ImVec2(cx, chair_y - box_h * 0.5f + r * 0.5f), col, 2.0f);
      if (hovered) ImGui::SetTooltip("Créer un personnage (emplacement %d)", ci + 1);
      if (clicked && !seat_edit_ && has_slot) {
        // Ouvre notre popup de création ImGui (aperçu live + envoi 0xa39), au lieu de
        // la fenêtre native 0xC8. Sexe par défaut = celui du compte.
        create_slot_ = ci;
        create_name_[0] = '\0';
        create_hair_ = 1;
        create_hair_color_ = kMinHairColor;  // 0-8 mal faites -> défaut = 1re correcte
        create_pending_ = false;
        create_failed_ = false;
        const uint8_t acc_sex = *reinterpret_cast<uint8_t*>(kAccountSex);
        create_sex_ = (acc_sex <= 1) ? acc_sex : 1;
        create_open_req_ = true;  // ouverture différée hors du PushID (cf. plus bas)
      }
    } else {
      const bool sel = (selected_ == ci);
      // Halo au sol sous le perso.
      if (sel)
        dl->AddCircleFilled(ImVec2(cx, chair_y), w * 0.55f,
                            IM_COL32(255, 210, 110, 70), 28);
      else if (hovered)
        dl->AddCircleFilled(ImVec2(cx, chair_y), w * 0.50f,
                            IM_COL32(200, 210, 235, 40), 28);

      DrawDollAt(v, cx, chair_y, box_h);

      // Badge(s) coupon en haut du perso (le natif y met une image « Click to Rename »
      // via un effet ; nous une pastille ImGui) : rename (or) et/ou change-slot (cyan).
      // Signale au joueur qu'un coupon est ACTIF sur ce perso (flag CHARACTER_INFO).
      {
        float bx = cx - w * 0.5f + 4.0f;
        const float by = chair_y - box_h + 2.0f;
        const auto badge = [&](const char* txt, ImU32 bg) {
          const ImVec2 ts = ImGui::CalcTextSize(txt);
          const ImVec2 a(bx, by), b(bx + ts.x + 8.0f, by + ts.y + 3.0f);
          dl->AddRectFilled(a, b, bg, 3.0f);
          dl->AddRect(a, b, IM_COL32(0, 0, 0, 120), 3.0f);
          dl->AddText(ImVec2(bx + 4.0f, by + 1.0f), IM_COL32(30, 25, 10, 255), txt);
          bx = b.x + 3.0f;
        };
        if (v.rename_avail > 0) badge("Renom.", IM_COL32(255, 214, 120, 235));
        if (v.moves_avail > 0) badge("Slot", IM_COL32(140, 220, 255, 235));
      }

      // Étiquette nom + niveau sur UNE SEULE ligne, bandeau lisible. Le niveau suit
      // le nom (couleur dimmée pour rester secondaire).
      const char* nm = LocalToUtf8(v.name);
      char lvl[32];
      std::snprintf(lvl, sizeof(lvl), "  %d/%d", v.base_level, v.job_level);
      const ImVec2 ns = ImGui::CalcTextSize(nm);
      const ImVec2 ls = ImGui::CalcTextSize(lvl);
      const float tw = ns.x + ls.x;      // largeur totale (nom + niveau)
      const float ly = chair_y - 3.0f;   // remonté de 5px : plus compact, plus près du sprite
      const float lx = cx - tw * 0.5f;   // ligne centrée sous les pieds
      dl->AddRectFilled(ImVec2(lx - 6, ly - 1),
                        ImVec2(lx + tw + 6, ly + ns.y + 1),
                        IM_COL32(0, 0, 0, 150), 3.0f);
      dl->AddText(ImVec2(lx, ly),
                  sel ? IM_COL32(255, 236, 190, 255) : IM_COL32(235, 235, 235, 255),
                  nm);
      dl->AddText(ImVec2(lx + ns.x, ly), IM_COL32(200, 205, 220, 220), lvl);
      if (v.del_rev_date > 0) {
        const char* del = "Suppression programmée";
        const ImVec2 ds = ImGui::CalcTextSize(del);
        const float dx = cx - ds.x * 0.5f, dyy = ly + ns.y + 3.0f;
        // Fond (rouge très sombre) pour détacher nettement le texte du décor de la
        // table. La ligne de texte réserve de l'espace d'ascension AU-DESSUS des
        // capitales -> on rentre le haut du rect de ~3px pour ne pas laisser de bande
        // vide au-dessus du texte (accents é toujours couverts).
        dl->AddRectFilled(ImVec2(dx - 6, dyy + 2.0f),
                          ImVec2(dx + ds.x + 6, dyy + ds.y + 1),
                          IM_COL32(40, 0, 0, 205), 3.0f);
        dl->AddText(ImVec2(dx, dyy), IM_COL32(255, 150, 150, 255), del);
      }

      if (hovered) {
        // Fiche récap : tout ce que la session char-select fournit (CHARACTER_INFO).
        // LocalToUtf8 a un buffer rotatif (4) -> consommer chaque conversion aussitôt
        // (un seul %s par ligne).
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(LocalToUtf8(v.name));
        ImGui::Separator();
        const char* jn = JobName(v.job, v.sex_eff);
        ImGui::Text("Classe : %s", jn ? LocalToUtf8(jn) : "—");
        ImGui::Text("Niveau : base %d / job %d", v.base_level, v.job_level);
        ImGui::Text("HP : %lld / %lld", v.hp, v.max_hp);
        ImGui::Text("SP : %lld / %lld", v.sp, v.max_sp);
        ImGui::Text("Zeny : %d", v.zeny);
        ImGui::Text("STR %d  AGI %d  VIT %d", v.str, v.agi, v.vit);
        ImGui::Text("INT %d  DEX %d  LUK %d", v.intel, v.dex, v.luk);
        if (v.map[0] != '\0') ImGui::Text("Carte : %s", LocalToUtf8(v.map));
        if (v.del_rev_date > 0)
          ImGui::TextColored(ImVec4(0.96f, 0.52f, 0.52f, 1.0f),
                             "Suppression programmée");
        ImGui::EndTooltip();
      }
      if (!seat_edit_) {
        if (clicked) selected_ = ci;
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            can_enter_slot(ci))
          EnterGame(ci);
        // Clic DROIT -> menu contextuel (coupons). Ouverture différée hors du PushID
        // (l'ID scopé au siège ne matcherait pas le popup au niveau racine).
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          selected_ = ci;
          ctx_slot_ = ci;
          ctx_open_req_ = true;
        }
      }
    }
    ImGui::PopID();
  }

  // ── Titre ────────────────────────────────────────────────────────────────────
  const char* title = "Choisis ton personnage";
  const ImVec2 tsz = ImGui::CalcTextSize(title);
  // Position ancrée (poignée déplaçable en mode édition). Le point = milieu-haut.
  const ImVec2 tp = Anchor("titre", 0.5f, 24.0f / disp.y, seat_edit_);
  const float tx = tp.x - tsz.x * 0.5f;
  dl->AddText(ImVec2(tx + 1, tp.y + 1), IM_COL32(0, 0, 0, 160), title);
  dl->AddText(ImVec2(tx, tp.y), IM_COL32(245, 236, 210, 255), title);

  // Bandeau « suppression refusée » (poll de del_rev_date resté à 0 -> refus serveur,
  // perso en guilde/groupe). Affiché ~5 s sous le titre. La msgbox native équivalente
  // est cachée derrière notre UI plein écran, d'où ce relais ImGui.
  if (del_reject_until_ > GetTickCount()) {
    const char* msg = DeleteRejectMsg(del_reject_reason_);  // guilde/groupe/db/échoppe…
    const ImVec2 ms = ImGui::CalcTextSize(msg);
    const float by = tp.y + tsz.y + 10.0f;
    const float bx = (disp.x - ms.x) * 0.5f;
    dl->AddRectFilled(ImVec2(bx - 12, by - 4), ImVec2(bx + ms.x + 12, by + ms.y + 4),
                      IM_COL32(70, 0, 0, 235), 4.0f);
    dl->AddRect(ImVec2(bx - 12, by - 4), ImVec2(bx + ms.x + 12, by + ms.y + 4),
                IM_COL32(210, 70, 70, 235), 4.0f, 0, 1.5f);
    dl->AddText(ImVec2(bx + 1, by + 1), IM_COL32(0, 0, 0, 180), msg);
    dl->AddText(ImVec2(bx, by), IM_COL32(255, 185, 185, 255), msg);
  }

  // ── Barre d'action (bas, centrée) ────────────────────────────────────────────
  // RoButton : label en ImGuiCol_Text ; l'art RO est clair -> texte sombre requis.
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(28, 32, 44, 255));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(118, 124, 138, 255));

  const bool sel_occupied =
      selected_ >= 0 && selected_ < cap && views[selected_].occupied;
  const bool pending =
      sel_occupied && views[selected_].del_rev_date > 0;

  // Largeur estimée de la barre pour la centrer.
  float bar_w = 180.0f + 8.0f + 190.0f;
  if (sel_occupied) bar_w += 8.0f + 200.0f;  // bouton suppression
  // Position ancrée (poignée déplaçable). Le point = milieu-haut de la barre.
  const ImVec2 bp = Anchor("boutons", 0.5f, (disp.y - 52.0f) / disp.y, seat_edit_);
  ImGui::SetCursorScreenPos(ImVec2(bp.x - bar_w * 0.5f, bp.y));

  const bool can_enter = can_enter_slot(selected_);
  if (!can_enter) ImGui::BeginDisabled();
  if (ro::RoButton("Entrer en jeu", 180.0f, 0.0f)) EnterGame(selected_);
  if (!can_enter) ImGui::EndDisabled();

  ImGui::SameLine();
  if (ro::RoButton("Mode Classique", 190.0f, 0.0f)) {
    native_fallback_ = true;
    LogDiag("[CharSelect] repli char-select natif (session)");
  }

  // Suppression : réservation (pure ImGui) / annulation / suppression définitive.
  bool tip_sched = false, tip_cancel = false, tip_del = false;
  int del_remaining = 0;  // secondes avant que « Supprimer » soit possible (0 = OK)
  if (sel_occupied) {
    ImGui::SameLine();
    if (!pending) {
      if (ro::RoButton("Programmer suppression", 200.0f, 0.0f)) {
        DriveNativeCtrl(0x197, selected_);  // CZ 0x0827 (réserve la suppression)
        del_reject_until_ = 0;  // efface un éventuel bandeau de refus précédent
        // Le résultat (succès -> del_rev_date ; échec -> bandeau) est capté par le
        // détour Net_OnDeleteCharReserveAck (rien à surveiller ici).
      }
      tip_sched = ImGui::IsItemHovered();
    } else {
      // Annuler la suppression programmée — PAQUET DIRECT. Le pilotage natif 0x198 ne
      // déclenchait rien de fiable sous notre UI. CH_DELETE_CHAR3_CANCEL 0x082b :
      // [op u16][CID u32] = 6 o. Le serveur répond 0x082c et le recv natif efface
      // del_rev_date -> le perso quitte l'état « en suppression ».
      if (ro::RoButton("Annuler suppression", 200.0f, 0.0f)) {
        // Poser kSelectedSlot avant l'envoi (cf. suppression) : la réponse
        // d'annulation met à jour le SLOT sélectionné côté client.
        *reinterpret_cast<uint8_t*>(kSelectedSlot) =
            static_cast<uint8_t>(selected_);
        uint8_t pkt[6] = {0};
        pkt[0] = 0x2b;
        pkt[1] = 0x08;  // opcode 0x082b (little-endian)
        std::memcpy(pkt + 2, &views[selected_].gid, 4);  // CID
        Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
        del_reject_until_ = 0;
        LogDiag("[CharSelect] CH_DELETE_CHAR3_CANCEL 0x082b (gid={}, slot={})",
                views[selected_].gid, selected_);
      }
      tip_cancel = ImGui::IsItemHovered();
      ImGui::SameLine();
      // Suppression DÉFINITIVE par EMAIL (popup de confirmation plus bas). moonlight :
      // char_del_option=1 (= EMAIL dans rAthena) -> le serveur exige l'email EXACT du
      // compte via CH_DELETE_CHAR 0x01fb ; char_del_delay=0 -> pas de délai serveur.
      // On NE pilote PAS le natif OnMsg 0xd3 (deux modaux BLOQUANTS qui figent sous
      // notre capture d'input). Le bouton ouvre un popup ImGui (pas de natif).
      const int32_t now = static_cast<int32_t>(time(nullptr));
      const int32_t expiry = views[selected_].del_rev_date;
      const bool del_elapsed = expiry <= now;
      del_remaining = del_elapsed ? 0 : (expiry - now);
      if (!del_elapsed) ImGui::BeginDisabled();
      if (ro::RoButton("Supprimer", 120.0f, 0.0f)) {
        del_popup_gid_ = views[selected_].gid;  // fige la cible (indépendant de la liste)
        del_popup_slot_ = selected_;             // pour poser kSelectedSlot à l'envoi
        std::strncpy(del_popup_name_, views[selected_].name,
                     sizeof(del_popup_name_) - 1);
        del_popup_name_[sizeof(del_popup_name_) - 1] = '\0';
        ImGui::OpenPopup("Suppression définitive");
      }
      if (!del_elapsed) ImGui::EndDisabled();
      tip_del = ImGui::IsItemHovered();
    }
  }
  ImGui::PopStyleColor(2);
  // Tooltips APRÈS le pop du texte sombre (sinon texte sombre sur tooltip sombre).
  if (tip_sched)
    ImGui::SetTooltip(
        "Planifie la suppression de ce personnage.\n"
        "- Il n'est PAS supprimé tout de suite : un délai serveur s'écoule d'abord\n"
        "  (le perso reste et se marque « Suppression programmée »).\n"
        "- Pendant ce délai tu peux ANNULER (bouton « Annuler suppression »).\n"
        "- Le délai écoulé, « Supprimer » finalise — IRRÉVERSIBLE.\n"
        "- Impossible si le perso est dans une GUILDE ou un GROUPE : le serveur\n"
        "  refuse, quitte-les d'abord.");
  if (tip_cancel)
    ImGui::SetTooltip(
        "Annule la suppression planifiée : le personnage est conservé.");
  if (tip_del) {
    if (del_remaining > 0) {
      const int h = del_remaining / 3600, m = (del_remaining % 3600) / 60,
                s = del_remaining % 60;
      ImGui::SetTooltip(
          "Suppression définitive indisponible : le délai serveur n'est pas\n"
          "écoulé. Disponible dans %02d:%02d:%02d.", h, m, s);
    } else {
      ImGui::SetTooltip(
          "Supprime DÉFINITIVEMENT ce personnage. Action IRRÉVERSIBLE.\n"
          "Ouvre une confirmation : saisis l'EMAIL du compte (le serveur le\n"
          "vérifie ; c'est le même email pour tous tes personnages).");
    }
  }

  // ── Barre de SORTIE (bas-droite) : revenir au login / quitter le jeu ─────────
  // Les deux issues du « Cancel » natif, séparées et explicites. Chacune ouvre sa
  // confirmation ImGui (jamais la msgbox native : elle est supprimée sous notre UI),
  // puis envoie la commande de mode (cf. kCmdBackToLogin / kCmdQuitGame).
  {
    const float back_w = 190.0f, quit_w = 160.0f, gap = 8.0f;
    const float exit_bar_w = back_w + gap + quit_w;
    // Point ancré (poignée déplaçable en mode édition) = coin GAUCHE-haut de la barre.
    const ImVec2 xp = Anchor("sortie", (disp.x - exit_bar_w - 24.0f) / disp.x,
                             (disp.y - 52.0f) / disp.y, seat_edit_);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(28, 32, 44, 255));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(118, 124, 138, 255));
    ImGui::SetCursorScreenPos(xp);
    const bool back_clicked = ro::RoButton("Revenir au login", back_w, 0.0f);
    const bool tip_back = ImGui::IsItemHovered();
    ImGui::SameLine(0.0f, gap);
    const bool quit_clicked = ro::RoButton("Quitter le jeu", quit_w, 0.0f);
    const bool tip_quit = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    // Titres de popup distincts des labels de bouton (pas d'ID partagé dans la même
    // fenêtre) et plus explicites dans la barre de titre RO.
    if (back_clicked) ImGui::OpenPopup("Retour à l'écran de connexion");
    if (quit_clicked) ImGui::OpenPopup("Fermer le jeu");
    // Tooltips APRÈS le pop du texte sombre (sinon sombre sur sombre).
    if (tip_back)
      ImGui::SetTooltip(
          "Se déconnecte du serveur et revient à l'écran de connexion,\n"
          "sans fermer le jeu.");
    if (tip_quit) ImGui::SetTooltip("Ferme le jeu.");
  }

  if (ro::BeginRoPopupModal("Retour à l'écran de connexion")) {
    ImGui::TextUnformatted(
        "Revenir à l'écran de connexion ?\n"
        "Tu seras déconnecté du serveur ; aucun personnage n'est affecté.");
    ImGui::Spacing();
    if (ro::RoButton("Revenir au login", 170.0f, 0.0f)) {
      DriveModeCmd(kCmdBackToLogin);
      // ⚠ Le client reste en CLoginMode (seul l'ÉTAT change, 9/6 -> 3) : aucun
      // OnModeSwitch n'est émis. Sans ce réarmement explicite, MoonlightAuth
      // resterait en kDriveLogin (session authentifiée, drive « terminé ») et ne
      // redessinerait PAS son formulaire -> le joueur retombait sur l'écran de
      // login NATIF. On le ramène donc à kWebLogin nous-mêmes.
      // (Effet de bord voulu : DroveMoonlightLogin() repasse à false, donc notre
      // gate nous retire dès la frame suivante — pas besoin de fondu ici.)
      // service_select_pending=false : l'état 3 recrée directement UILoginWnd, il n'y
      // a pas d'écran de choix de connexion à repasser.
      if (auth_)
        auth_->RearmWebLogin("retour au login depuis le char-select",
                             /*service_select_pending=*/false);
      left_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  if (ro::BeginRoPopupModal("Fermer le jeu")) {
    ImGui::TextUnformatted("Fermer le jeu ?");
    ImGui::Spacing();
    if (ro::RoButton("Quitter", 130.0f, 0.0f)) {
      DriveModeCmd(kCmdQuitGame);
      quitting_ = true;
      quit_tick_ = GetTickCount();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  // ── Popup de confirmation de suppression définitive (EMAIL) ──────────────────
  // Boîte de dialogue MODALE habillée RO (ro::BeginRoPopupModal, cf. ro_imgui) : barre
  // de titre 3-slice + corps clair, ImGui bloque/assombrit le banquet derrière. On
  // envoie CH_DELETE_CHAR 0x01fb : [op u16][CID u32][key char[50]] = 56 o. Le serveur
  // (chclif_delchar_check, CHAR_DEL_EMAIL) exige l'email EXACT du compte ; sur succès
  // il re-pousse la liste (le perso disparaît), sur mauvais email il refuse SANS figer.
  // JAMAIS le prompt natif (deux modaux bloquants qui figent sous notre capture).
  if (ro::BeginRoPopupModal("Suppression définitive")) {
    ImGui::Text("Supprimer DÉFINITIVEMENT « %s » ?", del_popup_name_);
    ImGui::TextUnformatted("Action IRRÉVERSIBLE. Saisis l'email du compte pour confirmer :");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool submit = ImGui::InputTextWithHint(
        "##del_email", "email du compte", del_email_, sizeof(del_email_),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();
    const bool has_email = del_email_[0] != '\0';
    if (!has_email) ImGui::BeginDisabled();
    if (ro::RoButton("Supprimer", 150.0f, 0.0f) || (submit && has_email)) {
      // ⚠ Poser g_CharSelect_SelectedSlot AVANT l'envoi : le handler natif de la
      // réponse de suppression retire le perso au SLOT sélectionné (la réponse
      // HC_ACCEPT_DELETECHAR ne porte pas le char_id). Sans ça, le client retirait le
      // MAUVAIS doll de l'affichage (le serveur, lui, supprimait le bon via le CID).
      if (del_popup_slot_ >= 0)
        *reinterpret_cast<uint8_t*>(kSelectedSlot) =
            static_cast<uint8_t>(del_popup_slot_);
      uint8_t pkt[56] = {0};
      pkt[0] = 0xfb;
      pkt[1] = 0x01;  // opcode 0x01fb (little-endian)
      std::memcpy(pkt + 2, &del_popup_gid_, 4);                       // CID
      std::strncpy(reinterpret_cast<char*>(pkt + 6), del_email_, 50);  // key[50]
      Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
      del_reject_until_ = 0;
      LogDiag("[CharSelect] CH_DELETE_CHAR 0x01fb (gid={}, slot={}, email_len={})",
              del_popup_gid_, del_popup_slot_,
              static_cast<int>(std::strlen(del_email_)));
      ImGui::CloseCurrentPopup();
    }
    if (!has_email) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  // ── Popup de CRÉATION de personnage (ImGui + aperçu doll live) ────────────────
  // Envoie CH_MAKE_CHAR 0xa39 (36 o) nous-mêmes (départ Novice job=0 ; le serveur
  // force les stats à 1 pour ce PACKETVER). L'aperçu à gauche reflète cheveux/couleur/
  // sexe en direct. Sur succès le serveur pousse le nouveau perso -> il apparaît à sa
  // place ; sur refus (nom pris…) rien n'apparaît, SANS figer (envoi direct).
  // Ouverture différée : le clic « siège libre » (dans PushID) a levé le drapeau ; on
  // ouvre ICI, au niveau racine, pour que l'ID matche BeginRoPopupModal.
  if (create_open_req_) {
    ImGui::OpenPopup("Créer un personnage");
    create_open_req_ = false;
  }
  if (ro::BeginRoPopupModal("Créer un personnage")) {
    // Aperçu doll (gauche). Le doll garde sa taille nominale (ph) mais est CENTRÉ
    // verticalement dans une colonne aussi haute que le formulaire de droite (mesuré
    // frame N-1) -> plus de gros vide sous l'aperçu. MOLETTE au survol = rotation.
    const float pw = 116.0f, ph = 168.0f;
    const float box_h = (create_form_h_ > ph) ? create_form_h_ : ph;
    const ImVec2 dp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        dp, ImVec2(dp.x + pw, dp.y + box_h), IM_COL32(0, 0, 0, 45), 4.0f);
    DrawCreateDoll(dp.x, dp.y + (box_h - ph) * 0.5f, pw, ph);
    ImGui::InvisibleButton("##preview", ImVec2(pw, box_h));  // capte survol/molette
    if (ImGui::IsItemHovered()) {
      if (io.MouseWheel != 0.0f)
        create_dir_ = (create_dir_ + static_cast<int>(io.MouseWheel) + 8) & 7;
      ImGui::SetTooltip("Molette pour faire tourner le personnage");
    }
    ImGui::SameLine();

    // Formulaire (droite) : nom, sexe, GRILLE d'icônes de coiffure, couleur.
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Nom");
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##cname", "nom du personnage", create_name_,
                             sizeof(create_name_));
    ImGui::TextUnformatted("Sexe");
    if (ro::RadioImage("Femelle", create_sex_ == 0)) create_sex_ = 0;
    ImGui::SameLine(0.0f, 18.0f);
    if (ro::RadioImage("Mâle", create_sex_ == 1)) create_sex_ = 1;

    // Grille de coiffures : icônes = frame du .spr natif (DrawHairIcon). Scrollable ;
    // seules les cases VISIBLES chargent (IsRectVisible) -> pas de saturation atlas.
    ImGui::TextUnformatted("Coiffure");
    // Grille dimensionnée pour contenir TOUTES les coiffures sans scroll : on calcule
    // la hauteur exacte (lignes * (case + espacement)) -> pas de barre de défilement,
    // les 80 styles sont visibles d'un coup (le user peut atteindre les coupes hautes).
    const float cell = 34.0f;
    const int cols = kHairGridCols;  // 8
    const int rows = (kMaxHairStyle + cols - 1) / cols;
    const float pad = ImGui::GetStyle().ItemSpacing.y;
    const float grid_w = cols * cell + (cols - 1) * ImGui::GetStyle().ItemSpacing.x +
                         2.0f * ImGui::GetStyle().WindowPadding.x;
    const float grid_h = rows * cell + (rows - 1) * pad +
                         2.0f * ImGui::GetStyle().WindowPadding.y;
    ImGui::BeginChild("##hairgrid", ImVec2(grid_w, grid_h), true);
    {
      ImDrawList* gdl = ImGui::GetWindowDrawList();
      for (int hs = 1; hs <= kMaxHairStyle; ++hs) {
        if ((hs - 1) % cols != 0) ImGui::SameLine();
        ImGui::PushID(hs);
        const ImVec2 cpos = ImGui::GetCursorScreenPos();
        const bool vis = ImGui::IsRectVisible(ImVec2(cell, cell));
        const bool sel = (create_hair_ == hs);
        gdl->AddRectFilled(cpos, ImVec2(cpos.x + cell, cpos.y + cell),
                           sel ? IM_COL32(255, 220, 140, 55) : IM_COL32(0, 0, 0, 40),
                           3.0f);
        if (vis) DrawHairIcon(hs, cpos.x, cpos.y, cell);
        // DEBUG : numéro de la coiffure en bas-gauche de la case (ombre pour lisibilité
        // sur sprite clair) -> permet de repérer d'un coup d'œil quel id est wrappé par
        // le %23 tant que l'exe n'est pas patché (cf. mémoire charselect_imgui).
        {
          char num[8];
          std::snprintf(num, sizeof(num), "%d", hs);
          const ImVec2 ts = ImGui::CalcTextSize(num);
          const ImVec2 tp(cpos.x + 2.0f, cpos.y + cell - ts.y - 1.0f);
          gdl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, 200), num);
          gdl->AddText(tp, IM_COL32(255, 255, 255, 235), num);
        }
        ImGui::InvisibleButton("c", ImVec2(cell, cell));
        if (ImGui::IsItemClicked()) create_hair_ = hs;
        if (sel)
          gdl->AddRect(cpos, ImVec2(cpos.x + cell, cpos.y + cell),
                       IM_COL32(255, 210, 110, 255), 3.0f, 0, 2.0f);
        else if (ImGui::IsItemHovered())
          gdl->AddRect(cpos, ImVec2(cpos.x + cell, cpos.y + cell),
                       IM_COL32(200, 210, 235, 160), 3.0f, 0, 1.5f);
        ImGui::PopID();
      }
    }
    ImGui::EndChild();

    // Navigation clavier dans la grille : flèches = déplacer la sélection (±1 horizontal,
    // ±cols vertical), bornée à [1, kMaxHairStyle]. Neutralisée pendant la saisie du nom
    // (sinon les flèches déplaceraient le curseur de l'InputText). Repeat activé.
    if (!io.WantTextInput) {
      int h = create_hair_;
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ++h;
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) --h;
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) h += cols;
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) h -= cols;
      if (h < 1) h = 1;
      if (h > kMaxHairStyle) h = kMaxHairStyle;
      create_hair_ = h;
    }

    // ── Couleur : grille de VIGNETTES « ma coupe en couleur N » ─────────────────
    // Chaque case = la coiffure SÉLECTIONNÉE, recolorée par la palette head_<N>.pal
    // (GRF, cf. hairicon::ColorPalette) — WYSIWYG fidèle, pipeline d'icônes sûr (même
    // chemin que la grille de coiffures, zéro std::string). Seules les cases VISIBLES
    // chargent/recolorent (IsRectVisible) -> pas de saturation atlas. Clic = sélection.
    ImGui::Text("Couleur (%d)", create_hair_color_);
    const float sw = 22.0f;               // taille vignette (réduite)
    const int sw_cols = 20;               // 20 colonnes (fenêtre élargie d'autant)
    const float sw_pad = ImGui::GetStyle().ItemSpacing.x;
    // Largeur : 20 colonnes + marges + LARGEUR DE LA SCROLLBAR verticale (13 lignes
    // pour 8 visibles -> scrollbar présente). Sans ça, le contenu déborde et la 1re
    // colonne se retrouve rognée. Marge d'un pad en plus pour la sécurité.
    const float sw_w = sw_cols * sw + sw_cols * sw_pad +
                       2.0f * ImGui::GetStyle().WindowPadding.x +
                       ImGui::GetStyle().ScrollbarSize;
    // ~8 lignes visibles (léger scroll au-delà).
    const float sw_h = 8 * (sw + ImGui::GetStyle().ItemSpacing.y) +
                       2.0f * ImGui::GetStyle().WindowPadding.y;
    ImGui::BeginChild("##haircols", ImVec2(sw_w, sw_h), true);
    {
      ImDrawList* cdl = ImGui::GetWindowDrawList();
      for (int c = kMinHairColor; c <= kMaxHairColor; ++c) {
        if ((c - kMinHairColor) % sw_cols != 0) ImGui::SameLine();
        ImGui::PushID(1000 + c);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool vis = ImGui::IsRectVisible(ImVec2(sw, sw));
        cdl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sw), IM_COL32(30, 32, 42, 200), 3.0f);
        if (vis) DrawHairIcon(create_hair_, p.x, p.y, sw, /*color_override=*/c);
        ImGui::InvisibleButton("s", ImVec2(sw, sw));
        if (ImGui::IsItemClicked()) create_hair_color_ = c;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Couleur %d", c);
        if (create_hair_color_ == c)
          cdl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + sw + 1, p.y + sw + 1),
                       IM_COL32(255, 210, 110, 255), 3.0f, 0, 2.0f);
        else if (ImGui::IsItemHovered())
          cdl->AddRect(p, ImVec2(p.x + sw, p.y + sw),
                       IM_COL32(210, 220, 240, 180), 3.0f, 0, 1.5f);
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
    ImGui::EndGroup();
    create_form_h_ = ImGui::GetItemRectSize().y;  // pour centrer l'aperçu (frame N+1)

    // Résultat de la création : le serveur pousse le perso au slot -> SUCCÈS (ferme) ;
    // sinon rien après un court délai -> ÉCHEC (nom pris/invalide). On déduit faute de
    // voir HC_REFUSE_MAKECHAR 0x006e (paquet char-server, hors de notre recv).
    CharView cv;
    const bool slot_filled =
        create_slot_ >= 0 && ReadSlot(create_slot_, &cv) && cv.occupied;
    if (slot_filled) {
      create_pending_ = false;
      ImGui::CloseCurrentPopup();
    } else if (create_pending_ && g_modal_suppressed_seq != create_modal_base_) {
      // Une modale native a été supprimée pendant l'attente = refus serveur (nom pris)
      // -> échec IMMÉDIAT (pas d'attente du timeout).
      create_pending_ = false;
      create_failed_ = true;
    } else if (create_pending_ && GetTickCount() - create_sent_tick_ > 2500) {
      create_pending_ = false;
      create_failed_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (create_pending_)
      ImGui::TextColored(ImVec4(0.85f, 0.82f, 0.45f, 1.0f), "Création en cours…");
    else if (create_failed_) {
      const char* why = g_suppressed_modal_msg[0]
                            ? LocalToUtf8(g_suppressed_modal_msg)
                            : "Échec : nom déjà pris, trop court ou caractères interdits.";
      ImGui::TextColored(ImVec4(0.96f, 0.52f, 0.52f, 1.0f), "%s", why);
    }

    const bool can_create =
        create_name_[0] != '\0' && create_slot_ >= 0 && !create_pending_;
    if (!can_create) ImGui::BeginDisabled();
    if (ro::RoButton("Créer", 130.0f, 0.0f)) {
      // Cohérence avec la suppression : on pose kSelectedSlot. CH_MAKE_CHAR 0xa39 :
      // [op u16][name char[24]][slot u8][hair_color u16][hair u16][job u32][sex u8].
      // Nom converti UTF-8 (ImGui) -> code-page client (accents FR corrects).
      *reinterpret_cast<uint8_t*>(kSelectedSlot) =
          static_cast<uint8_t>(create_slot_);
      uint8_t pkt[36] = {0};
      pkt[0] = 0x39;
      pkt[1] = 0x0a;  // opcode 0x0a39
      std::strncpy(reinterpret_cast<char*>(pkt + 2), Utf8ToLocal(create_name_), 24);
      pkt[26] = static_cast<uint8_t>(create_slot_);
      const uint16_t hc = static_cast<uint16_t>(create_hair_color_);
      const uint16_t hs = static_cast<uint16_t>(create_hair_);
      const uint32_t job = 0;  // Novice
      std::memcpy(pkt + 27, &hc, 2);
      std::memcpy(pkt + 29, &hs, 2);
      std::memcpy(pkt + 31, &job, 4);
      pkt[35] = static_cast<uint8_t>(create_sex_);
      Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
      LogDiag("[CharSelect] CH_MAKE_CHAR 0x0a39 (slot={}, hair={}, color={}, sex={})",
              create_slot_, create_hair_, create_hair_color_, create_sex_);
      create_pending_ = true;
      create_sent_tick_ = GetTickCount();
      create_modal_base_ = g_modal_suppressed_seq;  // baseline pour l'échec instantané
      g_suppressed_modal_msg[0] = '\0';  // frais : capture la raison de CETTE création
      create_failed_ = false;
    }
    if (!can_create) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) {
      create_pending_ = false;
      ImGui::CloseCurrentPopup();
    }
    ro::EndRoPopupModal();
  }

  // ── Menu contextuel coupons (clic droit) : Renommer / Changer de slot ─────────
  if (ctx_open_req_) {
    ImGui::OpenPopup("##charctx");
    ctx_open_req_ = false;
  }
  if (ImGui::BeginPopup("##charctx")) {
    CharView cv;
    if (ctx_slot_ >= 0 && ReadSlot(ctx_slot_, &cv) && cv.occupied) {
      ImGui::TextDisabled("%s", LocalToUtf8(cv.name));
      ImGui::Separator();
      // Renommer : activé seulement si un coupon rename est actif sur ce perso.
      // Renommer : activé seulement si un coupon rename est actif (label indique le
      // nb de coupons restants ; grisé sinon).
      char ri[32];
      std::snprintf(ri, sizeof(ri), "Renommer (%d)", cv.rename_avail);
      if (!(cv.rename_avail > 0)) ImGui::BeginDisabled();
      if (ImGui::MenuItem(ri)) {
        rename_slot_ = ctx_slot_;
        rename_gid_ = cv.gid;
        std::strncpy(rename_old_, cv.name, sizeof(rename_old_) - 1);
        rename_old_[sizeof(rename_old_) - 1] = '\0';
        rename_buf_[0] = '\0';
        rename_pending_ = false;
        rename_failed_ = false;
        rename_open_req_ = true;
      }
      if (!(cv.rename_avail > 0)) ImGui::EndDisabled();
      // Changer de slot : activé si un coupon change-slot est actif.
      char mi[32];
      std::snprintf(mi, sizeof(mi), "Changer de slot (%d)", cv.moves_avail);
      if (!(cv.moves_avail > 0)) ImGui::BeginDisabled();
      if (ImGui::MenuItem(mi)) {
        move_from_ = ctx_slot_;
        move_gid_ = cv.gid;
        move_to_ = -1;
        move_open_req_ = true;
      }
      if (!(cv.moves_avail > 0)) ImGui::EndDisabled();
    }
    ImGui::EndPopup();
  }

  // ── Popup Renommer (CH_REQ_CHANGE_CHARNAME 0x08fc : [op][CID.4][name.24] = 30 o) ──
  if (rename_open_req_) {
    ImGui::OpenPopup("Renommer le personnage");
    rename_open_req_ = false;
  }
  if (ro::BeginRoPopupModal("Renommer le personnage")) {
    ImGui::Text("Nom actuel : %s", LocalToUtf8(rename_old_));
    ImGui::TextUnformatted("Nouveau nom");
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##rname", "nouveau nom", rename_buf_,
                             sizeof(rename_buf_));
    // Succès = le slot s'est renommé (le serveur re-pousse la liste). Échec = modale
    // native supprimée (nom pris) -> instantané, sinon timeout.
    CharView cv;
    const bool renamed = rename_slot_ >= 0 && ReadSlot(rename_slot_, &cv) &&
                         cv.occupied && std::strcmp(cv.name, rename_old_) != 0;
    if (rename_pending_ && renamed) {
      rename_pending_ = false;
      ImGui::CloseCurrentPopup();
    } else if (rename_pending_ &&
               g_modal_suppressed_seq != rename_modal_base_) {
      rename_pending_ = false;
      rename_failed_ = true;
    } else if (rename_pending_ && GetTickCount() - rename_sent_tick_ > 2500) {
      rename_pending_ = false;
      rename_failed_ = true;
    }
    ImGui::Spacing();
    if (rename_pending_)
      ImGui::TextColored(ImVec4(0.85f, 0.82f, 0.45f, 1.0f), "Renommage en cours…");
    else if (rename_failed_) {
      // Message EXACT du serveur (capturé depuis la modale native supprimée) s'il y en a
      // un ; sinon générique (cas timeout : le serveur n'a pas répondu = perso introuvable
      // ou refus silencieux). Le message natif est en code-page client -> LocalToUtf8.
      const char* why = g_suppressed_modal_msg[0]
                            ? LocalToUtf8(g_suppressed_modal_msg)
                            : "Échec : nom pris/invalide, coupon absent, ou perso en "
                              "guilde/groupe (rename interdit).";
      ImGui::TextColored(ImVec4(0.96f, 0.52f, 0.52f, 1.0f), "%s", why);
    }
    ImGui::Spacing();
    const bool can_rename = rename_buf_[0] != '\0' && !rename_pending_;
    if (!can_rename) ImGui::BeginDisabled();
    if (ro::RoButton("Renommer", 130.0f, 0.0f)) {
      // ⚠ NE PAS envoyer CH_REQ_IS_VALID_CHARNAME 0x028d : le client NATIF réagit à la
      // réponse 0x028e en peuplant sa fenêtre de rename (classe 0x80, msg 34) avec le
      // perso « sélectionné » de SON flux — non initialisé ici -> CHARACTER_INFO NULL ->
      // SetText(null+0x6c) -> crash (sub_8FA8D0). On envoie donc UNIQUEMENT le commit :
      // CH_REQ_CHANGE_CHARNAME 0x08fc [op][CID.4][name.24] = 30 o. En PACKETVER≥20111101
      // le serveur (chclif_parse_ackrename) prend le nom DANS le paquet et pose sd.new_name
      // lui-même -> pas besoin de l'étape 0x028d. Succès -> re-push de la liste (détecté).
      // 🔴 Poser g_CharSelect_SelectedSlot sur le perso renommé AVANT l'envoi : à la
      // complétion (case 352 de UINewSelectCharWnd_OnMsg), le natif lit ce slot via le
      // dispatcher cmd 8 ; s'il pointe un slot VIDE -> NULL -> `mov ecx,[0]` (0x79df43)
      // = access-violation (faux « freeze » sous x32dbg). Même règle que delete/create.
      if (rename_slot_ >= 0)
        *reinterpret_cast<uint8_t*>(kSelectedSlot) =
            static_cast<uint8_t>(rename_slot_);
      uint8_t pkt[30] = {0};
      pkt[0] = 0xfc;
      pkt[1] = 0x08;  // opcode 0x08fc
      std::memcpy(pkt + 2, &rename_gid_, 4);
      std::strncpy(reinterpret_cast<char*>(pkt + 6), Utf8ToLocal(rename_buf_), 24);
      Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
      LogDiag("[CharSelect] CH_REQ_CHANGE_CHARNAME 0x08fc (slot={}, cid={})",
              rename_slot_, rename_gid_);
      rename_pending_ = true;
      rename_sent_tick_ = GetTickCount();
      rename_modal_base_ = g_modal_suppressed_seq;
      g_suppressed_modal_msg[0] = '\0';  // frais : capture la raison de CE rename
      rename_failed_ = false;
    }
    if (!can_rename) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) {
      rename_pending_ = false;
      ImGui::CloseCurrentPopup();
    }
    ro::EndRoPopupModal();
  }

  // ── Popup Changer de slot (CH_REQ_CHANGE_CHARACTER_SLOT 0x08d4) ───────────────
  // [op][slot_before.2][slot_after.2][remaining.2] = 8 o. Le serveur bouge le perso
  // de `from` vers `to` (siège LIBRE). On liste les slots libres comme boutons.
  if (move_open_req_) {
    ImGui::OpenPopup("Changer de slot");
    move_open_req_ = false;
  }
  if (ro::BeginRoPopupModal("Changer de slot")) {
    ImGui::TextUnformatted("Choisis un emplacement LIBRE :");
    ImGui::Spacing();
    const int cap = SlotCapacity();
    int shown = 0;
    for (int s = 0; s < cap; ++s) {
      CharView tmp;
      if (ReadSlot(s, &tmp)) continue;  // occupé -> pas une cible
      if (shown++ % 5 != 0) ImGui::SameLine();
      char lbl[16];
      std::snprintf(lbl, sizeof(lbl), "%d", s + 1);
      if (ro::RoButton(lbl, 44.0f, 0.0f)) {
        if (move_from_ >= 0)  // même garde que rename : slot valide pour la complétion
          *reinterpret_cast<uint8_t*>(kSelectedSlot) =
              static_cast<uint8_t>(move_from_);
        uint8_t pkt[8] = {0};
        pkt[0] = 0xd4;
        pkt[1] = 0x08;  // opcode 0x08d4
        const uint16_t from = static_cast<uint16_t>(move_from_);
        const uint16_t to = static_cast<uint16_t>(s);
        std::memcpy(pkt + 2, &from, 2);
        std::memcpy(pkt + 4, &to, 2);
        Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
        LogDiag("[CharSelect] CH_REQ_CHANGE_CHARACTER_SLOT 0x08d4 (from={}, to={})",
                move_from_, s);
        ImGui::CloseCurrentPopup();
      }
    }
    if (shown == 0) ImGui::TextDisabled("Aucun emplacement libre.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  // ── Pagination des bancs (si MAX_CHARS > 25 slots) ───────────────────────────
  // Table d'honneur fixe (persos 0-4) + bancs paginés. Point ancré (déplaçable).
  if (n_pages > 1) {
    const ImVec2 pp = Anchor("pages", 0.5f, (disp.y - 96.0f) / disp.y, seat_edit_);
    char pl[40];
    std::snprintf(pl, sizeof(pl), "Page %d / %d", page_ + 1, n_pages);
    const ImVec2 pls = ImGui::CalcTextSize(pl);
    const float bw = 46.0f, gap = 12.0f;
    const float x0 = pp.x - (bw + gap + pls.x + gap + bw) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(28, 32, 44, 255));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(118, 124, 138, 255));
    // ⚠ Capturer l'état AVANT le clic : Begin/EndDisabled doivent être appariés
    // (le clic modifie page_, ce qui changerait la condition du EndDisabled).
    const bool at_first = (page_ <= 0);
    ImGui::SetCursorScreenPos(ImVec2(x0, pp.y));
    if (at_first) ImGui::BeginDisabled();
    if (ro::RoButton("<", bw, 0.0f)) --page_;
    if (at_first) ImGui::EndDisabled();
    const bool at_last = (page_ >= n_pages - 1);
    ImGui::SetCursorScreenPos(ImVec2(x0 + bw + gap + pls.x + gap, pp.y));
    if (at_last) ImGui::BeginDisabled();
    if (ro::RoButton(">", bw, 0.0f)) ++page_;
    if (at_last) ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    // Indicateur central (clair, lisible sur le fond sombre).
    dl->AddText(ImVec2(x0 + bw + gap, pp.y + 6.0f), IM_COL32(240, 232, 208, 255), pl);
  }

  // ── Éditeur de layout (DÉSACTIVÉ) ────────────────────────────────────────────
  // Le layout est calé : plus de déclencheur. Le CODE reste (poignées Anchor, drag
  // des sièges, molette de taille, dump), mais `seat_edit_` ne peut plus passer à
  // true -> tout ce chemin est mort. Pour le réactiver le temps d'un recalage,
  // décommenter le bloc ci-dessous (F10 = bascule).
  // ⚠ On NE peut PAS gater sur IsStaff() ici : le niveau de groupe serveur n'arrive
  // qu'EN JEU (setting id 26 sur la session map). Au char-select il vaut 0 -> le
  // panneau ne s'affichait jamais. D'où F10, seul chemin qui marchait. La condition
  // IsStaff() a donc été retirée du bloc ci-dessous (et son include avec) : la
  // remettre ne ferait que rendre le panneau inaccessible à nouveau.
  //
  // if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) seat_edit_ = !seat_edit_;
  // if (seat_edit_) {
  //   ImGui::SetCursorPos(ImVec2(disp.x - 320.0f, disp.y - 30.0f));
  //   ImGui::Checkbox("Éditer layout (F10)", &seat_edit_);
  //   if (seat_edit_) {
  //     ImGui::SameLine();
  //     // Glisser une poignée = déplacer ; molette sur un siège = taille du pantin.
  //     if (ImGui::SmallButton("Dump layout")) {
  //       LogDiag("[CharSelect] --- sièges (recoller dans g_seats) ---");
  //       for (int i = 0; i < kSeatCount; ++i)
  //         LogDiag("    {{{:.3f}f, {:.3f}f, {:.3f}f}},  // {}", g_seats[i].nx,
  //                 g_seats[i].ny, g_seats[i].scale, i + 1);
  //       LogDiag("[CharSelect] --- points (nx, ny en fractions d'écran) ---");
  //       for (int i = 0; i < g_anchor_count; ++i)
  //         LogDiag("    {}  ->  {:.4f}f, {:.4f}f", g_anchors[i].name,
  //                 g_anchors[i].nx, g_anchors[i].ny);
  //     }
  //   }
  // }

  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();  // WindowBg
}
