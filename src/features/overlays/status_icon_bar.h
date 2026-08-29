#pragma once

#include <cstdint>
#include <vector>

#include "features/plugin.h"
#include "features/status_cell.h"  // statuscell::Sort, Sweep, DrawRow
#include "ui/hud_frame.h"

// ── Ce que le CLIENT sait de MES états ──────────────────────────────────────
//
// 🔴 Sa liste est PLUS COMPLÈTE que tout ce qui passe par le réseau. Le sommeil,
// le silence, le gel n'arrivent pas en EFST : le serveur les transmet dans
// `opt1`/`opt2` (ZC_STATE_CHANGE), et c'est le CLIENT qui les convertit en
// entrées de cette liste — c'est pour cela que sa barre les affiche alors
// qu'aucun paquet d'état ne les nomme.
//
// Les surfaces qui listent MES états doivent donc lire ICI, et non attendre du
// réseau ce qu'il ne porte pas. Pour les AUTRES entités, la question ne se pose
// pas : cette liste n'est que la mienne.
//
// `end_tick` est l'horloge du client (`GetTickCount`) ; 999999 = permanent,
// rendu tel quel — c'est sa sentinelle, à l'appelant de la reconnaître.
namespace statusicons {

struct Active {
  uint16_t id = 0;
  uint32_t end_tick = 0;
};

// Sentinelle « pas d'échéance » du client.
inline constexpr uint32_t kInfinite = 999999;

// Remplit `out` avec MES états actifs. Faux si la liste n'est pas lisible
// (hors jeu, ou pointeurs pas encore posés).
bool ReadOwn(std::vector<Active>* out);

}  // namespace statusicons

// ── La barre de MES états ───────────────────────────────────────────────────
//
// 🔴 CE QU'ELLE A REMPLACÉ, ET POURQUOI. La première version ne dessinait rien :
// elle PILOTAIT la barre du client par trois détours — la construction
// (`FUN_00bd4230`), le hit-test de l'infobulle (`FUN_00c93cb0`) et le rendu
// d'une icône (`FUN_00b5ed20`) — pour tordre un résultat qu'elle ne produisait
// pas. Trois adresses en dur, la fabrication de nœuds de scène avec
// l'`operator new` du jeu, et une couche ImGui par-dessus qui devait RECALCULER
// la position que le détour venait de donner à chaque icône. Deux dispositions à
// garder d'accord, pour un affichage qui n'avait toujours ni durée, ni
// infobulle à nous.
//
// Elle dessine maintenant elle-même, avec `statuscell` — la même case que la
// grille de groupe, la liste Groupe/Amis et la barre de cible. Ce qui était
// impossible à piloter vient gratuitement : le compte à rebours, le grisage de
// la part écoulée, l'infobulle complète du client, le tri, les lignes.
//
// ⚠ IL RESTE DEUX DÉTOURS, et chacun a sa raison :
//   · la CONSTRUCTION, pour que le client n'émette plus ses propres icônes quand
//     la nôtre est allumée — on rejoue sa porte de reconstruction (qui contient
//     SON effacement des anciens nœuds) et on n'émet rien ;
//   · le RENDU d'une icône, qui ne sert plus qu'à la barre NATIVE : l'opacité
//     quand on la garde, et l'écran de veille, qui doit pouvoir la taire.
//     🔴 Ces nœuds ne sont pas des fenêtres : l'effacement d'interface de l'écran
//     de veille veto le rendu des UIWindow, et les traversait intacts.
class StatusIconBar : public Plugin {
 public:
  StatusIconBar();

  const char* name() const override { return "StatusIcons"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Rend les contrôles de réglage (sans fenêtre à elle) ; hébergés par
  // moonlight_ui dans sa section « Icônes de statut ».
  void DrawSettings();

  // ── Réglages ──────────────────────────────────────────────────────────────
  //
  // 🔴 LES CLÉS ONT ÉTÉ RENOMMÉES (`statusbar_*`), et ce n'est pas cosmétique :
  // les anciennes (`statusicon_corner`, `statusicon_step_dir`…) décrivaient un
  // ancrage à un coin de l'écran avec un sens d'empilement, un modèle qui n'a
  // plus cours — la barre est un cadre qu'on pose comme les autres. Recharger
  // ces valeurs dans les nouveaux champs aurait donné une barre placée n'importe
  // où, sans que rien ne l'explique.
  bool enabled_ = false;
  bool locked_  = true;
  bool border_  = true;

  int  icon_px_   = 24;
  int  max_icons_ = 40;  // le client en tient jusqu'à une centaine
  int  gap_px_    = 2;
  int  rows_      = 1;

  // Le COIN du cadre d'où la rangée grandit : 0 = haut gauche, 1 = haut
  // droite, 2 = bas gauche, 3 = bas droite.
  //
  // 🔴 Ce n'est pas une coquetterie de mise en page. Une rangée grandit dans un
  // sens ; ancrée du mauvais côté, elle RECULE quand un état tombe, et toutes
  // les icônes glissent sous le curseur. Une barre posée en haut à droite de
  // l'écran veut donc pousser vers la gauche, celle du bas vers le haut.
  int  anchor_ = 0;

  int  sort_ = statuscell::kSortArrival;  // l'ordre du client, par défaut

  bool show_time_ = true;
  int  time_px_   = 11;

  int   sweep_ = statuscell::kSweepVertical;
  float col_sweep_[4] = {0.0f, 0.0f, 0.0f, 0.55f};
  float col_bg_[4]    = {0.05f, 0.05f, 0.07f, 0.55f};

  // L'aperçu : la barre se remplit de faux états, le temps de la régler.
  //
  // ⚠ NON persisté, délibérément. C'est un outil de réglage, pas un mode de
  // jeu : le retrouver allumé à la session suivante ferait croire à des buffs
  // qu'on n'a pas — et le premier réflexe serait de chercher d'où ils viennent.
  bool preview_ = false;

  // Opacité des icônes, en pourcent.
  //
  // ⚠ Elle vaut pour les DEUX barres : la nôtre l'applique à ses cases, et le
  // détour de rendu la force sur celles du client quand on garde la sienne. Un
  // réglage qui ne marcherait que dans un mode se lirait comme une panne.
  int  alpha_ = 100;

  ro::HudRect& rect()  { return rect_; }
  int& icon_px()   { return icon_px_; }
  int& max_icons() { return max_icons_; }
  int& gap_px()    { return gap_px_; }
  int& rows()      { return rows_; }
  int& sort()      { return sort_; }
  int& anchor()    { return anchor_; }
  int& time_px()   { return time_px_; }
  int& sweep()     { return sweep_; }
  int& alpha()     { return alpha_; }

 private:
  // Mes états, convertis dans le vocabulaire de `statuscell`. Complète avec de
  // faux états quand l'aperçu est allumé.
  void Collect(std::vector<StatusEffects::Entry>* out) const;

  ro::HudRect rect_{40, 120, 240, 34};
  bool geometry_dirty_ = false;
};
