#include "plugins/moonlight_ui/internal.h"

#include "bourgeon.h"
#include "imgui.h"
#include "plugins/moonlight_ui.h"
#include "plugins/staff_gate.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"

// bourgeon.h ne donne que des déclarations anticipées des plugins : il faut le
// type COMPLET de chacun de ceux qu'on pilote ici.
#include "plugins/doom_tweaks.h"
#include "plugins/dps_meter.h"
#include "plugins/keyboard_move.h"
#include "plugins/login_parade.h"
#include "plugins/player_jump.h"
#include "plugins/roggle_tweaks.h"
#include "plugins/rojeweled_tweaks.h"

// « DPS Meter » et « Mini-jeux » : deux en-têtes de premier niveau qui pilotent
// des plugins frères (DpsMeter, Doom, LoginParade, Roggle, RoJeweled, PlayerJump,
// KeyboardMove). Extraits d'OnRenderUI — 180 lignes.
void MoonlightUi::DrawFunPanels() {
  if (CollapsingHeader("DPS Meter")) {
    PushStyleCompact();
    if (auto* dps = Bourgeon::Instance().dps_meter()) {
      bool changed = false;
      changed |= ro::RoCheckbox("Afficher", &dps->visible_);
      changed |= ro::RoCheckbox("Verrouiller (fige + clic-traversant)", &dps->locked_);
      SameLine(); HelpMarker("Fige la fenêtre DPS (position/taille) et laisse passer les clics au jeu en dessous.");
      changed |= ColorPicker("Couleur texte",  dps->text_color_);
      changed |= ColorPicker("Couleur graphe", dps->plot_color_);

      PushItemWidth(160.0f); // sliders are narrow to fit the window
      changed |= WheelSliderFloat("Opacité fond", &dps->bg_alpha_, 0.0f, 1.0f);

      int slot_ms = dps->slot_ms_;
      if (WheelSliderInt("Résolution (ms/slot)", &slot_ms, 50, 2000)) {
        dps->slot_ms_ = slot_ms;
        dps->ResetHistory();
        changed = true;
      }
      SameLine(); HelpMarker("Largeur de chaque colonne du graphique en millisecondes.\nValeur plus basse = graphique plus précis mais moins smooth.");

      int win = dps->dps_window_secs_;
      if (WheelSliderInt("Fenêtre DPS (s)", &win, 1, 30)) {
        dps->dps_window_secs_ = win;
        changed = true;
      }
      SameLine(); HelpMarker("Fenêtre de temps pour calculer le DPS courant affiché.");

      int timeout = dps->combat_timeout_secs_;
      if (WheelSliderInt("Timeout combat (s)", &timeout, 1, 15)) {
        dps->combat_timeout_secs_ = timeout;
        changed = true;
      }
      SameLine(); HelpMarker("Secondes sans dégâts avant de quitter le mode combat.");

      PopItemWidth(); // restore default item width

      if (ro::RoButton("Reset graphique")) dps->ResetHistory();

      Separator();
      changed |= ro::RoCheckbox("Afficher dommages de sorts de zone dans le chat", &dps->show_ground_dmg_in_chat_);
      SameLine(); HelpMarker(
          "Affiche chaque coup de Storm Gust / Meteor Storm / LoV etc. dans le chat.\n"
          "Message custom Bourgeon — le serveur ne montre pas ces dégâts dans le chat habituel.");

          // Persist all DPS settings if any changed.
      if( changed ) SaveSettings();
    }
    PopStyleCompact();
  }

  // ── Interface de jeu  ────────────────────────────────────────────────────
  // NB: la vue caméra FPS (FpsViewTweaks) reste dans le code (toggle F9) mais
  // n'est plus exposée dans ce menu (expérimental, retiré à la demande).
  if (CollapsingHeader("Mini-jeux")) {
    SeparatorText("DOOM");
    if (auto* doom = Bourgeon::Instance().doom()) {
      bool on = doom->enabled();
      if (ro::RoCheckbox("Lancer DOOM (1993) dans Ragnarok", &on))
        doom->SetEnabled(on);
      SameLine(); HelpMarker(
          "Le vrai DOOM (moteur doomgeneric embarqué), rendu dans une fenêtre "
          "par-dessus le jeu.\n\n"
          "Nécessite doom1.wad (shareware) à côté de l'exe du client.\n"
          "Clique la fenêtre DOOM pour capturer le clavier : ZQSD (AZERTY), "
          "WASD ou flèches pour bouger, Ctrl tirer, Espace/E ouvrir, Shift "
          "courir, Échap menu.\n"
          "Décocher = pause. Quitter depuis le menu DOOM = définitif "
          "jusqu'au redémarrage du client.");
      GrayText("État : %s", doom->StatusText());
    } else
      GrayText(kPluginUnavailable);

    SeparatorText("Parade de Porings (login)");
    if (auto* lp = Bourgeon::Instance().login_parade()) {
      bool on = lp->enabled_;
      if (ro::RoCheckbox("Porings sur l'écran de login", &on)) {
        lp->enabled_ = on;
        SaveSettings();
      }
      SameLine(); HelpMarker(
          "Fait flâner une petite bande de monstres de la famille Poring sur "
          "l'écran de login : ils sautillent d'un bord à l'autre, font des "
          "pauses, et sursautent (avec un son) si tu cliques dessus.\n\n"
          "Purement cosmétique. Ils s'estompent au-dessus du panneau de login "
          "pour ne pas gêner la saisie. Visible uniquement à l'écran de login.");
    } else
      GrayText(kPluginUnavailable);

    SeparatorText("Roggle");
    if (auto* roggle = Bourgeon::Instance().roggle()) {
      bool on = roggle->enabled();
      if (ro::RoCheckbox("Ouvrir Roggle", &on))
        roggle->SetEnabled(on);
      SameLine(); HelpMarker(
          "Mini-jeu façon Peggle, dessiné en ImGui par-dessus le jeu.\n\n"
          "Vise à la souris depuis le canon en haut, clique pour tirer la "
          "bille. Dégomme tous les pegs ORANGE pour gagner ; le seau vert en "
          "bas rattrape la bille = bille gratuite.\n"
          "Fermer la fenêtre ou décocher = masquer (la partie est conservée).");
    } else
      GrayText(kPluginUnavailable);

    SeparatorText("Rojeweled");
    if (auto* rj = Bourgeon::Instance().rojeweled()) {
      bool on = rj->enabled();
      if (ro::RoCheckbox("Ouvrir Rojeweled", &on))
        rj->SetEnabled(on);
      SameLine(); HelpMarker(
          "Match-3 façon Bejeweled dont les gemmes sont de vrais sprites de "
          "monstres RO (famille Poring : Poring, Drops, Metaling, Poporing, "
          "Marin, Deviling).\n\n"
          "Clique deux monstres voisins pour les échanger ; aligne-en 3+ pour "
          "les faire disparaître (les cascades rapportent plus). DX9 requis "
          "(sinon tuiles colorées).");
    } else
      GrayText(kPluginUnavailable);

    SeparatorText("Saut (barre espace)");
    if (auto* pj = Bourgeon::Instance().player_jump()) {
      bool on = pj->enabled();
      if (ro::RoCheckbox("Sauter avec Espace", &on))
        pj->SetEnabled(on);
      SameLine(); HelpMarker(
          "Appuie sur Espace pour faire bondir ton personnage : un petit arc "
          "parabolique (montée puis retombée) purement visuel.\n\n"
          "Le serveur ne voit rien — c'est un simple décalage de hauteur du "
          "sprite, ré-appliqué chaque frame (tu peux même sauter en marchant). "
          "Taper une espace dans le chat ne déclenche PAS de saut.");
      // Réglages fins de l'arc de saut : réservés au staff (cf. IsStaff, group
      // level serveur >= 80). Mal réglés ils donnent un saut grotesque ou
      // invisible — les valeurs par défaut restent actives pour tout le monde.
      // Live, non persistés (comme FpsView).
      if (on && IsStaff()) {
        PushItemWidth(160.0f);
        WheelSliderFloat("Hauteur", pj->p_height(), 2.0f, 40.0f);
        WheelSliderInt("Durée (ms)", pj->p_duration_ms(), 200, 1500);
        PopItemWidth();
      }
    } else
      GrayText(kPluginUnavailable);

    SeparatorText("Déplacement au clavier");
    if (auto* km = Bourgeon::Instance().keyboard_move()) {
      bool on = km->enabled();
      if (ro::RoCheckbox("Marcher avec ZQSD / flèches", &on))
        km->SetEnabled(on);
      SameLine(); HelpMarker(
          "Déplace ton personnage au clavier : Z/S pour avancer et reculer, "
          "Q/D pour aller à gauche et à droite (les flèches font pareil). "
          "Deux touches ensemble = diagonale.\n\n"
          "Rien n'est simulé côté client : le plugin envoie la MÊME demande de "
          "marche que le clic au sol, donc le serveur reste maître du "
          "déplacement (murs, vitesse, blocages). Taper dans le chat ne fait "
          "pas courir le personnage.\n\n"
          "Attention si tu as des raccourcis de compétence sur Z, Q, S ou D : "
          "ils se déclencheront aussi.");
      if (on) {
        ro::RoCheckbox("Suivre la rotation de la caméra", km->p_camera_relative());
        SameLine(); HelpMarker(
            "« Haut » = le haut de l'écran, même après avoir fait pivoter la "
            "caméra. Décoché : les directions restent celles de la carte.");
        ro::RoCheckbox("S'arrêter au relâchement", km->p_stop_on_release());
        SameLine(); HelpMarker(
            "Coupe la marche dès que tu lâches la touche, au lieu de laisser "
            "le personnage finir le trajet demandé.");
        // Réglages fins du protocole de marche : réservés au staff (cf.
        // IsStaff, group level serveur >= 80). Mal réglés ils dégradent le
        // ressenti ou spamment le serveur — les valeurs par défaut restent
        // actives pour tout le monde. Live, non persistés (comme FpsView).
        if (IsStaff()) {
          PushItemWidth(160.0f);
          WheelSliderInt("Anticipation (cases)", km->p_look_ahead(), 1, 6);
          WheelSliderInt("Cadence (ms)", km->p_refresh_ms(), 60, 400);
          PopItemWidth();
        }
      }
    } else
      GrayText(kPluginUnavailable);
  }
}
