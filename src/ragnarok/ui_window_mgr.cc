#include "ui_window_mgr.h"

#include "bourgeon.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// Pointer to the game's UIWindowMgr singleton instance
std::atomic<UIWindowMgr*> UIWindowMgr::g_uiwindowmgr_ptr(nullptr);

// 🔴 Le client 2026-07-07 a ajoute un 6e argument a UIWindowMgr_ChatAction :
// `retn 18h` contre `retn 14h`. Mesure : le parametre s'ajoute EN FIN de liste.
// Le meme appelant (PostActorClickAction, paire etablie) pousse `0,0,0,0,0,3` la
// ou le 2025 pousse `0,0,0,0,3` ; et sur 150 sites d'appel, le pic des appels
// complets passe de 5 a 6 push, avec un biais de mesure identique des deux cotes
// (le pic parasite a 2 push vaut 57 % ici comme la-bas).
//
// Le hook membre a 5 arguments depilerait 20 octets la ou le natif en depile 24 :
// pile corrompue des le premier appel. Quand la configuration annonce
// `SendMsgArgs: 6`, on installe donc un hook de signature exacte.
//
// __fastcall avec ecx/edx en registres et TOUS les arguments sur la pile emule un
// __thiscall : `this` arrive en ecx, edx n'est pas lu par le natif, et les six
// parametres pile font bien RET 0x18. C'est le patron de ProcessInputArgs dans
// game_mode.cc.
using SendMsg6_t = size_t(__fastcall*)(void* ecx, void* edx, int message,
                                       int val1, int val2, int val3, int val4,
                                       int val5);
static SendMsg6_t g_orig_send_msg6 = nullptr;

static size_t __fastcall Hooked_SendMsg6(void* ecx, void* edx, int message,
                                         int val1, int val2, int val3,
                                         int val4, int val5) {
  if (message != static_cast<int>(UIMessage::UIM_PUSHINTOCHATHISTORY))
    return g_orig_send_msg6(ecx, edx, message, val1, val2, val3, val4, val5);

  const char* text = reinterpret_cast<const char*>(val1);
  Bourgeon::Instance().FireChatMessage(text);

  if (Bourgeon::Instance().RouteChatLine(text, static_cast<uint32_t>(val2)))
    return 0;  // prise par la chatbox ImGui : ne pas nourrir la file du natif

  return g_orig_send_msg6(ecx, edx, message, val1, val2, val3, val4, val5);
}

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
  // Signature a 6 arguments : client 2026 seulement (cf. le commentaire du hook
  // ci-dessus). Absent de l'entree 20250716, qui prend donc la branche `else`.
  const auto sendmsg_args = uiwindowmgr_configuration["SendMsgArgs"];
  if (sendmsg_args.IsDefined() && sendmsg_args.as<int>() == 6) {
    g_orig_send_msg6 =
        reinterpret_cast<SendMsg6_t>(HookManager::Instance().SetHook(
            HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(sendmsg_addr.as<uint32_t>()),
            reinterpret_cast<uint8_t*>(Hooked_SendMsg6)));
  } else {
    UIWindowMgr::SendMsgRef = HookManager::Instance().SetHook(
        HookType::kJmpHook,
        reinterpret_cast<uint8_t*>(sendmsg_addr.as<uint32_t>()),
        reinterpret_cast<uint8_t*>(void_cast(&UIWindowMgr::SendMsgHook)));
  }
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
  // Sur le client 2026 le natif attend six arguments : passer par le trampoline
  // a cinq laisserait 4 octets sur la pile a chaque ligne ecrite par un plugin.
  // Le 6e vaut 0 sur tous les sites d'appel releves dans le client.
  if (g_orig_send_msg6 != nullptr) {
    return g_orig_send_msg6(g_uiwindowmgr_ptr.load(), nullptr,
                            static_cast<int>(message), val1, val2, val3, val4,
                            0);
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
