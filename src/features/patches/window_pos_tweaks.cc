#include "features/patches/window_pos_tweaks.h"

#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <climits>
#include <cstdint>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"  // full MoonlightUi type for SaveSettings()
#include "features/windows/inventory_viewer.h"  // hide-native-at-creation (id 8)
#include "features/windows/cart_viewer.h"  // hide-native-at-creation (cart id 0x28)
#include "features/windows/make_item_window.h"  // hide-native-at-creation (94 / 79)
#include "features/windows/npc_shop_window.h"  // comparateur ATK/DEF (id variable, par vtable)
#include "features/windows/vending_window.h"  // hide-native-at-creation (id 0x29/0xAE)
#include "features/windows/rodex_window.h"  // hide-native-at-creation (courrier 0x107/0x109)
#include "features/windows/character_sheet.h"  // hide-native-at-creation (grimoire 0x25)
#include "features/windows/pet_window.h"  // hide-native-at-creation (fiche pet 88 / menu 260)
#include "features/windows/game_menu.h"  // hide-native-at-creation (menu Échap 155)
#include "features/windows/game_settings.h"    // hide-native-at-creation (réglages 0x271E)
#include "features/windows/hotkey_settings.h"  // hide-native-at-creation (raccourcis 156)
#include "features/windows/macro_window.h"     // hide-native-at-creation (macros 86)
#include "features/windows/party_friend_window.h"  // hide-native-at-creation (amis/groupe 0x45)
#include "features/windows/chat_room_window.h"  // hide-native-at-creation (salon de chat 27)
#include "features/windows/navigation_window.h"  // hide-native-at-creation (navi 203 + 3)
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// ===========================================================================
// Generic UIWindow position persistence — see window_pos_tweaks.h for the why.
//
// Two halves, table-driven over {id, key}:
//   RESTORE (flicker-free): a single jmp-hook on UIWindowMgr_MakeWindow. After
//     the factory finishes building a tracked window (SetSize + SetPos-to-centre
//     + OnCreate all run INSIDE MakeWindow), we override its position with the
//     saved one BEFORE the function returns — i.e. before the window is ever
//     rendered. This works regardless of how a given window positions itself
//     internally (direct SetPos vs. msg 0x22); the earlier per-vtable msg-0x22
//     hook silently missed every window that centres via a direct SetPos in its
//     MakeWindow case (achievement, bank, mail, rodex) — they never get msg 0x22.
//   SAVE: OnTick reads the live position via FindWindow and persists drags.
// Status/Equip keep their own richer plugins (relayout + their own hooks).
// ===========================================================================

namespace {

// UIWindow base field layout (universal for every window class).

// MakeWindow is __thiscall(mgr, int windowID) returning the window (id = [ebp+8]).
using MakeWindow_t = void* (__fastcall*)(void*, void*, int);

// A saved coordinate is valid if it isn't the "unset" sentinel and isn't absurdly
// off-screen. NEGATIVE coords are legal (a window dragged partly off the left/top
// edge, e.g. x=-93) and MUST restore — an earlier `>= 0` guard wrongly refused
// them, so such a window saved its position but never came back to it.
constexpr int kOffscreenFloor = -2000;  // below this = garbage / fully off-screen

// Palier d'enregistrement, partagé par le moteur et par WindowPos_TrackLive.
// Un glisser produit une position par frame ; sans palier on réécrirait le yaml
// des dizaines de fois par seconde. À 200 ms l'emplacement final est écrit au
// plus 200 ms après le lâcher — et le tout premier mouvement part en ~100 ms,
// le battement d'OnTick.
constexpr uint32_t kWindowPosSaveThrottleMs = 200;
inline bool ValidPos(int x, int y) {
  return x != INT_MIN && y != INT_MIN && x > kOffscreenFloor && y > kOffscreenFloor;
}

// One tracked window. `key` is the yaml prefix: "<key>_pos_x" / "<key>_pos_y".
struct TrackedWindow {
  int         id;                    // FindWindow / MakeWindow key (UIWindow id)
  const char* key;                   // yaml key prefix
  int         pos_x = INT_MIN;        // saved x (INT_MIN = unset)
  int         pos_y = INT_MIN;        // saved y
  // OnTick runtime state:
  int         tracked_x = INT_MIN;    // last position we persisted
  int         tracked_y = INT_MIN;
  bool        was_open = false;       // window was open on the previous tick
  DWORD       last_save_ms = 0;          // GetTickCount of the last persist (throttle)
};

// ── THE TABLE ───────────────────────────────────────────────────────────────
// Add an entry here to make a window remember its position. `id` = the UIWindow
// id; `key` = a unique, stable yaml key prefix. Nothing else is required — the
// restore hook keys off the id, no per-window vtable/handler RE needed.
//
// IDs confirmed via Ghidra (20250716 client). Status (0xb) and Equip (0xa) are
// intentionally NOT here: they own a richer plugin (relayout + their own hooks).
TrackedWindow g_windows[] = {
    // {id,     "yaml key"}
    {0x10e, "achievement"},  // UIAchievementWnd (270), ctor 0x00778250, vt 0x01019584
    {0x113, "bank"},         // UIBank_NewWnd    (275), vt 0x01030fd4 — LIVE id (0x169 was wrong)
    {0x106, "mail"},         // UIOpenMailBoxWnd (262), ctor 0x0090cc70, vt 0x01039dac
    {0x107, "rodex"},        // UIRodexInboxWnd  (263), ctor 0x007cd7d0, vt 0x01022170
    // 🔴 RETIRÉE le 2026-08-29 : {0x16d, "party"} ne pouvait rien faire.
    // Deux raisons, chacune suffisante (cf. docs/native_window_dispatch.md §9) :
    //   — 365 > 0x16A, donc le switch de MakeWindow n'a AUCUN cas pour cet id ;
    //   — `UIPartyInfoWnd` (vtable 0x0101a040) n'est pas une fenêtre de premier
    //     plan mais un CONTRÔLE ENFANT, construit par UIAdvenPartyBoardWnd_OnCreate
    //     (id 324 / 0x144).
    // Le hook ne faisant rien sur un id absent, l'entrée était muette — c'est
    // exactement ce qui l'a laissée passer.
    // Confirmed native-persisting (do NOT add): Inventory 8, Storage 0x21, Quest
    // journal 0x141, Skill 0xc, Pet 0x58, Homun 0x71, Merc 0x7d, shops/vending.
    // Guild: id RÉSOLU au RTTI le 2026-08-29 — UIGuildTotalInfoWnd = 66 (0x42),
    // vtable 0x0103b5d0. Le conteneur 59 (0x3b) est un AIGUILLEUR (il fabrique
    // 0x3c + l'onglet actif lu en mgr+0x844), donc suivre 59 ne suivrait rien.
    // Ajouter {0x42, "guild"} le jour où on voudra la persistance de ce panneau.
};

constexpr int kWindowCount = static_cast<int>(sizeof(g_windows) / sizeof(g_windows[0]));

inline void* FindWin(int id) {
  return uiwnd::FindWindow(id);
}

inline void SetWinPos(void* win, int x, int y) {
  uiwnd::SetPos(win, x, y);  // le foyer fait deja la resolution de vtable
}

MakeWindow_t g_orig_makewindow = nullptr;  // trampoline to the real MakeWindow

// Replacement for UIWindowMgr_MakeWindow. Build the window normally, then — for a
// tracked id with a valid saved position — override its position before returning.
// MakeWindow does ALL of a window's open-time positioning internally (SetSize /
// centre SetPos / OnCreate), so overriding here is the last word before the first
// render → no flicker. Untracked windows pass straight through.
// 🔴 Profondeur d'imbrication du point d'entrée « Amis / Groupe » (id 0x45).
//
// Cette fenêtre a deux ids : 0x45 est le point d'entrée du JOUEUR, et il rappelle
// MakeWindow(0x22), la vraie fabrique. Or le client fabrique AUSSI la 0x22 tout
// seul (création d'un groupe, jonction) — et là, il veut afficher, pas basculer.
// Un compteur posé avant l'appel original suffit à séparer les deux cas, sans
// hook supplémentaire : pendant que 0x45 s'exécute, l'appel 0x22 qu'il déclenche
// est forcément imbriqué. Mono-thread (fil principal), donc pas d'atomique.
int g_party_entry_depth = 0;

// 🔴 Profondeur de la RESTAURATION DE LAYOUT du client, et pourquoi elle existe.
//
// À chaque entrée dans le monde — donc à CHAQUE CHANGEMENT DE MAP, pas seulement
// à la connexion — `CGameMode::EnterWorld` relit son blob de configuration
// d'interface et rejoue les fenêtres qui y sont marquées ouvertes, en appelant
// `MakeWindow(id)` pour chacune (cf. uiwnd::kRestoreWindowLayoutAddr).
//
// Ces créations-là ne sont NI un geste du joueur NI un événement de jeu : le
// client rejoue son propre état, et cet état ne fait plus autorité pour les
// fenêtres dont nous avons DÉTRUIT la native — c'est notre module qui sait si le
// joueur la veut ouverte. Sans ce compteur, la fenêtre Amis / Groupe se rouvrait
// à chaque map : le blob la disait ouverte, la rouvrir la faisait resauvegarder
// ouverte, et la boucle s'entretenait toute seule — le joueur ne pouvait plus
// jamais la fermer pour de bon.
//
// Même patron que `g_party_entry_depth` (compteur autour de l'appel original,
// mono-thread), avec un hook en plus parce que la restauration n'est pas
// imbriquée dans un MakeWindow : c'est elle qui les appelle.
int g_layout_restore_depth = 0;

// __thiscall(mgr, blob) -> int (nombre d'octets consommés, -1 si le blob n'est
// pas un layout). On ne touche à rien : on marque juste le contexte.
using RestoreWindowLayout_t = int (__fastcall*)(void*, void*, void*);
RestoreWindowLayout_t g_orig_restore_layout = nullptr;

int __fastcall RestoreWindowLayoutHook(void* mgr, void* edx, void* blob) {
  ++g_layout_restore_depth;
  const int consumed = g_orig_restore_layout(mgr, edx, blob);
  --g_layout_restore_depth;
  return consumed;
}

void* __fastcall MakeWindowHook(void* mgr, void* edx, int windowID) {
  // ⚠ AVANT l'appel original : c'est LUI qui déclenche l'appel imbriqué.
  const bool party_entry = (windowID == 0x45);
  if (party_entry) ++g_party_entry_depth;
  void* win = g_orig_makewindow(mgr, edx, windowID);
  if (party_entry) --g_party_entry_depth;
  if (win) {
    for (auto& w : g_windows) {
      if (w.id != windowID) continue;
      if (ValidPos(w.pos_x, w.pos_y)) {
        __try {
          SetWinPos(win, w.pos_x, w.pos_y);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
      }
      break;
    }
    // 🔴 Ne routent RIEN ici, et c'est voulu : entrepôt (0x21), banque (275), cash
    // shop (0x13e), shop NPC (0x16-0x19), dialogue NPC (0x10/0x11/0x38/0x64/0xe2),
    // refine d'arme (111) et échange joueur. Leurs créateurs natifs sont tous
    // revendiqués — ces fenêtres ne NAISSENT plus. Les masquer à la naissance serait
    // même nuisible (une native invisible garde le clavier) et les détruire est
    // exclu depuis l'intérieur de MakeWindow, dont l'appelant déréférence le retour.
    // Ne restent donc ci-dessous que les fenêtres qui naissent encore pour de bon.
    //
    // Inventaire (id 8) et cart (UICartWnd id 0x28) : remplacés par leurs viewers.
    // Leur création EST la demande du joueur — ces deux fenêtres ne naissent que
    // d'une action, jamais d'un paquet (ZC_INVENTORY_START ne fait que vider leurs
    // listes). Le viewer bascule ici, et OnTick détruit la native.
    if (windowID == 8) {
      if (auto* iv = Bourgeon::Instance().inventory_viewer())
        iv->HandleNativeCreation(win);
    }
    if (windowID == 0x28) {
      if (auto* cv = Bourgeon::Instance().cart_viewer())
        cv->HandleNativeCreation(win);
    }
    // Sertissage de cartes (UIItemCompositionWnd id 0x4A) : FILET DE SÉCURITÉ. Son
    // unique créateur était le handler de ZC 0x017B, dont le viewer a pris la place —
    // en mode moderne ce popup ne naît donc plus du tout. S'il naissait quand même
    // (bascule de mode en plein sertissage), on le masque avant sa première frame et
    // OnTick le détruit.
    if (windowID == 0x4A) {
      if (auto* iv = Bourgeon::Instance().inventory_viewer())
        iv->HandleCardInsertCreation(win);
    }
    // Les fenêtres que la feuille de personnage remplace : Grimoire
    // (UINewSkillListWnd 0x25), Équipement (0xa), Status (UIStatusWnd 0xb) et la
    // GUILDE — 0x3B quand on en a une, 0xD4 quand on n'en a pas (les deux chemins
    // d'ouverture tranchent sur `g_Own_GuildId`, cf. character_sheet.cc). On masque
    // la native dès sa création et on route la demande vers l'onglet correspondant ;
    // OnTick la détruit ensuite. Raccourcis et boutons du menu d'icônes continuent
    // donc de marcher, mais atterrissent sur l'interface moderne (cf.
    // docs/skill_tree_re.md partie II).
    //
    // 🔴 C'est bien MakeWindow le point d'interception, et un seul suffit : les deux
    // chemins — UIWindowMgr_ToggleWindowById (0x00812e60) pour les boutons,
    // UIWindowMgr_DispatchHotkeyBehavior (0x00a451e0) pour les raccourcis — ferment
    // la fenêtre si elle existe et ne la créent que sinon. Comme celles-ci sont
    // détruites, elles n'existent jamais : toute demande repasse forcément ici.
    //
    // Les panneaux d'onglet du conteneur de guilde (0x3c..0x42) passent aussi par
    // ici : ils ne routent rien (le `default` de la fonction), mais il faut les
    // masquer à la naissance comme les autres — le conteneur en crée un d'office,
    // et sans ça sa frame native passe à l'écran.
    //
    // S'y ajoutent les deux fenêtres de l'HOMONCULE : 0x71 (UIHomunInfoWnd, la fiche
    // d'état, raccourci Alt+R) et 0x72 (UISkillListWnd en mode homoncule, son arbre
    // de compétences). L'onglet Homoncule les fusionne, comme l'onglet du mannequin
    // fusionne Status et Équipement. Cf. docs/homunculus_re.md.
    if (windowID == 0x25 || windowID == 0x0a || windowID == 0x0b ||
        windowID == 0x3b || windowID == 0xd4 ||
        windowID == 0x71 || windowID == 0x72 ||
        (windowID >= 0x3c && windowID <= 0x42)) {
      if (auto* cs = Bourgeon::Instance().character_sheet())
        cs->HandleReplacedNativeCreation(win, windowID);
    }
    // Le MENU ÉCHAP (« Game Options », UIEscOptionWnd id 155). Son unique
    // constructeur est le case 155 de CETTE fonction (0x00A3F24D) : comme GameMenu
    // DÉTRUIT la fenêtre au tick, elle n'existe jamais, donc toute demande
    // d'ouverture — touche Échap comme bouton 193 de la barre d'icônes — repasse
    // forcément ici. Un seul point d'interception suffit, et il n'y a pas de
    // deuxième chemin à couvrir. Cf. docs/game_option_re.md §5.2.
    if (windowID == 155) {
      if (auto* gm = Bourgeon::Instance().game_menu())
        gm->HandleNativeCreation(win);
    }
    // La fenêtre AMIS / GROUPE (UIMessengerGroupWnd), la classe à deux onglets qui
    // rend les deux listes.
    //
    // 🔴 C'EST 0x22, PAS 0x45 — cette fenêtre a DEUX ids et ils ne servent pas à la
    // même chose :
    //   · 0x22 = l'id de FABRIQUE. Le case 34 @0x00a3aeac est le seul endroit du
    //     binaire qui construise la classe (le ctor 0x00701fc0 n'a qu'UN appelant,
    //     cette fonction) ;
    //   · 0x45 = l'id d'ENREGISTREMENT, celui que FindWindow / CloseWindow prennent.
    //     Son case 69 @0x00a3ae55 n'est qu'un POINT D'ENTRÉE : il rappelle
    //     MakeWindow(0x22), puis envoie OnMsg(6, 0xD7, …) pour choisir l'onglet
    //     selon `mgr+0x740`, et sort par le `default` — il ne RETOURNE donc aucune
    //     fenêtre. Un hook posé sur 0x45 ne verrait jamais `win`.
    // Comme MakeWindow(0x45) rappelle MakeWindow(0x22), notre hook est réentrant et
    // c'est la passe 0x22 (imbriquée) qui porte la vraie fenêtre.
    // Cf. docs/party_friend_re.md §7.
    // `g_party_entry_depth > 0` = cet appel 0x22 est IMBRIQUÉ dans un 0x45, donc
    // il vient d'un geste du joueur (bouton « party », raccourci) -> bascule.
    // À plat, c'est le client qui fabrique la fenêtre pour la peupler (création
    // de groupe, jonction) -> on ouvre, on ne bascule pas.
    // `g_layout_restore_depth > 0` = ni l'un ni l'autre : le client rejoue son
    // layout à l'entrée dans le monde -> on masque la native et on ne touche PAS
    // à notre état, sinon la fenêtre se rouvre à chaque changement de map.
    if (windowID == 0x22) {
      if (auto* pf = Bourgeon::Instance().party_friend_window())
        pf->HandleNativeCreation(win, g_party_entry_depth > 0,
                                 g_layout_restore_depth > 0);
    }
    // « Create Chat Room » (UIChatRoomMakeWnd id 27 / 0x1B). Même raisonnement que
    // les précédentes, avec une raison de plus de ne PAS se contenter de masquer :
    // son OnCreate termine par `this[35] = 184`, c'est-à-dire « bouton par défaut
    // = OK ». Invisible mais vivante, elle CRÉERAIT un salon sur une frappe
    // d'Entrée destinée à notre champ de saisie (le piège de la banque).
    //
    // Un seul point d'interception suffit : les trois chemins d'ouverture —
    // /chat, le bouton 214 de UIBasicInfoWnd et l'action TalkType 0x17
    // (ChatRoom_OpenMakeWnd) — passent tous par MakeWindow(0x1B), et leurs gardes
    // (échoppe en cours, déjà dans un salon) ont déjà joué avant d'arriver ici.
    // Cf. docs/chat_room_re.md §2 et §12.2.
    if (windowID == 27) {
      if (auto* cr = Bourgeon::Instance().chat_room_window())
        cr->HandleNativeCreation(win);
    }
    // La SALLE (UIChatRoomWnd id 28). ⚠ Contrairement à toutes les précédentes,
    // celle-ci ne naît pas d'une demande du JOUEUR mais d'un ÉVÉNEMENT : l'ACK de
    // création (0x00CA17D2) et l'arrivée d'un membre (ZC_MEMBER_NEWENTRY, qui
    // appelle MakeWindow(0x1C) lui-même) la fabriquent. Le module ne bascule donc
    // pas ici — il ouvre la sienne si elle ne l'est pas déjà.
    if (windowID == 28) {
      if (auto* cr = Bourgeon::Instance().chat_room_window())
        cr->HandleNativeRoomCreation(win);
    }
    // « Réglages du salon » (UIChatRoomChangeWnd id 30). Elle dérive de la 27 et
    // partage son OnCreate — donc son bouton par défaut ENVOIE lui aussi. Elle
    // n'a de toute façon plus de source : c'est le natif de la fenêtre 28, morte,
    // qui lui poussait son `msg 34`. Notre formulaire la remplace, pré-rempli.
    if (windowID == 30) {
      if (auto* cr = Bourgeon::Instance().chat_room_window())
        cr->HandleNativeChangeCreation(win);
    }
    // « Veuillez saisir le mot de passe » (UIPasswordWnd id 29), au clic sur le
    // panneau d'un salon PRIVÉ. ⚠ Elle ne porte pas encore l'identifiant du salon
    // à cet instant : son ouvreur le lui pose juste après, par un `msg 47`. Le
    // module se contente donc de la masquer et relit son `+0xBC` au tick.
    if (windowID == 29) {
      if (auto* cr = Bourgeon::Instance().chat_room_window())
        cr->HandleNativePasswordCreation(win);
    }
    // La table des raccourcis (UIHotKeyWnd id 156). Même raisonnement que le menu
    // Échap ci-dessus : elle est détruite au tick, donc toute demande repasse ici.
    // ⚠ HotkeySettings la rouvre LUI-MÊME pour le remappage (les ponts Lua
    // d'écriture ne sont pas encore RE'd) : dans ce cas il neutralise ce hook,
    // sinon il basculerait son propre panneau au lieu de laisser la native vivre.
    if (windowID == 156) {
      if (auto* hs = Bourgeon::Instance().hotkey_settings())
        hs->HandleNativeCreation(win);
    }
    // La liste des MACROS de chat (« Shortcut List », UIEmotionWnd id 86, Alt+M).
    // Même raisonnement encore : détruite au tick, donc jamais existante, donc son
    // unique chemin d'ouverture — le comportement de raccourci 114 de
    // `UIWindowMgr_DispatchHotkeyBehavior` — repasse forcément par la fabrique.
    // ⚠ NE PAS confondre avec `UIMacroRegisterWnd` (0x11E) : malgré son nom, c'est
    // l'anti-bot, et aucun raccourci ne l'ouvre. Cf. docs/shortcut_list_re.md.
    if (windowID == 86) {
      if (auto* mw = Bourgeon::Instance().macro_window())
        mw->HandleNativeCreation(win);
    }
    // Les réglages du jeu (CUIGameSettingsUI id 0x271E). FILET DE SÉCURITÉ, et
    // rien de plus : notre menu Échap ouvre directement le panneau ImGui sans
    // passer par la fabrique. Ce hook ne sert donc que si le joueur a remis le
    // menu Échap natif tout en gardant nos réglages — son bouton « game settings »
    // fabrique alors la native, qu'on masque ici et que OnTick détruit.
    // ⚠ GameSettings la rouvre LUI-MÊME pour l'onglet Graphismes (reset de device,
    // non porté) : il neutralise ce hook le temps de la créer.
    if (windowID == 0x271E) {
      if (auto* gs = Bourgeon::Instance().game_settings())
        gs->HandleNativeCreation(win);
    }
    // Le PET : la fiche (UIPetInfoWnd id 88), le menu de commandes qu'elle
    // ouvrait (UIMenuWnd id 260) et la fenêtre d'évolution (UIPetEvolutionWnd
    // id 261), fusionnés par PetWindow. Les deux premières naissent
    // encore pour de bon — la 88 par le menu contextuel du pet OU, sans que le
    // joueur demande rien, par `GameMode_PetIntimacyWarnAndOpenInfo` quand
    // l'intimité tombe sous 100. C'est justement pour ça qu'il faut la DÉTRUIRE
    // et non la masquer : masquée, elle serait rouverte en boucle, invisible et
    // toujours vivante. Cf. docs/pet_re.md §5.4 et §12.1.
    if (windowID == 88 || windowID == 260 || windowID == 261) {
      if (auto* pw = Bourgeon::Instance().pet_window())
        pw->HandleNativeCreation(win, windowID);
    }
    // Fabrication : les DEUX listes natives (UIMakingArrowListWnd id 94 « LIST »
    // et UIMakeTargetListWnd id 79 « Manufacturing List »). Elles naissent dans un
    // handler de paquet, donc ENTRE deux OnTick — sans ce hook une frame native
    // passerait à l'écran.
    // La 80 (choix des matériaux) est masquée DEPUIS QU'ON NE L'OUVRE PLUS : la
    // commande 130 prend les trois matériaux en paramètre, le plugin les
    // collecte lui-même, et toute apparition de cette fenêtre est désormais un
    // résidu qui venait s'intercaler entre deux fabrications d'une série.
    if (windowID == 94 || windowID == 79 || windowID == 80) {
      if (auto* mk = Bourgeon::Instance().make_item_window())
        mk->HideNativeAtCreation(win);
    }
    // La NAVIGATION : la principale (UINavigationV4Wnd 203) et ses trois
    // satellites — itinéraire (314), sélecteur d'icône de trace (306) et aide
    // (229). Elle naît encore pour de bon : bouton 430 du menu d'icônes,
    // raccourci clavier, ou le moteur lui-même. Sa création EST donc la demande
    // du joueur, et le panneau bascule ici pendant que OnTick détruit.
    //
    // 🔴 Elle n'émet AUCUN paquet — vérifié sur toute la plage de sa classe —
    // donc ni devoir de naissance ni devoir de mort à rejouer, contrairement à
    // la rédaction de courrier (0x108). La détruire au tick est sans effet de
    // bord.
    //
    // Les trois satellites ne naissent que sur ordre de la 203, qui n'existe
    // plus : leur cas est un filet de sécurité, et il ne route rien.
    if (windowID == 203 || windowID == 306 || windowID == 314 ||
        windowID == 229) {
      if (auto* nav = Bourgeon::Instance().navigation_window())
        nav->HandleNativeCreation(win, windowID);
    }
    // Échoppe joueur : la COMPOSITION (vente 0x29, achat 0xAE) ET la grille des
    // objets disponibles (0x2A/0xAF) sont remplacées par VendingWindow, qui les
    // fusionne en une seule fenêtre ImGui.
    // 0x2D / 0xB0 = « My Shop » (UIMerchantItemMyShopWnd), ouverte APRÈS le
    // lancement de l'échoppe — elle a son propre cycle de vie dans le plugin.
    // 0x101 / 0x102 = « Item Sell History » (UIMerchantItemLogWnd), ouverte par la
    // fermeture de la boutique.
    // 0x2B / 0x2C = côté ACHETEUR (UIMerchantItemShopWnd = l'offre du vendeur,
    // UIMerchantItemPurchaseWnd = le panier), ouvertes en cliquant sur l'échoppe
    // d'un autre joueur.
    // 0xB1 / 0xB2 / 0xB3 = les MÊMES classes quand on VEND à un buying store
    // (recherche / vente / stock proposable) — trois fenêtres cette fois.
    if (windowID == 0x29 || windowID == 0x2A || windowID == 0xAE ||
        windowID == 0xAF || windowID == 0x2D || windowID == 0xB0 ||
        windowID == 0x2B || windowID == 0x2C ||
        windowID == 0xB1 || windowID == 0xB2 || windowID == 0xB3 ||
        windowID == 0x101 || windowID == 0x102) {
      if (auto* vt = Bourgeon::Instance().vending_window())
        vt->HideNativeAtCreation(win);
    }
    // Comparateur ATK/DEF (UIItemParamChangeDisplayWnd) : id variable, créé par le
    // handler d'achat natif -> détecté par vtable (no-op hors session shop).
    if (auto* sh = Bourgeon::Instance().npc_shop_window())
      sh->HideDetailWindow(win);
    // Courrier RODEX : la LISTE (0x107) est créée sur commande du joueur, mais la
    // LECTURE (0x109) et l'ÉCRITURE (0x108) le sont par des handlers de paquet
    // (ZC 0x0B63 et l'ack « commencer un courrier »), donc entre deux OnTick ->
    // sans ce hook une frame native passerait à l'écran.
    if (windowID == 0x107 || windowID == 0x108 || windowID == 0x109) {
      if (auto* rodex = Bourgeon::Instance().rodex_window())
        rodex->HideNativeAtCreation(win, windowID);
    }
  }
  return win;
}

// ── Snap hidden-window filter (native drag-snap fix) ─────────────────────────
// Injected into the native snap calculator FUN_00a32eb0 at 0x00a33005 — the
// point in its per-candidate loop right before it computes the align offset to a
// candidate window (candidate window ptr = ESI, set at 0x00a32fc7). The native
// loop NEVER checks whether the candidate is visible (UIWnd_SetVisible 0x009030c0
// stores the visible flag at window+0x28; the snap ignores it), so a HIDDEN
// window that still has an on-screen rect (the top-left "ghost" HUD pieces)
// magnetises every dragged window onto it. This trampoline skips a candidate
// whose +0x28 visible flag is 0 (jump to the loop's iterator-advance at
// 0x00a3304b); otherwise it re-runs the overwritten `MOV ECX,[EBP-0x88]` and
// falls back into the loop body at 0x00a3300b. Net effect: visible windows still
// snap to each other (the feature players like), hidden windows never do.
//
// Returns non-zero to SKIP this snap candidate (do not magnetise onto it). A
// candidate is skipped when it is NOT a real on-screen window: hidden (the
// UIWnd_SetVisible flag at +0x28 is 0) OR pinned off-screen (live x/y <= -2000,
// e.g. a HUD piece parked at -10000). Live diagnosis (2026-07-04) confirmed the
// top-left "ghost" is exactly such a window — present in the snap set with a
// stale on-screen rect — so both conditions are needed to kill it while leaving
// legitimate window-to-window snapping intact.
int SnapDecideSkip(void* window) {
  auto* b = static_cast<char*>(window);
  const int vis = *reinterpret_cast<int*>(b + uiwnd::kOffVisible);
  int x = 0, y = 0;
  uiwnd::LivePos(window, &x, &y);
  if (vis == 0) return 1;                 // hidden window (the ghost)
  if (x <= -2000 || y <= -2000) return 1; // pinned off-screen (e.g. -10000)
  return 0;                               // real on-screen window -> snap allowed
}

// Trampoline hooked into FUN_00a32eb0 at 0x00a33005 (candidate window = ESI). It
// asks SnapDecideSkip whether to skip this candidate; if so it jumps to the loop's
// iterator-advance (0x00a3304b), otherwise it re-runs the overwritten
// `MOV ECX,[EBP-0x88]` and falls back into the loop body (0x00a3300b).
// EAX/ECX/EDX are caller-saved (the loop reloads them); ESI/EBP survive the cdecl
// call, so SnapDecideSkip(esi) is safe. push/ret = absolute jump, esp-neutral.
__declspec(naked) void SnapHiddenFilter() {
  __asm {
    push esi                 // arg: candidate window
    call SnapDecideSkip
    add  esp, 4
    test eax, eax
    jnz  skip
    mov  ecx, [ebp-88h]      // replicate the overwritten MOV ECX,[EBP-0x88]
    push 0a3300bh            // -> loop body: compute snap to this window
    ret
  skip:
    push 0a3304bh            // -> iterator++ : skip this candidate
    ret
  }
}

}  // namespace

WindowPosTweaks::WindowPosTweaks() {
  using namespace hooking;
  g_orig_makewindow = reinterpret_cast<MakeWindow_t>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(uiwnd::kMakeWindowAddr),
          reinterpret_cast<uint8_t*>(&MakeWindowHook)));
  // LogInfo("[WinPos] tracking {} window(s); MakeWindow restore hook {}",
          // kWindowCount, g_orig_makewindow ? "installed" : "FAILED");

  // ── Marquage du contexte « le client rejoue son layout » ────────────────────
  // Ce hook ne change RIEN au comportement du client : il pose un compteur le
  // temps de la restauration, pour que le hook de MakeWindow ci-dessus sache que
  // les fenêtres qui naissent là ne viennent de personne. Sans lui, un module
  // qui a détruit sa native ne peut pas distinguer une ouverture VOULUE d'un
  // rejeu d'état — et se rouvre à chaque changement de map.
  g_orig_restore_layout = reinterpret_cast<RestoreWindowLayout_t>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(uiwnd::kRestoreWindowLayoutAddr),
          reinterpret_cast<uint8_t*>(&RestoreWindowLayoutHook)));

  // ── Disable native window dock-SNAP (kills the invisible "ghost" magnetism) ──
  // The base window-move FUN_00880e00 branches into its dock-SNAP path when the
  // dragged window belongs to a dock group (FUN_007a8bb0 test), via `JNZ 0x00880e9f`
  // at 0x00880e7f. That path magnetises the window to the group's bounding box —
  // which, because the hidden Basic Info + Status + shortcut bar form a top-left
  // dock group, is the invisible "ghost" other windows stuck to. Live-confirmed via
  // x32dbg: the snap query FUN_007a8840 is reached ONLY from this branch (return
  // 0x00880ed2). NOP-ing the JNZ forces the plain FREE-move branch → no snap, no
  // ghost, dragging otherwise unchanged. (Also disables docked children following a
  // dragged parent — fine here; those are hidden/HUD windows.)
  constexpr uintptr_t kSnapJnz = 0x00880e7f;  // JNZ 0x00880e9f  (bytes 75 1e)
  DWORD old;
  auto* p = reinterpret_cast<uint8_t*>(kSnapJnz);
  if (VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) {
    if (p[0] == 0x75 && p[1] == 0x1e) { p[0] = 0x90; p[1] = 0x90; }  // guard exact JNZ
    VirtualProtect(p, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
    // LogInfo("[WinPos] window dock-snap {}", (p[0] == 0x90) ? "disabled" : "UNCHANGED");
  }

  // ── Fix the native drag-snap: skip HIDDEN candidate windows ─────────────────
  // The snap the player sees is applied by a shared vtable method at 0x008818a0
  // (found live via SnapDiag, 2026-07-04): it computes a magnetic-align delta via
  // FUN_00a32eb0 (align to screen edges AND to every other window's rect) and adds
  // it to the dragged window's live +0x1c/+0x20. The delta computer never checks
  // visibility, so a HIDDEN HUD window with an on-screen rect (the top-left
  // "ghost") is a snap target → every window "bounced" onto it (Y forced to 134,
  // live-confirmed). We used to NOP the two ADDs in 0x008818a0, but that killed
  // ALL snapping (players want window-to-window snap) and broke a native alt-tab
  // window-clamp loop. Instead we hook FUN_00a32eb0's candidate loop to skip
  // non-visible windows (see SnapHiddenFilter): legitimate snap stays, ghost dies.
  constexpr uintptr_t kSnapLoopHook = 0x00a33005;  // MOV ECX,[EBP-0x88] (8B 8D 78 FF FF FF)
  DWORD old2;
  auto* h = reinterpret_cast<uint8_t*>(kSnapLoopHook);
  if (VirtualProtect(h, 6, PAGE_EXECUTE_READWRITE, &old2)) {
    if (h[0] == 0x8B && h[1] == 0x8D && h[2] == 0x78 &&
        h[3] == 0xFF && h[4] == 0xFF && h[5] == 0xFF) {  // guard exact bytes
      const int32_t rel = static_cast<int32_t>(
          reinterpret_cast<uintptr_t>(&SnapHiddenFilter) - (kSnapLoopHook + 5));
      h[0] = 0xE9;                                    // JMP rel32 -> SnapHiddenFilter
      *reinterpret_cast<int32_t*>(h + 1) = rel;
      h[5] = 0x90;                                    // NOP-pad the 6th byte
    }
    VirtualProtect(h, 6, old2, &old2);
    FlushInstructionCache(GetCurrentProcess(), h, 6);
    // LogInfo("[WinPos] snap hidden-window filter {}", (h[0] == 0xE9) ? "installed" : "UNCHANGED");
  }
}

// SAVE-ONLY. Restoration is done entirely by the pre-render MakeWindow hook above
// (no repositioning here — that is what caused the visible flicker). Each tick we
// read the live position and persist genuine moves. On the close->open edge we
// re-baseline WITHOUT saving: the hook has already placed the window at its saved
// spot, so baselining to it means only later drags are recorded (never the restored
// value re-saved, and never a clobber). FindWindow is null while closed.
void WindowPosTweaks::OnTick() {
  bool dirty = false;
  for (auto& w : g_windows) {
    void* win = FindWin(w.id);
    if (!win) {            // closed: force a re-baseline on the next open
      w.was_open = false;
      continue;
    }

    int liveX = 0, liveY = 0;
    uiwnd::LivePos(win, &liveX, &liveY);

    if (!w.was_open) {  // just opened — the hook already positioned it; baseline
      w.tracked_x = liveX;
      w.tracked_y = liveY;
      if (!ValidPos(w.pos_x, w.pos_y)) {  // first-ever use: seed the yaml with the spot
        w.pos_x = liveX;
        w.pos_y = liveY;
      }
      w.was_open = true;
      continue;
    }

    // Persist genuine moves (drag), throttled (cf. kWindowPosSaveThrottleMs).
    if ((liveX != w.tracked_x || liveY != w.tracked_y) &&
        GetTickCount() - w.last_save_ms >= kWindowPosSaveThrottleMs) {
      w.pos_x = liveX;
      w.pos_y = liveY;
      w.tracked_x = liveX;
      w.tracked_y = liveY;
      w.last_save_ms = GetTickCount();
      dirty = true;
    }
  }
  if (dirty) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  }
}

// ── Enumeration API for MoonlightUi Save/LoadSettings ───────────────────────
int         WindowPosTweaks_Count() { return kWindowCount; }
const char* WindowPosTweaks_Key(int i) { return g_windows[i].key; }
int         WindowPosTweaks_X(int i) { return g_windows[i].pos_x; }
int         WindowPosTweaks_Y(int i) { return g_windows[i].pos_y; }

void WindowPosTweaks_SetSavedPos(int i, int x, int y) {
  // Just load the saved position. The MakeWindow hook applies it on the next open.
  g_windows[i].pos_x = x;
  g_windows[i].pos_y = y;
}

// ── Le corps commun des deux détours de handler ──────────────────────────────
// Cf. l'en-tête pour le pourquoi (et pour la migration qui reste à trancher).
//
// Deux moments, et un seul suffit rarement :
//   · la CROIX (msg 6, sous-commande 0xc9) : on relève la position VIVANTE avant
//     de laisser le natif fermer, puis on demande l'écriture. C'est le seul
//     instant où elle est encore lisible ;
//   · la RESTAURATION DE DISPOSITION (msg 0x22) : on réimpose la position
//     enregistrée APRÈS le natif. On écrase, donc son validateur et sa valeur
//     par défaut n'ont plus d'importance.
//
// ⚠ Le handler d'équipement se ré-envoie msg 6/0xca à lui-même depuis le cas
// 0x22 ; les deux gardes ci-dessous (0xc9 et 0x22) l'ignorent, la réentrance est
// donc inoffensive.
int WindowPos_PersistOnMsg(void* self, void* edx, int arg0, int msg, int p2,
                           int p3, int p4, int p5, WindowPosMsgFn orig,
                           int* saved_x, int* saved_y,
                           bool (*applies)(void*)) {
  constexpr int kMsgCmd     = 6;     // message de commande
  constexpr int kSubClose    = 0xc9;  // sous-commande « fermer » du msg 6
  if (orig == nullptr) return 0;
  // ⚠ Le filtre est ré-évalué APRÈS l'appel natif, et ce n'est pas un oubli : les
  // deux détours d'origine le faisaient ainsi. Le mémoriser une fois serait
  // presque toujours équivalent — le drapeau de mode est posé à la création de la
  // fenêtre — mais « presque toujours » n'est pas la garantie qu'on veut dans un
  // détour de handler.
  __try {
    if (msg == kMsgCmd && p2 == kSubClose &&
        (applies == nullptr || applies(self))) {
      uiwnd::LivePos(self, saved_x, saved_y);
      const int r = orig(self, edx, arg0, msg, p2, p3, p4, p5);
      if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
      return r;
    }
    const int r = orig(self, edx, arg0, msg, p2, p3, p4, p5);
    if (msg == uiwnd::kMsgRestore && (applies == nullptr || applies(self)) &&
        *saved_x != INT_MIN && *saved_x >= 0 && *saved_y >= 0) {
      uiwnd::SetPos(self, *saved_x, *saved_y);
    }
    return r;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}


// ── Le corps commun des deux suivis au tick ──────────────────────────────────
// Cf. l'en-tête. Il était écrit deux fois, à l'identique, dans EquipTweaks::
// OnTick et StatusTweaks::OnTick — et une troisième fois, sous une autre forme,
// dans le OnTick du moteur ci-dessus. Les deux plugins l'appellent maintenant.
//
// ⚠ Ce n'est PAS le OnTick du moteur : celui-ci ne restaure rien (son hook de
// MakeWindow s'en charge avant le premier rendu), là où les deux « one-off »
// n'ont pas ce hook et doivent forcer la position depuis le tick.
void WindowPos_TrackLive(int window_id, WindowPosTracker* tracker,
                         int* saved_x, int* saved_y, bool* restore_pending) {
  void* win = uiwnd::FindWindow(window_id);
  if (!win) return;  // fenêtre fermée : les deux entiers gardent le dernier spot

  // Une position vient d'être lue du yaml : on la force UNE fois sur la fenêtre
  // vivante, puis on repasse en suivi. Le client rouvre ses fenêtres à leur
  // emplacement natif en dur, donc sans ce forçage la valeur chargée serait
  // écrasée par la lecture ci-dessous, puis l'emplacement natif réenregistré
  // par-dessus. C'est ce qui fait survivre la position à un redémarrage complet.
  if (*restore_pending) {
    uiwnd::SetPos(win, *saved_x, *saved_y);
    tracker->tracked_x = *saved_x;
    tracker->tracked_y = *saved_y;
    tracker->baselined = true;
    *restore_pending = false;
    return;
  }

  int live_x = 0, live_y = 0;
  uiwnd::LivePos(win, &live_x, &live_y);

  // Rien à restaurer : on prend la référence sur l'emplacement courant et on y
  // amorce la valeur persistée, mais SANS demander d'écriture disque — c'est
  // l'emplacement natif, pas un choix du joueur. Il ne partira au yaml que si
  // le joueur déplace la fenêtre, ou à la prochaine écriture provoquée par
  // autre chose.
  if (!tracker->baselined) {
    tracker->tracked_x = live_x;
    tracker->tracked_y = live_y;
    *saved_x = live_x;
    *saved_y = live_y;
    tracker->baselined = true;
    return;
  }

  if ((live_x != tracker->tracked_x || live_y != tracker->tracked_y) &&
      GetTickCount() - tracker->last_save_ms >= kWindowPosSaveThrottleMs) {
    *saved_x = live_x;
    *saved_y = live_y;
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    tracker->tracked_x = live_x;
    tracker->tracked_y = live_y;
    tracker->last_save_ms = GetTickCount();
  }
}
