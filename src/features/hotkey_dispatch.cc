#include "features/hotkey_dispatch.h"

#include <Windows.h>

#include "bourgeon.h"
#include "features/hotkey_actions.h"
#include "features/hotkey_util.h"
#include "imgui.h"

void HotkeyDispatch::OnKeyDown(unsigned long vkey, int, int) {
  if (vkey == 0) return;

  // Un remappage est en cours quelque part : la touche sert à CHOISIR le
  // raccourci, pas à le déclencher. Drapeau PARTAGÉ, qui expire de lui-même —
  // un panneau replié en pleine capture ne peut donc pas tuer les raccourcis
  // jusqu'au redémarrage.
  if (hotkeys::CaptureInProgress()) return;

  Bourgeon& bourgeon = Bourgeon::Instance();
  if (!bourgeon.IsGameActive()) return;
  if (bourgeon.IsMapLoading()) return;

  // Le clavier appartient à une saisie : la frappe est un caractère. Les deux
  // mondes de saisie sont testés — la nôtre et celle du client (chat, message
  // privé, montant de vente…).
  //
  // 🔴 SURTOUT PAS `WantCaptureKeyboard` : ImGui le lève dès qu'un widget est
  // ACTIF, et un GLISSER d'objet EST un widget actif —
  //     if ((g.ActiveId != 0) || (modal_window != NULL))
  //         io.WantCaptureKeyboard = true;            (imgui.cpp)
  // Résultat : dès qu'on avait attrapé un item dans l'inventaire, TOUS les
  // raccourcis tombaient, dont l'émote portant « @storage ». Le client natif
  // l'acceptait, et c'est justement quand on tient quelque chose qu'on veut
  // ouvrir l'entrepôt.
  //
  // Ce qui doit vraiment nous faire taire est plus étroit : une SAISIE (la
  // frappe est un caractère) ou une MODALE (le joueur doit répondre avant
  // tout). Un bouton enfoncé, un slider tiré, un objet glissé ne sont ni l'un
  // ni l'autre.
  if (ImGui::GetCurrentContext() != nullptr) {
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                        ImGuiPopupFlags_AnyPopupLevel))
      return;
  }
  if (hotkeys::NativeTextInputHasFocus()) return;

  // ⚠ `ProcessPushButton` ne transmet que la touche PRINCIPALE : les
  // modificateurs se lisent dans l'état clavier du message en cours (GetKeyState,
  // surtout pas GetAsyncKeyState, qui répondrait pour l'instant présent).
  const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

  for (int i = 0; i < hotkeys::ActionCount(); ++i) {
    if (!hotkeys::BindingAt(i).Matches(static_cast<int>(vkey), ctrl, alt, shift))
      continue;
    // 🔴 Action « rendue à ImGui » (le cycleur de fenêtres) : elle n'a PAS de
    // créneau ici, parce qu'il n'y a rien à exécuter — ImGui a déjà reçu la même
    // frappe par le WndProc, qui est un chemin SÉPARÉ de `ProcessPushButton`. On
    // confisque quand même la touche au client, comme pour n'importe quelle autre
    // liaison : c'est ce qui la lui retire vraiment.
    if (hotkeys::ActionAt(i).imgui_windowing) {
      hotkeys::ClaimKey();
      return;
    }
    pending_action_ = i;
    // 🔴 ET ON CONFISQUE LA FRAPPE : le hook qui nous l'a passée s'apprête à
    // appeler le handler natif, qui la routerait vers `DispatchHotkeyBehavior`.
    // Le contrôle de collision ne voit que les commandes REMAPPABLES du client ;
    // les autres (la tipbox d'Alt+D, par exemple) partaient donc EN MÊME TEMPS
    // que l'action — constaté en jeu le 2026-08-21.
    hotkeys::ClaimKey();
    return;
  }
}

void HotkeyDispatch::OnTick() {
  // Le combo du cycleur de fenêtres est REPOUSSÉ dans ImGui, pas dispatché ici.
  // Au tick plutôt qu'au seul moment du réglage : la lecture du yaml court avant
  // qu'ImGui existe, et un point unique vaut mieux que trois appelants à ne pas
  // oublier. Deux écritures tous les ~100 ms, dont on ne paie rien.
  hotkeys::ApplyImGuiWindowingChord();

  if (pending_action_ < 0) return;
  const int index = pending_action_;
  pending_action_ = -1;
  // La fenêtre a pu se fermer entre la frappe et le tick (changement de mode) :
  // les gardes de Invoke suffisent, mais on ne joue rien hors du jeu.
  if (!Bourgeon::Instance().IsGameActive()) return;
  hotkeys::Invoke(hotkeys::ActionAt(index).id);
}
