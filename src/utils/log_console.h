#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "spdlog/spdlog.h"

// Thread-safe, capacity-bounded ring buffer that mirrors every emitted log line
// (info/warn/error — whatever passes the active level) so the in-game ImGui log
// window can display them.  It is fed by an in-memory spdlog sink installed in
// LogConsole and read (snapshotted) by the Bourgeon window each frame.  spdlog
// sinks may run on any thread, so all access is mutex-guarded.
class LogLineBuffer {
 public:
  static LogLineBuffer& instance() {
    static LogLineBuffer instance;
    return instance;
  }
  LogLineBuffer(const LogLineBuffer&) = delete;
  void operator=(const LogLineBuffer&) = delete;

  // Append one already-formatted line (trailing newline stripped by the sink).
  // Drops the oldest line once the capacity is reached.
  void Push(std::string line);
  // Replace |out| with a snapshot of the buffered lines, oldest first.
  void Snapshot(std::vector<std::string>* out) const;

 private:
  LogLineBuffer() = default;

  mutable std::mutex mutex_;
  std::deque<std::string> lines_;
  static constexpr std::size_t kCapacity = 2000;
};

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
  // Re-applies |console_level| to the stdout sink and lowers the logger threshold
  // to min(console_level, capture_floor_) so the file + in-game-window sinks keep
  // receiving info+ even when the console is quieter.
  void ApplyConsoleLevel(spdlog::level::level_enum console_level);

  std::unique_ptr<spdlog::logger> p_logger_;
  // The stdout/console sink — the only one that honours the configured log level;
  // the file and in-game-window sinks always capture down to |capture_floor_|.
  std::shared_ptr<spdlog::sinks::sink> stdout_sink_;
  // Lowest level the always-on sinks (file, in-game window) capture: info in
  // release, debug in debug builds.
  spdlog::level::level_enum capture_floor_ = spdlog::level::info;
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
