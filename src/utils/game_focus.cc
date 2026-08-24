#include "utils/game_focus.h"

#include <Windows.h>

namespace win {

bool GameHasFocus() {
  const HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

}  // namespace win
