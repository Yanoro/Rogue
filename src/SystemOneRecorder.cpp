#include "SystemOneRecorder.h"

#include <filesystem>
#include <fstream>
#include <utility>

#include "TimeUtils.hpp"

namespace SystemOne {
namespace {

// One record as the log file writes it: the same content the window shows, in a
// shape that survives a grep. Multi-line payloads are indented rather than
// escaped, because a hand-tuned prompt is read by a human far more often than it
// is parsed by a tool.
std::string FormatForFile(const SystemOneRecord &record) {
  std::string out;
  out += "=== [" + record.clockTime + "] " + record.scenario;

  if (!record.advisory) {
    // Loud on purpose: a non-advisory call is the one thing in this log that
    // could have changed the game, and it must be findable without reading every
    // line.
    out += "  ** CHANGED BEHAVIOUR (not advisory) **";
  }
  out += "\n";

  if (!record.summary.empty()) out += "summary:  " + record.summary + "\n";
  if (!record.model.empty()) out += "model:    " + record.model + "\n";

  out += "latency:  ";
  if (record.latencyMs < 0) {
    out += "(not measured)\n";
  } else {
    out += std::to_string(record.latencyMs) + "ms\n";
  }

  out += "tokens:   ";
  if (record.promptTokens < 0 && record.outputTokens < 0) {
    out += "(not reported)\n";
  } else {
    out += "prompt " + std::to_string(record.promptTokens) + ", output " +
           std::to_string(record.outputTokens) + "\n";
  }

  if (!record.error.empty()) {
    out += "error:    " + record.error + "\n";
  }

  out += "-- request --\n" + record.requestPayload + "\n";
  out += "-- response --\n" + record.responseText + "\n";

  if (!record.answers.empty()) {
    out += "-- answers --\n";
    for (const auto &[id, answer] : record.answers) {
      // Choice is a string key and the others are numbers, so each is labelled
      // rather than concatenated blind -- a choice of "0" and a probability of 0
      // must not read the same in the log.
      out += "  " + id + ":";
      if (!answer.choice.empty()) out += " choice=" + answer.choice;
      out += " probability " + std::to_string(answer.probability);
      out += ", score " + std::to_string(answer.score);
      out += ", confidence " + std::to_string(answer.confidence);
      if (!answer.probabilities.empty()) {
        out += ", distribution {";
        bool first = true;
        for (const auto &[key, value] : answer.probabilities) {
          if (!first) out += ", ";
          first = false;
          out += key + "=" + std::to_string(value);
        }
        out += "}";
      }
      out += "\n";
    }
  }

  out += "\n";
  return out;
}

}  // namespace

SystemOneRecorder::SystemOneRecorder(std::size_t maxEntries)
    : maxEntries(maxEntries == 0 ? 1 : maxEntries) {}

void SystemOneRecorder::SetWarningSink(
    std::function<void(const std::string &)> sink) {
  std::lock_guard<std::mutex> lock(mutex);
  warningSink = std::move(sink);
}

void SystemOneRecorder::EnsureLogFile() {
  // Caller holds the mutex. Same three-state contract as the designer window's
  // log: empty means "not tried yet", a single space means "tried and failed",
  // anything else is the path.
  if (!logFilePath.empty()) {
    return;
  }

  std::error_code ec;
  // logs/system1/, beside the NPC transcripts and the designer's, in its own
  // directory: System 1 traffic is neither an NPC conversation nor a design
  // session, and mixing them would make all three harder to scan.
  std::filesystem::create_directories("logs/system1", ec);
  if (ec) {
    logFilePath = " ";
    if (!warnedAboutLog && warningSink) {
      warnedAboutLog = true;
      warningSink("System 1: could not create logs/system1: " + ec.message());
    }
    return;
  }

  // One file per run, named for the run's start, so records from one session
  // stay together and sort chronologically.
  logFilePath = "logs/system1/" + TimeUtils::GetStartTimeString() + "-system1.txt";

  std::ofstream header(logFilePath, std::ios::app);
  if (!header.is_open()) {
    logFilePath = " ";
    if (!warnedAboutLog && warningSink) {
      warnedAboutLog = true;
      warningSink("System 1: could not open " + logFilePath + " for writing");
    }
    return;
  }

  // Which model was in use is the first thing a later reader needs, and it is
  // not recoverable from the records alone once the model has been swapped.
  header << "# System 1 call log\n"
         << "# Every request made to the System 1 backend, with its reply.\n"
         << "# 'advisory' calls cannot have changed agent behaviour; a line marked\n"
         << "# CHANGED BEHAVIOUR is a call that could have.\n\n";
}

void SystemOneRecorder::Record(SystemOneRecord record) {
  std::lock_guard<std::mutex> lock(mutex);

  if (!record.advisory) {
    ++nonAdvisoryCount;
  }
  ++scenarioCounts[record.scenario];

  const std::string formatted = FormatForFile(record);
  // Kept in memory even when it is a harness call, so the window can show it, but
  // never written to the file the game's own traffic is judged from.
  const bool writeToFile = !record.harness;

  entries.push_front(std::move(record));
  while (entries.size() > maxEntries) {
    entries.pop_back();
  }

  if (!writeToFile) {
    return;
  }

  EnsureLogFile();
  if (logFilePath.empty() || logFilePath == " ") {
    return;
  }

  std::ofstream out(logFilePath, std::ios::app);
  if (out.is_open()) {
    out << formatted;
  }
}

std::vector<SystemOneRecord> SystemOneRecorder::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex);
  // Copied out under the lock rather than returned by reference: the draw loop
  // must not hold this mutex, and a worker thread may record mid-frame.
  return {entries.begin(), entries.end()};
}

std::size_t SystemOneRecorder::Count() const {
  std::lock_guard<std::mutex> lock(mutex);
  return entries.size();
}

void SystemOneRecorder::Clear() {
  std::lock_guard<std::mutex> lock(mutex);
  // Deliberately does not clear the file: the file is the durable record and the
  // reason this exists. Clearing only drops the in-memory view.
  entries.clear();
  scenarioCounts.clear();
  nonAdvisoryCount = 0;
}

std::unordered_map<std::string, int> SystemOneRecorder::ScenarioCounts() const {
  std::lock_guard<std::mutex> lock(mutex);
  return scenarioCounts;
}

int SystemOneRecorder::NonAdvisoryCount() const {
  std::lock_guard<std::mutex> lock(mutex);
  return nonAdvisoryCount;
}

std::string SystemOneRecorder::LogFilePath() const {
  std::lock_guard<std::mutex> lock(mutex);
  if (logFilePath == " ") return {};
  return logFilePath;
}

}  // namespace SystemOne
