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

  struct LogEntry {
    std::string timestamp;
    LogLevel level;
    std::string message;

    LogEntry(const std::string &ts, LogLevel lv, const std::string &msg)
        : timestamp(ts), level(lv), message(msg) {}
  };

  DebugLog(size_t maxEntries = 1000);

  void Log(const std::string &message, LogLevel level = LogLevel::INFO);
  void LogInfo(const std::string &message);
  void LogWarning(const std::string &message);
  void LogError(const std::string &message);
  void LogDebug(const std::string &message);

  const std::deque<LogEntry> &GetEntries() const { return entries; }

  void Clear() { entries.clear(); }

  size_t GetMaxEntries() const { return maxEntries; }

private:
  std::deque<LogEntry> entries;
  size_t maxEntries;

  std::string GetCurrentTimestamp() const;
  std::string GetLevelString(LogLevel level) const;
};
