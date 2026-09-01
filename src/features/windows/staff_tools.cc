#include "features/windows/staff_tools.h"

#include "bourgeon.h"
#include "features/fx/ground_paint.h"
#include "features/fx/weapon_dual_sprites.h"
#include "features/fx/zone_recorder.h"
#include "features/gameplay/quick_cast.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/windows/char_diagnostics.h"
#include "features/overlays/entity_names.h"
#include "features/patches/pick_quad_tweaks.h"
#include "features/staff_gate.h"
#include "imgui.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

namespace {

// Persiste les réglages du panneau partagé. Les outils ci-dessous écrivent tous
// dans le même fichier que le reste de Bourgeon — la fenêtre change, pas le
// stockage.
void Persist() {
  if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
}

}  // namespace

void StaffTools::Open() {
  open_ = true;
  need_pos_ = true;
  show_panel_ = true;
  Persist();
}

void StaffTools::Toggle() {
  if (open_) {
    open_ = false;
    Persist();
  } else {
    Open();
  }
}

void StaffTools::OnRenderUI() {
  // 🔴 Droit revérifié À CHAQUE FRAME, jamais mémorisé : le niveau de groupe
  // arrive au login et peut changer en cours de session. Un `open_` hérité du
  // yaml d'un compte devenu ordinaire n'ouvre donc rien.
  if (!open_ || !IsStaff()) return;
  if (!Bourgeon::Instance().IsGameActive()) return;

  if (need_pos_) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ro::Px(420.0f), ro::Px(520.0f)),
                             ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }

  // 🔴 HORS DE LA PILE ÉCHAP : cette fenêtre ne se ferme QUE par sa croix ou par
  // sa bascule. C'est un établi qu'on garde ouvert en jouant, et chaque Échap
  // destiné au menu du jeu le refermerait par surprise. En sortir a un second
  // effet, voulu lui aussi : elle n'AVALE plus Échap, qui continue donc d'ouvrir
  // le menu même quand l'établi est au premier plan.
  ro::SkipNextEscapeWindow();

  // Titre à suffixe STABLE : la fenêtre garde position et taille d'une langue à
  // l'autre. « Staff Tools » n'est pas traduit — c'est le nom que le staff se
  // donne, et il figure tel quel dans le menu Échap qui l'ouvre.
  const bool begun =
      ro::BeginRoWindow("Staff Tools###bourgeon_staff_tools", &show_panel_);
  if (!show_panel_) {
    open_ = false;
    show_panel_ = true;
    Persist();
  }
  if (!begun) { ro::EndRoWindow(); return; }

  mui::PushStyleCompact();

  // ── Fiche technique du personnage ──────────────────────────────────────────
  // En tête, parce que c'est ce qu'on ouvre le plus souvent quand on débogue un
  // comportement de combat : elle répond à « quels nombres mon client
  // applique-t-il », là où la feuille de perso répond à « qu'est-ce que je
  // gagne à monter une stat ».
  mui::SeparatorText(i18n::Tr("Diagnostic du personnage"));
  if (auto* diag = Bourgeon::Instance().char_diagnostics()) {
    if (ro::RoButton(i18n::Tr("Ouvrir la fiche technique"))) diag->Open();
    ImGui::SameLine();
    // L'aide de la fiche technique, collée au bouton qu'elle décrit. Elle
    // traînait après le bloc du curseur, donc sur la ligne du réglage voisin
    // quand celui-ci ne dessinait rien.
    mui::HelpMarker(
        i18n::Tr("Toutes les stats du personnage joué TELLES QUE LE CLIENT LES "
                 "CONNAÎT, y compris les valeurs techniques que l'interface du "
                 "jeu ne montre jamais : le délai d'attaque en millisecondes "
                 "(amotion) derrière l'ASPD, le recul encaissé (dmotion) lu sur "
                 "les paquets de coups, la vitesse de déplacement, et la cadence "
                 "réelle du .act en train d'être joué.\n\n"
                 "Sous déguisement, elle montre le sprite RÉELLEMENT joué et la "
                 "durée que ses animations donnent à l'attaque — c'est-à-dire ce "
                 "que le personnage subit à l'écran, pas ce que la classe "
                 "laisserait croire."));
    ImGui::SameLine();
    // Contour des zones cliquables : indépendant de la fenêtre, parce qu'on
    // l'observe justement en regardant la scène.
    ro::RoCheckbox(i18n::Tr("Contour des zones cliquables"),
                   &diag->show_pick_boxes());
    ImGui::SameLine();
    mui::HelpMarker(
        i18n::Tr("Dessine les rectangles ÉCRAN dont le client se sert pour "
                 "savoir sur QUOI la souris pointe — ce ne sont pas les "
                 "positions au sol, et c'est le rectangle le plus « devant » "
                 "qui gagne le clic, quel que soit son propriétaire.\n\n"
                 "Jaune épais = celui que le clic prendrait maintenant, avec "
                 "son AID. Rose = le vôtre. Vert = acteur, bleu = PNJ de "
                 "carte, orange = unité de compétence.\n\n"
                 "À regarder quand une compétence part sur la mauvaise cible, "
                 "ou ne part pas alors que le curseur semble bien placé."));

    // Précision du picking : le client gonfle tout petit rectangle à un minimum.
    if (diag->pick_min_default() > 0) {
      if (ro::RoCheckbox(i18n::Tr("Forcer la zone cliquable minimale"),
                         &diag->pick_min_enabled()))
        Persist();
      ImGui::SameLine();
      mui::HelpMarker(
          i18n::Tr("Décochée, Bourgeon ne touche PAS à la valeur du client — "
                   "c'est l'état à choisir pour mesurer le jeu tel quel, ou "
                   "pour vérifier ce que fait un patch posé sur "
                   "l'exécutable.\n\nCochée, la valeur du curseur est "
                   "réécrite à chaque image."));
      // Le TÉMOIN : ce que le client a calculé, relevé avant toute écriture de
      // notre part. C'est lui qui dit si un patch posé sur l'exe a mordu — et
      // il doit rester lisible quand la case est décochée, justement parce que
      // c'est là qu'on mesure.
      ImGui::SameLine();
      ImGui::TextDisabled(i18n::Tr("client : %d px"), diag->pick_min_default());
    }
    if (diag->pick_min_default() > 0 && diag->pick_min_enabled()) {
      // 🔴 La borne haute est LE DÉFAUT DU CLIENT, pas une constante. Il vaut
      // `largeur_fenêtre / 640 × 40` — 110 px en 1760 de large, bien au-delà des
      // 64 codés en dur au départ : le curseur ne pouvait alors QUE descendre,
      // et toucher la poignée faisait chuter la valeur sans retour possible.
      // Curseur à fond = comportement d'origine, dans toutes les résolutions.
      int px = diag->pick_min_size();
      const int px_max = (diag->pick_min_default() > 64)
                             ? diag->pick_min_default()
                             : 64;
      ImGui::SetNextItemWidth(ro::Px(160.0f));
      if (mui::WheelSliderInt(i18n::Tr("Zone cliquable minimale (px)"), &px, 4,
                              px_max))
        diag->pick_min_size() = px;
      if (ImGui::IsItemDeactivatedAfterEdit()) Persist();
      ImGui::SameLine();
      char tip[420];
      _snprintf_s(tip, sizeof(tip), _TRUNCATE,
                  i18n::Tr("Le client ÉLARGIT toute zone cliquable plus petite "
                           "que ce nombre de pixels, pour qu'un petit sprite "
                           "reste attrapable. Défaut de ce client : %d px.\n\n"
                           "Baisser resserre le ciblage — deux entités "
                           "voisines cessent de se disputer le même clic. "
                           "Trop bas, les petits monstres deviennent "
                           "difficiles à viser.\n\n"
                           "⚠ Sans effet sur un GROS sprite : son rectangle "
                           "vient de son dessin, pas de ce minimum."),
                  diag->pick_min_default());
      mui::HelpMarker(tip);
    }
  }

  // Le fantôme au GID négatif — même sujet que le contour ci-dessus, puisqu'on
  // le DÉBUSQUE avec, et corrigé au même endroit du client.
  if (pick_quad::DrawSettings()) Persist();

  mui::SeparatorText(i18n::Tr("Noms des entités"));
  if (auto* entity_names = Bourgeon::Instance().entity_names()) {
    if (entity_names->DrawSettings()) Persist();
  }

  // Cast en une action : la touche du sort suffit, la visée est résolue sous le
  // curseur et le lancement émis par les messages d'acteur du clic natif
  // (cf. quick_cast.h pour les deux approches écartées).
  mui::SeparatorText(i18n::Tr("Quick cast"));
  if (auto* quick_cast = Bourgeon::Instance().quick_cast()) {
    if (quick_cast->DrawSettings()) Persist();
  }

  // Fond neutre pour les captures d'écran : repeint le terrain d'une couleur unie
  // sans toucher à sa géométrie (l'occlusion reste correcte).
  mui::SeparatorText(i18n::Tr("Fond de capture"));
  if (ground_paint::DrawSettings()) Persist();

  // ── Sprites d'armes doubles ────────────────────────────────────────────────
  // ⚠ CE N'EST PLUS UN RÉGLAGE DE JOUEUR. Le comportement est devenu le défaut —
  // une arme porte son sprite, main gauche comprise — et la case n'est ici que
  // pour COMPARER avec le rendu d'origine du client quand on débogue les couches
  // d'armes. Elle reste donc accessible, mais au seul endroit où l'on sait ce
  // qu'on éteint.
  mui::SeparatorText(i18n::Tr("Sprites d'armes doubles"));
  if (auto* dual = Bourgeon::Instance().weapon_dual_sprites()) {
    if (ro::RoCheckbox(i18n::Tr("Sprite propre à chaque arme"), &dual->enabled()))
      Persist();
    ImGui::SameLine();
    mui::HelpMarker(
        i18n::Tr("ON (défaut) : chaque arme garde son apparence d'origine quand tu "
                 "en portes deux (assassin, kagerou/oboro) ou une seule en main "
                 "gauche.\n\nOFF : le comportement d'ORIGINE du client, qui fond "
                 "les deux armes en un sprite générique. À n'éteindre que pour "
                 "comparer les deux rendus."));
  }

  // Enregistrement d'une zone de l'écran en GIF animé : de quoi illustrer un
  // tutoriel avec ce que le joueur verra vraiment, interface Bourgeon comprise.
  mui::SeparatorText(i18n::Tr("Enregistrer une zone (GIF)"));
  if (auto* zone_recorder = Bourgeon::Instance().zone_recorder()) {
    if (zone_recorder->DrawSettings()) Persist();
  }

  // ── Journal Bourgeon ───────────────────────────────────────────────────────
  // Remplace la console Windows : tout ce qui passe par LogInfo/LogDiag/LogError
  // y arrive, sélectionnable et copiable.
  mui::SeparatorText(i18n::Tr("Journal"));
  if (ro::RoCheckbox(i18n::Tr("Fenêtre de logs"),
                     &Bourgeon::Instance().show_log_window()))
    Persist();
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Miroir en jeu de tout ce que le client journalise "
      "(LogInfo / LogDiag / LogError), à la place de la console Windows.\n\n"
      "Le texte est SÉLECTIONNABLE et copiable : sélection à la souris, "
      "Ctrl+A, Ctrl+C, ou le bouton « Copier tout ». L'affichage se restreint "
      "à une sous-chaîne et aux niveaux cochés — Info, Diag / Warn, Erreur.\n\n"
      "Réservé au staff, et le droit est revérifié à chaque frame : la "
      "fenêtre disparaît si le niveau de groupe change en cours de session."));

  // ── Glyphes coréens ────────────────────────────────────────────────────────
  // Ici, à côté du journal, parce que c'est SON usage : lire les chemins des
  // fichiers du jeu (« 유저인터페이스\… ») quand on débogue.
  if (ro::RoCheckbox(i18n::Tr("Glyphes coréens (redémarrage)"), &korean_glyphs_))
    Persist();
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Charge les caractères coréens dans les polices. Utile UNIQUEMENT "
      "pour lire les chemins des fichiers du jeu dans le journal — rien "
      "en jeu ne s'affiche en coréen.\n\n"
      "⚠ Prend effet au PROCHAIN LANCEMENT : les polices sont préparées "
      "une seule fois au démarrage, et le moteur DirectDraw ne sait pas "
      "les refaire en cours de partie.\n\n"
      "Éteint, l'atlas de polices est vingt fois plus léger et le client "
      "démarre plus vite. Un caractère coréen y apparaîtrait en carré."));

  mui::PopStyleCompact();

  // Fermer, calé à droite comme dans le panneau de réglages : position
  // recalculée à chaque frame, largeur mesurée sur le libellé traduit.
  ImGui::Separator();
  const float close_w = ro::MaxButtonWidth({i18n::Tr("Fermer")});
  ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - close_w);
  if (ro::RoButton(i18n::Tr("Fermer"), close_w)) {
    open_ = false;
    Persist();
  }

  ro::EndRoWindow();
}
