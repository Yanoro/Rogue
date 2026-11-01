#pragma once

#include <string>
#include <vector>
#include <deque>
#include <ctime>
#include <sstream>
#include <iomanip>

class DebugLog {
public:
  enum class LogLevel {
    INFO,
    WARNING,
    ERROR,
    DEBUG,
  };

  // What a line is about, orthogonal to its severity. Combat is its own
  // category because it is the one stream a player wants to read without the
  // engine's bookkeeping around it; everything else is "General" and is what
  // the debug toggle and the existing level filters already describe.
  enum class LogCategory {
    General,
    Combat,
  };

  struct LogEntry {
    std::string timestamp;
    LogLevel level;
    LogCategory category = LogCategory::General;
    std::string message;

    LogEntry(const std::string &ts, LogLevel lv, const std::string &msg,
             LogCategory cat = LogCategory::General)
        : timestamp(ts), level(lv), category(cat), message(msg) {}
  };

  DebugLog(size_t maxEntries = 1000);

  void Log(const std::string &message, LogLevel level = LogLevel::INFO,
           LogCategory category = LogCategory::General);
  void LogInfo(const std::string &message);
  void LogWarning(const std::string &message);
  void LogError(const std::string &message);
  void LogDebug(const std::string &message);

  // A combat line. Always INFO: a hit that lands is a fact about the world, not
  // a diagnostic, and severity is what the level filters already sort.
  void LogCombat(const std::string &message);

  const std::deque<LogEntry> &GetEntries() const { return entries; }

  void Clear() { entries.clear(); }

  size_t GetMaxEntries() const { return maxEntries; }

  // Keep old entries instead of dropping one on every append.
  //
  // The ring buffer trims the front when it is full, so even a viewer that is not
  // following the tail sees every visible line shift up whenever a message
  // arrives. The Debug Log window turns this on while Auto-scroll is off, which
  // is what makes "the text stays where I put it" true rather than nearly true.
  //
  // Bounded (see RetentionLimit in DebugLog.cpp) so a window left open for hours
  // cannot grow without limit. Turning it off trims back to maxEntries at once,
  // resuming normal ring-buffer behaviour.
  void SetRetainOldEntries(bool retain);
  bool IsRetainingOldEntries() const { return retainOldEntries; }

  // Short label for a level ("INFO", "WARN", "ERR", "DBG"). Public because the
  // Debug Log window prints it beside each entry: once entries can be filtered
  // by level, the colour alone is no longer enough to say what class a line is.
  // Returns a literal rather than a std::string because the window calls it for
  // every visible entry on every frame.
  const char *GetLevelString(LogLevel level) const;

private:
  std::deque<LogEntry> entries;
  size_t maxEntries;
  bool retainOldEntries = false;

  std::string GetCurrentTimestamp() const;
  size_t RetentionLimit() const;
};
