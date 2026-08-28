#pragma once

// ── La barre d'états de la CIBLE ────────────────────────────────────────────
//
// Une barre À PART, et non une rangée greffée sous le HUD de cible : elle se
// place, se dimensionne et se règle toute seule. Un joueur qui suit les états de
// sa cible ne les regarde pas au même endroit que ses points de vie — celui qui
// dissipe veut ses icônes grosses et près du centre de l'écran, celui qui frappe
// veut juste savoir si sa cible est gelée.
//
// La matière vient de `StatusEffects`, qui écoute le fil et interroge le serveur
// (CZ 0x0F2C). Cette barre ne connaît aucun paquet : elle demande le sondage et
// dessine ce qu'on lui rend.
//
// 🔴 ELLE SUIT LE CIBLAGE. Mode de ciblage éteint, pas de barre — au même titre
// que la barre de vie de la cible. Ce n'est pas une contrainte technique (la
// sélection vient du clic natif, `CGameMode+0xF4`, qui existe de toute façon)
// mais une règle d'interface : tout ce qui parle de la cible s'éteint avec
// elle, sinon il resterait à l'écran un morceau orphelin d'une fonction qu'on
// vient de couper.
//
// 🔴 CE QU'ELLE NE MONTRERA JAMAIS. Le serveur ne répond que sur un membre de
// mon groupe et sur les entités qui ne sont à personne (les monstres). Cibler un
// adversaire PVP ne donne rien, et c'est voulu : ses états sont une information
// de jeu. La barre reste alors vide, sans rien annoncer — un message dirait
// autant qu'une icône.
//
// 🔴 VIDE = INVISIBLE. Aucun emplacement réservé, aucun cadre en attente. C'est
// la même règle que partout ailleurs dans ce projet : on ne peint pas un
// réservoir vide qui se lirait comme un réservoir épuisé. Le cadre ne réapparaît
// que quand il a quelque chose à dire — ou quand on le déverrouille pour le
// placer.

#include <cstdint>
#include <vector>

#include "features/plugin.h"
#include "features/systems/status_effects.h"
#include "ui/hud_frame.h"

class TargetStatusBar : public Plugin {
 public:
  const char* name() const override { return "TargetStatusBar"; }

  void OnRenderUI() override;

  // ── Réglages ──────────────────────────────────────────────────────────────
  bool enabled_ = false;
  // Verrouillée : figée et clic-traversante. MAJ la libère ponctuellement, comme
  // les autres cadres de HUD.
  bool locked_  = true;
  bool border_  = true;

  int  icon_px_   = 24;  // côté d'une icône, en pixels d'interface
  int  max_icons_ = 12;  // au-delà, on tronque plutôt que de déborder
  int  gap_px_    = 2;   // écart entre deux icônes

  // L'ordre de lecture. « Bientôt fini » d'abord est le tri utile : c'est ce
  // qu'on va devoir relancer, ou ce qu'il suffit d'attendre.
  //
  // ⚠ Un état SANS échéance (durée infinie) n'a pas de place naturelle dans un
  // tri par durée. Il va toujours en FIN, quel que soit le sens : le mettre en
  // tête sous prétexte qu'il « dure le plus longtemps » aurait chassé de l'écran
  // les états qui pressent.
  enum Sort { kSortArrival = 0, kSortShortest, kSortLongest };
  int  sort_ = kSortShortest;

  // Le compte à rebours sous l'icône. Un état sans échéance n'en porte pas —
  // afficher « 0 » sur un buff permanent le ferait croire fini.
  bool show_time_   = true;
  int  time_px_     = 11;

  // ── Le grisage de la case ─────────────────────────────────────────────────
  //
  // Un voile qui recouvre la part ÉCOULÉE de l'état. Il se lit sans lire : on
  // voit d'un coup d'œil lesquels sont près de tomber, là où il faudrait
  // déchiffrer autant de nombres.
  //
  //   kSweepRadial   — le balayage horaire des jeux de rôle, depuis midi ;
  //   kSweepVertical — le voile descend depuis le haut, plus lisible petit ;
  //   kSweepNone     — rien, le nombre suffit.
  //
  // ⚠ Un état SANS échéance n'est jamais voilé : il n'a pas de part écoulée, et
  // le griser à moitié le ferait croire à mi-course.
  enum Sweep { kSweepNone = 0, kSweepRadial, kSweepVertical };
  int   sweep_ = kSweepRadial;
  float col_sweep_[4] = {0.0f, 0.0f, 0.0f, 0.55f};

  float col_bg_[4]  = {0.05f, 0.05f, 0.07f, 0.55f};

  ro::HudRect& rect() { return rect_; }
  int& icon_px()   { return icon_px_; }
  int& max_icons() { return max_icons_; }
  int& gap_px()    { return gap_px_; }
  int& sort()      { return sort_; }
  int& time_px()   { return time_px_; }
  int& sweep()     { return sweep_; }

 private:
  // Les états à peindre cette frame, déjà triés et tronqués.
  void Collect(std::vector<StatusEffects::Entry>* out) const;

  ro::HudRect rect_{40, 320, 240, 34};
  bool geometry_dirty_ = false;
};
