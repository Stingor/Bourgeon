#include "ragnarok/globals.h"
#include "ui/game_texture.h"
#include "ui/doll.h"
#include "ui/head_icon.h"
#include "features/windows/char_select.h"
#include "features/windows/char_select_layout.h"
#include "ragnarok/held_sprites.h"

#include "ragnarok/uiwnd.h"
#include "utils/game_paths.h"
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"      // Overlay_CreateTextureARGB / Overlay_DeviceEpoch
#include "imgui.h"
#include "features/systems/moonlight_auth.h"
#include "features/systems/native_login.h"  // sondes d'écran (fenêtre 0x115, liste)
#include "ui/ro_imgui.h"
#include "utils/hooking/hook_manager.h"  // détour Net_OnDeleteCharReserveAck
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"
#include "utils/i18n.h"

namespace {

// ── Accès natif (client 20250716, base 0x400000) ──────────────────────────────
// Dispatcher CMode : *(0x0121333c) -> objet ; vtbl+0x18, cmd 8 = get CHARACTER_INFO
// par slot (renvoie nullptr si slot vide). Convention __thiscall confirmée dans
// character_sheet.cc (SendConfigToggle). Carte des champs = docs/charselect_re.md.
constexpr int       kVfDispCmd = 0x18;
constexpr int       kCmdGetChar = 8;

// ── Loader de texture natif (fond banquet, BMP côté client) ───────────────────
// Même chemin que les icônes d'items (character_sheet.cc) : UITextureMgr_Get ->
// MakeKey(path) -> LoadTex ; la texture expose largeur/hauteur/pixels BGRA.
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;
// Chemin VFS du décor d'USINE : déposé dans le GRF/data du client (via le
// patcher). Fichier 24/32 bits ; dessiné étiré plein écran (toute taille convient
// — plus petit = moins de VRAM).
//
// Ce n'est plus le seul décor possible : le joueur en choisit un dans la galerie
// du mode « Personnaliser » (charsel::Backgrounds(), fichiers de data\lobby\), et
// son choix est persisté. Celui-ci reste le repli quand aucun n'est choisi.
const char kHallBmpPath[] =
    "lobby_hall.bmp";

// ── Fenêtre native de création (ouverte par le contrôle « créer » 0x1A0) ──────
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
// mgr+0x3d4, jamais remis à zéro à la destruction). C'est la SEULE sonde fiable de
// « on est bien à l'écran char-select », et la seule source du `this` qu'on pilote.
constexpr int       kCharSelWndId = 0x115;  // 277 = UINewSelectCharWnd

// La fenêtre native du char-select existe-t-elle encore ?
bool NativeCharSelectAlive() {
  __try {
    return uiwnd::FindWindow(kCharSelWndId) != nullptr;
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
// ⚠ Le cache mgr+0x3d4 (g_pCharSelectWnd, 0x0131F8BC) a été RETIRÉ de ce fichier :
// il n'est jamais remis à zéro à la destruction, et au login SUIVANT (même
// processus) il pointe encore sur l'objet du cycle précédent — mémoire libérée mais
// non réutilisée, donc la garde de vtable PASSE et on pilotait une fenêtre morte.
// C'est ce qui bloquait le joueur derrière notre fondu « Entrée en jeu… » : le
// OnMsg 0xB8 partait dans le vide, le natif restait au char-select. On demande
// donc la fenêtre au MANAGER (FindWindow 0x115), qui ne connaît que les vivantes.
constexpr uintptr_t kCharSelWndVtbl  = 0x0101D424;  // garde anti-pointeur périmé
constexpr uintptr_t kCharSelOnMsg    = 0x0079D610;  // vtbl+0x94, RET 0x18

// Entrée en jeu : délai au-delà duquel une séquence qui n'a RIEN produit est
// abandonnée (le natif purge ses fenêtres en une frame quand elle aboutit).
constexpr unsigned long kEnterTimeoutMs = 2500;
// Insensibilité à l'Entrée pendant les premières ms d'affichage de la table :
// la file de messages contient encore les Entrées de l'écran précédent.
constexpr unsigned long kEnterGraceMs = 400;

// ── Fraîcheur de la liste de personnages (cf. char_select.h, list_tick_) ──────
// Opcodes de LISTE, seulement observés : c'est leur arrivée qui prouve que les
// CHARACTER_INFO appartiennent à la session courante.
constexpr uint16_t kOpCharList     = 0x006B;  // HC_ACCEPT_ENTER (variable)
constexpr uint16_t kOpCharListPage = 0x0B72;  // HC_ACK_CHARINFO_PER_PAGE (variable)
// Repli de sûreté : au-delà de ce délai sans avoir vu de paquet de liste, on
// s'arme quand même (et on le SIGNALE une fois). Sans ce repli, un opcode qui ne
// serait pas celui de ce client rendrait la table définitivement invisible — une
// régression bien pire que le bug corrigé. Le message de log est aussi ce qui
// nous dira que le fait-générateur ne se déclenche pas.
constexpr unsigned long kListWaitMs = 3000;
// Durée maximale du voile d'attente (DrawWaitCover) depuis l'ouverture du
// char-select natif. Couvre largement l'arrivée + le décodage de la liste ; au
// delà on rend la main au natif (un compte sans personnage n'aurait sinon plus
// aucune issue, sa liste ne devenant jamais décodable).
constexpr unsigned long kCoverWaitMs = 3000;

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
// Arme et bouclier. Le char-select NATIF ne les passe pas à Actor_Init — il ne
// les affiche pas — mais la liste les porte.
//
// 🔴 u16, PAS u32, et les champs voisins le prouvent : lus sur 32 bits, ils
// renvoyaient 65537 (= 0x00010001, l'arme ET le niveau de base collés, ce
// dernier étant à +0x5C). L'ordre du paquet est celui de rAthena :
// hair, body, WEAPON, base_level, skill_point, head_bottom, SHIELD, head_top…
constexpr int kWeapon    = 0x5a;  // u16 vue d'arme
constexpr int kShield    = 0x62;  // u16 vue de bouclier
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
  // ⚠ g_cover_active SEUL ne suffit pas : il n'est remis à false qu'en tête de
  // OnRenderLoginUI, qui cesse d'être appelé dès l'entrée en jeu. Il restait donc
  // collé à true pour toute la session, et CE détour supprimait TOUTES les modales
  // natives EN JEU. Symptôme observé : le bouton « Apply » du grimoire sans effet —
  // UINewSkillListWnd::OnMsg case 271 exige que la modale de confirmation renvoie
  // 0xBB, elle rendait 185 (notre no-op), donc sub_974530 n'était jamais appelé et
  // aucun CZ_UPGRADE_SKILLLEVEL 0x0112 n'était émis.
  // Notre char-select n'existe QUE hors monde : on exige explicitement d'être hors
  // jeu pour supprimer quoi que ce soit. Ceinture et bretelles avec le reset posé
  // dans OnModeSwitch.
  if (g_cover_active && !Bourgeon::Instance().IsGameActive()) {
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
      return i18n::Tr("Suppression refusée : ce personnage est dans une GUILDE. "
             "Quitte-la d'abord, puis réessaie.");
    case 5:
      return i18n::Tr("Suppression refusée : ce personnage est dans un GROUPE. "
             "Quitte-le d'abord, puis réessaie.");
    case 6:
      return i18n::Tr("Suppression impossible : ce personnage tient un shop.");
    case 3:
      return i18n::Tr("Suppression impossible : erreur base de données ou personnage "
             "introuvable.");
    case 0:
      return i18n::Tr("La suppression de ce personnage est déjà programmée.");
    default:
      return i18n::Tr("Suppression refusée par le serveur.");
  }
}

// Texte du jeu -> UTF-8 pour ImGui. Le corps — lecture de la code-page EFFECTIVE
// du client, SEH, repli sur les octets bruts — est parti dans ro::LocalToUtf8 :
// il était recopié à l'identique dans item_desc_window, et l'adresse de la
// code-page déclarée dans les deux fichiers. Cf. ui/ro_imgui.h et
// rag::kClientCodePageAddr.
const char* LocalToUtf8(const char* s) { return ro::LocalToUtf8(s); }

// Saisie ImGui (UTF-8) -> code-page du client, pour ce qui PART au natif ou sur
// le fil (le nom à la création). Même mutualisation que ci-dessus.
const char* Utf8ToLocal(const char* s) { return ro::Utf8ToLocal(s); }

// Les icônes de coiffure sont désormais rendues par ui/head_icon.h — même
// sprite, même table de remap des 12 coupes historiques, même palette de
// couleur, mais via notre propre parseur .spr/.act. Cf. CharSelect::DrawHairIcon.

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
        rag::Session(), nullptr,
        static_cast<unsigned>(job), sex);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
  return (n && *n) ? n : nullptr;
}

// (Chemin du yaml : paths::SettingsPath(), utils/game_paths.h.)

// ── Table des PLACES (scène banquet) ──────────────────────────────────────────
// L'utilisateur a numéroté le décor (1..25) : le slot i occupe la place n°(i+1).
// Coords NORMALISÉES [0..1] sur le fond (indépendantes de la résolution : on
// dessine le fond étiré plein écran, donc place écran = (nx*disp.x, ny*disp.y)).
// `chair` = point où poser les PIEDS du perso (assis/debout sur le siège) ; `scale`
// = hauteur d'un personnage STANDARD en fraction de la hauteur d'écran
// (perspective : trônes au fond = petits, bancs du 1er plan = grands ; les
// sprites plus grands que la norme débordent, cf. RefDollSpanUnits). Valeurs extraites à l'œil de l'image
// numérotée -> AJUSTABLES en jeu via l'éditeur de sièges (staff), qui journalise la
// table prête à recoller ici. Ordre = 1 grand trône, 2 petit trône, 3-5 petites
// tables, 6-14 rangée haute, 15/16 bouts, 17-25 rangée basse.
// La table elle-même vit désormais dans features/windows/char_select_layout :
// elle est MODIFIABLE par le joueur (mode « Personnaliser ») et persistée dans
// bourgeon_charselect_layout.yaml. Ici on ne fait que la lire.
using charsel::kSeatCount;
using charsel::Seat;
// Sièges courants. Fonction et non référence statique : State() lit le fichier au
// premier appel, qui peut survenir avant ou après ce fichier selon l'ordre
// d'initialisation — on ne fige donc rien à la construction.
inline Seat* Seats() { return charsel::State().seats; }

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

// ── Points d'ancrage des blocs d'interface (titre, boutons, pages, sortie) ─────
// Toute position qui passe par Anchor() devient, en mode « Personnaliser », une
// petite poignée qu'on glisse à la souris. Le registre (idempotent par nom, en
// FRACTIONS d'écran donc stable d'une résolution à l'autre) vit dans
// char_select_layout : ce qui n'était qu'un outil d'auteur, dont la seule sortie
// était un LogDiag à recopier dans le code, est maintenant enregistré.
//
// Renvoie la position ÉCRAN courante (px) du point `name`, enregistré au défaut
// def_nx/def_ny à son premier appel.
ImVec2 Anchor(const char* name, float def_nx, float def_ny, bool edit) {
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  charsel::AnchorPt* a = charsel::AnchorRef(name, def_nx, def_ny);
  if (!a) return ImVec2(def_nx * disp.x, def_ny * disp.y);  // registre plein
  ImVec2 c(a->nx * disp.x, a->ny * disp.y);
  if (edit) {
    ImGui::PushID(name);
    ImGui::SetCursorScreenPos(ImVec2(c.x - 9.0f, c.y - 9.0f));
    ImGui::InvisibleButton("##anc", ImVec2(18.0f, 18.0f));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      a->nx += ImGui::GetIO().MouseDelta.x / disp.x;
      a->ny += ImGui::GetIO().MouseDelta.y / disp.y;
      c = ImVec2(a->nx * disp.x, a->ny * disp.y);
      charsel::MarkDirty();
    }
    // Draw-list de la fenêtre (la couverture plein écran) et non le FOREGROUND :
    // celui-ci passerait AU-DESSUS du panneau « Personnaliser », qui capte pourtant
    // les clics à cet endroit — on verrait des poignées qu'on ne peut pas saisir.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, 5.0f,
                        ImGui::IsItemActive() ? IM_COL32(255, 255, 150, 255)
                                              : IM_COL32(120, 200, 255, 230));
    dl->AddText(ImVec2(c.x + 8.0f, c.y - 6.0f), IM_COL32(150, 210, 255, 255), name);
    ImGui::PopID();
  }
  return c;
}

// Récupère (SEH, POD only) les dimensions + le pointeur des pixels BGRA de la
// texture native du décor. ⚠ AUCUN objet C++ ici : un std::vector/std::string dans
// un __try déclenche C2712 (« __try dans une fonction nécessitant un déroulement
// d'objet »). D'où la scission avec la conversion C++ ci-dessous.
const uint8_t* FetchHallBgra(const char* path, int* out_w, int* out_h) {
  __try {
    using TexMgr_t  = void*(__cdecl*)();
    using MakeKey_t = void*(__cdecl*)(const char*);
    using LoadTex_t = void*(__fastcall*)(void*, void*, void*);
    void* mgr = reinterpret_cast<TexMgr_t>(ro::texmgr::kGet)();
    if (!mgr) return nullptr;
    void* key = reinterpret_cast<MakeKey_t>(ro::texmgr::kMakeKey)(path);
    if (!key) return nullptr;
    void* t = reinterpret_cast<LoadTex_t>(ro::texmgr::kLoad)(mgr, nullptr, key);
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

// ── Décor : BMP client -> texture ImGui (cache, invalidé au reset device) ──────
// Chargé via le loader natif, converti BGRA->ARGB pour Overlay. Renvoie 0 tant que
// le .bmp est absent (le décor tombe alors sur un fond sombre uni).
//
// Le cache porte le CHEMIN chargé, et pas seulement une texture : changer de décor
// dans le mode « Personnaliser » doit se voir à la frame suivante, sans quitter
// l'écran. La texture précédente est explicitement relâchée — sans quoi parcourir
// la galerie fuirait une texture plein écran par décor essayé.
//
// ⚠ Release SEULEMENT quand le device est le même : après un changement d'epoch
// les anciens handles appartiennent à un device détruit, les toucher fait fauter
// le backend (cf. Overlay_ReleaseTexture).
//
// 🔴 Et JAMAIS dans la frame courante. Le décor est peint au DÉBUT de la frame
// (DrawHallBackdrop -> AddImage), le clic qui change de décor arrive PLUS TARD
// dans cette même frame : au moment du changement, la draw-list contient déjà un
// dessin de l'ancienne texture, que rien d'autre ne référence encore (SetTexture
// n'aura lieu qu'au rendu). Un Release immédiat la détruit donc avant qu'elle ne
// soit dessinée, et d3d9 déréférence un objet mort au rendu :
//   mov esi,[eax+0Ch] ; call esi   avec esi = 0  ->  EIP = 0 (crash observé,
//   x32dbg, retour dans d3d9.dll 0x67ea199b).
// La libération est donc DIFFÉRÉE de deux frames : la frame qui référençait la
// texture est alors rendue et présentée depuis longtemps.
struct PendingTexRelease {
  void* tex = nullptr;
  int   frame = 0;  // frame ImGui où la texture a cessé d'être utilisée
};

void* LoadHallTexture(const char* path) {
  static void* s_tex = nullptr;
  static unsigned s_epoch = 0xffffffff;
  static std::string s_path;  // chemin de la texture en cache
  static bool s_tried = false;  // ne pas re-tenter en boucle si le .bmp manque
  static PendingTexRelease s_pending[8];

  const int frame = ImGui::GetFrameCount();
  const unsigned e = Overlay_DeviceEpoch();
  if (e != s_epoch) {  // device (re)créé -> l'ancienne texture est morte
    s_tex = nullptr;   // (surtout PAS de Release : elle appartient au device parti)
    // Les différées aussi : leurs handles sont morts avec le device.
    for (PendingTexRelease& p : s_pending) p.tex = nullptr;
    s_tried = false;
    s_path.clear();
    s_epoch = e;
  }
  // Purge des textures dont la dernière frame d'usage est passée.
  for (PendingTexRelease& p : s_pending) {
    if (p.tex && frame > p.frame + 1) {
      Overlay_ReleaseTexture(p.tex);
      p.tex = nullptr;
    }
  }
  if (s_path != path) {  // décor changé à chaud
    if (s_tex) {
      bool queued = false;
      for (PendingTexRelease& p : s_pending) {
        if (!p.tex) { p.tex = s_tex; p.frame = frame; queued = true; break; }
      }
      // File pleine (le joueur a parcouru la galerie très vite) : on ABANDONNE la
      // texture. Fuir quelques Mo de VRAM jusqu'au prochain reset de device est
      // sans conséquence visible ; la relâcher trop tôt fait tomber le client.
      if (!queued)
        LogError("[CharSelect] file de libération pleine -> texture de décor "
                 "abandonnée (fuite bénigne)");
    }
    s_tex = nullptr;
    s_tried = false;
    s_path = path;
  }
  if (s_tex || s_tried) return s_tex;
  s_tried = true;
  int w = 0, h = 0;
  const uint8_t* bgra = FetchHallBgra(path, &w, &h);
  if (!bgra) return s_tex;  // .bmp absent/invalide -> fond uni
  // Conversion + upload en C++ normal : le std::vector a un destructeur, donc HORS
  // __try (sinon C2712). La recopie lisant `bgra` reste, elle, SEH-gardée.
  std::vector<uint8_t> argb(static_cast<size_t>(w) * h * 4);
  if (!CopyBgraToArgb(argb.data(), bgra, w, h)) return s_tex;
  s_tex = Overlay_CreateTextureARGB(argb.data(), w, h);
  return s_tex;
}

// Le décor choisi, ou celui d'usine si le réglage est vide.
const char* CurrentBackgroundPath() {
  const std::string& bg = charsel::State().background;
  return bg.empty() ? kHallBmpPath : bg.c_str();
}

// Le décor demandé s'est-il chargé ? Sert au mode « Personnaliser » à dire au
// joueur que son fichier n'a pas été trouvé, au lieu de lui montrer un fond uni
// sans explication.
bool BackgroundLoaded() { return LoadHallTexture(CurrentBackgroundPath()) != nullptr; }

// ── Derniers personnages joués (reprise après déco/reco) ─────────────────────
// Le natif replace toujours la sélection sur le slot 0 : après une déconnexion
// suivie d'une reconnexion rapide, le joueur devait re-désigner son personnage.
//
// On mémorise le **CID** (identifiant serveur du perso), pas le numéro de slot :
// un déplacement de slot (CH_REQ_CHANGE_CHARACTER_SLOT) ou un autre compte RO
// rendrait le slot faux, alors que le CID reste juste. La liste garde les
// derniers personnages, le plus récent en tête — un compte Moonlight portant
// PLUSIEURS comptes RO, chacun doit retrouver le sien : au char-select on
// présélectionne le premier CID de la liste qui est effectivement présent.
constexpr size_t kRecentCharsMax = 16;

std::vector<uint32_t> LoadRecentChars() {
  std::vector<uint32_t> cids;
  std::ifstream f(paths::LastCharsPath());
  if (!f) return cids;
  std::string line;
  while (std::getline(f, line) && cids.size() < kRecentCharsMax) {
    const unsigned long long cid = std::strtoull(line.c_str(), nullptr, 10);
    if (cid > 0 && cid <= 0xFFFFFFFFull)
      cids.push_back(static_cast<uint32_t>(cid));
  }
  return cids;
}

// Repousse `gid` en tête de `cids` (dédupliqué, borné) et réécrit le fichier.
void SaveRecentChars(std::vector<uint32_t>& cids, uint32_t gid) {
  if (gid == 0) return;
  cids.erase(std::remove(cids.begin(), cids.end(), gid), cids.end());
  cids.insert(cids.begin(), gid);
  if (cids.size() > kRecentCharsMax) cids.resize(kRecentCharsMax);
  std::ofstream f(paths::LastCharsPath(), std::ios::trunc);
  if (!f) return;  // disque en lecture seule : la reprise dégrade, rien de plus
  for (const uint32_t cid : cids) f << cid << "\n";
}

}  // namespace

CharSelect::CharSelect(MoonlightAuth* auth) : auth_(auth) {
  recent_chars_ = LoadRecentChars();
  // Liste de personnages : on OBSERVE (jamais on n'intercepte), le handler natif
  // doit continuer à peupler les CHARACTER_INFO. Aucun payload ne nous intéresse,
  // seulement le fait qu'un paquet de liste soit arrivé -> forward_len = 0.
  // 0x006B = HC_ACCEPT_ENTER (liste complète, variable) ;
  // 0x0B72 = HC_ACK_CHARINFO_PER_PAGE (liste paginée, variable).
  // Cf. docs/charselect_re.md.
  Bourgeon::Instance().RegisterObserveOpcode(kOpCharList, 0);
  Bourgeon::Instance().RegisterObserveOpcode(kOpCharListPage, 0);
  std::ifstream f(paths::SettingsPath());
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
    void* d = *reinterpret_cast<void**>(rag::kActiveModePtr);
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
    out->weapon        = Read<uint16_t>(c, ci::kWeapon);
    out->shield        = Read<uint16_t>(c, ci::kShield);
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
  // CID lu AVANT de piloter le natif : l'entrée en jeu VIDE les CHARACTER_INFO
  // (cf. le gel d'affichage plus bas), le slot ne serait alors plus lisible.
  CharView entering_view;
  const uint32_t gid = ReadSlot(slot, &entering_view) ? entering_view.gid : 0;
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
    // Fenêtre demandée au MANAGER, pas au cache mgr+0x3d4 : lui pendouille après
    // destruction et sa vtable matche encore (cf. kCharSelWndVtbl plus haut), donc
    // la garde le laissait passer et on pilotait une fenêtre morte.
    void* wnd = uiwnd::FindWindow(kCharSelWndId);
    if (!wnd || *reinterpret_cast<uintptr_t*>(wnd) != kCharSelWndVtbl) {
      LogError("[CharSelect] fenêtre native absente/invalide -> entrée en jeu "
               "impossible (slot {}). Utilise « Mode Classique ».", slot);
      return;
    }
    // ⚠ RET 0x18 = 6 args pile (même piège que UILoginWnd_OnMsg : un typedef à
    // 5 args corromprait ESP).
    // Positions = celles de uiwnd::OnMsg : (this, arg0=0, msg=6, p2=0xB8 la
    // sous-commande « entrer en jeu », p3..p5=0). On n'appelle PAS uiwnd::OnMsg
    // ici : lui résout le handler par la vtable, alors qu'on vise l'adresse
    // FIXE kCharSelOnMsg.
    using WndOnMsg_t = int(__thiscall*)(void*, int, int, int, int, int, int);
    reinterpret_cast<WndOnMsg_t>(kCharSelOnMsg)(wnd, 0, 6, 0xB8, 0, 0, 0);
    entering_ = true;
    enter_tick_ = GetTickCount();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogError("[CharSelect] exception pendant l'entrée en jeu (slot {})", slot);
  }
  // Mémorise le personnage joué -> il sera présélectionné à la reconnexion.
  // HORS du __try : std::ofstream a un destructeur (SEH + unwinding = C2712).
  if (entering_ && gid != 0) SaveRecentChars(recent_chars_, gid);
}

// Rapport largeur/hauteur du cadre d'un siège. C'est l'encombrement d'un sprite
// RO debout ; il ne sert plus qu'à mesurer la référence ci-dessous et à poser le
// placeholder, l'échelle ne s'y ajustant plus.
constexpr float kDollBoxAspect = 0.62f;

// Hauteur EFFECTIVE du pantin de RÉFÉRENCE dans une pose donnée, en unités .act
// (= pixels de sprite).
//
// 🔴 Elle existe pour donner à la scène UNE échelle commune. Par défaut le
// composeur ajuste chaque pantin à son cadre : il choisit l'échelle qui fait
// remplir la box du siège à SA silhouette. Deux personnages n'ont alors plus la
// même taille relative — celui dont le sprite est petit est agrandi jusqu'à
// rejoindre le grand, un chapeau volumineux rétrécit son porteur, une monture le
// réduit de moitié. Sur une scène de banquet, où les personnages se comparent du
// regard, c'est le défaut le plus visible.
//
// On mesure donc un Novice masculin nu, et toutes les échelles du siège s'en
// déduisent. L'intérêt de passer par cette mesure plutôt que par une constante
// écrite en dur : `Seat::scale` garde exactement le sens qu'il avait — « hauteur
// d'un personnage standard en fraction de l'écran » — donc les layouts déjà
// enregistrés ne bougent pas d'un pixel. Ce qui change, ce sont les autres : un
// Doram rend plus petit, un cavalier dépasse par le haut.
//
// ⚠ Mesurée dans la POSE du siège, sinon un banquet entier rapetisserait : un
// personnage assis est bien plus court que debout, et le rapporter à une
// référence debout le réduirait d'un quart.
//
// ⚠ …mais toujours à la direction 0. Prendre celle du siège ferait varier
// l'échelle quand on tourne le personnage à la molette — c'est justement le
// défaut que `fit_span` corrige ailleurs.
//
// Rend 0 tant que les sprites ne répondent pas (GRF pas encore prêt) : l'appelant
// retombe alors sur l'ajustement au cadre, et retentera à la frame suivante.
static float RefDollSpanUnits(int anim) {
  // Une pose = une hauteur ; la mesure est chère (chargement + composition) et
  // ne change jamais, d'où le cache. 13 types d'action, cf. char_select_layout.h.
  constexpr int kAnimCount = 13;
  static float cached[kAnimCount] = {0.0f};
  if (anim < 0 || anim >= kAnimCount) anim = 0;
  if (cached[anim] > 0.0f) return cached[anim];

  ro::DollLook ref;   // tout à 0 = Novice nu, sans coiffe ni cape
  ref.sex = 1;
  ro::DollPlacement pl;
  ro::DollDrawOpts o;
  o.anim          = anim;
  o.anim_seconds  = -1.0f;  // figé sur la première image
  o.measure_only  = true;   // rien n'est dessiné, seules les boîtes comptent
  o.out_placement = &pl;
  // 🔴 Cadre aux PROPORTIONS d'un siège, pas un cadre libre : c'est l'échelle
  // qu'aurait choisie l'ancien ajustement pour ce Novice qu'on relève, contrainte
  // de largeur comprise. Sans elle, les poses larges — assis surtout, où le
  // sprite est plus large que haut — mesureraient une hauteur qui n'a jamais été
  // celle du rendu, et toute la scène grossirait d'un tiers.
  //
  // `h / scale` rend donc une hauteur EFFECTIVE : celle à laquelle rapporter la
  // box du siège pour retomber sur la taille d'avant.
  constexpr float kProbeH = 1000.0f;
  if (!ro::DrawDoll(ImGui::GetWindowDrawList(), ref, 0.0f, 0.0f,
                    kProbeH * kDollBoxAspect, kProbeH, o) ||
      pl.scale <= 0.0f)
    return 0.0f;
  cached[anim] = kProbeH / pl.scale;
  return cached[anim];
}

void CharSelect::DrawDollAt(const CharView& v, float cx, float chair_y,
                           float box_h, const charsel::Seat& pose,
                           int seat_index) {
  // Paperdoll (corps + tête + coiffes + cape, palettes) : ui/doll.h le COMPOSE à
  // partir des fichiers. Il prend une apparence en paramètre, donc ne dépend ni
  // d'une session en jeu ni d'une UIWindow — indispensable ici, on est au
  // char-select. Pieds ancrés en (cx, chair_y), corps centré en X, box de hauteur
  // box_h vers le haut, en coords ÉCRAN.
  //
  // Échelle sprite par JOB : le client natif rétrécit les classes BÉBÉ
  // (Actor_GetJobSpriteScale 0x00d7fd30 -> 0.75 bébé 1re classe, 0.80/0.82 les
  // autres, 1.0 non-bébé). Le composeur ne l'applique pas — on le multiplie donc
  // à l'échelle demandée, et le bébé rend bien `js` fois plus petit, pieds
  // toujours ancrés au banc.
  using JobScaleFn = float(__stdcall*)(int);
  float js = reinterpret_cast<JobScaleFn>(0x00d7fd30)(v.job);
  if (!(js > 0.05f && js <= 1.0f)) js = 1.0f;  // garde-fou (job inconnu)
  const float dh = box_h * js;
  const float w = dh * kDollBoxAspect;
  const float x = cx - w * 0.5f;
  const float y = chair_y - dh;

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

  // Pose : elle vient du SIÈGE (mode « Personnaliser »), défauts = dir 0 (de
  // face) et anim 2 (ASSIS, les convives sont attablés). La pose assise pose le
  // perso SUR le banc — son point d'assise, pas ses pieds, arrive à chair_y —,
  // ce qui corrige le « flottement » de la pose debout.
  ro::DollLook dl;
  dl.sex           = v.sex_eff;  // 99 déjà résolu en sexe de compte
  dl.job           = v.job;
  dl.body          = v.body;  // 🔴 c'est LUI qui choisit le sprite de corps
  dl.hair          = v.hair;
  dl.hair_color    = v.hair_color;
  dl.clothes_color = v.clothes_color;
  dl.head_low      = v.head_low;
  dl.head_top      = v.head_top;
  dl.head_mid      = v.head_mid;
  dl.garment       = v.garment;
  // Arme et bouclier : le composeur veut des CHEMINS, pas des identifiants (leur
  // résolution native est trop tordue pour être recopiée, cf. ui/doll.h). En jeu
  // ils se lisent sur l'acteur ; ici il n'y en a pas, donc on demande au client
  // de les construire (rag::ResolveHeldSprites). Les tampons vivent jusqu'au
  // DrawDoll ci-dessous, qui ne conserve rien.
  //
  // 🔴 On passe `body`, pas `job` : c'est la classe qui NOMME les sprites (celle
  // que le composeur utilise déjà pour le corps), et les fichiers d'arme sont
  // rangés sous ce nom — `…\초보자\초보자_남_….spr`. Le champ `class` (+0x54) ne
  // sert pas ici ; il rend d'ailleurs des valeurs surprenantes sur cet écran.
  rag::HeldSpritePaths held;
  if (rag::ResolveHeldSprites(v.body, v.sex_eff, v.weapon, v.shield, &held)) {
    if (held.weapon_spr[0]) {
      dl.weapon.spr_base = held.weapon_spr;
      dl.weapon.act_base = held.weapon_act[0] ? held.weapon_act : nullptr;
    }
    if (held.shield_spr[0]) {
      dl.shield.spr_base = held.shield_spr;
      dl.shield.act_base = held.shield_act[0] ? held.shield_act : nullptr;
    }
  }
  // L'horloge n'anime QUE les accessoires en pose assise — un masque dont les
  // couleurs changent, une mâchoire qui mordille. Le corps et la tête restent
  // figés : leurs images sont des poses et des expressions, pas une décoration.
  ro::DollDrawOpts opts;
  opts.dir          = pose.dir;
  opts.anim         = pose.anim;
  opts.head_dir     = pose.head_dir;
  opts.anim_seconds = static_cast<float>(ImGui::GetTime());
  opts.tint         = tint;
  // Corps figé par défaut, animé sur demande (« Combat (animé) »). L'horloge
  // reste posée dans les deux cas : elle anime les ACCESSOIRES, qui doivent
  // continuer à vivre sur un personnage immobile, comme en jeu.
  opts.freeze_body  = !pose.animate;
  // Image imposée (sous-menu « Image ») : court-circuite horloge et freeze_body.
  opts.force_frame  = pose.frame;
  // 🔴 Échelle COMMUNE : chaque sprite garde les proportions de son fichier au
  // lieu d'être étiré jusqu'aux bords de la box (cf. RefDollSpanUnits). La box
  // n'est plus une limite mais un repère de pose — ce qui dépasse déborde du
  // cadre du siège, chapeaux hauts et montures compris, et c'est voulu : c'est le
  // seul moyen que deux personnages côte à côte soient à la même échelle.
  const float ref_span = RefDollSpanUnits(pose.anim);
  if (ref_span > 0.0f) {
    opts.force_scale = dh / ref_span;
    // Le repère de pose devient le CORPS et non la silhouette. Sans ça, ce qui
    // dépasse déplacerait le personnage au lieu de sortir du cadre : une arme
    // large le pousserait latéralement, une cape longue l'enfoncerait sous le
    // banc. Le corps est le seul point fixe une fois l'ajustement retiré.
    opts.center_on_body = true;
  }
  // Le nombre d'images de l'action, relevé au passage : le menu ne peut proposer
  // que celles qui existent, et ce nombre dépend de la pose ET de la classe.
  ro::DollPlacement place;
  opts.out_placement = &place;
  const bool drawn =
      ro::DrawDoll(ImGui::GetWindowDrawList(), dl, x, y, w, dh, opts);
  if (drawn && seat_index >= 0 && seat_index < kSeatCount)
    seat_frames_[seat_index] = place.frame_count;
  if (!drawn) {
    // Placeholder tant que les fichiers ne sont pas chargés : une silhouette
    // discrète, même encombrement -> zéro saut de scène.
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
  // Aperçu de CRÉATION : un Novice (job/body 0) avec les cheveux/couleur/sexe
  // choisis, debout, orienté par la flèche de rotation. Même moteur que
  // DrawDollAt ; l'apparence étant relue à chaque frame, changer un curseur met
  // l'aperçu à jour immédiatement.
  ro::DollLook dl;  // tout à 0 par défaut = Novice sans équipement
  dl.sex        = create_sex_;
  dl.hair       = create_hair_;
  dl.hair_color = create_hair_color_;
  const bool drawn =
      ro::DrawDoll(ImGui::GetWindowDrawList(), dl, x, y, w, h, create_dir_,
                   /*anim=*/0, static_cast<float>(ImGui::GetTime()));  // debout
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
  // Icône de coiffure = le sprite de tête, ajusté à la case en gardant le ratio
  // et centré. Rien dessiné tant que le sprite n'est pas prêt.
  // `color_override` >= 0 : recolore la coupe avec la palette head_<N>.pal.
  //
  // ⚠ `hair` part BRUT. Le remap des 12 coiffures historiques (id -> n° de
  // FICHIER .spr) vit dans ro::DrawHeadIcon, qui le fait pour tous ses
  // appelants — le dupliquer ici ferait diverger l'icône du reste de l'UI.
  // Et on n'envoie JAMAIS de n° de fichier : create_hair_ (id brut) part au
  // serveur, qui le stocke et laisse le client ré-appliquer la table.
  //
  // allow_upscale : une vignette de grille doit remplir sa case, contrairement
  // à la tête d'une ligne de liste qu'on ne fait que réduire.
  // `job` = 0 : la création ne propose que le Novice humain. Un Doram irait
  // chercher sa coupe dans une autre table ET un autre dossier de race.
  ro::DrawHeadIcon(ImGui::GetWindowDrawList(), x, y, sz, hair, create_sex_,
                   color_override, /*allow_upscale=*/true, /*job=*/0);
}

void CharSelect::DriveNativeCtrl(int ctrl, int slot) {
  // Pose le slot sélectionné (le natif le relit) puis pilote UINewSelectCharWnd
  // OnMsg (msg 6 / ctrl). Aucun paquet fabriqué ici : le natif construit tout.
  __try {
    *reinterpret_cast<uint8_t*>(kSelectedSlot) = static_cast<uint8_t>(slot);
    void* wnd = uiwnd::FindWindow(kCharSelWndId);  // vivante (cf. EnterGame)
    if (!wnd || *reinterpret_cast<uintptr_t*>(wnd) != kCharSelWndVtbl) {
      LogError("[CharSelect] fenêtre native absente/invalide -> ctrl 0x{:x} ignoré "
               "(slot {})", ctrl, slot);
      return;
    }
    // RET 0x18 = 6 args pile (cf. EnterGame) : typedef à 6 args obligatoire.
    // Positions = celles de uiwnd::OnMsg : (this, arg0=0, msg=6, p2=ctrl,
    // p3..p5=0). Adresse FIXE (pas la vtable) -> pas d'uiwnd::OnMsg, cf. EnterGame.
    using WndOnMsg_t = int(__thiscall*)(void*, int, int, int, int, int, int);
    reinterpret_cast<WndOnMsg_t>(kCharSelOnMsg)(wnd, 0, 6, ctrl, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogError("[CharSelect] exception ctrl 0x{:x} (slot {})", ctrl, slot);
  }
}

void CharSelect::DriveModeCmd(int cmd) {
  // Sortie d'écran (retour au login / fermeture du jeu) : on grave la mise en page
  // avant de partir. C'est indispensable ICI et pas seulement dans OnModeSwitch :
  // un retour au login ne CHANGE PAS de mode (le client reste en CLoginMode, seul
  // son état bouge), OnModeSwitch ne repasserait donc jamais et le travail du
  // joueur serait perdu.
  seat_edit_ = false;
  charsel::SaveIfDirty();
  // Envoie une commande au MODE courant (dispatcher *(0x0121333c), vtbl+0x18) — le
  // même point d'entrée que ReadSlot (cmd 8) et que le natif quand il quitte l'écran.
  // 5 args pile : DispCmd_t est le typedef partagé (aucun ne sert ici, tous à 0).
  __try {
    void* d = *reinterpret_cast<void**>(rag::kActiveModePtr);
    if (!d) {
      LogError("[CharSelect] dispatcher de mode absent -> commande {} ignorée", cmd);
      return;
    }
    auto fn = reinterpret_cast<DispCmd_t>(
        (*reinterpret_cast<uintptr_t**>(d))[kVfDispCmd / 4]);
    fn(d, cmd, 0, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogError("[CharSelect] exception sur la commande de mode {}", cmd);
  }
}

void CharSelect::OnRecvPacket(uint16_t opcode, const uint8_t* /*data*/,
                              uint16_t /*len*/) {
  // Purement passif : on ne lit AUCUN octet du paquet (forward_len = 0), on note
  // seulement l'instant. Le handler natif fait tout le travail de décodage.
  if (opcode == kOpCharList || opcode == kOpCharListPage) {
    list_tick_ = GetTickCount();
    if (list_tick_ == 0) list_tick_ = 1;  // 0 = « jamais reçu », réservé
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
    active_since_ = 0;
    enter_failed_until_ = 0;
    del_reject_until_ = 0;
    del_reject_seq_seen_ = g_del_reject_seq;  // ne pas ressortir un refus d'avant
    wait_since_ = 0;
    list_warned_ = false;
    seat_edit_ = false;  // on n'arrive jamais sur l'écran en mode personnalisation
  } else {
    // Entrée en jeu : dernier filet pour la mise en page laissée ouverte (le
    // joueur a pu double-cliquer un personnage en pleine personnalisation).
    seat_edit_ = false;
    charsel::SaveIfDirty();
    // On quitte le login (entrée en jeu) : notre scène ne couvre plus rien. Sans ce
    // reset, g_cover_active reste collé à true — OnRenderLoginUI, seul endroit qui
    // le remettait à false, n'est plus appelé une fois en jeu. Voir Detour_ShowModal.
    g_cover_active = false;
    // ⚠ Et c'est AUSSI ici qu'il faut marquer la sortie d'écran pour la fraîcheur
    // de la liste : le front « présent -> absent » est détecté dans
    // OnRenderLoginUI, or celui-ci n'est plus appelé une fois en jeu. Sans cette
    // ligne, screen_gone_tick_ resterait figé à une date antérieure au paquet de
    // liste de la session précédente, qui repasserait donc pour « fraîche » au
    // retour au char-select — exactement le bug qu'on corrige.
    screen_gone_tick_ = GetTickCount();
    screen_was_alive_ = false;
    wait_since_ = 0;
  }
}

void CharSelect::DrawHallBackdrop(ImDrawList* dl, const ImVec2& disp) {
  // Décor plein écran : BMP du banquet chargé par le loader natif, ou dégradé
  // sombre si le fichier n'est pas déployé. Partagé par la table et le fondu de
  // transition (qui doit s'assombrir DEPUIS le décor, pas depuis un écran vide,
  // sinon le char-select natif réapparaîtrait le temps du fondu).
  if (void* hall = LoadHallTexture(CurrentBackgroundPath())) {
    dl->AddImage(reinterpret_cast<ImTextureID>(hall), ImVec2(0, 0), disp);
  } else {
    dl->AddRectFilledMultiColor(ImVec2(0, 0), disp, IM_COL32(24, 22, 30, 255),
                                IM_COL32(24, 22, 30, 255), IM_COL32(12, 11, 16, 255),
                                IM_COL32(12, 11, 16, 255));
  }
  // Léger voile bas pour asseoir titre/barre d'action sur le décor.
  dl->AddRectFilledMultiColor(ImVec2(0, disp.y - 96.0f), disp,
                              IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                              IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150));
}

void CharSelect::DrawTransitionFade(const char* label, unsigned long since) {
  // Transitions (entrée en jeu / fermeture du jeu) : décor + fondu au noir.
  // Une fois l'entrée déclenchée (EnterGame -> OnMsg 0xB8), le natif VIDE les
  // CHARACTER_INFO pour semer l'état en jeu. Si on continuait à dessiner les sièges,
  // on verrait pendant ~½ s les dolls s'effacer et le slot retomber sur ses valeurs
  // par défaut (job 0 / sex 0 = Novice femelle). Le retour au login, lui, n'a pas
  // besoin de fondu : le formulaire Moonlight reprend l'écran dès la frame suivante.
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  ImGui::SetNextFrameWantCaptureKeyboard(true);
  ImGui::SetNextFrameWantCaptureMouse(true);
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(disp, ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  g_cover_active = true;  // on couvre encore le natif (détour de msgbox)
  // MÊME nom de fenêtre que la table : ImGui garde ainsi le même état/z-order, la
  // transition ne provoque aucun saut.
  ImGui::Begin("##charselect_full", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  DrawHallBackdrop(dl, disp);
  float a = static_cast<float>(GetTickCount() - since) / 260.0f;  // ~260 ms
  if (a > 1.0f) a = 1.0f;
  const int av = static_cast<int>(a * 255.0f);
  dl->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, av));
  const ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2((disp.x - ts.x) * 0.5f, disp.y * 0.5f - ts.y * 0.5f),
              IM_COL32(235, 230, 220, av), label);
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();  // WindowBg
}

bool CharSelect::NativeScreenHasKeyboard() const {
  // Hors char-select, rien à dire : c'est le parcours de login, dont MoonlightAuth
  // garde la touche (les écrans natifs y sont masqués derrière le formulaire).
  if (!native_login::CharSelectWindowPresent()) return false;
  // Écran natif ASSUMÉ : plugin désactivé, repli « Mode Classique », dialogue
  // natif en cours (création / suppression), ou login non-Moonlight (règle
  // produit : login natif => char-select natif).
  if (!enabled_ || native_fallback_ || native_op_) return true;
  if (!force_ && (auth_ == nullptr || !auth_->DroveMoonlightLogin())) return true;
  // Compte SANS aucun personnage : notre scène ne s'affiche pas (rien à peupler)
  // et le joueur crée son premier personnage dans la fenêtre native — où le nom
  // se valide à l'Entrée. Fait natif, pas un drapeau de rendu.
  //
  // ⚠ Mais pas tout de suite : au tout début, « aucun personnage » est aussi ce
  // que voit un compte normal dont la liste n'est pas encore décodée, et notre
  // voile d'attente couvre alors l'écran. Même borne que lui (kCoverWaitMs), pour
  // que couverture et clavier ne se contredisent jamais.
  if (!native_login::CharListLoaded())
    return (GetTickCount() - screen_arrived_tick_) >= kCoverWaitMs;
  return false;  // notre scène tient l'écran
}

void CharSelect::DrawWaitCover(const char* label) {
  // Le char-select NATIF est à l'écran mais nos données ne sont pas encore
  // exploitables (liste pas confirmée fraîche, slots pas décodables). On COUVRE
  // au lieu de rendre la main.
  //
  // 🔴 Rendre la main laissait le natif VISIBLE et surtout JOIGNABLE : le joueur
  // qui martèle Entrée depuis l'écran de login (ou une Entrée encore en file)
  // cliquait son bouton par défaut — entrée en jeu sur le premier slot, ou
  // fenêtre native de création quand ce slot est vide, dont notre scène ne
  // reprend plus la main tant qu'on n'a pas annulé à la main. La confiscation de
  // clavier (MoonlightAuth::WantsKeyboard) ferme cette porte-là ; ce voile
  // ferme celle de la SOURIS et supprime le clignotement d'écran natif.
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  ro::SetFullscreenCursorActive();
  ImGui::SetNextFrameWantCaptureKeyboard(true);
  ImGui::SetNextFrameWantCaptureMouse(true);
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(disp, ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  g_cover_active = true;  // on couvre le natif (détour de msgbox)
  // MÊME nom de fenêtre que la table et que le fondu : ImGui garde le même
  // état/z-order, l'arrivée de la scène ne provoque aucun saut.
  ImGui::Begin("##charselect_full", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  DrawHallBackdrop(dl, disp);
  const ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2((disp.x - ts.x) * 0.5f, disp.y * 0.5f - ts.y * 0.5f),
              IM_COL32(235, 230, 220, 235), label);
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();  // WindowBg
}

void CharSelect::ToggleLayoutEdit() {
  seat_edit_ = !seat_edit_;
  if (seat_edit_) {
    // Le joueur vient peut-être de déposer un .bmp sans quitter le jeu : la
    // galerie doit refléter le dossier tel qu'il est À CET INSTANT.
    charsel::RescanBackgrounds();
    // Sélection repartie de zéro : l'index d'un passage précédent ne désigne plus
    // forcément le même enregistrement (une suppression a pu décaler la liste).
    preset_sel_ = -1;
    preset_del_armed_ = false;
  } else {
    // Sortie du mode = on grave. Enregistrer à chaque pixel de glissement
    // réécrirait le fichier des dizaines de fois par seconde.
    charsel::SaveIfDirty();
  }
}

void CharSelect::DrawLayoutGuides(ImDrawList* dl, const ImVec2& disp) {
  // Repères de composition (règle des tiers + axe central) : placer un personnage
  // « au milieu » à l'œil nu sur un décor chargé est autrement un pari.
  const ImU32 grid = IM_COL32(255, 255, 255, 26);
  for (int i = 1; i < 3; ++i) {
    const float x = disp.x * static_cast<float>(i) / 3.0f;
    const float y = disp.y * static_cast<float>(i) / 3.0f;
    dl->AddLine(ImVec2(x, 0.0f), ImVec2(x, disp.y), grid);
    dl->AddLine(ImVec2(0.0f, y), ImVec2(disp.x, y), grid);
  }
  dl->AddLine(ImVec2(disp.x * 0.5f, 0.0f), ImVec2(disp.x * 0.5f, disp.y),
              IM_COL32(255, 210, 90, 45));

  // Rappel des gestes, en haut : le panneau peut être déplacé n'importe où, le
  // joueur ne doit pas avoir à le retrouver pour se souvenir de la molette.
  const char* hint =
      i18n::Tr("Personnalisation — glisser : placer  •  molette : tourner  •  "
      "Ctrl+molette : taille  •  clic droit : pose  •  pastilles bleues : "
      "blocs d'interface");
  const ImVec2 hs = ImGui::CalcTextSize(hint);
  const float hx = (disp.x - hs.x) * 0.5f, hy = 6.0f;
  dl->AddRectFilled(ImVec2(hx - 12.0f, hy - 4.0f),
                    ImVec2(hx + hs.x + 12.0f, hy + hs.y + 4.0f),
                    IM_COL32(20, 30, 50, 225), 4.0f);
  dl->AddRect(ImVec2(hx - 12.0f, hy - 4.0f),
              ImVec2(hx + hs.x + 12.0f, hy + hs.y + 4.0f),
              IM_COL32(120, 200, 255, 200), 4.0f, 0, 1.5f);
  dl->AddText(ImVec2(hx, hy), IM_COL32(215, 235, 255, 255), hint);
}

// Chemin rendu LISIBLE : les antislashs deviennent des barres obliques pour
// l'affichage seul. La police de l'UI est Malgun Gothic (coréenne), qui dessine
// U+005C comme le symbole won « ₩ » — l'octet est bien 0x5C et le loader reçoit le
// bon chemin, mais à l'écran « lobby\x.bmp » se lit « lobby₩x.bmp » et donne
// l'impression que le séparateur est cassé. Le chemin STOCKÉ garde ses antislashs.
std::string DisplayPath(const std::string& p) {
  std::string s = p;
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

void CharSelect::DrawLayoutEditor() {
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  // Taille FIXE (redimensionnable à la main) et non AlwaysAutoResize : le panneau
  // est fait de TextWrapped et d'un enfant à largeur libre, dont la mise en page
  // dépend de la largeur — les laisser décider de cette même largeur fait osciller
  // la fenêtre d'une frame à l'autre.
  ImGui::SetNextWindowSize(ImVec2(370.0f, 480.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2((std::max)(20.0f, disp.x - 400.0f), 70.0f),
                          ImGuiCond_FirstUseEver);
  bool open = true;
  // Repli AUTORISÉ pour ce panneau : il couvre une partie de la scène qu'on est
  // justement en train de composer, et il faut pouvoir le sortir du chemin sans
  // quitter le mode (ce qui écrirait le layout et ferait perdre la sélection).
  //
  // ⚠ Le repli est interdit GLOBALEMENT hors du monde de jeu (Bourgeon::RenderUI
  // pose SetWindowCollapseAllowed(false) à chaque frame au login/char-select,
  // pour qu'un formulaire de connexion ne puisse pas rester réduit à une barre de
  // titre). On lève donc l'interdit le temps de CETTE fenêtre, et on le remet
  // aussitôt — la valeur étant reposée à chaque frame, rien ne fuit.
  ro::SetWindowCollapseAllowed(true);
  const bool begun =
      ro::BeginRoWindow(i18n::Tr("Personnaliser l'écran###bourgeon_charsel_layout"), &open);
  if (begun) {
    charsel::Layout& lay = charsel::State();

    ImGui::TextUnformatted(i18n::Tr("Décor"));
    ImGui::Separator();
    const std::vector<charsel::Background>& bgs = charsel::Backgrounds();
    const std::string current = lay.background;
    ImGui::BeginChild("##bg_list", ImVec2(0.0f, 132.0f), true);
    for (const charsel::Background& b : bgs) {
      const bool sel = (b.path == current);
      if (ImGui::Selectable(b.label.c_str(), sel) && !sel) {
        // Appliqué IMMÉDIATEMENT : LoadHallTexture recharge dès que le chemin
        // change, le joueur voit son décor derrière le panneau sans valider.
        lay.background = b.path;
        charsel::MarkDirty();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", DisplayPath(b.path).c_str());
    }
    ImGui::EndChild();

    // Un fichier peut avoir été supprimé/renommé depuis, ou ne pas être un BMP
    // que le loader accepte : le dire, plutôt que de laisser le joueur devant un
    // fond uni sans comprendre.
    if (!BackgroundLoaded()) {
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 40, 40, 255));
      ImGui::TextWrapped(i18n::Tr("Ce décor n'a pas pu être chargé (fichier absent ou "
                         "format refusé) — fond uni en attendant."));
      ImGui::PopStyleColor();
    }
    if (ro::RoButton(i18n::Tr("Rafraîchir"), 110.0f, 0.0f)) charsel::RescanBackgrounds();
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Ouvrir le dossier"), 160.0f, 0.0f))
      charsel::OpenBackgroundFolder();
    // Chemins écrits en barres obliques : cf. DisplayPath (la police coréenne
    // dessine l'antislash en « ₩ »).
    ImGui::TextWrapped(
        i18n::Tr("Dépose tes images .bmp dans data/texture/lobby/ (24 ou 32 bits, nom "
        "sans accent), puis « Rafraîchir ». Le bouton ci-dessus ouvre le bon "
        "dossier et le crée au besoin."));
    // Le client ne résout PAS data\lobby\ (il cherche sous data\texture\) : des
    // images laissées là resteraient invisibles sans explication.
    if (const int misplaced = charsel::MisplacedBackgroundCount()) {
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 90, 0, 255));
      ImGui::TextWrapped(i18n::Tr("%d image(s) se trouvent dans %s : le client ne lit pas "
                         "ce dossier, déplace-les vers data/texture/lobby/."),
                         misplaced,
                         DisplayPath(charsel::MisplacedBackgroundDir()).c_str());
      ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(i18n::Tr("Places libres"));
    ImGui::Separator();
    bool hide = lay.hide_empty_seats;
    if (ro::RoCheckbox(i18n::Tr("Masquer les places libres"), &hide)) {
      lay.hide_empty_seats = hide;
      charsel::MarkDirty();
    }
    if (!hide) {
      float alpha = lay.empty_seat_alpha;
      if (ro::RoSliderFloat(i18n::Tr("Opacité"), &alpha, 0.0f, 1.0f)) {
        lay.empty_seat_alpha = alpha;
        charsel::MarkDirty();
      }
    }
    ImGui::TextWrapped(i18n::Tr("Masquées, elles réapparaissent au survol : créer un "
                       "personnage reste possible. Elles restent toujours "
                       "visibles pendant la personnalisation."));

    ImGui::Spacing();
    ImGui::TextUnformatted("Placement");
    ImGui::Separator();
    ImGui::TextWrapped(
        i18n::Tr("Glisse un personnage pour déplacer sa place. Molette dessus : le "
        "tourner ; Ctrl+molette : sa taille ; clic droit : sa pose (assis, "
        "debout, touché, mort) et l'orientation de sa tête.\n"
        "Les pastilles bleues déplacent le titre, la barre de boutons, la "
        "pagination et la barre de sortie."));
    if (ro::RoButton(i18n::Tr("Replacer les personnages"), 200.0f, 0.0f))
      charsel::ResetPlacement();
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Tout restaurer"), 150.0f, 0.0f)) {
      // Retour à la mise en page LIVRÉE (décor d'origine compris). Les
      // enregistrements du joueur ne sont pas touchés : c'est une remise à zéro de
      // l'écran, pas un effacement de son travail.
      charsel::ResetAll();
      preset_sel_ = -1;
    }

    // ── Mises en page enregistrées ───────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextUnformatted(i18n::Tr("Mises en page enregistrées"));
    ImGui::Separator();
    const std::vector<charsel::Preset>& presets = charsel::Presets();
    ImGui::BeginChild("##preset_list", ImVec2(0.0f, 96.0f), true);
    if (presets.empty()) {
      ImGui::TextColored(ImVec4(0.36f, 0.38f, 0.42f, 1.0f),
                         i18n::Tr("Aucune pour l'instant."));
    }
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
      if (ImGui::Selectable(presets[i].name.c_str(), preset_sel_ == i)) {
        preset_sel_ = i;
        // Le champ suit la sélection : « Enregistrer » écrase alors CE nom, ce que
        // le joueur attend après avoir cliqué dessus.
        std::snprintf(preset_name_, sizeof(preset_name_), "%s",
                      presets[i].name.c_str());
        preset_del_armed_ = false;
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        charsel::ApplyPreset(i);
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##preset_name", i18n::Tr("Nom (ex. « Ma taverne »)"),
                             preset_name_, sizeof(preset_name_));
    const bool has_name = (preset_name_[0] != '\0');
    if (!has_name) ImGui::BeginDisabled();
    if (ro::RoButton("Enregistrer", 130.0f, 0.0f)) {
      if (charsel::SavePreset(preset_name_)) preset_del_armed_ = false;
      // Sélectionne l'entrée qui vient d'être écrite (nouvelle ou mise à jour).
      const std::vector<charsel::Preset>& after = charsel::Presets();
      for (int i = 0; i < static_cast<int>(after.size()); ++i)
        if (after[i].name == preset_name_) { preset_sel_ = i; break; }
    }
    if (!has_name) ImGui::EndDisabled();
    const bool has_sel =
        (preset_sel_ >= 0 && preset_sel_ < static_cast<int>(presets.size()));
    ImGui::SameLine();
    if (!has_sel) ImGui::BeginDisabled();
    if (ro::RoButton("Appliquer", 120.0f, 0.0f)) {
      charsel::ApplyPreset(preset_sel_);
      preset_del_armed_ = false;
    }
    ImGui::SameLine();
    // Suppression en DEUX temps plutôt qu'un dialogue : un enregistrement effacé
    // par erreur est irrécupérable, mais une boîte modale de plus sur cet écran
    // (déjà peuplé de popups create/delete) coûterait plus qu'elle ne protège.
    if (ro::RoButton(preset_del_armed_ ? "Confirmer" : "Supprimer", 120.0f, 0.0f)) {
      if (preset_del_armed_) {
        charsel::DeletePreset(preset_sel_);
        preset_sel_ = -1;
        preset_del_armed_ = false;
      } else {
        preset_del_armed_ = true;
      }
    }
    if (!has_sel) ImGui::EndDisabled();
    if (preset_del_armed_ && has_sel)
      ImGui::TextColored(ImVec4(0.55f, 0.15f, 0.10f, 1.0f),
                         i18n::Tr("Clique « Confirmer » pour supprimer « %s »."),
                         presets[preset_sel_].name.c_str());
    else
      ImGui::TextWrapped(i18n::Tr("Double-clic sur un nom = l'appliquer. « Enregistrer » "
                         "range l'écran tel qu'il est sous ce nom."));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ro::RoButton("Terminer", 120.0f, 0.0f)) open = false;
    ImGui::SameLine();
    // Couleurs explicites : le corps d'une fenêtre RO est CLAIR, ImGui::TextDisabled
    // y est illisible.
    if (charsel::Dirty())
      ImGui::TextColored(ImVec4(0.45f, 0.30f, 0.05f, 1.0f),
                         i18n::Tr("Sera enregistré en quittant"));
    else
      ImGui::TextColored(ImVec4(0.15f, 0.42f, 0.18f, 1.0f), i18n::Tr("Enregistré"));
  }
  ro::EndRoWindow();
  ro::SetWindowCollapseAllowed(false);  // on rend l'interdit général de l'écran
  if (!open) ToggleLayoutEdit();  // « Terminer » ou croix -> sortie + écriture
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

  // ── Transition en cours : fondu au noir, traité AVANT toute sonde d'écran ────
  // Pendant l'entrée en jeu le natif purge ses fenêtres ET vide les CHARACTER_INFO :
  // les sondes ci-dessous tombent toutes, et on cesserait de dessiner en plein fondu.
  //
  // Filet anti-écran-mort : notre OnMsg 0xB8 peut ne RIEN produire (fenêtre native
  // pas encore construite, slot refusé par le natif…). Le mode reste alors sagement
  // au char-select pendant que nous couvrons l'écran et captons tout l'input — plus
  // aucune issue pour le joueur, écran noir « Entrée en jeu… » définitif (constaté :
  // mode CLoginMode état 7, transition demandée à -1, fenêtre native bien vivante
  // sous le fondu). Une entrée qui ABOUTIT change l'état du mode, ce qui purge les
  // fenêtres en une frame : si le char-select natif est toujours là au bout de
  // kEnterTimeoutMs, c'est que rien n'est parti -> on rend la main.
  if (entering_ || quitting_) {
    const bool stalled = entering_ && NativeCharSelectAlive() &&
                         (GetTickCount() - enter_tick_) > kEnterTimeoutMs;
    if (!stalled) {
      DrawTransitionFade(entering_ ? i18n::Tr("Entrée en jeu…") : i18n::Tr("Fermeture du jeu…"),
                         entering_ ? enter_tick_ : quit_tick_);
      return;
    }
    LogError("[CharSelect] entrée en jeu sans effet (char-select natif toujours "
             "présent après {} ms) -> retour à la table", kEnterTimeoutMs);
    entering_ = false;
    enter_failed_until_ = GetTickCount() + 8000;  // bandeau d'explication
    // Réarme la fenêtre d'insensibilité : sans ça, une touche Entrée encore
    // maintenue/spammée relancerait la séquence dans la foulée, en boucle.
    active_since_ = GetTickCount();
  }

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

  // ── Sonde d'écran : la FENÊTRE NATIVE, surtout PAS les CHARACTER_INFO ────────
  // Les CHARACTER_INFO SURVIVENT au retour à l'écran de connexion : au login SUIVANT
  // (même processus) elles sont encore lisibles, donc « au moins un slot chargé »
  // était déjà vrai à l'instant où le joueur envoyait ses identifiants. La table
  // s'affichait alors PAR-DESSUS le login en cours et captait le clavier ; une Entrée
  // (spam du joueur, ou auto-confirm char-server de MoonlightAuth) déclenchait une
  // entrée en jeu sur un écran qui n'existait pas encore -> écran mort. Les fenêtres,
  // elles, sont purgées à chaque changement d'état du mode : FindWindow(0x115) ne
  // répond que si le char-select natif est RÉELLEMENT à l'écran.
  const bool screen_alive = NativeCharSelectAlive();
  // FRONT présent -> absent : c'est l'instant de référence de la fraîcheur. On ne
  // l'enregistre QUE sur le front — le rafraîchir à chaque frame d'absence rendrait
  // toute liste « périmée », puisque le paquet arrive justement pendant l'absence.
  if (screen_was_alive_ && !screen_alive) screen_gone_tick_ = GetTickCount();
  // FRONT absent -> présent : départ du voile d'attente (cf. DrawWaitCover).
  if (!screen_was_alive_ && screen_alive) screen_arrived_tick_ = GetTickCount();
  screen_was_alive_ = screen_alive;
  if (!screen_alive) {
    active_ = false;
    active_since_ = 0;
    return;
  }

  // ── La liste affichée appartient-elle à CETTE session ? ─────────────────────
  // Le natif ouvre sa fenêtre dès l'entrée sur l'écran, mais les CHARACTER_INFO
  // de la session PRÉCÉDENTE sont encore lisibles : sans ce test, la table
  // s'affiche avec les mauvais personnages, et l'auto-confirm de MoonlightAuth
  // peut faire entrer en jeu dessus (le natif, lui, relit le vrai slot -> job et
  // sexe incohérents). La liste est fraîche si son paquet est arrivé APRÈS la
  // dernière sortie de l'écran.
  const unsigned long now = GetTickCount();
  // Le voile ne se tient qu'un temps borné : au-delà, on rend la main au natif
  // comme avant (compte sans personnage, opcode de liste inattendu…). Il ferme
  // une fenêtre de quelques frames, il ne doit jamais devenir une prison.
  const bool may_cover = (now - screen_arrived_tick_) < kCoverWaitMs;
  const bool list_fresh =
      list_tick_ != 0 &&
      static_cast<long>(list_tick_ - screen_gone_tick_) >= 0;
  if (!list_fresh) {
    // Repli de sûreté : on ne bloque JAMAIS durablement. Passé kListWaitMs on
    // s'arme quand même — et on le signale une fois.
    if (wait_since_ == 0) wait_since_ = now;
    if (now - wait_since_ < kListWaitMs) {
      active_ = false;
      active_since_ = 0;
      // On COUVRE l'attente au lieu de découvrir le char-select natif : le
      // laisser visible, c'est le laisser cliquable et le laisser réagir aux
      // Entrées encore en file (entrée en jeu / création non demandées).
      if (may_cover) DrawWaitCover(i18n::Tr("Chargement des personnages…"));
      return;
    }
    if (!list_warned_) {
      list_warned_ = true;
      LogError("[CharSelect] aucun paquet de liste (0x{:04X}/0x{:04X}) vu en {} ms "
              "-> table armée sans confirmation. Si ça se répète, l'opcode de "
              "liste de ce client n'est pas celui observé.",
              kOpCharList, kOpCharListPage, kListWaitMs);
    }
  } else {
    wait_since_ = 0;
  }

  const int cap = SlotCapacity();
  if (cap <= 0) {
    active_ = false;
    if (may_cover) DrawWaitCover(i18n::Tr("Chargement des personnages…"));
    return;
  }

  // Lit tous les slots (occupés + vides).
  static CharView views[128];
  int nfilled = 0;
  for (int i = 0; i < cap && i < 128; ++i) {
    if (ReadSlot(i, &views[i])) ++nfilled;
  }
  if (nfilled == 0) {  // liste pas encore décodable — ou compte sans personnage
    active_ = false;
    if (may_cover) DrawWaitCover(i18n::Tr("Chargement des personnages…"));
    return;
  }
  if (!active_) active_since_ = GetTickCount();   // arrivée sur l'écran (edge)
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
      ImGui::TextUnformatted(i18n::Tr("Gestion native en cours…"));
      if (ImGui::Button(i18n::Tr("Revenir à la table"))) {
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

  // Autofocus : à l'arrivée, présélectionne le DERNIER personnage joué s'il est
  // là (déco/reco rapide : on retombe sur son perso, pas sur le slot 0), sinon le
  // premier personnage. Dans les deux cas Entrée joue tout de suite, sans clic.
  if (selected_ < 0) {
    for (const uint32_t cid : recent_chars_) {  // du plus récent au plus ancien
      for (int i = 0; i < cap && i < 128; ++i) {
        if (views[i].occupied && views[i].gid == cid) { selected_ = i; break; }
      }
      if (selected_ >= 0) break;
    }
  }
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

  DrawHallBackdrop(dl, disp);

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
  //
  // ⚠ Et pas non plus dans les premières kEnterGraceMs d'affichage de la table : en
  // arrivant ici la file de messages contient encore les Entrées destinées aux écrans
  // PRÉCÉDENTS (joueur qui spamme Entrée pour traverser login + « Select Service »,
  // et les Entrées postées par l'auto-confirm char-server de MoonlightAuth). Sans ce
  // délai, elles jouaient le personnage avant même que le joueur ait vu la table.
  const bool enter_key = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
  // ⚠ Ni pendant la personnalisation : on y glisse des sièges au-dessus des
  // personnages, entrer en jeu sur une frappe d'Entrée ferait perdre la mise en
  // page en cours.
  const bool enter_armed = (GetTickCount() - active_since_) > kEnterGraceMs;
  if (enter_key && enter_armed && !io.WantTextInput && !any_popup && !seat_edit_ &&
      can_enter_slot(selected_))
    EnterGame(selected_);

  // ── Sièges (position = Seats()[i]) ; slot de perso = CharForSeat(i, page) ─────
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
          curx = Seats()[i].nx * disp.x;
          cury = Seats()[i].ny * disp.y;
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
        const float vx = Seats()[i].nx * disp.x - curx;
        const float vy = Seats()[i].ny * disp.y - cury;
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

    const Seat& st = Seats()[i];
    const float cx = st.nx * disp.x;
    const float chair_y = st.ny * disp.y;
    const float box_h = st.scale * disp.y;
    // Cadre du siège : c'est la ZONE CLIQUABLE et le repère du mode
    // « Personnaliser », plus la limite du dessin — le pantin garde l'échelle de
    // ses fichiers et peut en déborder (cf. DrawDollAt).
    const float w = box_h * kDollBoxAspect;
    const ImVec2 tl(cx - w * 0.5f, chair_y - box_h);
    const ImVec2 br(cx + w * 0.5f, chair_y + disp.y * 0.02f);

    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(tl);
    ImGui::InvisibleButton("seat", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    // Mode « Personnaliser » : glisser pour placer, molette pour TOURNER le
    // personnage, Ctrl+molette pour sa taille. La rotation est le geste le plus
    // fréquent une fois le décor changé — elle a donc la molette nue ; le zoom,
    // qu'on règle une fois, prend le modificateur.
    // Les valeurs partent dans le layout persisté (écrit à la sortie du mode).
    if (seat_edit_) {
      if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        Seats()[i].nx += io.MouseDelta.x / disp.x;
        Seats()[i].ny += io.MouseDelta.y / disp.y;
        charsel::MarkDirty();
      }
      if (hovered && io.MouseWheel != 0.0f) {
        if (io.KeyCtrl) {
          Seats()[i].scale =
              (std::max)(0.05f, Seats()[i].scale + io.MouseWheel * 0.005f);
        } else {
          // 8 orientations en boucle. Un cran = un huitième de tour ; le signe
          // suit le sens de la molette, et l'entier reste dans [0,7] (le & 7
          // ramènerait -1 à 7 sans passer par un modulo négatif).
          const int step = (io.MouseWheel > 0.0f) ? 1 : -1;
          Seats()[i].dir = (Seats()[i].dir + step + 8) & 7;
        }
        charsel::MarkDirty();
      }
      // Clic DROIT : choisir la pose de ce siège (action + orientation de tête).
      // Ouverture différée hors du PushID, comme le menu contextuel des coupons.
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        seat_ctx_ = i;
        seat_ctx_req_ = true;
      }
      dl->AddRect(tl, br, IM_COL32(255, 210, 90, 220), 3.0f, 0, 1.5f);
      char lbl[8];
      std::snprintf(lbl, sizeof(lbl), "%d", i + 1);
      dl->AddText(ImVec2(cx - 4, tl.y - 14), IM_COL32(255, 220, 120, 255), lbl);
    }

    if (empty) {
      // Place libre : marqueur « + ». Masquable / atténuable (réglage du mode
      // « Personnaliser ») — mais JAMAIS pendant l'édition elle-même, sinon on
      // ne pourrait plus placer les sièges inoccupés.
      const charsel::Layout& lay = charsel::State();
      // 🔴 Masquées, elles restent CLIQUABLES et réapparaissent au survol : le
      // bouton invisible est déjà soumis (plus haut), seul le dessin est sauté.
      // Sans ça, masquer les places libres supprimerait le seul chemin de
      // création de personnage.
      if (lay.hide_empty_seats && !seat_edit_ && !hovered) {
        ImGui::PopID();
        continue;
      }
      // L'opacité ne s'applique qu'au repos : au survol, le marqueur reprend
      // toute sa présence, sans quoi une place très atténuée deviendrait
      // impossible à viser avec assurance.
      const int a_idle = static_cast<int>(150.0f * lay.empty_seat_alpha);
      const ImU32 col = hovered ? IM_COL32(255, 230, 150, 230)
                                : IM_COL32(210, 200, 180, a_idle);
      const float r = w * 0.30f;
      dl->AddCircle(ImVec2(cx, chair_y - box_h * 0.5f), r, col, 24, 2.0f);
      dl->AddLine(ImVec2(cx - r * 0.5f, chair_y - box_h * 0.5f),
                  ImVec2(cx + r * 0.5f, chair_y - box_h * 0.5f), col, 2.0f);
      dl->AddLine(ImVec2(cx, chair_y - box_h * 0.5f - r * 0.5f),
                  ImVec2(cx, chair_y - box_h * 0.5f + r * 0.5f), col, 2.0f);
      if (hovered) ImGui::SetTooltip(i18n::Tr("Créer un personnage (emplacement %d)"), ci + 1);
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

      DrawDollAt(v, cx, chair_y, box_h, st, i);

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

      // Ligne suivante libre sous le nom : badges de coupon, puis suppression.
      float stack_y = ly + ns.y + 3.0f;

      // Badge(s) coupon (le natif met une image « Click to Rename » via un effet ;
      // nous une pastille ImGui) : rename (or) et/ou change-slot (cyan). Signale
      // qu'un coupon est ACTIF sur ce perso (flag CHARACTER_INFO).
      //
      // 🔴 SOUS le nom, et non en haut du cadre du siège comme avant : la hauteur
      // du cadre suit le zoom du personnage, si bien qu'un siège agrandi envoyait
      // sa pastille très au-dessus, détachée de tout. Sous le nom, elle reste
      // collée au groupe quelle que soit la taille du pantin.
      if (v.rename_avail > 0 || v.moves_avail > 0) {
        // Centré comme le nom : on mesure d'abord la largeur totale des pastilles.
        const float gap = 3.0f;
        float total = 0.0f;
        if (v.rename_avail > 0)
          total += ImGui::CalcTextSize("Renom.").x + 8.0f + gap;
        if (v.moves_avail > 0) total += ImGui::CalcTextSize("Slot").x + 8.0f + gap;
        float bx = cx - (total - gap) * 0.5f;
        const float by = stack_y;
        float bh = 0.0f;
        const auto badge = [&](const char* txt, ImU32 bg) {
          const ImVec2 ts = ImGui::CalcTextSize(txt);
          const ImVec2 a(bx, by), b(bx + ts.x + 8.0f, by + ts.y + 3.0f);
          dl->AddRectFilled(a, b, bg, 3.0f);
          dl->AddRect(a, b, IM_COL32(0, 0, 0, 120), 3.0f);
          dl->AddText(ImVec2(bx + 4.0f, by + 1.0f), IM_COL32(30, 25, 10, 255), txt);
          bx = b.x + gap;
          bh = b.y - a.y;
        };
        if (v.rename_avail > 0) badge("Renom.", IM_COL32(255, 214, 120, 235));
        if (v.moves_avail > 0) badge("Slot", IM_COL32(140, 220, 255, 235));
        stack_y += bh + 2.0f;
      }

      if (v.del_rev_date > 0) {
        const char* del = i18n::Tr("Suppression programmée");
        const ImVec2 ds = ImGui::CalcTextSize(del);
        const float dx = cx - ds.x * 0.5f, dyy = stack_y;
        // Fond (rouge très sombre) pour détacher nettement le texte du décor de la
        // table. La ligne de texte réserve de l'espace d'ascension AU-DESSUS des
        // capitales -> on rentre le haut du rect de ~3px pour ne pas laisser de bande
        // vide au-dessus du texte (accents é toujours couverts).
        dl->AddRectFilled(ImVec2(dx - 6, dyy + 2.0f),
                          ImVec2(dx + ds.x + 6, dyy + ds.y + 1),
                          IM_COL32(40, 0, 0, 205), 3.0f);
        dl->AddText(ImVec2(dx, dyy), IM_COL32(255, 150, 150, 255), del);
      }

      // Pas de fiche pendant la personnalisation : elle suivrait la souris qui
      // traîne le siège et masquerait le placement en cours.
      if (hovered && !seat_edit_) {
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
                             i18n::Tr("Suppression programmée"));
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
  const char* title = i18n::Tr("Choisis ton personnage");
  const ImVec2 tsz = ImGui::CalcTextSize(title);
  // Position ancrée (poignée déplaçable en mode édition). Le point = milieu-haut.
  const ImVec2 tp = Anchor("titre", 0.5f, 24.0f / disp.y, seat_edit_);
  const float tx = tp.x - tsz.x * 0.5f;
  dl->AddText(ImVec2(tx + 1, tp.y + 1), IM_COL32(0, 0, 0, 160), title);
  dl->AddText(ImVec2(tx, tp.y), IM_COL32(245, 236, 210, 255), title);

  // Bandeau « suppression refusée » (poll de del_rev_date resté à 0 -> refus serveur,
  // perso en guilde/groupe). Affiché ~5 s sous le titre. La msgbox native équivalente
  // est cachée derrière notre UI plein écran, d'où ce relais ImGui.
  // Même bandeau pour l'entrée en jeu abandonnée par le filet anti-écran-mort : le
  // joueur doit comprendre pourquoi le fondu est retombé sur la table.
  if (del_reject_until_ <= GetTickCount() && enter_failed_until_ > GetTickCount()) {
    const char* msg = i18n::Tr("L'entrée en jeu n'a pas abouti — réessaie.");
    const ImVec2 ms = ImGui::CalcTextSize(msg);
    const float by = tp.y + tsz.y + 10.0f;
    const float bx = (disp.x - ms.x) * 0.5f;
    dl->AddRectFilled(ImVec2(bx - 12, by - 4), ImVec2(bx + ms.x + 12, by + ms.y + 4),
                      IM_COL32(70, 40, 0, 235), 4.0f);
    dl->AddRect(ImVec2(bx - 12, by - 4), ImVec2(bx + ms.x + 12, by + ms.y + 4),
                IM_COL32(220, 160, 60, 235), 4.0f, 0, 1.5f);
    dl->AddText(ImVec2(bx + 1, by + 1), IM_COL32(0, 0, 0, 180), msg);
    dl->AddText(ImVec2(bx, by), IM_COL32(255, 225, 170, 255), msg);
  }
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

  // Largeur RÉELLE de la barre, pour la centrer. ⚠ Suppression PROGRAMMÉE = DEUX
  // boutons (« Annuler suppression » + « Supprimer ») : les compter tous les deux,
  // sinon la barre est dessinée 64 px trop à droite et déborde sur la barre de
  // sortie.
  const float kBarGap = 8.0f;
  float bar_w = 180.0f + kBarGap + 190.0f + kBarGap + 160.0f;  // + « Personnaliser »
  if (sel_occupied) {
    bar_w += kBarGap + 200.0f;                     // « Programmer » / « Annuler »
    if (pending) bar_w += kBarGap + 120.0f;        // + « Supprimer »
  }
  // Largeur MAXIMALE atteignable (perso sélectionné ET suppression programmée).
  // C'est elle — et non la largeur courante — qui décide du placement de la barre
  // de sortie : sinon celle-ci sauterait de ligne au gré des sélections.
  const float bar_max_w = 180.0f + kBarGap + 190.0f + kBarGap + 160.0f + kBarGap +
                          200.0f + kBarGap + 120.0f;

  // ── Géométrie de la barre de SORTIE (dessinée plus bas) ─────────────────────
  // Calculée ICI, avant le dessin de la barre d'action : c'est la comparaison des
  // deux rectangles qui décide s'ils tiennent sur la même ligne.
  const float kExitBackW = 190.0f, kExitQuitW = 160.0f;
  const float exit_bar_w = kExitBackW + kBarGap + kExitQuitW;
  // Point ancré (poignée déplaçable en mode édition) = coin GAUCHE-haut de la barre.
  const ImVec2 xp = Anchor("sortie", (disp.x - exit_bar_w - 24.0f) / disp.x,
                           (disp.y - 52.0f) / disp.y, seat_edit_);

  // Position ancrée (poignée déplaçable). Le point = milieu-haut de la barre.
  const ImVec2 bp = Anchor("boutons", 0.5f, (disp.y - 52.0f) / disp.y, seat_edit_);

  // En dessous d'environ 1650 px de large, la barre d'action pleine (suppression
  // programmée) et la barre de sortie ne tiennent PAS côte à côte : elles se
  // chevauchaient, « Supprimer » passant sous « Revenir au login ». On empile donc
  // la sortie une ligne au-dessus — le rang de droite y est libre (l'indicateur de
  // page est centré). En mode « Personnaliser » on n'y touche pas : le joueur doit
  // voir ses poignées là où il les pose.
  const float kRowH = 20.0f;  // hauteur d'un RoButton (btn_out_left.bmp)
  ImVec2 exit_pos = xp;
  if (!seat_edit_) {
    const float bmax_x0 = bp.x - bar_max_w * 0.5f;
    const float bmax_x1 = bmax_x0 + bar_max_w;
    const bool same_row = std::fabs(xp.y - bp.y) < kRowH;
    const bool overlap =
        bmax_x1 + kBarGap > xp.x && xp.x + exit_bar_w + kBarGap > bmax_x0;
    if (same_row && overlap) exit_pos.y = bp.y - (kRowH + 6.0f);
  }

  ImGui::SetCursorScreenPos(ImVec2(bp.x - bar_w * 0.5f, bp.y));

  const bool can_enter = can_enter_slot(selected_);
  if (!can_enter) ImGui::BeginDisabled();
  if (ro::RoButton(i18n::Tr("Entrer en jeu"), 180.0f, 0.0f)) EnterGame(selected_);
  if (!can_enter) ImGui::EndDisabled();

  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Mode Classique"), 190.0f, 0.0f)) {
    native_fallback_ = true;
    // Notre UI disparaît pour la session : le panneau de personnalisation avec
    // elle, et la mise en page en cours doit être gravée maintenant.
    seat_edit_ = false;
    charsel::SaveIfDirty();
  }

  // Personnalisation de l'écran (décor + placement). Bouton VISIBLE et non un
  // raccourci : c'est une fonctionnalité joueur, pas un outil d'auteur. F10 reste
  // en doublon pour qui l'a appris.
  //
  // ⚠ Pas de gate IsStaff() : le niveau de groupe serveur n'arrive qu'EN JEU
  // (setting id 26 sur la session map), il vaut 0 ici — un tel test masquerait le
  // bouton à tout le monde. Cf. le commentaire de l'ancien éditeur F10.
  ImGui::SameLine();
  if (ro::RoButton(seat_edit_ ? "Terminer" : "Personnaliser", 160.0f, 0.0f))
    ToggleLayoutEdit();

  // Suppression : réservation (pure ImGui) / annulation / suppression définitive.
  bool tip_sched = false, tip_cancel = false, tip_del = false;
  int del_remaining = 0;  // secondes avant que « Supprimer » soit possible (0 = OK)
  if (sel_occupied) {
    ImGui::SameLine();
    if (!pending) {
      if (ro::RoButton(i18n::Tr("Programmer suppression"), 200.0f, 0.0f)) {
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
      if (ro::RoButton(i18n::Tr("Annuler suppression"), 200.0f, 0.0f)) {
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
        ImGui::OpenPopup(i18n::Tr("Suppression définitive###bourgeon_charsel_delete"));
      }
      if (!del_elapsed) ImGui::EndDisabled();
      tip_del = ImGui::IsItemHovered();
    }
  }
  ImGui::PopStyleColor(2);
  // Tooltips APRÈS le pop du texte sombre (sinon texte sombre sur tooltip sombre).
  if (tip_sched)
    ImGui::SetTooltip(
        i18n::Tr("Planifie la suppression de ce personnage.\n"
        "- Il n'est PAS supprimé tout de suite : un délai serveur s'écoule d'abord\n"
        "  (le perso reste et se marque « Suppression programmée »).\n"
        "- Pendant ce délai tu peux ANNULER (bouton « Annuler suppression »).\n"
        "- Le délai écoulé, « Supprimer » finalise — IRRÉVERSIBLE.\n"
        "- Impossible si le perso est dans une GUILDE ou un GROUPE : le serveur\n"
        "  refuse, quitte-les d'abord."));
  if (tip_cancel)
    ImGui::SetTooltip(
        i18n::Tr("Annule la suppression planifiée : le personnage est conservé."));
  if (tip_del) {
    if (del_remaining > 0) {
      const int h = del_remaining / 3600, m = (del_remaining % 3600) / 60,
                s = del_remaining % 60;
      ImGui::SetTooltip(
          i18n::Tr("Suppression définitive indisponible : le délai serveur n'est pas\n"
          "écoulé. Disponible dans %02d:%02d:%02d."), h, m, s);
    } else {
      ImGui::SetTooltip(
          i18n::Tr("Supprime DÉFINITIVEMENT ce personnage. Action IRRÉVERSIBLE.\n"
          "Ouvre une confirmation : saisis l'EMAIL du compte (le serveur le\n"
          "vérifie ; c'est le même email pour tous tes personnages)."));
    }
  }

  // ── Barre de SORTIE (bas-droite) : revenir au login / quitter le jeu ─────────
  // Les deux issues du « Cancel » natif, séparées et explicites. Chacune ouvre sa
  // confirmation ImGui (jamais la msgbox native : elle est supprimée sous notre UI),
  // puis envoie la commande de mode (cf. kCmdBackToLogin / kCmdQuitGame).
  {
    // Ancre et largeurs : calculées plus haut avec la barre d'action (c'est là que
    // se décide l'empilement quand les deux barres ne tiennent pas sur une ligne).
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(28, 32, 44, 255));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(118, 124, 138, 255));
    ImGui::SetCursorScreenPos(exit_pos);
    const bool back_clicked = ro::RoButton(i18n::Tr("Revenir au login"), kExitBackW, 0.0f);
    const bool tip_back = ImGui::IsItemHovered();
    ImGui::SameLine(0.0f, kBarGap);
    const bool quit_clicked = ro::RoButton(i18n::Tr("Quitter le jeu"), kExitQuitW, 0.0f);
    const bool tip_quit = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    // Titres de popup distincts des labels de bouton (pas d'ID partagé dans la même
    // fenêtre) et plus explicites dans la barre de titre RO.
    if (back_clicked) ImGui::OpenPopup(i18n::Tr("Retour à l'écran de connexion###bourgeon_charsel_back"));
    if (quit_clicked) ImGui::OpenPopup("Fermer le jeu###bourgeon_charsel_quit");
    // Tooltips APRÈS le pop du texte sombre (sinon sombre sur sombre).
    if (tip_back)
      ImGui::SetTooltip(
          i18n::Tr("Se déconnecte du serveur et revient à l'écran de connexion,\n"
          "sans fermer le jeu."));
    if (tip_quit) ImGui::SetTooltip(i18n::Tr("Ferme le jeu."));
  }

  if (ro::BeginRoPopupModal(i18n::Tr("Retour à l'écran de connexion###bourgeon_charsel_back"))) {
    ImGui::TextUnformatted(
        i18n::Tr("Revenir à l'écran de connexion ?\n"
        "Tu seras déconnecté du serveur ; aucun personnage n'est affecté."));
    ImGui::Spacing();
    if (ro::RoButton(i18n::Tr("Revenir au login"), 170.0f, 0.0f)) {
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
      if (auth_) auth_->RearmWebLogin(/*service_select_pending=*/false);
      left_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  if (ro::BeginRoPopupModal("Fermer le jeu###bourgeon_charsel_quit")) {
    ImGui::TextUnformatted(i18n::Tr("Fermer le jeu ?"));
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
  if (ro::BeginRoPopupModal(i18n::Tr("Suppression définitive###bourgeon_charsel_delete"))) {
    ImGui::Text(i18n::Tr("Supprimer DÉFINITIVEMENT « %s » ?"), del_popup_name_);
    ImGui::TextUnformatted(i18n::Tr("Action IRRÉVERSIBLE. Saisis l'email du compte pour confirmer :"));
    ImGui::Spacing();
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool submit = ImGui::InputTextWithHint(
        "##del_email", i18n::Tr("email du compte"), del_email_, sizeof(del_email_),
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
    ImGui::OpenPopup(i18n::Tr("Créer un personnage###bourgeon_charsel_create"));
    create_open_req_ = false;
  }
  if (ro::BeginRoPopupModal(i18n::Tr("Créer un personnage###bourgeon_charsel_create"))) {
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
      ImGui::SetTooltip(i18n::Tr("Molette pour faire tourner le personnage"));
    }
    ImGui::SameLine();

    // Formulaire (droite) : nom, sexe, GRILLE d'icônes de coiffure, couleur.
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Nom");
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##cname", i18n::Tr("nom du personnage"), create_name_,
                             sizeof(create_name_));
    ImGui::TextUnformatted("Sexe");
    if (ro::RadioImage("Femelle", create_sex_ == 0)) create_sex_ = 0;
    ImGui::SameLine(0.0f, 18.0f);
    if (ro::RadioImage(i18n::Tr("Mâle"), create_sex_ == 1)) create_sex_ = 1;

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
    // du GRF — WYSIWYG fidèle, même chemin que la grille de coiffures. Seules les
    // cases VISIBLES chargent et recolorent (IsRectVisible) : le .spr n'est
    // analysé qu'une fois, mais chaque teinte coûte ses propres textures.
    // Clic = sélection.
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
      ImGui::TextColored(ImVec4(0.85f, 0.82f, 0.45f, 1.0f), i18n::Tr("Création en cours…"));
    else if (create_failed_) {
      const char* why = g_suppressed_modal_msg[0]
                            ? LocalToUtf8(g_suppressed_modal_msg)
                            : i18n::Tr("Échec : nom déjà pris, trop court ou caractères interdits.");
      ImGui::TextColored(ImVec4(0.96f, 0.52f, 0.52f, 1.0f), "%s", why);
    }

    const bool can_create =
        create_name_[0] != '\0' && create_slot_ >= 0 && !create_pending_;
    if (!can_create) ImGui::BeginDisabled();
    if (ro::RoButton(i18n::Tr("Créer"), 130.0f, 0.0f)) {
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

  // ── Menu contextuel du mode « Personnaliser » : pose du siège ────────────────
  // Clic droit sur un siège pendant l'édition. Le menu des coupons, lui, ne
  // s'ouvre qu'HORS édition : les deux ne peuvent pas se disputer le clic droit.
  if (seat_ctx_req_) {
    ImGui::OpenPopup("##seatpose");
    seat_ctx_req_ = false;
  }
  if (ImGui::BeginPopup("##seatpose")) {
    if (seat_ctx_ >= 0 && seat_ctx_ < kSeatCount) {
      charsel::Seat& s = Seats()[seat_ctx_];
      ImGui::Text("Place %d", seat_ctx_ + 1);
      ImGui::Separator();
      // Les quatre poses demandées. L'index est celui de l'action dans le .act
      // (cf. charsel::kAnim*) ; le composeur replie une action absente, donc un
      // sprite pauvre en actions retombe sur une pose voisine sans casser.
      // `animate` ne vaut que pour les actions que le composeur fait défiler
      // (Marche et Combat) : ailleurs, les images sont des poses, pas une boucle.
      struct PoseItem { int anim; bool animate; const char* label; };
      static const PoseItem kSeatPoses[] = {
          {charsel::kAnimSit,   false, "Assis"},
          {charsel::kAnimStand, false, "Debout"},
          {charsel::kAnimFight, false, "Combat"},
          {charsel::kAnimFight, true,  "Combat (animé)"},
          {charsel::kAnimWalk,  true,  "Marche (animée)"},
          {charsel::kAnimPick,  false, "Ramassage"},
          {charsel::kAnimAtk,   false, "Attaque"},
          {charsel::kAnimAtk,   true,  "Attaque (animée)"},
          {charsel::kAnimAtk2,  true,  "Attaque 2 (animée)"},
          {charsel::kAnimAtk3,  true,  "Attaque 3 (animée)"},
          {charsel::kAnimHurt,  false, "Touché"},
          {charsel::kAnimFroze, false, "Gelé"},
          {charsel::kAnimDie,   false, "Mort"},
          {charsel::kAnimExtra1, false, "Action 12"},
          {charsel::kAnimExtra2, false, "Action 13"},
      };
      for (const PoseItem& p : kSeatPoses) {
        const bool on = (s.anim == p.anim && s.animate == p.animate);
        if (ImGui::MenuItem(i18n::Tr(p.label), nullptr, on)) {
          s.anim = p.anim;
          s.animate = p.animate;
          // L'image choisie appartenait à l'action PRÉCÉDENTE : la nouvelle n'en
          // a pas forcément autant. On repart d'« Automatique ».
          s.frame = -1;
          charsel::MarkDirty();
        }
      }
      // Image de l'action. Une action en compte plusieurs — une attaque, un
      // ramassage passent par des postures que l'image 0 ne montre pas. Le
      // nombre proposé est celui RELEVÉ au rendu de cette place : il dépend de
      // la pose et de la classe, deux personnages n'ont pas forcément le même.
      const int nframes = seat_frames_[seat_ctx_];
      if (nframes > 1 && ImGui::BeginMenu("Image")) {
        if (ImGui::MenuItem("Automatique", nullptr, s.frame < 0)) {
          s.frame = -1;
          charsel::MarkDirty();
        }
        ImGui::Separator();
        for (int f = 0; f < nframes; ++f) {
          char lbl[24];
          std::snprintf(lbl, sizeof(lbl), "Image %d / %d", f + 1, nframes);
          if (ImGui::MenuItem(lbl, nullptr, s.frame == f)) {
            s.frame = f;
            charsel::MarkDirty();
          }
        }
        ImGui::EndMenu();
      }

      ImGui::Separator();
      // Tête tournée : les TROIS inclinaisons que porte le sprite de tête, celles
      // que `/doridori` fait alterner en jeu (cf. ro::DollDrawOpts::head_dir).
      if (ImGui::BeginMenu(i18n::Tr("Tête (doridori)"))) {
        struct HeadItem { int dir; const char* label; };
        static const HeadItem kHeads[] = {
            {0, "Droit devant"},
            {1, "Penchée à droite"},
            {2, "Penchée à gauche"},
        };
        for (const HeadItem& h : kHeads) {
          // -1 et 0 valent tous deux « dans l'axe » : la coche suit ce sens.
          const bool on = (h.dir == 0) ? (s.head_dir <= 0) : (s.head_dir == h.dir);
          if (ImGui::MenuItem(i18n::Tr(h.label), nullptr, on)) {
            s.head_dir = h.dir;
            charsel::MarkDirty();
          }
        }
        ImGui::EndMenu();
      }
      ImGui::Separator();
      ImGui::TextColored(ImVec4(0.36f, 0.38f, 0.42f, 1.0f),
                         "Molette : tourner — Ctrl+molette : taille");
      // Poser la même pose partout : replacer 25 sièges un par un après un
      // changement de décor est le geste le plus pénible du mode.
      if (ImGui::MenuItem(i18n::Tr("Appliquer cette pose à toutes les places"))) {
        // Copie locale : la boucle écrit dans le tableau dont `s` est une
        // référence, et écraserait la source dès la première place traitée.
        const charsel::Seat src = s;
        for (int k = 0; k < kSeatCount; ++k) {
          Seats()[k].anim = src.anim;
          Seats()[k].animate = src.animate;
          Seats()[k].head_dir = src.head_dir;
          Seats()[k].dir = src.dir;
          Seats()[k].frame = src.frame;
        }
        charsel::MarkDirty();
      }
    }
    ImGui::EndPopup();
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
      std::snprintf(mi, sizeof(mi), i18n::Tr("Changer de slot (%d)"), cv.moves_avail);
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
    ImGui::OpenPopup("Renommer le personnage###bourgeon_charsel_rename");
    rename_open_req_ = false;
  }
  if (ro::BeginRoPopupModal("Renommer le personnage###bourgeon_charsel_rename")) {
    ImGui::Text(i18n::Tr("Nom actuel : %s"), LocalToUtf8(rename_old_));
    ImGui::TextUnformatted(i18n::Tr("Nouveau nom"));
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##rname", i18n::Tr("nouveau nom"), rename_buf_,
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
      ImGui::TextColored(ImVec4(0.85f, 0.82f, 0.45f, 1.0f), i18n::Tr("Renommage en cours…"));
    else if (rename_failed_) {
      // Message EXACT du serveur (capturé depuis la modale native supprimée) s'il y en a
      // un ; sinon générique (cas timeout : le serveur n'a pas répondu = perso introuvable
      // ou refus silencieux). Le message natif est en code-page client -> LocalToUtf8.
      const char* why = g_suppressed_modal_msg[0]
                            ? LocalToUtf8(g_suppressed_modal_msg)
                            : i18n::Tr("Échec : nom pris/invalide, coupon absent, ou perso en "
                              "guilde/groupe (rename interdit).");
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
    ImGui::OpenPopup("Changer de slot###bourgeon_charsel_moveslot");
    move_open_req_ = false;
  }
  if (ro::BeginRoPopupModal("Changer de slot###bourgeon_charsel_moveslot")) {
    ImGui::TextUnformatted(i18n::Tr("Choisis un emplacement LIBRE :"));
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
        ImGui::CloseCurrentPopup();
      }
    }
    if (shown == 0) ImGui::TextDisabled(i18n::Tr("Aucun emplacement libre."));
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

  // ── Mode « Personnaliser » ───────────────────────────────────────────────────
  // F10 en doublon du bouton de la barre (l'ancien raccourci d'auteur, conservé).
  // Neutralisé pendant une saisie ou un popup : F10 n'est pas un caractère, mais
  // basculer le mode sous un dialogue de création laisserait le joueur devant deux
  // interactions concurrentes.
  if (ImGui::IsKeyPressed(ImGuiKey_F10, false) && !io.WantTextInput && !any_popup)
    ToggleLayoutEdit();

  // Aide au placement, PAR-DESSUS les sièges : lignes de tiers + rappel des gestes.
  if (seat_edit_) DrawLayoutGuides(dl, disp);

  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();  // WindowBg

  // Le panneau, lui, est une fenêtre à part ENTIÈRE, ouverte après la fermeture de
  // la fenêtre plein écran : déclarée en dernier, elle passe au-dessus (la
  // couverture est en NoBringToFrontOnFocus) et reste déplaçable, ce qu'un
  // BeginChild n'aurait pas permis.
  if (seat_edit_) DrawLayoutEditor();
}
