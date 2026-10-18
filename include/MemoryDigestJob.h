#pragma once

// The out-of-band memory digest call (docs/spatial-memory-design.md section 6.3).
//
// One job per character: a worker thread asks the character's own chat backend to
// group a numbered list of memory items, then hands the groups back for the main
// thread to compose. The contract is PromptRewriter's, deliberately -- it is the
// known-good shape in this tree:
//
//   * one request in flight, `Request` refuses while busy;
//   * completion is the empty-token sentinel, never `generateStream` returning;
//   * a bounded wait, because there is no frame to return to on the worker;
//   * the raw reply is logged before it is interpreted.
//
// It differs in what it does with the answer: the model returns groups of item
// numbers, and the engine writes the words (include/MemoryDigest.hpp). No label,
// aspect or fact can be invented because none is accepted from the model.

#include "AI.h"
#include "DebugLog.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class MemoryDigestJob {
public:
  struct Result {
    bool ok = false;
    std::vector<std::vector<int>> groups;
    std::string raw;
    std::string error;
    int candidates = 0;
  };

  MemoryDigestJob() = default;
  ~MemoryDigestJob();

  MemoryDigestJob(const MemoryDigestJob &) = delete;
  MemoryDigestJob &operator=(const MemoryDigestJob &) = delete;

  // `ai` is borrowed and may be null: a character with no backend then reports
  // unavailable rather than offering a request that cannot complete. The prompt
  // file carries the instructions and a %CANDIDATES% placeholder; a missing file
  // disables the feature rather than guessing a prompt.
  void Configure(AI *ai, std::string promptFile, std::string contextId,
                 DebugLog *log = nullptr);

  bool Available() const { return ai != nullptr && !instructions.empty(); }
  std::string UnavailableReason() const;

  // Non-blocking. False when unavailable or already busy; `reason` says which.
  bool Request(const std::vector<std::string> &candidates,
               std::string *reason = nullptr);

  bool Busy() const { return busy.load(); }

  // The finished result, once. Called every frame; empty means "nothing yet".
  std::optional<Result> TakeResult();

  std::string DescribeRequest(const std::vector<std::string> &candidates) const;

  int RequestsMade() const { return requestsMade; }

  void Stop();

private:
  void Run(std::vector<std::string> candidates);

  AI *ai = nullptr;
  DebugLog *log = nullptr;
  std::string instructions;
  std::string contextId;

  std::thread worker;
  mutable std::mutex mutex;
  std::atomic<bool> busy{false};

  std::optional<Result> finished;
  int requestsMade = 0;
};
