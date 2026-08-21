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
  // tests, parce qu'il y a deux mondes de saisie — la nôtre et celle du client
  // (chat, message privé, montant de vente…).
  if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard)
    return;
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
  if (pending_action_ < 0) return;
  const int index = pending_action_;
  pending_action_ = -1;
  // La fenêtre a pu se fermer entre la frappe et le tick (changement de mode) :
  // les gardes de Invoke suffisent, mais on ne joue rien hors du jeu.
  if (!Bourgeon::Instance().IsGameActive()) return;
  hotkeys::Invoke(hotkeys::ActionAt(index).id);
}
