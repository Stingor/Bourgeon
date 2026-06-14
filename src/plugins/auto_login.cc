#include "plugins/auto_login.h"

#include <Windows.h>
#include <shellapi.h>  // CommandLineToArgvW

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

#include "ragnarok/ragnarok_client.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

namespace {

// Directory of the game executable (with trailing separator).
std::string GameDir() {
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string path(buf);
  const auto sep = path.find_last_of("\\/");
  if (sep != std::string::npos) path.resize(sep + 1);
  return path;
}

std::string SettingsPath() { return GameDir() + "bourgeon_settings.yaml"; }
std::string ClientInfoPath() { return GameDir() + "data\\clientinfo.xml"; }

// Narrow a wide command-line argument. Credentials are ASCII in practice, so a
// plain byte-truncation is sufficient here.
std::string Narrow(const wchar_t* w) {
  std::string s;
  for (; *w; ++w) s.push_back(static_cast<char>(*w));
  return s;
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Interprets common truthy/falsy spellings; returns `fallback` if unrecognised.
bool ParseBool(const std::string& s, bool fallback) {
  const std::string v = ToLower(Trim(s));
  if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
  if (v == "false" || v == "0" || v == "no" || v == "off") return false;
  return fallback;
}

// If `arg` is "--<key>:<value>" or "--<key>=<value>", writes the value to `out`
// and returns true.
bool MatchOption(const std::string& arg, const char* key, std::string& out) {
  const std::string prefix = std::string("--") + key;
  if (arg.size() <= prefix.size() ||
      arg.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  const char sep = arg[prefix.size()];
  if (sep != ':' && sep != '=') return false;
  out = arg.substr(prefix.size() + 1);
  return true;
}

// Extracts the ordered list of <display>…</display> values from clientinfo.xml.
// A deliberately tiny scanner — clientinfo display names are plain ASCII.
std::vector<std::string> ReadConnectionNames(const std::string& path) {
  std::vector<std::string> names;
  std::ifstream f(path, std::ios::binary);
  if (!f) return names;
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string xml = ss.str();

  size_t pos = 0;
  const std::string open = "<display>";
  const std::string close = "</display>";
  while ((pos = xml.find(open, pos)) != std::string::npos) {
    const size_t start = pos + open.size();
    const size_t end = xml.find(close, start);
    if (end == std::string::npos) break;
    names.push_back(Trim(xml.substr(start, end - start)));
    pos = end + close.size();
  }
  return names;
}

}  // namespace

AutoLogin::AutoLogin() {
  const bool from_cmdline = ParseCommandLine();
  if (!from_cmdline) LoadFromYaml();

  if (login_.empty() || password_.empty()) {
    stage_ = Stage::kDisabled;
    return;
  }

  ResolveServerFromClientInfo();
  stage_ = Stage::kIdle;
  LogInfo(
      "[AutoLogin] armed for '{}' — server '{}' (index {} of {} connection(s)), "
      "save_id={} ({} focused first)",
      login_, server_, server_index_, server_count_, save_id_,
      save_id_ ? "password" : "id");
}

bool AutoLogin::ParseCommandLine() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return false;

  bool found = false;
  std::string saveid;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = Narrow(argv[i]);
    if (MatchOption(arg, "login", login_) ||
        MatchOption(arg, "pass", password_) ||
        MatchOption(arg, "server", server_)) {
      found = true;
    } else if (MatchOption(arg, "saveid", saveid)) {
      save_id_ = ParseBool(saveid, save_id_);
      found = true;
    }
  }
  LocalFree(argv);
  return found;
}

void AutoLogin::LoadFromYaml() {
  std::ifstream f(SettingsPath());
  if (!f) return;  // no settings file — nothing to load
  try {
    const YAML::Node root = YAML::Load(f);
    const YAML::Node al = root["auto_login"];
    if (!al) return;
    if (login_.empty()) login_ = al["login"].as<std::string>("");
    if (password_.empty()) password_ = al["pass"].as<std::string>("");
    if (server_.empty()) server_ = al["server"].as<std::string>("");
    save_id_ = al["save_id"].as<bool>(save_id_);
  } catch (const std::exception& e) {
    LogError("[AutoLogin] failed to read settings: {}", e.what());
  }
}

void AutoLogin::ResolveServerFromClientInfo() {
  const std::vector<std::string> names = ReadConnectionNames(ClientInfoPath());
  server_count_ = static_cast<int>(names.size());
  server_index_ = 0;
  if (server_.empty()) return;

  const std::string target = ToLower(server_);
  for (int i = 0; i < server_count_; ++i) {
    if (ToLower(names[i]) == target) {
      server_index_ = i;
      return;
    }
  }
  LogError(
      "[AutoLogin] server '{}' not found in clientinfo.xml; using first entry",
      server_);
}

void AutoLogin::OnModeSwitch(ModeMgr::ModeType mode_type, const char* /*map*/) {
  if (stage_ == Stage::kDisabled || stage_ == Stage::kDone) return;

  // (Re)arm whenever we land on the login phase. We intentionally do NOT
  // re-run after kDone, so logging out doesn't kick off an auto-login loop.
  if (mode_type == ModeMgr::ModeType::kLogin) {
    stage_ = Stage::kSettle;
    tick_counter_ = 0;
  }
}

void AutoLogin::OnTick() {
  switch (stage_) {
    case Stage::kSettle:
      if (++tick_counter_ >= kSettleTicks) {
        // With 0 or 1 connection the client skips the server-select screen and
        // shows the login window straight away.
        tick_counter_ = 0;
        stage_ = (server_count_ > 1) ? Stage::kSelectServer : Stage::kCredentials;
      }
      break;

    case Stage::kSelectServer: {
      if (RagnarokClient::GameWindow() == nullptr) break;
      // The first entry is highlighted by default; step down to our target and
      // confirm. (Moonlight-Destiny is index 0 here, so this is just Enter.)
      for (int i = 0; i < server_index_; ++i) PressKey(VK_DOWN);
      PressKey(VK_RETURN);
      LogInfo("[AutoLogin] selected server index {}", server_index_);
      tick_counter_ = 0;
      stage_ = Stage::kWaitLogin;
      break;
    }

    case Stage::kWaitLogin:
      if (++tick_counter_ >= kWaitLoginTicks) {
        tick_counter_ = 0;
        stage_ = Stage::kCredentials;
      }
      break;

    case Stage::kCredentials: {
      if (RagnarokClient::GameWindow() == nullptr) break;
      // Fill the initially-focused field first, Tab to the other, fill it, then
      // submit. With Save ID on the password field is focused first; with it off
      // the ID field is. Tab toggles between the two. Steps are spread one per
      // tick to stay robust against any per-frame focus handling.
      const std::string& first = save_id_ ? password_ : login_;
      const std::string& second = save_id_ ? login_ : password_;
      switch (tick_counter_++) {
        case 0:
          TypeString(first);  // into the initially-focused field
          break;
        case 1:
          PressKey(VK_TAB);  // toggle to the other field
          break;
        case 3:  // tick 2 left empty so the focus switch settles
          TypeString(second);
          break;
        case 4:
          PressKey(VK_RETURN);  // submit (Enter triggers the Login button)
          LogInfo("[AutoLogin] submitted credentials for '{}'", login_);
          tick_counter_ = 0;
          stage_ = Stage::kCharServer;
          break;
        default:
          break;
      }
      break;
    }

    case Stage::kCharServer:
      // After login the account server returns its char-server list. For a
      // single char-server, confirming the default entry reaches char select.
      if (++tick_counter_ >= kCharServerTicks) {
        PressKey(VK_RETURN);
        LogInfo("[AutoLogin] confirmed char-server select");
        stage_ = Stage::kDone;
      }
      break;

    default:
      break;
  }
}

void AutoLogin::TypeString(const std::string& text) {
  HWND hwnd = static_cast<HWND>(RagnarokClient::GameWindow());
  if (hwnd == nullptr) return;
  for (unsigned char ch : text) {
    PostMessageW(hwnd, WM_CHAR, ch, 0);
  }
}

void AutoLogin::PressKey(int vkey) {
  HWND hwnd = static_cast<HWND>(RagnarokClient::GameWindow());
  if (hwnd == nullptr) return;
  PostMessageW(hwnd, WM_KEYDOWN, static_cast<WPARAM>(vkey), 0);
  PostMessageW(hwnd, WM_KEYUP, static_cast<WPARAM>(vkey), 0);
}
