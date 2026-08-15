#include "features/windows/staff_tools.h"

#include "bourgeon.h"
#include "features/fx/ground_paint.h"
#include "features/fx/weapon_dual_sprites.h"
#include "features/fx/zone_recorder.h"
#include "features/gameplay/quick_cast.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/overlays/entity_names.h"
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

  mui::SeparatorText(i18n::Tr("Noms des entités"));
  if (auto* entity_names = Bourgeon::Instance().entity_names())
    entity_names->DrawSettings();

  // Cast en une action : la touche du sort suffit, la visée est résolue sous le
  // curseur et le lancement émis par les messages d'acteur du clic natif
  // (cf. quick_cast.h pour les deux approches écartées).
  mui::SeparatorText(i18n::Tr("Quick cast"));
  if (auto* quick_cast = Bourgeon::Instance().quick_cast())
    quick_cast->DrawSettings();

  // Fond neutre pour les captures d'écran : repeint le terrain d'une couleur unie
  // sans toucher à sa géométrie (l'occlusion reste correcte).
  mui::SeparatorText(i18n::Tr("Fond de capture"));
  ground_paint::DrawSettings();

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
  if (auto* zone_recorder = Bourgeon::Instance().zone_recorder())
    zone_recorder->DrawSettings();

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
