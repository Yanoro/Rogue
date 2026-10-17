#include "PromptRewriter.hpp"

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "DebugLog.h"

namespace {

// How long Stop() waits for a worker. A rewrite is a short request, but a provider
// can be slow and a game that will not close is its own bug -- the same bounded
// compromise TileArtQueue::Stop makes, for the same reason.
constexpr int kStopTimeoutSeconds = 20;

// Pulls the rewritten text and the `WHY:` note apart.
//
// The instructions ask for the description first and the note second, and the model
// normally obliges. This does not assume it, because when it guesses wrong the user
// gets an explanation in their text box instead of the prompt they asked for -- which
// is why this is not a two-line `substr`.
//
// The rule, in one line: **everything from the marker to the end of its line is the
// note, and the description is whatever is left.**
//
// That single rule covers the cases that matter, and it is worth stating why it is
// phrased that way rather than as "split at the marker":
//
//   * `description\nWHY: note` -- the note is the rest, the description is the rest
//     minus it. The ordinary case.
//   * `WHY: note\ndescription` -- the note is its line, so the description survives.
//     Taking the text *after* the marker as the description would have handed the user
//     the note here; taking the text *before* it would have handed them nothing.
//   * `...which is why: the boards...` inside a description -- only a marker at the
//     start of a line counts, so a sentence containing the word does not split
//     anything.
//   * `Here is a fuller description:` before the real one -- a leading announcement is
//     dropped, because that sentence is otherwise exactly what lands in the box.
//
// A missing note is a cosmetic loss, never a reason to discard a good rewrite.
void SplitWhy(const std::string &reply, std::string &text, std::string &why) {
  auto trim = [](std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
  };

  const std::string trimmed = trim(reply);

  // The marker, preferred where a model starts a line with it. Matching from a line
  // start is what makes the rule above safe: a description may contain the word.
  const std::string marker = "WHY:";
  constexpr size_t kMarkerLength = 4;
  size_t at = std::string::npos;
  {
    std::string upper = trimmed;
    for (char &c : upper) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    size_t search = 0;
    while ((search = upper.find(marker, search)) != std::string::npos) {
      const bool atLineStart = search == 0 || upper[search - 1] == '\n';
      if (atLineStart) {
        at = search;
        break;
      }
      search += kMarkerLength;
    }
    if (at == std::string::npos) {
      // No line-start marker: fall back to any occurrence, which is the ordinary case
      // for a model that writes "description WHY: note" on one line.
      at = upper.rfind(marker);
    }
  }

  if (at == std::string::npos) {
    text = trimmed;
    why.clear();
  } else {
    // The note runs to the end of its own line and no further, so a description placed
    // after it survives.
    const size_t noteStart = at + kMarkerLength;
    const size_t noteEnd = trimmed.find('\n', noteStart);
    why = trim(trimmed.substr(noteStart, noteEnd == std::string::npos
                                               ? std::string::npos
                                               : noteEnd - noteStart));
    std::string description = trimmed.substr(0, at);
    if (noteEnd != std::string::npos) {
      description += "\n" + trimmed.substr(noteEnd + 1);
    }
    text = trim(std::move(description));
  }

  // A leading announcement is not a description. Dropped only when a newline or a colon
  // ends it, and only when what remains can stand alone, so a description that begins
  // with such a phrase keeps its opening words.
  static const char *kLeadIns[] = {"HERE IS",    "HERE'S",      "IMPROVED",
                                   "REWRITTEN",  "SURE,",       "CERTAINLY,",
                                   "OF COURSE,", "A REWRITTEN", "THE IMPROVED",
                                   "AN IMPROVED"};
  std::string upperText = text;
  for (char &c : upperText) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  constexpr size_t kMinimumDescription = 24;
  for (const char *lead : kLeadIns) {
    if (upperText.rfind(lead, 0) != 0) continue;
    const size_t newline = text.find('\n');
    const size_t colon = text.find(':');
    size_t cut = std::string::npos;
    if (colon != std::string::npos && (newline == std::string::npos || colon < newline)) {
      cut = colon + 1;
    } else if (newline != std::string::npos) {
      cut = newline + 1;
    }
    if (cut != std::string::npos && cut < text.size()) {
      const std::string remainder = trim(text.substr(cut));
      if (remainder.size() >= kMinimumDescription) {
        text = remainder;
      }
    }
    break;
  }
}

// Replaces every occurrence of `placeholder` in `text`.
void Substitute(std::string &text, const std::string &placeholder,
                const std::string &value) {
  size_t at = text.find(placeholder);
  while (at != std::string::npos) {
    text.replace(at, placeholder.size(), value);
    at = text.find(placeholder, at + value.size());
  }
}

}  // namespace

PromptRewriter::~PromptRewriter() { Stop(); }

void PromptRewriter::Configure(AI *backend, std::string file, std::string id,
                               DebugLog *logger) {
  ai = backend;
  promptFile = std::move(file);
  contextId = std::move(id);
  log = logger;
  instructions.clear();

  if (promptFile.empty()) {
    return;
  }
  std::ifstream in(promptFile);
  if (!in.is_open()) {
    if (log) {
      log->LogWarning("Rewriter: could not read " + promptFile +
                      "; the Improve button is unavailable.");
    }
    return;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  instructions = buffer.str();
}

std::string PromptRewriter::UnavailableReason() const {
  if (ai == nullptr) {
    return "no model backend is configured for this window";
  }
  if (instructions.empty()) {
    return "the rewriter instructions could not be read (" + promptFile + ")";
  }
  return {};
}

std::string PromptRewriter::DescribeRequest(const std::string &text) const {
  std::string prompt = instructions;
  Substitute(prompt, "%BRIEF%", currentBrief.context);
  Substitute(prompt, "%TEXT%", text);
  return prompt;
}

bool PromptRewriter::Request(const std::string &text, std::string *reason) {
  auto refuse = [reason](const std::string &why) {
    if (reason != nullptr) *reason = why;
    return false;
  };

  const std::string unavailable = UnavailableReason();
  if (!unavailable.empty()) {
    return refuse(unavailable);
  }
  if (text.empty()) {
    return refuse("there is nothing to improve yet");
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (inFlight) {
      return refuse("a rewrite is already in progress");
    }
    inFlight = true;
    // The previous result is dropped, not kept: taking a second look supersedes the
    // first, and applying a stale rewrite over text the user has since changed is the
    // one outcome this feature must not have.
    pending.reset();
  }
  ++requestsMade;

  {
    std::lock_guard<std::mutex> lock(workerMutex);
    // A previous run that has finished is joined here rather than detached: the
    // worker writes back into this object, so it must not outlive it, and a joinable
    // thread can be waited on by Stop while a detached one can only be hoped for.
    if (worker.joinable()) {
      worker.join();
    }
    workerRunning = true;
  }
  worker = std::thread([this, text]() {
    RunRewrite(text);
    {
      std::lock_guard<std::mutex> lock(workerMutex);
      workerRunning = false;
    }
    workerDone.notify_all();
  });
  return true;
}

void PromptRewriter::RunRewrite(std::string text) {
  Result result;
  result.original = text;

  AI *backend = ai;
  if (backend == nullptr) {
    result.error = "no model backend is configured for this window";
    std::lock_guard<std::mutex> lock(mutex);
    pending = std::move(result);
    inFlight = false;
    return;
  }

  // Its own provider context, reset per request, for the same reason the designers
  // do it: the history is sent explicitly and a leftover context would replay the
  // previous rewrite as if it were part of this one.
  backend->resetContext(contextId);

  std::vector<ChatMessage> request;
  request.push_back({"system", DescribeRequest(text)});
  // A user turn is required, not cosmetic. A request carrying only a system message
  // makes at least one backend answer with reasoning and *no* content: measured on
  // deepseek-v4-flash, a system-only call returned `content: None` with 5 completion
  // tokens billed, while the same instructions with the text as a user turn returned
  // the answer -- and the model also stopped wrapping its reply in a preamble about
  // ways to describe the thing, which the system-only form invited. The system prompt
  // says what a rewrite is; this says which text.
  request.push_back({"user", text});

  // Completion is the empty-token sentinel, never `generateStream` returning.
  //
  // This is the one handoff rule every backend in this repo has, and getting it wrong
  // is invisible until it is not: `generateStream` detaches its own transfer thread
  // (OllamaAI explicitly, OpenRouterAI explicitly) and returns as soon as the request
  // is *accepted*, so a reply read at that point is always empty. The designer windows
  // avoid this only by accident -- they inspect on a later frame, by which time tokens
  // have arrived. This worker has no later frame, so it waits for the sentinel.
  std::atomic<bool> streamFinished{false};
  std::string reply;
  const bool delivered = backend->generateStream(
      contextId, request, [&reply, &streamFinished](const std::string &token) {
        if (!token.empty()) {
          reply += token;
          return;
        }
        // Empty token: the end of the stream.
        streamFinished.store(true);
      });

  if (!delivered) {
    result.error = "the backend could not accept the rewrite request";
  } else {
    // Bounded, and generously: a rewrite is a short request, but a cold model or a
    // slow provider can take a while and there is no frame to return to. Giving up
    // early would report "the model returned nothing" for a reply that was still on
    // its way, which is precisely the bug this replaced.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(180);
    while (!streamFinished.load() && std::chrono::steady_clock::now() < deadline) {
      // Stop requests are not plumbed through here: a rewrite is short enough that
      // the bound is the whole story, and cancelling mid-write would leave the
      // provider's context in an unknown state for the next request.
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!streamFinished.load()) {
      result.error = "the rewrite timed out";
    } else if (reply.empty()) {
      result.error = "the model returned nothing";
    } else {
      SplitWhy(reply, result.text, result.why);
      // Kept whether or not the split looks right, so the host can log what the model
      // actually said and a wrong half can be diagnosed from the file rather than by
      // reproducing it.
      result.rawReply = reply;
      if (result.text.empty()) {
        result.error = "the model returned no description";
      } else {
        result.ok = true;
      }
    }
  }

  if (log) {
    // The raw reply is logged before it is interpreted, and the two parsed halves
    // after. "The box filled with the wrong half" is a parsing question, and without
    // the raw reply there is no way to answer it after the fact -- which is exactly
    // how the inverted-order case survived until a user hit it.
    log->LogInfo("Rewriter raw reply (" + std::to_string(reply.size()) + " bytes):\n" +
                 reply);
    if (result.ok) {
      log->LogInfo("Rewriter: rewrote " + std::to_string(text.size()) +
                   " chars into " + std::to_string(result.text.size()) + "\n" +
                   "  text: " + result.text + "\n" +
                   "  why:  " + (result.why.empty() ? "(none)" : result.why));
    } else {
      log->LogWarning("Rewriter: failed: " + result.error);
    }
  }

  std::lock_guard<std::mutex> lock(mutex);
  pending = std::move(result);
  inFlight = false;
}

bool PromptRewriter::Busy() const {
  std::lock_guard<std::mutex> lock(mutex);
  return inFlight;
}

std::optional<PromptRewriter::Result> PromptRewriter::TakeResult() {
  std::lock_guard<std::mutex> lock(mutex);
  if (!pending.has_value()) return std::nullopt;
  std::optional<Result> taken = std::move(pending);
  pending.reset();
  return taken;
}

void PromptRewriter::Clear() {
  std::lock_guard<std::mutex> lock(mutex);
  pending.reset();
  // `inFlight` is deliberately not cleared: a worker that is still running will
  // write its own result and clear the flag itself. Clearing it here would let a
  // second request start while the first is in flight on the same context, which is
  // exactly what resetContext must not have done to it.
}

void PromptRewriter::Stop() {
  std::thread pending;
  {
    std::unique_lock<std::mutex> lock(workerMutex);
    if (!workerRunning) {
      // Nothing in flight. Join anything left over from a finished run so the thread
      // does not outlive the object.
      if (worker.joinable()) {
        pending = std::move(worker);
      }
    } else {
      // Wait for the worker to finish before joining. Bounded inside RunRewrite, so
      // this cannot block forever. The timeout here is informational -- the join below
      // waits regardless -- and exists so a slow provider is reported rather than
      // silently extending shutdown.
      workerDone.wait_for(lock, std::chrono::seconds(kStopTimeoutSeconds),
                          [this]() { return !workerRunning; });
      if (worker.joinable()) {
        pending = std::move(worker);
      }
    }
  }
  if (pending.joinable()) {
    pending.join();
  }
}
