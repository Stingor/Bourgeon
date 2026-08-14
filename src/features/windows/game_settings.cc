#include "features/windows/game_settings.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>  // std::size

#include "bourgeon.h"
#include "imgui.h"
#include "ragnarok/game_settings.h"
#include "ragnarok/msgstring.h"
#include "ragnarok/uiwnd.h"
#include "ui/game_texture.h"  // ro::InvalidateGameTextures (changement de skin)
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
// 🔴 TOUS PASSENT PAR `Utf8Or`, JAMAIS PAR `Utf8`, ET LES IDS VIENNENT DE
// `tools/lang/msgstring_ids.csv` — PAS DES LIGNES DE `msgstringtable.csv`.
//
// Le client ne numérote pas ses messages par leur rang dans le csv : il porte sa
// propre table de clés, `g_MsgStringSymbolNames` (0x0104F0A8, 4355 entrées), et
// c'est L'INDEX DANS CETTE TABLE qui est l'id. Le csv livré, lui, a 4360 lignes
// et diverge dès la 4075e — un bloc y est permuté, et 40 clés lui sont propres.
// Les deux coïncident dans les petits rangs, ce qui rend l'erreur invisible
// jusqu'à ce qu'elle ne le soit plus : trois ids relevés à la ligne étaient faux
// (l'onglet « Other » et « Login Notification » sortaient « NO MSG », et le titre
// affichait « Indoor teleport is not supported. »).
//
// ⚠ Les cinq ids de la page Basique ne sont PAS contigus : 4145 est déjà
// l'onglet Graphismes, et 4147..4159 les libellés des groupes câblés en dur. Une
// boucle `base + i` afficherait n'importe quoi.
//
// `Utf8Or` reste la règle malgré la table : le client de PRODUCTION est
// WARP-patché et son csv peut différer de l'exe qu'on désassemble. Le repli
// affiche notre libellé traduit plutôt que le texte d'un autre message. (La
// traduction globale, elle, ne dépend d'aucun id : elle s'indexe sur le TEXTE
// anglais — cf. ragnarok/msgstring_override.cc.)
constexpr int kMsgTabBasic    = 4142;  // MSI_GAME_SETTINGS_TAB_BASIC
constexpr int kMsgTabEffect   = 4143;  // MSI_GAME_SETTINGS_TAB_EFFECT
constexpr int kMsgTabControl  = 4144;  // MSI_GAME_SETTINGS_TAB_CONTROL
constexpr int kMsgTabEtc      = 4222;  // MSI_GAME_SETTINGS_TAB_ETC
constexpr int kMsgEmblemFrame = 4146;  // MSI_GAME_SETTINGS_EMBLEM_FRAME
constexpr int kMsgLoginNotify = 4221;  // MSI_GAME_SETTINGS_LOGINOUT
constexpr int kMsgWindowTitle = 4232;  // MSI_OPTION_ESC — « Options (ESC) »
// « Set Basic Settings? » — la confirmation que le natif pose sur son [Reset].
constexpr int kMsgResetConfirm = 3166;

// ── Les libellés des trois derniers groupes de la page Basique ──────────────
constexpr int kMsgSkin           = 1497;  // MSI_SKIN
constexpr int kMsgSkinDefault    = 3090;  // MSI_BASIC_SKIN — « <Basic Skin> »
constexpr int kMsgPriority       = 4153;  // ..._PROCESS_PRIORITY
constexpr int kMsgPriorityHigh   = 4154;
constexpr int kMsgPriorityHighTip   = 4155;
constexpr int kMsgPriorityNormal    = 4156;
constexpr int kMsgPriorityNormalTip = 4157;
constexpr int kMsgPriorityLow       = 4158;  // ..._IDLE, que le client dit « Low »
constexpr int kMsgPriorityLowTip    = 4159;

// ── Onglet Graphismes ──────────────────────────────────────────────────────
constexpr int kMsgTabGraphics    = 4145;  // MSI_GAME_SETTINGS_TAB_GRAPHICS
constexpr int kMsgSpriteDetail   = 324;   // MSI_SPRITE_RESOLUTION
constexpr int kMsgTextureDetail  = 325;   // MSI_TEXTURE_RESOLUTION
constexpr int kMsgTrilinear      = 4186;  // MSI_GAME_SETTINGS_TRILINEAR
constexpr int kMsgRenderSystem   = 4097;  // MSI_GRAPHIC_SETTING_RENDER_SYSTEM
constexpr int kMsgGraphicDevice  = 3181;  // MSI_GRAPHIC_SETTING_DEVICE_SET
constexpr int kMsgResolution     = 3182;  // MSI_GRAPHIC_SETTING_RESOLUTION_SET
constexpr int kMsgScreenMode     = 4183;  // MSI_GAME_SETTINGS_SCREEN_MODE
constexpr int kMsgFullscreenMode = 4184;  // MSI_GAME_SETTINGS_FULLSCREEN_MODE
constexpr int kMsgWindowMode     = 4185;  // MSI_GAME_SETTINGS_WINDOW_MODE
// « To apply these values, client restart is required, proceed? » — le texte
// EXACT que le natif pose sur sa modale quand un réglage structurel a changé.
constexpr int kMsgRestartConfirm = 3168;  // MSI_GRAPHIC_SETTING_WARNING_RESTART

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

  // Le brouillon graphique repart TOUJOURS de la configuration réelle du client.
  // Le garder d'une ouverture à l'autre proposerait une relance pour un
  // changement que le joueur a déjà annulé en fermant la fenêtre.
  namespace gfx = gamesettings::graphics;
  draft_.system     = gfx::System();
  draft_.fullscreen = gfx::Fullscreen();
  draft_.adapter    = gfx::CurrentAdapterIndex();
  if (draft_.adapter < 0) draft_.adapter = 0;
  draft_.mode = -1;
  // Les listes elles-mêmes ne sont PAS relues ici : elles coûtent un device
  // Direct3D, et l'onglet Graphismes les demandera s'il est ouvert.
  graphics_ready_ = false;
  adapters_.clear();
  modes_.clear();
}

void GameSettings::Close() {
  open_ = false;
  confirm_reset_ = false;
  confirm_restart_ = false;
}

void GameSettings::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  if (mode_type != ModeMgr::ModeType::kGame) {
    Close();
    rows_.clear();
    rows_dirty_ = true;
    // Les listes d'adaptateurs valent pour une session de rendu : hors du jeu,
    // les redemander est le seul moyen de ne pas afficher un état périmé.
    graphics_ready_ = false;
  }
}

void GameSettings::OnTick() {
  if (!imgui_enabled_) {
    if (open_) Close();
    return;
  }
  if (!Bourgeon::Instance().IsGameActive()) {
    if (open_) Close();
    return;
  }

  if (pending_reset_) {
    pending_reset_ = false;
    gamesettings::ResetAllToDefault();
    rows_dirty_ = true;
    return;
  }

  // 🔴 LA RELANCE EN PREMIER : elle déconnecte et arrête le mode courant, donc
  // rien de ce qui suit n'aurait de sens après elle.
  if (pending_restart_) {
    pending_restart_ = false;
    const bool has_mode =
        draft_.mode >= 0 && draft_.mode < static_cast<int>(modes_.size());
    if (has_mode) {
      Close();  // ne pas laisser un panneau ouvert par-dessus un client qui s'arrête
      gamesettings::graphics::ApplyAndRestart(
          draft_.system, draft_.adapter, modes_[draft_.mode].width,
          modes_[draft_.mode].height, modes_[draft_.mode].bpp, draft_.fullscreen);
    }
    return;
  }

  // Énumération auprès du client : coûteuse (elle crée un device Direct3D en
  // DX9), donc au tick et sur évènement seulement.
  if (pending_graphics_refresh_) {
    pending_graphics_refresh_ = false;
    RefreshGraphicsLists();
    return;
  }

  if (pending_skin_ != kNoPendingSkin) {
    const int skin = pending_skin_;
    pending_skin_ = kNoPendingSkin;
    // 🔴 L'ORDRE COMPTE. On jette NOS textures d'abord : après `SetSkin`, les
    // chemins mémorisés désignent d'autres images, et un cache gardé afficherait
    // l'ancien skin jusqu'au prochain reset de device.
    ro::InvalidateGameTextures();   // les .bmp mémorisés par chemin
    ro::InvalidateSkinTextures();   // les pièces d'habillage de nos propres fenêtres
    gamesettings::SetSkin(skin);
    return;
  }

  if (!pending_writes_.empty()) {
    // Toutes les écritures en attente, pas une par tick : ce sont des clics du
    // joueur, donc quelques-unes au plus, et les étaler ferait durer le
    // clignotement que l'affichage optimiste vient justement de supprimer.
    const std::vector<PendingWrite> writes = pending_writes_;
    pending_writes_.clear();
    for (const PendingWrite& write : writes) {
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
    }
    rows_dirty_ = true;
    return;
  }

  // 🔴 DÉTRUIRE, pas masquer : le hook de création l'a rendue invisible, mais une
  // native vivante avale un appui sur deux et garde le clavier.
  if (uiwnd::FindWindow(kGameSettingsWndId)) uiwnd::CloseWindow(kGameSettingsWndId);
}

bool GameSettings::PendingValue(int id, bool actual) const {
  // À rebours : la DERNIÈRE demande pour cet id est celle du dernier clic.
  for (auto it = pending_writes_.rbegin(); it != pending_writes_.rend(); ++it) {
    if (!it->exec && it->id == id) return it->on;
  }
  return actual;
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
  const int tab_order[] = {kTabAll,   kTabBasic,
                           kTabGraphics, gamesettings::kTabEffect,
                           gamesettings::kTabControl, gamesettings::kTabEtc};
  if (ro::RoBeginTabBar("gs_tabs")) {
    for (int tab : tab_order) {
      const char* label =
          (tab == kTabAll)        ? i18n::Tr("Tout")
          : (tab == kTabBasic)    ? msgstr::Utf8Or(kMsgTabBasic, i18n::Tr("Basique"))
          : (tab == kTabGraphics) ? msgstr::Utf8Or(kMsgTabGraphics,
                                                   i18n::Tr("Graphismes"))
                                  : TabLabel(tab);
      // Identifiant TECHNIQUE et stable : le libellé vient du client et change
      // avec sa langue, ce qui recréerait l'onglet à chaque bascule.
      char tab_id[96];
      std::snprintf(tab_id, sizeof(tab_id), "%s###gs_tab_%d", label, tab);
      if (ImGui::BeginTabItem(tab_id)) {
        // Les listes d'adaptateurs et de modes sont demandées À L'ENTRÉE dans
        // l'onglet, jamais à l'ouverture du panneau : l'énumération DX9 crée un
        // device Direct3D, et le joueur qui ne va pas dans Graphismes n'a pas à
        // le payer.
        if (tab == kTabGraphics && tab_ != kTabGraphics && !graphics_ready_)
          pending_graphics_refresh_ = true;
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
    } else if (tab_ == kTabGraphics) {
      DrawGraphicsTab();
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

  // ⛔ PLUS DE BOUTON « Réglages natifs ». La fenêtre 0x271E n'est plus jamais
  // ouverte par nous : l'onglet Graphismes reprend ses six groupes, et le reste
  // du panneau couvre tout ce qu'elle savait faire.
  const char* label_reset = i18n::Tr("Tout réinitialiser");
  const char* label_close = i18n::Tr("Fermer");
  const float btn_w = ro::MaxButtonWidth({label_reset, label_close});

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

  // ── Confirmation de la RELANCE du client ───────────────────────────────────
  // 🔴 Le seul geste de tout le panneau qui ferme la session du joueur. Le texte
  // est celui du CLIENT — c'est exactement l'avertissement que sa propre fenêtre
  // affiche — et le bouton dit ce qu'il fait, pas « OK ».
  if (confirm_restart_) {
    ImGui::OpenPopup("###gs_confirm_restart");
    confirm_restart_ = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  char restart_id[128];
  std::snprintf(restart_id, sizeof(restart_id), "%s###gs_confirm_restart",
                i18n::Tr("Redémarrer le client"));
  if (ro::BeginRoPopupModal(restart_id)) {
    ro::SuppressEscapeStack();

    ImGui::TextWrapped("%s",
                       msgstr::Utf8Or(kMsgRestartConfirm,
                                      i18n::Tr("Ces réglages demandent un "
                                               "redémarrage du client. Continuer ?")));
    ImGui::Spacing();
    ImGui::TextColored(kChangedText, "%s",
                       i18n::Tr("Votre personnage sera déconnecté."));
    ImGui::Spacing();
    const float w = ro::MaxButtonWidth({i18n::Tr("Redémarrer"), i18n::Tr("Annuler")});
    if (ro::RoButton(i18n::Tr("Redémarrer"), w)) {
      pending_restart_ = true;
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

  bool emblem = PendingValue(kTtEmblemFrame, gamesettings::IsOn(kTtEmblemFrame));
  if (ro::RoCheckbox(
          msgstr::Utf8Or(kMsgEmblemFrame, i18n::Tr("Bordure d'emblème")), &emblem)) {
    PendingWrite write;
    write.valid = true;
    write.id = kTtEmblemFrame;
    write.on = emblem;
    // 🔴 PAR LA COMMANDE, pas par l'id. `/frame` est absent de `CmdOnOffList`
    // dans `SaveData\OptionInfo.lua`, donc la clé de cette option n'existe pas
    // dans la table des drapeaux — et `SetFlagRaw` refuse de créer ce qu'il ne
    // trouve pas. Le réglage est pour cette raison inerte JUSQUE DANS LA FENÊTRE
    // NATIVE (constaté en jeu). Le chemin par nom de commande, lui, insère.
    write.slash = "/frame";
    pending_writes_.push_back(write);
  }
  ImGui::SameLine();
  mui::HelpMarker(i18n::Tr("Encadre l'emblème de guilde affiché à côté des noms."));

  bool login = PendingValue(kTtLoginNotify, gamesettings::IsOn(kTtLoginNotify));
  if (ro::RoCheckbox(
          msgstr::Utf8Or(kMsgLoginNotify, i18n::Tr("Notification de connexion")),
          &login)) {
    PendingWrite write;
    write.valid = true;
    write.id = kTtLoginNotify;
    write.on = login;
    pending_writes_.push_back(write);
  }
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Annonce au chat les connexions et déconnexions de vos amis."));

  // ⛔ PAS DE GROUPE « COURRIER » ICI, ET C'EST UN CHOIX. Le natif en a un — deux
  // boutons radio « recevoir de tout le monde / bloquer les inconnus » — mais il
  // est MORT sur Moonlight : le client demande le changement par `CZ 0x0B93`, et
  // le serveur ne mappe aucun `0x0b9x` (`clif_parse_configuration` n'écoute que
  // `0x02d8`). Le paquet part dans le vide, chez nous comme dans la fenêtre du
  // client. Une case qui ne bouge jamais est pire qu'une case absente.
  //
  // Le RE complet est au §3.9 de docs/game_option_re.md, y compris ce qu'il
  // faudrait ajouter côté serveur pour le faire vivre : un type
  // `CONFIG_RODEX_SPAM` (celui que le client envoie, 1, vaut `CONFIG_CALL` chez
  // rAthena — le brancher tel quel basculerait le refus des APPELS), le mappage
  // du paquet, un champ persisté, et l'émission de `0x0B95` à l'entrée en jeu.

  // ── Priorité du processus ──────────────────────────────────────────────────
  // Réglage purement local (SetPriorityClass) : il s'applique immédiatement, sans
  // passer par la file — il ne touche ni au réseau, ni à une fenêtre native.
  //
  // ⚠ Il ne vaut que FENÊTRE AU PREMIER PLAN : le client se rabat de force sur
  // « Low » dès qu'il perd le focus (docs §3.9). Le dire, sinon le joueur croira
  // que le réglage n'a pas pris.
  // ⚠ PAS de `SameLine` après un `SeparatorText` : son trait occupe toute la
  // largeur restante, et le marqueur atterrit au bout de la ligne, détaché de son
  // titre. Le (?) va donc après le DERNIER bouton radio, comme les cases
  // au-dessus. Constaté en jeu.
  mui::SeparatorText(msgstr::Utf8Or(kMsgPriority, i18n::Tr("Priorité du jeu")));

  struct PriorityChoice {
    int value;
    int label_id;
    int tip_id;
    const char* fallback;  // déjà traduit : `Tr` veut un LITTÉRAL au point d'appel
  };
  // ⚠ Table LOCALE, refaite à chaque frame, et non `static` : les pointeurs de
  // `Tr` appartiennent au catalogue et meurent au changement de langue.
  const PriorityChoice choices[] = {
      {gamesettings::kPriorityHigh, kMsgPriorityHigh, kMsgPriorityHighTip,
       i18n::Tr("Haute")},
      {gamesettings::kPriorityNormal, kMsgPriorityNormal, kMsgPriorityNormalTip,
       i18n::Tr("Normale")},
      {gamesettings::kPriorityLow, kMsgPriorityLow, kMsgPriorityLowTip,
       i18n::Tr("Basse")},
  };

  const int priority = gamesettings::ProcessPriority();
  for (int i = 0; i < static_cast<int>(std::size(choices)); ++i) {
    const PriorityChoice& choice = choices[i];
    if (i > 0) ImGui::SameLine();
    if (ro::RadioImage(msgstr::Utf8Or(choice.label_id, choice.fallback),
                       priority == choice.value)) {
      gamesettings::SetProcessPriority(choice.value);
    }
    // La description du client, telle quelle — elle explique le compromis mieux
    // qu'une paraphrase, et elle suit sa langue. Vide, l'infobulle est tue.
    if (ImGui::IsItemHovered()) {
      const char* tip = msgstr::Utf8Or(choice.tip_id, "");
      if (tip && *tip) ImGui::SetTooltip("%s", tip);
    }
  }
  // Ce que le natif ne dit nulle part, et qui décide si le réglage sert à
  // quelque chose : c'est du temps CPU, pas du GPU, et seulement au premier plan.
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Règle la part de temps PROCESSEUR que Windows accorde au jeu "
               "face aux autres programmes.\n\n"
               "Haute : le jeu passe avant le reste. Utile seulement si la "
               "machine est déjà chargée ; sur un PC au repos, aucun gain.\n"
               "Normale : à égalité avec les autres programmes. C'est le défaut.\n"
               "Basse : le jeu ne tourne que quand rien d'autre ne réclame le "
               "processeur — à réserver aux calculs de fond, il devient vite "
               "saccadé.\n\n"
               "Sans effet sur la carte graphique : des ralentissements dus au "
               "GPU ne bougeront pas d'un cran.\n"
               "Le réglage ne vaut que fenêtre active — en arrière-plan, le "
               "client se met de lui-même en priorité basse.\n"
               "Conservé d'une session à l'autre."));

  // ── Skin de l'interface ────────────────────────────────────────────────────
  mui::SeparatorText(msgstr::Utf8Or(kMsgSkin, i18n::Tr("Skin")));

  const int skin_count = gamesettings::SkinCount();
  const int current_skin = (pending_skin_ != kNoPendingSkin)
                               ? pending_skin_
                               : gamesettings::CurrentSkin();
  char current_name[128];
  if (!gamesettings::SkinName(current_skin, current_name, sizeof(current_name))) {
    std::strncpy(current_name, msgstr::Utf8Or(kMsgSkinDefault, i18n::Tr("Par défaut")),
                 sizeof(current_name) - 1);
    current_name[sizeof(current_name) - 1] = '\0';
  }

  ImGui::SetNextItemWidth(ro::Px(220.0f));
  if (ImGui::BeginCombo("###gs_skin", current_name)) {
    char name[128];
    if (!gamesettings::SkinName(gamesettings::kSkinDefault, name, sizeof(name))) {
      std::strncpy(name, msgstr::Utf8Or(kMsgSkinDefault, i18n::Tr("Par défaut")),
                   sizeof(name) - 1);
      name[sizeof(name) - 1] = '\0';
    }
    if (ImGui::Selectable(name, current_skin == gamesettings::kSkinDefault))
      pending_skin_ = gamesettings::kSkinDefault;

    for (int i = 0; i < skin_count; ++i) {
      if (!gamesettings::SkinName(i, name, sizeof(name))) continue;
      ImGui::PushID(i);
      if (ImGui::Selectable(name, current_skin == i)) pending_skin_ = i;
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Remplace les images de l'interface du client.\n"
               "Le changement recharge toutes les textures : un temps d'arrêt "
               "d'une fraction de seconde est normal."));

  if (skin_count == 0) {
    ImGui::TextColored(kSecondaryText, "%s",
                       i18n::Tr("Aucun skin installé — seule l'apparence "
                                "d'origine est disponible."));
  }

  mui::PopStyleCompact();

  // Ce que cet onglet ne reprend toujours pas — dit franchement plutôt que laissé
  // deviner. Les graphismes restent au client : ce sont des resets de device.
  ImGui::Spacing();
  ImGui::TextColored(kSecondaryText, "%s",
                     i18n::Tr("Les réglages graphiques (résolution, mode d'écran, "
                              "filtrage) restent dans la fenêtre du client."));
}

void GameSettings::RefreshGraphicsLists() {
  namespace gfx = gamesettings::graphics;

  if (draft_.system != gfx::kRenderDx7 && draft_.system != gfx::kRenderDx9)
    draft_.system = gfx::System();

  adapters_.resize(kMaxAdapters);
  int count = 0;
  if (!gfx::EnumerateAdapters(draft_.system, adapters_.data(), kMaxAdapters, &count))
    count = 0;
  adapters_.resize(static_cast<size_t>(count));

  // L'adaptateur du brouillon doit rester dans la liste : changer d'API la
  // renouvelle entièrement, et garder un index qui n'y est plus ferait écrire
  // une configuration que le client refusera au démarrage.
  bool adapter_found = false;
  for (const gfx::Adapter& adapter : adapters_) {
    if (adapter.index == draft_.adapter) { adapter_found = true; break; }
  }
  if (!adapter_found) draft_.adapter = adapters_.empty() ? 0 : adapters_[0].index;

  modes_.resize(kMaxModes);
  count = 0;
  if (!gfx::EnumerateModes(draft_.system, draft_.adapter, modes_.data(), kMaxModes,
                           &count))
    count = 0;
  modes_.resize(static_cast<size_t>(count));

  // Retrouver le mode du brouillon dans la nouvelle liste, par ses trois
  // entiers — c'est la seule comparaison que le client fasse lui-même.
  const int want_w = (draft_.mode >= 0 && draft_.mode < static_cast<int>(modes_.size()))
                         ? modes_[draft_.mode].width : gfx::Width();
  const int want_h = (draft_.mode >= 0 && draft_.mode < static_cast<int>(modes_.size()))
                         ? modes_[draft_.mode].height : gfx::Height();
  const int want_b = (draft_.mode >= 0 && draft_.mode < static_cast<int>(modes_.size()))
                         ? modes_[draft_.mode].bpp : gfx::BitsPerPixel();
  draft_.mode = -1;
  for (size_t i = 0; i < modes_.size(); ++i) {
    if (modes_[i].width == want_w && modes_[i].height == want_h &&
        modes_[i].bpp == want_b) {
      draft_.mode = static_cast<int>(i);
      break;
    }
  }

  graphics_ready_ = true;
}

void GameSettings::DrawGraphicsTab() {
  namespace gfx = gamesettings::graphics;
  mui::PushStyleCompact();

  // ── ⚡ Ce qui s'applique tout de suite ─────────────────────────────────────
  mui::SeparatorText(i18n::Tr("Effet immédiat"));

  const char* detail_names[] = {i18n::Tr("Basse"), i18n::Tr("Moyenne"),
                                i18n::Tr("Élevée")};

  int sprite = gfx::SpriteDetail();
  ImGui::SetNextItemWidth(ro::Px(150.0f));
  if (ImGui::BeginCombo(msgstr::Utf8Or(kMsgSpriteDetail,
                                       i18n::Tr("Finesse des sprites")),
                        detail_names[sprite])) {
    for (int i = 0; i <= gfx::kDetailMax; ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(detail_names[i], i == sprite)) gfx::SetSpriteDetail(i);
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }

  int texture = gfx::TextureDetail();
  ImGui::SetNextItemWidth(ro::Px(150.0f));
  if (ImGui::BeginCombo(msgstr::Utf8Or(kMsgTextureDetail,
                                       i18n::Tr("Finesse des textures")),
                        detail_names[texture])) {
    for (int i = 0; i <= gfx::kDetailMax; ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(detail_names[i], i == texture)) gfx::SetTextureDetail(i);
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  mui::HelpMarker(i18n::Tr("Divise la résolution des textures pour économiser de "
                           "la mémoire vidéo.\nLes textures déjà chargées sont "
                           "rechargées aussitôt."));

  bool trilinear = gfx::Trilinear();
  if (ro::RoCheckbox(msgstr::Utf8Or(kMsgTrilinear, i18n::Tr("Filtrage trilinéaire")),
                     &trilinear)) {
    gfx::SetTrilinear(trilinear);
  }
  ImGui::SameLine();
  mui::HelpMarker(i18n::Tr("Lisse les textures vues de loin. Coût négligeable sur "
                           "une carte moderne."));

  // ── 🔁 Ce qui exige une relance ───────────────────────────────────────────
  ImGui::Spacing();
  mui::SeparatorText(i18n::Tr("Demande un redémarrage du client"));

  ImGui::TextColored(kSecondaryText, "%s",
                     i18n::Tr("Le client n'applique ces quatre réglages qu'au "
                              "démarrage : les changer ferme la session."));
  ImGui::Spacing();

  // Système de rendu — la liste du client est en dur, deux entrées.
  struct SystemChoice { int value; const char* name; };
  const SystemChoice kSystems[] = {{gfx::kRenderDx7, "DirectX 7"},
                                   {gfx::kRenderDx9, "DirectX 9"}};
  const char* system_name = (draft_.system == gfx::kRenderDx7) ? "DirectX 7" : "DirectX 9";
  ImGui::SetNextItemWidth(ro::Px(150.0f));
  if (ImGui::BeginCombo(msgstr::Utf8Or(kMsgRenderSystem, i18n::Tr("API graphique")),
                        system_name)) {
    for (const SystemChoice& choice : kSystems) {
      ImGui::PushID(choice.value);
      if (ImGui::Selectable(choice.name, draft_.system == choice.value) &&
          draft_.system != choice.value) {
        draft_.system = choice.value;
        // Changer d'API renouvelle adaptateurs ET modes : on redemande au
        // client, au tick — l'énumération DX9 crée un device Direct3D.
        pending_graphics_refresh_ = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }

  // Adaptateur.
  const char* adapter_name = i18n::Tr("(aucun)");
  for (const gfx::Adapter& adapter : adapters_) {
    if (adapter.index == draft_.adapter) { adapter_name = adapter.name; break; }
  }
  ImGui::SetNextItemWidth(ro::Px(260.0f));
  if (ImGui::BeginCombo(msgstr::Utf8Or(kMsgGraphicDevice, i18n::Tr("Carte graphique")),
                        adapter_name)) {
    for (const gfx::Adapter& adapter : adapters_) {
      ImGui::PushID(adapter.index);
      if (ImGui::Selectable(adapter.name, adapter.index == draft_.adapter) &&
          adapter.index != draft_.adapter) {
        draft_.adapter = adapter.index;
        pending_graphics_refresh_ = true;  // les modes dépendent de l'adaptateur
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }

  // Résolution.
  char mode_label[96];
  if (draft_.mode >= 0 && draft_.mode < static_cast<int>(modes_.size())) {
    const gfx::Mode& mode = modes_[draft_.mode];
    std::snprintf(mode_label, sizeof(mode_label), "%d x %d - %d bits", mode.width,
                  mode.height, mode.bpp);
  } else {
    // Le mode courant n'est pas dans la liste : le dire plutôt que d'afficher la
    // première ligne, qui laisserait croire qu'elle est active.
    std::snprintf(mode_label, sizeof(mode_label), "%d x %d - %d bits  (%s)",
                  gfx::Width(), gfx::Height(), gfx::BitsPerPixel(),
                  i18n::Tr("hors liste"));
  }
  ImGui::SetNextItemWidth(ro::Px(260.0f));
  if (ImGui::BeginCombo(msgstr::Utf8Or(kMsgResolution, i18n::Tr("Résolution")),
                        mode_label)) {
    for (size_t i = 0; i < modes_.size(); ++i) {
      const gfx::Mode& mode = modes_[i];
      char label[96];
      std::snprintf(label, sizeof(label), "%d x %d - %d bits", mode.width,
                    mode.height, mode.bpp);
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(label, static_cast<int>(i) == draft_.mode))
        draft_.mode = static_cast<int>(i);
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }

  // Plein écran / fenêtré : deux radios, comme le natif.
  ImGui::TextUnformatted(msgstr::Utf8Or(kMsgScreenMode, i18n::Tr("Mode d'écran")));
  ImGui::SameLine();
  if (ro::RadioImage(msgstr::Utf8Or(kMsgFullscreenMode, i18n::Tr("Plein écran")),
                     draft_.fullscreen))
    draft_.fullscreen = true;
  ImGui::SameLine();
  if (ro::RadioImage(msgstr::Utf8Or(kMsgWindowMode, i18n::Tr("Fenêtré")),
                     !draft_.fullscreen))
    draft_.fullscreen = false;

  // ── Le bouton, actif seulement s'il y a quelque chose à appliquer ─────────
  const bool has_mode = (draft_.mode >= 0 && draft_.mode < static_cast<int>(modes_.size()));
  const bool changed =
      draft_.system != gfx::System() || draft_.fullscreen != gfx::Fullscreen() ||
      draft_.adapter != gfx::CurrentAdapterIndex() ||
      (has_mode && (modes_[draft_.mode].width != gfx::Width() ||
                    modes_[draft_.mode].height != gfx::Height() ||
                    modes_[draft_.mode].bpp != gfx::BitsPerPixel()));

  ImGui::Spacing();
  ImGui::BeginDisabled(!changed || !has_mode);
  if (ro::RoButton(i18n::Tr("Appliquer et redémarrer"),
                   ro::ButtonWidth(i18n::Tr("Appliquer et redémarrer"))))
    confirm_restart_ = true;
  ImGui::EndDisabled();
  if (!has_mode && !modes_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kChangedText, "%s", i18n::Tr("Choisissez une résolution."));
  }

  mui::PopStyleCompact();
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
          PendingWrite write;
          write.valid = true;
          write.exec = true;
          write.id = row.option.id;
          pending_writes_.push_back(write);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(row.option.title);
      } else {
        bool value = PendingValue(row.option.id, row.value);
        if (ro::RoCheckbox(row.option.title, &value)) {
          PendingWrite write;
          write.valid = true;
          write.id = row.option.id;
          write.on = value;
          pending_writes_.push_back(write);
        }
        // Marqueur « plus au défaut » : le natif ne le dit nulle part, et c'est
        // pourtant la première question qu'on se pose devant une liste d'options.
        if (value != row.option.default_on) {
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
