#include "plugins/moonlight_ui/internal.h"

#include <algorithm>

#include "bourgeon.h"
#include "imgui.h"
#include "plugins/moonlight_ui.h"
#include "ui/align_grid.h"
#include "ui/color_codec.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"

// Types COMPLETS des plugins pilotés par les 11 sections (bourgeon.h n'en donne
// que des déclarations anticipées).
#include "plugins/basic_info.h"
#include "plugins/bug_report.h"
#include "plugins/cashshop_tweaks.h"
#include "plugins/character_sheet.h"
#include "plugins/chat.h"
#include "plugins/inventory_viewer.h"
#include "plugins/item_desc_tweaks.h"
#include "plugins/menu_icons.h"
#include "plugins/npc_dialog_tweaks.h"
#include "plugins/quest_tracker_tweaks.h"
#include "plugins/shop_tweaks.h"
#include "plugins/skill_bar_tweaks.h"
#include "plugins/status_icon_tweaks.h"
#include "plugins/storage_tweaks.h"

// ── En-tête « Interface de jeu » ─────────────────────────────────────────────
// Navigation latérale + les 11 sections de configuration. C'était le bloc dominant
// d'OnRenderUI (742 lignes sur 1702) : une nav, puis une cascade de 11 tests sur
// iface_nav_, chacun avec sa propre variable changed homonyme.
// La table kIfaceSections (source unique libellé + identifiant, cf. chantier 5)
// vit ici, au plus près de son usage.
void MoonlightUi::DrawInterfacePanel() {
  // Saut demandé (bullet de barre de titre d'une fenêtre Bourgeon) : on force
  // l'en-tête ouvert et on scrolle dessus, une seule fois.
  const bool iface_jump = iface_jump_;
  iface_jump_ = false;
  if (iface_jump) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  if (CollapsingHeader("Interface de jeu")) {
    if (iface_jump) ImGui::SetScrollHereY(0.0f);
    PushStyleCompact();
    bool changed = false;
    changed |= ro::RoCheckbox("Grille d'alignement", &grid_.show);
    SameLine(); HelpMarker(
        "Affiche une grille plein écran pour aligner ton interface "
        "(comme les add-ons d'interface de WoW).");
    ImGui::SetNextItemWidth(160.0f);
    changed |= WheelSliderInt("Taille grille", &grid_.size, 4, 128);
    changed |= ro::RoCheckbox("Aimanter à la grille", &grid_.snap);
    SameLine(); HelpMarker(
        "Les barres et les icônes s'alignent sur les cellules de la grille "
        "pendant le déplacement et le redimensionnement.");
    changed |= ColorPicker("Couleur grille", grid_.color);

    // (Inventaire et Storage : tout est regroupé dans leurs sections dédiées.)

    // (Storage : tout est regroupé dans la section « Storage » ci-dessous.)

    // Cash shop : redraw ImGui moderne OU fenêtre native
    if (auto* cs = Bourgeon::Instance().cashshop_tweaks()) {
      changed |= ro::RoCheckbox("Cash Shop Moonlight®", &cs->imgui_enabled_);
      SameLine(); HelpMarker(
          "ON : cash shop ImGui moderne (icônes, catégories, panier) et la "
          "fenêtre native est cachée.\nOFF : cash shop natif classique.");
    }

    // Shop NPC : fenêtre achat/vente ImGui unifiée OU natif
    if (auto* sh = Bourgeon::Instance().shop_tweaks()) {
      changed |= ro::RoCheckbox("Shop NPC Moonlight®", &sh->imgui_enabled_);
      SameLine(); HelpMarker(
          "ON : fenêtre boutique ImGui unifiée (onglets Acheter/Vendre, saut "
          "du choix Acheter/Vendre natif).\nOFF : boutique NPC native classique.");
    }

    // Feuille de personnage (agrege Status + Equipement)
    if (auto* cse = Bourgeon::Instance().character_sheet()) {
      changed |= ro::RoCheckbox("Feuille de perso Moonlight® (Alt+F)", &cse->imgui_enabled_);
      SameLine(); HelpMarker(
          "Fenêtre façon WoW : avatar + slots équipement/costume + stats, en "
          "COMPLÉMENT des fenêtres natives (conservées). Ouvre/ferme avec Alt+F.\n"
          "Clic gauche slot = description, clic droit = desequiper, boutons +stat.");
    }

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
    };
    static_assert(IM_ARRAYSIZE(kIfaceSections) == kIfaceCount,
                  "kIfaceSections doit couvrir exactement l'enum IfaceSection");

    // Dimensions dérivées du texte/style (pas de pixels fixes) : la liste garde
    // la largeur de sa plus longue entrée, bornée à 40 % de la place dispo pour
    // rester lisible sur fenêtre étroite.
    // La mesure des 11 libellés ne dépend que de la POLICE : on la garde en cache
    // au lieu de refaire 11 CalcTextSize à chaque frame, et on la réinvalide quand
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
          GrayText("(plugin indisponible)");
      }

      // ── Barres d'info (HUD bars + alignment grid) ────────────────────────
      // ── Status Portrait (head + pseudo + classe + niveau, indépendants) ──
      if (iface_nav_ == kIfaceBasicInfo) {
        bool changed = false;
        if (auto* eb = Bourgeon::Instance().basic_info()) {
          PushStyleCompact();

          changed |= ro::RoCheckbox("Masquer la fenêtre Basic Info d'origine", &eb->portrait_hide_basic_info_);
          SameLine(); HelpMarker("Masque la fenêtre native \"Basic Info\".");

          SeparatorText("Barres d'info");
          changed |= ro::RoCheckbox("Afficher les barres", &eb->visible_);
          ImGui::BeginDisabled(!eb->visible_);
          Indent();
            for (int i = 0; i < BasicInfoTweaks::kBarCount; ++i) {
              if (i) SameLine();
              changed |= ro::RoCheckbox(BasicInfoTweaks::kBarLabels[i], &eb->bars_[i].show);
            }
            SameLine(); HelpMarker("Affiche/cache chaque barre indépendamment.");
          Unindent();
          ImGui::EndDisabled();

          changed |= ro::RoCheckbox("Verrouiller les barres", &eb->locked_);
          SameLine(); HelpMarker(
              "Verrouillée : les barres ne bougent plus et laissent passer les clics au jeu.\n"
              "Déverrouillée : glissez-les pour les déplacer, tirez le coin pour redimensionner.");

          changed |= ro::RoCheckbox("Aimanter les barres (snap)", &eb->sticky_);
          SameLine(); HelpMarker(
              "Quand tu glisses une barre près d'une autre, ses bords s'alignent "
              "et se collent automatiquement (~10px).\nÉloigne-la pour la "
              "détacher. Les barres restent indépendantes.");

          changed |= ro::RoCheckbox("Vertical", &eb->vertical_);
          SameLine(); HelpMarker(
              "Remplissage vertical des barres. \n"
              "Décoche pour les barres horizontales.");

          changed |= ro::RoCheckbox("Bordure des barres", &eb->border_);
          SameLine(); HelpMarker(
              "Trait sombre 1px autour de chaque barre (HP/SP/EXP...). \n"
              "Décoche pour des barres sans contour.");

          const char* modes[] = {"Aucun", "Pourcentage", "Valeurs", "Les deux"};
          changed |= ro::RoCombo("Texte des barres", &eb->text_mode_, modes, IM_ARRAYSIZE(modes));
          SameLine(); HelpMarker(
              "Ce qui est écrit sur les barres : rien, le pourcentage, les "
              "valeurs brutes (courant / max) ou les deux.");
          changed |= WheelSliderFloat("Arrondi", &eb->rounding_, 0.0f, 16.0f);
          SameLine(); HelpMarker("Arrondi des coins des barres.");

          for (int i = 0; i < BasicInfoTweaks::kBarCount; ++i) {
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "Couleur %s", BasicInfoTweaks::kBarLabels[i]);
            changed |= ColorPicker(lbl, eb->bars_[i].fill);
          }
          changed |= ColorPicker("Fond / Opacité", eb->bg_color_);

          TextUnformatted("Tailles rapides de barres (toutes) :");
          auto preset = [&](const char* label, int w, int h) {
            SameLine();
            if (ro::RoButton(label)) {
              for (int j = 0; j < BasicInfoTweaks::kBarCount; ++j) {
                eb->bars_[j].w = w;
                eb->bars_[j].h = h;
              }
              eb->force_apply_ = true;  // re-apply size even while unlocked
              changed = true;
            }
          };
          preset("XS", 200, 9);
          preset("S", 400, 16);
          preset("M", 600, 22);
          preset("L", 800, 30);

          SeparatorText("Portrait personnage");
          changed |= ro::RoCheckbox("Afficher le portrait et les étiquettes", &eb->portrait_visible_);
          SameLine(); HelpMarker(
              "Portrait de statut : la tête du personnage, le pseudo, la classe "
              "et le niveau sont des éléments INDÉPENDANTS — chacun déplaçable, "
              "redimensionnable, avec sa couleur/opacité de fond et son arrondi.");

          ImGui::BeginDisabled(!eb->portrait_visible_);

          changed |= ro::RoCheckbox("Verrouiller le portrait", &eb->portrait_locked_);
          Tooltip("Si les éléments sont déverrouillés et en contact les uns avec les autres, ils sont déplaçables en maintenant Ctrl.");
          SameLine(); HelpMarker(
              "Verrouillé : les éléments ne bougent plus et laissent passer les clics au jeu.\n"
              "Déverrouillé : glisse pour déplacer, tire un bord/coin pour redimensionner (aimantage à la grille d'alignement).");

          changed |= ro::RoCheckbox("Tête seule (sans le corps)", &eb->portrait_head_only_);
          SameLine(); HelpMarker(
              "Ne garde ne génère que la tête (visage/cheveux/coiffes) et retire le corps.\n"
              "Décoche pour le personnage entier.");

          changed |= ro::RoCheckbox("Cape / garment", &eb->portrait_show_garment_);
          SameLine(); HelpMarker(
              "Affiche la cape/garment équipée (seulement en mode corps "
              "entier — décoche \"Tête seule\" pour la voir).");

          changed |= WheelSliderFloat("Zoom", &eb->portrait_head_zoom_, 0.10f, 2.0f);
          SameLine(); HelpMarker("Ajuster avec le zoom.");

          changed |= WheelSliderFloat("Décalage horiz.", &eb->portrait_head_offx_, -1.5f, 1.5f);
          SameLine(); HelpMarker(
              "Décale le portrait horizontalement (0 = centré).\n"
              "Sert à cadrer la tête/le corps ; le zoom reste centré.");

          changed |= WheelSliderFloat("Décalage vert.", &eb->portrait_head_offy_, -1.5f, 1.5f);
          SameLine(); HelpMarker(
              "Décale le portrait verticalement (0 = centré).\n"
              "Optionnel — le zoom reste centré ; laisse à 0 si tu n'en as pas besoin.");

          static const char* kLabelsAnim[] = { "Repos", "Marche", "Assis", "Ramasser", "Combat", "Attaque", "Touché", "Gelé", "Mort" };
          changed |= ro::RoCombo("Animation", &eb->portrait_anim_, kLabelsAnim, IM_ARRAYSIZE(kLabelsAnim));
          SameLine(); HelpMarker(
              "Pose animée du portrait (Combat = posture prête au combat).\n"
              "Le nombre d'images de l'animation s'ajuste automatiquement.");

          static const char* kLabelsDir[] = { "Face", "Profil-Gauche", "Gauche", "Arrière-Gauche", "Dos", "Arrière-Droite", "Droite", "Profil-Droite" };
          changed |= ro::RoCombo("Direction", &eb->portrait_dir_, kLabelsDir, IM_ARRAYSIZE(kLabelsDir));
          SameLine(); HelpMarker(
              "Oriente le portrait. 0 = face. Essaie les valeurs pour trouver "
              "l'angle voulu (le rendu se met à jour en direct).");

          changed |= ro::RoCheckbox("Animer", &eb->portrait_animate_);
          SameLine(); HelpMarker(
              "Joue les images de l'animation (ex. le balayage de la posture "
              "Combat). Décoche pour figer une pose calme (image 0).");

          SeparatorText("Couleurs et arrondis du portrait et des étiquettes");
          changed |= ro::RoCheckbox("Bordure", &eb->portrait_border_);
          SameLine(); HelpMarker("Trait 1px autour du cadre et des étiquettes.");

          // Per-element config: show / background colour+opacity / rounding /
          // text colour.  Each element is independent.
          for (int i = 0; i < BasicInfoTweaks::kPortCount; ++i) {
            auto& e = eb->ports_[i];
            ImGui::PushID(i);
            changed |= ro::RoCheckbox(BasicInfoTweaks::kPortLabels[i], &e.show);
            Indent();
            changed |= ColorPicker("Fond / Opacité", e.bg);
            if (i != BasicInfoTweaks::kPortHead) {
              SameLine();
              changed |= ColorPicker("Texte", e.fg);
            }
            changed |= WheelSliderFloat("Arrondi", &e.rounding, 0.0f, 16.0f, "%.0f", 1.0f);
            Unindent();
            ImGui::PopID();
          }
          PopStyleCompact();

          ImGui::EndDisabled(); // eb->portrait_visible_
        }
        if (changed) SaveSettings();
      }

      // ── Chat Settings ────────────────────────────────────────────────────
      if (iface_nav_ == kIfaceChat) {
        bool changed = false;
        // Ce panneau ne dépend d'AUCUN plugin : il ne pilote que chat:: et l'état
        // de MoonlightUi. Il était pourtant gaté sur BasicInfoTweaks — un
        // copier-coller du panneau Basic Info, où le pointeur sert quarante fois.
        // Sans ce plugin, le joueur voyait une page entièrement vide, sans même
        // le « (plugin indisponible) » affiché partout ailleurs.
        // Les accolades restent pour ne pas réindenter 130 lignes.
        {
          PushStyleCompact();

          SeparatorText("Réglages généraux");
          if (ro::RoCheckbox("Chat Discord (Gonryun only)", &discord_chat_)) {
            UpdateRelay();
            SendSetting(kSettingDiscordChat, discord_chat_ ? 1 : 0);
          }

          if (ro::RoCheckbox("Largeur du chat", &chat_width_enabled_)) {
            chat::SetCustomWidth(chat_width_enabled_, chat_width_px_);
            changed = true;
          }

          if (chat_width_enabled_) {
            // Le slider bouge à 60 Hz, mais chat::SetCustomWidth relance le relayout
            // natif ET RebuildFromHistory sur TOUS les onglets — le chemin exact du
            // freeze de word-wrap déjà corrigé côté mesure. On ne l'applique donc
            // qu'au RELÂCHEMENT du slider, pas à chaque frame de glissement (même
            // logique que le color picker du fond de chat, plus bas).
            // `moved && !IsItemActive()` = ajustement à la MOLETTE (WheelSliderInt la
            // traite hors du slider, sans jamais « désactiver » l'item) : on applique
            // tout de suite. `IsItemDeactivatedAfterEdit()` = fin de drag ou fin de
            // saisie Ctrl+clic. Pendant le drag l'item est actif : on ne fait rien.
            const bool moved = WheelSliderInt("Largeur (px)", &chat_width_px_, 320, 1200);
            if ((moved && !ImGui::IsItemActive()) || ImGui::IsItemDeactivatedAfterEdit()) {
              chat::SetCustomWidth(true, chat_width_px_);
              changed = true;
            }
          }

          if (ro::RoCheckbox("Horodatage du chat", &chat_timestamps_)) {
            chat::SetTimestamps(chat_timestamps_);
            changed = true;
          }

          if (ro::RoCheckbox("Icônes d'objets", &chat_item_icons_)) {
            chat::SetItemIcons(chat_item_icons_);
            changed = true;
          }

          // Clear chat history (all channels of the main chat window)
          if (ro::RoButton("Effacer l'historique du chat")) chat::ClearHistory();
          SameLine(); HelpMarker(
              "Vide l'historique de tous les canaux de la fenêtre de chat principale "
              "(historique brut effacé + affichage vidé). Les nouveaux messages "
              "réapparaissent normalement ensuite.");

          SeparatorText("Couleurs du chat");
          // Chat Background Colours (Main / Detached / Whisper)
          // One independent colour+opacity picker per group, persisted locally.
          auto render_chatbg = [&](ChatBgGroup& g) {
            if (g.instrs.empty()) return;
            ImGui::PushID(g.yaml_key);
            const ImVec4 swatch(g.color[0], g.color[1], g.color[2], g.color[3]);
            if (ImGui::ColorButton("##btn", swatch, ImGuiColorEditFlags_AlphaPreview, ImVec2(20, 20)))
            OpenPopup("picker");
            SameLine();
            TextUnformatted(g.label);

            if (BeginPopup("picker")) {
              // ── Shared user presets ─────────────────────────────────────────
              if (!chat_bg_presets_.empty()) {
                TextUnformatted("Presets:");
                int delete_idx = -1;
                // Display each preset as a colour swatch + name + delete button.
                for (int i = 0; i < static_cast<int>(chat_bg_presets_.size()); ++i) {
                  const auto& p = chat_bg_presets_[i];
                  const ImVec4 col = ro::ImVec4FromArgb(p.argb);
                  ImGui::PushID(i);
                  // Clicking a preset swatch updates the picker to match the preset, applies it to the chat background, and saves the settings.
                  if (ImGui::ColorButton("##swatch", col,
                                        ImGuiColorEditFlags_AlphaPreview |
                                        ImGuiColorEditFlags_NoTooltip,
                                        ImVec2(18, 18))) {
                    ro::PickerFromArgb(g.color, p.argb); // update the picker to match the preset
                    ApplyChatBg(g, p.argb, true);
                    changed = true;
                  }
                  SameLine();
                  TextUnformatted(p.name.c_str());
                  SameLine();
                  if (ro::RoSmallButton("x")) delete_idx = i;
                  ImGui::PopID();
                }
                if (delete_idx >= 0) {
                  chat_bg_presets_.erase(chat_bg_presets_.begin() + delete_idx);
                  changed = true;
                }
                SeparatorText("Sauvegarder une couleur comme preset");
              }
              // ── Save current colour as a preset ─────────────────────────────
              ImGui::SetNextItemWidth(120.0f);
              ImGui::InputTextWithHint("##preset_name", "Preset name", preset_name_buf_, sizeof(preset_name_buf_));
              SameLine();
              ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3.0f); // vertically align the button with the input text
              if (ro::RoButton("Save preset") && preset_name_buf_[0] != '\0') {
                chat_bg_presets_.push_back({preset_name_buf_, ro::ArgbFromPicker(g.color)});
                preset_name_buf_[0] = '\0';
                changed = true;
              }
              SeparatorText("Choisir une couleur");
              if (ImGui::ColorPicker4("##pick", g.color,
                                ImGuiColorEditFlags_AlphaBar |
                                ImGuiColorEditFlags_NoSidePreview)) {
                ApplyChatBg(g, ro::ArgbFromPicker(g.color), false);
                g.editing = true;
              }
              if (g.editing && ImGui::IsMouseReleased(0)) {
                ApplyChatBg(g, ro::ArgbFromPicker(g.color), true);
                changed = true;
                g.editing = false;
              }
              if (ro::RoButton("Close")) ImGui::CloseCurrentPopup();
              ImGui::EndPopup();
            }
            ImGui::PopID();
          };

          if (chat_bg_found_) {
            render_chatbg(chat_bg_[kChatBgMain]);
            // Quick preset switcher toggle, on the same line as the main chat picker.
            SameLine();
            changed |= ro::RoCheckbox("Preset bar", &mainchat_preset_bar_);
            render_chatbg(chat_bg_[kChatBgDetached]);
            render_chatbg(chat_bg_[kChatBgWhisper]);
          } else GrayText("(chat background patch unavailable)");

          PopStyleCompact();
        }
        if (changed) SaveSettings();
      }

      // ── Menu icons (ImGui replacement) ───────────────────────────────────
      if (iface_nav_ == kIfaceMenuIcons) {
        bool changed = false;
        if (auto* mi = Bourgeon::Instance().menu_icons()) {
          SeparatorText("Réglages généraux");
          changed |= ro::RoCheckbox("Rendre les icônes déplaçables", &mi->enabled_);
          SameLine(); HelpMarker("Cache la grille native et recrée les icônes fonctionnelles.");

          ImGui::BeginDisabled(!mi->enabled_);

          changed |= ro::RoCheckbox("Mode édition (glisser pour déplacer)", &mi->edit_mode_);
          SameLine(); HelpMarker(
              "En mode édition : glisse chaque icône pour la repositionner.\n"
              "Aimantage aux autres icônes et à la grille d'alignement.\n"
              "Désactive le mode pour cliquer les icônes normalement.");

          // Per-icon show/hide. icons() is populated once in-game.
          SeparatorText("Icônes");
          auto& icons = mi->icons();
          if (icons.empty()) {
            GrayText("(disponible une fois en jeu)");
          } else {
            for (auto& ic : icons) {
              bool shown = !ic.hidden;
              ImGui::PushID(ic.cmd_id);
              if (ro::RoCheckbox(ic.name, &shown)) {
                ic.hidden = !shown;
                mi->saved_[ic.name] = {ic.x, ic.y, ic.hidden, true};
                changed = true;
              }
              ImGui::PopID();
            }
          }

          ImGui::EndDisabled();
        }
        if (changed) SaveSettings();
      }

      // ── Status icons (StatusIconTweaks) ──────────────────────────────────
      if (iface_nav_ == kIfaceStatusIcons) {
        if (auto* si = Bourgeon::Instance().status_icons())
          si->DrawSettings();
        else
          GrayText("(plugin indisponible)");
      }

      // ── Suivi de quête (QuestTrackerTweaks) ──────────────────────────────
      if (iface_nav_ == kIfaceQuest) {
        if (auto* qt = Bourgeon::Instance().quest_tracker())
          qt->DrawSettings();
        else
          GrayText("(plugin indisponible)");
      }

      // ── Descriptions (ItemDescTweaks : panneaux techniques item/skill) ───
      if (iface_nav_ == kIfaceDesc) {
        bool changed = false;
        if (auto* idt = Bourgeon::Instance().item_desc()) {
          TextUnformatted("Descriptions modernes des items et skills.");

          changed |= ro::RoCheckbox("Panneau technique des items", &idt->show_item_panel());
          SameLine(); HelpMarker(
              "Affiche le panneau enrichi description d'un ITEM.\n"
              "Clic droit item");

          changed |= ro::RoCheckbox("Panneau technique des skills", &idt->show_skill_panel());
          SameLine(); HelpMarker(
              "Affiche le panneau enrichi à côté de la description d'un SKILL.\n"
              "Clic droit dans le grimoire");

          changed |= ro::RoCheckbox("Ouvrir près de la souris", &idt->desc_spawn_at_cursor());
          SameLine(); HelpMarker(
              "ON : la description apparaît près du curseur à chaque ouverture.\n"
              "OFF : elle réapparaît à sa dernière position connue.");

          if (idt->desc_spawn_at_cursor()) {
            Indent();
              const char* kAnchors[] = {"Haut-gauche", "Haut-droite", "Bas-gauche","Bas-droite", "Centre"};
              changed |= ro::RoCombo("Ancrage", &idt->desc_anchor(), kAnchors, 5);
              changed |= WheelSliderInt("Offset X", &idt->desc_offset_x(), -400, 400, "%d px");
              changed |= WheelSliderInt("Offset Y", &idt->desc_offset_y(), -400, 400, "%d px");
              SameLine(); HelpMarker("Décalage depuis le curseur (molette au survol pour ajuster).");
            Unindent();
          }
        } else {
          GrayText("(plugin indisponible)");
        }
        if (changed) SaveSettings();
      }

      // ── Skin RO (police + habillage des fenêtres ImGui) ──────────────────
      if (iface_nav_ == kIfaceSkin) {
        bool changed = false;
        bool font_on = ro::IsFontEnabled();
        if (ro::RoCheckbox("Police Malgun (UI)", &font_on)) {
          ro::SetFontEnabled(font_on);
          changed = true;
        }
        SameLine(); HelpMarker(
            "ON : police Malgun Gothic pour toute l'UI ImGui (latin + coreen).\n"
            "OFF : police integree d'ImGui (ProggyClean).");
        // (Le skin RO n'est plus optionnel : c'est l'habillage standard des
        // fenêtres ImGui Bourgeon. Seuls ses réglages restent configurables.)
        changed |= ro::ShowRoSkinSettings();

        // ── Presets : jeux de couleurs sauvegardés ────────────────────────
        SeparatorText("Presets");
        const int npreset = static_cast<int>(g_ro_presets.size());
        const bool valid_sel = g_ro_preset_sel >= 0 && g_ro_preset_sel < npreset;
        const char* preview = valid_sel ? g_ro_presets[g_ro_preset_sel].name.c_str()
                                        : "(choisir)";
        if (ro::RoBeginCombo("##ro_preset", preview)) {
          for (int i = 0; i < npreset; ++i)
            if (ImGui::Selectable(g_ro_presets[i].name.c_str(), g_ro_preset_sel == i))
              g_ro_preset_sel = i;
          ro::RoEndCombo();
        }
        SameLine();
        if (ro::RoButton("Appliquer") && valid_sel) {
          ro::SkinConfig() = g_ro_presets[g_ro_preset_sel].cfg;
          changed = true;
        }
        SameLine();
        if (ro::RoButton("Supprimer") && valid_sel) {
          g_ro_presets.erase(g_ro_presets.begin() + g_ro_preset_sel);
          g_ro_preset_sel = -1;
          changed = true;
        }
        static char preset_name[32] = "";
        ImGui::InputText("##ro_preset_name", preset_name, sizeof(preset_name));
        SameLine();
        if (ro::RoButton("Sauvegarder") && preset_name[0]) {
          bool found = false;
          for (auto& p : g_ro_presets)
            if (p.name == preset_name) { p.cfg = ro::SkinConfig(); found = true; break; }
          if (!found) g_ro_presets.push_back({preset_name, ro::SkinConfig()});
          changed = true;
          preset_name[0] = '\0';
        }
        SameLine(); HelpMarker(
          "Sauvegarde les couleurs/luminosite/opacite actuelles sous un nom.\n"
          "'Appliquer' recharge un preset ; les joueurs peuvent se faire plusieurs themes.");
        if (changed) SaveSettings();
      }

      // ── Fenêtre NPC (dialogue / menu / prompt ImGui) ─────────────────────
      if (iface_nav_ == kIfaceNpc) {
        if (auto* nd = Bourgeon::Instance().npc_dialog_tweaks()) {
          if (ro::RoCheckbox("Dialogue NPC ImGui", &nd->imgui_enabled_))
            SaveSettings();
          SameLine(); HelpMarker(
              "Remplace le dialogue / menu / prompt NPC natif par un overlay ImGui "
              "(texte en couleur, menu à navigation clavier : flèches + Entrée, "
              "touches 1-9). Opt-in ; la fenêtre native est cachée quand c'est actif.");
          ImGui::BeginDisabled(!nd->imgui_enabled_);
          if (ro::RoCheckbox("Barre de recherche du menu", &nd->menu_search_))
            SaveSettings();
          SameLine(); HelpMarker(
              "Affiche un champ de recherche au-dessus des longs menus (plus de 8 "
              "choix) pour filtrer les options. Décoche pour un menu épuré.");
          ImGui::EndDisabled();
        }
      }

      // ── Storage (StorageTweaks : viewer ImGui + colonnes/filtre/survol) ───
      if (iface_nav_ == kIfaceStorage) {
        if (auto* stg = Bourgeon::Instance().storage_tweaks()) {
          bool changed = false;
          // Interrupteur GLOBAL synchronisé : bascule aussi l'inventaire et les
          // barres d'action (tout-ImGui ou tout-natif, plus de mixe).
          if (ro::RoCheckbox("Interface moderne (inventaire + storage + barres + échange)",
                             &stg->imgui_enabled_)) {
            SetModernInterface(stg->imgui_enabled_);
            changed = true;
          }
          SameLine(); HelpMarker(
              "Interrupteur GLOBAL : inventaire, storage, barres d'action et "
              "échange modernes s'activent ENSEMBLE — pas de mixe (tout ImGui ou tout "
              "natif). Les cases des sections Inventaire et Barre d'action "
              "reflètent le même état.\n\n"
              "ON : storage ImGui moderne (icônes, onglets, tri, drag-drop) "
              "et la fenêtre native est cachée.\nOFF : storage natif classique, aucun "
              "viewer.");

          ImGui::BeginDisabled(!stg->imgui_enabled_);

          changed |= ro::RoCheckbox("Description au survol", &stg->desc_tooltip());
          SameLine(); HelpMarker(
              "Survoler un item affiche un aperçu SIMPLIFIÉ (nom, illustration, "
              "texte) dans un panneau au skin RO, posé au curseur et effacé dès "
              "que la souris quitte la ligne.\n"
              "La description COMPLÈTE reste accessible au Ctrl + clic droit / "
              "menu contextuel.");

          changed |= ro::RoCheckbox("Onglets verticaux (à gauche)", &stg->tabs_vertical());
          SameLine(); HelpMarker(
              "Dispose les catégories en liste verticale à gauche, comme la "
              "fenêtre native.\nOFF (défaut) : onglets horizontaux en haut.");

          changed |= ro::RoCheckbox("Images d'onglet", &stg->tab_images());
          SameLine(); HelpMarker(
              "ON : tuiles images du client — jeu tab_* en disposition verticale, "
              "tabh_* en horizontale (all/use/wea/ammo/card/fav/cash/cos/etc). Les "
              "catégories sans art propre réutilisent celui de leur famille et "
              "portent alors un sigle (Am, Cs, Et).\n"
              "OFF : onglets texte — TabBar classique en horizontal, libellé écrit "
              "à la verticale en vertical.");

          changed |= ro::RoCheckbox("Champ de filtre", &stg->show_filter());
          SameLine(); HelpMarker(
              "Affiche la barre de recherche par nom au-dessus de la liste.\n"
              "Décoche pour gagner une ligne (le filtre est alors vidé).");

          changed |= ro::RoCheckbox("Valeur estimée du storage", &stg->show_total_value());
          SameLine(); HelpMarker(
              "Somme des prix de revente NPC (× quantité) des items AFFICHÉS "
              "— elle suit donc l'onglet, le sous-type et le filtre.");

          SeparatorText("Colonnes");
          changed |= ro::RoCheckbox("Index", &stg->show_index_col());
          SameLine(); HelpMarker(
              "Index storage (slot) — un item récemment ajouté a un index élevé.");
          changed |= ro::RoCheckbox("ID d'item", &stg->show_id_col());
          SameLine(); HelpMarker("Colonne avec l'id numérique de l'item.");
          changed |= ro::RoCheckbox("Slots", &stg->show_slots_col());
          SameLine(); HelpMarker("Colonne avec le nombre de slots de carte.");
          changed |= ro::RoCheckbox("Prix de revente", &stg->show_value_col());
          SameLine(); HelpMarker(
              "Colonne avec le prix de revente NPC × la quantité du stack.");

          ImGui::EndDisabled();
          if (changed) SaveSettings();
        } else {
          GrayText("(plugin indisponible)");
        }
      }

      // ── Inventaire (InventoryViewer : viewer ImGui + filtre/onglets) ──────
      if (iface_nav_ == kIfaceInventory) {
        if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
          bool changed = false;
          // Interrupteur GLOBAL synchronisé : bascule aussi le storage et les
          // barres d'action (tout-ImGui ou tout-natif, plus de mixe).
          if (ro::RoCheckbox("Interface moderne (inventaire + storage + barres + échange)",
                             &iv->imgui_enabled_)) {
            SetModernInterface(iv->imgui_enabled_);
            changed = true;
          }
          SameLine(); HelpMarker(
              "Interrupteur GLOBAL : inventaire, storage, barres d'action et "
              "échange modernes s'activent ENSEMBLE — pas de mixe (tout ImGui ou tout "
              "natif). Les cases des sections Storage et Barre d'action reflètent "
              "le même état.\n\n"
              "ON : inventaire ImGui moderne (grille d'icônes, onglets, recherche, "
              "double-clic utiliser/équiper, clic-droit, drag) et la fenêtre native "
              "est cachée.\nOFF (défaut) : inventaire natif classique, aucun viewer.\n\n"
              "Inclut la fenêtre de SERTISSAGE de cartes (double-clic sur une carte) : "
              "elle remplace le popup natif « Insert Card ». La liste des équipements "
              "compatibles reste calculée par le serveur, donc identique au natif.");

          ImGui::BeginDisabled(!iv->imgui_enabled_);

          changed |= ro::RoCheckbox("Description au survol", &iv->desc_tooltip());
          SameLine(); HelpMarker(
              "Survoler un item affiche un aperçu SIMPLIFIÉ (nom, illustration, "
              "texte, cartes et options) dans un panneau au skin RO, à la place du "
              "petit tooltip nom + quantité.\n"
              "La description COMPLÈTE reste accessible au Ctrl + clic droit / "
              "menu contextuel.");

          changed |= ro::RoCheckbox("Champ de filtre", &iv->show_filter());
          SameLine(); HelpMarker(
              "Affiche la barre de recherche par nom au-dessus de la grille.\n"
              "Décoche pour gagner une ligne (le filtre est alors vidé).");

          changed |= ro::RoCheckbox("Onglets verticaux (à gauche)", &iv->tabs_vertical());
          SameLine(); HelpMarker(
              "ON (défaut) : onglets en colonne à gauche de la grille, comme la "
              "fenêtre native (images tab_*).\n"
              "OFF : rangée horizontale au-dessus de la grille (images tabh_*).");

          changed |= ro::RoCheckbox("Verrouiller la taille", &iv->lock_size());
          SameLine(); HelpMarker(
              "La fenêtre ne peut plus être redimensionnée (elle reste déplaçable).");

          // Le placement libre EXIGE la taille verrouillée : une case est un index
          // absolu (ligne × colonnes), donc changer la largeur change le nombre de
          // colonnes et mélangerait toutes les positions mémorisées.
          ImGui::BeginDisabled(!iv->lock_size());
          Indent();
            changed |= ro::RoCheckbox("Placement libre des items", &iv->free_layout());
            SameLine(); HelpMarker(
                "Glisse un item sur une case vide pour l'y fixer ; sur une case "
                "occupée, les deux s'échangent. Les items sans case attribuée "
                "remplissent les trous restants, donc un nouvel objet ramassé ne "
                "bouscule plus ta disposition.\n\n"
                "L'onglet « Tout » n'est PAS concerné : il mélange les catégories, "
                "donc une case n'y désigne pas le même emplacement que dans "
                "l'onglet d'origine de l'item. Il garde le remplissage automatique.\n\n"
                "Nécessite « Verrouiller la taille » : les cases sont repérées par "
                "un index absolu, qu'un changement de largeur décalerait.");
          Unindent();
          ImGui::EndDisabled();

          ImGui::EndDisabled();
          if (changed) SaveSettings();
        } else {
          GrayText("(plugin indisponible)");
        }
      }
      PopItemWidth();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    PopStyleCompact();
  }
}
