#include "plugins/discord_relay.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <regex>
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
// Discord speaks UTF-8. Characters that don't exist in the codepage (e.g.
// Unicode emoji) are DROPPED rather than turned into '?' clutter, while
// representable ones (accents, etc.) are kept. Converted codepoint-by-codepoint
// so surrogate-pair emoji are detected and skipped as a unit.
std::string Utf8ToAnsi(const std::string& utf8) {
  std::wstring wide = Utf8ToWide(utf8);
  if (wide.empty()) return {};
  std::string ansi;
  ansi.reserve(wide.size());
  for (size_t i = 0; i < wide.size();) {
    int units = 1;
    if (wide[i] >= 0xD800 && wide[i] <= 0xDBFF && i + 1 < wide.size() &&
        wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
      units = 2;  // surrogate pair (e.g. an emoji) → one codepoint
    }
    char buf[8];
    BOOL used_default = FALSE;
    int n = WideCharToMultiByte(CP_ACP, 0, &wide[i], units, buf, sizeof(buf),
                                "?", &used_default);
    if (n > 0 && !used_default) ansi.append(buf, n);  // drop if unmappable
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

void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos;
       pos += to.size()) {
    s.replace(pos, from.size(), to);
  }
}

// Turns Discord's raw markup in a message into something readable in the ANSI
// game chat:
//   <@id> / <@!id>  user mention  -> @name  (resolved via the message's
//                                    `mentions` array, else @user)
//   <@&id>          role mention  -> @role
//   <#id>           channel       -> #channel
//   <:name:id> / <a:name:id> custom emote -> :name:
// Unicode emoji left in the text are dropped later by Utf8ToAnsi.
std::string SanitizeContent(const json& msg, std::string s) {
  if (msg.contains("mentions") && msg["mentions"].is_array()) {
    for (const auto& user : msg["mentions"]) {
      const std::string id = user.value("id", "");
      if (id.empty()) continue;
      std::string name;
      if (user.contains("global_name") && user["global_name"].is_string())
        name = user["global_name"].get<std::string>();
      if (name.empty()) name = user.value("username", "user");
      ReplaceAll(s, "<@" + id + ">", "@" + name);
      ReplaceAll(s, "<@!" + id + ">", "@" + name);
    }
  }
  s = std::regex_replace(s, std::regex(R"(<a?:([A-Za-z0-9_]+):[0-9]+>)"),
                         ":$1:");
  s = std::regex_replace(s, std::regex(R"(<@&[0-9]+>)"), "@role");
  s = std::regex_replace(s, std::regex(R"(<@!?[0-9]+>)"), "@user");
  s = std::regex_replace(s, std::regex(R"(<#[0-9]+>)"), "#channel");
  return s;
}

}  // namespace

DiscordRelay::DiscordRelay() {
  // Read the encrypted blob written by discord_encrypt.exe.
  std::ifstream enc_file(kEncConfigPath, std::ios::binary);
  if (!enc_file) {
    LogInfo("Discord relay: {} not found — relay inactive", kEncConfigPath);
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
    bot_token_ = config["bot_token"].as<std::string>("");
    guild_name_ = config["guild_name"].as<std::string>("");
    if (config["channels_to_watch"].IsSequence()) {
      for (const auto& channel : config["channels_to_watch"]) {
        channel_names_.push_back(channel.as<std::string>());
      }
    }
    webhook_url_ = config["webhook_url"].as<std::string>("");
    char_name_fallback_ = config["char_name"].as<std::string>("");
    avatar_base_ = config["avatar_base"].as<std::string>(
        "https://moonlight-destiny.fr/images/CacheAvatar/");
  } catch (const std::exception& e) {
    LogError("Discord relay: failed to parse config: {}", e.what());
    return;
  }

  enabled_ = !bot_token_.empty() && !guild_name_.empty() &&
             !channel_names_.empty();
  if (enabled_) {
    LogInfo("Discord relay: active (guild={}, channels={})", guild_name_,
            channel_names_.size());
  } else {
    LogError("Discord relay: config loaded but missing required fields");
  }
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

void DiscordRelay::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                uint16_t len) {
  // ZC_BOURGEON_SETTINGS (registered by MoonlightUi) carries the player's
  // char_id as its first field, right after the [opcode:2][len:2] header.
  if (opcode != kOpcodeSettings || len < 4) return;
  char_id_ = *reinterpret_cast<const uint32_t*>(data);
}

void DiscordRelay::OnTalkType(const char* chat_buffer) {
  if (!enabled_ || webhook_url_.empty() || !in_game_ || !chat_active_.load()) return;

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
  // Per-character avatar, once the server has told us this character's id.
  if (char_id_ != 0 && !avatar_base_.empty()) {
    payload["avatar_url"] = fmt::format("{}{}.png", avatar_base_, char_id_);
  }

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
      std::string content = Trim(SanitizeContent(msg, msg.value("content", "")));
      if (content.empty()) continue;  // e.g. an emoji-only message

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

      if (chat_active_.load()) {
        PushEvent(Event::Type::kChat,
                  fmt::format("(#{}) {}: {}", channel.name, display, content));
      }
    }
  }
}
