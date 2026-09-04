#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "spdlog/spdlog.h"

// Une ligne du journal telle que la fenêtre en jeu la consomme : le texte déjà
// mis en forme par spdlog, et le NIVEAU d'où elle vient.
//
// 🔴 Le niveau est MÉMORISÉ, jamais relu dans le texte. Chercher « [info] » dans
// la ligne formatée marcherait aujourd'hui et casserait silencieusement demain :
// le motif de spdlog est configurable (il peut cesser d'écrire le niveau), et un
// message peut parfaitement contenir « [info] » lui-même — un paquet relayé, un
// bout de Lua, une ligne de log serveur recopiée.
struct LogLine {
  std::string text;
  spdlog::level::level_enum level = spdlog::level::info;
};

// Thread-safe, capacity-bounded ring buffer that mirrors every emitted log line
// (info/warn/error — whatever passes the active level) so the in-game ImGui log
// window can display them.  It is fed by an in-memory spdlog sink installed in
// LogConsole and read by the Bourgeon window each frame.  spdlog sinks may run
// on any thread, so all access is mutex-guarded.
//
// 🔴 LA RÉVISION EST LA SEULE MARQUE DE FRAÎCHEUR VALABLE. La taille n'en est
// pas une : l'anneau sature à `kCapacity`, et à partir de là chaque nouvelle
// ligne en chasse une ancienne — `lines_.size()` reste figé à 2000 pendant que
// le contenu change entièrement. La fenêtre de log, qui se rafraîchissait sur ce
// critère, cessait donc de se mettre à jour passé la 2000ᵉ ligne. Le même piège
// est documenté sur `ingest_kept_` dans la chatbox, pour la même raison.
class LogLineBuffer {
 public:
  static LogLineBuffer& instance() {
    static LogLineBuffer instance;
    return instance;
  }
  LogLineBuffer(const LogLineBuffer&) = delete;
  void operator=(const LogLineBuffer&) = delete;

  // Append one already-formatted line (trailing newline stripped by the sink),
  // with the level of the record it came from.
  // Drops the oldest line once the capacity is reached.
  void Push(std::string line, spdlog::level::level_enum level);
  // Recopie les lignes bufferisées dans |out| (la plus ancienne d'abord), mais
  // SEULEMENT si le tampon a changé depuis la révision |*revision| — que
  // l'appelant conserve d'une frame à l'autre, initialisée à zéro. Met alors
  // |*revision| à jour et rend true ; rend false SANS RIEN COPIER sinon.
  //
  // La comparaison se fait sous le même verrou que la copie : lire la révision
  // par un accesseur séparé laisserait une ligne s'insérer entre les deux, et le
  // lecteur retiendrait une révision plus récente que le contenu qu'il a pris.
  //
  // ⚠ La copie n'est pas gratuite : jusqu'à `kCapacity` `std::string`. C'est
  // exactement ce que cette garde évite de refaire à chaque frame.
  bool SnapshotIfChanged(std::vector<LogLine>* out, std::uint64_t* revision) const;
  // Drop every buffered line (« Vider » in the in-game log window). Useful to
  // isolate what a single action logs, without restarting the client.
  void Clear();

 private:
  LogLineBuffer() = default;

  mutable std::mutex mutex_;
  std::deque<LogLine> lines_;
  // Avance à CHAQUE modification du tampon — ajout comme vidage. Elle part de
  // zéro, comme la révision que l'appelant garde : un tampon jamais écrit n'est
  // donc jamais recopié, et un tampon vidé l'est (le vidage l'incrémente).
  std::uint64_t revision_ = 0;
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

// LogDiag = diagnostic line at warn level (shown even when LogInfo is filtered).
#define LogDiag(fmt, ...) \
  LogConsole::instance().logger()->warn(fmt, ##__VA_ARGS__)

#define LogError(fmt, ...) \
  LogConsole::instance().logger()->error(fmt, ##__VA_ARGS__)

#ifdef BOURGEON_DEBUG
#define LogDebug(fmt, ...) \
  LogConsole::instance().logger()->debug(fmt, ##__VA_ARGS__)
#else
#define LogDebug(fmt, ...) ((void)0)
#endif  // BOURGEON_DEBUG
