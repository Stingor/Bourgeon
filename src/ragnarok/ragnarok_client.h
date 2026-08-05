#pragma once

#include <string>

#include "ragnarok/game_mode.h"
#include "ragnarok/login_mode.h"
#include "ragnarok/mode_mgr.h"
#include "ragnarok/rag_connection.h"
#include "ragnarok/session.h"
#include "ragnarok/ui_window_mgr.h"
#include "yaml-cpp/yaml.h"

class RagnarokClient {
 public:
  static const uint32_t kUnknownTimeStamp = 0;

  RagnarokClient();
  ~RagnarokClient();

  bool Initialize();

  uint32_t timestamp() const;
  Session& session() const;
  RagConnection& rag_connection() const;
  UIWindowMgr& window_mgr() const;

  // High level methods implemented by the client
  // 🔴 FAIT PLANTER LE CLIENT 20250716 — NE PAS APPELER (cf. Session::
  // GetItemInfoById, dont l'offset de liste est faux sur ce build). Le paquet
  // qu'elle compose est juste ; c'est la résolution de l'index qui plante.
  // Pour ré-utiliser un objet : résoudre l'index sur le global d'inventaire
  // 0x015FBAB0 puis envoyer soi-même (exemple dans make_item_window.cc).
  bool UseItemById(int item_id) const;

  // Raw HWND of the main game window, captured when the client creates it.
  // Returned as void* so this header doesn't need to pull in <Windows.h>;
  // callers cast back to HWND. Null until the window exists.
  static void* GameWindow();

  // Poste une frappe (WM_KEYDOWN + WM_KEYUP) DESTINÉE AU CLIENT NATIF, marquée
  // comme venant de nous. Le hook de WndProc reconnaît la marque et la route
  // droit au jeu : elle traverse la capture clavier d'ImGui, et ImGui ne la voit
  // même pas.
  //
  // 🔴 C'est le SEUL moyen de piloter un écran natif au clavier pendant qu'on
  // confisque la touche au joueur (parcours de login moderne : voir
  // MoonlightAuth::WantsKeyboard). Un PostMessage nu serait soit avalé par la
  // capture, soit — pire — traité par notre propre UI comme une frappe du joueur
  // (l'auto-confirm du char-server déclenchait alors une entrée en jeu).
  static void PostGameKey(int vkey);

 private:
  static YAML::Node LoadConfiguration();
  static uint32_t GetClientTimeStamp();
  static void* GetClientBase();
  static uint32_t ConvertClientTimestamp(uint32_t timestamp);
  static bool SetupImgui();

  uint32_t timestamp_;
  Session::Pointer session_;
  RagConnection::Pointer rag_connection_;
  UIWindowMgr::Pointer window_mgr_;
  ModeMgr::Pointer mode_mgr_;
  LoginMode::Pointer login_mode_;
  GameMode::Pointer game_mode_;
};
