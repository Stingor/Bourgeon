#include "features/moonlight_ui/internal.h"

#include <string>

#include "bourgeon.h"
#include "imgui.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/staff_gate.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"

// bourgeon.h ne donne que des déclarations anticipées des plugins : il faut le
// type COMPLET de chacun de ceux qu'on pilote ici.
#include "features/minigames/doom.h"
#include "features/overlays/dps_meter.h"
#include "features/hotkey_util.h"
#include "features/gameplay/keyboard_move.h"
#include "features/overlays/login_parade.h"
#include "features/gameplay/player_jump.h"
#include "features/minigames/roggle.h"
#include "features/minigames/rojeweled.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// Ligne « Touche : Espace [Redéfinir] » du saut, avec capture du nouveau combo.
// Le combo passe par le contrôle de conflit PARTAGÉ (hotkeys::Conflict) : il est
// refusé, en nommant son propriétaire, s'il appartient déjà à un preset
// d'équipement, à un raccourci natif de la barre de skills ou à Alt+F — même
// contrôle que les raccourcis de preset de la fiche de personnage.
// Renvoie true quand la touche a changé (l'appelant persiste).
bool DrawJumpKeyBinding(PlayerJump* player_jump) {
  bool changed = false;
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled(i18n::Tr("Touche :"));
  SameLine();

  if (player_jump->key_capturing()) {
    // Gèle les raccourcis (saut ET presets) le temps du choix : la touche
    // pressée doit remapper, pas déclencher l'action qu'elle porte encore.
    hotkeys::PingCapture();
    Text(i18n::Tr("appuie sur une touche…  (Échap : annuler)"));
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      player_jump->key_capturing() = false;
      player_jump->key_conflict_msg().clear();
    } else if (const int vkey = hotkeys::CaptureMainVk()) {
      const bool ctrl = io.KeyCtrl, alt = io.KeyAlt, shift = io.KeyShift;
      char what[64];
      if (hotkeys::Conflict(vkey, ctrl, alt, shift, hotkeys::Owner::kJump, -1, what,
                            sizeof(what))) {
        player_jump->key_conflict_msg() =
            std::string(i18n::Tr("Déjà utilisé par ")) + what + i18n::Tr(" — choisis une autre touche");
      } else {  // libre : on assigne, l'appelant persiste
        player_jump->key_vk()    = vkey;
        player_jump->key_ctrl()  = ctrl;
        player_jump->key_alt()   = alt;
        player_jump->key_shift() = shift;
        player_jump->key_capturing() = false;
        player_jump->key_conflict_msg().clear();
        changed = true;
      }
    }
    if (!player_jump->key_conflict_msg().empty())
      ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", player_jump->key_conflict_msg().c_str());
    return changed;
  }

  char label[48];
  hotkeys::Label(player_jump->key_vk(), player_jump->key_ctrl(), player_jump->key_alt(),
                 player_jump->key_shift(), label, sizeof(label));
  Text("%s", label);
  SameLine(0.0f, 6.0f);
  if (ro::RoButton(i18n::Tr("Redéfinir"))) {
    player_jump->key_capturing() = true;
    player_jump->key_conflict_msg().clear();
  }
  Tooltip(i18n::Tr("Lettres, chiffres, F1-F12 et Espace (avec Ctrl/Alt/Maj si tu veux).\n"
          "Une touche déjà prise par un preset d'équipement ou par la barre de "
          "skills est refusée."));
  return changed;
}

}  // namespace

// « DPS Meter » et « Mini-jeux » : deux en-têtes de premier niveau qui pilotent
// des plugins frères (DpsMeter, Doom, LoginParade, Roggle, RoJeweled, PlayerJump,
// KeyboardMove). Extraits d'OnRenderUI — 180 lignes.
void MoonlightUi::DrawFunPanels() {
  if (iface::LinkableHeader("dps")) {
    PushStyleCompact();
    if (auto* dps_meter = Bourgeon::Instance().dps_meter()) {
      if (dps_meter->DrawSettings()) SaveSettings();
    } else {
      ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
    }
    PopStyleCompact();
  }

  // ── Interface de jeu  ────────────────────────────────────────────────────
  // NB: la vue caméra FPS (FpsView) reste dans le code (toggle F9) mais
  // n'est plus exposée dans ce menu (expérimental, retiré à la demande).
  if (iface::LinkableHeader("minigames")) {
    SeparatorText(i18n::Tr("DOOM"));
    if (auto* doom = Bourgeon::Instance().doom()) {
      bool on = doom->enabled();
      if (ro::RoCheckbox(i18n::Tr("Lancer DOOM (1993) dans Ragnarok"), &on))
        doom->SetEnabled(on);
      SameLine(); HelpMarker(
          i18n::Tr("Le vrai DOOM (moteur doomgeneric embarqué), rendu dans une fenêtre "
          "par-dessus le jeu.\n\n"
          "Nécessite doom1.wad (shareware) à côté de l'exe du client.\n"
          "Clique la fenêtre DOOM pour capturer le clavier : ZQSD (AZERTY), "
          "WASD ou flèches pour bouger, Ctrl tirer, Espace/E ouvrir, Shift "
          "courir, Échap menu.\n"
          "Décocher = pause. Quitter depuis le menu DOOM = définitif "
          "jusqu'au redémarrage du client."));
      ImGui::TextDisabled(i18n::Tr("État : %s"), doom->StatusText());
    } else
      ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));

    SeparatorText(i18n::Tr("Parade de Porings (login)"));
    if (auto* login_parade = Bourgeon::Instance().login_parade()) {
      bool on = login_parade->enabled_;
      if (ro::RoCheckbox(i18n::Tr("Porings sur l'écran de login"), &on)) {
        login_parade->enabled_ = on;
        SaveSettings();
      }
      SameLine(); HelpMarker(
          i18n::Tr("Fait flâner une petite bande de monstres de la famille Poring sur "
          "l'écran de login : ils sautillent d'un bord à l'autre, font des "
          "pauses, et sursautent (avec un son) si tu cliques dessus.\n\n"
          "Purement cosmétique. Ils s'estompent au-dessus du panneau de login "
          "pour ne pas gêner la saisie. Visible uniquement à l'écran de login."));
    } else
      ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));

    SeparatorText(i18n::Tr("Roggle"));
    if (auto* roggle = Bourgeon::Instance().roggle()) {
      bool on = roggle->enabled();
      if (ro::RoCheckbox(i18n::Tr("Ouvrir Roggle"), &on))
        roggle->SetEnabled(on);
      SameLine(); HelpMarker(
          i18n::Tr("Mini-jeu façon Peggle, dessiné en ImGui par-dessus le jeu.\n\n"
          "Vise à la souris depuis le canon en haut, clique pour tirer la "
          "bille. Dégomme tous les pegs ORANGE pour gagner ; le seau vert en "
          "bas rattrape la bille = bille gratuite.\n"
          "Fermer la fenêtre ou décocher = masquer (la partie est conservée)."));
    } else
      ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));

    SeparatorText(i18n::Tr("Rojeweled"));
    if (auto* rojeweled = Bourgeon::Instance().rojeweled()) {
      bool on = rojeweled->enabled();
      if (ro::RoCheckbox(i18n::Tr("Ouvrir Rojeweled"), &on))
        rojeweled->SetEnabled(on);
      SameLine(); HelpMarker(
          i18n::Tr("Match-3 façon Bejeweled dont les gemmes sont de vrais sprites de "
          "monstres RO (famille Poring : Poring, Drops, Metaling, Poporing, "
          "Marin, Deviling).\n\n"
          "Clique deux monstres voisins pour les échanger ; aligne-en 3+ pour "
          "les faire disparaître (les cascades rapportent plus). DX9 requis "
          "(sinon tuiles colorées)."));
    } else
      ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));

    SeparatorText(i18n::Tr("Saut"));
    if (auto* player_jump = Bourgeon::Instance().player_jump()) {
      bool on = player_jump->enabled();
      if (ro::RoCheckbox(i18n::Tr("Sauter au clavier"), &on)) {
        player_jump->SetEnabled(on);
        SaveSettings();
      }
      SameLine(); HelpMarker(
          i18n::Tr("Appuie sur la touche de saut (Espace par défaut) pour faire bondir "
          "ton personnage : un petit arc parabolique (montée puis retombée) "
          "purement visuel.\n\n"
          "Le serveur ne voit rien — c'est un simple décalage de hauteur du "
          "sprite, ré-appliqué chaque frame (tu peux même sauter en marchant). "
          "Taper cette touche dans le chat ne déclenche PAS de saut."));
      if (on && DrawJumpKeyBinding(player_jump)) SaveSettings();
      // Réglages fins de l'arc de saut : réservés au staff (cf. IsStaff, group
      // level serveur >= 80). Mal réglés ils donnent un saut grotesque ou
      // invisible — les valeurs par défaut restent actives pour tout le monde.
      // Live, non persistés (comme FpsView).
      if (on && IsStaff()) {
        PushItemWidth(160.0f);
        WheelSliderFloat(i18n::Tr("Hauteur"), player_jump->p_height(), 2.0f, 40.0f);
        WheelSliderInt(i18n::Tr("Durée (ms)"), player_jump->p_duration_ms(), 200, 1500);
        PopItemWidth();
      }
    } else
      ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));

    SeparatorText(i18n::Tr("Déplacement au clavier (Expérimental)"));
    if (auto* keyboard_move = Bourgeon::Instance().keyboard_move()) {
      bool on = keyboard_move->enabled();
      if (ro::RoCheckbox(i18n::Tr("Marcher avec ZQSD / flèches"), &on)) {
        keyboard_move->SetEnabled(on);
        SaveSettings();
      }
      SameLine(); HelpMarker(
          i18n::Tr("Déplace ton personnage au clavier : Z/S pour avancer et reculer, "
          "Q/D pour aller à gauche et à droite (les flèches font pareil). "
          "Deux touches ensemble = diagonale.\n\n"
          "Rien n'est simulé côté client : le plugin envoie la MÊME demande de "
          "marche que le clic au sol, donc le serveur reste maître du "
          "déplacement (murs, vitesse, blocages). Taper dans le chat ne fait "
          "pas courir le personnage.\n\n"
          "Attention si tu as des raccourcis de compétence sur Z, Q, S ou D : "
          "ils se déclencheront aussi."));
      if (on) {
        if (ro::RoCheckbox(i18n::Tr("Suivre la rotation de la caméra"),
                           keyboard_move->p_camera_relative()))
          SaveSettings();
        SameLine(); HelpMarker(
            i18n::Tr("« Haut » = le haut de l'écran, même après avoir fait pivoter la "
            "caméra. Décoché : les directions restent celles de la carte."));
        if (ro::RoCheckbox(i18n::Tr("S'arrêter au relâchement"),
                           keyboard_move->p_stop_on_release()))
          SaveSettings();
        SameLine(); HelpMarker(
            i18n::Tr("Coupe la marche dès que tu lâches la touche, au lieu de laisser "
            "le personnage finir le trajet demandé."));
        // Réglages fins du protocole de marche : réservés au staff (cf.
        // IsStaff, group level serveur >= 80). Mal réglés ils dégradent le
        // ressenti ou spamment le serveur — les valeurs par défaut restent
        // actives pour tout le monde. Live, non persistés (comme FpsView).
        if (IsStaff()) {
          PushItemWidth(160.0f);
          WheelSliderInt(i18n::Tr("Anticipation (cases)"), keyboard_move->p_look_ahead(), 1, 6);
          WheelSliderInt(i18n::Tr("Cadence (ms)"), keyboard_move->p_refresh_ms(), 60, 400);
          PopItemWidth();
        }
      }
    } else
      ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
  }
}
