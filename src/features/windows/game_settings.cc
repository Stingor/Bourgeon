#include "features/windows/game_settings.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>  // std::size
#include <string>

#include "bourgeon.h"
#include "features/fx/screen_fx.h"         // les effets d'écran, hébergés ici
#include "features/link_gesture.h"         // Maj + clic sur un onglet = un lien
#include "features/moonlight_ui/moonlight_ui.h"  // SaveSettings (bourgeon_settings.yaml)
#include "features/fx/weapon_dual_sprites.h"
#include "features/systems/dx7_warning.h"  // dx7::DrawWarningBody — texte PARTAGÉ
#include "imgui.h"
#include "ragnarok/game_settings.h"
#include "ragnarok/msgstring.h"
#include "ragnarok/uiwnd.h"
#include "ui/game_texture.h"  // ro::InvalidateGameTextures (changement de skin)
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"
#include "utils/text.h"  // text::ToLowerAscii / ContainsNoCase
#include "ui/ui_palette.h"  // ro::pal : la palette de l'UI

namespace {

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
// Le marqueur « ce réglage n'est plus à son défaut ».
const ImVec4 kChangedText(0.65f, 0.30f, 0.10f, 1.0f);

// ── Les onglets, vus par le système de liens (cf. le .h) ────────────────────
//
// ⚠ Le libellé est le NOM NU de l'onglet, sans « Réglages : » devant. La
// première version le préfixait pour donner du contexte à un lien qui arrive
// seul au milieu d'une conversation — mais l'enveloppe le dit DÉJÀ : le résultat
// se lisait « [Réglage: Réglages : Effets ] ». Le contexte appartient à
// `SettingLabel`, pas au libellé. Signalé en jeu le 2026-08-15.
//
// Corollaire heureux : ces noms-là sont exactement les replis de `TabLabel`, donc
// déjà traduits dans les deux catalogues. Le libellé se passe en FRANÇAIS NU —
// c'est `SettingLabel` qui le traduit, chez le lecteur, dans SA langue.
struct TabLink {
  int tab;
  const char* key;
  const char* label;  // non traduit
};

constexpr TabLink kTabLinks[] = {
    {GameSettings::kTabAll,           "gs_all",      "Tous les onglets"},
    {GameSettings::kTabBasic,         "gs_basic",    "Basique"},
    // 🔴 « graphics » et pas « gs_graphics » : c'est la clé qu'avait la section
    // Graphismes de Moonlight Settings, d'où elle a déménagé. Les liens déjà
    // posés dans le chat ouvrent donc toujours la bonne chose.
    {GameSettings::kTabGraphics,      "graphics",    "Graphismes"},
    {gamesettings::kTabEffect,        "gs_effect",   "Effets"},
    {gamesettings::kTabControl,       "gs_control",  "Contrôles"},
    {gamesettings::kTabEtc,           "gs_etc",      "Divers"},
};

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

// Ce que Windows sait dire d'une sortie d'affichage, une fois interrogé.
//
// 🔴 `number` N'EST PAS le chiffre du nom GDI. `\\.\DISPLAYn` porte un
// identifiant que Windows ALLOUE ET NE RECYCLE JAMAIS : chaque combinaison
// écran/sortie jamais vue (un branchement, un câble passé de DP à HDMI, un
// pilote mis à jour, une sortie inutilisée de la carte…) en consomme un de plus.
// Sur une machine un peu vécue on tombe sur `DISPLAY11` et `DISPLAY12` là où
// Windows, lui, affiche « 1 » et « 2 » — signalé en jeu le 2026-08-22, et c'est
// exactement ce que montrait l'ancien libellé, qui lisait ce chiffre au mot.
//
// Le numéro des Paramètres est un RANG : celui de la sortie parmi les écrans
// attachés au bureau, dans l'ordre où `EnumDisplayDevices` les rend. C'est ce
// rang qu'on recompte ici.
struct ScreenInfo {
  int  number  = 0;      // rang parmi les écrans du bureau ; 0 = introuvable
  int  width   = 0;
  int  height  = 0;
  bool primary = false;
};

ScreenInfo QueryScreenInfo(const char* device) {
  ScreenInfo info;
  if (!device || device[0] == '\0') return info;

  int rank = 0;
  for (DWORD i = 0; ; ++i) {
    DISPLAY_DEVICEA dd = {};
    dd.cb = sizeof(dd);
    if (!EnumDisplayDevicesA(nullptr, i, &dd, 0)) break;
    // Un pilote miroir (capture, bureau à distance) est « attaché » sans être un
    // écran : le compter décalerait tous les rangs qui suivent.
    if (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;
    if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
    ++rank;
    if (std::strcmp(dd.DeviceName, device) != 0) continue;

    info.number  = rank;
    info.primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(device, ENUM_CURRENT_SETTINGS, &dm)) {
      info.width  = static_cast<int>(dm.dmPelsWidth);
      info.height = static_cast<int>(dm.dmPelsHeight);
    }
    return info;
  }
  return info;
}

// Le libellé d'un adaptateur : sa description, et l'écran quand il y en a un.
//
// On donne de quoi RECONNAÎTRE l'écran — un numéro qui est celui des Paramètres
// Windows, sa définition, et le fait qu'il soit ou non l'écran principal —
// plutôt que le nom GDI, exact mais illisible ET trompeur. Ce nom brut reste
// disponible sous la souris (infobulle du combo), pour un diagnostic.
//
// `rank` est le rang de la ligne dans l'énumération du client : c'est le repli
// quand Windows ne reconnaît pas la sortie (elle a pu être débranchée entre
// l'énumération Direct3D et maintenant), et il vaut toujours mieux qu'un numéro
// arbitraire. En DirectX 7 le client ne donne aucun nom de sortie : la ligne se
// réduit alors à la description, sans numéro inventé.
std::string FormatAdapterLabel(const gamesettings::graphics::Adapter& adapter,
                               int rank) {
  char out[224];
  if (adapter.device[0] == '\0') {
    std::snprintf(out, sizeof(out), "%s", adapter.name);
    return out;
  }

  const ScreenInfo info = QueryScreenInfo(adapter.device);
  const int number = (info.number > 0) ? info.number : rank;

  char details[80] = {0};
  const bool has_size = (info.width > 0 && info.height > 0);
  if (has_size && info.primary) {
    std::snprintf(details, sizeof(details), "  (%d x %d, %s)", info.width,
                  info.height, i18n::Tr("principal"));
  } else if (has_size) {
    std::snprintf(details, sizeof(details), "  (%d x %d)", info.width, info.height);
  } else if (info.primary) {
    std::snprintf(details, sizeof(details), "  (%s)", i18n::Tr("principal"));
  }

  std::snprintf(out, sizeof(out), i18n::Tr("%s  —  écran %d"), adapter.name, number);
  std::strncat(out, details, sizeof(out) - std::strlen(out) - 1);
  return out;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

namespace gslink {

int TabByKey(const char* key) {
  if (key == nullptr || key[0] == '\0') return kNoTab;
  for (const TabLink& link : kTabLinks)
    if (std::strcmp(link.key, key) == 0) return link.tab;
  return kNoTab;
}

const char* LabelByKey(const char* key) {
  if (key == nullptr || key[0] == '\0') return nullptr;
  for (const TabLink& link : kTabLinks)
    if (std::strcmp(link.key, key) == 0) return link.label;
  return nullptr;
}

const char* KeyByTab(int tab) {
  for (const TabLink& link : kTabLinks)
    if (link.tab == tab) return link.key;
  return nullptr;
}

}  // namespace gslink

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
  // 🔴 L'adaptateur N'EST PAS relu ici : le connaître demande une énumération, et
  // donc un device Direct3D, pour un panneau qui s'ouvre le plus souvent sur un
  // autre onglet. −1 = « pas encore su » ; `RefreshGraphicsLists` tranchera.
  draft_.adapter    = -1;
  current_adapter_  = -1;
  draft_.mode = -1;
  saved_structural_ = false;
  // Les listes elles-mêmes ne sont PAS relues ici : elles coûtent un device
  // Direct3D, et l'onglet Graphismes les demandera s'il est ouvert.
  graphics_ready_ = false;
  adapters_.clear();
  adapter_labels_.clear();
  modes_.clear();
}

void GameSettings::OpenTab(int tab) {
  // Réouvrir remet le brouillon graphique à plat : c'est ce que fait le menu
  // Échap, et un lien de chat ne doit pas se comporter autrement.
  OpenFromMenu();
  tab_ = tab;
  // 🔴 L'onglet Graphismes a besoin de ses listes, et il ne passera PAS par le
  // `BeginTabItem` qui les demande d'ordinaire : `tab_` est déjà posé, donc le
  // test « on vient d'y entrer » sera faux à la première frame. Sans cette ligne,
  // un lien vers Graphismes ouvre un onglet aux listes vides.
  if (tab == kTabGraphics) pending_graphics_refresh_ = true;
}

void GameSettings::Close() {
  open_ = false;
  confirm_reset_ = false;
  confirm_restart_ = false;
}

void GameSettings::ApplyEmblemFrame() {
  // La table des drapeaux est reconstruite à chaque session, et celle-ci n'y
  // figure dans AUCUNE source du client (ni `OptionTbl`, ni `CmdOnOffList`) :
  // sans cette réinjection, le réglage repart à zéro à chaque lancement, ce que
  // le joueur voit comme « ça ne se retient pas ».
  gamesettings::SetOn(kTtEmblemFrame, emblem_frame_);
  emblem_frame_applied_ = true;
}

void GameSettings::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  // Réarmé à CHAQUE bascule, y compris vers le jeu : le manager n'est pas encore
  // prêt ici (`CSession_ctor` remplit sa table juste après), donc on ne peut
  // qu'armer — c'est `OnTick` qui écrira, une fois le manager debout.
  emblem_frame_applied_ = false;
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

  // 🔴 Un warp / @load referme aussi cet écran. Le client, lui, démonte tout son
  // HUD à l'entrée dans la carte suivante — sa fenêtre de réglages y passe avec le
  // reste ; rester ouvert par-dessus la nouvelle carte est une survivance de
  // notre côté, pas un comportement du jeu.
  //
  // Sur l'ÉPOQUE et non sur `IsMapLoading()` : ce dernier est un état, et un
  // chargement plus court que le battement d'OnTick (100 ms) tiendrait entre deux
  // regards — l'écran resterait alors ouvert, ce qui est précisément le bug.
  const uint32_t map_epoch = Bourgeon::Instance().MapLoadEpoch();
  if (map_epoch != map_epoch_) {
    map_epoch_ = map_epoch;
    if (open_) Close();
  }
  // Et tant que la carte charge, on ne touche à rien : pendant
  // `CGameMode::EnterWorld` le HUD natif est détruit puis reconstruit, et c'est
  // la fenêtre de tir où agir dessus a déjà coûté un use-after-free (cf.
  // Bourgeon::IsMapLoading). Ce qui est en attente le reste et partira au premier
  // tick d'après le chargement.
  if (Bourgeon::Instance().IsMapLoading()) return;

  // Le réglage que le client ne sait pas retenir, réinjecté une fois par entrée
  // en jeu. `gamesettings::Count()` ne rend rien tant que le manager n'est pas
  // rempli : on attend qu'il le soit plutôt que d'écrire dans le vide.
  if (!emblem_frame_applied_ && gamesettings::Count() > 0) ApplyEmblemFrame();

  if (pending_reset_) {
    pending_reset_ = false;
    gamesettings::ResetAllToDefault();
    rows_dirty_ = true;
    return;
  }

  // Les réglages structurels : différés jusqu'ici parce qu'ils réénumèrent les
  // adaptateurs (un device Direct3D le temps de l'appel) et écrivent le fichier
  // d'options. Rien ne ferme la session — le panneau reste ouvert, et l'on
  // affiche ce qui a été retenu.
  if (pending_restart_) {
    pending_restart_ = false;
    const bool has_mode =
        draft_.mode >= 0 && draft_.mode < static_cast<int>(modes_.size());
    if (has_mode) {
      saved_structural_ = gamesettings::graphics::ApplyStructural(
          draft_.system, draft_.adapter, modes_[draft_.mode].width,
          modes_[draft_.mode].height, modes_[draft_.mode].bpp, draft_.fullscreen);
      // Le brouillon EST la configuration, maintenant : sans cette ligne le
      // bouton resterait actif et proposerait d'enregistrer ce qui vient de
      // l'être — la seule alternative étant de réénumérer pour le savoir.
      if (saved_structural_) current_adapter_ = draft_.adapter;
    }
    // 🔴 L'arrêt APRÈS l'écriture, et seulement si elle a réussi : quitter sur un
    // réglage non enregistré ferait perdre la session pour rien.
    if (pending_shutdown_) {
      pending_shutdown_ = false;
      if (saved_structural_) {
        Close();  // ne pas laisser un panneau ouvert par-dessus un client qui s'arrête
        gamesettings::graphics::ShutdownClient();
      }
    }
    return;
  }

  // Les trois réglages « effet immédiat » : différés jusqu'ici parce que les
  // appliquer recharge des textures du client.
  if (pending_hot_.sprite >= 0 || pending_hot_.texture >= 0 ||
      pending_hot_.trilinear >= 0) {
    const PendingHotGraphics hot = pending_hot_;
    pending_hot_ = PendingHotGraphics();
    if (hot.sprite >= 0)    gamesettings::graphics::SetSpriteDetail(hot.sprite);
    if (hot.texture >= 0)   gamesettings::graphics::SetTextureDetail(hot.texture);
    if (hot.trilinear >= 0) gamesettings::graphics::SetTrilinear(hot.trilinear != 0);
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
  if (uiwnd::FindWindow(uiwnd::kCUIGameSettingsUI))
    uiwnd::CloseWindow(uiwnd::kCUIGameSettingsUI);
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
    ImGui::TextColored(ro::pal::kSecondaryText, "%s",
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
      const bool selected = ImGui::BeginTabItem(tab_id);
      // ── Maj + clic sur la languette : le lien dans le chat ──────────────────
      //
      // Le geste se lit APRÈS `BeginTabItem` et AVANT `EndTabItem` : c'est la
      // languette qui est le dernier item, et le corps de l'onglet le remplacera
      // dès la première ligne dessinée.
      //
      // ⚠ Contrairement à un en-tête repliable, un onglet ACCEPTE le clic
      // modifié : la languette bascule donc aussi. On le laisse — poser un lien
      // vers un onglet en l'ouvrant au passage montre ce qu'on vient de poser, et
      // l'annuler demanderait de rejouer l'état interne de la barre d'onglets.
      const char* link_key = gslink::KeyByTab(tab);
      if (link_key != nullptr && links::CanPostToChat()) {
        if (links::ShiftClickedLastItem())
          links::PostToChat(links::FromSetting(link_key));
        // Le geste n'a aucune trace visible : sans cette aide, il n'existe que
        // pour qui l'a lu dans un changelog.
        else if (links::HoveredForLinkTooltip())
          ImGui::SetTooltip(i18n::Tr("Maj + clic : poser le lien de cet onglet "
                                     "dans le chat"));
      }
      if (selected) {
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
  // ⛔ PAS DE `RegionWheel` ICI, et c'était un contresens qui MANGEAIT le
  // défilement. `RegionWheel` sert à une zone qui VEUT la molette pour son propre
  // usage — un aperçu qui tourne, les pages d'un livre : elle en revendique la
  // possession (`SetKeyOwner`) dès que le curseur s'y est posé un instant, puis
  // la consomme. L'appliquer au corps défilant, c'est la retirer à la seule chose
  // qui devait l'avoir.
  //
  // Le symptôme était exactement celui décrit en jeu — « ne fonctionne pas
  // toujours » : la molette marchait dans les 0,3 s qui suivaient un mouvement de
  // souris, puis mourait. Pire, le verrou ne se relâchait plus : il ne se rearme
  // que lorsqu'un défilement a EU LIEU, et il n'en avait plus lieu.
  //
  // Il n'y avait rien à protéger. Les deux curseurs de volume passent par
  // `mui::WheelSliderInt`, qui filtre déjà la molette par `LastItemWheel` — c'est
  // LÀ que vit le verrou anti-défilement, au widget qui prend la molette, jamais
  // au conteneur qui défile (feedback_imgui_wheel_scroll_gate).
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

  // ── Avertissement DirectX 7 ────────────────────────────────────────────────
  // Le même constat que la modale de démarrage, à la source partagée : c'est une
  // liste de fonctionnalités qui bouge, et deux copies auraient divergé.
  if (confirm_dx7_) {
    ImGui::OpenPopup("###gs_confirm_dx7");
    confirm_dx7_ = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  char dx7_id[128];
  std::snprintf(dx7_id, sizeof(dx7_id), "%s###gs_confirm_dx7",
                i18n::Tr("DirectX 7 ?"));
  if (ro::BeginRoPopupModal(dx7_id)) {
    ro::SuppressEscapeStack();

    ImGui::TextColored(kChangedText, "%s",
                       i18n::Tr("DirectX 7 éteint une partie de Bourgeon."));
    ImGui::Spacing();
    dx7::DrawWarningBody();
    ImGui::Spacing();

    const float w = ro::MaxButtonWidth(
        {i18n::Tr("Choisir DirectX 7 quand même"), i18n::Tr("Rester en DirectX 9")});
    if (ro::RoButton(i18n::Tr("Choisir DirectX 7 quand même"), w))
      ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Rester en DirectX 9"), w)) {
      // Retour au choix précédent, listes comprises : le brouillon ne doit pas
      // garder un adaptateur énuméré pour l'autre API.
      draft_.system = system_before_dx7_;
      pending_graphics_refresh_ = true;
      ImGui::CloseCurrentPopup();
    }
    ro::EndRoPopupModal();
  }

  // ── Confirmation d'un réglage qui n'agira qu'au prochain démarrage ─────────
  //
  // 🔴 ON NE FERME PAS LA SESSION, ET ON NE PROMET PAS DE RELANCE. Le [Apply]
  // natif pose `g_RestartRequested` — mais ce drapeau ne relance rien : à la
  // sortie de sa boucle, le client ouvre la PAGE WEB du lanceur d'origine
  // (`MSI_WEB_ADDRESS_FOR_RESTART`) et s'arrête. Reproduire le geste faisait
  // donc surgir un navigateur au lieu d'un client, signalé en jeu le
  // 2026-08-15. On écrit la configuration, on la sauve, et on le dit.
  if (confirm_restart_) {
    ImGui::OpenPopup("###gs_confirm_restart");
    confirm_restart_ = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  char restart_id[128];
  std::snprintf(restart_id, sizeof(restart_id), "%s###gs_confirm_restart",
                i18n::Tr("Au prochain démarrage"));
  if (ro::BeginRoPopupModal(restart_id)) {
    ro::SuppressEscapeStack();

    ImGui::TextWrapped("%s",
                       i18n::Tr("Ces réglages sont enregistrés tout de suite, mais "
                                "le client ne les lit qu'à son démarrage : ils "
                                "prendront effet la prochaine fois que vous "
                                "lancerez le jeu."));
    ImGui::Spacing();
    ImGui::TextColored(kChangedText, "%s",
                       i18n::Tr("Quitter maintenant vous déconnecte : le client se "
                                "ferme, et c'est à vous de le relancer."));
    ImGui::Spacing();
    const float w = ro::MaxButtonWidth({i18n::Tr("Enregistrer"),
                                        i18n::Tr("Enregistrer et quitter"),
                                        i18n::Tr("Annuler")});
    if (ro::RoButton(i18n::Tr("Enregistrer"), w)) {
      pending_restart_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Enregistrer et quitter"), w)) {
      pending_restart_ = true;
      pending_shutdown_ = true;
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
    // 🔴 NOTRE copie D'ABORD : c'est la seule qui survive à la relance (cf. le
    // .h). L'écriture dans la table du client suit, pour la session en cours.
    emblem_frame_ = emblem;
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    PendingWrite write;
    write.valid = true;
    write.id = kTtEmblemFrame;
    write.on = emblem;
    // ⛔ PLUS DE CHEMIN PAR NOM DE COMMANDE. La clé de cette option n'existe pas
    // dans la table des drapeaux — `SetFlagRaw` ne met à jour que l'existant —
    // et le réglage est pour cette raison inerte JUSQUE DANS LA FENÊTRE NATIVE.
    // La première correction passait par « /frame », la seule fonction qui
    // INSÈRE… mais cette chaîne n'existe nulle part dans le client, ni dans le
    // `CmdOnOffList` du Lua : la résolution rendait « commande inconnue » et la
    // case restait morte. `SetOn` insère désormais lui-même (docs §3.3).
    pending_writes_.push_back(write);
  }
  ImGui::SameLine();
  // 🔴 L'INFOBULLE DIT LA CONDITION MANQUANTE, parce que le réglage seul ne
  // suffit jamais : le client calcule `cadre = (carte GvG) ET (/frame)`. Sans
  // cette phrase, la case passe pour cassée — c'est exactement ce qui a coûté
  // une demi-journée d'enquête (docs/game_option_re.md §3.3).
  mui::HelpMarker(
      i18n::Tr("Encadre l'emblème de guilde au-dessus des noms. Le client ne "
               "dessine ce cadre que sur une carte de siège (GvG) : ailleurs la "
               "commande bascule, mais rien ne change à l'écran. Bourgeon "
               "l'honore partout où il dessine lui-même l'emblème, comme la "
               "fiche de personnage."));

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
  if (ro::RoBeginCombo("###gs_skin", current_name)) {
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
    ro::RoEndCombo();
  }
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Remplace les images de l'interface du client.\n"
               "Le changement recharge toutes les textures : un temps d'arrêt "
               "d'une fraction de seconde est normal."));

  if (skin_count == 0) {
    ImGui::TextColored(ro::pal::kSecondaryText, "%s",
                       i18n::Tr("Aucun skin installé — seule l'apparence "
                                "d'origine est disponible."));
  }

  mui::PopStyleCompact();

  // Ce que cet onglet ne reprend toujours pas — dit franchement plutôt que laissé
  // deviner. Les graphismes restent au client : ce sont des resets de device.
  ImGui::Spacing();
  ImGui::TextColored(ro::pal::kSecondaryText, "%s",
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

  // Les libellés sont composés ICI, une fois par énumération — pas à la frame :
  // chacun interroge Windows sur la sortie (`EnumDisplayDevices` puis
  // `EnumDisplaySettings`), ce qui n'a rien à faire dans une boucle de rendu.
  adapter_labels_.clear();
  adapter_labels_.reserve(adapters_.size());
  for (size_t i = 0; i < adapters_.size(); ++i)
    adapter_labels_.push_back(FormatAdapterLabel(adapters_[i], static_cast<int>(i) + 1));

  // Celui que le client choisira réellement — relu ICI et nulle part ailleurs,
  // parce que le savoir coûte une énumération de plus.
  current_adapter_ = (draft_.system == gfx::System()) ? gfx::CurrentAdapterIndex() : -1;

  // Première venue : le brouillon part de la configuration réelle.
  if (draft_.adapter < 0)
    draft_.adapter = (current_adapter_ >= 0) ? current_adapter_ : 0;

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

  // ── ⚡ Ce qui ne demande pas de redémarrage ────────────────────────────────
  //
  // 🔴 « Sans redémarrage » ne veut PAS dire « visible sur-le-champ », et la
  // nuance est celle du client, pas la nôtre (docs §3.10) :
  //   - la finesse des TEXTURES l'est vraiment : le cache de sprites est
  //     rechargé avec le nouveau diviseur ;
  //   - la finesse des SPRITES est lue par `CSprRes_Load`, donc au CHARGEMENT
  //     d'un sprite : elle ne touche pas ceux déjà en mémoire ;
  //   - le filtrage trilinéaire est FIGÉ dans chaque texture à sa création : il
  //     ne change que celles recréées ensuite — l'interface tout de suite, le
  //     reste au fil des rechargements.
  // Chacun le dit à sa ligne. Promettre un effet immédiat pour les deux derniers
  // ferait passer le comportement normal du client pour une panne.
  mui::SeparatorText(i18n::Tr("Sans redémarrage"));

  const char* detail_names[] = {i18n::Tr("Basse"), i18n::Tr("Moyenne"),
                                i18n::Tr("Élevée")};

  // Affichage OPTIMISTE : l'écriture est différée au tick, et sans cela la liste
  // se redessinerait dans son ancien état entre le clic et le tick.
  const int sprite = (pending_hot_.sprite >= 0) ? pending_hot_.sprite
                                                : gfx::SpriteDetail();
  ImGui::SetNextItemWidth(ro::Px(150.0f));
  if (ro::RoBeginCombo(msgstr::Utf8Or(kMsgSpriteDetail,
                                       i18n::Tr("Finesse des sprites")),
                        detail_names[sprite])) {
    for (int i = 0; i <= gfx::kDetailMax; ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(detail_names[i], i == sprite)) pending_hot_.sprite = i;
      ImGui::PopID();
    }
    ro::RoEndCombo();
  }
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Le client lit ce réglage quand il CHARGE un sprite : les "
               "personnages et monstres déjà à l'écran gardent leur finesse.\n"
               "Changez de carte pour tout revoir."));

  const int texture = (pending_hot_.texture >= 0) ? pending_hot_.texture
                                                  : gfx::TextureDetail();
  ImGui::SetNextItemWidth(ro::Px(150.0f));
  if (ro::RoBeginCombo(msgstr::Utf8Or(kMsgTextureDetail,
                                       i18n::Tr("Finesse des textures")),
                        detail_names[texture])) {
    for (int i = 0; i <= gfx::kDetailMax; ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(detail_names[i], i == texture)) pending_hot_.texture = i;
      ImGui::PopID();
    }
    ro::RoEndCombo();
  }
  ImGui::SameLine();
  mui::HelpMarker(i18n::Tr("Divise la résolution des textures pour économiser de "
                           "la mémoire vidéo.\nLes textures déjà chargées sont "
                           "rechargées aussitôt."));

  bool trilinear = (pending_hot_.trilinear >= 0) ? (pending_hot_.trilinear != 0)
                                                 : gfx::Trilinear();
  if (ro::RoCheckbox(msgstr::Utf8Or(kMsgTrilinear, i18n::Tr("Filtrage trilinéaire")),
                     &trilinear)) {
    pending_hot_.trilinear = trilinear ? 1 : 0;
  }
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Lisse les textures vues de loin. Coût négligeable sur une carte "
               "moderne.\n"
               "Le filtre est figé dans chaque texture au moment où elle est "
               "créée : le changement se voit tout de suite sur l'interface, et "
               "sur le reste au fur et à mesure des rechargements."));

  // ── 🔁 Ce qui n'est lu qu'au démarrage ────────────────────────────────────
  ImGui::Spacing();
  mui::SeparatorText(i18n::Tr("Au prochain démarrage"));

  ImGui::TextColored(ro::pal::kSecondaryText, "%s",
                     i18n::Tr("Le client ne lit ces quatre réglages qu'à son "
                              "démarrage. Ils sont enregistrés tout de suite et "
                              "prendront effet au prochain lancement."));
  ImGui::Spacing();

  // Système de rendu — la liste du client est en dur, deux entrées.
  struct SystemChoice { int value; const char* name; };
  const SystemChoice kSystems[] = {{gfx::kRenderDx7, "DirectX 7"},
                                   {gfx::kRenderDx9, "DirectX 9"}};
  const char* system_name = (draft_.system == gfx::kRenderDx7) ? "DirectX 7" : "DirectX 9";
  ImGui::SetNextItemWidth(ro::Px(150.0f));
  if (ro::RoBeginCombo(msgstr::Utf8Or(kMsgRenderSystem, i18n::Tr("API graphique")),
                        system_name)) {
    for (const SystemChoice& choice : kSystems) {
      ImGui::PushID(choice.value);
      if (ImGui::Selectable(choice.name, draft_.system == choice.value) &&
          draft_.system != choice.value) {
        // 🔴 Choisir DX7 mérite l'avertissement, et il vaut mieux le donner ICI
        // qu'après la relance : en DX7 le moteur n'a ni shaders ni cible de
        // rendu, et une bonne part de Bourgeon s'éteint. On garde de quoi
        // revenir en arrière si le joueur refuse.
        if (choice.value == gfx::kRenderDx7) {
          system_before_dx7_ = draft_.system;
          confirm_dx7_ = true;
        }
        draft_.system = choice.value;
        // Changer d'API renouvelle adaptateurs ET modes : on redemande au
        // client, au tick — l'énumération DX9 crée un device Direct3D.
        pending_graphics_refresh_ = true;
      }
      ImGui::PopID();
    }
    ro::RoEndCombo();
  }

  // Adaptateur.
  //
  // 🔴 Direct3D 9 énumère une sortie D'AFFICHAGE, pas une carte : deux écrans
  // branchés sur la même carte donnent deux lignes à la description identique.
  // Le numéro d'écran est la seule chose qui les distingue, et sans lui la liste
  // est inutilisable — c'est exactement ce qu'un joueur a signalé.
  const char* adapter_label = i18n::Tr("(aucun)");
  for (size_t i = 0; i < adapters_.size() && i < adapter_labels_.size(); ++i) {
    if (adapters_[i].index == draft_.adapter) {
      adapter_label = adapter_labels_[i].c_str();
      break;
    }
  }

  // La largeur est MESURÉE sur les libellés : ils portent désormais la
  // définition de l'écran, et une largeur fixe couperait le nom de la carte.
  float combo_width = ro::Px(300.0f);
  for (const std::string& label : adapter_labels_) {
    const float needed = ImGui::CalcTextSize(label.c_str()).x +
                         ImGui::GetStyle().FramePadding.x * 2.0f +
                         ImGui::GetFrameHeight();  // la flèche du combo
    combo_width = (std::max)(combo_width, needed);
  }
  ImGui::SetNextItemWidth(combo_width);
  if (ro::RoBeginCombo(msgstr::Utf8Or(kMsgGraphicDevice, i18n::Tr("Carte graphique")),
                        adapter_label)) {
    for (size_t i = 0; i < adapters_.size(); ++i) {
      const gfx::Adapter& adapter = adapters_[i];
      const char* label = (i < adapter_labels_.size()) ? adapter_labels_[i].c_str()
                                                       : adapter.name;
      ImGui::PushID(adapter.index);
      if (ImGui::Selectable(label, adapter.index == draft_.adapter) &&
          adapter.index != draft_.adapter) {
        draft_.adapter = adapter.index;
        pending_graphics_refresh_ = true;  // les modes dépendent de l'adaptateur
      }
      // Le nom Windows de la sortie n'est plus à la ligne, mais il reste ce
      // qu'un joueur pourra citer si sa liste ne ressemble pas à ses Paramètres.
      if (adapter.device[0] != '\0' && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", adapter.device);
      ImGui::PopID();
    }
    ro::RoEndCombo();
  }
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Une même carte apparaît une fois PAR ÉCRAN branché : ce que vous "
               "choisissez ici, c'est l'écran sur lequel le jeu s'ouvrira.\n"
               "Le numéro est celui de vos Paramètres Windows ; le nom technique "
               "de la sortie s'affiche sous la souris.\n"
               "Uniquement en plein écran : en fenêtré, le client place toujours "
               "sa fenêtre sur l'écran principal de Windows."));

  // 🔴 Le dire À LA LIGNE, pas seulement dans l'infobulle : en fenêtré, ce choix
  // n'a AUCUN effet, et un réglage qui ne fait rien sans le dire passe pour une
  // panne. `GameWindow_Create` place la fenêtre d'après
  // `GetSystemMetrics(SM_CXSCREEN)` — l'écran principal — et borne toute
  // abscisse qui en sortirait ; l'adaptateur n'entre jamais dans cette fonction.
  // Signalé en jeu le 2026-08-15 : « il ouvre toujours sur écran 1 ».
  if (!draft_.fullscreen && adapters_.size() > 1) {
    ImGui::TextColored(ro::pal::kSecondaryText, "%s",
                       i18n::Tr("En fenêtré, le jeu s'ouvre toujours sur l'écran "
                                "principal de Windows."));
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
  if (ro::RoBeginCombo(msgstr::Utf8Or(kMsgResolution, i18n::Tr("Résolution")),
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
    ro::RoEndCombo();
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
  //
  // 🔴 `current_adapter_` et pas `gfx::CurrentAdapterIndex()` : depuis qu'elle
  // compare comme le client, cette fonction ÉNUMÈRE — donc crée un device
  // Direct3D le temps de l'appel. L'appeler ici la mettrait dans le chemin de
  // CHAQUE FRAME. Elle est relue avec les listes, c'est-à-dire quand quelque
  // chose a pu changer.
  const bool has_mode = (draft_.mode >= 0 && draft_.mode < static_cast<int>(modes_.size()));
  const bool changed =
      draft_.system != gfx::System() || draft_.fullscreen != gfx::Fullscreen() ||
      draft_.adapter != current_adapter_ ||
      (has_mode && (modes_[draft_.mode].width != gfx::Width() ||
                    modes_[draft_.mode].height != gfx::Height() ||
                    modes_[draft_.mode].bpp != gfx::BitsPerPixel()));

  ImGui::Spacing();
  ImGui::BeginDisabled(!changed || !has_mode);
  if (ro::RoButton(i18n::Tr("Enregistrer ces réglages"),
                   ro::ButtonWidth(i18n::Tr("Enregistrer ces réglages"))))
    confirm_restart_ = true;
  ImGui::EndDisabled();
  if (!has_mode && !modes_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kChangedText, "%s", i18n::Tr("Choisissez une résolution."));
  } else if (saved_structural_ && !changed) {
    // Rien ne bouge à l'écran avant le prochain démarrage : sans cet accusé de
    // réception, le joueur n'a qu'un bouton qui se grise pour toute réponse.
    ImGui::SameLine();
    ImGui::TextColored(ro::pal::kSecondaryText, "%s",
                       i18n::Tr("Enregistré. Actif au prochain démarrage du jeu."));
  }

  DrawBourgeonGraphics();
  mui::PopStyleCompact();
}

// ── Les réglages d'image qui n'appartiennent PAS au client ──────────────────
//
// Ils vivaient dans la section « Graphismes » de Moonlight Settings. Ils sont ici
// parce qu'un joueur qui cherche un réglage d'image ne devrait pas avoir à
// deviner lequel de deux panneaux le porte — le client ni Bourgeon ne sont une
// distinction qui l'intéresse.
//
// La frontière reste dite, en revanche : ce qui suit ne passe par aucun réglage
// du client, n'est écrit dans aucun de ses fichiers, et survit à un [Tout
// réinitialiser] qui, lui, ne touche que les siens.
void GameSettings::DrawBourgeonGraphics() {
  ImGui::Spacing();
  mui::SeparatorText(i18n::Tr("Ajouts de Bourgeon"));

  if (auto* screen_fx = Bourgeon::Instance().screen_fx()) screen_fx->DrawSettings();

  // ⛔ PLUS DE CASE « sprites d'armes doubles » ICI. Le comportement est devenu
  // le DÉFAUT — une arme porte son sprite, main gauche comprise — donc il n'y a
  // plus rien à décider pour un joueur. La bascule survit dans Staff Tools, où
  // elle sert à comparer avec le rendu d'origine du client.
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
      if (filter_[0] && !text::ContainsNoCase(row.option.title, filter_) &&
          !text::ContainsNoCase(row.option.tooltip, filter_) &&
          !text::ContainsNoCase(row.option.description, filter_))
        continue;

      ++shown;
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      int column = 0;

      if (all_mode) {
        ImGui::TableSetColumnIndex(column++);
        ImGui::TextColored(ro::pal::kSecondaryText, "%s", TabLabel(row.option.tab));
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
        ImGui::TextColored(ro::pal::kSecondaryText, "%s",
                           msgstr::Flatten(row.option.tooltip));
      }

      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  if (shown == 0) {
    ImGui::TextColored(ro::pal::kSecondaryText, "%s",
                       filter_[0] ? i18n::Tr("Aucun réglage ne correspond.")
                                  : i18n::Tr("Aucun réglage dans cet onglet."));
  }
}
