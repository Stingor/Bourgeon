// ── Section « Gameplay » du panneau Moonlight ────────────────────────────────
// Entre « Interface de jeu » et « Commands Settings » : les réglages qui
// changent la MANIÈRE DE JOUER, pas l'habillage — précision du ciblage, quick
// cast, enregistreur GIF, écran de veille. Nés dans Staff Tools, ouverts aux joueurs le
// 2026-08-18 ; le staff garde ses propres vues dans sa fenêtre.

#include <algorithm>  // std::max (rectangle = max(dessin, minimum))
#include <cfloat>     // FLT_MAX (bbox du sprite d'aperçu)
#include <cstdio>     // snprintf (libellés avec les pixels calculés)

#include "features/moonlight_ui/internal.h"
#include "features/moonlight_ui/moonlight_ui.h"

#include "bourgeon.h"
#include "features/fx/zone_recorder.h"
#include "features/gameplay/afk_screen.h"
#include "features/gameplay/quick_cast.h"
#include "features/patches/pick_quad_tweaks.h"
#include "imgui.h"
#include "ui/mob_sprite.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// Armé par le proxy DirectDraw dès le premier EndScene du chemin DX7 : c'est le
// seul témoin du mode de rendu réellement en cours. La passe de post-traitement
// n'existe que sous DX9, et trois curseurs qui ne font rien méritent une phrase
// plutôt que du silence.
extern bool g_imgui_dx7_active;

namespace {

// L'aperçu emprunte un monstre RÉEL et PETIT — l'œuf (classe 2408) — pour que
// la démonstration soit celle du problème : un sprite de 25 px qui se défend
// sur 100. Le choix de l'id vit ici et nulle part ailleurs.
constexpr int kPreviewMobId = 2408;

// Boîte englobante du sprite (action 0, image 0), en pixels ÉCRAN — c'est la
// part « dessin » du rectangle cliquable du jeu, qui vaut max(dessin, minimum).
// Rend false tant que le sprite n'est pas exploitable.
bool MobSpriteBBox(const ro::MobSpriteRes& res, float* w, float* h) {
  ro::SpriteQuad quads[8];
  const int n = ro::SpriteResolveFrame(res.sprite, 0, 0, quads, 8,
                                       /*apply_rotation=*/false);
  if (n <= 0) return false;
  float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
  for (int i = 0; i < n; ++i) {
    for (const ImVec2& c : quads[i].corner) {
      if (c.x < min_x) min_x = c.x;
      if (c.y < min_y) min_y = c.y;
      if (c.x > max_x) max_x = c.x;
      if (c.y > max_y) max_y = c.y;
    }
  }
  *w = max_x - min_x;
  *h = max_y - min_y;
  return (*w > 0.0f && *h > 0.0f);
}

// L'œuf et sa zone cliquable, À L'ÉCHELLE 1:1 DE L'ÉCRAN — le rectangle affiché
// a exactement la taille de celui que le jeu consulte, c'est ce qui rend
// l'aperçu honnête. Gris : la zone d'origine du client. Jaune : celle du
// réglage courant. L'œuf par-dessus, animé, à sa taille réelle.
void DrawEggPreview() {
  static ro::MobSpriteRes egg;
  if (!ro::LoadMobSprite(kPreviewMobId, &egg)) return;

  const int def = pick_quad::MinAreaDefault(pick_quad::kFamilyActors);
  const int cur = pick_quad::MinAreaCurrent(pick_quad::kFamilyActors);
  if (def <= 0) return;  // pas encore mesuré (aucun acteur affiché ?)

  float egg_w = 0.0f, egg_h = 0.0f;
  if (!MobSpriteBBox(egg, &egg_w, &egg_h)) return;

  // Le rectangle du JEU : le dessin, élargi au minimum s'il est plus petit —
  // la formule exacte de CActorSprite_SubmitNameplateQuad.
  const float box_w = (std::max)(static_cast<float>(cur), egg_w);
  const float box_h = (std::max)(static_cast<float>(cur), egg_h);
  const float ref_w = (std::max)(static_cast<float>(def), egg_w);
  const float ref_h = (std::max)(static_cast<float>(def), egg_h);

  const float frame_h = ref_h + ro::Px(26.0f);
  ImGui::BeginChild("##pickbox_preview", ImVec2(0.0f, frame_h), false,
                    ImGuiWindowFlags_NoScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail  = ImGui::GetContentRegionAvail();
  const ImVec2 center(origin.x + avail.x * 0.5f, origin.y + frame_h * 0.5f);

  // La zone d'ORIGINE d'abord (dessous), en gris : c'est le point de
  // comparaison. Puis la zone COURANTE en jaune. À réglage d'origine, les deux
  // se confondent — on ne dessine que la jaune.
  auto rect = [&](float w, float h, ImU32 col, float thickness) {
    dl->AddRect(ImVec2(center.x - w * 0.5f, center.y - h * 0.5f),
                ImVec2(center.x + w * 0.5f, center.y + h * 0.5f), col, 0.0f, 0,
                thickness);
  };
  if (cur != def)
    rect(ref_w, ref_h, IM_COL32(160, 160, 160, 110), 1.0f);
  rect(box_w, box_h, IM_COL32(255, 210, 60, 230), 2.0f);

  // L'œuf, à sa taille réelle (pas d'agrandissement : le gabarit EST
  // l'information), animé pour rappeler que c'est le jeu, pas un pictogramme.
  const ImVec2 egg_min(center.x - egg_w * 0.5f, center.y - egg_h * 0.5f);
  const ImVec2 egg_max(center.x + egg_w * 0.5f, center.y + egg_h * 0.5f);
  ro::DrawMobSprite(dl, egg, egg_min, egg_max,
                    static_cast<float>(ImGui::GetTime()));

  ImGui::EndChild();
}

}  // namespace

void MoonlightUi::DrawGameplayPanel() {
  if (!iface::LinkableHeader("gameplay")) return;
  PushStyleCompact();

  // ── Précision du ciblage ────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Précision du ciblage"));

  const int def_actor = pick_quad::MinAreaDefault(pick_quad::kFamilyActors);
  const int def_npc   = pick_quad::MinAreaDefault(pick_quad::kFamilyNpc);

  // Le préambule donne les CHIFFRES DE CET ÉCRAN, pas une généralité : le
  // minimum dépend de la résolution, et c'est précisément ce que le joueur n'a
  // aucun moyen de deviner.
  if (def_actor > 0) {
    char intro[512];
    std::snprintf(
        intro, sizeof(intro),
        i18n::Tr("Le jeu ÉLARGIT la surface qui répond au clic de chaque entité "
                 "jusqu'à un minimum qui dépend de la résolution — sur ton "
                 "écran : %d px pour les monstres et joueurs, %d px pour les "
                 "PNJ. Un petit monstre se défend donc sur une surface bien "
                 "plus grande que lui, et deux entités proches se disputent le "
                 "même clic."),
        def_actor, (def_npc > 0) ? def_npc : (def_actor * 34) / 40);
    ImGui::TextWrapped("%s", intro);
  } else {
    ImGui::TextWrapped("%s", i18n::Tr(
        "Le jeu ÉLARGIT la surface qui répond au clic de chaque entité jusqu'à "
        "un minimum qui dépend de la résolution. Un petit monstre se défend "
        "donc sur une surface bien plus grande que lui, et deux entités "
        "proches se disputent le même clic."));
  }

  // Un combo, pas un curseur : quatre crans nommés suffisent, ce sont ceux du
  // patch WARP TighterClickArea — le réglage se décrit pareil des deux côtés.
  static const char* kShiftNames[5] = {
      "Réglage d'origine du jeu", "Moitié", "Quart", "Huitième", "Seizième",
  };
  int shift = pick_quad::min_shift();
  if (shift < 0) shift = 0;
  if (shift > 4) shift = 4;

  // Chaque option porte SA taille en pixels : « Moitié (50 px) » se compare,
  // « Moitié » se devine.
  auto option_label = [&](int s, char* buf, size_t n) {
    if (def_actor > 0) {
      const int px = (std::max)(1, def_actor >> s);
      std::snprintf(buf, n, "%s (%d px)", i18n::Tr(kShiftNames[s]), px);
    } else {
      std::snprintf(buf, n, "%s", i18n::Tr(kShiftNames[s]));
    }
  };

  char preview[96];
  option_label(shift, preview, sizeof(preview));
  ImGui::SetNextItemWidth(ro::Px(230.0f));
  if (ro::RoBeginCombo(i18n::Tr("Zone cliquable minimale"), preview)) {
    for (int s = 0; s <= 4; ++s) {
      char label[96];
      option_label(s, label, sizeof(label));
      const bool selected = (shift == s);
      if (ImGui::Selectable(label, selected)) {
        pick_quad::min_shift() = s;
        SaveSettings();
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ro::RoEndCombo();
  }
  SameLine();
  HelpMarker(
      i18n::Tr("Réduit ce minimum : le ciblage devient plus précis — deux "
               "entités voisines cessent de se disputer le même clic — mais "
               "les petits monstres deviennent plus exigeants à viser.\n\n"
               "« Moitié » est un bon départ. La proportion à la résolution "
               "est conservée, et le réglage s'applique aussi aux PNJ.\n\n"
               "⚠ Sans effet sur un GROS sprite : sa surface vient de son "
               "dessin, pas de ce minimum."));

  // L'aperçu vaut mieux que le texte : l'œuf est dessiné à sa taille réelle,
  // et le rectangle est EXACTEMENT celui que le jeu consultera — même échelle,
  // mêmes pixels.
  DrawEggPreview();
  if (def_actor > 0)
    ImGui::TextDisabled(
        "%s", i18n::Tr("En jaune : la surface qui répond au clic avec ton "
                       "réglage. En gris : celle d'origine."));

  // ── Quick cast ──────────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Quick cast"));
  if (auto* quick_cast = Bourgeon::Instance().quick_cast())
    quick_cast->DrawSettings();
  else
    ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));

  // ── Enregistreur GIF ────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Enregistrer une zone (GIF)"));
  if (auto* zone_recorder = Bourgeon::Instance().zone_recorder())
    zone_recorder->DrawSettings(/*player_view=*/true);
  else
    ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));

  // ── Écran de veille ─────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Écran de veille"));
  DrawAfkScreenSettings();

  PopStyleCompact();
}

// Réglages de l'écran de veille. Tout est vivant : chaque valeur touchée pendant
// un aperçu se voit immédiatement, puisque le module relit sa configuration à
// chaque frame plutôt que de la figer à l'entrée en veille.
void MoonlightUi::DrawAfkScreenSettings() {
  auto* afk = Bourgeon::Instance().afk_screen();
  if (!afk) {
    ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
    return;
  }
  AfkScreen::Config& cfg = afk->config();
  bool changed = false;

  changed |= ro::RoCheckbox(i18n::Tr("Activer l'écran de veille"), &cfg.enabled);
  SameLine(); HelpMarker(i18n::Tr(
      "Passé un moment sans toucher au clavier ni à la souris, l'interface "
      "s'efface et la caméra recule pour tourner lentement autour de ton "
      "personnage.\n\n"
      "Le premier geste rend tout comme c'était. Le clic qui te réveille ne "
      "part pas dans le jeu : le décor a changé d'angle, il tomberait ailleurs "
      "que là où tu crois viser."));

  ImGui::BeginDisabled(!cfg.enabled);

  ImGui::SetNextItemWidth(ro::Px(200.0f));
  // 🔴 Le format d'un slider RO est une VALEUR, jamais une phrase :
  // `RoSliderScalar` réserve la place du texte en formatant les BORNES avec lui,
  // si bien qu'un « %d s sans rien toucher » réservait la largeur de « 900 s sans
  // rien toucher » — la piste tombait à zéro et les flèches disparaissaient avec
  // elle. L'explication est dans l'infobulle, à sa place.
  changed |= WheelSliderInt(i18n::TrId("Délai", "afk_delay"), &cfg.delay_s, 10, 900,
                            "%d s");
  SameLine(); HelpMarker(i18n::Tr(
      "Temps sans toucher au clavier ni à la souris avant que la veille ne "
      "commence."));

  // Items passés NUS : RoCombo les traduit à leur lecture (les envelopper ici
  // les traduirait deux fois).
  static const char* const kWakeModes[] = {"Clavier et souris", "Clavier seulement",
                                           "Souris seulement"};
  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= ro::RoCombo(i18n::Tr("Réveil"), &cfg.wake_on, kWakeModes,
                         IM_ARRAYSIZE(kWakeModes));
  SameLine(); HelpMarker(i18n::Tr(
      "Ce qui met fin à la veille. « Clavier seulement » évite qu'une souris "
      "frôlée ne la réveille sans arrêt.\n\n"
      "Ce réglage ne change PAS ce qui repousse la mise en veille : tant que tu "
      "joues, tout compte. Et ce qui n'a pas le droit de te réveiller n'agit pas "
      "non plus dans le jeu — sinon une touche partirait sur un décor que tu ne "
      "vois pas."));

  SeparatorText(i18n::Tr("Caméra"));
  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= WheelSliderFloat(i18n::TrId("Rotation", "afk_spin"), &cfg.spin_deg_s,
                              -30.0f, 30.0f, "%.1f °/s");
  SameLine(); HelpMarker(i18n::Tr(
      "Vitesse de l'orbite autour du personnage. Une valeur négative tourne "
      "dans l'autre sens ; zéro fige la vue, ce qui donne un simple plan large."));

  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= WheelSliderFloat(i18n::TrId("Plongée", "afk_tilt"), &cfg.tilt_deg,
                              15.0f, 85.0f, "%.0f °");
  SameLine(); HelpMarker(i18n::Tr(
      "Hauteur du regard au-dessus de l'horizon. Le jeu se joue à 45° ; plus "
      "haut, la caméra domine la scène, et tout en haut elle est presque à la "
      "verticale."));

  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= WheelSliderFloat(i18n::TrId("Recul", "afk_zoom"), &cfg.zoom_factor,
                              1.0f, 2.5f, "%.2f x");
  SameLine(); HelpMarker(i18n::Tr(
      "De combien la caméra s'éloigne, en multiples de ta distance habituelle. "
      "Le moteur ne laisse pas aller plus loin que sa propre limite de vue : "
      "au-delà, on verrait le bord du monde et le brouillard."));

  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= WheelSliderFloat(i18n::TrId("Transition", "afk_ease"), &cfg.ease_s,
                              0.2f, 6.0f, "%.1f s");
  SameLine(); HelpMarker(i18n::Tr(
      "Durée du basculement en veille. Le retour, lui, va trois fois plus vite : "
      "il répond à un geste que tu viens de faire."));

  SeparatorText(i18n::Tr("Image"));
  changed |= ro::RoCheckbox(i18n::Tr("Masquer toute l'interface"), &cfg.hide_ui);
  SameLine(); HelpMarker(i18n::Tr(
      "L'interface Bourgeon ET celle du client — fenêtres, noms au-dessus des "
      "personnages, bulles de chat. Rien n'est fermé pour autant : tout ce qui "
      "était ouvert l'est encore au réveil."));

  changed |= ro::RoCheckbox(i18n::Tr("Masquer le curseur"), &cfg.hide_cursor);
  SameLine(); HelpMarker(i18n::Tr(
      "La flèche de la souris disparaît aussi. Elle revient dès le premier "
      "geste, en même temps que l'interface."));

  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= WheelSliderFloat(i18n::TrId("Vignette", "afk_vignette"), &cfg.vignette,
                              0.0f, 1.0f, "%.2f");
  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= WheelSliderFloat(i18n::TrId("Noir et blanc", "afk_bw"), &cfg.desaturate,
                              0.0f, 1.0f, "%.2f");
  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= WheelSliderFloat(i18n::TrId("Grain", "afk_grain"), &cfg.grain,
                              0.0f, 1.0f, "%.2f");
  // Ces trois-là passent par le post-traitement DX9. Sous le proxy DX7 la veille
  // fonctionne — caméra et masquage — mais l'image reste telle quelle, et le
  // dire vaut mieux que de laisser croire à trois curseurs cassés.
  if (g_imgui_dx7_active)
    ImGui::TextDisabled("%s", i18n::Tr("Vignette, noir et blanc et grain "
                                       "demandent le rendu DirectX 9."));

  SeparatorText(i18n::Tr("Horloge"));
  changed |= ro::RoCheckbox(i18n::Tr("Afficher l'heure"), &cfg.show_clock);

  ImGui::BeginDisabled(!cfg.show_clock);
  changed |= ro::RoCheckbox(i18n::Tr("Afficher la durée d'absence"), &cfg.show_away);

  // Libellés passés NUS : RoCombo traduit chaque item à sa lecture. Les
  // envelopper ici les traduirait deux fois, et le second passage inscrirait de
  // l'anglais dans la liste des textes à traduire — invisible à l'écran, et long
  // à comprendre après coup.
  static const char* const kAnchors[] = {
      "Haut-gauche",   "Haut-centre", "Haut-droite",
      "Milieu-gauche", "Centre",      "Milieu-droite",
      "Bas-gauche",    "Bas-centre",  "Bas-droite"};
  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= ro::RoCombo(i18n::Tr("Position"), &cfg.clock_anchor, kAnchors,
                         IM_ARRAYSIZE(kAnchors));

  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= WheelSliderInt(i18n::TrId("Marge", "afk_clock_margin"),
                            &cfg.clock_margin, 0, 400, "%d px");
  SameLine(); HelpMarker(i18n::Tr(
      "Distance au bord de l'écran. Sans effet sur une position centrée, qui n'a "
      "pas de bord dont s'écarter."));

  ImGui::SetNextItemWidth(ro::Px(200.0f));
  changed |= WheelSliderFloat(i18n::TrId("Taille du texte", "afk_clock_scale"),
                              &cfg.clock_scale, 0.5f, 6.0f, "%.1fx");
  SameLine(); HelpMarker(i18n::Tr(
      "Taille de l'heure. La ligne d'absence suit, en plus petit."));

  // ⚠ `RoColorSwatch` avec l'alpha : « couleur ET opacité » est UNE décision
  // pour le joueur, et deux widgets l'obligeraient à faire l'aller-retour entre
  // un curseur et une pastille pour juger du résultat.
  ImVec4 clock_rgba = ImGui::ColorConvertU32ToFloat4(cfg.clock_col);
  if (RoColorSwatch(i18n::Tr("Couleur du texte"), &clock_rgba.x, nullptr,
                    /*with_alpha=*/true, /*numeric_inputs=*/false)) {
    cfg.clock_col = ImGui::ColorConvertFloat4ToU32(clock_rgba);
    changed = true;
  }

  changed |= ro::RoCheckbox(i18n::Tr("Ombrage du texte"), &cfg.clock_shadow);
  SameLine(); HelpMarker(i18n::Tr(
      "Une ombre d'un pixel sous le texte. C'est elle qui le garde lisible quand "
      "la caméra passe devant un décor clair."));
  ImGui::EndDisabled();

  ImGui::Spacing();
  if (ro::RoButton(i18n::Tr("Essayer maintenant"))) afk->StartNow();
  SameLine(); HelpMarker(i18n::Tr(
      "Entre en veille tout de suite, sans attendre le délai. Bouge la souris "
      "pour en sortir."));

  ImGui::EndDisabled();

  if (changed) SaveSettings();
}
