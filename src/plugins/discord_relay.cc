#include "plugins/discord_relay.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "bourgeon.h"
#include "nlohmann/json.hpp"
#include "spdlog/fmt/fmt.h"
#include "yaml-cpp/yaml.h"

using json = nlohmann::json;

namespace {

constexpr char kConfigPath[] = "./plugins/config/discord.yaml";
constexpr wchar_t kApiHost[] = L"discord.com";
constexpr char kApiBasePath[] = "/api/v10";
constexpr unsigned int kDiscordColor = 0x7289DA;
constexpr auto kPollInterval = std::chrono::seconds(5);
// Wait ~3s of ticks before hitting the Discord API so the game can start up.
constexpr uint32_t kInitDelayTicks = 30;
constexpr char kChatCommandPrefixes[] = "/@$%#!^";

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  int wlen =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), utf8.size(), nullptr, 0);
  std::wstring wide(wlen, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), utf8.size(), wide.data(), wlen);
  return wide;
}

// The game uses the ANSI codepage (Windows-1252 on Western systems) while
// Discord speaks UTF-8; unmappable characters become '?'.
std::string Utf8ToAnsi(const std::string& utf8) {
  std::wstring wide = Utf8ToWide(utf8);
  if (wide.empty()) return {};
  int alen = WideCharToMultiByte(CP_ACP, 0, wide.data(), wide.size(), nullptr,
                                 0, "?", nullptr);
  std::string ansi(alen, '\0');
  WideCharToMultiByte(CP_ACP, 0, wide.data(), wide.size(), ansi.data(), alen,
                      "?", nullptr);
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

// Synchronous HTTPS request; only call from background threads.
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

// Splits "https://host/path?query" into host and path+query.
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
  try {
    YAML::Node config = YAML::LoadFile(kConfigPath);
    bot_token_ = config["bot_token"].as<std::string>("");
    guild_name_ = config["guild_name"].as<std::string>("");
    if (config["channels_to_watch"].IsSequence()) {
      for (const auto& channel : config["channels_to_watch"]) {
        channel_names_.push_back(channel.as<std::string>());
      }
    }
    webhook_url_ = config["webhook_url"].as<std::string>("");
    char_name_fallback_ = config["char_name"].as<std::string>("");
  } catch (const std::exception&) {
    return;  // No/invalid config: plugin stays inert.
  }

  enabled_ = !bot_token_.empty() && !guild_name_.empty() &&
             !channel_names_.empty();
}

void DiscordRelay::PushEvent(Event::Type type, std::string text) {
  std::lock_guard<std::mutex> lock(events_mutex_);
  events_.push_back({type, std::move(text)});
}

void DiscordRelay::OnTick() {
  ++tick_count_;

  if (enabled_ && !init_started_ && tick_count_ > kInitDelayTicks) {
    init_started_ = true;
    std::thread([this] { InitThread(); }).detach();
  }

  std::vector<Event> events;
  {
    std::lock_guard<std::mutex> lock(events_mutex_);
    events.swap(events_);
  }
  for (const auto& event : events) {
    if (event.type == Event::Type::kLog) {
      Bourgeon::Instance().AddLogLine(event.text);
    } else {
      const std::string ansi = Utf8ToAnsi(event.text);
      Bourgeon::Instance().client().window_mgr().SendMsg(
          UIMessage::UIM_PUSHINTOCHATHISTORY,
          reinterpret_cast<int>(ansi.c_str()), kDiscordColor, 0, 0);
    }
  }
}

void DiscordRelay::OnModeSwitch(ModeMgr::ModeType mode_type,
                                const char* /*map_name*/) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
}

void DiscordRelay::OnTalkType(const char* chat_buffer) {
  if (!enabled_ || webhook_url_.empty() || !in_game_) return;

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
  payload["content"] = AnsiToUtf8(text);

  std::thread([url = webhook_url_, body = payload.dump()] {
    std::wstring host, path;
    if (!SplitUrl(url, host, path)) return;
    HttpResponse response;
    HttpsRequest(host, path, L"POST",
                 L"Content-Type: application/json\r\n", body, response);
  }).detach();
}

namespace {

// GET an authenticated Discord API path and parse the JSON response.
// Returns a discarded json value on any failure.
json ApiGet(const std::string& bot_token, const std::string& path,
            std::string* error) {
  HttpResponse response;
  const std::wstring headers =
      L"Authorization: Bot " + Utf8ToWide(bot_token) + L"\r\n";
  if (!HttpsRequest(kApiHost, Utf8ToWide(kApiBasePath + path), L"GET", headers,
                    "", response)) {
    if (error) *error = "request failed";
    return json(json::value_t::discarded);
  }
  if (response.status != 200) {
    if (error) {
      *error = fmt::format("HTTP {}: {}", response.status,
                           response.body.substr(0, 120));
    }
    return json(json::value_t::discarded);
  }
  json parsed = json::parse(response.body, nullptr, false);
  if (parsed.is_discarded() && error) *error = "invalid JSON";
  return parsed;
}

}  // namespace

void DiscordRelay::InitThread() {
  std::string error;
  json guilds = ApiGet(bot_token_, "/users/@me/guilds", &error);
  if (!guilds.is_array()) {
    PushEvent(Event::Type::kLog, "Discord: guild list failed: " + error);
    return;
  }

  std::string guild_id;
  for (const auto& guild : guilds) {
    if (guild.value("name", "") == guild_name_) {
      guild_id = guild.value("id", "");
      break;
    }
  }
  if (guild_id.empty()) {
    PushEvent(Event::Type::kLog,
              fmt::format("Discord: server '{}' not found", guild_name_));
    return;
  }

  json channels =
      ApiGet(bot_token_, "/guilds/" + guild_id + "/channels", &error);
  if (!channels.is_array()) {
    PushEvent(Event::Type::kLog, "Discord: channel list failed: " + error);
    return;
  }

  std::string watching;
  for (const auto& channel : channels) {
    if (channel.value("type", -1) != 0) continue;  // text channels only
    const std::string name = channel.value("name", "");
    if (std::find(channel_names_.begin(), channel_names_.end(), name) ==
        channel_names_.end()) {
      continue;
    }
    watched_channels_.push_back({channel.value("id", ""), name, ""});
    watching += (watching.empty() ? "#" : ", #") + name;
  }
  if (watched_channels_.empty()) {
    PushEvent(Event::Type::kLog, "Discord: no matching channels found");
    return;
  }

  PushEvent(Event::Type::kLog, "Discord watching: " + watching);
  init_done_ = true;
  PollThread();  // never returns; runs until process exit
}

void DiscordRelay::PollThread() {
  while (true) {
    if (in_game_) {
      PullMessages();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
}

void DiscordRelay::PullMessages() {
  for (auto& channel : watched_channels_) {
    std::string error;
    if (channel.cursor.empty()) {
      // First poll: remember the latest message and only relay what follows.
      json messages = ApiGet(
          bot_token_, "/channels/" + channel.id + "/messages?limit=1", &error);
      channel.cursor = (messages.is_array() && !messages.empty())
                           ? messages[0].value("id", "0")
                           : "0";
      continue;
    }

    json messages = ApiGet(bot_token_,
                           "/channels/" + channel.id + "/messages?after=" +
                               channel.cursor + "&limit=50",
                           &error);
    if (!messages.is_array()) {
      PushEvent(Event::Type::kLog,
                fmt::format("Discord #{} poll failed: {}", channel.name,
                            error));
      continue;
    }

    // The API returns newest first; relay in chronological order.
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
      const json& msg = *it;
      channel.cursor = msg.value("id", channel.cursor);
      if (msg.contains("webhook_id")) continue;  // skip our own echoes
      const std::string content = Trim(msg.value("content", ""));
      if (content.empty()) continue;

      std::string display;
      if (msg.contains("member")) display = msg["member"].value("nick", "");
      if (display.empty() && msg.contains("author")) {
        const json& author = msg["author"];
        if (author.contains("global_name") &&
            author["global_name"].is_string()) {
          display = author["global_name"].get<std::string>();
        }
        if (display.empty()) display = author.value("username", "");
      }

      PushEvent(Event::Type::kChat,
                fmt::format("(#{}) {}: {}", channel.name, display, content));
    }
  }
}
