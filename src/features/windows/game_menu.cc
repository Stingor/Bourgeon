#include "features/windows/game_menu.h"

#include "features/windows/chat_window.h"  // QueueCommand (@load)
#include "features/windows/hotkey_settings.h"

#include <Windows.h>

#include <algorithm>

#include "bourgeon.h"
#include "imgui.h"
#include "ragnarok/globals.h"
#include "ragnarok/msgstring.h"
#include "ragnarok/uiwnd.h"
#include "ui/ro_imgui.h"
#include "utils/i18n.h"
#include "utils/log_console.h"

namespace {

// ── Adresses (client 20250716, no-ASLR : addr IDA == live) ───────────────────
// RE : docs/game_option_re.md §2. Les ids de FENÊTRE viennent du §2.6, les ids de
// COMMANDE du §2.4 — ces derniers sont locaux à `UIEscOptionWnd_OnMsg` et ne
// valent rien ailleurs.

constexpr int kEscMenuWndId = 155;  // UIEscOptionWnd, vtable 0x010384A0, objet 0xD8
// Les deux sous-fenêtres que ce menu est le SEUL à ouvrir (vérifié par recherche
// d'octets : rien d'autre dans l'image ne les fabrique).
constexpr int kHotkeyWndId       = 156;     // 0x9C   « Shortcut Settings »
constexpr int kGameSettingsWndId = 0x271E;  // 10014  « Game Settings »

// La fenêtre des macros — celle d'Alt+M.
//
// 🔴 IDENTIFIANT REMONTÉ, PAS DEVINÉ. Le client fabrique ses fenêtres par une
// table à deux étages : `0x00A42CA8[id]` donne un numéro de cas, et
// `0x00A42904[cas]` l'adresse du bloc qui construit la fenêtre. En partant du
// bloc qui installe la vtable portant `EmotionHotkey_SaveFromEditBoxes` — les
// champs de saisie des macros — on retombe sur l'id **86**. C'est aussi celui
// qu'ouvre la commande de raccourci 114 dans `UIWindowMgr_DispatchHotkeyBehavior`,
// ce qui le confirme par un second chemin.
// ⚠ NE PAS confondre avec `UIMacroRegisterWnd` (id 0x11E), qu'AUCUN raccourci
// n'ouvre : c'est une autre fenêtre, malgré son nom.
constexpr int kMacroWndId = 86;  // 0x56

// Les trois fenêtres que le branchement « Character Select » ferme. On ne les
// ferme pas nous-mêmes (le natif le fait), elles sont ici pour le REPLI.
constexpr int kWndAlsoClosedA = 164;
constexpr int kWndAlsoClosedB = 269;

// Ids de commande de `UIEscOptionWnd_OnMsg` (0x008FAE60), msg 6.
constexpr int kNativeCmdCharSelect = 371;

// `CMode::SendMsg` (0x00C86740) — vtable+0x18 du mode actif. ⚠ Les numéros de case
// d'IDA sont DÉJÀ la valeur du message, pas un index (reference_cmode_sendmsg_use_skill).
constexpr int kVfDispCmd = 0x18;
constexpr int kCmdRestart          = 25;   // CZ_RESTART 0x00B2 {u16 op, u8 type}
constexpr int kCmdRequestDisconnect = 88;  // -> case 128 -> CZ_REQ_DISCONNECT 0x018A
constexpr int kCmdStandingResurrect = 250; // CZ_STANDING_RESURRECTION 0x0292
constexpr int kRestartTypeSavePoint  = 0;  // réapparition au point de sauvegarde
constexpr int kRestartTypeCharSelect = 1;  // retour à la sélection de personnage

// Le drapeau « personnage mort » du mode de jeu. Confirmé en jeu le 2026-08-13 :
// c'est lui qui fait passer le menu natif de 5 boutons à 2 (ou 3).
constexpr int kOffGameModeDead = 0x250;

// `Own_HasResurrectionToken` (__thiscall sur le mode) : cherche dans l'inventaire
// les ids 7621 (Token of Siegfried), 6833, 6316, 6293, 25310 — liste EN DUR dans
// l'exe, vérifiée en mémoire vive. 🔴 On APPELLE le natif au lieu de recopier la
// liste : une copie se désynchronise dès que le serveur ajoute un objet, et rate
// surtout la garde `*(mode+0xCC)+0x48/+0x4C/+0x54` qui annule tout (docs §2.4.1).
constexpr uintptr_t kOwnHasResurrectionTokenAddr = 0x00c6b290;

// `StatusEffectList_Has(id)` : parcourt la liste d'effets actifs (pas de 5 dwords).
constexpr uintptr_t kStatusEffectListHasAddr = 0x00d8eba0;
constexpr int kStatusResurrectReady = 580;  // 2e voie vers le bouton Résurrection

// MsgStringTable : le seul libellé du natif qu'on reprend tel quel.
constexpr int kMsgMoveToSavePoint = 1548;  // MSI_MOVETO_SAVEPOINT

using DispCmd_t = void(__thiscall*)(void*, int, int, int, int, int);
using HasToken_t = char(__thiscall*)(void*);
using StatusHas_t = int(__stdcall*)(int);

// Le mode actif, par le GETTER natif et non par la lecture directe de
// `kActiveModePtr` : le getter rend 0 tant que `mgr+0x58 != 1`, ce qui masque le
// mode pendant les transitions (login, chargement de carte). C'est exactement ce
// que fait `UIEscOptionWnd_OnMsg`, et la nuance est documentée dans globals.h.
void* ActiveGameMode() {
  __try {
    using GetActive_t = void*(__thiscall*)(void*);
    return reinterpret_cast<GetActive_t>(rag::kModeMgrGetActiveAddr)(
        reinterpret_cast<void*>(rag::kModeMgrAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Envoie une commande au mode actif. Renvoie false si le mode est indisponible —
// l'appelant doit alors NE RIEN faire plutôt que réessayer autrement.
bool SendModeCmd(int cmd, int p1) {
  __try {
    void* mode = ActiveGameMode();
    if (!mode) return false;
    uiwnd::Vf<DispCmd_t>(mode, kVfDispCmd)(mode, cmd, p1, 0, 0, 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool PlayerIsDead() {
  __try {
    void* mode = ActiveGameMode();
    if (!mode) return false;
    return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(mode) +
                                   kOffGameModeDead) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CanResurrectOnTheSpot() {
  __try {
    void* mode = ActiveGameMode();
    if (!mode) return false;
    if (reinterpret_cast<HasToken_t>(kOwnHasResurrectionTokenAddr)(mode) != 0)
      return true;
    return reinterpret_cast<StatusHas_t>(kStatusEffectListHasAddr)(
               kStatusResurrectReady) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}  // namespace

// ── Cycle de vie ─────────────────────────────────────────────────────────────

void GameMenu::HandleNativeCreation(void* win) {
  // C'est nous qui venons de fabriquer cette native pour lui router une commande :
  // ne pas la prendre pour une demande du joueur.
  if (routing_) {
    uiwnd::SafeSetVisible(win, false);
    return;
  }
  if (!imgui_enabled_) return;  // interface native : on ne touche à rien

  // Masquer AVANT la première frame (on ne peut pas détruire ici : l'appelant de
  // MakeWindow déréférence encore le retour). `OnTick` détruit ensuite.
  uiwnd::SafeSetVisible(win, false);

  // Bascule, exactement comme `ToggleWindowById` : la native étant détruite à
  // chaque tick, chaque appui sur Échap repasse ici.
  if (open_) {
    Close();
  } else {
    open_ = true;
    need_pos_ = true;
    show_panel_ = true;
    confirm_savepoint_ = false;
    pending_ = Action::kNone;
    // AVANT la première frame : sinon elle dessinerait la disposition de la fois
    // précédente, corrigée seulement au tick suivant (cf. game_menu.h).
    RefreshLayout();
    // La frappe qui vient d'ouvrir ce panneau est aussi vue par ImGui : sans cette
    // grâce, la pile Échap le refermerait dans la même frappe (cf. game_menu.h).
    esc_grace_frames_ = 2;
  }
}

void GameMenu::ToggleFromUi() {
  if (!imgui_enabled_) {
    // Interface native : rendre la demande au client, qui ouvrira son propre menu.
    uiwnd::MakeWindow(kEscMenuWndId);
    return;
  }
  if (open_) {
    Close();
  } else {
    open_ = true;
    need_pos_ = true;
    show_panel_ = true;
    confirm_savepoint_ = false;
    pending_ = Action::kNone;
    // AVANT la première frame : sinon elle dessinerait la disposition de la fois
    // précédente, corrigée seulement au tick suivant (cf. game_menu.h).
    RefreshLayout();
    // La frappe qui vient d'ouvrir ce panneau est aussi vue par ImGui : sans cette
    // grâce, la pile Échap le refermerait dans la même frappe (cf. game_menu.h).
    esc_grace_frames_ = 2;
  }
}

void GameMenu::RefreshLayout() {
  if (!PlayerIsDead()) {
    layout_ = Layout::kAlive;
    return;
  }
  layout_ = CanResurrectOnTheSpot() ? Layout::kDeadWithToken
                                    : Layout::kDeadNoToken;
}

void GameMenu::Close() {
  open_ = false;
  confirm_savepoint_ = false;
  pending_ = Action::kNone;
}

void GameMenu::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  // Sortie du monde : l'état ne doit pas survivre au changement de personnage,
  // sinon le menu s'ouvre tout seul chez le suivant.
  if (mode_type != ModeMgr::ModeType::kGame) Close();
}

void GameMenu::OnTick() {
  if (!imgui_enabled_) {
    if (open_) Close();
    return;
  }
  if (!Bourgeon::Instance().IsGameActive()) {
    if (open_) Close();
    return;
  }

  // DÉTRUIRE la native, jamais seulement la masquer (cf. l'en-tête). No-op quand
  // il n'y en a pas — le cas normal.
  if (uiwnd::SafeFindWindow(kEscMenuWndId)) uiwnd::SafeCloseWindow(kEscMenuWndId);

  // Le joueur peut mourir — ou être ressuscité — panneau ouvert : la disposition se
  // relit aussi au tick, pas seulement à l'ouverture.
  if (open_) {
    RefreshLayout();
    // Un joueur ressuscité pendant que la confirmation est affichée n'a plus rien
    // à confirmer.
    if (layout_ == Layout::kAlive) confirm_savepoint_ = false;
  }

  RunPendingAction();
}

// ── Actions ──────────────────────────────────────────────────────────────────

void GameMenu::DriveNativeCommand(int native_cmd) {
  // Fabrique la native le temps d'un message, puis la détruit. Le handler natif
  // fait tout le travail — c'est ce qu'on veut pour « Character Select », dont le
  // branchement porte deux purges locales non identifiées.
  //
  // Appelé depuis OnTick, JAMAIS depuis une frame ImGui.
  routing_ = true;
  void* win = uiwnd::MakeWindow(kEscMenuWndId);
  if (win) {
    __try {
      uiwnd::OnMsg(win, /*msg=*/6, /*p2=*/native_cmd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      LogError("[GameMenu] exception sur OnMsg natif cmd {}", native_cmd);
    }
  }
  routing_ = false;

  // La plupart des branchements se ferment eux-mêmes (`SaveRectAndCloseWindow(155)`) ;
  // ce nettoyage couvre ceux qui ne le font pas, et le cas où `OnMsg` a levé.
  if (uiwnd::SafeFindWindow(kEscMenuWndId)) uiwnd::SafeCloseWindow(kEscMenuWndId);

  if (!win) {
    // Les deux gardes du case 155 (`mgr+0x4F10C`, `mgr+0x4F1B0`) ont refusé la
    // création — le natif lui-même n'aurait pas ouvert le menu. On rejoue alors le
    // strict nécessaire, sans les purges qu'on ne sait pas appeler.
    LogDiag("[GameMenu] MakeWindow(155) refusée -> repli direct pour la cmd {}",
            native_cmd);
    if (native_cmd == kNativeCmdCharSelect &&
        SendModeCmd(kCmdRestart, kRestartTypeCharSelect)) {
      uiwnd::SafeCloseWindow(kEscMenuWndId);
      uiwnd::SafeCloseWindow(kWndAlsoClosedA);
      uiwnd::SafeCloseWindow(kWndAlsoClosedB);
    }
  }
}

void GameMenu::RunPendingAction() {
  const Action action = pending_;
  if (action == Action::kNone) return;
  pending_ = Action::kNone;

  switch (action) {
    case Action::kCharSelect:
      // Routée vers le natif : purges + CZ_RESTART type 1 + fermeture de 155/164/269.
      Close();
      DriveNativeCommand(kNativeCmdCharSelect);
      break;

    case Action::kSavePoint:
      // CZ_RESTART type 0. Branchement natif entièrement connu (modale puis envoi),
      // et la modale est la NÔTRE — on ne route donc pas.
      if (SendModeCmd(kCmdRestart, kRestartTypeSavePoint)) Close();
      break;

    case Action::kResurrect:
      if (SendModeCmd(kCmdStandingResurrect, 0)) Close();
      break;

    case Action::kExitToWindows:
      // ⚠ NE quitte PAS le processus : demande la déconnexion au serveur, la sortie
      // vient de ZC_ACK_REQ_DISCONNECT 0x018B. Court-circuiter par un ExitProcess
      // perdrait la sauvegarde serveur des raccourcis (docs §4.8).
      Close();
      SendModeCmd(kCmdRequestDisconnect, 0);
      break;

    case Action::kOpenMacros:
      Close();
      uiwnd::MakeWindow(kMacroWndId);
      break;

    case Action::kOpenGameSettings:
      Close();
      uiwnd::MakeWindow(kGameSettingsWndId);
      break;

    case Action::kOpenHotkeyNative:
      Close();
      uiwnd::MakeWindow(kHotkeyWndId);
      break;

    case Action::kNone:
      break;
  }
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void GameMenu::OnRenderUI() {
  if (!imgui_enabled_ || !open_) return;

  // Grâce sur la pile Échap : la frappe d'ouverture ne doit pas nous refermer.
  if (esc_grace_frames_ > 0) {
    --esc_grace_frames_;
    ro::SuppressEscapeStack();
  }

  if (need_pos_) {
    // Centré à la première ouverture, puis ImGui garde la position que le joueur
    // lui donne — le natif, lui, retombait toujours sur (185 en repère 640,
    // 300 en repère 480).
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    need_pos_ = false;
  }

  // Les libellés, à nous (cf. l'en-tête : ceux du natif sont peints dans les bmp).
  const char* label_char_select = i18n::Tr("Sélection du personnage");
  const char* label_settings    = i18n::Tr("Réglages du jeu");
  const char* label_shortcuts   = i18n::Tr("Configuration des raccourcis");
  const char* label_exit        = i18n::Tr("Quitter le jeu");
  const char* label_return      = i18n::Tr("Retour au jeu");
  const char* label_savepoint   = i18n::Tr("Retour au point de sauvegarde");
  const char* label_resurrect   = i18n::Tr("Résurrection");
  // ⚠ Deux libellés PROCHES pour deux gestes différents : `label_savepoint` est la
  // réapparition APRÈS LA MORT (menu de mort, CZ_RESTART), `label_load` est la
  // téléportation d'un personnage VIVANT (commande serveur @load). Le possessif
  // « mon » les distingue à la lecture, et ils ne s'affichent jamais ensemble.
  const char* label_load        = i18n::Tr("Retour à mon point de sauvegarde");
  const char* label_macros      = i18n::Tr("Ouvrir les macros");

  // Largeur MESURÉE sur le plus long libellé, jamais en dur : la police ET la
  // langue sont des réglages (feedback_ui_width_measured_not_hardcoded), et une
  // fenêtre en AlwaysAutoResize sans largeur explicite s'emballe
  // (feedback_imgui_autoresize_needs_explicit_widths).
  const float button_w = ro::MaxButtonWidth({label_char_select, label_settings,
                                             label_shortcuts, label_exit,
                                             label_return, label_savepoint,
                                             label_resurrect, label_load,
                                             label_macros});
  ImGui::SetNextWindowSize(
      ImVec2(button_w + ImGui::GetStyle().WindowPadding.x * 2.0f, 0.0f),
      ImGuiCond_Always);

  // Titre PARLANT quand le personnage est mort : c'est l'information la plus utile
  // au moment où le joueur ouvre ce menu, et elle explique pourquoi les boutons ne
  // sont pas ceux qu'il attendait.
  //
  // ⚠ Le suffixe `###bourgeon_game_menu` est IDENTIQUE dans les deux cas, et c'est
  // ce qui compte : ImGui identifie une fenêtre par ce qui suit `###`. Deux titres
  // sans suffixe commun donneraient DEUX fenêtres — position, taille et z-order
  // repartis de zéro à chaque mort.
  const char* title = (layout_ == Layout::kAlive)
                          ? i18n::Tr("Options du jeu###bourgeon_game_menu")
                          : i18n::Tr("Vous êtes mort !###bourgeon_game_menu");

  const bool begun = ro::BeginRoWindow(
      title, &show_panel_,
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
  if (!show_panel_) {  // clic sur le X = « Retour au jeu »
    Close();
    show_panel_ = true;
  }
  if (!begun) { ro::EndRoWindow(); return; }

  // Les boutons, dans l'ORDRE du natif pour chaque disposition (docs §2.4), avec
  // NOS ajouts en tête.
  if (layout_ == Layout::kAlive) {
    // ── Retour au point de sauvegarde (@load) ──────────────────────────────
    // 🔴 CE N'EST PAS le « Return to save point » du menu de MORT : celui-là est
    // `CZ_RESTART` type 0, c'est-à-dire la réapparition après un décès. Ici le
    // personnage est VIVANT, et rien dans le protocole ne permet de se téléporter
    // — c'est une commande du SERVEUR. On l'envoie donc telle qu'un joueur la
    // taperait, par le pipeline complet du client, ce qui a l'avantage de faire
    // revenir son refus éventuel dans le chat, à sa place.
    if (ro::RoButton(label_load, button_w)) {
      Close();
      // `QueueCommand` ARME la commande ; c'est `FlushPending`, hors frame ImGui,
      // qui l'envoie. Appelée sans condition depuis `OnProcessInput`, elle part
      // donc aussi quand la chatbox ImGui est éteinte.
      if (auto* chat = Bourgeon::Instance().chat_window()) chat->QueueCommand("@load");
    }
    // La commande EXACTE en infobulle : le joueur voit ce qui part, et peut la
    // retaper lui-même. On ne traduit pas — c'est un nom de commande serveur.
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("@load");

    if (ro::RoButton(label_char_select, button_w)) pending_ = Action::kCharSelect;
    if (ro::RoButton(label_settings, button_w)) {
      // ⏳ INTERIM : la fenêtre Game Settings ImGui n'existe pas encore, on ouvre
      // donc la native (id 0x271E). À remplacer par l'ouverture de notre panneau
      // dès que game_settings.{h,cc} atterrit — c'est l'étape suivante du chantier
      // décrit dans docs/game_option_re.md §5.7. Un bouton mort serait pire : le
      // joueur perdrait l'accès à ses réglages.
      pending_ = Action::kOpenGameSettings;
    }
    if (ro::RoButton(label_shortcuts, button_w)) {
      Close();
      // ⚠ CE BOUTON NE DOIT JAMAIS ÊTRE MORT. Notre table est ON par défaut, mais
      // le joueur peut la désactiver seule dans le panneau — et notre panneau
      // refuse alors de s'ouvrir. On retombe donc sur la fenêtre du client, que
      // son hook de création laissera vivre puisqu'il est éteint lui aussi.
      auto* hotkeys = Bourgeon::Instance().hotkey_settings();
      if (hotkeys && hotkeys->imgui_enabled_) hotkeys->OpenFromMenu();
      else                                    pending_ = Action::kOpenHotkeyNative;
    }
    // Les macros n'ont AUCUN chemin d'ouverture dans le menu du client : elles ne
    // s'atteignent qu'au raccourci (Alt+M par défaut). Un joueur qui l'a remappé,
    // ou qui ne l'a jamais su, n'y accédait donc plus du tout.
    if (ro::RoButton(label_macros, button_w)) pending_ = Action::kOpenMacros;
    if (ro::RoButton(label_exit, button_w)) pending_ = Action::kExitToWindows;
    if (ro::RoButton(label_return, button_w)) Close();
  } else {
    if (layout_ == Layout::kDeadWithToken) {
      if (ro::RoButton(label_resurrect, button_w)) pending_ = Action::kResurrect;
    }
    if (ro::RoButton(label_savepoint, button_w)) confirm_savepoint_ = true;
    if (ro::RoButton(label_return, button_w)) Close();
  }

  // Confirmation du retour au point de sauvegarde. Le texte est celui du CLIENT
  // (MsgStringTable 1548), pas une paraphrase — règle du projet.
  //
  // ⚠ UNE SEULE chaîne pour l'ouverture ET pour le Begin : ImGui apparie les popups
  // par l'identifiant dérivé de la partie qui suit `###`, donc deux libellés
  // différents ne se retrouvent pas — et l'échec est SILENCIEUX (le bouton ne fait
  // simplement rien). C'est la convention des autres modales du projet.
  const char* kSavePointPopup =
      i18n::Tr("Confirmation###bourgeon_game_menu_savepoint");
  if (confirm_savepoint_) {
    ImGui::OpenPopup(kSavePointPopup);
    confirm_savepoint_ = false;
  }
  if (ro::BeginRoPopupModal(kSavePointPopup)) {
    // Modale ouverte : neutraliser la pile Échap, sinon un Échap fermerait À LA FOIS
    // la confirmation ET le menu derrière (usage documenté de SuppressEscapeStack).
    ro::SuppressEscapeStack();
    const char* question = msgstr::Utf8(kMsgMoveToSavePoint);
    if (!question || !*question)
      question = i18n::Tr("Revenir au point de sauvegarde ?");
    ImGui::TextUnformatted(question);
    ImGui::Spacing();
    const float ok_w = ro::MaxButtonWidth({i18n::Tr("OK"), i18n::Tr("Annuler")});
    if (ro::RoButton(i18n::Tr("OK"), ok_w)) {
      pending_ = Action::kSavePoint;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), ok_w)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  ro::EndRoWindow();
}
