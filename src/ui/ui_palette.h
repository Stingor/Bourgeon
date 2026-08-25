#pragma once

// ── La palette de l'interface Bourgeon ───────────────────────────────────────
//
// Ces couleurs étaient une CONVENTION D'ÉCRITURE : chaque fenêtre redéclarait
// les siennes, en haut de fichier ou dans le corps d'une fonction. Une
// convention se recopie, et ce qui se recopie dérive — c'est arrivé ici :
//
//   · le gris de libellé portait QUATRE noms — `kGray`, `kLabel`, `kLabelCol`,
//     `kDimOnLight` — pour dix-sept déclarations d'une seule et même valeur ;
//   · `kGreen` existait en QUATRE nuances, `kRed` en deux, `kBlack` en deux ;
//   · `character_sheet.cc` portait à lui seul dix `kGray` identiques, un par
//     fonction, et trois `kGreen` DIFFÉRENTS.
//
// D'où ce foyer. Il ne contient que les valeurs qui étaient déjà partagées à
// l'identique : le regrouper n'a rien changé à l'écran, et c'était la condition
// pour le faire sans arbitrer à la place du joueur.
//
// 🔴 CE QUI N'EST PAS ICI, ET POURQUOI. Les nuances minoritaires restent chez
// elles, chacune avec un commentaire disant de quelle entrée d'ici elle
// s'écarte. Les aligner CHANGERAIT l'apparence : c'est un choix d'interface, pas
// un nettoyage, et il appartient à qui regarde l'écran.
//
// ⚠ Pour du texte NORMAL, ne pas prendre une couleur d'ici : prendre
// `ImGui::GetStyleColorVec4(ImGuiCol_Text)`, qui suit le skin choisi par le
// joueur. Ce qui vit ici, ce sont les écarts VOULUS par rapport à ce texte —
// un libellé en retrait, un état bon/mauvais, une alerte.
//
// ⚠ Ces valeurs sont pensées pour le corps CLAIR du skin RO. Sur un fond sombre
// (les overlays sans chrome), elles s'y noient : là, c'est le blanc cassé et
// l'ombre portée qui font le travail.

#include "imgui.h"

namespace ro {
namespace pal {

// ── Texte ────────────────────────────────────────────────────────────────────

// Le libellé d'un couple « libellé : valeur », et plus généralement tout texte
// SECONDAIRE sur le corps clair d'une fenêtre RO. Remplace `ImGui::TextDisabled`,
// qui vire à l'illisible dès que le fond s'éclaircit.
inline const ImVec4 kLabel(0.35f, 0.35f, 0.42f, 1.0f);

// La VALEUR en regard du libellé : presque noir, jamais tout à fait — un noir
// pur sur fond parchemin tape plus dur que le reste de l'interface.
inline const ImVec4 kValue(0.10f, 0.10f, 0.13f, 1.0f);

// Le noir franc, lui, existe pour ce qui doit trancher : une ombre portée, un
// chiffre sur une pastille de couleur.
inline const ImVec4 kBlack(0.00f, 0.00f, 0.00f, 1.0f);

// Le texte secondaire des fenêtres de RÉGLAGES. ⚠ Ce n'est PAS `kLabel` : plus
// chaud, plus proche du parchemin. Les deux coexistent depuis toujours, chacun
// dans sa famille de fenêtres ; les fondre est une décision d'interface.
inline const ImVec4 kSecondaryText(0.42f, 0.38f, 0.32f, 1.0f);

// ── États ────────────────────────────────────────────────────────────────────
// Assez sombres pour rester lisibles SUR le corps clair. Une teinte plus vive
// attirerait l'œil sur un fond sombre et s'y noierait ici — quand une mention
// doit vraiment saillir, on peint une pastille et on écrit dessus en `kBlack`.

inline const ImVec4 kGreen(0.10f, 0.50f, 0.15f, 1.0f);  // bonus, réussite, actif
inline const ImVec4 kRed(0.60f, 0.12f, 0.12f, 1.0f);    // malus, échec, danger
inline const ImVec4 kBlue(0.15f, 0.25f, 0.60f, 1.0f);   // information, neutre

// Ambre sombre : « absent », « non renseigné », « à vérifier ». Ni une erreur ni
// une information — l'entre-deux.
inline const ImVec4 kWarn(0.55f, 0.33f, 0.08f, 1.0f);

}  // namespace pal
}  // namespace ro
