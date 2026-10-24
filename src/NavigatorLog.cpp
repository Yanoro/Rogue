#include "NavigatorLog.h"

#include "LogPaths.h"
#include "TimeUtils.hpp"

#include <fstream>
#include <sstream>
#include <string>

NavigatorLog::NavigatorLog(std::string agent, std::uint64_t entityId)
    : agent(std::move(agent)),
      path(LogPaths::NavigatorFile(this->agent, entityId).string()) {}

void NavigatorLog::Write(const std::string &text) {
  if (failed || text.empty()) {
    return;
  }
  std::ofstream out(path, std::ios::app);
  if (!out.is_open()) {
    failed = true;
    return;
  }
  out << text;
}

void NavigatorLog::Begin(const std::string &goal) {
  if (begun) {
    return;
  }
  begun = true;
  std::ostringstream out;
  out << "\n=== navigator episode [" << TimeUtils::GetCurrentTimeString()
      << "] goal: " << goal << " ===\n"
      << "# session: " << LogPaths::SessionRoot().string() << "\n"
      << "# agent: " << agent << "\n";
  Write(out.str());
}

void NavigatorLog::Handoff(const std::string &reason) {
  Write("--- handoff [" + TimeUtils::GetCurrentTimeString() + "]: " + reason +
        " ---\n");
}

void NavigatorLog::Request(const std::string &reason, int attempt,
                           int maxAttempts, const std::string &prompt) {
  std::ostringstream out;
  out << "\n=== strategist request [" << TimeUtils::GetCurrentTimeString()
      << "] reason: " << reason << ", attempt " << attempt << "/"
      << maxAttempts << " ===\n"
      << "-- prompt --\n"
      << prompt << "\n";
  Write(out.str());
}

void NavigatorLog::Response(const std::string &response) {
  Write("\n-- response --\n" + response + "\n");
}

void NavigatorLog::Partial(const std::string &partial) {
  Write("\n-- partial/cut-off response --\n" + partial + "\n");
}

void NavigatorLog::Error(const std::string &error) {
  Write("-- error: " + error + " --\n");
}

void NavigatorLog::Retry(const std::string &reason, int retry) {
  Write("-- retry " + std::to_string(retry) + ": " + reason + " --\n");
}

void NavigatorLog::Plan(const std::string &summary) {
  Write("-- plan: " + summary + " --\n");
}

void NavigatorLog::S1(const std::string &state, const std::string &answer,
                      float confidence) {
  std::ostringstream out;
  out << "\n=== s1 [" << TimeUtils::GetCurrentTimeString() << "] ===\n"
      << "-- state --\n"
      << state << "\n"
      << "-- answer: " << answer;
  if (confidence >= 0.0f) {
    out << " (confidence " << confidence << ")";
  }
  out << " --\n";
  Write(out.str());
}

void NavigatorLog::Leg(const std::string &key, const std::string &label, int x,
                       int y) {
  Write("--- leg: " + key + " " + label + " -> (" + std::to_string(x) + ", " +
        std::to_string(y) + ") ---\n");
}

void NavigatorLog::Summary(const std::string &summary) {
  Write("\n-- summary --\n" + summary + "\n");
}
