#pragma once

#include <cstdarg>

#include "imgui.h"

// ── ro_widgets : helpers ImGui génériques, partagés par tous les plugins ──────
// Extraits de features/moonlight_ui/moonlight_ui.h, où ils avaient été écrits faute d'un endroit
// mieux placé. Ils n'ont RIEN à voir avec le panneau de réglages : ce sont des
// widgets d'usage général. Les laisser dans le plugin obligeait 18 unités de
// compilation à inclure un en-tête de 400 lignes pour un tooltip, et forçait même
// le toolkit ui/ro_imgui.cc à dépendre de features/ — une inversion de couches.
//
// Rien ici ne connaît Bourgeon : uniquement de l'ImGui. Le pendant « habillé RO »
// (fenêtres, boutons, combos, scrollbars, CP949) est dans ui/ro_imgui.h.

// -- namespace mui -----------------------------------------------------------
// « Text », « Separator », « SameLine » sont des noms extrêmement génériques, et
// ce header est très largement inclus : ils vivaient donc dans le namespace
// GLOBAL de presque chaque unité de compilation. Le namespace les isole sans
// rien renommer — les .cc concernés ouvrent simplement `using namespace mui;`,
// ce qui laisse intacts les centaines de sites d'appel.
//
// ⚠ Ce header n'est PAS dans le PCH, et ne doit pas y entrer : pch.h s'interdit
// tout en-tête interne (il serait reconstruit à chaque retouche, avec les 66
// unités derrière). Il n'arrive pas non plus par ui/ro_imgui.h, qui n'inclut que
// <cstddef> — un fichier qui veut `mui::` doit inclure CELUI-CI.
namespace mui {

// Affiche un petit « (?) » qui montre `desc` en infobulle au survol.
void HelpMarker(const char* desc);

// ── Molette : verrou anti-défilement ─────────────────────────────────────────
// Une zone qui prend la molette au survol — slider de réglage, aperçu qui tourne,
// case de compétence — VOLE le défilement de la page qui la contient. Le joueur
// qui parcourt une page de réglages à la molette voit alors ses valeurs bouger
// sous le curseur sans l'avoir demandé : il voulait défiler, pas régler.
//
// Ces helpers n'accordent la molette à la zone survolée que quand le geste lui
// est clairement destiné :
//   * aucun défilement en cours — une salve de crans reste au défilement jusqu'au
//     bout, y compris une fois la page arrivée en butée (c'est là que le curseur
//     s'immobilise sur un widget et que l'accident se produisait) ;
//   * et le curseur est posé sur la zone depuis un court instant, pour que la
//     traverser en visant plus bas ne compte pas.
// La détection de défilement vient d'ImGui lui-même (`WheelingWindowScrolledFrame`,
// posé quand il APPLIQUE un scroll) et pas d'un comptage de crans : là où rien
// n'est défilable — char-select, fenêtre `NoScrollWithMouse` — aucun verrou ne
// s'arme et la molette reste franche.
//
// À appeler UNE fois par frame, juste après ImGui::NewFrame() et avant le moindre
// widget : c'est le point où l'on sait si la frame vient de défiler.
void WheelGateNewFrame();

// Molette destinée au DERNIER widget soumis, filtrée par le verrou ci-dessus.
// Renvoie les crans (signés) qui lui reviennent, 0 sinon — et les CONSOMME, donc
// personne d'autre ne les relira dans la frame. À appeler juste après le widget,
// comme IsLastItemHovered.
//
// `engaged` = l'utilisateur manipule DÉJÀ la zone (glisser en cours) : elle garde
// alors la molette sans délai et même si le curseur en est sorti.
float LastItemWheel(bool engaged = false);

// Même chose pour une zone hit-testée à la main (motif overlay : ImDrawList +
// rectangle calculé), où l'« item » ImGui n'existe pas. `key` identifie la zone —
// n'importe quelle chaîne stable et unique, elle ne sert qu'à la reconnaître d'une
// frame à l'autre ; `hovered` est le test de survol de l'appelant.
float RegionWheel(const char* key, bool hovered, bool engaged = false);

// Sliders qui s'ajustent AUSSI à la molette quand ils sont survolés (réglage fin
// sans attraper le curseur). Shift = pas plus large. `step` vaut par défaut
// 0.01 (float) / 1 (int). La molette passe par le verrou anti-défilement ci-dessus.
// ⚠ La molette est traitée HORS du slider et ne « désactive » donc jamais l'item :
// un appelant qui veut réagir seulement en fin d'édition doit combiner
// `IsItemDeactivatedAfterEdit()` avec `retour && !ImGui::IsItemActive()`, sinon les
// ajustements à la molette sont perdus.
bool WheelSliderFloat(const char* label, float* v, float lo, float hi,
                      const char* fmt = "%.2f", float step = 0.0f);
bool WheelSliderInt(const char* label, int* v, int lo, int hi,
                    const char* fmt = "%d", int step = 0);

// Sélecteur de couleur façon « Couleurs du chat » : une pastille cliquable, le
// libellé à côté, et le nuancier dans un popup. Deux raisons de le préférer à un
// ColorEdit4 en ligne — il tient sur une ligne courte, donc il passe dans un menu
// contextuel ; et il est IDENTIQUE au sélecteur des fonds du chat natif, juste
// à côté dans le même panneau. Deux widgets différents pour le même geste, dans la
// même fenêtre, c'est une hésitation gratuite pour le joueur.
//
// `rgba` est au format picker (0..1, RGBA). Renvoie true tant que la couleur
// change — donc à chaque frame d'un glissement dans le nuancier.
//
// `out_open` (facultatif) dit si le nuancier est ouvert CETTE frame. 🔴 Il est
// là pour un piège réel : le popup s'ouvre sous la pastille et RECOUVRE la suite
// de la fenêtre. Un appelant qui pose des curseurs juste en dessous doit les
// neutraliser pendant ce temps (`ImGui::BeginDisabled`), sinon viser le nuancier
// revient à attraper une poignée restée dessous.
//
// `with_alpha` à false retire la barre d'opacité ET la composante A de l'aperçu
// comme du champ hexadécimal. À utiliser dès que la couleur choisie n'a pas de
// transparence à régler — une entrée de palette de sprite, par exemple, dont
// l'octet d'alpha ne veut rien dire. Un curseur qui ne commande rien est pire
// qu'absent : il invite à le bouger.
bool RoColorSwatch(const char* label, float rgba[4], bool* out_open = nullptr,
                   bool with_alpha = true);

// Style compact (padding/espacement réduits) — pour les panneaux dense en champs.
// Toujours par paire.
void PushStyleCompact();
void PopStyleCompact();

// ── Enveloppes ImGui ─────────────────────────────────────────────────────────
// Pour qu'un plugin n'ait pas à inclure imgui.h juste pour ça. Inline et triviales,
// le compilateur les efface.
inline void SameLine(float x = 0.0f, float spacing = -1.0f) {
    ImGui::SameLine(x, spacing);
}
inline void Spacing() {
    ImGui::Spacing();
}
inline void Separator() {
    ImGui::Separator();
}
inline void BulletWrapped(const char* text) {
    ImGui::Bullet(); ImGui::SameLine(); ImGui::TextWrapped("%s", text);
}
inline void TextWrapped(const char* text) {
    ImGui::TextWrapped("%s", text);
}
inline void SeparatorText(const char* text) {
    ImGui::SeparatorText(text);
}
inline void Text(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}
inline void TextUnformatted(const char* text) {
    ImGui::TextUnformatted(text);
}
inline bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0) {
    return ImGui::CollapsingHeader(label, flags);
}
inline void Indent(float indent_w = 0.0f) {
    ImGui::Indent(indent_w);
}
inline void Unindent(float indent_w = 0.0f) {
    ImGui::Unindent(indent_w);
}
inline void OpenPopup(const char* str_id) {
    ImGui::OpenPopup(str_id);
}
inline bool BeginPopup(const char* str_id) {
    return ImGui::BeginPopup(str_id);
}
// Teste le DERNIER widget soumis (ImGui::IsItemHovered), pas la fenêtre ni un
// objet nommé — le sujet est dans le nom, pour ne pas inviter à l'appeler loin
// du widget concerné, où il répondrait sur autre chose.
inline bool IsLastItemHovered() {
    return ImGui::IsItemHovered();
}

// Clic DROIT sur le dernier widget soumis. Dans Ragnarok le clic droit sur un
// item ouvre sa description — c'est la convention du client, respectée par tous
// les viewers (inventaire, chariot, storage, boutiques, échoppe, refine), et le
// clic gauche reste libre pour sélectionner/glisser.
//
// Ça existait en quatre orthographes différentes dans le projet :
//   IsItemClicked(ImGuiMouseButton_Right)
//   IsItemHovered() && IsMouseClicked(ImGuiMouseButton_Right)   ← le MÊME test
//   hovered && IsMouseClicked(ImGuiMouseButton_Right)           ← rect à la main
// Les deux premières sont strictement équivalentes (IsItemClicked n'est que
// `IsItemHovered(None) && IsMouseClicked(b)`) et se disent maintenant ici.
//
// ⚠ Ne convient QU'À un vrai widget ImGui. Pour une zone hit-testée à la main
// (motif overlay : ImDrawList + rectangle calculé), l'« item » n'existe pas —
// il faut y écrire `hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)`.
//
// ⚠ Ne remplace PAS un `IsMouseReleased(Right)`. Relâchement ≠ enfoncement : sur
// un widget dont le clic droit ouvre un menu contextuel, déclencher au
// relâchement laisse le temps de sortir de la zone pour annuler. Là où le projet
// teste le relâchement, c'est délibéré.
inline bool IsLastItemRightClicked() {
    return ImGui::IsItemClicked(ImGuiMouseButton_Right);
}

inline void Tooltip(const char* text) {
    if (IsLastItemHovered()) ImGui::SetTooltip("%s", text);
}

inline void PushItemWidth(float item_width) {
    ImGui::PushItemWidth(item_width);
}
inline void PopItemWidth() {
    ImGui::PopItemWidth();
}
// ColorEdit4 et ColorPicker4 sont deux widgets ImGui DIFFÉRENTS ; celui-ci est
// le premier, avec barre alpha, sans aperçu latéral ni champs numériques. Le
// nom le dit maintenant, au lieu de faire attendre l'autre.
inline bool ColorEdit4WithAlphaBar(const char* label, float rgba[4]) {
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaBar |
                                ImGuiColorEditFlags_NoSidePreview |
                                ImGuiColorEditFlags_NoInputs |
                                ImGuiColorEditFlags_NoColorMarkers;
    return ImGui::ColorEdit4(label, rgba, flags);
}

}  // namespace mui
