#pragma once

#include "features/plugin.h"

// ── HotkeyDispatch : la touche liée déclenche l'action ───────────────────────
//
// Pendant d'exécution du catalogue (features/hotkey_actions.h) et de l'écran de
// réglage (features/windows/hotkey_settings.h). Il ne porte aucun réglage : une
// action ne se déclenche que si le joueur lui a donné une touche, et le
// catalogue n'en propose aucune par défaut.
//
// 🔴 IL N'Y A RIEN À « AVALER », ET C'EST STRUCTUREL. `OnKeyDown` n'écoute pas le
// WndProc : il est alimenté par notre hook de `ProcessPushButton`, donc DEPUIS le
// jeu (feedback_swallowed_key_starves_own_handler). Confisquer la frappe y est
// impossible — et inutile : le contrôle de collision refuse déjà à une action
// toute touche qu'une commande du client utilise, si bien qu'aucune frappe ne
// peut nourrir les deux mondes à la fois.
//
// ⚠ LE RÉSIDU, qu'il faut connaître : ce contrôle est fait au MOMENT DU CHOIX.
// Si les raccourcis du client changent ensuite par un autre chemin — le [Reset]
// de la fenêtre native, ou une configuration que le serveur pousse au login —
// une commande du jeu peut retomber sur une touche déjà prise par une action. Les
// deux partiront alors ensemble. Le remède est le même que pour le natif :
// rouvrir l'écran des raccourcis, qui montre les deux mondes côte à côte.
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
