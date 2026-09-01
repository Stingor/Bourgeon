#include "features/moonlight_ui/internal.h"

#include "bourgeon.h"
#include "imgui.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"

// bourgeon.h ne donne que des déclarations anticipées des plugins : il faut le
// type COMPLET de chacun de ceux qu'on pilote ici.
#include "features/minigames/doom.h"
#include "features/overlays/dps_meter.h"
#include "features/overlays/login_parade.h"
#include "features/minigames/roggle.h"
#include "features/minigames/rojeweled.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// « DPS Meter » et « Mini-jeux » : deux en-têtes de premier niveau qui pilotent
// des plugins frères (DpsMeter, Doom, LoginParade, Roggle, RoJeweled).
//
// ⚠ Le saut, la marche au clavier et le jaillissement du butin ont quitté cet
// en-tête pour la nav de « Gameplay » : aucun des trois n'était un mini-jeu, et
// deux d'entre eux sont des ENTRÉES. Leurs corps sont dans panel_gameplay.cc.
void MoonlightUi::DrawFunPanels() {
  if (iface::LinkableHeader("dps")) {
    PushStyleCompact();
    if (auto* dps_meter = Bourgeon::Instance().dps_meter()) {
      if (dps_meter->DrawSettings()) SaveSettings();
    } else {
      ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
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
          "Nécessite un WAD à côté de l'exe du client : doom.wad (Doom complet), "
          "doom2.wad, tnt.wad, plutonia.wad, freedoom1.wad, freedoom2.wad, "
          "chex.wad, hacx.wad ou doom1.wad (shareware). Si plusieurs sont "
          "présents, la fenêtre te fait choisir au lancement.\n"
          "Clique la fenêtre DOOM pour capturer le clavier : ZQSD (AZERTY), "
          "WASD ou flèches pour bouger, Ctrl tirer, Espace/E ouvrir, Shift "
          "courir, Échap menu.\n"
          "Décocher = pause. Quitter depuis le menu DOOM = définitif "
          "jusqu'au redémarrage du client."));
      ImGui::TextDisabled(i18n::Tr("État : %s"), doom->StatusText());
    } else
      ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));

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
      ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));

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
      ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));

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
      ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
  }
}
