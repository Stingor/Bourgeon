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

// DEUX FORMES selon ce que le panneau touche :
//  - fonction libre dans `moonlight_ui::` quand il ne lit aucun état de la classe
//    (charte du serveur) ;
//  - MÉTHODE MEMBRE de MoonlightUi, déclarée dans moonlight_ui.h et définie ici,
//    dès qu'il manipule l'état privé (miroirs de réglages serveur, presets…).
//    L'alternative — fonctions libres prenant MoonlightUi& — aurait imposé de
//    rendre ces membres publics, soit exactement le défaut que le chantier 8
//    cherche à supprimer.

#include <cstdint>

class MoonlightUi;

// Dernier item dont la fenêtre de description a été ouverte, capté par le hook
// ItemDescWndHook (moonlight_ui.cc). Le panneau Autoloots s'en sert pour proposer
// l'item survolé en un clic. Défini dans item_desc_probe.cc.
extern uint32_t g_last_viewed_item;

namespace moonlight_ui {

// Charte du serveur — texte pur, aucun état.
void DrawRules();

}  // namespace moonlight_ui

// Méthodes membres définies dans ce dossier (déclarées dans moonlight_ui.h) :
//   MoonlightUi::DrawCommandsPanel()      -> panel_commands.cc
//   MoonlightUi::DrawFunPanels()         -> panel_fun.cc
//   MoonlightUi::DrawAlootOverlay()      -> item_desc_probe.cc
//   MoonlightUi::InstallItemDescProbe()  -> item_desc_probe.cc
