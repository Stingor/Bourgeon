#include "utils/log_console.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include "spdlog/sinks/base_sink.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/msvc_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

namespace {
// spdlog sink that mirrors each record into LogLineBuffer for the in-game log
// window.  base_sink<std::mutex> serialises sink_it_ so the formatter is used by
// one thread at a time; LogLineBuffer adds its own lock for the snapshot reader.
class UiLogSink : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t buf;
    formatter_->format(msg, buf);
    std::string line(buf.data(), buf.size());
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.pop_back();
    LogLineBuffer::instance().Push(std::move(line));
  }
  void flush_() override {}
};

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
  // In-memory sink: mirrors every emitted line into LogLineBuffer so the in-game
  // Bourgeon log window shows all LogInfo/LogWarn/LogError output, not just the
  // plugin lines pushed via Bourgeon::AddLogLine.
  auto ui_sink = std::make_shared<UiLogSink>();

  // The file and in-game-window sinks always capture down to |capture_floor_| so
  // they keep info even when the console is configured quieter (e.g. warn).
#ifdef BOURGEON_DEBUG
  capture_floor_ = spdlog::level::debug;
#else
  capture_floor_ = spdlog::level::info;
#endif
  stdout_sink_ = stdout_sink;
  file_sink->set_level(capture_floor_);
  ui_sink->set_level(capture_floor_);

  p_logger_ = std::make_unique<spdlog::logger>(
      "Bourgeon",
      spdlog::sinks_init_list{stdout_sink, file_sink, ui_sink});
  p_logger_->flush_on(spdlog::level::trace);  // flush every write to file

  // Console threshold default (debug builds are chattier), overridable by config.
  spdlog::level::level_enum lvl = capture_floor_;
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
  ApplyConsoleLevel(lvl);
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
  ApplyConsoleLevel(ParseLevel(name, stdout_sink_->level()));
}

void LogConsole::ApplyConsoleLevel(spdlog::level::level_enum console_level) {
  stdout_sink_->set_level(console_level);
  // The logger gates a record before any sink sees it, so it must pass the more
  // verbose of the console level and the always-capture floor — otherwise a quiet
  // console (e.g. warn) would also starve the file / in-game-window sinks of info.
  p_logger_->set_level(std::min(console_level, capture_floor_));
}

void LogLineBuffer::Push(std::string line) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (lines_.size() >= kCapacity) lines_.pop_front();
  lines_.push_back(std::move(line));
}

void LogLineBuffer::Snapshot(std::vector<std::string> *out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  out->assign(lines_.begin(), lines_.end());
}
