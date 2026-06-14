#include "plugins/integrity_check.h"

#include <Windows.h>
#include <bcrypt.h>
#include <shellapi.h>  // CommandLineToArgvW

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

bool IntegrityCheck::ParseEnabled() {
  bool enabled = true;  // default: send the integrity packet
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return enabled;

  for (int i = 1; i < argc; ++i) {
    // Match "--integrity:<v>" or "--integrity=<v>".
    const wchar_t* a = argv[i];
    if (wcsncmp(a, L"--integrity", 11) != 0) continue;
    const wchar_t sep = a[11];
    if (sep != L':' && sep != L'=') continue;
    const wchar_t* v = a + 12;
    enabled = !(wcscmp(v, L"false") == 0 || wcscmp(v, L"0") == 0 ||
                wcscmp(v, L"no") == 0 || wcscmp(v, L"off") == 0);
  }
  LocalFree(argv);
  return enabled;
}

IntegrityCheck::IntegrityCheck() {
  enabled_ = ParseEnabled();

  // Always register the kick-notice opcode so the server can warn us even if
  // integrity sending is disabled on this client.
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeKickNotice);

  if (!enabled_) {
    LogInfo("[Integrity] disabled via --integrity:false — not sending checksum");
    return;  // skip hashing entirely; nothing will be sent
  }

  std::wstring path;
  if (SelfModulePath(path) && Sha256OfFile(path, hash_, kHashLen)) {
    have_hash_ = true;
    LogInfo("[Integrity] self SHA-256 {:02x}{:02x}{:02x}{:02x}... computed",
            hash_[0], hash_[1], hash_[2], hash_[3]);
  } else {
    // Without a checksum we simply send nothing; the server will see a client
    // that never reports and can treat that as a failure when enforcing.
    LogError("[Integrity] failed to compute self checksum");
  }
}

void IntegrityCheck::OnModeSwitch(ModeMgr::ModeType mode_type,
                                  const char* /*map*/) {
  // Re-arm on every login so each game session reports once.
  if (mode_type == ModeMgr::ModeType::kLogin) {
    sent_ = false;
    return;
  }
  if (mode_type != ModeMgr::ModeType::kGame) return;
  if (!enabled_ || sent_ || !have_hash_) return;
  SendChecksum();
  sent_ = true;
}

void IntegrityCheck::SendChecksum() {
  uint8_t buf[4 + kHashLen];
  *reinterpret_cast<uint16_t*>(buf) = kOpcodeToServer;
  *reinterpret_cast<uint16_t*>(buf + 2) = static_cast<uint16_t>(sizeof(buf));
  std::memcpy(buf + 4, hash_, kHashLen);
  Bourgeon::Instance().SendPacket(buf, sizeof(buf));
  LogInfo("[Integrity] checksum sent");
}

void IntegrityCheck::OnRecvPacket(uint16_t opcode, const uint8_t* /*data*/,
                                  uint16_t /*len*/) {
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
