#include "utils/log_console.h"
#include "utils/game_paths.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "spdlog/sinks/base_sink.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/msvc_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "yaml-cpp/yaml.h"

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

// (Le dossier du jeu vit dans utils/game_paths.h : paths::GameDir().)

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

// Valeur de `moonlight_ui.log_level` dans bourgeon_settings.yaml, ou "" si absente.
//
// Ce niveau est nécessaire DÈS la construction du logger, donc bien avant que les
// plugins existent et que MoonlightUi::LoadSettings ne tourne : sans cette lecture
// anticipée, toutes les lignes du démarrage sortiraient au niveau par défaut. D'où
// une seconde lecture du même fichier, assumée.
//
// Elle se faisait par recherche TEXTUELLE de « log_level: » ligne par ligne, à
// n'importe quelle profondeur : un commentaire ou une valeur de chaîne contenant
// ces caractères, dans n'importe quelle section du fichier partagé, était pris
// pour le réglage. On lit maintenant la clé à sa place exacte, avec le parseur du
// projet.
std::string FileLevelName() {
  try {
    const YAML::Node root = YAML::LoadFile(paths::SettingsPath());
    const YAML::Node ui = root["moonlight_ui"];
    if (!ui) return {};
    return ui["log_level"].as<std::string>("");
  } catch (const std::exception&) {
    // Fichier absent (premier lancement) ou yaml invalide : on garde le défaut.
    // Surtout pas de log ici — le logger est justement en train de se construire.
    return {};
  }
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
  // Bourgeon log window shows all LogInfo/LogDiag/LogError output, not just the
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
