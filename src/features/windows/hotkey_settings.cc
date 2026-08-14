#include "features/windows/hotkey_settings.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "features/hotkey_actions.h"
#include "features/hotkey_util.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "imgui.h"
#include "ragnarok/msgstring.h"
#include "ragnarok/ui_window_mgr.h"  // UIM_PUSHINTOCHATHISTORY (retour de /bm)
#include "ragnarok/uiwnd.h"
#include "ui/ro_imgui.h"
#include "utils/i18n.h"
#include "utils/log_console.h"

namespace {

// UIHotKeyWnd — vtable 0x010383C8, objet 0x120, cache mgr+0x404.
constexpr int kHotkeyWndId = 156;  // 0x9C

// Libellés d'onglet : ceux du CLIENT (MsgStringTable), pas les nôtres.
//
// 🔴 Choix assumé, et cohérent : les LIGNES de la table sont des libellés du jeu
// (« Hotkey 2-1 », champ EXE de UserKeys.lua) qu'on ne peut pas traduire — ils
// viennent du Lua du client. Des onglets français au-dessus de lignes anglaises
// donneraient une fenêtre bâtarde. On reprend donc les quatre libellés du client,
// qui suivent d'eux-mêmes sa langue. Même règle que les noms d'objets.
constexpr int kMsgTabSkillBar1 = 1491;  // MSI_HOTKEYWND_TAB1      « Skill Bar »
constexpr int kMsgTabSkillBar2 = 3595;  // MSI_HOTKEYWND_SKILLBAR2 « Hotkey Bar 2 »
constexpr int kMsgTabInterface = 1492;  // MSI_HOTKEYWND_TAB2      « Interface »
constexpr int kMsgTabMacros    = 1493;  // MSI_HOTKEYWND_TAB3      « Macros »
constexpr int kMsgNotAssigned  = 1518;  // MSI_HOTKEY_NOTHING      « Not Assigned »
constexpr int kMsgUnspecified  = 1700;  // MSI_HOTKEY_UNKOWN       « Unspecified value »
constexpr int kMsgWindowTitle  = 1494;  // MSI_HOTKEYWND_TITLE     « Shortcut Settings »
constexpr int kMsgBattleMode   = 1775;  // MSI_CHATMODE_ONOFF      « Enable Battle Mode »
// « Stored shortcut key combination will be initialized. Do you want to
// continue? » — le client a le texte mais n'affiche AUCUNE modale sur sa
// commande 363. Nous, si : l'action efface tous les raccourcis du joueur.
constexpr int kMsgResetConfirm = 1490;
// Les deux lignes que `/bm` écrit au chat. La commande 213 de la fenêtre ne les
// émet PAS — seul le chemin de la commande de chat le fait — alors que c'est le
// seul retour visible d'une bascule dont l'effet, lui, ne se voit qu'en jouant.
constexpr int kMsgBattleOn  = 785;  // MSI_BATTLE_ON  « …are Enabled. [/bm ON] »
constexpr int kMsgBattleOff = 786;  // MSI_BATTLE_OFF « …are Disabled. [/bm OFF] »

// ── Battle Mode ──────────────────────────────────────────────────────────────
// g_ChangeChatMode : l'état du mode combat, un OCTET. Écrit par le message 213 de
// UIHotKeyWnd, lu par la fenêtre de chat.
constexpr uintptr_t kChangeChatModeAddr = 0x0131f50e;

// 🔴 213 est une COMMANDE, pas un message. `UIHotKeyWnd_OnMsg` (0x008FB130)
// n'entre dans son switch que si le message vaut **6** (= clic sur un bouton) ;
// 213 y est la valeur du paramètre suivant, au même titre que 184 (OK) ou 185
// (cancel). Envoyer 213 EN MESSAGE tombe dans `UIWindow_OnMsg_Default`, qui ne
// fait rien et ne se plaint pas — la case se dessinait donc, sans jamais agir.
constexpr int kMsgButtonClick      = 6;
constexpr int kCmdToggleBattleMode = 213;
// Les deux commandes du [Reset] natif. Il en faut DEUX : 363 ne fait que METTRE
// les défauts en attente dans les quatre maps d'édition, 184 (OK) est ce qui les
// commet — et referme la fenêtre au passage.
constexpr int kCmdStageDefaults    = 363;
constexpr int kCmdCommitAndClose   = 184;
// Le handler REFUSE la commande si son premier argument n'est pas ce bouton-là
// (`if (param_1 != *(this + 264)) return 0;`) — d'où la lecture de +0x108.
constexpr int kOffToggleButton     = 0x108;

bool BattleModeEnabled() {
  __try {
    return *reinterpret_cast<const uint8_t*>(kChangeChatModeAddr) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// 🔴 Texte secondaire : couleur EXPLICITE, jamais ImGui::TextDisabled. Le corps
// d'une fenêtre RO est CLAIR, et le gris de TextDisabled y est illisible
// (feedback_imgui_ro_light_body_colors).
const ImVec4 kSecondaryText(0.42f, 0.38f, 0.32f, 1.0f);

const int kTabMsgIds[userhotkey::kCategoryCount] = {
    kMsgTabSkillBar1, kMsgTabSkillBar2, kMsgTabInterface, kMsgTabMacros};

// Repli si la table de messages n'est pas encore chargée (appel très tôt).
const char* kTabFallback[userhotkey::kCategoryCount] = {
    "Skill Bar", "Hotkey Bar 2", "Interface", "Macros"};

// L'onglet des actions Bourgeon porte le nom du projet : ce n'est pas un mot à
// traduire, et c'est ce qui le distingue au premier coup d'œil des quatre
// onglets du client.
constexpr const char* kBourgeonTabLabel = "Bourgeon";

const char* TabLabel(int tab) {
  if (tab == HotkeySettings::kTabBourgeon) return kBourgeonTabLabel;
  if (tab < 0 || tab >= userhotkey::kCategoryCount) return "";
  const char* s = msgstr::Utf8(kTabMsgIds[tab]);
  return (s && *s) ? s : kTabFallback[tab];
}

// Décompose un couple (touche, modificateur) du client en booléens de
// modificateurs. Le client range le modificateur en `key_code2`… la plupart du
// temps : les deux ordres existent dans UserKeys.lua, d'où la lecture des deux
// champs plutôt que la confiance en une position.
void SplitClientKeys(const userhotkey::Binding& binding, int* main_vk, bool* ctrl,
                     bool* alt, bool* shift) {
  auto is_modifier = [](int key_code) {
    return key_code == VK_CONTROL || key_code == VK_SHIFT || key_code == VK_MENU;
  };
  int mod_vk = 0;
  *main_vk = binding.key_code1;
  if (is_modifier(binding.key_code1)) {
    mod_vk = binding.key_code1;
    *main_vk = binding.key_code2;
  } else if (is_modifier(binding.key_code2)) {
    mod_vk = binding.key_code2;
  }
  *ctrl  = (mod_vk == VK_CONTROL);
  *alt   = (mod_vk == VK_MENU);
  *shift = (mod_vk == VK_SHIFT);
}

// VK du modificateur d'un combo, 0 s'il n'y en a pas — la forme qu'attend le
// client, qui n'en retient qu'UN.
int ModifierVk(bool ctrl, bool alt, bool shift) {
  if (ctrl)  return VK_CONTROL;
  if (alt)   return VK_MENU;
  if (shift) return VK_SHIFT;
  return 0;
}

int ModifierCount(bool ctrl, bool alt, bool shift) {
  return (ctrl ? 1 : 0) + (alt ? 1 : 0) + (shift ? 1 : 0);
}

// Recherche insensible à la casse, sur l'ASCII seulement — suffisant : ces deux
// textes viennent du client et sont en pratique latins.
bool Contains(const char* haystack, const char* needle) {
  if (!needle || !*needle) return true;
  if (!haystack || !*haystack) return false;
  const size_t n = std::strlen(needle);
  for (const char* p = haystack; *p; ++p) {
    size_t i = 0;
    while (i < n && p[i] &&
           std::tolower(static_cast<unsigned char>(p[i])) ==
               std::tolower(static_cast<unsigned char>(needle[i])))
      ++i;
    if (i == n) return true;
  }
  return false;
}

}  // namespace

// ── Cycle de vie ─────────────────────────────────────────────────────────────

void HotkeySettings::HandleNativeCreation(void* win) {
  // C'est nous qui venons d'ouvrir la native pour le remappage : ne pas la
  // prendre pour une demande d'ouverture de notre panneau, et surtout NE PAS la
  // masquer — le joueur doit la voir pour s'en servir.
  if (routing_) return;
  if (!imgui_enabled_) return;

  uiwnd::SafeSetVisible(win, false);

  if (open_) {
    Close();
  } else {
    OpenFromMenu();
  }
}

void HotkeySettings::OpenFromMenu() {
  open_ = true;
  need_pos_ = true;
  show_panel_ = true;
  rows_dirty_ = true;  // relire AVANT la première frame (docs §5.6 point 11)
  esc_grace_frames_ = 2;
}

void HotkeySettings::Close() {
  open_ = false;
  // Une capture laissée en cours gèlerait les raccourcis de Bourgeon sur une
  // fenêtre fermée… jusqu'au prochain PingCapture, qui n'aurait jamais lieu.
  CancelCapture();
  capture_error_[0] = '\0';
}

void HotkeySettings::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  if (mode_type != ModeMgr::ModeType::kGame) {
    Close();
    native_editing_ = false;
    rows_.clear();
    rows_dirty_ = true;
  }
}

void HotkeySettings::OnTick() {
  if (!imgui_enabled_) {
    if (open_) Close();
    return;
  }
  if (!Bourgeon::Instance().IsGameActive()) {
    if (open_) Close();
    native_editing_ = false;
    return;
  }

  // Bascule du Battle Mode : routée vers le natif (cf. hotkey_settings.h).
  if (pending_battle_mode_ >= 0) {
    const bool on = (pending_battle_mode_ != 0);
    pending_battle_mode_ = -1;
    DriveBattleMode(on);
    return;
  }

  if (pending_reset_) {
    pending_reset_ = false;
    DriveResetDefaults();
    return;
  }

  // Écriture d'une commande du client, posée par la capture au rendu.
  if (pending_write_.valid) {
    const PendingWrite write = pending_write_;
    pending_write_ = PendingWrite();
    if (userhotkey::WriteBinding(write.category, write.command_index, write.key1,
                                 write.key2, write.label)) {
      // 🔴 Le fichier n'est PAS le seul destinataire : `UserHotkey_SaveToTable`
      // (0x0059EEF0) rebâtit la charge `/userconfig/save` DEPUIS LE LUA à la
      // sortie propre. Écrire par ce pont suffit donc à se retrouver dans la
      // synchro par compte — aucun drapeau « modifié » à lever.
      userhotkey::Save();
    } else {
      LogDiag("HotkeySettings: ecriture refusee (cat={} cmd={})", write.category,
              write.command_index);
    }
    rows_dirty_ = true;
    return;
  }

  // Demande de remappage : on fabrique la native NOUS-MÊMES, hook neutralisé, et
  // on la laisse vivre et visible tant que le joueur s'en sert.
  if (pending_open_native_) {
    pending_open_native_ = false;
    routing_ = true;
    void* win = uiwnd::MakeWindow(kHotkeyWndId);
    routing_ = false;
    native_editing_ = (win != nullptr);
    return;
  }

  void* native = uiwnd::SafeFindWindow(kHotkeyWndId);

  if (native_editing_) {
    // Le joueur remappe dans la fenêtre du jeu : on la laisse vivre. Quand elle
    // disparaît (OK / cancel / close), les raccourcis ont pu changer -> relire.
    if (!native) {
      native_editing_ = false;
      rows_dirty_ = true;
    }
    return;
  }

  // 🔴 DÉTRUIRE, pas masquer : tant qu'elle existe, la native détourne et consomme
  // TOUTE la frappe clavier (UIWindowMgr_OnKeyDown @0x00A47201).
  if (native) uiwnd::SafeCloseWindow(kHotkeyWndId);
}

// ── Données ──────────────────────────────────────────────────────────────────

void HotkeySettings::RefreshRows() {
  rows_.clear();
  rows_dirty_ = false;

  const bool all = (tab_ == kTabAll);
  if (all || tab_ != kTabBourgeon) {
    const int first = all ? 0 : tab_;
    const int last  = all ? userhotkey::kCategoryCount - 1 : tab_;
    for (int tab = first; tab <= last; ++tab) {
      const int category = userhotkey::CategoryForTab(tab);
      const int count = userhotkey::RowCount(category);
      for (int row = 0; row < count; ++row) {
        Row entry;
        entry.tab = tab;
        entry.category = category;
        if (userhotkey::ReadBinding(category, row, &entry.binding))
          rows_.push_back(entry);
      }
    }
  }

  if (!all && tab_ != kTabBourgeon) return;

  // Les actions de Bourgeon, rendues dans la MÊME struct que les commandes du
  // client : le libellé traduit prend la place du champ EXE, le libellé de combo
  // celle du nom de touche. Le dessin et la recherche n'ont donc qu'un chemin.
  for (int i = 0; i < hotkeys::ActionCount(); ++i) {
    Row entry;
    entry.tab = kTabBourgeon;
    entry.action_index = i;
    entry.binding.command_index = i;
    std::snprintf(entry.binding.label, sizeof(entry.binding.label), "%s",
                  i18n::Tr(hotkeys::ActionAt(i).label_fr));
    const hotkeys::Binding& binding = hotkeys::BindingAt(i);
    entry.binding.key_code1 = binding.vk;
    entry.binding.key_code2 = ModifierVk(binding.ctrl, binding.alt, binding.shift);
    entry.binding.assigned = (binding.vk != 0);
    if (entry.binding.assigned) {
      hotkeys::Label(binding.vk, binding.ctrl, binding.alt, binding.shift,
                     entry.binding.key_name, sizeof(entry.binding.key_name));
    }
    rows_.push_back(entry);
  }
}

// ── Menu contextuel d'une ligne ──────────────────────────────────────────────

bool HotkeySettings::DrawRowMenu(const Row& row) {
  // Attaché à la cellule qui précède (la colonne des touches). L'identifiant est
  // celui poussé par la boucle : deux lignes ne peuvent pas se le disputer.
  if (!ImGui::BeginPopupContextItem("##hotkey_row_menu")) return false;

  bool wrote = false;
  const bool is_action = (row.action_index >= 0);

  // « Touche du client » n'a de sens que pour SES commandes : le catalogue de
  // Bourgeon ne propose aucun défaut, remettre le sien reviendrait à effacer.
  if (!is_action) {
    if (ImGui::MenuItem(i18n::Tr("Remettre la touche par défaut"))) {
      userhotkey::Binding fallback;
      if (userhotkey::ReadDefaultBinding(row.category, row.binding.command_index,
                                         &fallback)) {
        int  main_vk = 0;
        bool ctrl = false, alt = false, shift = false;
        SplitClientKeys(fallback, &main_vk, &ctrl, &alt, &shift);
        // Le défaut peut très bien être déjà pris par un autre raccourci — le
        // joueur l'a peut-être déplacé lui-même. On le contrôle donc comme
        // n'importe quelle affectation, plutôt que de créer un doublon muet.
        char what[96];
        if (main_vk != 0 &&
            hotkeys::Conflict(main_vk, ctrl, alt, shift, hotkeys::Owner::kClientCommand,
                              hotkeys::ClientSelf(row.category, row.binding.command_index),
                              what, sizeof(what))) {
          std::snprintf(capture_error_, sizeof(capture_error_),
                        i18n::Tr("Déjà utilisé par %s — choisis une autre touche."),
                        what);
        } else {
          pending_write_.valid = true;
          pending_write_.category = row.category;
          pending_write_.command_index = row.binding.command_index;
          pending_write_.key1 = main_vk;
          pending_write_.key2 = ModifierVk(ctrl, alt, shift);
          std::snprintf(pending_write_.label, sizeof(pending_write_.label), "%s",
                        row.binding.label);
          capture_error_[0] = '\0';
          wrote = true;
        }
      } else {
        std::snprintf(capture_error_, sizeof(capture_error_), "%s",
                      i18n::Tr("Le client ne donne aucune touche par défaut à "
                               "cette commande."));
      }
    }
  }

  if (ImGui::MenuItem(i18n::Tr("Effacer la touche"), nullptr, false,
                      row.binding.assigned || row.binding.key_code1 != 0)) {
    if (is_action) {
      hotkeys::SetBinding(row.action_index, hotkeys::Binding());
      if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
    } else {
      pending_write_.valid = true;
      pending_write_.category = row.category;
      pending_write_.command_index = row.binding.command_index;
      pending_write_.key1 = 0;  // 0/0 = effacement, c'est la convention du pont
      pending_write_.key2 = 0;
      std::snprintf(pending_write_.label, sizeof(pending_write_.label), "%s",
                    row.binding.label);
    }
    capture_error_[0] = '\0';
    wrote = true;
  }

  ImGui::EndPopup();
  if (wrote) rows_dirty_ = true;
  return wrote;
}

// ── Capture ──────────────────────────────────────────────────────────────────

bool HotkeySettings::IsCapturing(const Row& row) const {
  if (!capturing_) return false;
  if (row.action_index >= 0) return capture_action_ == row.action_index;
  return capture_category_ == row.category &&
         capture_command_ == row.binding.command_index;
}

void HotkeySettings::BeginCapture(const Row& row) {
  capturing_ = true;
  capture_error_[0] = '\0';
  capture_action_   = row.action_index;
  capture_category_ = (row.action_index >= 0) ? -1 : row.category;
  capture_command_  = (row.action_index >= 0) ? -1 : row.binding.command_index;
}

void HotkeySettings::CancelCapture() {
  capturing_ = false;
  capture_action_   = -1;
  capture_category_ = -1;
  capture_command_  = -1;
}

bool HotkeySettings::RunCapture(const Row& row) {
  // Gèle TOUS les raccourcis de Bourgeon le temps du choix : la touche pressée
  // doit remapper, pas déclencher l'action qu'elle porte encore.
  hotkeys::PingCapture();
  // Et retient la pile Échap, sinon la touche d'annulation refermerait aussi la
  // fenêtre (même piège que l'ouverture, docs §5.6 point 10).
  ro::SuppressEscapeStack();

  // 🔴 ÉCHAP N'EST JAMAIS AFFECTABLE, et c'est la première chose testée. Elle a
  // déjà deux rôles qu'aucun raccourci ne doit pouvoir lui prendre : annuler la
  // capture ici, et ouvrir le menu du jeu partout ailleurs. Le verrou tient à
  // deux endroits — ce test, qui la consomme avant tout le reste, et
  // `hotkeys::CaptureMainVk` qui ne rend que lettres, chiffres, F1-F12 et Espace.
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    CancelCapture();
    return false;
  }

  const bool clear = ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
  int vkey = clear ? 0 : hotkeys::CaptureMainVk();
  if (!clear && vkey == 0) return false;

  ImGuiIO& io = ImGui::GetIO();
  const bool ctrl = !clear && io.KeyCtrl;
  const bool alt = !clear && io.KeyAlt;
  const bool shift = !clear && io.KeyShift;

  // ⚠ Le client ne retient qu'UN modificateur (`key2`) : lui en passer deux en
  // perdrait un en silence, et la touche affectée ne serait pas celle affichée.
  // Nos propres actions, elles, en acceptent autant qu'on veut.
  if (!clear && row.action_index < 0 && ModifierCount(ctrl, alt, shift) > 1) {
    std::snprintf(capture_error_, sizeof(capture_error_), "%s",
                  i18n::Tr("Le jeu ne retient qu'un seul modificateur (Ctrl, Alt "
                           "ou Maj) par raccourci."));
    return false;
  }

  if (!clear) {
    char what[96];
    const hotkeys::Owner owner = (row.action_index >= 0) ? hotkeys::Owner::kAction
                                                         : hotkeys::Owner::kClientCommand;
    const int self_index = (row.action_index >= 0)
                               ? row.action_index
                               : hotkeys::ClientSelf(row.category, row.binding.command_index);
    if (hotkeys::Conflict(vkey, ctrl, alt, shift, owner, self_index, what, sizeof(what))) {
      std::snprintf(capture_error_, sizeof(capture_error_),
                    i18n::Tr("Déjà utilisé par %s — choisis une autre touche."), what);
      return false;
    }
  }

  if (row.action_index >= 0) {
    hotkeys::Binding binding;
    binding.vk = vkey;
    binding.ctrl = ctrl;
    binding.alt = alt;
    binding.shift = shift;
    hotkeys::SetBinding(row.action_index, binding);
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  } else {
    // Différée au tick : ces deux ponts appellent le Lua du client.
    pending_write_.valid = true;
    pending_write_.category = row.category;
    pending_write_.command_index = row.binding.command_index;
    pending_write_.key1 = vkey;
    pending_write_.key2 = ModifierVk(ctrl, alt, shift);
    std::snprintf(pending_write_.label, sizeof(pending_write_.label), "%s",
                  row.binding.label);
  }

  CancelCapture();
  rows_dirty_ = true;
  return true;
}

void HotkeySettings::DriveBattleMode(bool on) {
  // Fabrique la native le temps d'un message, puis la détruit. Le handler natif
  // fait tout : l'écriture de g_ChangeChatMode ET la reconfiguration de la fenêtre
  // de chat (barre de saisie, bandeau d'onglets, purge des liens d'objets).
  //
  // La native ne vit que le temps de cet appel, HORS frame ImGui : aucune frame ne
  // la dessine, et son détournement du clavier n'a pas le temps d'exister.
  routing_ = true;
  void* win = uiwnd::MakeWindow(kHotkeyWndId);
  if (win) {
    __try {
      uiwnd::SetVisible(win, false);
      // ⚠ Le handler REFUSE la commande si son premier argument n'est pas le
      // bouton bascule de la fenêtre : on le lui repasse tel quel.
      void* toggle = *reinterpret_cast<void**>(
          reinterpret_cast<uint8_t*>(win) + kOffToggleButton);
      uiwnd::OnMsg(win, kMsgButtonClick, /*p2=*/kCmdToggleBattleMode,
                   /*p3=*/on ? 1 : 0, /*p4=*/0, /*p5=*/0,
                   /*arg0=*/static_cast<int>(reinterpret_cast<uintptr_t>(toggle)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  routing_ = false;

  if (uiwnd::SafeFindWindow(kHotkeyWndId)) uiwnd::SafeCloseWindow(kHotkeyWndId);

  SayBattleMode(on);
}

// Reprend le retour au chat de la commande `/bm`, que la case ne donnait pas :
// une bascule dont l'effet ne se voit qu'en jouant méritait d'être confirmée
// quelque part. Libellé du CLIENT, en CP949 — c'est ce qu'attend le chat.
//
// UIM_PUSHINTOCHATHISTORY empile une ligne et n'ouvre aucune modale ; il est
// d'ailleurs appelé ici depuis le TICK, donc hors de toute frame ImGui. Quand la
// chatbox ImGui a remplacé la native, la ligne ne sort même pas de chez nous
// (Bourgeon::RouteChatLine la reprend au passage).
void HotkeySettings::SayBattleMode(bool on) {
  const char* text = msgstr::Cp949(on ? kMsgBattleOn : kMsgBattleOff);
  if (!text || !*text) return;
  UIWindowMgr::SendMsg(UIMessage::UIM_PUSHINTOCHATHISTORY,
                       reinterpret_cast<int>(text), 0, 0, 0);
}

void HotkeySettings::DriveResetDefaults() {
  // Même recette que le Battle Mode : la native ne vit que le temps de deux
  // commandes, HORS frame ImGui, et c'est elle qui fait tout le travail.
  //
  // 🔴 DEUX COMMANDES, PAS UNE. `StageDefaultBindings` (363) ne fait que remplir
  // les maps d'édition avec ce que rend le Lua `GetOriginalHotKeyInfo` ; sans le
  // OK (184) qui suit, rien n'est écrit et tout est jeté à la fermeture. C'est
  // exactement le piège documenté du bouton Reset natif.
  routing_ = true;
  void* win = uiwnd::MakeWindow(kHotkeyWndId);
  if (win) {
    __try {
      uiwnd::SetVisible(win, false);
      uiwnd::OnMsg(win, kMsgButtonClick, kCmdStageDefaults);
      // ⚠ 184 commet PUIS referme la fenêtre lui-même
      // (`UIWindowMgr_SaveRectAndCloseWindow`) : `win` est mort après cet appel,
      // ne plus y toucher.
      uiwnd::OnMsg(win, kMsgButtonClick, kCmdCommitAndClose);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  routing_ = false;

  if (uiwnd::SafeFindWindow(kHotkeyWndId)) uiwnd::SafeCloseWindow(kHotkeyWndId);
  rows_dirty_ = true;
}

void HotkeySettings::OpenNativeForEditing() {
  // Le remappage se fait chez nous ; la native ne sert plus qu'au [Reset], qui
  // relit les défauts du client (`GetOriginalHotKeyInfo`) et n'a pas d'équivalent
  // de notre côté. Appelée depuis OnRenderUI, donc l'ouverture est DIFFÉRÉE au
  // tick (feedback_no_native_cmd_during_imgui_frame).
  CancelCapture();
  pending_open_native_ = true;
  Close();
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void HotkeySettings::OnRenderUI() {
  if (!imgui_enabled_ || !open_) return;

  if (esc_grace_frames_ > 0) {
    --esc_grace_frames_;
    ro::SuppressEscapeStack();
  }

  if (rows_dirty_) RefreshRows();

  if (need_pos_) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ro::Px(420.0f), ro::Px(430.0f)),
                             ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }

  // Titre : celui du client (« Shortcut Settings »), suffixe ### figé pour que la
  // fenêtre garde sa position et sa taille d'une langue à l'autre.
  char title[128];
  const char* native_title = msgstr::Utf8(kMsgWindowTitle);
  std::snprintf(title, sizeof(title), "%s###bourgeon_hotkey_settings",
                (native_title && *native_title) ? native_title
                                                : i18n::Tr("Raccourcis clavier"));

  const bool begun = ro::BeginRoWindow(title, &show_panel_);
  if (!show_panel_) { Close(); show_panel_ = true; }
  if (!begun) { ro::EndRoWindow(); return; }

  // ── Recherche ──────────────────────────────────────────────────────────────
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##hotkey_filter", i18n::Tr("Rechercher une commande ou une touche..."),
                           filter_, sizeof(filter_));

  // ── Onglets ────────────────────────────────────────────────────────────────
  // Les quatre onglets du client, plus « Tout » qui les fusionne en une seule
  // liste — vue que le natif n'a pas, et qui prend tout son sens avec la
  // recherche : chercher une touche sans savoir dans quelle catégorie elle vit.
  // « Tout » en TÊTE, puis les quatre onglets du client dans leur ordre à eux.
  const int tab_order[] = {kTabAll, 0, 1, 2, 3, kTabBourgeon};
  if (ro::RoBeginTabBar("hotkey_tabs")) {
    for (int tab : tab_order) {
      // Identifiant d'onglet TECHNIQUE et stable : le libellé vient du client et
      // change avec sa langue, ce qui recréerait l'onglet à chaque bascule.
      char tab_id[96];
      std::snprintf(tab_id, sizeof(tab_id), "%s###hotkey_tab_%d",
                    (tab == kTabAll) ? i18n::Tr("Tout") : TabLabel(tab), tab);
      if (ImGui::BeginTabItem(tab_id)) {
        if (tab_ != tab) {
          tab_ = tab;
          // Changer d'onglet en pleine capture laisserait une ligne invisible en
          // attente d'une touche, et le premier appui la lui donnerait.
          CancelCapture();
          capture_error_[0] = '\0';
          RefreshRows();
        }
        ImGui::EndTabItem();
      }
    }
    ro::RoEndTabBar();
  }

  // ── La table ───────────────────────────────────────────────────────────────
  // Hauteur : tout l'espace restant moins le pied de fenêtre. Le natif paginait
  // par 36 ; ici la liste défile.
  const float footer_h = ImGui::GetFrameHeightWithSpacing() +
                         ImGui::GetTextLineHeightWithSpacing() +
                         ImGui::GetStyle().ItemSpacing.y * 2.0f;
  const ImVec2 table_size(0.0f, ImGui::GetContentRegionAvail().y - footer_h);

  // ⚠ Deux identifiants de table DISTINCTS selon le nombre de colonnes : ImGui
  // persiste les largeurs par index sous l'identifiant de la table, et réutiliser
  // le même pour 2 puis 3 colonnes mélangerait les deux mises en page.
  const bool all_mode = (tab_ == kTabAll);
  const int  columns  = all_mode ? 3 : 2;
  const char* table_id = all_mode ? "hotkey_table_all" : "hotkey_table";

  int shown = 0;
  if (ImGui::BeginTable(table_id, columns,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                        table_size)) {
    // Largeurs MESURÉES sur le plus long libellé possible, jamais figées : la
    // police et la langue sont des réglages.
    const float key_col_w =
        ImGui::CalcTextSize(msgstr::Utf8(kMsgUnspecified)).x +
        ImGui::GetFontSize() * 2.0f;
    if (all_mode) {
      float tab_col_w = ImGui::CalcTextSize(TabLabel(kTabBourgeon)).x;
      for (int tab = 0; tab < userhotkey::kCategoryCount; ++tab)
        tab_col_w = (std::max)(tab_col_w, ImGui::CalcTextSize(TabLabel(tab)).x);
      ImGui::TableSetupColumn(i18n::Tr("Onglet"), ImGuiTableColumnFlags_WidthFixed,
                              tab_col_w + ImGui::GetFontSize());
    }
    ImGui::TableSetupColumn(i18n::Tr("Commande"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(i18n::Tr("Touche"), ImGuiTableColumnFlags_WidthFixed,
                            key_col_w);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // ⚠ La table est reconstruite dès qu'une écriture aboutit : on parcourt par
    // INDEX et on sort de la boucle sitôt `rows_` invalidé, sinon la référence
    // `entry` pendrait sur un vecteur réalloué.
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      const Row& entry = rows_[i];
      const userhotkey::Binding& binding = entry.binding;
      if (!Contains(binding.label, filter_) &&
          !Contains(binding.key_name, filter_))
        continue;
      ++shown;

      ImGui::PushID(i);
      ImGui::TableNextRow();
      int column = 0;
      if (all_mode) {
        ImGui::TableSetColumnIndex(column++);
        ImGui::TextColored(kSecondaryText, "%s", TabLabel(entry.tab));
      }
      ImGui::TableSetColumnIndex(column++);
      ImGui::TextUnformatted(binding.label);

      ImGui::TableSetColumnIndex(column);
      bool wrote = false;
      if (IsCapturing(entry)) {
        ImGui::TextColored(ImVec4(0.65f, 0.30f, 0.10f, 1.0f), "%s",
                           i18n::Tr("appuie sur une touche…"));
        wrote = RunCapture(entry);
      } else {
        // Cellule cliquable plutôt que bouton : toute la colonne est une cible,
        // et la ligne garde l'aspect d'un tableau.
        char cell[80];
        if (binding.assigned) {
          std::snprintf(cell, sizeof(cell), "%s", binding.key_name);
        } else {
          // Le natif distingue les deux cas, et c'est une vraie information : un
          // code de touche présent sans nom veut dire « touche que le client ne
          // sait pas nommer », pas « aucune touche ».
          const int msg_id =
              (binding.key_code1 != 0) ? kMsgUnspecified : kMsgNotAssigned;
          const char* text = msgstr::Utf8(msg_id);
          std::snprintf(cell, sizeof(cell), "%s", (text && *text) ? text : "-");
        }
        if (!binding.assigned) ImGui::PushStyleColor(ImGuiCol_Text, kSecondaryText);
        if (ImGui::Selectable(cell, false, ImGuiSelectableFlags_AllowDoubleClick))
          BeginCapture(entry);
        if (!binding.assigned) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s",
                            i18n::Tr("Clic : choisir une touche.  Clic droit : "
                                     "effacer, ou remettre celle du client.  "
                                     "Pendant la capture, Suppr efface et Échap "
                                     "annule."));
        }
        wrote = DrawRowMenu(entry);
      }
      ImGui::PopID();
      if (wrote) break;  // `rows_` sera relu : ne pas continuer sur l'ancien
    }
    ImGui::EndTable();
  }

  // ── Pied ───────────────────────────────────────────────────────────────────
  // Le refus de la dernière capture passe AVANT le décompte : c'est la seule
  // réponse à une action du joueur, elle ne doit pas se lire en second.
  if (capture_error_[0]) {
    ImGui::TextColored(ImVec4(0.75f, 0.10f, 0.10f, 1.0f), "%s", capture_error_);
  } else if (rows_.empty()) {
    ImGui::TextColored(kSecondaryText, "%s",
                       i18n::Tr("Aucun raccourci dans cet onglet."));
  } else if (shown == 0) {
    ImGui::TextColored(kSecondaryText, "%s",
                       i18n::Tr("Aucun résultat pour cette recherche."));
  } else {
    ImGui::TextColored(kSecondaryText, i18n::Tr("%d raccourcis"), shown);
  }

  // ── Battle Mode ────────────────────────────────────────────────────────────
  // Le seul RÉGLAGE de cette fenêtre. Son libellé vient du client (MsgStringTable
  // 1775), comme les onglets. L'état vif est lu dans g_ChangeChatMode : c'est la
  // source de vérité, et elle bouge aussi quand le joueur passe par la native.
  // 🔴 AFFICHAGE OPTIMISTE TANT QUE LA BASCULE EST EN VOL. L'état vif
  // (`g_ChangeChatMode`) est bien la source de vérité — il bouge aussi quand le
  // joueur passe par la native — mais il n'est écrit qu'au TICK, alors que la
  // case se redessine à chaque frame. La lire sans nuance faisait revenir la
  // coche à sa position d'avant pendant les quelques frames de l'aller-retour :
  // un clignotement qu'on prendrait volontiers pour un raté du natif masqué,
  // alors que c'est notre propre différé qui se voit.
  bool battle_mode = (pending_battle_mode_ >= 0) ? (pending_battle_mode_ != 0)
                                                 : BattleModeEnabled();
  const char* battle_label = msgstr::Utf8(kMsgBattleMode);
  if (ro::RoCheckbox((battle_label && *battle_label) ? battle_label
                                                     : i18n::Tr("Mode combat"),
                     &battle_mode)) {
    // Différée au tick : la bascule passe par le OnMsg natif, qui reconfigure la
    // fenêtre de chat — donc du natif, interdit pendant une frame ImGui.
    pending_battle_mode_ = battle_mode ? 1 : 0;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s",
                      i18n::Tr("Entrée ouvre la saisie du chat au lieu de la "
                               "laisser toujours ouverte : les touches de "
                               "raccourci restent actives pendant le combat."));
  }

  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Réinitialiser..."))) confirm_reset_ = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s",
                      i18n::Tr("Remet TOUTES les commandes du jeu aux touches "
                               "par défaut du client. Les actions de Bourgeon "
                               "ne sont pas touchées."));
  }

  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Fenêtre du jeu..."))) OpenNativeForEditing();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s",
                      i18n::Tr("Ouvre la fenêtre de raccourcis du client, telle "
                               "quelle. Cette vue-ci se met à jour dès que tu la "
                               "refermes."));
  }

  // Fermer, calé à DROITE comme dans la fenêtre du client. Position mesurée sur
  // le libellé traduit : « Fermer », « Close » et « Cerrar » n'ont pas la même
  // largeur, et une valeur en dur décollerait le bouton du bord dans deux langues
  // sur trois.
  const float close_w = ro::MaxButtonWidth({i18n::Tr("Fermer")});
  ImGui::SameLine(ImGui::GetContentRegionMax().x - close_w);
  if (ro::RoButton(i18n::Tr("Fermer"), close_w)) Close();

  // ⚠ UNE SEULE chaîne pour l'ouverture ET pour le Begin : ImGui apparie les
  // popups par l'identifiant qui suit `###`, et l'échec est SILENCIEUX.
  const char* kResetPopup = i18n::Tr("Confirmation###bourgeon_hotkey_reset");
  if (confirm_reset_) {
    ImGui::OpenPopup(kResetPopup);
    confirm_reset_ = false;
  }
  if (ro::BeginRoPopupModal(kResetPopup)) {
    // Sinon un Échap fermerait À LA FOIS la confirmation et la fenêtre derrière.
    ro::SuppressEscapeStack();
    const char* question = msgstr::Utf8(kMsgResetConfirm);
    if (!question || !*question) {
      question = i18n::Tr("Les raccourcis enregistrés vont être réinitialisés. "
                          "Continuer ?");
    }
    ImGui::TextUnformatted(question);
    ImGui::Spacing();
    const float ok_w = ro::MaxButtonWidth({i18n::Tr("OK"), i18n::Tr("Annuler")});
    if (ro::RoButton(i18n::Tr("OK"), ok_w)) {
      pending_reset_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), ok_w)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  ro::EndRoWindow();
}
