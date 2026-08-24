#include "features/systems/cheat_detector.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include "bourgeon.h"
#include "imgui.h"
#include "ui/imgui_escape.h"
#include "ui/ro_imgui.h"  // ro::Px (échelle de l'interface, largeurs de colonnes)
#include "utils/log_console.h"
#include "utils/i18n.h"
#include "utils/text.h"  // text::ToLowerAscii / ContainsNoCase

// ── Signature table ───────────────────────────────────────────────────────────
// Patterns must be lowercase.
// kProcess      : exact match on szExeFile (lowercased).
// kProcessSubstr: substring match on szExeFile (lowercased) — use when the exe
//                 name has CPU-specific suffixes (e.g. cheatengine-x86_64-SSE4-AVX2).
// kWindowClass  : substring match on GetClassNameA result (lowercased).
// kModule       : substring match on module name injected in the current process.

struct Sig {
  CheatDetector::SigType type;
  const char*             tool_name;
  const char*             pattern;
};

static const Sig kSigs[] = {
    // AutoHotKey — macro / bot scripting, très commun sur RO
    { CheatDetector::SigType::kProcess,       "AutoHotKey",   "autohotkey.exe"   },
    { CheatDetector::SigType::kProcess,       "AutoHotKey",   "autohotkey64.exe" },
    { CheatDetector::SigType::kProcess,       "AutoHotKey",   "autohotkey32.exe" },
    { CheatDetector::SigType::kWindowClass,   "AutoHotKey",   "autohotkey"       },

    // 4rTools — suite coréenne bot/cheat RO
    { CheatDetector::SigType::kProcess,       "4rTools",      "4r.exe"           },
    { CheatDetector::SigType::kProcess,       "4rTools",      "4rtools.exe"      },
    { CheatDetector::SigType::kWindowClass,   "4rTools",      "4rtools"          },

    // WPE Pro — packet editor
    { CheatDetector::SigType::kProcess,       "WPE Pro",      "wpepro.exe"       },
    { CheatDetector::SigType::kProcess,       "WPE Pro",      "wpe pro.exe"      },

    // Cheat Engine — éditeur mémoire générique.
    // Le nom du process varie selon le CPU : cheatengine-x86_64-SSE4-AVX2.exe,
    // cheatengine-x86_64-SSE4.exe, etc.  → substring match sur "cheatengine".
    // Le launcher s'appelle "Cheat Engine.exe" → substring "cheat engine".
    { CheatDetector::SigType::kProcessSubstr, "Cheat Engine", "cheatengine"      },
    { CheatDetector::SigType::kProcessSubstr, "Cheat Engine", "cheat engine"     },
    // Injection optionnelle de CE dans le processus cible
    { CheatDetector::SigType::kModule,        "Cheat Engine", "cheatengine"      },

    // ArtMoney — memory scanner
    { CheatDetector::SigType::kProcess,       "ArtMoney",     "artmoney.exe"     },
    { CheatDetector::SigType::kProcess,       "ArtMoney",     "artmoneyse.exe"   },
};
static constexpr int kSigCount = static_cast<int>(std::size(kSigs));

// ── Helpers ───────────────────────────────────────────────────────────────────

// ── CheatDetector ─────────────────────────────────────────────────────────────

CheatDetector::CheatDetector() {
  last_scan_tick_ = 0;
  ui_enabled_ = (strstr(GetCommandLineA(), "--scancheat") != nullptr);
}

void CheatDetector::OnTick() {
  DrainScanResults();
  StartScanIfReady();
}

void CheatDetector::StartScanIfReady() {
  if (scan_busy_.load(std::memory_order_acquire)) return;  // scan in progress

  const DWORD now = GetTickCount();
  if (last_scan_tick_ != 0 && now - last_scan_tick_ < kScanIntervalMs) return;
  last_scan_tick_ = now;
  ++scan_count_;

  if (scan_thread_.joinable()) scan_thread_.join();  // clean up previous thread

  scan_results_.clear();
  scan_busy_.store(true, std::memory_order_release);
  scan_thread_ = std::thread([this]() { RunScanBackground(); });
}

void CheatDetector::DrainScanResults() {
  if (scan_busy_.load(std::memory_order_acquire)) return;
  if (!scan_thread_.joinable()) return;
  scan_thread_.join();

  // scan_results_ is now safe to read (scan_busy_ was released by the thread).
  for (auto& r : scan_results_)
    AddDetection(r.tool_name.c_str(), std::move(r.detail));
  scan_results_.clear();
}

void CheatDetector::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  const bool was_in_game = in_game_;
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);

  if (in_game_ && !was_in_game) {
    // Entering game: clear cached detections so the first scan re-reports
    // everything to the server now that the map-server connection exists.
    detections_.clear();
    last_scan_tick_ = 0;  // trigger scan on next tick
  }
}

void CheatDetector::RunScanBackground() {
  // Runs on the scan thread — must not call AddDetection or SendPacket.
  // Writes results to scan_results_, then clears scan_busy_ to signal done.
  std::vector<Detection> results;
  ScanProcesses(results);
  ScanWindows(results);
  ScanModules(results);
  scan_results_ = std::move(results);
  scan_busy_.store(false, std::memory_order_release);
}

// ── Process scan ──────────────────────────────────────────────────────────────

void CheatDetector::ScanProcesses(std::vector<Detection>& out) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;

  const DWORD now = GetTickCount();
  PROCESSENTRY32 pe = { sizeof(pe) };
  if (Process32First(snap, &pe)) {
    do {
      const std::string exe = text::ToLowerAscii(pe.szExeFile);
      for (int i = 0; i < kSigCount; ++i) {
        bool hit = false;
        if (kSigs[i].type == SigType::kProcess)
          hit = (exe == kSigs[i].pattern);
        else if (kSigs[i].type == SigType::kProcessSubstr)
          hit = (exe.find(kSigs[i].pattern) != std::string::npos);
        if (hit)
          out.push_back({ kSigs[i].tool_name, pe.szExeFile, now, now, 1 });
      }
    } while (Process32Next(snap, &pe));
  }

  CloseHandle(snap);
}

// ── Module scan (DLLs injectés dans le process courant) ───────────────────────

void CheatDetector::ScanModules(std::vector<Detection>& out) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return;

  const DWORD now = GetTickCount();
  MODULEENTRY32 me = { sizeof(me) };
  if (Module32First(snap, &me)) {
    do {
      const std::string mod = text::ToLowerAscii(me.szModule);
      for (int i = 0; i < kSigCount; ++i) {
        if (kSigs[i].type != SigType::kModule) continue;
        if (mod.find(kSigs[i].pattern) != std::string::npos)
          out.push_back({ kSigs[i].tool_name,
                          std::string(i18n::Tr("injected module: ")) + me.szModule,
                          now, now, 1 });
      }
    } while (Module32Next(snap, &me));
  }

  CloseHandle(snap);
}

// ── Window scan ───────────────────────────────────────────────────────────────

BOOL CALLBACK CheatDetector::WindowEnumProc(HWND hwnd, LPARAM lparam) {
  auto* out = reinterpret_cast<std::vector<Detection>*>(lparam);
  char cls[256] = {};
  GetClassNameA(hwnd, cls, sizeof(cls));
  const std::string cls_lower = text::ToLowerAscii(cls);
  const DWORD now = GetTickCount();
  for (int i = 0; i < kSigCount; ++i) {
    if (kSigs[i].type != SigType::kWindowClass) continue;
    if (cls_lower.find(kSigs[i].pattern) != std::string::npos)
      out->push_back({ kSigs[i].tool_name,
                       std::string(i18n::Tr("window class: ")) + cls,
                       now, now, 1 });
  }
  return TRUE;
}

void CheatDetector::ScanWindows(std::vector<Detection>& out) {
  EnumWindows(WindowEnumProc, reinterpret_cast<LPARAM>(&out));
}

// ── Detection storage ─────────────────────────────────────────────────────────

// Sent once per new detection — only when connected to the map server.
void CheatDetector::SendCheatReport(const char* tool_name, const std::string& detail) {
  if (!in_game_) return;
#pragma pack(push, 1)
  struct Pkt {
    uint16_t opcode;
    uint16_t len;
    char     tool_name[kToolNameLen];
    char     detail[kDetailLen];
  };
#pragma pack(pop)

  Pkt p = {};
  p.opcode = kOpcodeCheatReport;
  p.len    = static_cast<uint16_t>(sizeof(p));
  strncpy_s(p.tool_name, sizeof(p.tool_name), tool_name,    _TRUNCATE);
  strncpy_s(p.detail,    sizeof(p.detail),    detail.c_str(), _TRUNCATE);

  Bourgeon::Instance().SendPacket(reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}

void CheatDetector::AddDetection(const char* tool_name, std::string detail) {
  const DWORD now = GetTickCount();
  for (auto& d : detections_) {
    if (d.tool_name == tool_name && d.detail == detail) {
      d.last_seen_tick = now;
      ++d.seen_count;
      return;
    }
  }
  // New detection — log, report to server, then store.
  // LogInfo("[CheatDetector] detected: {} ({})", tool_name, detail);
  SendCheatReport(tool_name, detail);
  detections_.push_back({ tool_name, std::move(detail), now, now, 1 });
}

// ── ImGui window ──────────────────────────────────────────────────────────────

void CheatDetector::OnRenderUI() {
  if (!ui_enabled_ || !visible_) return;

  ImGui::SetNextWindowSize(ImVec2(460, 180), ImGuiCond_FirstUseEver);
  bool open = true;
  ImGui::Begin(i18n::Tr("Cheat Detector"), &open);
  bourgeon::CloseWindowOnEscape(open);
  if (!open) { visible_ = false; ImGui::End(); return; }

  const DWORD now   = GetTickCount();
  const DWORD since = (last_scan_tick_ > 0) ? (now - last_scan_tick_) / 1000 : 0;
  ImGui::Text(i18n::Tr("Scans: %d  |  Dernier: il y a %us"), scan_count_, since);
  ImGui::SameLine();
  if (ro::RoSmallButton(i18n::Tr("Scanner")) && !scan_busy_.load()) {
    last_scan_tick_ = 0;  // force next StartScanIfReady to launch immediately
    StartScanIfReady();
  }

  ImGui::Separator();

  if (detections_.empty()) {
    ImGui::TextDisabled(i18n::Tr("Aucune detection"));
  } else {
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders
                                | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_SizingFixedFit
                                | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##det", 3, flags,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
      ImGui::TableSetupColumn(i18n::Tr("Outil"),  ImGuiTableColumnFlags_WidthFixed,  ro::Px(110.0f));
      ImGui::TableSetupColumn(i18n::Tr("Detail"), ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn(i18n::Tr("Vu"),     ImGuiTableColumnFlags_WidthFixed,   ro::Px(80.0f));
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableHeadersRow();

      for (const auto& d : detections_) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "%s", d.tool_name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(d.detail.c_str());
        ImGui::TableSetColumnIndex(2);
        const DWORD age = (now - d.last_seen_tick) / 1000;
        ImGui::Text(i18n::Tr("%dx  (%us)"), d.seen_count, age);
      }

      ImGui::EndTable();
    }
  }

  ImGui::End();
}
