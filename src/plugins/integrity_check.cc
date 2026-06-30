#include "plugins/integrity_check.h"

#include <Windows.h>
#include <bcrypt.h>

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

}  // namespace

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
  LogInfo("[Integrity] self SHA-256 {} computed (= hash du DLL charge ; comparer au build)", hex);
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

  if (ReadMachineGuid(guid_)) {
    have_guid_ = true;
    LogInfo("[Integrity] MachineGuid: {:.8s}...", guid_);
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
  uint8_t buf[4 + kHashLen + kGuidLen];
  *reinterpret_cast<uint16_t*>(buf)     = kOpcodeToServer;
  *reinterpret_cast<uint16_t*>(buf + 2) = static_cast<uint16_t>(sizeof(buf));
  std::memcpy(buf + 4, hash_, kHashLen);
  if (have_guid_)
    std::memcpy(buf + 4 + kHashLen, guid_, kGuidLen);
  else
    std::memset(buf + 4 + kHashLen, 0, kGuidLen);
  const bool ok = Bourgeon::Instance().SendPacket(buf, sizeof(buf));
  if (ok)
    LogInfo("[Integrity] checksum + MachineGuid sent");
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
    LogInfo("[Integrity] kick-notice received — showing update popup");
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
      // frozen on the disconnected game screen.
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
    ImGui::TextWrapped(
        "Please close the game and run the patcher to update,\n"
        "then reconnect.");
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
