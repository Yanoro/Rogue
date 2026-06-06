#include "DebugLog.h"

DebugLog::DebugLog(size_t maxEntries) : maxEntries(maxEntries) {}

void DebugLog::Log(const std::string &message, LogLevel level) {
  std::string timestamp = GetCurrentTimestamp();
  entries.emplace_back(timestamp, level, message);

  if (entries.size() > maxEntries) {
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

std::string DebugLog::GetLevelString(LogLevel level) const {
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
