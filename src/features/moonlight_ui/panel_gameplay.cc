// ── Section « Gameplay » du panneau Moonlight ────────────────────────────────
// Entre « Interface de jeu » et « Commands Settings » : les réglages qui
// changent la MANIÈRE DE JOUER, pas l'habillage — précision du ciblage, quick
// cast, enregistreur GIF. Nés dans Staff Tools, ouverts aux joueurs le
// 2026-08-18 ; le staff garde ses propres vues dans sa fenêtre.

#include <algorithm>  // std::max (rectangle = max(dessin, minimum))
#include <cfloat>     // FLT_MAX (bbox du sprite d'aperçu)
#include <cstdio>     // snprintf (libellés avec les pixels calculés)

#include "features/moonlight_ui/internal.h"
#include "features/moonlight_ui/moonlight_ui.h"

#include "bourgeon.h"
#include "features/fx/zone_recorder.h"
#include "features/gameplay/quick_cast.h"
#include "features/patches/pick_quad_tweaks.h"
#include "imgui.h"
#include "ui/mob_sprite.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

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

  PopStyleCompact();
}
