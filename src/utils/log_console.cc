#include "utils/log_console.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/msvc_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

namespace {
// Returns the directory of the running module (with trailing separator).
std::string ModuleDir() {
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string path(buf);
  const auto sep = path.find_last_of("\\/");
  return sep == std::string::npos ? std::string() : path.substr(0, sep + 1);
}

// Returns the --loglevel=<name> value from the command line, or "" if absent.
std::string CmdlineLevelName() {
  if (const char* cl = std::strstr(GetCommandLineA(), "--loglevel=")) {
    cl += 11;  // past "--loglevel="
    std::string v;
    while (*cl && *cl != ' ' && *cl != '\t' && *cl != '"') v += *cl++;
    return v;
  }
  return {};
}

// Returns the "log_level: <name>" value from bourgeon_settings.yaml (any
// nesting), or "" if absent.
std::string FileLevelName() {
  std::ifstream f(ModuleDir() + "bourgeon_settings.yaml");
  std::string line;
  while (std::getline(f, line)) {
    const auto key = line.find("log_level:");
    if (key == std::string::npos) continue;
    std::string v = line.substr(key + 10);
    const auto b = v.find_first_not_of(" \t\"'");
    if (b == std::string::npos) continue;
    const auto e = v.find_last_not_of(" \t\"'\r\n");
    return v.substr(b, e - b + 1);
  }
  return {};
}
}  // namespace

LogConsole::LogConsole() {
  // Only allocate a console window when the process was launched with --console.
  // The file sink (bourgeon.log) is always active regardless.
  if (strstr(GetCommandLineA(), "--console") != nullptr) {
    if (AllocConsole() == TRUE) {
      FILE *out;
      freopen_s(&out, "CONOUT$", "w", stdout);
      setvbuf(stdout, nullptr, _IONBF, 0);
    }
  }

  auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  // File sink: each write is flushed immediately so the file survives crashes.
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      "bourgeon.log", /*truncate=*/true);
  file_sink->set_level(spdlog::level::debug);

  p_logger_ = std::make_unique<spdlog::logger>(
      "Bourgeon",
      spdlog::sinks_init_list{stdout_sink, file_sink});
  p_logger_->flush_on(spdlog::level::trace);  // flush every write to file

  // Default threshold (debug builds are chattier), overridable by config.
#ifdef BOURGEON_DEBUG
  spdlog::level::level_enum lvl = spdlog::level::debug;
#else
  spdlog::level::level_enum lvl = spdlog::level::info;
#endif
  // Command line wins and is "sticky": when --loglevel is given, later
  // SetLevel() calls (e.g. from loading settings) are ignored so the cmdline
  // override is never clobbered by the yaml default.
  const std::string cl = CmdlineLevelName();
  if (!cl.empty()) {
    cmdline_forced_ = true;
    lvl = ParseLevel(cl, lvl);
  } else {
    lvl = ParseLevel(FileLevelName(), lvl);
  }
  p_logger_->set_level(lvl);
}

LogConsole::~LogConsole() {}

spdlog::logger *LogConsole::logger() const { return p_logger_.get(); }

spdlog::level::level_enum LogConsole::ParseLevel(
    const std::string &name, spdlog::level::level_enum fallback) {
  std::string n = name;
  std::transform(n.begin(), n.end(), n.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (n == "trace") return spdlog::level::trace;
  if (n == "debug") return spdlog::level::debug;
  if (n == "info" || n == "notice") return spdlog::level::info;
  if (n == "warn" || n == "warning") return spdlog::level::warn;
  if (n == "error" || n == "err") return spdlog::level::err;
  if (n == "off" || n == "none") return spdlog::level::off;
  return fallback;
}

void LogConsole::SetLevel(const std::string &name) {
  if (cmdline_forced_) return;  // --loglevel on the command line is sticky
  p_logger_->set_level(ParseLevel(name, p_logger_->level()));
}
