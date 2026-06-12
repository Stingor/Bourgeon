#pragma once

#include "ragnarok/mode_mgr.h"

// Base class for Bourgeon plugins. Plugins are written in C++ and compiled
// into the DLL; there is intentionally no runtime scripting or external
// plugin loading. To add a plugin, subclass Plugin, override the events you
// need and register an instance in Bourgeon::LoadPlugins().
//
// All events are invoked from the game's main thread.
class Plugin {
 public:
  virtual ~Plugin() = default;

  virtual const char* name() const = 0;

  // Fired roughly every 100ms while the client runs its update loop.
  virtual void OnTick() {}

  // Fired when the client switches modes (login <-> game).
  virtual void OnModeSwitch(ModeMgr::ModeType mode_type,
                            const char* map_name) {}

  // Fired when the user submits text in the chat box.
  virtual void OnTalkType(const char* chat_buffer) {}

  // Fired when a message is pushed into the chat history.
  virtual void OnChatMessage(const char* chat_buffer) {}

  // Fired when the game processes WM_KEYDOWN/WM_SYSKEYDOWN.
  virtual void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) {}

  // Fired every frame between ImGui::NewFrame() and ImGui::Render();
  // draw plugin windows here using the ImGui API directly.
  virtual void OnRenderUI() {}
};
