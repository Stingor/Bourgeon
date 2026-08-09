#include "features/moonlight_ui/internal.h"

#include <windows.h>
#include <shellapi.h>  // ShellExecuteA (lien « avatar Discord » vers l'UCP)

#include <algorithm>

#include "bourgeon.h"
#include "imgui.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/staff_gate.h"  // IsStaff (compteur de textes non traduits)
#include "ui/align_grid.h"
#include "ui/color_codec.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "ui/skin_panel.h"
#include "utils/i18n.h"
#include "utils/log_console.h"  // LogDiag (export du gabarit de traduction)

// Types COMPLETS des plugins pilotés par les 13 sections (bourgeon.h n'en donne
// que des déclarations anticipées).
#include "features/overlays/basic_info.h"
#include "features/overlays/cast_bar.h"
#include "features/overlays/chat_balloon.h"
#include "features/windows/bank_window.h"
#include "features/windows/craft_atlas.h"
#include "features/windows/make_item_window.h"
#include "features/windows/entity_context_menu.h"
#include "features/windows/monster_info_window.h"
#include "features/windows/weapon_refine_window.h"
#include "features/systems/bug_report.h"
#include "features/patches/chat.h"
#include "features/windows/chat_window.h"
#include "features/windows/inventory_viewer.h"
#include "features/windows/cart_viewer.h"
#include "features/windows/item_desc_window.h"
#include "features/overlays/menu_icons.h"
#include "features/windows/npc_dialog_window.h"
#include "features/overlays/quest_tracker.h"
#include "features/overlays/item_obtain_toast.h"
#include "features/windows/rodex_window.h"
#include "features/overlays/skill_bar.h"
#include "features/overlays/status_icon_bar.h"
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
  if (CollapsingHeader(i18n::Tr("Interface de jeu"))) {
    if (jump_requested) ImGui::SetScrollHereY(0.0f);
    PushStyleCompact();
    bool changed = false;

    // ── L'interrupteur du groupe, ICI et nulle part ailleurs ─────────────────
    // Il vivait dans cinq sections à la fois (Barre d'action, Storage, Inventaire,
    // Cart, Banque) : cinq cases synchronisées pour un seul état, chacune donnant
    // l'impression de ne concerner que sa fenêtre alors qu'elle en basculait douze.
    // Sa place est en tête de l'en-tête, au-dessus de la navigation — c'est le
    // réglage dont dépendent sept des quinze sections.
    bool modern = ModernInterfaceEnabled();
    changed |= DrawModernInterfaceCheckbox(
        &modern,
        i18n::Tr("Les réglages propres à chaque fenêtre restent dans leur section "
        "ci-dessous (Inventaire, Cart, Storage, Banque, Refine, Fabrication, "
        "Barre d'action). Ils sont grisés tant que cette case est décochée : "
        "sans elle, ces fenêtres n'existent pas."));

    // ── Langue de l'interface ────────────────────────────────────────────────
    // Le seul réglage de tout le panneau dont le libellé se traduit LUI-MÊME :
    // c'est aussi le seul qu'un joueur doit pouvoir retrouver quand l'interface
    // est déjà dans une langue qu'il ne lit pas.
    {
      // COPIE et non référence : i18n::SetLanguage écrit dans la chaîne globale
      // au milieu de la boucle ci-dessous. Une référence changerait donc de
      // valeur en cours de route, et les entrées suivantes se compareraient au
      // code qu'on vient tout juste de poser.
      const std::string current = i18n::LanguageCode();
      ImGui::SetNextItemWidth(160.0f);
      // `TrId` et non `Tr` : RoBeginCombo fait `PushID(label)`, donc un libellé
      // traduit donnerait un widget différent à chaque langue. C'est le premier
      // cas du chantier, et il sera la règle pour tout ce qui porte un état.
      if (ro::RoBeginCombo(i18n::TrId("Langue", "bourgeon_language"),
                           i18n::LabelOf(current))) {
        for (const i18n::Language& language : i18n::AvailableLanguages()) {
          const bool selected = (current == language.code);
          // Une langue sans catalogue reste VISIBLE, mais inerte. La masquer
          // laisserait croire que Bourgeon ne la connaît pas ; la griser dit ce
          // qui est vrai — elle est prévue, son fichier n'est pas là.
          if (!language.available) ImGui::BeginDisabled();
          if (ImGui::Selectable(language.label, selected) && !selected) {
            changed |= i18n::SetLanguage(language.code);
          }
          if (!language.available) ImGui::EndDisabled();
          if (selected) ImGui::SetItemDefaultFocus();
        }
        // 🔴 `ro::RoEndCombo`, PAS `ImGui::EndCombo` : RoBeginCombo n'appelle pas
        // BeginCombo, il dessine le champ à la main et ouvre un `ImGui::BeginPopup`.
        // Le refermer avec EndCombo laisserait cinq PushStyleColor et un PushID
        // sur la pile.
        ro::RoEndCombo();
      }
      SameLine(); HelpMarker(
          i18n::Tr("Langue de l'interface Bourgeon. Le jeu lui-même (noms d'objets, "
          "descriptions, messages du serveur) n'est pas concerné.\n"
          "Une langue grisée est connue mais son fichier de traduction est "
          "absent de SaveData\\lang\\."));

      // Le compteur de textes non traduits, staff uniquement : c'est un outil de
      // TRADUCTION, pas un réglage. Un joueur n'a rien à faire d'un décompte
      // qu'il ne peut pas réduire, et le voir donnerait l'impression d'une
      // interface cassée là où elle se contente de retomber en français.
      if (IsStaff() && i18n::MissingCount() > 0) {
        ImGui::TextDisabled(i18n::Tr("%zu textes sans traduction"), i18n::MissingCount());
        SameLine();
        if (ImGui::SmallButton(i18n::Tr("Exporter"))) {
          std::string exported_path;
          // Le chemin est journalisé dans les DEUX cas : en échec, c'est lui qui
          // dit pourquoi (dossier absent, fichier verrouillé), ce qu'un simple
          // « erreur » ne dirait pas.
          //
          // Deux appels plutôt qu'un gabarit choisi par ternaire : le format de
          // spdlog est vérifié à la COMPILATION, et une expression ternaire n'en
          // est pas une constante — ça ne compile pas.
          if (i18n::ExportMissing(&exported_path)) {
            LogDiag("[i18n] gabarit écrit : {}", exported_path);
          } else {
            LogDiag("[i18n] gabarit NON écrit : {}", exported_path);
          }
        }
        SameLine(); HelpMarker(
            i18n::Tr("Écrit les textes rencontrés depuis le lancement et absents du "
            "catalogue dans SaveData\\lang\\<langue>.missing.yaml, prêts à "
            "traduire. N'y figure que ce qui a été AFFICHÉ : ouvre les fenêtres "
            "concernées avant d'exporter."));
      }
    }

    changed |= ro::RoCheckbox(i18n::Tr("Grille d'alignement"), &grid_.show);
    SameLine(); HelpMarker(
        i18n::Tr("Affiche une grille plein écran pour aligner ton interface "
        "(comme les add-ons d'interface de WoW)."));
    ImGui::SetNextItemWidth(160.0f);
    changed |= WheelSliderInt(i18n::Tr("Taille grille"), &grid_.cell_size_px, 4, 128);
    changed |= ro::RoCheckbox(i18n::Tr("Aimanter à la grille"), &grid_.snap);
    SameLine(); HelpMarker(
        i18n::Tr("Les barres et les icônes s'alignent sur les cellules de la grille "
        "pendant le déplacement et le redimensionnement."));
    changed |= ColorEdit4WithAlphaBar(i18n::Tr("Couleur grille"), grid_.color);

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
      changed |= ro::RoCheckbox(i18n::Tr("Afficher le bouton « Signaler un bug »"), &br->enabled());
      SameLine(); HelpMarker(
          i18n::Tr("Affiche le bouton de rapport de bug dans les fenêtres de "
          "description (item/skill) et le dialogue PNJ, et active le "
          "raccourci Ctrl+Alt+B. Décoche pour tout désactiver."));
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
        {kIfaceCastBar,     "Barre d'incantation"},
        {kIfaceChat,        "Chat"},
        {kIfaceMenuIcons,   "Icônes du menu"},
        {kIfaceStatusIcons, "Icônes de statut"},
        {kIfaceQuest,       "Suivi de quête"},
        {kIfaceItemToast,   "Objet obtenu"},
        {kIfaceDesc,        "Descriptions"},
        {kIfaceSkin,        "Skin RO"},
        {kIfaceNpc,         "Fenêtre NPC"},
        {kIfaceStorage,     "Storage"},
        {kIfaceInventory,   "Inventaire"},
        {kIfaceCart,        "Cart"},
        {kIfaceRefine,      "Refine"},
        {kIfaceMakeItem,    "Fabrication"},
        {kIfaceMonsterInfo, "Fiche de monstre"},
        {kIfaceContextMenu, "Menu contextuel"},
        {kIfaceCraftAtlas,  "Atlas des recettes"},
    };
    // Message de static_assert : un LITTÉRAL. Il est lu à la compilation et
    // s'adresse au développeur — jamais i18n::Tr.
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
        s_labels_w = (std::max)(s_labels_w, ImGui::CalcTextSize(i18n::Tr(entry.label)).x);
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
      if (ImGui::Selectable(i18n::Tr(entry.label), iface_nav_ == entry.id)) iface_nav_ = entry.id;
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

      // ── Sections qui n'existent QUE si le groupe « Interface moderne » l'est ─
      //
      // Ces sept-là ne règlent que des fenêtres ImGui : groupe coupé, elles
      // n'existent pas et leurs options ne changent rien. Un réglage sans effet est
      // un piège — on le grise, plutôt que de laisser croire qu'il agit.
      //
      // Le test est fait ICI, au site d'appel unique, et non dans chacun des sept
      // DrawSettings() : un BeginDisabled/EndDisabled à apparier dans sept plugins
      // finit toujours par se dépareiller sur un chemin de sortie.
      const bool needs_modern =
          iface_nav_ == kIfaceSkillBar  || iface_nav_ == kIfaceStorage ||
          iface_nav_ == kIfaceInventory || iface_nav_ == kIfaceCart    ||
          iface_nav_ == kIfaceRefine    || iface_nav_ == kIfaceMakeItem  ||
          iface_nav_ == kIfaceMonsterInfo;
      const bool locked = needs_modern && !ModernInterfaceEnabled();
      if (locked) {
        // 🔴 Un APERÇU, pas un cimetière. Ces sections sont la meilleure vitrine de
        // l'interface moderne : le joueur qui les parcourt doit pouvoir LIRE ce
        // qu'elle apporte et avoir envie d'essayer. On bloque donc l'interaction —
        // un réglage sans effet est un piège — mais sans éteindre le texte.
        //
        // Volontairement AVANT le BeginDisabled, pour rester pleinement lisible, et
        // avec l'interrupteur À PORTÉE : renvoyer le joueur chercher une case en
        // haut de page, c'est le perdre.
        // Ocre d'avertissement du projet, celui de la fabrication et du refine
        // (`IM_COL32(166, 102, 0)`) : le jaune vif d'une première rédaction passait
        // mal sur le gris clair du skin RO — trop criard pour une invitation, et
        // moins lisible qu'un ton sourd sur fond pâle.
        ImGui::TextColored(ImVec4(166 / 255.0f, 102 / 255.0f, 0.0f, 1.0f),
                           i18n::Tr("Aperçu — ces réglages appartiennent à l'interface "
                           "moderne, qui est désactivée."));
        if (ro::RoButton(i18n::Tr("Activer l'interface moderne"))) {
          SetModernInterface(true);
          SaveSettings();
        }
        ImGui::Spacing();
      }
      // Seule l'INTERACTION est coupée : les libellés et leurs infobulles restent
      // lisibles, la section garde donc sa valeur de vitrine.
      ImGui::BeginDisabled(locked);

      // ── Barre d'action ───────────────────────────────────────────────────
      if (iface_nav_ == kIfaceSkillBar)
      {
        if (auto* sb = Bourgeon::Instance().skill_bar())
          sb->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
      }

      // ── Barres d'info (HUD bars + alignment grid) ────────────────────────
      // ── Status Portrait (head + pseudo + classe + niveau, indépendants) ──
      if (iface_nav_ == kIfaceBasicInfo) {
        if (auto* basic_info = Bourgeon::Instance().basic_info()) {
          if (basic_info->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Barre d'incantation (CastBar) ────────────────────────────────────
      // Sa jumelle HUD (« Cast ») se règle avec les autres barres, dans la
      // section Basic Info : c'est la même famille de widgets, même verrou,
      // même aimantation. Ici on ne traite que celles des entités.
      if (iface_nav_ == kIfaceCastBar) {
        if (auto* cast = Bourgeon::Instance().cast_bar())
          cast->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
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

          SeparatorText(i18n::Tr("Réglages généraux"));
          if (ro::RoCheckbox(i18n::Tr("Chat Discord (Gonryun only)"), &discord_chat_)) {
            UpdateRelay();
            SendSetting(kSettingDiscordChat, discord_chat_ ? 1 : 0);
          }
          SameLine(); HelpMarker(
              i18n::Tr("Relaie le canal Discord du serveur dans le chat du jeu, et tes "
              "messages vers Discord — uniquement sur la carte Gonryun."));

          // Avatar Discord : la page UCP du site génère l'image du personnage
          // déjà recadrée/dimensionnée pour Discord. C'est la MÊME identité que
          // le relais ci-dessus (le pseudo affiché côté Discord), d'où la place
          // de la mention ici plutôt que dans un panneau « compte ».
          ImGui::TextDisabled(
              i18n::Tr("Ton avatar Discord : le panneau utilisateur du site génère "
              "l'image de ton personnage à la bonne dimension pour Discord, en "
              "un clic."));
          if (ro::RoButton(i18n::Tr("Générer mon avatar Discord"))) {
            ShellExecuteA(nullptr, "open", kDiscordAvatarUrl, nullptr, nullptr,
                          SW_SHOWNORMAL);
          }
          SameLine(); HelpMarker(kDiscordAvatarUrl);

          // 🔴 Les réglages du chat NATIF ne s'affichent que tant que ce chat
          // EXISTE. La chatbox ImGui le détruit : largeur, horodatage, icônes et
          // couleurs de fond n'agiraient alors plus sur rien. Laisser des réglages
          // inertes est le pire des retours — le joueur tourne une valeur, rien ne
          // bouge, et rien ne le lui dit.
          const bool native_chat_replaced =
              Bourgeon::Instance().chat_window() != nullptr &&
              Bourgeon::Instance().chat_window()->imgui_enabled_;

          if (!native_chat_replaced) {
            if (auto* chat_tweaks = Bourgeon::Instance().chat_tweaks()) {
              changed |= chat_tweaks->DrawSettings();
            } else {
              ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
            }
          }

          // Chatbox ImGui — son remplacement. Les réglages du natif ci-dessus ne
          // s'affichent que si elle est ÉTEINTE : allumée, elle détruit la fenêtre
          // qu'ils habillent (cf. features/windows/chat_window.h).
          SeparatorText(i18n::Tr("Chatbox ImGui"));
          if (auto* chat_window = Bourgeon::Instance().chat_window()) {
            changed |= chat_window->DrawSettings();
          } else {
            ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
          }

          // La bulle au-dessus des têtes vit ICI, avec la chatbox, et pas dans
          // une section « overlays » : elle affiche exactement les mêmes lignes,
          // résolues par le MÊME parseur (ChatWindow::PlainTextFromWire). Les
          // séparer inviterait à les faire diverger.
          SeparatorText(i18n::Tr("Bulles au-dessus des têtes"));
          if (auto* balloon = Bourgeon::Instance().chat_balloon()) {
            balloon->DrawSettings();
          } else {
            ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
          }

          if (!native_chat_replaced) SeparatorText(i18n::Tr("Couleurs du chat"));
          // Les trois fonds appartiennent à ChatTweaks (patch .text + parcours du
          // tas). Ils sont dessinés un par un et non en bloc pour une seule
          // raison : la case « Barre de préréglages » ci-dessous est un réglage de
          // MoonlightUi, et elle se pose à DROITE du premier sélecteur.
          if (auto* chat_tweaks =
                  native_chat_replaced ? nullptr : Bourgeon::Instance().chat_tweaks()) {
            if (chat_tweaks->bg_available()) {
              changed |= chat_tweaks->DrawBackgroundGroup(ChatTweaks::kBgMain);
              SameLine();
              changed |= ro::RoCheckbox(i18n::Tr("Barre de préréglages"), &mainchat_preset_bar_);
              changed |= chat_tweaks->DrawBackgroundGroup(ChatTweaks::kBgDetached);
              changed |= chat_tweaks->DrawBackgroundGroup(ChatTweaks::kBgWhisper);
            } else {
              ImGui::TextDisabled(i18n::Tr("(patch du fond de chat indisponible)"));
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
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Status icons (StatusIconBar) ──────────────────────────────────
      if (iface_nav_ == kIfaceStatusIcons) {
        if (auto* si = Bourgeon::Instance().status_icons())
          si->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
      }

      // ── Suivi de quête (QuestTracker) ──────────────────────────────
      if (iface_nav_ == kIfaceQuest) {
        if (auto* qt = Bourgeon::Instance().quest_tracker())
          qt->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
      }

      // ── Bandeau « objet obtenu » (ItemObtainToast) ─────────────────
      if (iface_nav_ == kIfaceItemToast) {
        if (auto* iot = Bourgeon::Instance().item_obtain_toast())
          iot->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
      }

      // ── Descriptions (ItemDescWindow : panneaux techniques item/skill) ───
      if (iface_nav_ == kIfaceDesc) {
        if (auto* idt = Bourgeon::Instance().item_desc()) {
          if (idt->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
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
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Storage (StorageWindow : viewer ImGui + colonnes/filtre/survol) ───
      if (iface_nav_ == kIfaceStorage) {
        if (auto* stg = Bourgeon::Instance().storage_window()) {
          if (stg->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Inventaire (InventoryViewer : viewer ImGui + filtre/onglets) ──────
      if (iface_nav_ == kIfaceInventory) {
        if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
          if (iv->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Cart (CartViewer : viewer ImGui + filtre/onglets) ──────────────
      if (iface_nav_ == kIfaceCart) {
        if (auto* cv = Bourgeon::Instance().cart_viewer()) {
          if (cv->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // (Pas de section « Banque » : elle n'avait plus aucun réglage à offrir. Son
      // contenu est imposé par un fond bitmap à hauteur fixe — rien n'y est
      // masquable — et elle suit le groupe « Interface moderne » comme les autres.
      // Il ne restait qu'un paragraphe descriptif, qui n'a pas sa place dans un
      // panneau de réglages. `bank_imgui` continue d'être persisté et basculé par
      // SetModernInterface : c'est la SECTION qui disparaît, pas le réglage.)

      // ── Refine d'arme (WeaponRefineWindow : compétence Upgrade Weapon) ─
      if (iface_nav_ == kIfaceRefine) {
        if (auto* wr = Bourgeon::Instance().weapon_refine_window()) {
          if (wr->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Fabrication (MakeItemWindow : « LIST » 94 + « Manufacturing List » 79) ─
      if (iface_nav_ == kIfaceMakeItem) {
        if (auto* mk = Bourgeon::Instance().make_item_window()) {
          if (mk->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Fiche de monstre (MonsterInfoWindow : « Monster Info » 0x4D, Sense) ──
      if (iface_nav_ == kIfaceMonsterInfo) {
        if (auto* mi = Bourgeon::Instance().monster_info()) {
          ImGui::TextWrapped(
              i18n::Tr("Remplace la fenêtre « Monster Info » qu'ouvre la compétence Sense. "
              "Elle ajoute ce que le paquet du skill ne transporte pas : nom "
              "fiable, EXP, ATK/MATK, stats de base, modes, drops, lieux "
              "d'apparition et compétences. Le nom d'un monstre dans la table des "
              "sources d'une fiche d'objet l'ouvre d'un clic."));
          ImGui::Separator();
          if (mi->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Menu contextuel du clic droit sur une entité ────────────────────────
      if (iface_nav_ == kIfaceContextMenu) {
        if (auto* ecm = Bourgeon::Instance().entity_context_menu()) {
          ImGui::TextWrapped(
              i18n::Tr("Remplace le menu du clic droit sur une entité. Les actions ne "
              "sont pas réécrites : elles repassent par le dispatcher du client, "
              "donc ses vérifications et ses confirmations restent jouées. Le "
              "menu du client n'existait que sur un joueur, son pet, son "
              "homoncule et son mercenaire — ici il peut aussi s'ouvrir sur les "
              "monstres et les NPC."));
          ImGui::Separator();
          if (ecm->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Atlas des recettes (CraftAtlas) ────────────────────────────────────
      // 🔴 PAS dans `needs_modern` : l'Atlas n'AGIT sur rien — il n'envoie aucun
      // paquet et ne remplace aucune fenêtre native, il lit un fichier. Le gate
      // « interface moderne » porte sur ce qui agit, pas sur ce qui affiche.
      if (iface_nav_ == kIfaceCraftAtlas) {
        if (auto* atlas = Bourgeon::Instance().craft_atlas()) {
          if (atlas->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      ImGui::EndDisabled();  // apparié au BeginDisabled(locked) au-dessus
      PopItemWidth();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    PopStyleCompact();
  }
}
