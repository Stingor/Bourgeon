#include "plugins/discord_relay.h"

#include <Windows.h>
#include <winhttp.h>

#include <cstring>
#include <fstream>
#include <thread>

#include "bourgeon.h"
#include "nlohmann/json.hpp"
#include "plugins/discord_key.h"
#include "spdlog/fmt/fmt.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

using json = nlohmann::json;

namespace {

constexpr char kEncConfigPath[] = "./discord.enc";
constexpr char kChatCommandPrefixes[] = "/@$%#!^";
constexpr unsigned int kDiscordColor = 0x7289DA;

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  int wlen =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), utf8.size(), nullptr, 0);
  std::wstring wide(wlen, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), utf8.size(), wide.data(), wlen);
  return wide;
}

// Converts UTF-8 text (from Discord) to ANSI (Windows-1252) for the game chat.
// Characters not representable in the ANSI codepage are dropped silently.
std::string Utf8ToAnsi(const std::string& utf8) {
  std::wstring wide = Utf8ToWide(utf8);
  if (wide.empty()) return {};
  std::string ansi;
  ansi.reserve(wide.size());
  for (size_t i = 0; i < wide.size();) {
    int units = 1;
    if (wide[i] >= 0xD800 && wide[i] <= 0xDBFF && i + 1 < wide.size() &&
        wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
      units = 2;
    }
    char buf[8];
    BOOL used_default = FALSE;
    int n = WideCharToMultiByte(CP_ACP, 0, &wide[i], units, buf, sizeof(buf),
                                "?", &used_default);
    if (n > 0 && !used_default) ansi.append(buf, n);
    i += units;
  }
  return ansi;
}

std::string AnsiToUtf8(const std::string& ansi) {
  if (ansi.empty()) return {};
  int wlen =
      MultiByteToWideChar(CP_ACP, 0, ansi.data(), ansi.size(), nullptr, 0);
  std::wstring wide(wlen, L'\0');
  MultiByteToWideChar(CP_ACP, 0, ansi.data(), ansi.size(), wide.data(), wlen);
  int ulen = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide.size(), nullptr,
                                 0, nullptr, nullptr);
  std::string utf8(ulen, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide.size(), utf8.data(), ulen,
                      nullptr, nullptr);
  return utf8;
}

struct HttpResponse {
  DWORD status = 0;
  std::string body;
};

bool HttpsRequest(const std::wstring& host, const std::wstring& path,
                  const wchar_t* method, const std::wstring& headers,
                  const std::string& body, HttpResponse& out) {
  bool ok = false;
  HINTERNET session =
      WinHttpOpen(L"Bourgeon", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

  HINTERNET connect =
      WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET request = nullptr;
  if (connect) {
    request = WinHttpOpenRequest(connect, method, path.c_str(), nullptr,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
  }
  if (request &&
      WinHttpSendRequest(request,
                         headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                         : headers.c_str(),
                         headers.empty() ? 0 : -1L,
                         body.empty() ? WINHTTP_NO_REQUEST_DATA
                                      : const_cast<char*>(body.data()),
                         body.size(), body.size(), 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status_size = sizeof(out.status);
    WinHttpQueryHeaders(
        request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &out.status, &status_size,
        WINHTTP_NO_HEADER_INDEX);

    DWORD available = 0;
    do {
      available = 0;
      if (!WinHttpQueryDataAvailable(request, &available)) break;
      if (available == 0) break;
      size_t offset = out.body.size();
      out.body.resize(offset + available);
      DWORD read = 0;
      if (!WinHttpReadData(request, &out.body[offset], available, &read))
        break;
      out.body.resize(offset + read);
    } while (available > 0);
    ok = true;
  }

  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  if (session) WinHttpCloseHandle(session);
  return ok;
}

bool SplitUrl(const std::string& url, std::wstring& host, std::wstring& path) {
  constexpr char kScheme[] = "https://";
  if (url.compare(0, sizeof(kScheme) - 1, kScheme) != 0) return false;
  const auto host_begin = sizeof(kScheme) - 1;
  const auto slash = url.find('/', host_begin);
  if (slash == std::string::npos) return false;
  host = Utf8ToWide(url.substr(host_begin, slash - host_begin));
  path = Utf8ToWide(url.substr(slash));
  return true;
}

std::string Trim(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

}  // namespace

DiscordRelay::DiscordRelay() {
  // Always register the inbound relay opcode so the packet is captured,
  // even when the outbound webhook is not configured.
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeDiscordMsg);

  std::ifstream enc_file(kEncConfigPath, std::ios::binary);
  if (!enc_file) {
    LogInfo("Discord relay: {} not found — outbound webhook inactive",
            kEncConfigPath);
    return;
  }

  const std::string raw((std::istreambuf_iterator<char>(enc_file)),
                         std::istreambuf_iterator<char>());
  const std::vector<uint8_t> blob(raw.begin(), raw.end());

  std::string yaml_text;
  if (!discord_crypto::Decrypt(blob, yaml_text)) {
    LogError("Discord relay: failed to decrypt {} (wrong key or corrupted)",
             kEncConfigPath);
    return;
  }

  try {
    YAML::Node config = YAML::Load(yaml_text);
    webhook_url_        = config["webhook_url"].as<std::string>("");
    char_name_fallback_ = config["char_name"].as<std::string>("");
    avatar_base_        = config["avatar_base"].as<std::string>(
        "https://moonlight-destiny.fr/images/CacheAvatar/");
  } catch (const std::exception& e) {
    LogError("Discord relay: failed to parse config: {}", e.what());
    return;
  }

  enabled_ = !webhook_url_.empty();
  if (enabled_) {
    LogInfo("Discord relay: outbound webhook active");
  } else {
    LogInfo("Discord relay: config loaded but no webhook_url — outbound disabled");
  }
}

void DiscordRelay::PushEvent(Event::Type type, std::string text) {
  std::lock_guard<std::mutex> lock(events_mutex_);
  events_.push_back({type, std::move(text)});
}

void DiscordRelay::OnTick() {
  std::vector<Event> events;
  {
    std::lock_guard<std::mutex> lock(events_mutex_);
    events.swap(events_);
  }
  for (const auto& event : events) {
    if (event.type == Event::Type::kLog) {
      Bourgeon::Instance().AddLogLine(event.text);
    } else {
      // The packet bytes come from rAthena, which uses the game's ANSI/Latin-1 encoding
      // internally.  Do NOT call Utf8ToAnsi: the bytes are already in the right codepage,
      // and treating Latin-1 as UTF-8 would drop every accented character.
      Bourgeon::Instance().client().window_mgr().SendMsg(
          UIMessage::UIM_PUSHINTOCHATHISTORY,
          reinterpret_cast<int>(event.text.c_str()), kDiscordColor, 0, 0);
    }
  }
}

void DiscordRelay::OnModeSwitch(ModeMgr::ModeType mode_type,
                                const char* /*map_name*/) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
}

void DiscordRelay::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                uint16_t len) {
  if (opcode == kOpcodeDiscordMsg) {
    // Silently drop if the player has the relay checkbox OFF.
    if (!chat_active_.load() || len == 0) return;
    // data is a UTF-8 string (possibly null-terminated within len bytes).
    const char* p = reinterpret_cast<const char*>(data);
    size_t str_len = 0;
    while (str_len < len && p[str_len] != '\0') ++str_len;
    if (str_len > 0)
      PushEvent(Event::Type::kChat, std::string(p, str_len));
    return;
  }
  // ZC_BOURGEON_SETTINGS (registered by MoonlightUi) carries the player's
  // char_id as its first field, right after the [opcode:2][len:2] header.
  if (opcode == kOpcodeSettings && len >= 4)
    char_id_ = *reinterpret_cast<const uint32_t*>(data);
}

void DiscordRelay::OnTalkType(const char* chat_buffer) {
  if (!enabled_ || webhook_url_.empty() || !in_game_ || !chat_active_.load())
    return;

  const std::string text = Trim(chat_buffer ? chat_buffer : "");
  if (text.empty() ||
      strchr(kChatCommandPrefixes, text.front()) != nullptr) {
    return;
  }

  std::string char_name = Bourgeon::Instance().client().session().GetCharName();
  if (char_name.empty()) char_name = char_name_fallback_;
  if (char_name.empty()) char_name = "Unknown";

  json payload;
  payload["username"] = AnsiToUtf8(char_name);
  payload["content"]  = AnsiToUtf8(text);
  if (char_id_ != 0 && !avatar_base_.empty())
    payload["avatar_url"] = fmt::format("{}{}.png", avatar_base_, char_id_);

  std::thread([url = webhook_url_, body = payload.dump()] {
    std::wstring host, path;
    if (!SplitUrl(url, host, path)) return;
    HttpResponse response;
    HttpsRequest(host, path, L"POST",
                 L"Content-Type: application/json\r\n", body, response);
  }).detach();
}
