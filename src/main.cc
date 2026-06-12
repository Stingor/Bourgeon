#include <Windows.h>

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"
#include "ddraw/ddraw.h"
#include "ddraw/proxy_idirectdraw.h"

BOOL WINAPI DllMain(HINSTANCE hinst_dll, DWORD fdw_reason,
                    LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(hinst_dll);

      // Load the original ddraw DLL
      if (!LoadDDraw()) {
        return FALSE;
      }

      // Always install rendering hooks so the ImGui overlay works regardless of
      // whether the current client is supported by Bourgeon's game logic.
      InitDX7Hook();
      InitD3D9Hook();

      // Initialize Bourgeon (Create hooks and load plugins).
      // On failure, stay in memory so the game can still start.
      Bourgeon::Instance().Initialize();

      break;
    case DLL_PROCESS_DETACH:
      if (lpv_reserved != nullptr) {
        // Process is terminating — forcefully exit before C++ destructors
        // (hooks, background threads) run on partially-freed memory.
        TerminateProcess(GetCurrentProcess(), 0);
      }
      FreeDDraw();
      break;
    default:
      break;
  };

  return TRUE;
}
