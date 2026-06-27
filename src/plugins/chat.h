#pragma once

#include "plugins/plugin.h"

// Chat-window client-side tweaks (20250716 client): native item icons on
// <ITEML> links + a custom main-chat width (the stock chat resizes height only).
// This plugin owns the engine HOOKS + the apply mechanism; the settings UI and
// persistence live in MoonlightUi's chat section, which drives it through
// chat::SetCustomWidth.  Compiled into the DLL; register in LoadPlugins().
class ChatTweaks : public Plugin {
 public:
  ChatTweaks();

  const char* name() const override { return "Chat"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
};

namespace chat {
// Set/clear a custom width for the main chat window (px clamped 320..1200).
// Driven by the settings UI in MoonlightUi.  Applies live if the chat exists;
// otherwise the WndProc hook enforces it on the next in-game relayout.
void SetCustomWidth(bool enabled, int px);

// Enable/disable a [HH:MM:SS] timestamp prefix on every new chat line.  Stored
// into the chat's raw history, so it persists across re-wraps/resizes.  Driven
// by the settings UI in MoonlightUi.
void SetTimestamps(bool enabled);

// Enable/disable the native item icons on <ITEML> chat links.  Driven by the
// settings UI in MoonlightUi.
void SetItemIcons(bool enabled);

// Clear the main chat window's history: empties every channel tab's raw-history
// vectors and re-wraps to a blank display.  Safe no-op if the chat isn't live.
// Driven by the "Effacer l'historique" button in MoonlightUi's chat section.
void ClearHistory();
}  // namespace chat
