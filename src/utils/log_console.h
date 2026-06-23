#pragma once

#include <memory>
#include <string>

#include "spdlog/spdlog.h"

class LogConsole {
 public:
  LogConsole(LogConsole const&) = delete;
  void operator=(LogConsole const&) = delete;
  static LogConsole& instance() {
    // Guaranteed to be destroyed.
    // Instantiated on first use.
    static LogConsole instance;

    return instance;
  }

  spdlog::logger* logger() const;

  // Sets the active log level by name (rAthena-style): "trace", "debug",
  // "info"/"notice", "warn"/"warning", "error", "off".  Unknown names are
  // ignored.  Safe to call any time (e.g. after loading settings).
  void SetLevel(const std::string& name);

  // Maps a level name to spdlog level; returns |fallback| when unrecognised.
  static spdlog::level::level_enum ParseLevel(const std::string& name,
                                              spdlog::level::level_enum fallback);

 private:
  LogConsole();
  ~LogConsole();

 private:
  std::unique_ptr<spdlog::logger> p_logger_;
  // True when --loglevel was on the command line; makes SetLevel a no-op so the
  // cmdline override is never clobbered by settings load.
  bool cmdline_forced_ = false;
};

// Runtime threshold (info/warn/error) is set from config (log_level in
// bourgeon_settings.yaml) or --loglevel=<name>.  LogDebug stays COMPILED OUT in
// release builds: its arguments must not be evaluated when disabled (many call
// sites pass pointer derefs / side effects that are only valid in debug), so it
// is a true no-op unless BOURGEON_DEBUG is defined.
#define LogInfo(fmt, ...) \
  LogConsole::instance().logger()->info(fmt, ##__VA_ARGS__)

#define LogWarn(fmt, ...) \
  LogConsole::instance().logger()->warn(fmt, ##__VA_ARGS__)

#define LogError(fmt, ...) \
  LogConsole::instance().logger()->error(fmt, ##__VA_ARGS__)

#ifdef BOURGEON_DEBUG
#define LogDebug(fmt, ...) \
  LogConsole::instance().logger()->debug(fmt, ##__VA_ARGS__)
#else
#define LogDebug(fmt, ...) ((void)0)
#endif  // BOURGEON_DEBUG
