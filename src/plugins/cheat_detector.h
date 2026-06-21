#pragma once

#include <Windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "plugins/plugin.h"

// Scans the local machine for known cheat tools (AHK, 4rTools, WPE, CE, …).
// Detection is purely indicative — no enforcement, no server report in v1.
// Runs a full scan every 30 s via OnTick; results are shown in an ImGui table.
class CheatDetector : public Plugin {
 public:
  CheatDetector();

  const char* name() const override { return "Cheat Detector"; }

  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRenderUI() override;

  bool visible_ = true;

  // True only when the process was launched with --scancheat.
  // Detections always run; only the UI is gated.
  bool ui_enabled_ = false;

  enum class SigType { kProcess, kProcessSubstr, kWindowClass, kModule };

 private:
  static constexpr DWORD    kScanIntervalMs  = 30'000;
  static constexpr uint16_t kOpcodeCheatReport = 0x0C23;
  static constexpr size_t   kToolNameLen      = 32;
  static constexpr size_t   kDetailLen        = 64;

  struct Detection {
    std::string tool_name;
    std::string detail;       // e.g. process name or window class
    DWORD       first_seen_tick;
    DWORD       last_seen_tick;
    int         seen_count;
  };

  // Launches the scan in a background thread if none is running.
  void StartScanIfReady();
  // Called by OnTick on the main thread when the background scan completes.
  void DrainScanResults();

  // Background scan — runs on scan_thread_, touches only scan_results_.
  void RunScanBackground();
  void ScanProcesses(std::vector<Detection>& out);
  void ScanWindows(std::vector<Detection>& out);
  void ScanModules(std::vector<Detection>& out);

  void AddDetection(const char* tool_name, std::string detail);
  void SendCheatReport(const char* tool_name, const std::string& detail);

  static BOOL CALLBACK WindowEnumProc(HWND hwnd, LPARAM lparam);

  std::vector<Detection> detections_;
  DWORD last_scan_tick_ = 0;
  int   scan_count_     = 0;
  bool  in_game_        = false;

  // Background scan state — only OnTick (main thread) accesses scan_thread_
  // and scan_busy_; scan_results_ is written by the scan thread and read by
  // the main thread only after scan_busy_ is false (acquire).
  std::thread           scan_thread_;
  std::atomic<bool>     scan_busy_{false};
  std::vector<Detection> scan_results_;  // produced by scan thread
};
