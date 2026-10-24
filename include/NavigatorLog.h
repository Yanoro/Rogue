#pragma once

#include <cstdint>
#include <fstream>
#include <string>

// One agent's navigator transcript: the outer strategist request/response
// (including a partial or cut-off stream), parse errors and retries, the parsed
// plan, handoffs, the S1 state/answer, legs and the final summary.
//
// It lives at <session>/navigator/<Agent>.txt. The session directory is created
// lazily by LogPaths.
class NavigatorLog {
public:
  NavigatorLog(std::string agent, std::uint64_t entityId);

  void Begin(const std::string &goal);
  void Handoff(const std::string &reason);
  void Request(const std::string &reason, int attempt, int maxAttempts,
               const std::string &prompt);
  void Response(const std::string &response);
  void Partial(const std::string &partial);
  void Error(const std::string &error);
  void Retry(const std::string &reason, int retry);
  void Plan(const std::string &summary);
  void S1(const std::string &state, const std::string &answer,
          float confidence);
  void Leg(const std::string &key, const std::string &label, int x, int y);
  void Summary(const std::string &summary);

private:
  void Write(const std::string &text);
  std::string agent;
  std::string path;
  bool begun = false;
  bool failed = false;
};
