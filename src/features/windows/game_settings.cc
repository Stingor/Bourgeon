#include "features/windows/game_settings.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "imgui.h"
#include "ragnarok/game_settings.h"
#include "ragnarok/msgstring.h"
#include "ragnarok/uiwnd.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

namespace {

// CUIGameSettingsUI — vtable 0x01047D7C, objet 0x100, fabrique générique.
constexpr int kGameSettingsWndId = 0x271E;  // 10014

// Les libellés d'onglet du CLIENT (MsgStringTable).
//
// 🔴 Même choix que la table des raccourcis, et pour la même raison : le CONTENU
// de ces onglets est en anglais — ce sont les `Title` du Lua du serveur, qu'on ne
// peut pas traduire sans figer une copie. Des onglets français au-dessus de
// lignes anglaises donneraient une fenêtre bâtarde. Ceux du client suivent d'eux
// -mêmes sa langue. Nos propres ajouts (« Tout », les boutons) passent, eux, par
// i18n.
//
// ⚠ Le cinquième id n'est PAS contigu aux quatre autres : 4146 est déjà
// « Emblem Border ». Une boucle `base + i` afficherait ce libellé comme onglet.
//
// 🔴 TOUS PASSENT PAR `Utf8Or`, JAMAIS PAR `Utf8`. Constaté en jeu le 2026-08-14 :
// sur Moonlight, **4216 et 4217 rendent « NO MSG »** alors que 4142-4146, juste
// à côté, répondent correctement — et que les deux textes sont bel et bien
// présents dans `data\msgstringtable.csv` (calibré : 1483 = « Game Options »,
// 1548 = la confirmation de retour au point de sauvegarde). La cause exacte n'est
// PAS établie : le `.txt` du GRF s'arrête à 4024 entrées, le `.csv` en a 4361, et
// je n'ai pas pu lire la table en mémoire du client pour trancher lequel il
// charge vraiment. Le repli, lui, ne dépend pas de la réponse.
constexpr int kMsgTabBasic    = 4142;
constexpr int kMsgTabEffect   = 4143;
constexpr int kMsgTabControl  = 4144;
constexpr int kMsgTabEtc      = 4217;
constexpr int kMsgEmblemFrame = 4146;  // MSI_GAME_SETTINGS_EMBLEM_FRAME
constexpr int kMsgLoginNotify = 4216;  // MSI_GAME_SETTINGS_LOGINOUT
// ⚠ `MSI_OPTION_ESC` est à **4241**, pas 4240 : 4240 est `MSI_EXPANSION_MINIMAP`
// (« Expanded Minimap »). Le §3.8 du doc portait la mauvaise valeur, et sans le
// repli la fenêtre se serait intitulée « Expanded Minimap ».
constexpr int kMsgWindowTitle = 4241;
// « Set Basic Settings? » — la confirmation que le natif pose sur son [Reset].
constexpr int kMsgResetConfirm = 3166;

// ── Les deux bascules câblées en dur de l'onglet Basique ────────────────────
// 🔴 Ce sont les SEULS identifiants d'option que ce fichier connaisse, et ce
// n'est pas une entorse à la règle « ne rien recopier » : ces deux-là ne sont pas
// dans la table pilotée par données, le CLIENT les code lui-même en dur dans ses
// groupes de la page Basique (`0x009EEF50` et `0x009EF000`, docs §3.3). Les
// prendre ici, c'est reproduire son câblage — pas dupliquer ses données.
constexpr int kTtEmblemFrame = 0xf3;  // TT_EMBLEM_FRAME_ON_OFF  — bordure d'emblème
constexpr int kTtLoginNotify = 0xa5;  // TT_LOGINOUT_ON_OFF      — connexion/déconnexion

// 🔴 Texte secondaire : couleur EXPLICITE, jamais ImGui::TextDisabled. Le corps
// d'une fenêtre RO est CLAIR, et le gris de TextDisabled y est illisible
// (feedback_imgui_ro_light_body_colors).
const ImVec4 kSecondaryText(0.42f, 0.38f, 0.32f, 1.0f);
// Le marqueur « ce réglage n'est plus à son défaut ».
const ImVec4 kChangedText(0.65f, 0.30f, 0.10f, 1.0f);

const char* TabLabel(int tab) {
  switch (tab) {
    case gamesettings::kTabEffect:
      return msgstr::Utf8Or(kMsgTabEffect, i18n::Tr("Effets"));
    case gamesettings::kTabControl:
      return msgstr::Utf8Or(kMsgTabControl, i18n::Tr("Contrôles"));
    case gamesettings::kTabEtc:
      return msgstr::Utf8Or(kMsgTabEtc, i18n::Tr("Divers"));
    default:
      return "";
  }
}

// Recherche insensible à la casse, sur une sous-chaîne.
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

// ─────────────────────────────────────────────────────────────────────────────

void GameSettings::HandleNativeCreation(void* win) {
  // C'est nous qui venons d'ouvrir la native pour ses réglages graphiques : ne
  // pas la prendre pour une demande d'ouverture de notre panneau, et surtout NE
  // PAS la masquer — le joueur doit la voir pour s'en servir.
  if (routing_) return;
  if (!imgui_enabled_) return;

  uiwnd::SafeSetVisible(win, false);

  if (open_) {
    Close();
  } else {
    OpenFromMenu();
  }
}

void GameSettings::OpenFromMenu() {
  open_ = true;
  need_pos_ = true;
  show_panel_ = true;
  rows_dirty_ = true;  // relire AVANT la première frame (docs §5.6 point 11)
  esc_grace_frames_ = 2;
}

void GameSettings::Close() {
  open_ = false;
  confirm_reset_ = false;
}

void GameSettings::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  if (mode_type != ModeMgr::ModeType::kGame) {
    Close();
    native_open_ = false;
    rows_.clear();
    rows_dirty_ = true;
  }
}

void GameSettings::OpenNativeForGraphics() {
  routing_ = true;
  uiwnd::MakeWindow(kGameSettingsWndId);
  routing_ = false;
  native_open_ = true;
  // Le panneau se retire pendant que le joueur est dans la fenêtre du client :
  // deux écrans de réglages superposés qui disent la même chose, c'est une
  // question de plus pour le joueur, pas une commodité.
  Close();
}

void GameSettings::OnTick() {
  if (!imgui_enabled_) {
    if (open_) Close();
    return;
  }
  if (!Bourgeon::Instance().IsGameActive()) {
    if (open_) Close();
    native_open_ = false;
    return;
  }

  // La native est ouverte à la demande du joueur (réglages graphiques) : on ne la
  // détruit pas, on attend qu'il la referme.
  if (native_open_) {
    if (!uiwnd::FindWindow(kGameSettingsWndId)) {
      native_open_ = false;
      rows_dirty_ = true;  // il a pu changer des réglages là-bas
    }
    return;
  }

  if (pending_open_native_) {
    pending_open_native_ = false;
    OpenNativeForGraphics();
    return;
  }

  if (pending_reset_) {
    pending_reset_ = false;
    gamesettings::ResetAllToDefault();
    rows_dirty_ = true;
    return;
  }

  if (pending_write_.valid) {
    const PendingWrite write = pending_write_;
    pending_write_ = PendingWrite();
    if (write.exec) {
      gamesettings::Exec(write.id);
    } else if (write.slash) {
      // Option absente de la table des drapeaux : seul le chemin par nom de
      // commande sait l'y insérer. Si le client ne connaît pas la commande, on
      // retombe sur l'écriture ordinaire plutôt que de ne rien faire.
      if (!gamesettings::SetOnByCommand(write.slash, write.on))
        gamesettings::SetOn(write.id, write.on);
    } else {
      gamesettings::SetOn(write.id, write.on);
    }
    rows_dirty_ = true;
    return;
  }

  // 🔴 DÉTRUIRE, pas masquer : le hook de création l'a rendue invisible, mais une
  // native vivante avale un appui sur deux et garde le clavier.
  if (uiwnd::FindWindow(kGameSettingsWndId)) uiwnd::CloseWindow(kGameSettingsWndId);
}

void GameSettings::RefreshRows() {
  rows_.clear();
  rows_dirty_ = false;

  const int count = gamesettings::Count();
  rows_.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    Row row;
    if (!gamesettings::At(i, &row.option)) continue;
    if (row.option.title[0] == '\0') continue;  // ligne incomplète côté Lua
    row.value = gamesettings::IsOn(row.option.id);
    rows_.push_back(row);
  }
}

// ─────────────────────────────────────────────────────────────────────────────

void GameSettings::OnRenderUI() {
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
    ImGui::SetNextWindowSize(ImVec2(ro::Px(460.0f), ro::Px(440.0f)),
                             ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }

  // Titre du CLIENT, suffixe ### figé : la fenêtre garde position et taille d'une
  // langue à l'autre.
  char title[128];
  std::snprintf(title, sizeof(title), "%s###bourgeon_game_settings",
                msgstr::Utf8Or(kMsgWindowTitle, i18n::Tr("Réglages du jeu")));

  const bool begun = ro::BeginRoWindow(title, &show_panel_);
  if (!show_panel_) { Close(); show_panel_ = true; }
  if (!begun) { ro::EndRoWindow(); return; }

  if (!gamesettings::Available()) {
    ImGui::TextColored(kSecondaryText, "%s",
                       i18n::Tr("Les réglages du client ne sont pas encore chargés."));
    ro::EndRoWindow();
    return;
  }

  // ── Recherche ──────────────────────────────────────────────────────────────
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##gs_filter",
                           i18n::Tr("Rechercher un réglage ou une commande..."),
                           filter_, sizeof(filter_));

  // ── Onglets ────────────────────────────────────────────────────────────────
  // « Tout » en tête — c'est lui qui donne son sens à la recherche.
  const int tab_order[] = {kTabAll, kTabBasic, gamesettings::kTabEffect,
                           gamesettings::kTabControl, gamesettings::kTabEtc};
  if (ro::RoBeginTabBar("gs_tabs")) {
    for (int tab : tab_order) {
      const char* label =
          (tab == kTabAll)     ? i18n::Tr("Tout")
          : (tab == kTabBasic) ? msgstr::Utf8Or(kMsgTabBasic, i18n::Tr("Basique"))
                               : TabLabel(tab);
      // Identifiant TECHNIQUE et stable : le libellé vient du client et change
      // avec sa langue, ce qui recréerait l'onglet à chaque bascule.
      char tab_id[96];
      std::snprintf(tab_id, sizeof(tab_id), "%s###gs_tab_%d", label, tab);
      if (ImGui::BeginTabItem(tab_id)) {
        tab_ = tab;
        ImGui::EndTabItem();
      }
    }
    ro::RoEndTabBar();
  }

  // ── Le corps ───────────────────────────────────────────────────────────────
  const float footer_h = ImGui::GetFrameHeightWithSpacing() +
                         ImGui::GetStyle().ItemSpacing.y * 2.0f;
  const ImVec2 body_size(0.0f, ImGui::GetContentRegionAvail().y - footer_h);

  if (ImGui::BeginChild("gs_body", body_size, false)) {
    if (tab_ == kTabBasic) {
      DrawBasicTab();
    } else {
      DrawListTab(tab_);
    }
  }
  // 🔴 Le verrou anti-défilement : sans lui, la molette au-dessus d'un slider
  // fait aussi défiler la fenêtre derrière (feedback_imgui_wheel_scroll_gate).
  mui::RegionWheel("gs_body", ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows));
  ImGui::EndChild();

  // ── Pied ───────────────────────────────────────────────────────────────────
  ImGui::Separator();

  const char* label_graphics = i18n::Tr("Réglages natifs...");
  const char* label_reset    = i18n::Tr("Tout réinitialiser");
  const char* label_close    = i18n::Tr("Fermer");
  const float btn_w = ro::MaxButtonWidth({label_graphics, label_reset, label_close});

  if (ro::RoButton(label_graphics, btn_w)) pending_open_native_ = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s",
                      i18n::Tr("Ouvre la fenêtre du client pour ce qui n'est pas "
                               "repris ici : graphismes, skin, priorité, courrier."));
  }
  ImGui::SameLine();
  if (ro::RoButton(label_reset, btn_w)) confirm_reset_ = true;

  // Fermer, calé à droite et recalculé à chaque frame : il suit le redimensionnement.
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btn_w);
  if (ro::RoButton(label_close, btn_w)) Close();

  // ── Confirmation du reset ──────────────────────────────────────────────────
  // Popup ImGui, PAS la modale native : celle-ci bloque le fil principal
  // (feedback_no_blocking_dialog_main_thread). Le texte est celui du CLIENT.
  if (confirm_reset_) {
    ImGui::OpenPopup("###gs_confirm_reset");
    confirm_reset_ = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  // ⚠ Le suffixe ### est collé APRÈS la traduction, jamais dedans : il identifie
  // la popup pour ImGui, et l'enfermer dans la clé du catalogue ferait dépendre
  // cet identifiant de la langue — la popup ouverte ne serait plus la même que
  // celle qu'on dessine.
  char popup_id[128];
  std::snprintf(popup_id, sizeof(popup_id), "%s###gs_confirm_reset",
                i18n::Tr("Réinitialiser"));
  if (ro::BeginRoPopupModal(popup_id)) {
    // Pendant une modale, la pile Échap est neutralisée : sinon un Échap fermerait
    // la confirmation ET le panneau derrière (docs §5.6 point 10).
    ro::SuppressEscapeStack();

    ImGui::TextUnformatted(msgstr::Utf8Or(
        kMsgResetConfirm, i18n::Tr("Rétablir les réglages par défaut ?")));
    ImGui::Spacing();
    const float w = ro::MaxButtonWidth({i18n::Tr("Réinitialiser"), i18n::Tr("Annuler")});
    if (ro::RoButton(i18n::Tr("Réinitialiser"), w)) {
      pending_reset_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), w)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  ro::EndRoWindow();
}

// ─────────────────────────────────────────────────────────────────────────────

void GameSettings::DrawBasicTab() {
  mui::PushStyleCompact();

  // ── Audio ──────────────────────────────────────────────────────────────────
  // Les volumes sont lus À CHAQUE FRAME dans le gestionnaire de son : ils peuvent
  // changer sous nos pieds (chargement d'OptionInfo.lua, commande /bgm), et un
  // curseur qui garderait sa valeur mentirait.
  mui::SeparatorText(i18n::Tr("Son"));

  bool bgm_on = gamesettings::BgmEnabled();
  if (ro::RoCheckbox(i18n::Tr("Musique de fond"), &bgm_on))
    gamesettings::SetBgmEnabled(bgm_on);

  int bgm = gamesettings::BgmVolume();
  ImGui::BeginDisabled(!bgm_on);
  if (mui::WheelSliderInt(i18n::Tr("Volume musique"), &bgm, 0,
                          gamesettings::kVolumeMax))
    gamesettings::SetBgmVolume(bgm);
  ImGui::EndDisabled();

  bool sfx_on = gamesettings::EffectSoundEnabled();
  if (ro::RoCheckbox(i18n::Tr("Effets sonores"), &sfx_on))
    gamesettings::SetEffectSoundEnabled(sfx_on);

  int sfx = gamesettings::EffectVolume();
  ImGui::BeginDisabled(!sfx_on);
  if (mui::WheelSliderInt(i18n::Tr("Volume effets"), &sfx, 0,
                          gamesettings::kVolumeMax))
    gamesettings::SetEffectVolume(sfx);
  ImGui::EndDisabled();

  // ── Les deux bascules de la page Basique qui sont de simples TALKTYPE ───────
  mui::SeparatorText(i18n::Tr("Affichage"));

  bool emblem = gamesettings::IsOn(kTtEmblemFrame);
  if (ro::RoCheckbox(
          msgstr::Utf8Or(kMsgEmblemFrame, i18n::Tr("Bordure d'emblème")), &emblem)) {
    pending_write_.valid = true;
    pending_write_.id = kTtEmblemFrame;
    pending_write_.on = emblem;
    // 🔴 PAR LA COMMANDE, pas par l'id. `/frame` est absent de `CmdOnOffList`
    // dans `SaveData\OptionInfo.lua`, donc la clé de cette option n'existe pas
    // dans la table des drapeaux — et `SetFlagRaw` refuse de créer ce qu'il ne
    // trouve pas. Le réglage est pour cette raison inerte JUSQUE DANS LA FENÊTRE
    // NATIVE (constaté en jeu). Le chemin par nom de commande, lui, insère.
    pending_write_.slash = "/frame";
  }
  ImGui::SameLine();
  mui::HelpMarker(i18n::Tr("Encadre l'emblème de guilde affiché à côté des noms."));

  bool login = gamesettings::IsOn(kTtLoginNotify);
  if (ro::RoCheckbox(
          msgstr::Utf8Or(kMsgLoginNotify, i18n::Tr("Notification de connexion")),
          &login)) {
    pending_write_.valid = true;
    pending_write_.id = kTtLoginNotify;
    pending_write_.on = login;
  }
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Annonce au chat les connexions et déconnexions de vos amis."));

  mui::PopStyleCompact();

  // Ce que cet onglet ne reprend pas — dit franchement plutôt que laissé deviner.
  ImGui::Spacing();
  ImGui::TextColored(kSecondaryText, "%s",
                     i18n::Tr("Skin, priorité, courrier et graphismes restent dans "
                              "la fenêtre du client (bouton ci-dessous)."));
}

void GameSettings::DrawListTab(int tab) {
  const bool all_mode = (tab == kTabAll);
  const int columns = all_mode ? 3 : 2;
  const char* table_id = all_mode ? "gs_table_all" : "gs_table";

  int shown = 0;
  if (ImGui::BeginTable(table_id, columns,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_Resizable)) {
    // Largeurs MESURÉES sur le contenu réel, jamais figées : la police et la
    // langue sont des réglages (feedback_ui_width_measured_not_hardcoded).
    const float tab_col_w =
        ImGui::CalcTextSize(TabLabel(gamesettings::kTabControl)).x +
        ImGui::GetStyle().CellPadding.x * 4.0f;
    const float cmd_col_w = ImGui::CalcTextSize("/notalkmsg2").x +
                            ImGui::GetStyle().CellPadding.x * 4.0f;

    if (all_mode)
      ImGui::TableSetupColumn(i18n::Tr("Onglet"), ImGuiTableColumnFlags_WidthFixed,
                              tab_col_w);
    ImGui::TableSetupColumn(i18n::Tr("Réglage"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(i18n::Tr("Commande"), ImGuiTableColumnFlags_WidthFixed,
                            cmd_col_w);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < rows_.size(); ++i) {
      const Row& row = rows_[i];
      if (!all_mode && row.option.tab != tab) continue;
      if (filter_[0] && !Contains(row.option.title, filter_) &&
          !Contains(row.option.tooltip, filter_) &&
          !Contains(row.option.description, filter_))
        continue;

      ++shown;
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      int column = 0;

      if (all_mode) {
        ImGui::TableSetColumnIndex(column++);
        ImGui::TextColored(kSecondaryText, "%s", TabLabel(row.option.tab));
      }

      ImGui::TableSetColumnIndex(column++);
      if (row.option.type == gamesettings::kTypeCommand) {
        // Ligne EXE : le natif dessine un bouton « appliquer » à la place de la
        // case. Le libellé de ces lignes EST la commande (« /sit », « /where »).
        if (ro::RoSmallButton(i18n::Tr("Exécuter"))) {
          pending_write_.valid = true;
          pending_write_.exec = true;
          pending_write_.id = row.option.id;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(row.option.title);
      } else {
        bool value = row.value;
        if (ro::RoCheckbox(row.option.title, &value)) {
          pending_write_.valid = true;
          pending_write_.id = row.option.id;
          pending_write_.on = value;
        }
        // Marqueur « plus au défaut » : le natif ne le dit nulle part, et c'est
        // pourtant la première question qu'on se pose devant une liste d'options.
        if (row.value != row.option.default_on) {
          ImGui::SameLine();
          ImGui::TextColored(kChangedText, "*");
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", row.option.default_on
                                        ? i18n::Tr("Par défaut : activé")
                                        : i18n::Tr("Par défaut : désactivé"));
          }
        }
      }
      // La description en INFOBULLE, là où le natif la coince dans une colonne
      // tronquée. Elle vient du client, donc de sa langue.
      if (ImGui::IsItemHovered() && row.option.description[0]) {
        hovered_id_ = row.option.id;
        ImGui::SetTooltip("%s", row.option.description);
      }

      ImGui::TableSetColumnIndex(column++);
      if (row.option.tooltip[0]) {
        // ⚠ Le champ `Tooltip` du Lua n'est PAS une infobulle : il contient la
        // COMMANDE SLASH équivalente (parfois deux, séparées par un saut de
        // ligne). D'où sa colonne à lui — c'est une information que le natif
        // n'affiche jamais alors qu'elle est dans ses données.
        ImGui::TextColored(kSecondaryText, "%s",
                           msgstr::Flatten(row.option.tooltip));
      }

      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  if (shown == 0) {
    ImGui::TextColored(kSecondaryText, "%s",
                       filter_[0] ? i18n::Tr("Aucun réglage ne correspond.")
                                  : i18n::Tr("Aucun réglage dans cet onglet."));
  }
}
