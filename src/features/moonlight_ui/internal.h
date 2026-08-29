#pragma once

// ── Découpage interne du panneau Moonlight ───────────────────────────────────
// En-tête PRIVÉ au dossier src/features/moonlight_ui/ : il ne doit être inclus que
// par moonlight_ui.cc et par les panel_*.cc voisins. Rien d'autre dans le projet
// n'a de raison de connaître ces fonctions — l'API publique du plugin reste
// features/moonlight_ui/moonlight_ui.h.
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
#include <string>
#include <vector>

#include "features/moonlight_ui/moonlight_ui.h"  // iface::NavGroup (tables de nav)
#include "ui/ro_imgui.h"  // ro::RoSkinConfig

// Texte affiché quand le plugin propriétaire d'une section n'est pas enregistré.
// Deux libellés concurrents cohabitaient — « Indisponible. » (6 fois) et
// « (plugin indisponible) » (6 fois) — pour exactement le même cas. Un seul
// désormais : la formulation ne peut plus dépendre de la section qu'on regarde.
inline constexpr const char* kPluginUnavailable = "(plugin indisponible)";

// Dernier item dont la fenêtre de description a été ouverte, capté par le hook
// ItemDescWndHook (moonlight_ui.cc). Le panneau Autoloots s'en sert pour proposer
// l'item survolé en un clic. Défini dans item_desc_probe.cc.
extern uint32_t g_last_viewed_item;

// (Les presets de skin RO ont déménagé vers ui/skin_panel.h : ro::SkinPreset,
// ro::SkinPresets(), ro::SkinPresetSelection(). Ce sont des données du TOOLKIT ;
// moonlight_ui n'en garde que la persistance yaml.)

namespace moonlight_ui {

// Charte du serveur — texte pur, aucun état.
void DrawRules();

}  // namespace moonlight_ui

namespace iface {

// Les deux tables de nav, chacune définie dans le panel qui la dessine. Elles ne
// sortent pas du dossier : `iface::Group()` (panel_nav.cc) est ce que le reste du
// projet consulte, et il passe par ces deux-ci.
//
// Pourquoi une fonction plutôt qu'une table `extern` : le tableau d'entrées est un
// `constexpr` interne à son .cc, au plus près du contenu qu'il indexe. Une
// déclaration `extern` obligerait à le rendre visible — et à le déplacer.
const NavGroup& InterfaceGroup();  // panel_interface.cc
const NavGroup& GameplayGroup();   // panel_gameplay.cc

}  // namespace iface

// Méthodes membres définies dans ce dossier (déclarées dans moonlight_ui.h) :
//   MoonlightUi::DrawCommandsPanel()      -> panel_commands.cc
//   MoonlightUi::DrawFunPanels()         -> panel_fun.cc
//   MoonlightUi::DrawAlootOverlay()      -> item_desc_probe.cc
//   MoonlightUi::InstallItemDescProbe()  -> item_desc_probe.cc
