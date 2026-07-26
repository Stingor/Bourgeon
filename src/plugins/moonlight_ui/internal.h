#pragma once

// ── Découpage interne du panneau Moonlight ───────────────────────────────────
// En-tête PRIVÉ au dossier src/plugins/moonlight_ui/ : il ne doit être inclus que
// par moonlight_ui.cc et par les panel_*.cc voisins. Rien d'autre dans le projet
// n'a de raison de connaître ces fonctions — l'API publique du plugin reste
// plugins/moonlight_ui.h.
//
// Pourquoi ce découpage : MoonlightUi::OnRenderUI faisait 1702 lignes d'un seul
// tenant, 11 niveaux d'accolades au pic, avec 9 variables `bool changed` homonymes
// dans 9 portées de la MÊME fonction. Deux panneaux voisins à l'écran étaient à
// 700 lignes l'un de l'autre dans le fichier. C'est ce qui a permis au bug
// EndTabBar (425 lignes entre le Begin et le End) de survivre à toutes les revues.
//
// CONVENTION : chaque panneau qui modifie un réglage RENVOIE son `changed`, et
// c'est OnRenderUI qui décide d'appeler SaveSettings() une seule fois. Les
// panneaux qui ne portent aucun état renvoient void.

class MoonlightUi;

namespace moonlight_ui {

// Charte du serveur — texte pur, aucun état.
void DrawRules();

}  // namespace moonlight_ui
