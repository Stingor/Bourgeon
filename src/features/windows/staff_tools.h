#pragma once

#include "features/plugin.h"

// ── StaffTools : les outils du staff, dans leur propre fenêtre ───────────────
//
// Ils vivaient sous un en-tête repliable du panneau de réglages, coincés entre
// les règles du serveur et le DPS meter. Deux raisons de les en sortir :
//
//   - ce ne sont PAS des réglages de confort. Renommer l'affichage des noms,
//     repeindre le sol pour une capture, filmer une zone en GIF, lire le journal
//     du client : ce sont des outils de travail, qu'on ouvre pour s'en servir et
//     qu'on referme. Les noyer dans une liste de préférences joueur les rendait
//     longs à atteindre et faciles à ouvrir par accident ;
//   - le panneau de réglages est désormais FERMABLE. Y laisser les outils du
//     staff aurait lié leur disponibilité à celle d'une fenêtre que le staff
//     ferme comme tout le monde.
//
// 🔴 GATÉ SUR LE NIVEAU DE GROUPE SERVEUR (IsStaff, reçu au login), et le droit
// est revérifié À CHAQUE FRAME : la fenêtre disparaît d'elle-même si le niveau
// change en cours de session. C'est la même règle que la fenêtre de journal, et
// elle vaut aussi pour l'état persisté — un `staff_tools_open` à `true` dans le
// yaml d'un joueur ordinaire n'ouvre rien.
//
// Le menu Échap est son chemin d'ouverture (bouton visible pour le staff seul).

class StaffTools : public Plugin {
 public:
  const char* name() const override { return "StaffTools"; }

  void OnRenderUI() override;

  void Open();
  void Toggle();
  bool IsOpen() const { return open_; }

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, via MoonlightUi) ─────────
  // « staff_tools_open » : pour qui s'en sert comme établi, rouvrir la fenêtre à
  // chaque lancement serait une corvée quotidienne — même raison que la fenêtre
  // de journal.
  bool open_ = false;

  // « korean_glyphs » — MÊME CLÉ qu'avant l'extraction, pour ne pas perdre le
  // réglage des yaml déjà en place.
  //
  // ⚠ Ce booléen n'est qu'un MIROIR persistable : la valeur qui compte est celle
  // que `ro::KoreanGlyphsWanted()` lit directement dans le yaml à l'initialisation
  // des polices, bien avant que les plugins n'existent. L'écrire ici ne change
  // donc rien avant le prochain lancement, ce que dit l'infobulle.
  bool korean_glyphs_ = false;

 private:
  bool need_pos_    = false;
  bool show_panel_  = true;
};
