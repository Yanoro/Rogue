#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "AI.h"

class DebugLog;

// Improving a user's own words before they are sent.
//
// The problem this solves: the user knows what they want and types it in six words
// -- "create a rotting wooden floor tile" -- while the model that consumes it would
// do better with the detail left implicit. Asking the user to write the long version
// is asking them to do the model's prompt engineering. So a second, cheap request
// turns their sentence into a fuller one that still means what they meant.
//
// The rules that make that safe rather than presumptuous:
//
//   1. **The user's intent is fixed.** The rewriter clarifies and adds texture. It
//      never adds a requirement the user did not state and never substitutes its own
//      idea for theirs -- those are different features (ideation, not rewriting) and
//      would make the button something you cannot trust with a specific request.
//   2. **Its output is always shown, never sent behind the user's back.** The box
//      holds the result and the original stays one click away (see ImproveInputBox),
//      so a bad rewrite is an annoyance rather than a lost message.
//   3. **Nothing blocks a frame.** A rewrite is a model call of seconds, so it runs
//      on a worker thread and is collected on the main thread -- the same shape the
//      AI pipeline and the tile-art queue use.
//
// It is deliberately general: `Brief` is whatever the *host window* knows that the
// rewriter should, so the same component serves a tile description, a building
// description, or anything with a text box. Nothing here knows about tiles.
class PromptRewriter {
public:
  // What only the calling window knows: the constraints of its domain and the
  // style it is trying to hit.
  //
  // `%BRIEF%` in the prompt file is replaced with this, so a window states its own
  // context without the component carrying a case for each one.
  struct Brief {
    // A short name for the domain, used in log lines. E.g. "tile", "building".
    std::string kind;
    // The prose to inject at `%BRIEF%`. Should say what the rewriter may and may not
    // promise, in that window's terms.
    std::string context;
  };

  PromptRewriter() = default;
  ~PromptRewriter();

  PromptRewriter(const PromptRewriter &) = delete;
  PromptRewriter &operator=(const PromptRewriter &) = delete;

  // `ai` is borrowed and may be null: a window whose backend failed to construct
  // then reports "no rewriter" rather than crashing, the same contract
  // DesignerChatWindow has for its own backend.
  //
  // `promptFile` is the instructions, with `%BRIEF%` and `%TEXT%` placeholders. A
  // missing or unreadable file is reported once and disables the feature -- there is
  // no meaningful fallback for "what should a rewrite be", so guessing one would be
  // worse than not offering the button.
  void Configure(AI *ai, std::string promptFile, std::string contextId,
                 DebugLog *log = nullptr);

  bool Available() const { return ai != nullptr && !instructions.empty(); }
  // Why it is not available, or empty when it is. For a tooltip that says what is
  // missing rather than leaving a dead button.
  std::string UnavailableReason() const;

  // The brief for the calling window. Safe to change between requests.
  void SetBrief(Brief brief) { currentBrief = std::move(brief); }

  // The system prompt as it will actually be sent, for the log and for a headless
  // mode that has to show what was asked. The trailing brief and text are included,
  // because the whole point is that the instructions alone do not determine a
  // rewrite.
  std::string DescribeRequest(const std::string &text) const;

  // One finished rewrite.
  struct Result {
    bool ok = false;
    // The improved text. Empty on failure.
    std::string text;
    // The model's one-line note on what it changed, when it gave one. Shown beside
    // the result so a rewrite is never a silent substitution.
    std::string why;
    std::string error;
    // The original, carried through so a stale result cannot be applied to text the
    // user has since edited -- the caller compares, and the box refuses to overwrite
    // something that changed while the request was in flight.
    std::string original;
    // The reply exactly as it arrived, before it was split into text and note.
    //
    // Carried so the host can write it to its own transcript log. "The box filled
    // with the wrong half" is a parsing question, and the only way to answer it after
    // the fact is to have kept what the model actually said -- which nothing did until
    // a user hit the inverted-order case.
    std::string rawReply;
  };

  // Asks for a rewrite. Returns false when the rewriter is unavailable or a request
  // is already in flight; `reason` says which. Non-blocking: the answer arrives
  // through TakeResult on a later frame.
  bool Request(const std::string &text, std::string *reason = nullptr);

  // True while a request is in flight.
  bool Busy() const;

  // The finished result, once. Called every frame by the host; empty optional means
  // "nothing yet".
  std::optional<Result> TakeResult();

  // Abandons any result and clears `Busy`. For a window being closed, or a request
  // the user superseded. Does not stop a worker that is already talking to the
  // provider -- that request has been paid for either way.
  void Clear();

  // Joins the worker. Called from the host window's destructor through Shutdown,
  // with the same bounded wait TileArtQueue::Stop documents.
  void Stop();

  // How many rewrites have been requested this session, and the last result, for a
  // status line. Cheap enough to poll per frame.
  int RequestsMade() const { return requestsMade; }

private:
  void RunRewrite(std::string text);

  AI *ai = nullptr;
  DebugLog *log = nullptr;
  std::string promptFile;
  std::string instructions;
  std::string contextId;
  Brief currentBrief;

  int requestsMade = 0;

  // The thread boundary. A worker writes `pending` under the mutex; the main thread
  // swaps it out in TakeResult.
  mutable std::mutex mutex;
  std::optional<Result> pending;
  bool inFlight = false;
  std::mutex workerMutex;
  std::condition_variable workerDone;
  // Joinable, not detached: the worker writes its result back into this object, so it
  // must be waited on before this object dies. Stop() does that, and the destructor
  // calls Stop().
  std::thread worker;
  bool workerRunning = false;
};
