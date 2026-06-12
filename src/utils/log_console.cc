#include "utils/log_console.h"

#include <Windows.h>

#include <vector>

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/msvc_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

LogConsole::LogConsole() {
  if (AllocConsole() == TRUE) {
    FILE *out;
    freopen_s(&out, "CONOUT$", "w", stdout);
    // Disable buffering so every log write reaches the console immediately.
    setvbuf(stdout, nullptr, _IONBF, 0);
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

#ifdef BOURGEON_DEBUG
  p_logger_->set_level(spdlog::level::debug);
#else
  p_logger_->set_level(spdlog::level::info);
#endif
}

LogConsole::~LogConsole() {}

spdlog::logger *LogConsole::logger() const { return p_logger_.get(); }
