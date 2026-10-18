#include "MemoryDigestJob.h"

#include "MemoryDigest.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

namespace {

std::string ReadFile(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

}  // namespace

MemoryDigestJob::~MemoryDigestJob() { Stop(); }

void MemoryDigestJob::Configure(AI *backend, std::string promptFile,
                                std::string id, DebugLog *debugLog) {
  ai = backend;
  contextId = std::move(id);
  log = debugLog;
  instructions = ReadFile(promptFile);
  if (instructions.empty() && log) {
    log->LogWarning("Memory digest: no prompt at '" + promptFile +
                    "', so the feature stays off.");
  }
}

std::string MemoryDigestJob::UnavailableReason() const {
  if (ai == nullptr) {
    return "no chat backend";
  }
  if (instructions.empty()) {
    return "no prompt file";
  }
  return {};
}

std::string MemoryDigestJob::DescribeRequest(
    const std::vector<std::string> &candidates) const {
  std::string list;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    list += std::to_string(i) + ". " + candidates[i] + "\n";
  }
  std::string text = instructions;
  const std::size_t marker = text.find("%CANDIDATES%");
  if (marker != std::string::npos) {
    text.replace(marker, std::string("%CANDIDATES%").size(), list);
  }
  return text;
}

bool MemoryDigestJob::Request(const std::vector<std::string> &candidates,
                              std::string *reason) {
  if (!Available()) {
    if (reason) {
      *reason = UnavailableReason();
    }
    return false;
  }
  if (busy.load()) {
    if (reason) {
      *reason = "a request is already in flight";
    }
    return false;
  }

  // Join the previous worker before replacing it: a finished thread still holds
  // its resources, and this is the only place they are reaped.
  if (worker.joinable()) {
    worker.join();
  }
  busy.store(true);
  {
    std::lock_guard<std::mutex> lock(mutex);
    finished.reset();
  }
  ++requestsMade;
  worker = std::thread([this, candidates]() { Run(candidates); });
  return true;
}

void MemoryDigestJob::Run(std::vector<std::string> candidates) {
  Result result;
  result.candidates = static_cast<int>(candidates.size());

  const std::string prompt = DescribeRequest(candidates);
  std::vector<ChatMessage> request;
  request.push_back({"system", instructions});
  // The list goes in a user turn, not the system turn: measured on
  // deepseek-v4-flash, a system-only request can return no content at all, and
  // PromptRewriter documents the same lesson.
  std::string user;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    user += std::to_string(i) + ". " + candidates[i] + "\n";
  }
  request.push_back({"user", user});

  std::atomic<bool> streamFinished{false};
  std::string reply;
  const bool delivered = ai->generateStream(
      contextId, request, [&reply, &streamFinished](const std::string &token) {
        if (!token.empty()) {
          reply += token;
          return;
        }
        streamFinished.store(true);
      });

  if (!delivered) {
    result.error = "the backend could not accept the request";
  } else {
    // Bounded, and generously: a digest is a short request, but a cold model can
    // take a while and the worker has no frame to return to.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!streamFinished.load() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!streamFinished.load()) {
      result.error = "the digest request timed out";
    } else if (reply.empty()) {
      result.error = "the model returned nothing";
    } else {
      result.raw = reply;
      result.groups = MemoryDigestBuilder::ParseGroups(
          reply, static_cast<std::size_t>(candidates.size()));
      if (result.groups.empty()) {
        // Carry a snippet of what actually arrived: "the model returned nothing
        // useful" is unactionable, "it returned 12 tokens of prose" is a prompt
        // problem, and "it stopped mid-array" is a budget problem.
        std::string snippet = reply.substr(0, 160);
        for (char &c : snippet) {
          if (c == '\n' || c == '\r') c = ' ';
        }
        result.error = "no group of item numbers in " +
                       std::to_string(reply.size()) + " chars: " + snippet;
      } else {
        result.ok = true;
      }
    }
  }

  if (log) {
    log->LogInfo("Memory digest raw reply (" + std::to_string(reply.size()) +
                 " bytes):\n" + reply);
    if (result.ok) {
      log->LogInfo("Memory digest: " + std::to_string(candidates.size()) +
                   " candidates -> " + std::to_string(result.groups.size()) +
                   " groups");
    } else {
      log->LogWarning("Memory digest: failed: " + result.error);
    }
  }

  bool wasBusy = busy.load();
  {
    std::lock_guard<std::mutex> lock(mutex);
    finished = std::move(result);
  }
  busy.store(false);
  (void)wasBusy;
}

std::optional<MemoryDigestJob::Result> MemoryDigestJob::TakeResult() {
  if (busy.load()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex);
  if (!finished.has_value()) {
    return std::nullopt;
  }
  std::optional<Result> result = std::move(finished);
  finished.reset();
  return result;
}

void MemoryDigestJob::Stop() {
  if (worker.joinable()) {
    worker.join();
  }
  busy.store(false);
}
