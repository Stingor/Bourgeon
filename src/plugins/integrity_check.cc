#include "plugins/integrity_check.h"

#include <Windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "bourgeon.h"
#include "imgui.h"
#include "utils/log_console.h"

namespace {

// Full path of THIS module (the Bourgeon ddraw.dll), found from an address that
// lives inside it — no need to plumb the HINSTANCE through from DllMain.
bool SelfModulePath(std::wstring& out) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&SelfModulePath), &self)) {
    return false;
  }
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  out.assign(buf, n);
  return true;
}

bool Sha256(const uint8_t* data, size_t len, uint8_t* out, ULONG out_len) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) <
      0) {
    return false;
  }
  const NTSTATUS status =
      BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data),
                 static_cast<ULONG>(len), out, out_len);
  BCryptCloseAlgorithmProvider(alg, 0);
  return status >= 0;  // BCRYPT_SUCCESS
}

bool Sha256OfFile(const std::wstring& path, uint8_t* out, ULONG out_len) {
  std::ifstream f(path.c_str(), std::ios::binary);
  if (!f) return false;
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
  if (data.empty()) return false;
  return Sha256(data.data(), data.size(), out, out_len);
}

// Directory of the game executable, with a trailing backslash. Preferred over
// GetCurrentDirectory: the CWD can be anything if the client was started from a
// shortcut, whereas the patcher always sits next to the exe it launches.
bool GameDir(std::wstring& out) {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  std::wstring path(buf, n);
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) return false;
  out = path.substr(0, slash + 1);
  return true;
}

// Minimal extraction of an integer field from rpatchur's cache file. The file is
// a single flat JSON object written by serde ({"last_patch_index":42}), so a
// scan for the key beats pulling in a JSON parser for one number.
bool ReadJsonInt(const std::wstring& path, const char* key, int32_t& out) {
  std::ifstream f(path.c_str(), std::ios::binary);
  if (!f) return false;
  const std::string text((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
  const size_t at = text.find(key);
  if (at == std::string::npos) return false;
  size_t i = text.find(':', at + std::strlen(key));
  if (i == std::string::npos) return false;
  ++i;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
  int64_t value = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    value = value * 10 + (text[i] - '0');
    if (value > INT32_MAX) return false;  // corrupt file — treat as unknown
  }
  out = static_cast<int32_t>(value);
  return true;
}

}  // namespace

void IntegrityCheck::DiscoverPatcher() {
  std::wstring dir;
  if (!GameDir(dir)) {
    LogError("[Integrity] cannot resolve game directory — patcher discovery skipped");
    return;
  }

  // rpatchur names its config, cache and lock after its own executable stem, so
  // any `<stem>.yml` with a matching `<stem>.exe` identifies it.
  WIN32_FIND_DATAW fd;
  const HANDLE h = FindFirstFileW((dir + L"*.yml").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::wstring stem(fd.cFileName);
    const size_t dot = stem.find_last_of(L'.');
    if (dot == std::wstring::npos) continue;
    stem.resize(dot);

    const std::wstring exe = dir + stem + L".exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

    patcher_exe_ = exe;
    // Missing cache file is normal on a fresh install: the patcher only writes it
    // after applying its first patch. patch_index_ stays -1.
    ReadJsonInt(dir + stem + L".dat", "last_patch_index", patch_index_);
    break;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

void IntegrityCheck::LaunchPatcher() const {
  if (patcher_exe_.empty()) return;

  std::wstring dir;
  if (!GameDir(dir)) return;

  // ShellExecute rather than CreateProcess: rpatchur elevates itself via the
  // "runas" verb, and the shell handles the UAC prompt for us. The patcher only
  // starts downloading once the player clicks Update in its window, so it will
  // not race this process for the file locks on ddraw.dll and the GRF.
  SHELLEXECUTEINFOW sei = {};
  sei.cbSize       = sizeof(sei);
  sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb       = L"open";
  sei.lpFile       = patcher_exe_.c_str();
  sei.lpDirectory  = dir.c_str();
  sei.nShow        = SW_SHOWNORMAL;
  if (ShellExecuteExW(&sei)) {
    if (sei.hProcess) CloseHandle(sei.hProcess);
  } else {
    LogError("[Integrity] failed to launch the patcher (error {})", GetLastError());
  }
}

bool IntegrityCheck::ReadMachineGuid(char out[37]) {
  HKEY hKey = nullptr;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Microsoft\\Cryptography",
                    0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS) {
    return false;
  }
  DWORD size = 37;
  const LSTATUS status = RegQueryValueExA(hKey, "MachineGuid", nullptr, nullptr,
                                          reinterpret_cast<LPBYTE>(out), &size);
  RegCloseKey(hKey);
  if (status != ERROR_SUCCESS || size < kGuidLen) return false;
  out[kGuidLen] = '\0';
  return true;
}

bool IntegrityCheck::TryComputeHash() {
  std::wstring path;
  if (!SelfModulePath(path) || !Sha256OfFile(path, hash_, kHashLen))
    return false;
  have_hash_ = true;
  char hex[kHashLen * 2 + 1];
  for (int i = 0; i < kHashLen; ++i)
    std::snprintf(hex + i * 2, 3, "%02x", hash_[i]);
  // LogInfo("[Integrity] self SHA-256 {} computed (= hash du DLL charge ; comparer au build)", hex);
  return true;
}

IntegrityCheck::IntegrityCheck() {
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeKickNotice);
  // Observe map-entry so we re-handshake on every new zone session (e.g. after a
  // character change), not just once per process. Passive — the game's own
  // handler still runs.
  Bourgeon::Instance().RegisterObserveOpcode(kOpcodeAcceptEnter, 0);

  if (!TryComputeHash())
    LogError("[Integrity] failed to compute self checksum at startup — will retry on game entry");

  // Pure file I/O, safe under the loader lock (no LoadLibrary, no network).
  DiscoverPatcher();

  if (ReadMachineGuid(guid_)) {
    have_guid_ = true;
    // LogInfo("[Integrity] MachineGuid: {:.8s}...", guid_);
  } else {
    LogError("[Integrity] failed to read MachineGuid from registry");
  }
}

void IntegrityCheck::OnModeSwitch(ModeMgr::ModeType mode_type,
                                  const char* /*map*/) {
  if (mode_type == ModeMgr::ModeType::kLogin) {
    in_game_ = false;
    sent_    = false;
    return;
  }
  if (mode_type != ModeMgr::ModeType::kGame) return;
  in_game_ = true;

  if (sent_) return;
  // Retry hash if it failed at startup (e.g. DLL locked by patcher).
  if (!have_hash_ && !TryComputeHash()) return;
  if (SendChecksum())
    sent_ = true;
  // If SendPacket failed, sent_ stays false — OnTick will retry.
}

void IntegrityCheck::OnTick() {
  // Retry sending if OnModeSwitch's SendPacket failed (socket not ready yet).
  if (!in_game_ || sent_) return;
  if (!have_hash_ && !TryComputeHash()) return;
  if (SendChecksum())
    sent_ = true;
}

bool IntegrityCheck::SendChecksum() {
  uint8_t buf[4 + kHashLen + kGuidLen + 4];
  *reinterpret_cast<uint16_t*>(buf)     = kOpcodeToServer;
  *reinterpret_cast<uint16_t*>(buf + 2) = static_cast<uint16_t>(sizeof(buf));
  std::memcpy(buf + 4, hash_, kHashLen);
  if (have_guid_)
    std::memcpy(buf + 4 + kHashLen, guid_, kGuidLen);
  else
    std::memset(buf + 4 + kHashLen, 0, kGuidLen);
  *reinterpret_cast<int32_t*>(buf + 4 + kHashLen + kGuidLen) = patch_index_;
  const bool ok = Bourgeon::Instance().SendPacket(buf, sizeof(buf));
  if (ok)
    /* LogInfo("[Integrity] checksum + MachineGuid sent") */;
  else
    LogError("[Integrity] SendPacket failed — will retry on next tick");
  return ok;
}

void IntegrityCheck::OnRecvPacket(uint16_t opcode, const uint8_t* /*data*/,
                                  uint16_t /*len*/) {
  if (opcode == kOpcodeAcceptEnter) {
    // Entered a (new) zone-server session — re-arm so OnTick re-sends the
    // integrity handshake. Covers character changes, where the client does not
    // reliably re-fire a login->game mode switch but the server still expects a
    // fresh CZ_BOURGEON_INTEGRITY per session. We just received a packet on this
    // socket, so it is connected; the actual send happens on the next OnTick.
    in_game_ = true;
    sent_    = false;
    return;
  }
  if (opcode == kOpcodeKickNotice) {
    // LogInfo("[Integrity] kick-notice received — showing update popup");
    kick_notice_tick_ = static_cast<uint32_t>(GetTickCount());
    popup_pending_ = true;
  }
}

void IntegrityCheck::OnRenderUI() {
  if (popup_pending_) {
    ImGui::OpenPopup("Client Update Required");
    popup_pending_ = false;
  }

  if (kick_notice_tick_ != 0) {
    const uint32_t elapsed =
        static_cast<uint32_t>(GetTickCount()) - kick_notice_tick_;
    if (elapsed >= kKickDelayMs + 500) {
      // Server has kicked us by now; close the process rather than staying
      // frozen on the disconnected game screen. Hand the player straight over to
      // the patcher — it cannot rewrite ddraw.dll or the GRF until we are gone,
      // but it waits for a click on Update before touching anything.
      LaunchPatcher();
      ExitProcess(0);
    }
  }

  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Client Update Required", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoMove)) {
    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                       "Your game client is outdated!");
    ImGui::Spacing();
    if (patcher_exe_.empty()) {
      ImGui::TextWrapped(
          "Please close the game and run the patcher to update,\n"
          "then reconnect.");
    } else {
      ImGui::TextWrapped(
          "The patcher will open automatically once the game closes.\n"
          "Update, then reconnect.");
    }
    ImGui::Spacing();

    if (kick_notice_tick_ != 0) {
      const uint32_t elapsed =
          static_cast<uint32_t>(GetTickCount()) - kick_notice_tick_;
      const uint32_t remaining =
          elapsed < kKickDelayMs ? kKickDelayMs - elapsed : 0;
      const int secs = static_cast<int>((remaining + 999) / 1000);
      ImGui::TextDisabled("Closing in %d second%s...", secs,
                          secs == 1 ? "" : "s");
    } else {
      ImGui::TextDisabled("Disconnecting in a few seconds...");
    }

    ImGui::EndPopup();
  }
}
