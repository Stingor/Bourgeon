#include "features/windows/hotkey_settings.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>  // std::size (taille de la liste de détenteurs d'un combo)

#include "bourgeon.h"
#include "features/fx/zone_recorder.h"        // ses 3 touches, volables comme les autres
#include "features/gameplay/keyboard_move.h"  // les 8 touches de déplacement
#include "features/gameplay/player_jump.h"    // la touche de saut
#include "features/hotkey_actions.h"
#include "features/hotkey_util.h"
#include "features/staff_gate.h"  // IsStaff (actions réservées)
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/windows/character_sheet.h"  // EquipPreset (raccourci d'un preset)
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

bool KeyboardMoveEnabled() {
  auto* keyboard_move = Bourgeon::Instance().keyboard_move();
  return keyboard_move && keyboard_move->enabled();
}

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

// Commandes du client RETIRÉES de la table, parce que leur touche ne nous
// appartient pas.
//
// 🔴 « Screenshot » vit sur Impr. écran, que WINDOWS intercepte avant le jeu
// (Outil Capture d'écran sur Windows 11). La commande reste donc VIVANTE dans le
// client — on ne la débranche pas — mais la montrer réglable serait un mensonge :
// la délier n'empêche pas la capture, et lui donner une autre touche ne libère
// pas Impr. écran. Une ligne dont aucune manipulation n'a d'effet visible est
// pire qu'une ligne absente.
//
// La comparaison porte sur le champ EXE de `UserKeys.lua`, l'identifiant INTERNE
// de la commande : il ne se traduit pas d'une langue de client à l'autre,
// contrairement à ce que le natif affiche.
bool IsUnmappableCommand(const char* exe_label) {
  return exe_label && std::strcmp(exe_label, "Screenshot") == 0;
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

void HotkeySettings::OpenFromMenu(int force_tab) {
  open_ = true;
  need_pos_ = true;
  show_panel_ = true;
  rows_dirty_ = true;  // relire AVANT la première frame (docs §5.6 point 11)
  esc_grace_frames_ = 2;
  force_tab_ = force_tab;
}

void HotkeySettings::Close() {
  open_ = false;
  // Une capture laissée en cours gèlerait les raccourcis de Bourgeon sur une
  // fenêtre fermée… jusqu'au prochain PingCapture, qui n'aurait jamais lieu.
  CancelCapture();
  capture_error_[0] = '\0';
  capture_note_[0]  = '\0';
}

void HotkeySettings::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  if (mode_type != ModeMgr::ModeType::kGame) {
    Close();
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

  // Écritures des commandes du client, posées par la capture au rendu. Elles
  // partent TOUTES dans le même tick : voler une touche en demande deux (délier
  // le détenteur, affecter le demandeur), et les laisser à un tick d'écart
  // ferait exister une frame où la touche n'est nulle part — ou, si le joueur
  // referme entre les deux, ne la lui rendrait jamais.
  if (!pending_writes_.empty()) {
    const std::vector<PendingWrite> writes = pending_writes_;
    pending_writes_.clear();
    bool wrote = false;
    for (const PendingWrite& write : writes) {
      if (userhotkey::WriteBinding(write.category, write.command_index, write.key1,
                                   write.key2, write.label)) {
        wrote = true;
      } else {
        LogDiag("HotkeySettings: ecriture refusee (cat={} cmd={})", write.category,
                write.command_index);
      }
    }
    // 🔴 Le fichier n'est PAS le seul destinataire : `UserHotkey_SaveToTable`
    // (0x0059EEF0) rebâtit la charge `/userconfig/save` DEPUIS LE LUA à la
    // sortie propre. Écrire par ce pont suffit donc à se retrouver dans la
    // synchro par compte — aucun drapeau « modifié » à lever. UNE fois pour la
    // rafale : c'est un fichier disque.
    if (wrote) userhotkey::Save();
    rows_dirty_ = true;
    return;
  }

  // 🔴 DÉTRUIRE, pas masquer, et SANS exception depuis qu'aucun bouton ne l'ouvre :
  // tant qu'elle existe, la native détourne et consomme TOUTE la frappe clavier
  // (`UIWindowMgr_OnKeyDown` @0x00A47201). Le seul usage qui lui restait — le
  // [Reset] — passe par `DriveResetDefaults`, qui la fabrique invisible le temps
  // de deux commandes. Une native trouvée ici vient donc forcément d'ailleurs
  // (menu Échap du client remis par le joueur) : elle n'a rien à faire debout.
  if (void* native = uiwnd::SafeFindWindow(kHotkeyWndId))
    uiwnd::SafeCloseWindow(kHotkeyWndId);
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
        if (!userhotkey::ReadBinding(category, row, &entry.binding)) continue;
        if (IsUnmappableCommand(entry.binding.label)) continue;
        // 🔴 SANS SURCHARGE, C'EST LA TOUCHE D'ORIGINE QUI AGIT — et le natif
        // affiche pourtant « Not Assigned ». `UserKeys.lua` ne contient QUE les
        // surcharges (vérifié : `USERKEY_2` vide alors que les commandes
        // d'interface fonctionnent). On lit donc TOUJOURS les deux, et la table
        // les montre côte à côte plutôt que d'en cacher une.
        userhotkey::ReadDefaultBinding(category, entry.binding.command_index,
                                       &entry.fallback);
        rows_.push_back(entry);
      }
    }
  }

  if (!all && tab_ != kTabBourgeon) return;

  // Les lignes de Bourgeon, rendues dans la MÊME struct que les commandes du
  // client : le libellé traduit prend la place du champ EXE, le libellé de combo
  // celle du nom de touche. Le dessin et la recherche n'ont donc qu'un chemin.
  // Le défaut est un `hotkeys::Binding` et non une simple touche : le rapport de
  // bug est livré sur Ctrl+Alt+B, et la colonne « touche d'origine » doit montrer
  // le combo entier — un VK seul y aurait affiché « B », c'est-à-dire faux.
  auto add_own = [this](RowKind kind, int index, const char* label,
                        hotkeys::Binding fallback = {}) {
    Row entry;
    entry.tab   = kTabBourgeon;
    entry.kind  = kind;
    entry.index = index;
    entry.binding.command_index = index;
    std::snprintf(entry.binding.label, sizeof(entry.binding.label), "%s", label);
    // La colonne « touche d'origine » garde son sens ici : ce à quoi on revient.
    if (fallback.vk != 0) {
      entry.fallback.key_code1 = fallback.vk;
      entry.fallback.key_code2 = ModifierVk(fallback.ctrl, fallback.alt, fallback.shift);
      entry.fallback.assigned  = true;
      hotkeys::Label(fallback.vk, fallback.ctrl, fallback.alt, fallback.shift,
                     entry.fallback.key_name, sizeof(entry.fallback.key_name));
    }
    const hotkeys::Binding binding = ReadOwnBinding(entry);
    entry.binding.key_code1 = binding.vk;
    entry.binding.key_code2 = ModifierVk(binding.ctrl, binding.alt, binding.shift);
    entry.binding.assigned  = (binding.vk != 0);
    if (entry.binding.assigned) {
      hotkeys::Label(binding.vk, binding.ctrl, binding.alt, binding.shift,
                     entry.binding.key_name, sizeof(entry.binding.key_name));
    }
    rows_.push_back(entry);
  };

  // 🔴 LE JEU D'ABORD, LES FENÊTRES ENSUITE. Le saut et le déplacement sont les
  // deux seules lignes qui touchent au personnage : les mettre APRÈS quinze
  // ouvertures de fenêtres les enterrait sous la ligne de flottaison, et on ne
  // trouve pas ce qu'on ne voit pas.
  //
  // Le saut se réglait jusqu'ici dans un coin du panneau « Fun », loin de tous les
  // autres raccourcis. Il est ici AUSSI — même valeur, deux endroits pour
  // l'atteindre.
  if (Bourgeon::Instance().player_jump()) add_own(RowKind::kJump, 0, i18n::Tr("Saut"));

  // ⚠ MONTRÉES MÊME QUAND LE DÉPLACEMENT EST ÉTEINT. Les cacher paraissait propre
  // — un réglage qui ne pilote rien — mais il est OFF par défaut : personne ne les
  // voyait, et personne ne pouvait deviner qu'il fallait d'abord activer la
  // fonctionnalité ailleurs pour que ses touches apparaissent ici. Le libellé
  // grisé dit l'inactivité sans rien cacher (cf. le rendu).
  if (Bourgeon::Instance().keyboard_move()) {
    static const char* kMoveLabels[KeyboardMove::kMoveKeyCount] = {
        "Déplacement : avancer",           "Déplacement : reculer",
        "Déplacement : vers la gauche",    "Déplacement : vers la droite",
        "Déplacement : avancer (2)",       "Déplacement : reculer (2)",
        "Déplacement : vers la gauche (2)", "Déplacement : vers la droite (2)",
    };
    // Les défauts viennent d'une instance construite par défaut : ils restent
    // écrits UNE seule fois, dans keyboard_move.h, jamais recopiés ici.
    static const KeyboardMove kDefaults;
    for (int slot = 0; slot < KeyboardMove::kMoveKeyCount; ++slot)
      add_own(RowKind::kMove, slot, i18n::Tr(kMoveLabels[slot]),
              hotkeys::Binding{kDefaults.keys_[slot]});
  }

  for (int i = 0; i < hotkeys::ActionCount(); ++i) {
    // Une action réservée ne se montre PAS à qui ne peut pas s'en servir : une
    // ligne réglable qui ne déclenche rien vaut moins qu'une ligne absente.
    if (hotkeys::ActionAt(i).staff_only && !IsStaff()) continue;
    add_own(RowKind::kAction, i, i18n::Tr(hotkeys::ActionAt(i).label_fr),
            hotkeys::ActionAt(i).default_binding);
  }
}

// ── Liaisons de Bourgeon : un seul point de lecture et d'écriture ────────────

hotkeys::Binding HotkeySettings::ReadOwnBinding(const Row& row) const {
  hotkeys::Binding binding;
  switch (row.kind) {
    case RowKind::kAction:
      return hotkeys::BindingAt(row.index);
    case RowKind::kJump:
      if (auto* jump = Bourgeon::Instance().player_jump()) {
        binding.vk    = jump->key_vk();
        binding.ctrl  = jump->key_ctrl();
        binding.alt   = jump->key_alt();
        binding.shift = jump->key_shift();
      }
      return binding;
    case RowKind::kMove:
      if (auto* keyboard_move = Bourgeon::Instance().keyboard_move()) {
        if (row.index >= 0 && row.index < KeyboardMove::kMoveKeyCount)
          binding.vk = keyboard_move->keys_[row.index];
      }
      return binding;
    case RowKind::kClient:
      break;
  }
  return binding;
}

void HotkeySettings::WriteOwnBinding(const Row& row, const hotkeys::Binding& binding) {
  switch (row.kind) {
    case RowKind::kAction:
      hotkeys::SetBinding(row.index, binding);
      break;
    case RowKind::kJump:
      if (auto* jump = Bourgeon::Instance().player_jump()) {
        jump->key_vk()    = binding.vk;
        jump->key_ctrl()  = binding.ctrl;
        jump->key_alt()   = binding.alt;
        jump->key_shift() = binding.shift;
      }
      break;
    case RowKind::kMove:
      if (auto* keyboard_move = Bourgeon::Instance().keyboard_move()) {
        if (row.index >= 0 && row.index < KeyboardMove::kMoveKeyCount)
          keyboard_move->keys_[row.index] = binding.vk;
      }
      break;
    case RowKind::kClient:
      return;  // celles-là passent par la file d'écritures, pas par ici
  }
  if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
}

void HotkeySettings::QueueClientWrite(int category, int command_index, int key1,
                                      int key2, const char* label) {
  PendingWrite write;
  write.category = category;
  write.command_index = command_index;
  write.key1 = key1;
  write.key2 = key2;
  std::snprintf(write.label, sizeof(write.label), "%s", label ? label : "");
  pending_writes_.push_back(write);
}

// ── Le VOL d'une touche ──────────────────────────────────────────────────────
//
// Chaque monde se délie chez lui. Une seule règle commune : après ça, la touche
// ne doit plus appartenir à personne d'autre — sinon on aurait écrit la nouvelle
// affectation ET laissé l'ancienne, exactement le défaut qu'on corrige.
bool HotkeySettings::ReleaseConflict(const hotkeys::ConflictOwner& owner) {
  switch (owner.owner) {
    case hotkeys::Owner::kClientCommand:
      // Entrée SANS `KEY1` : la commande cesse vraiment de répondre. Ne PAS se
      // contenter d'omettre l'entrée — l'absence laisse au contraire agir la
      // touche d'origine (les trois états, docs/game_option_re.md §4.9).
      if (owner.command_index < 0) return false;
      QueueClientWrite(owner.category, owner.command_index, 0, 0, owner.label);
      return true;

    case hotkeys::Owner::kAction:
      hotkeys::SetBinding(owner.index, hotkeys::Binding());
      break;

    case hotkeys::Owner::kJump:
      if (auto* jump = Bourgeon::Instance().player_jump()) {
        jump->key_vk()    = 0;
        jump->key_ctrl()  = false;
        jump->key_alt()   = false;
        jump->key_shift() = false;
      }
      break;

    case hotkeys::Owner::kKeyboardMove:
      if (auto* keyboard_move = Bourgeon::Instance().keyboard_move()) {
        if (owner.index < 0 || owner.index >= KeyboardMove::kMoveKeyCount) return false;
        keyboard_move->keys_[owner.index] = 0;
      }
      break;

    case hotkeys::Owner::kZoneRecorder:
      if (auto* zone_recorder = Bourgeon::Instance().zone_recorder()) {
        switch (owner.index) {
          case hotkeys::kZoneRecKeyRecord:
            zone_recorder->key_vk() = 0;
            zone_recorder->key_ctrl() = zone_recorder->key_alt() =
                zone_recorder->key_shift() = false;
            break;
          case hotkeys::kZoneRecKeySelect:
            zone_recorder->sel_key_vk() = 0;
            zone_recorder->sel_key_ctrl() = zone_recorder->sel_key_alt() =
                zone_recorder->sel_key_shift() = false;
            break;
          case hotkeys::kZoneRecKeyShot:
            zone_recorder->shot_key_vk() = 0;
            zone_recorder->shot_key_ctrl() = zone_recorder->shot_key_alt() =
                zone_recorder->shot_key_shift() = false;
            break;
          default: return false;
        }
      }
      break;

    case hotkeys::Owner::kEquipPreset:
      if (auto* character_sheet = Bourgeon::Instance().character_sheet()) {
        std::vector<EquipPreset>& presets = character_sheet->equip_presets();
        if (owner.index < 0 || owner.index >= static_cast<int>(presets.size()))
          return false;
        EquipPreset& preset = presets[owner.index];
        preset.hotkey_vk    = 0;
        preset.hotkey_ctrl  = false;
        preset.hotkey_alt   = false;
        preset.hotkey_shift = false;
      }
      break;

    // 🔴 Combo RÉSERVÉ (Alt+F) : il n'appartient à aucun réglage, il n'y a rien à
    // délier. L'appelant a déjà refusé sur `releasable`, mais on ne rend pas
    // « fait » pour autant — un jour un nouveau propriétaire non libérable
    // apparaîtra, et il doit buter ici plutôt que de passer.
    case hotkeys::Owner::kNone:
      return false;
  }

  // Les cinq mondes de Bourgeon vivent dans le MÊME yaml : une seule sauvegarde
  // les couvre tous (elle est anti-rebondie).
  if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  return true;
}

// ── Menu contextuel d'une ligne ──────────────────────────────────────────────

bool HotkeySettings::DrawRowMenu(const Row& row) {
  // Attaché à la cellule qui précède (la colonne des touches). L'identifiant est
  // celui poussé par la boucle : deux lignes ne peuvent pas se le disputer.
  if (!ImGui::BeginPopupContextItem("##hotkey_row_menu")) return false;

  bool wrote = false;

  if (row.kind != RowKind::kClient) {
    // Nos réglages, eux, savent vraiment n'avoir aucune touche : notre stockage
    // sait écrire « rien ». Une direction sans touche est simplement injouable,
    // ce qui est un choix valide (ne jouer qu'aux flèches, par exemple).
    if (ImGui::MenuItem(i18n::Tr("Effacer la touche"), nullptr, false,
                        row.binding.assigned)) {
      WriteOwnBinding(row, hotkeys::Binding());
      capture_error_[0] = '\0';
      wrote = true;
    }
    ImGui::EndPopup();
    if (wrote) rows_dirty_ = true;
    return wrote;
  }

  // ── Commandes du CLIENT ────────────────────────────────────────────────────
  // Délier pour de bon : `UserKeys.lua` reçoit une entrée SANS `KEY1`, qui masque
  // la touche d'origine — c'est ce que fait l'Échap du natif, et la commande cesse
  // vraiment de répondre. À ne pas confondre avec l'ABSENCE d'entrée, qui laisse
  // au contraire la touche d'origine agir.
  if (ImGui::MenuItem(i18n::Tr("Retirer la touche"), nullptr, false,
                      row.binding.assigned || row.binding.key_code1 != 0)) {
    // 0/0 = « aucune touche », la convention du pont — le Lua écrit alors une
    // entrée sans `KEY1`.
    QueueClientWrite(row.category, row.binding.command_index, 0, 0, row.binding.label);
    capture_error_[0] = '\0';
    capture_note_[0]  = '\0';
    wrote = true;
  }

  ImGui::EndPopup();
  if (wrote) rows_dirty_ = true;
  return wrote;
}

// ── Capture ──────────────────────────────────────────────────────────────────

bool HotkeySettings::IsCapturing(const Row& row) const {
  if (!capturing_ || capture_kind_ != row.kind) return false;
  if (row.kind == RowKind::kClient)
    return capture_category_ == row.category &&
           capture_command_ == row.binding.command_index;
  return capture_index_ == row.index;
}

void HotkeySettings::BeginCapture(const Row& row) {
  capturing_ = true;
  capture_error_[0] = '\0';
  capture_note_[0]  = '\0';
  capture_kind_     = row.kind;
  capture_index_    = row.index;
  capture_category_ = row.category;
  capture_command_  = row.binding.command_index;
}

void HotkeySettings::CancelCapture() {
  capturing_ = false;
  capture_kind_     = RowKind::kClient;
  capture_index_    = -1;
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
  // `hotkeys::CaptureActionVk` qui ne rend que lettres, chiffres, F1-F12, Espace
  // et Tab.
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    CancelCapture();
    return false;
  }

  // 🔴 DEUX JEUX DE TOUCHES, ET LA DISTINCTION EST DE FOND. Une commande du
  // CLIENT part par SON dispatch, qui accepte bien plus que `ProcessPushButton` —
  // Impr. écran, le pavé numérique, la ponctuation. Une action de BOURGEON, elle,
  // ne nous revient que par `OnKeyDown` : lui offrir une touche que le jeu ne
  // route pas donnerait un raccourci muet.
  //
  // ⚠ Suppr et Retour arrière n'effacent PLUS pendant la capture : ce sont des
  // touches que le client sait affecter, et les réserver à un geste les rendait
  // impossibles à choisir. L'effacement est au clic droit, où il est visible.
  // Le déplacement, lui, prend le jeu LARGE : ses touches par défaut sont les
  // flèches, et c'est un usage à la manette, pas un raccourci de commande.
  const bool own = (row.kind != RowKind::kClient);
  const int vkey = (own && row.kind != RowKind::kMove) ? hotkeys::CaptureActionVk()
                                                      : hotkeys::CaptureAnyVk();
  if (vkey == 0) return false;

  ImGuiIO& io = ImGui::GetIO();
  const bool ctrl = io.KeyCtrl;
  const bool alt = io.KeyAlt;
  const bool shift = io.KeyShift;

  // ⚠ Le client ne retient qu'UN modificateur (`key2`) : lui en passer deux en
  // perdrait un en silence, et la touche affectée ne serait pas celle affichée.
  // Nos actions et le saut, eux, en acceptent autant qu'on veut.
  if (!own && ModifierCount(ctrl, alt, shift) > 1) {
    std::snprintf(capture_error_, sizeof(capture_error_), "%s",
                  i18n::Tr("Le jeu ne retient qu'un seul modificateur (Ctrl, Alt "
                           "ou Maj) par raccourci."));
    return false;
  }

  // 🔴 AUCUN modificateur pour le déplacement, et le refuser vaut mieux que
  // l'accepter : `KeyboardMove::Update` s'arrête net dès que Ctrl, Alt ou Maj est
  // enfoncé (ce sont des raccourcis du jeu, et Maj sert au clic forcé). Une
  // direction sur « Ctrl+Z » serait donc acceptée, affichée… et morte.
  if (row.kind == RowKind::kMove && ModifierCount(ctrl, alt, shift) > 0) {
    std::snprintf(capture_error_, sizeof(capture_error_), "%s",
                  i18n::Tr("Le déplacement au clavier ne prend pas de "
                           "modificateur : la marche s'arrête dès que Ctrl, Alt "
                           "ou Maj est enfoncé."));
    return false;
  }

  hotkeys::Owner owner = hotkeys::Owner::kClientCommand;
  int self_index = hotkeys::ClientSelf(row.category, row.binding.command_index);
  switch (row.kind) {
    case RowKind::kAction: owner = hotkeys::Owner::kAction;        self_index = row.index; break;
    case RowKind::kJump:   owner = hotkeys::Owner::kJump;          self_index = -1;        break;
    case RowKind::kMove:   owner = hotkeys::Owner::kKeyboardMove;  self_index = row.index; break;
    case RowKind::kClient: break;
  }

  // 🔴 ON VOLE LA TOUCHE À QUI LA DÉTIENT, comme le natif. Refuser laissait la
  // touche à l'ancienne fonction alors que le joueur venait de la donner à une
  // autre — et lui laissait le travail de retrouver la ligne coupable.
  //
  // La liste est DIMENSIONNÉE LARGE et son débordement est traité : une touche
  // peut avoir plusieurs détenteurs (les catégories 0 et 3 sont deux pages de la
  // même barre), et n'en délier qu'une partie laisserait le doublon debout —
  // exactement le défaut qu'on corrige. Si on n'a pas pu tous les voir, on
  // refuse plutôt que de voler à moitié.
  hotkeys::ConflictOwner owners[16];
  const int owner_count = hotkeys::FindConflicts(
      vkey, ctrl, alt, shift, owner, self_index, owners, static_cast<int>(std::size(owners)));
  if (owner_count > static_cast<int>(std::size(owners))) {
    std::snprintf(capture_error_, sizeof(capture_error_), "%s",
                  i18n::Tr("Cette touche est prise par trop de fonctions à la fois : "
                           "libère-en quelques-unes d'abord."));
    return false;
  }
  for (int i = 0; i < owner_count; ++i) {
    if (owners[i].releasable) continue;
    std::snprintf(capture_error_, sizeof(capture_error_),
                  i18n::Tr("Réservé à %s : cette touche n'est pas réattribuable."),
                  owners[i].what);
    return false;
  }
  // Libérer AVANT d'affecter : les écritures du client partent en file, dans cet
  // ordre, et une commande déliée puis réaffectée dans la même passe (ce que fait
  // un joueur qui déplace une touche d'une ligne à l'autre) doit finir affectée.
  char note[320] = {0};
  for (int i = 0; i < owner_count; ++i) {
    if (!ReleaseConflict(owners[i])) {
      std::snprintf(capture_error_, sizeof(capture_error_),
                    i18n::Tr("Déjà utilisé par %s, et je ne sais pas la lui retirer."),
                    owners[i].what);
      // On jette les libérations du client encore en file — rien n'a été écrit.
      // ⚠ Celles de NOS réglages, déjà appliquées, ne se reprennent pas : le seul
      // échec possible est un index devenu invalide, cas où la liaison visée
      // n'existe de toute façon plus.
      pending_writes_.clear();
      return false;
    }
    const int used = static_cast<int>(std::strlen(note));
    std::snprintf(note + used, sizeof(note) - used, "%s%s", used ? ", " : "",
                  owners[i].what);
  }
  if (note[0]) {
    char combo[64];
    hotkeys::Label(vkey, ctrl, alt, shift, combo, sizeof(combo));
    // ⚠ Tournure choisie pour les libellés de `FindConflicts`, qui commencent
    // TOUS par un article (« le saut », « l'action « X » ») : « retirée à %s »
    // aurait donné « à le saut ».
    std::snprintf(capture_note_, sizeof(capture_note_),
                  i18n::Tr("%s était utilisé par %s — touche reprise."), combo, note);
  } else {
    capture_note_[0] = '\0';
  }

  if (own) {
    hotkeys::Binding binding;
    binding.vk = vkey;
    binding.ctrl = ctrl;
    binding.alt = alt;
    binding.shift = shift;
    WriteOwnBinding(row, binding);
  } else {
    // Différée au tick : ces deux ponts appellent le Lua du client.
    QueueClientWrite(row.category, row.binding.command_index, vkey,
                     ModifierVk(ctrl, alt, shift), row.binding.label);
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
      // Sélection d'office demandée par un appelant (la fenêtre des macros).
      // Consommée à la première frame : sans ça l'onglet redeviendrait actif à
      // chaque frame et le joueur ne pourrait plus en changer.
      const ImGuiTabItemFlags tab_flags =
          (force_tab_ == tab) ? ImGuiTabItemFlags_SetSelected : 0;
      if (ImGui::BeginTabItem(tab_id, nullptr, tab_flags)) {
        if (tab_ != tab) {
          tab_ = tab;
          // Changer d'onglet en pleine capture laisserait une ligne invisible en
          // attente d'une touche, et le premier appui la lui donnerait.
          CancelCapture();
          capture_error_[0] = '\0';
          capture_note_[0]  = '\0';
          RefreshRows();
        }
        ImGui::EndTabItem();
      }
    }
    ro::RoEndTabBar();
  }
  force_tab_ = -1;

  // ── La table ───────────────────────────────────────────────────────────────
  // Hauteur : tout l'espace restant moins le pied de fenêtre. Le natif paginait
  // par 36 ; ici la liste défile.
  const float footer_h = ImGui::GetFrameHeightWithSpacing() +
                         ImGui::GetTextLineHeightWithSpacing() +
                         ImGui::GetStyle().ItemSpacing.y * 2.0f;
  const ImVec2 table_size(0.0f, ImGui::GetContentRegionAvail().y - footer_h);

  // ⚠ Deux identifiants de table DISTINCTS selon le nombre de colonnes : ImGui
  // persiste les largeurs par index sous l'identifiant de la table, et réutiliser
  // le même pour 3 puis 4 colonnes mélangerait les deux mises en page.
  // Le suffixe `_v2` date du passage à DEUX colonnes de touches : sans lui, les
  // largeurs mémorisées de l'ancienne disposition auraient été réappliquées à la
  // nouvelle, colonne par colonne, sans rapport avec leur contenu.
  const bool all_mode = (tab_ == kTabAll);
  const int  columns  = all_mode ? 4 : 3;
  const char* table_id = all_mode ? "hotkey_table_all_v2" : "hotkey_table_v2";

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
    // DEUX COLONNES DE TOUCHES : ce que le client livre, et ce qui agit
    // réellement. Le natif ne montre que la seconde, si bien qu'on ne peut jamais
    // savoir ce qu'on a changé ni à quoi revenir. Les trois états du client se
    // lisent alors d'un coup d'œil :
    //   colonnes IDENTIQUES  -> rien n'a été touché ;
    //   colonnes DIFFÉRENTES -> le joueur a choisi sa touche ;
    //   « actuelle » vide    -> la commande est DÉLIÉE (entrée sans `KEY1`).
    ImGui::TableSetupColumn(i18n::Tr("Touche d'origine"),
                            ImGuiTableColumnFlags_WidthFixed, key_col_w);
    ImGui::TableSetupColumn(i18n::Tr("Touche actuelle"),
                            ImGuiTableColumnFlags_WidthFixed, key_col_w);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // ⚠ La table est reconstruite dès qu'une écriture aboutit : on parcourt par
    // INDEX et on sort de la boucle sitôt `rows_` invalidé, sinon la référence
    // `entry` pendrait sur un vecteur réalloué.
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      const Row& entry = rows_[i];
      const userhotkey::Binding& binding = entry.binding;
      if (!Contains(binding.label, filter_) &&
          !Contains(binding.key_name, filter_) &&
          !Contains(entry.fallback.key_name, filter_))
        continue;
      ++shown;

      ImGui::PushID(i);
      ImGui::TableNextRow();
      int column = 0;
      if (all_mode) {
        ImGui::TableSetColumnIndex(column++);
        ImGui::TextColored(kSecondaryText, "%s", TabLabel(entry.tab));
      }
      // Une ligne de déplacement dont la fonctionnalité est éteinte est GRISÉE :
      // sa touche est réglable, mais elle ne pilote rien tant que le déplacement
      // au clavier n'est pas activé dans le panneau. Le dire en couleur évite
      // d'avoir à cacher la ligne — ce qui la rendait introuvable.
      const bool inert = (entry.kind == RowKind::kMove) && !KeyboardMoveEnabled();
      ImGui::TableSetColumnIndex(column++);
      if (inert) ImGui::TextColored(kSecondaryText, "%s", binding.label);
      else       ImGui::TextUnformatted(binding.label);
      if (inert && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          i18n::Tr("Le déplacement au clavier est désactivé : "
                                   "cette touche ne fera rien tant qu'il ne sera "
                                   "pas activé dans les réglages de Bourgeon."));
      }

      // ── Touche d'origine ─────────────────────────────────────────────────
      // Non cliquable : c'est un repère, pas un réglage — ce que le client
      // donnerait sur une installation neuve, et donc ce à quoi on revient.
      ImGui::TableSetColumnIndex(column++);
      ImGui::TextColored(kSecondaryText, "%s",
                         entry.fallback.assigned ? entry.fallback.key_name : "-");

      // ── Touche actuelle ──────────────────────────────────────────────────
      ImGui::TableSetColumnIndex(column);
      bool wrote = false;
      if (IsCapturing(entry)) {
        ImGui::TextColored(ImVec4(0.65f, 0.30f, 0.10f, 1.0f), "%s",
                           i18n::Tr("appuie sur une touche…"));
        wrote = RunCapture(entry);
      } else {
        // Cellule cliquable plutôt que bouton : toute la colonne est une cible,
        // et la ligne garde l'aspect d'un tableau.
        char cell[96];
        if (binding.assigned) {
          std::snprintf(cell, sizeof(cell), "%s", binding.key_name);
        } else if (binding.key_code1 != 0) {
          // Un code de touche présent sans nom veut dire « touche que le client
          // ne sait pas nommer », pas « aucune touche » — le natif fait la même
          // distinction, et c'est une vraie information.
          const char* text = msgstr::Utf8(kMsgUnspecified);
          std::snprintf(cell, sizeof(cell), "%s", (text && *text) ? text : "?");
        } else {
          // Vraiment aucune touche : soit la commande n'en a jamais eu, soit elle
          // a été DÉLIÉE. La colonne d'à côté distingue les deux — si elle montre
          // une touche, c'est que le joueur l'a retirée.
          std::snprintf(cell, sizeof(cell), "%s", "-");
        }
        if (!binding.assigned) ImGui::PushStyleColor(ImGuiCol_Text, kSecondaryText);
        if (ImGui::Selectable(cell, false, ImGuiSelectableFlags_AllowDoubleClick))
          BeginCapture(entry);
        if (!binding.assigned) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s",
                            i18n::Tr("Clic : choisir une touche (Échap annule).  "
                                     "Clic droit : retirer la touche.  "
                                     "Une touche déjà prise est retirée à son "
                                     "ancienne fonction."));
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
  } else if (capture_note_[0]) {
    // Le VOL a réussi : ce n'est donc pas une erreur, mais ce n'est pas non plus
    // une banalité. La touche vient d'être retirée à quelqu'un — parfois à un
    // réglage qui n'a aucune ligne ici (un preset d'équipement, une touche de
    // l'enregistreur de zone). Sans cette ligne, il disparaîtrait sans un mot.
    ImGui::TextColored(ImVec4(0.85f, 0.62f, 0.15f, 1.0f), "%s", capture_note_);
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

  // ⛔ PLUS DE BOUTON « Fenêtre du jeu… ». Il ouvrait la native telle quelle, à
  // l'époque où elle seule savait remettre les défauts. Ce n'est plus vrai :
  // `DriveResetDefaults` la fabrique INVISIBLE, lui envoie ses deux commandes et
  // la détruit — le joueur ne la voit jamais. Le garder, c'était laisser une
  // porte vers une fenêtre qui vole tout le clavier tant qu'elle vit
  // (`UIWindowMgr_OnKeyDown`), pour un service que le panneau rend déjà.
  // Symétrique du « Réglages natifs… » retiré de Game Settings (docs §5.9).

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
