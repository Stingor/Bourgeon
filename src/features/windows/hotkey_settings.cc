#include "features/windows/hotkey_settings.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "imgui.h"
#include "ragnarok/msgstring.h"
#include "ragnarok/uiwnd.h"
#include "ui/ro_imgui.h"
#include "utils/i18n.h"

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

// ── Battle Mode ──────────────────────────────────────────────────────────────
// g_ChangeChatMode : l'état du mode combat, un OCTET. Écrit par le message 213 de
// UIHotKeyWnd, lu par la fenêtre de chat.
constexpr uintptr_t kChangeChatModeAddr = 0x0131f50e;

// Message natif de la bascule, et l'emplacement du bouton bascule dans la fenêtre.
// Le handler REFUSE le message si son premier argument n'est pas ce bouton-là
// (`if (param_1 != *(this + 264)) return 0;`) — d'où la lecture de +0x108.
constexpr int kMsgToggleBattleMode = 213;
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

const char* TabLabel(int tab) {
  if (tab < 0 || tab >= userhotkey::kCategoryCount) return "";
  const char* s = msgstr::Utf8(kTabMsgIds[tab]);
  return (s && *s) ? s : kTabFallback[tab];
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

  const int first = (tab_ == kTabAll) ? 0 : tab_;
  const int last  = (tab_ == kTabAll) ? userhotkey::kCategoryCount - 1 : tab_;

  for (int tab = first; tab <= last; ++tab) {
    const int category = userhotkey::CategoryForTab(tab);
    const int count = userhotkey::RowCount(category);
    for (int row = 0; row < count; ++row) {
      Row entry;
      entry.tab = tab;
      if (userhotkey::ReadBinding(category, row, &entry.binding))
        rows_.push_back(entry);
    }
  }
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
      // ⚠ Le handler REFUSE le message si son premier argument n'est pas le bouton
      // bascule de la fenêtre : on le lui repasse tel quel.
      void* toggle = *reinterpret_cast<void**>(
          reinterpret_cast<uint8_t*>(win) + kOffToggleButton);
      uiwnd::OnMsg(win, kMsgToggleBattleMode, /*p2=*/0, /*p3=*/on ? 1 : 0,
                   /*p4=*/0, /*p5=*/0,
                   /*arg0=*/static_cast<int>(reinterpret_cast<uintptr_t>(toggle)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  routing_ = false;

  if (uiwnd::SafeFindWindow(kHotkeyWndId)) uiwnd::SafeCloseWindow(kHotkeyWndId);
}

void HotkeySettings::OpenNativeForEditing() {
  // Interim : le remappage reste au natif tant que les ponts Lua d'écriture ne
  // sont pas RE'd. Appelé depuis OnRenderUI, donc on DIFFÈRE l'ouverture au tick.
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
  const int tab_order[] = {kTabAll, 0, 1, 2, 3};
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
      float tab_col_w = 0.0f;
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

    for (const Row& entry : rows_) {
      const userhotkey::Binding& binding = entry.binding;
      if (!Contains(binding.label, filter_) &&
          !Contains(binding.key_name, filter_))
        continue;
      ++shown;

      ImGui::TableNextRow();
      int column = 0;
      if (all_mode) {
        ImGui::TableSetColumnIndex(column++);
        ImGui::TextColored(kSecondaryText, "%s", TabLabel(entry.tab));
      }
      ImGui::TableSetColumnIndex(column++);
      ImGui::TextUnformatted(binding.label);

      ImGui::TableSetColumnIndex(column);
      if (binding.assigned) {
        ImGui::TextUnformatted(binding.key_name);
      } else {
        // Le natif distingue les deux cas, et c'est une vraie information : un
        // code de touche présent sans nom veut dire « touche que le client ne
        // sait pas nommer », pas « aucune touche ».
        const int msg_id =
            (binding.key_code1 != 0) ? kMsgUnspecified : kMsgNotAssigned;
        const char* text = msgstr::Utf8(msg_id);
        ImGui::TextColored(kSecondaryText, "%s", (text && *text) ? text : "-");
      }
    }
    ImGui::EndTable();
  }

  // ── Pied ───────────────────────────────────────────────────────────────────
  if (rows_.empty()) {
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
  bool battle_mode = BattleModeEnabled();
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
  if (ro::RoButton(i18n::Tr("Modifier les touches..."))) OpenNativeForEditing();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s",
                      i18n::Tr("Ouvre la fenêtre de raccourcis du jeu, la seule "
                               "qui sache encore écrire les touches. Cette "
                               "vue-ci se met à jour dès que tu la refermes."));
  }

  ro::EndRoWindow();
}
