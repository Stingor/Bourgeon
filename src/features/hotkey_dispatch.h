#pragma once

#include "features/plugin.h"

// ── HotkeyDispatch : la touche liée déclenche l'action ───────────────────────
//
// Pendant d'exécution du catalogue (features/hotkey_actions.h) et de l'écran de
// réglage (features/windows/hotkey_settings.h). Il ne porte aucun réglage : une
// action ne se déclenche que si le joueur lui a donné une touche, et le
// catalogue n'en propose aucune par défaut.
//
// 🔴 LA FRAPPE LIÉE EST CONFISQUÉE AU CLIENT (`hotkeys::ClaimKey`), et il a fallu
// un bug pour le comprendre. Il était écrit ici qu'avaler était « structurellement
// impossible » parce que `OnKeyDown` est alimenté par notre hook de
// `ProcessPushButton`, donc DEPUIS le jeu. C'est justement l'inverse : ce hook
// décide d'appeler ou non le handler natif, et lui rendre `true` avale la touche.
// Nous, nous sommes servis AVANT — aucun risque d'affamer notre propre handler
// (feedback_swallowed_key_starves_own_handler ne vise pas ce sens-là).
//
// Il était aussi écrit que le contrôle de collision suffisait, aucune frappe ne
// pouvant nourrir les deux mondes. Faux : il ne voit que les commandes
// REMAPPABLES du client, et `UserHotkey_RowToCommandIndex` en SAUTE douze pour la
// catégorie Interface. Alt+D, la tipbox, en fait partie — l'affecter à une action
// était accepté sans un mot, et les deux partaient ensemble à chaque appui
// (remonté en jeu le 2026-08-21).
//
// ⚠ LE RÉSIDU qui demeure, plus étroit : le contrôle de collision est fait au
// MOMENT DU CHOIX. Si les raccourcis du client changent ensuite par un autre
// chemin — le [Reset] de la fenêtre native, une configuration poussée au login —
// une commande du jeu peut retomber sur une touche déjà prise par une action.
// C'est alors l'ACTION qui gagne, la commande du jeu ne partant plus ; le remède
// reste de rouvrir l'écran des raccourcis, qui montre les deux mondes côte à côte.
//
// L'exécution est DIFFÉRÉE au tick : la plupart des actions ouvrent une fenêtre
// par `MakeWindow`, et une commande native lancée pendant une frame ImGui gèle le
// client sans un mot (feedback_no_native_cmd_during_imgui_frame).

class HotkeyDispatch : public Plugin {
 public:
  const char* name() const override { return "HotkeyDispatch"; }

  void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) override;
  void OnTick() override;

 private:
  // Index de l'action à exécuter au prochain tick, -1 si aucune. Un seul créneau :
  // deux actions dans la même frame voudrait dire deux touches pressées en même
  // temps, ce qui n'arrive pas — et si cela arrivait, en jouer une suffit.
  int pending_action_ = -1;
};
