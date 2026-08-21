#include "ui_window_mgr.h"

#include "bourgeon.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// Pointer to the game's UIWindowMgr singleton instance
std::atomic<UIWindowMgr*> UIWindowMgr::g_uiwindowmgr_ptr(nullptr);

UIWindowMgr::UIWindowMgr(const YAML::Node& uiwindowmgr_configuration) {
  using namespace hooking;

  // Hooks
  const auto uiwindowmgr_addr = uiwindowmgr_configuration["UIWindowMgr"];
  if (!uiwindowmgr_addr.IsDefined()) {
    throw std::exception(
        "Missing required field 'UIWindowMgr' for UIWindowMgr");
  }
  UIWindowMgr::UIWindowMgrRef = HookManager::Instance().SetHook(
      HookType::kJmpHook,
      reinterpret_cast<uint8_t*>(uiwindowmgr_addr.as<uint32_t>()),
      reinterpret_cast<uint8_t*>(void_cast(&UIWindowMgr::UIWindowMgrHook)));

  const auto processpushbtn_addr =
      uiwindowmgr_configuration["ProcessPushButton"];
  if (!processpushbtn_addr.IsDefined()) {
    throw std::exception(
        "Missing required field 'ProcessPushButton' for UIWindowMgr");
  }
  UIWindowMgr::ProcessPushButtonRef = HookManager::Instance().SetHook(
      HookType::kJmpHook,
      reinterpret_cast<uint8_t*>(processpushbtn_addr.as<uint32_t>()),
      reinterpret_cast<uint8_t*>(
          void_cast(&UIWindowMgr::ProcessPushButtonHook)));

  const auto sendmsg_addr = uiwindowmgr_configuration["SendMsg"];
  if (!sendmsg_addr.IsDefined()) {
    throw std::exception("Missing required field 'SendMsg' for UIWindowMgr");
  }
  UIWindowMgr::SendMsgRef = HookManager::Instance().SetHook(
      HookType::kJmpHook,
      reinterpret_cast<uint8_t*>(sendmsg_addr.as<uint32_t>()),
      reinterpret_cast<uint8_t*>(void_cast(&UIWindowMgr::SendMsgHook)));
}

bool UIWindowMgr::ProcessPushButton(unsigned long vkey, int new_key,
                                    int accurate_key) {
  return ProcessPushButtonRef(g_uiwindowmgr_ptr.load(), vkey, new_key,
                              accurate_key);
}

size_t UIWindowMgr::SendMsg(UIMessage message, int val1, int val2, int val3,
                            int val4) {
  // 🔴 CE CHEMIN-CI EST CELUI DE NOS PLUGINS, et il NE PASSE PAS par SendMsgHook :
  // il appelle `SendMsgRef`, le trampoline vers la fonction native d'origine. Le
  // hook n'intercepte que les appels du JEU. Aiguiller là-bas ne pouvait donc rien
  // changer pour le relais Discord ni le DPS meter, qui appellent ici.
  //
  // C'est aussi ce qui rend la panne si discrète : nos lignes n'ont jamais croisé
  // le moindre code à nous entre l'envoi et la chatbox native — désormais morte.
  if (message == UIMessage::UIM_PUSHINTOCHATHISTORY &&
      Bourgeon::Instance().RouteChatLine(reinterpret_cast<const char*>(val1),
                                         static_cast<uint32_t>(val2))) {
    return 0;  // prise par la chatbox ImGui
  }
  return SendMsgRef(g_uiwindowmgr_ptr.load(), static_cast<int>(message), val1,
                    val2, val3, val4);
}

void UIWindowMgr::UIWindowMgrHook() {
  LogDebug("UIWindowMgr: 0x{:x}", reinterpret_cast<uintptr_t>(this));
  g_uiwindowmgr_ptr.store(this);
  UIWindowMgrRef(this);
}

bool UIWindowMgr::ProcessPushButtonHook(unsigned long vkey, int new_key,
                                        int accurate_key) {
  // Lock the keyboard while a map is loading: swallow the key without running the
  // native handler (which routes hotkeys -> DispatchHotkeyBehavior -> skill cast /
  // window open) or Bourgeon's own hotkeys. Acting mid-transition dereferences a
  // not-yet-rebuilt world (e.g. the F-key skill-cast NULL deref) or opens a window
  // during the HUD churn. Return true = "handled/consumed" so the key is dropped.
  if (Bourgeon::Instance().IsMapLoading()) return true;

  // 🔴 REMAPPAGE EN COURS : la frappe sert à CHOISIR une touche, pas à agir. On
  // l'avale ici — avant `FireKeyDown` (nos plugins) comme avant le handler natif,
  // qui route les raccourcis du CLIENT vers `DispatchHotkeyBehavior`. Sans ça,
  // choisir Alt+D pour un raccourci l'affectait ET ouvrait la tipbox du jeu au
  // passage (constaté en jeu le 2026-08-21).
  //
  // C'est le devoir que la fenêtre native remplissait seule : tant que 0x9C vit,
  // `UIWindowMgr_OnKeyDown` détourne TOUT le clavier vers elle. Nous la
  // détruisons, donc il nous revient — et ce hook est le dernier endroit d'où on
  // puisse encore couper le dispatch du jeu. ImGui, lui, reçoit la frappe par le
  // WndProc du backend : la capture n'en est pas privée.
  if (Bourgeon::Instance().IsHotkeyCaptureActive()) return true;

  Bourgeon::Instance().FireKeyDown(vkey, new_key, accurate_key);

  // 🔴 UNE ACTION DE BOURGEON A PRIS LA FRAPPE : le client ne doit pas la voir.
  // Sans ce test, une touche liée à une action partait AUSSI vers
  // `DispatchHotkeyBehavior` — invisible tant que la touche n'était prise par
  // rien d'autre chez le client, criant dès qu'elle l'était (Alt+D ouvrait la
  // tipbox en plus de l'action). Le contrôle de collision ne pouvait pas
  // l'empêcher : il ne voit que les commandes REMAPPABLES, et le client en garde
  // douze qui ne le sont pas.
  if (Bourgeon::Instance().TakeHotkeyActionClaim()) return true;

  return ProcessPushButtonRef(this, vkey, new_key, accurate_key);
}

size_t UIWindowMgr::SendMsgHook(UIMessage message, int val1, int val2, int val3,
                                int val4) {
  if (message != UIMessage::UIM_PUSHINTOCHATHISTORY)
    return SendMsgRef(this, static_cast<int>(message), val1, val2, val3, val4);

  const char* text = reinterpret_cast<const char*>(val1);
  Bourgeon::Instance().FireChatMessage(text);

  // 🔴 SEULE VOIE par laquelle Bourgeon écrit dans le chat (relais Discord, DPS
  // meter), et elle s'adresse à la chatbox NATIVE. Quand la chatbox ImGui l'a
  // détruite, ce message ne mène plus nulle part : la ligne s'empile dans la file
  // `mgr+0x4C4`, drainée à la seule création d'une fenêtre — donc jamais. Nos deux
  // plugins parlaient dans le vide, sans la moindre erreur pour le dire.
  //
  // ⚠ Et surtout : cette voie NE PASSE PAS par `ChatAction` (mesuré en jeu — les
  // lignes n'arrivaient qu'en source 'W', jamais 'A'). Le détour qui rattrape tout
  // le reste ne pouvait donc pas rattraper celle-ci.
  if (Bourgeon::Instance().RouteChatLine(text, static_cast<uint32_t>(val2)))
    return 0;  // prise par la chatbox ImGui : ne pas nourrir la file du natif

  return SendMsgRef(this, static_cast<int>(message), val1, val2, val3, val4);
}

// References
MethodRef<UIWindowMgr, void (UIWindowMgr::*)()> UIWindowMgr::UIWindowMgrRef;

MethodRef<UIWindowMgr, bool (UIWindowMgr::*)(unsigned long vkey, int new_key,
                                             int accurate_key)>
    UIWindowMgr::ProcessPushButtonRef;

MethodRef<UIWindowMgr, size_t (UIWindowMgr::*)(int message, int val1, int val2,
                                               int val3, int val4)>
    UIWindowMgr::SendMsgRef;
