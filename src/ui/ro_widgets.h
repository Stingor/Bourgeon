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

// Sliders qui s'ajustent AUSSI à la molette quand ils sont survolés (réglage fin
// sans attraper le curseur). Shift = pas plus large. `step` vaut par défaut
// 0.01 (float) / 1 (int).
// ⚠ La molette est traitée HORS du slider et ne « désactive » donc jamais l'item :
// un appelant qui veut réagir seulement en fin d'édition doit combiner
// `IsItemDeactivatedAfterEdit()` avec `retour && !ImGui::IsItemActive()`, sinon les
// ajustements à la molette sont perdus.
bool WheelSliderFloat(const char* label, float* v, float lo, float hi,
                      const char* fmt = "%.2f", float step = 0.0f);
bool WheelSliderInt(const char* label, int* v, int lo, int hi,
                    const char* fmt = "%d", int step = 0);

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
