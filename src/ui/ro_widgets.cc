#include "ui/ro_widgets.h"

#include "imgui_internal.h"  // ImGui::SetKeyOwner, ImHashStr, WheelingWindowScrolledFrame
#include "ui/ro_imgui.h"  // ro::RoSliderFloat / RoSliderInt (rendu « façon RO »)
#include "utils/i18n.h"

namespace mui {

// Helper to display a little (?) mark which shows a tooltip when hovered.
// In your own code you may want to display an actual icon if you are using a merged icon fonts (see docs/FONTS.md)
void HelpMarker(const char* desc) {
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
    TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// Pastille + nuancier en popup, calqué sur `ChatTweaks::DrawBackgroundGroup` —
// mêmes dimensions de pastille (20×20), mêmes drapeaux de nuancier, même bouton
// « Fermer ». Ce qui n'est PAS repris, ce sont les préréglages : ils appartiennent
// aux fonds du chat natif, qu'ils patchent dans le .text.
bool RoColorSwatch(const char* label, float rgba[4], bool* out_open,
                   bool with_alpha) {
  bool changed = false;
  if (out_open) *out_open = false;
  ImGui::PushID(label);
  const ImVec4 swatch(rgba[0], rgba[1], rgba[2], rgba[3]);
  const int btn_flags = with_alpha ? ImGuiColorEditFlags_AlphaPreview
                                   : ImGuiColorEditFlags_NoAlpha;
  if (ImGui::ColorButton("##btn", swatch, btn_flags, ImVec2(20, 20)))
    ImGui::OpenPopup("picker");
  ImGui::SameLine();
  // Le libellé peut porter un identifiant caché (`##` ou `###`) : on n'affiche
  // que ce qui précède, comme le fait ImGui pour tous ses widgets. Coupé à la
  // main plutôt qu'avec `FindRenderedTextEnd`, qui vit dans imgui_internal.h —
  // un widget partagé n'a pas à faire dépendre ses appelants de l'API interne.
  const char* visible_end = label;
  while (*visible_end != '\0' && !(visible_end[0] == '#' && visible_end[1] == '#'))
    ++visible_end;
  ImGui::TextUnformatted(label, visible_end);

  // 🔴 Le nuancier RECOUVRE ce qui suit dans la fenêtre, et c'est inévitable :
  // un popup s'ouvre sous l'item qui l'appelle. Le déplacer hors de la fenêtre
  // supprimerait bien le recouvrement, mais un sélecteur qui surgit à côté de sa
  // pastille est déroutant. C'est donc à l'APPELANT de neutraliser ce qui passe
  // dessous pendant ce temps — d'où `out_open`, qui le lui dit.
  const bool open = ImGui::BeginPopup("picker");
  if (open) {
    const int pick_flags = ImGuiColorEditFlags_NoSidePreview |
                           (with_alpha ? ImGuiColorEditFlags_AlphaBar
                                       : ImGuiColorEditFlags_NoAlpha);
    changed |= ImGui::ColorPicker4("##pick", rgba, pick_flags);
    if (ro::RoButton(i18n::Tr("Fermer"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (out_open) *out_open = open;
  ImGui::PopID();
  return changed;
}

// ── Molette : verrou anti-défilement (cf. l'exposé dans ro_widgets.h) ─────────
namespace {

// Temps de pose du curseur avant qu'une zone puisse prendre la molette, et durée
// d'une salve (deux crans plus rapprochés que ça appartiennent au même geste, et
// un geste ne change pas de destinataire en cours de route). Réglés au ressenti :
// plus court, le survol de passage repasse ; plus long, le réglage se fait attendre.
constexpr double kHoverSettleSec = 0.30;
constexpr double kBurstSec = 0.35;

struct WheelGateState {
  ImGuiID hovered_id = 0;    // zone survolée à la dernière frame observée…
  int hovered_frame = -2;    // …et le numéro de cette frame (le survol est-il continu ?)
  double hovered_since = 0.0;
  double scroll_until = 0.0;  // fin de la salve de défilement en cours
  ImGuiID tuning_id = 0;      // zone en train d'être réglée à la molette…
  double tuning_until = 0.0;  // …et la fin de SA salve
};
WheelGateState g_wheel_gate;

float WheelGate(ImGuiID id, bool hovered, bool engaged) {
  if (id == 0 || (!hovered && !engaged)) return 0.0f;
  const double now = ImGui::GetTime();
  const int frame = ImGui::GetFrameCount();
  if (hovered) {
    // Le survol est NEUF si la zone change, mais aussi si la frame précédente ne
    // la survolait pas : sans ce second test, revenir sur une zone quittée depuis
    // longtemps hériterait de son ancien chrono et prendrait la molette aussitôt.
    if (id != g_wheel_gate.hovered_id || frame != g_wheel_gate.hovered_frame + 1) {
      g_wheel_gate.hovered_id = id;
      g_wheel_gate.hovered_since = now;
    }
    g_wheel_gate.hovered_frame = frame;
  }

  // Une zone déjà en train d'être réglée garde la main jusqu'au bout de sa salve :
  // elle ne doit pas se faire reprendre la molette au milieu d'un geste.
  const bool tuning =
      (g_wheel_gate.tuning_id == id && now < g_wheel_gate.tuning_until);
  if (!engaged && !tuning &&
      (now < g_wheel_gate.scroll_until ||
       now - g_wheel_gate.hovered_since < kHoverSettleSec))
    return 0.0f;

  // Possession posée à CHAQUE frame dès que la zone est prête, et non au moment où
  // un cran arrive : le défilement est appliqué dans NewFrame, donc ce qu'on
  // revendique ici ne vaut que pour la frame SUIVANTE. Revendiquer au cran, c'est
  // laisser la fenêtre défiler ce cran-là en plus de régler la valeur.
  ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, id);

  const float wheel = ImGui::GetIO().MouseWheel;
  if (wheel == 0.0f) return 0.0f;
  g_wheel_gate.tuning_id = id;
  g_wheel_gate.tuning_until = now + kBurstSec;
  ImGui::GetIO().MouseWheel = 0.0f;  // consommé : plus personne ne le relit
  return wheel;
}

}  // namespace

void WheelGateNewFrame() {
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (!ctx) return;
  // Un défilement a-t-il VRAIMENT eu lieu cette frame ? ImGui le note lui-même en
  // appliquant le scroll (UpdateMouseWheel, exécuté dans NewFrame juste avant
  // nous). Compter les crans ne le dirait pas : un cran au-dessus d'une fenêtre
  // qui n'a rien à défiler ne doit RIEN verrouiller.
  if (ctx->WheelingWindowScrolledFrame == ctx->FrameCount)
    g_wheel_gate.scroll_until = ImGui::GetTime() + kBurstSec;
}

float LastItemWheel(bool engaged) {
  return WheelGate(ImGui::GetItemID(), ImGui::IsItemHovered(), engaged);
}

float RegionWheel(const char* key, bool hovered, bool engaged) {
  // Hachage de la clé plutôt que ImGui::GetID : une zone dessinée à la main peut
  // être testée hors de toute fenêtre (après le End), où GetID n'a pas de pile
  // d'ID à interroger. L'identifiant ne sert qu'à reconnaître la zone d'une frame
  // à l'autre — il n'a pas à correspondre à un item existant.
  return WheelGate(ImHashStr(key), hovered, engaged);
}

// SliderFloat/SliderInt variants that ALSO adjust on mouse-wheel while hovered
// (fine-tuning without grabbing the handle). Step defaults to 0.01 (float) / 1 (int).
// Shift+wheel uses a larger step (0.10 / 10) for faster adjustment.
bool WheelSliderFloat(const char* label, float* v, float lo, float hi, const char* fmt, float step) {
  // Rendu = scrollbar horizontale RO (cf. ro::RoSliderFloat) ; ses flèches ajustent
  // au même pas que la molette ci-dessous.
  bool changed = ro::RoSliderFloat(label, v, lo, hi, fmt, step,
                                   ImGuiSliderFlags_AlwaysClamp);
  // 🔴 La molette passe par le verrou anti-défilement : parcourir une page de
  // réglages ne doit toucher à AUCUN slider traversé au passage. `LastItemWheel`
  // teste le survol, ce qui couvre aussi le slider GRISÉ — la molette est traitée
  // hors du widget (c'est ce qui permet de régler sans attraper la poignée), donc
  // `BeginDisabled` ne la voit pas passer, alors que le survol y est faux.
  const float w = LastItemWheel();
  if (w != 0.0f) {
    if (step <= 0.0f) step = ImGui::GetIO().KeyShift ? 0.10f : 0.01f;
    float nv = *v + w * step;
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    if (nv != *v) { *v = nv; changed = true; }
  }
  Tooltip(i18n::Tr("- Arrows / mouse wheel adjust value (Shift = larger step)\n- Ctrl-Click for direct input."));
  return changed;
}

bool WheelSliderInt(const char* label, int* v, int lo, int hi, const char* fmt, int step) {
  bool changed = ro::RoSliderInt(label, v, lo, hi, fmt, step,
                                 ImGuiSliderFlags_AlwaysClamp);
  // Voir WheelSliderFloat : même verrou, mêmes raisons.
  const float w = LastItemWheel();
  if (w != 0.0f) {
    if (step <= 0) step = ImGui::GetIO().KeyShift ? 10 : 1;  // unit precision by default ("à l'unité près")
    int nv = *v + (w > 0.0f ? step : -step);
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    if (nv != *v) { *v = nv; changed = true; }
  }
  Tooltip(i18n::Tr("- Arrows / mouse wheel adjust value (Shift = larger step)\n- Ctrl-Click for direct input."));
  return changed;
}

// Make the UI compact because there are so many fields
void PushStyleCompact()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, (float)(int)(style.FramePadding.y * 0.60f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, (float)(int)(style.ItemSpacing.y * 0.60f)));
}

// Restore the default UI style after PushStyleCompact()
void PopStyleCompact()
{
    ImGui::PopStyleVar(2);
}

}  // namespace mui
