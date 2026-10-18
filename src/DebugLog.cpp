#include "DebugLog.h"

#include <algorithm>

namespace {

// How far the log may grow while a viewer is reading without following the tail.
// The floor keeps a small maxEntries setting from making retention meaningless,
// the factor keeps a large configured capacity proportionally generous. At the
// default 1000 this allows 50k entries of a few dozen bytes each -- a handful of
// megabytes, and only while the Debug Log window is open with Auto-scroll off.
constexpr size_t kRetentionFloor = 50000;
constexpr size_t kRetentionFactor = 20;

}  // namespace

DebugLog::DebugLog(size_t maxEntries) : maxEntries(maxEntries) {}

size_t DebugLog::RetentionLimit() const {
  return std::max(maxEntries * kRetentionFactor, kRetentionFloor);
}

void DebugLog::SetRetainOldEntries(bool retain) {
  if (retainOldEntries == retain) {
    return;
  }
  retainOldEntries = retain;
  if (!retain) {
    while (entries.size() > maxEntries) {
      entries.pop_front();
    }
  }
}

void DebugLog::Log(const std::string &message, LogLevel level) {
  std::string timestamp = GetCurrentTimestamp();
  entries.emplace_back(timestamp, level, message);

  const size_t limit = retainOldEntries ? RetentionLimit() : maxEntries;
  while (entries.size() > limit) {
    entries.pop_front();
  }
}

void DebugLog::LogInfo(const std::string &message) {
  Log(message, LogLevel::INFO);
}

void DebugLog::LogWarning(const std::string &message) {
  Log(message, LogLevel::WARNING);
}

void DebugLog::LogError(const std::string &message) {
  Log(message, LogLevel::ERROR);
}

void DebugLog::LogDebug(const std::string &message) {
  Log(message, LogLevel::DEBUG);
}

std::string DebugLog::GetCurrentTimestamp() const {
  auto now = std::time(nullptr);
  auto *tm = std::localtime(&now);
  std::ostringstream oss;
  oss << std::put_time(tm, "%H:%M:%S");
  return oss.str();
}

const char *DebugLog::GetLevelString(LogLevel level) const {
  switch (level) {
  case LogLevel::INFO:
    return "INFO";
  case LogLevel::WARNING:
    return "WARN";
  case LogLevel::ERROR:
    return "ERR";
  case LogLevel::DEBUG:
    return "DBG";
  default:
    return "UNKNOWN";
  }
}
