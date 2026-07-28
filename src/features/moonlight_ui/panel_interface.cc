#include "features/moonlight_ui/internal.h"

#include <windows.h>
#include <shellapi.h>  // ShellExecuteA (lien « avatar Discord » vers l'UCP)

#include <algorithm>

#include "bourgeon.h"
#include "imgui.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "ui/align_grid.h"
#include "ui/color_codec.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "ui/skin_panel.h"

// Types COMPLETS des plugins pilotés par les 13 sections (bourgeon.h n'en donne
// que des déclarations anticipées).
#include "features/overlays/basic_info.h"
#include "features/windows/bank_window.h"
#include "features/systems/bug_report.h"
#include "features/patches/chat.h"
#include "features/windows/inventory_viewer.h"
#include "features/windows/cart_viewer.h"
#include "features/windows/item_desc_window.h"
#include "features/overlays/menu_icons.h"
#include "features/windows/npc_dialog_window.h"
#include "features/overlays/quest_tracker_tweaks.h"
#include "features/windows/rodex_window.h"
#include "features/overlays/skill_bar_tweaks.h"
#include "features/overlays/status_icon_tweaks.h"
#include "features/windows/storage_window.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// Page du panneau utilisateur (UCP) du site qui génère l'avatar Discord du
// personnage au bon format. Mentionnée dans la section « Chat », à côté du
// réglage du relais Discord.
constexpr const char* kDiscordAvatarUrl =
    "https://moonlight-destiny.fr/ucp.php?i=profile&mode=avatar";

// ── En-tête « Interface de jeu » ─────────────────────────────────────────────
// Navigation latérale + les 13 sections de configuration. C'était le bloc dominant
// d'OnRenderUI (742 lignes sur 1702) : une nav, puis une cascade de 11 tests sur
// iface_nav_, chacun avec sa propre variable changed homonyme.
// La table kIfaceSections (source unique libellé + identifiant, cf. chantier 5)
// vit ici, au plus près de son usage.
void MoonlightUi::DrawInterfacePanel() {
  // Saut demandé (bullet de barre de titre d'une fenêtre Bourgeon) : on force
  // l'en-tête ouvert et on scrolle dessus, une seule fois.
  const bool jump_requested = pending_iface_jump_;
  pending_iface_jump_ = false;
  if (jump_requested) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  if (CollapsingHeader("Interface de jeu")) {
    if (jump_requested) ImGui::SetScrollHereY(0.0f);
    PushStyleCompact();
    bool changed = false;
    changed |= ro::RoCheckbox("Grille d'alignement", &grid_.show);
    SameLine(); HelpMarker(
        "Affiche une grille plein écran pour aligner ton interface "
        "(comme les add-ons d'interface de WoW).");
    ImGui::SetNextItemWidth(160.0f);
    changed |= WheelSliderInt("Taille grille", &grid_.cell_size_px, 4, 128);
    changed |= ro::RoCheckbox("Aimanter à la grille", &grid_.snap);
    SameLine(); HelpMarker(
        "Les barres et les icônes s'alignent sur les cellules de la grille "
        "pendant le déplacement et le redimensionnement.");
    changed |= ColorEdit4WithAlphaBar("Couleur grille", grid_.color);

    // (Inventaire, Cart et Storage : tout est regroupé dans leurs sections
    // dédiées, sous la navigation latérale ci-dessous.)

    // (Cash shop, boutiques PNJ, échoppe joueur — vente ET échoppe d'achat —,
    // courrier RODEX et feuille de personnage (Alt+F) : AUCUNE case ici. Tous font
    // partie de l'interrupteur GLOBAL « Interface moderne » (liste dans
    // SetModernInterface, moonlight_ui.h), porté par les sections Inventaire,
    // Cart, Storage et Barre d'action.
    // Ils achètent, vendent, joignent ou équipent VERS et DEPUIS l'inventaire :
    // moderne d'un côté et natif de l'autre, les objets ne se glissent plus. Une
    // case isolée rouvrirait donc exactement le mixe qu'on a supprimé.)

    // Bouton « Signaler un bug » (desc item/skill + dialogue PNJ + raccourci).
    if (auto* br = Bourgeon::Instance().bug_report()) {
      changed |= ro::RoCheckbox("Afficher le bouton « Signaler un bug »", &br->enabled());
      SameLine(); HelpMarker(
          "Affiche le bouton de rapport de bug dans les fenêtres de "
          "description (item/skill) et le dialogue PNJ, et active le "
          "raccourci Ctrl+Alt+B. Décoche pour tout désactiver.");
    }

    if (changed) SaveSettings();

    // Navigation latérale (liste à gauche, contenu à droite). L'entrée active est
    // un MEMBRE (iface_nav_) : OpenInterfaceSection la pilote depuis le bullet de
    // barre de titre d'une autre fenêtre Bourgeon.
    // Source UNIQUE des sections : chaque ligne porte son identifiant d'enum ET
    // son libellé. Insérer/déplacer une entrée ne peut donc plus désaligner
    // silencieusement le libellé et le contenu — la panne muette que produisait
    // la paire « enum + tableau de chaînes » maintenue à la main.
    struct IfaceEntry { IfaceSection id; const char* label; };
    static constexpr IfaceEntry kIfaceSections[] = {
        {kIfaceSkillBar,    "Barre d'action"},
        {kIfaceBasicInfo,   "Basic Info"},
        {kIfaceChat,        "Chat"},
        {kIfaceMenuIcons,   "Icônes du menu"},
        {kIfaceStatusIcons, "Icônes de statut"},
        {kIfaceQuest,       "Suivi de quête"},
        {kIfaceDesc,        "Descriptions"},
        {kIfaceSkin,        "Skin RO"},
        {kIfaceNpc,         "Fenêtre NPC"},
        {kIfaceStorage,     "Storage"},
        {kIfaceInventory,   "Inventaire"},
        {kIfaceCart,        "Cart"},
        {kIfaceBank,        "Banque"},
    };
    static_assert(IM_ARRAYSIZE(kIfaceSections) == kIfaceCount,
                  "kIfaceSections doit couvrir exactement l'enum IfaceSection");

    // Dimensions dérivées du texte/style (pas de pixels fixes) : la liste garde
    // la largeur de sa plus longue entrée, bornée à 40 % de la place dispo pour
    // rester lisible sur fenêtre étroite.
    // La mesure des 13 libellés ne dépend que de la POLICE : on la garde en cache
    // au lieu de refaire 13 CalcTextSize à chaque frame, et on la réinvalide quand
    // la police change (bascule du skin RO, taille de police).
    const ImGuiStyle& st = ImGui::GetStyle();
    static float s_labels_w    = 0.0f;   // largeur du plus long libellé, en px
    static ImFont* s_labels_font = nullptr;
    static float s_labels_size = 0.0f;
    if (s_labels_font != ImGui::GetFont() || s_labels_size != ImGui::GetFontSize()) {
      s_labels_w = 0.0f;
      for (const IfaceEntry& entry : kIfaceSections)
        s_labels_w = (std::max)(s_labels_w, ImGui::CalcTextSize(entry.label).x);
      s_labels_font = ImGui::GetFont();
      s_labels_size = ImGui::GetFontSize();
    }
    float nav_w = s_labels_w + st.WindowPadding.x * 2.0f + st.FramePadding.x * 2.0f;
    const float nav_w_min = ImGui::GetFontSize() * 5.0f;
    const float nav_w_max =
        (std::max)(nav_w_min, ImGui::GetContentRegionAvail().x * 0.4f);
    nav_w = (std::min)((std::max)(nav_w, nav_w_min), nav_w_max);
    const float nav_h = ImGui::GetTextLineHeightWithSpacing() * kIfaceCount
                      + st.WindowPadding.y * 2.0f;

    ImGui::BeginChild("iface_nav", ImVec2(nav_w, nav_h), ImGuiChildFlags_Borders);
    for (const IfaceEntry& entry : kIfaceSections)
      if (ImGui::Selectable(entry.label, iface_nav_ == entry.id)) iface_nav_ = entry.id;
    ImGui::EndChild();

    SameLine();
    // Hauteur libre (AutoResizeY) : le panneau prend exactement la place de son
    // contenu et c'est la fenêtre parente qui scrolle — plus de dépendance à la
    // taille de la fenêtre ni de scrollbar imbriquée.
    ImGui::BeginChild("iface_content", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PushTextWrapPos(0.0f);  // wrap le texte à la largeur du child
    {
      PushItemWidth(160.0f);

      // ── Barre d'action ───────────────────────────────────────────────────
      if (iface_nav_ == kIfaceSkillBar)
      {
        if (auto* sb = Bourgeon::Instance().skill_bar())
          sb->DrawSettings();
        else
          GrayText(kPluginUnavailable);
      }

      // ── Barres d'info (HUD bars + alignment grid) ────────────────────────
      // ── Status Portrait (head + pseudo + classe + niveau, indépendants) ──
      if (iface_nav_ == kIfaceBasicInfo) {
        if (auto* basic_info = Bourgeon::Instance().basic_info()) {
          if (basic_info->DrawSettings()) SaveSettings();
        } else {
          GrayText(kPluginUnavailable);
        }
      }

      // ── Chat Settings ────────────────────────────────────────────────────
      if (iface_nav_ == kIfaceChat) {
        bool changed = false;
        // Deux propriétaires dans cette section, et c'est assumé : le relais
        // Discord est un réglage SERVEUR (MoonlightUi l'envoie), les retouches de
        // la fenêtre appartiennent à ChatTweaks, et les couleurs de fond au patch
        // mémoire de MoonlightUi. Les accolades restent pour ne pas réindenter.
        {
          PushStyleCompact();

          SeparatorText("Réglages généraux");
          if (ro::RoCheckbox("Chat Discord (Gonryun only)", &discord_chat_)) {
            UpdateRelay();
            SendSetting(kSettingDiscordChat, discord_chat_ ? 1 : 0);
          }
          SameLine(); HelpMarker(
              "Relaie le canal Discord du serveur dans le chat du jeu, et tes "
              "messages vers Discord — uniquement sur la carte Gonryun.");

          // Avatar Discord : la page UCP du site génère l'image du personnage
          // déjà recadrée/dimensionnée pour Discord. C'est la MÊME identité que
          // le relais ci-dessus (le pseudo affiché côté Discord), d'où la place
          // de la mention ici plutôt que dans un panneau « compte ».
          GrayText(
              "Ton avatar Discord : le panneau utilisateur du site génère "
              "l'image de ton personnage à la bonne dimension pour Discord, en "
              "un clic.");
          if (ro::RoButton("Générer mon avatar Discord")) {
            ShellExecuteA(nullptr, "open", kDiscordAvatarUrl, nullptr, nullptr,
                          SW_SHOWNORMAL);
          }
          SameLine(); HelpMarker(kDiscordAvatarUrl);

          if (auto* chat_tweaks = Bourgeon::Instance().chat_tweaks()) {
            changed |= chat_tweaks->DrawSettings();
          } else {
            GrayText(kPluginUnavailable);
          }

          SeparatorText("Couleurs du chat");
          // Les trois fonds appartiennent à ChatTweaks (patch .text + parcours du
          // tas). Ils sont dessinés un par un et non en bloc pour une seule
          // raison : la case « Barre de préréglages » ci-dessous est un réglage de
          // MoonlightUi, et elle se pose à DROITE du premier sélecteur.
          if (auto* chat_tweaks = Bourgeon::Instance().chat_tweaks()) {
            if (chat_tweaks->bg_available()) {
              changed |= chat_tweaks->DrawBackgroundGroup(ChatTweaks::kBgMain);
              SameLine();
              changed |= ro::RoCheckbox("Barre de préréglages", &mainchat_preset_bar_);
              changed |= chat_tweaks->DrawBackgroundGroup(ChatTweaks::kBgDetached);
              changed |= chat_tweaks->DrawBackgroundGroup(ChatTweaks::kBgWhisper);
            } else {
              GrayText("(patch du fond de chat indisponible)");
            }
          }

          PopStyleCompact();
        }
        if (changed) SaveSettings();
      }

      // ── Menu icons (ImGui replacement) ───────────────────────────────────
      if (iface_nav_ == kIfaceMenuIcons) {
        if (auto* mi = Bourgeon::Instance().menu_icons()) {
          if (mi->DrawSettings()) SaveSettings();
        } else {
          GrayText(kPluginUnavailable);
        }
      }

      // ── Status icons (StatusIconTweaks) ──────────────────────────────────
      if (iface_nav_ == kIfaceStatusIcons) {
        if (auto* si = Bourgeon::Instance().status_icons())
          si->DrawSettings();
        else
          GrayText(kPluginUnavailable);
      }

      // ── Suivi de quête (QuestTrackerTweaks) ──────────────────────────────
      if (iface_nav_ == kIfaceQuest) {
        if (auto* qt = Bourgeon::Instance().quest_tracker())
          qt->DrawSettings();
        else
          GrayText(kPluginUnavailable);
      }

      // ── Descriptions (ItemDescWindow : panneaux techniques item/skill) ───
      if (iface_nav_ == kIfaceDesc) {
        if (auto* idt = Bourgeon::Instance().item_desc()) {
          if (idt->DrawSettings()) SaveSettings();
        } else {
          GrayText(kPluginUnavailable);
        }
      }

      // ── Skin RO (police + habillage des fenêtres ImGui) ──────────────────
      // Seule section qui ne pilote PAS un plugin : elle règle le toolkit, donc
      // elle vit dans ui/skin_panel.cc. Ici on ne fait que l'afficher.
      if (iface_nav_ == kIfaceSkin) {
        if (ro::DrawSkinPanel()) SaveSettings();
      }

      // ── Fenêtre NPC (dialogue / menu / prompt ImGui) ─────────────────────
      if (iface_nav_ == kIfaceNpc) {
        if (auto* nd = Bourgeon::Instance().npc_dialog_window()) {
          if (nd->DrawSettings()) SaveSettings();
        } else {
          GrayText(kPluginUnavailable);
        }
      }

      // ── Storage (StorageWindow : viewer ImGui + colonnes/filtre/survol) ───
      if (iface_nav_ == kIfaceStorage) {
        if (auto* stg = Bourgeon::Instance().storage_window()) {
          if (stg->DrawSettings()) SaveSettings();
        } else {
          GrayText(kPluginUnavailable);
        }
      }

      // ── Inventaire (InventoryViewer : viewer ImGui + filtre/onglets) ──────
      if (iface_nav_ == kIfaceInventory) {
        if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
          if (iv->DrawSettings()) SaveSettings();
        } else {
          GrayText(kPluginUnavailable);
        }
      }

      // ── Cart (CartViewer : viewer ImGui + filtre/onglets) ──────────────
      if (iface_nav_ == kIfaceCart) {
        if (auto* cv = Bourgeon::Instance().cart_viewer()) {
          if (cv->DrawSettings()) SaveSettings();
        } else {
          GrayText(kPluginUnavailable);
        }
      }

      // ── Banque de zeny (BankWindow : Ctrl+B, phase 1 = coexistence) ─────
      if (iface_nav_ == kIfaceBank) {
        if (auto* bt = Bourgeon::Instance().bank_window()) {
          if (bt->DrawSettings()) SaveSettings();
        } else {
          GrayText(kPluginUnavailable);
        }
      }
      PopItemWidth();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    PopStyleCompact();
  }
}
