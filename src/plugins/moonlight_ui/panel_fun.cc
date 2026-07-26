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
    if (auto* dps_meter = Bourgeon::Instance().dps_meter()) {
      if (dps_meter->DrawSettings()) SaveSettings();
    } else {
      GrayText(kPluginUnavailable);
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
    if (auto* login_parade = Bourgeon::Instance().login_parade()) {
      bool on = login_parade->enabled_;
      if (ro::RoCheckbox("Porings sur l'écran de login", &on)) {
        login_parade->enabled_ = on;
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
    if (auto* rojeweled = Bourgeon::Instance().rojeweled()) {
      bool on = rojeweled->enabled();
      if (ro::RoCheckbox("Ouvrir Rojeweled", &on))
        rojeweled->SetEnabled(on);
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
    if (auto* player_jump = Bourgeon::Instance().player_jump()) {
      bool on = player_jump->enabled();
      if (ro::RoCheckbox("Sauter avec Espace", &on))
        player_jump->SetEnabled(on);
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
        WheelSliderFloat("Hauteur", player_jump->p_height(), 2.0f, 40.0f);
        WheelSliderInt("Durée (ms)", player_jump->p_duration_ms(), 200, 1500);
        PopItemWidth();
      }
    } else
      GrayText(kPluginUnavailable);

    SeparatorText("Déplacement au clavier");
    if (auto* keyboard_move = Bourgeon::Instance().keyboard_move()) {
      bool on = keyboard_move->enabled();
      if (ro::RoCheckbox("Marcher avec ZQSD / flèches", &on))
        keyboard_move->SetEnabled(on);
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
        ro::RoCheckbox("Suivre la rotation de la caméra", keyboard_move->p_camera_relative());
        SameLine(); HelpMarker(
            "« Haut » = le haut de l'écran, même après avoir fait pivoter la "
            "caméra. Décoché : les directions restent celles de la carte.");
        ro::RoCheckbox("S'arrêter au relâchement", keyboard_move->p_stop_on_release());
        SameLine(); HelpMarker(
            "Coupe la marche dès que tu lâches la touche, au lieu de laisser "
            "le personnage finir le trajet demandé.");
        // Réglages fins du protocole de marche : réservés au staff (cf.
        // IsStaff, group level serveur >= 80). Mal réglés ils dégradent le
        // ressenti ou spamment le serveur — les valeurs par défaut restent
        // actives pour tout le monde. Live, non persistés (comme FpsView).
        if (IsStaff()) {
          PushItemWidth(160.0f);
          WheelSliderInt("Anticipation (cases)", keyboard_move->p_look_ahead(), 1, 6);
          WheelSliderInt("Cadence (ms)", keyboard_move->p_refresh_ms(), 60, 400);
          PopItemWidth();
        }
      }
    } else
      GrayText(kPluginUnavailable);
  }
}
