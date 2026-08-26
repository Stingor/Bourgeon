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

// ── Les mêmes idées, mais en ImU32 ───────────────────────────────────────────
//
// Ce que consomment les ImDrawList et `PushStyleColor` — l'autre moitié de
// l'interface, celle qui peint au lieu d'écrire.
//
// 🔴 CES TEINTES NE SONT PAS CELLES D'AU-DESSUS, et il ne faut pas les y
// ramener sans le vouloir : `kColOk` vaut (13,107,31) quand `kGreen` vaut
// (≈25,127,38) ; `kColWarn` vaut (166,102,0) quand `kWarn` vaut (≈140,84,20).
// Elles ont été choisies séparément, pour des fonds différents. Les aligner est
// un choix d'interface, pas une correction.

// L'état d'une fabrication — les trois fenêtres (atlas de recettes, fabrication,
// affinage) les déclaraient chacune pour elle, à la valeur près.
inline constexpr ImU32 kColOk = IM_COL32(13, 107, 31, 255);   // succès, stock présent
inline constexpr ImU32 kColBad = IM_COL32(166, 38, 38, 255);  // échec, stock à zéro
inline constexpr ImU32 kColWarn =
    IM_COL32(166, 102, 0, 255);  // refus, attente, plafond

// ── Le chrome d'une fenêtre de DESCRIPTION ───────────────────────────────────
// Le parchemin et son bandeau : la description d'objet et la fiche de monstre
// les poussaient chacune, treize fois en tout.
inline constexpr ImU32 kDescBg = IM_COL32(245, 243, 232, 255);
// Le même fond pour une POPUP, qui laisse voir un peu de ce qu'il y a dessous.
inline constexpr ImU32 kDescPopupBg = IM_COL32(245, 243, 232, 240);
inline constexpr ImU32 kDescTitleBg = IM_COL32(120, 110, 90, 255);

// L'OMBRE PORTÉE d'un texte, alpha par défaut. Six sites la posaient à
// l'identique ; c'est la référence, pas une obligation — un texte sur fond très
// clair peut vouloir moins.
//
// ⚠ N'est PAS la couleur d'un fond translucide : même valeur, autre rôle. La
// minimap et l'enregistreur de zone posent le même (0,0,0,200) SOUS quelque
// chose, et ils ne s'aligneront pas sur celui-ci.
inline constexpr ImU32 kTextShadow = IM_COL32(0, 0, 0, 200);

// ── 🔴 CE QUI N'EST PAS ICI, ET NE PEUT PAS Y ÊTRE SANS DÉCISION ─────────────
//
// Il reste ~340 littéraux `IM_COL32` dans le projet, et ils ne se rangent pas :
// leurs teintes DIVERGENT pour un même rôle. Relevé du 2026-08-26 :
//
//     fond    60 nuances,  dont 7 noirs :  α = 205 200 180 150 45 40 30
//     trait   45 nuances,  dont 8 noirs :  α = 200 190 180 160 120 90 80 40
//     texte   35 nuances,  dont 4 noirs :  α = 200 190 180 160
//
// Sept alphas pour « un fond sombre », huit pour « un trait sombre ». Les
// ramener à deux ou trois VALEURS CHANGERAIT L'APPARENCE de la moitié des
// fenêtres — ce n'est pas un rangement, c'est une refonte visuelle, et elle
// appartient à qui regarde l'écran.
//
// Ce qui a été rangé l'a été parce que c'était DÉJÀ identique : 73 littéraux
// qu'ImGui nomme lui-même (`IM_COL32_WHITE`, `_BLACK`, `_BLACK_TRANS`) et les
// valeurs partagées entre fichiers pour un même rôle. Le reste attend un choix.

// Le survol d'une case : on ÉCLAIRCIT, on ne colore pas. Un voile bleu façon
// ImGui jurerait avec l'art du jeu — les trois viewers d'objets s'accordaient
// déjà là-dessus, chacun dans son coin.
inline constexpr ImU32 kHoverTint = IM_COL32(255, 255, 255, 45);

}  // namespace pal
}  // namespace ro
