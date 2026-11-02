#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "AI.h"
#include "SpeechQueue.h"

class Game;
struct TileDef;

// The reply-text utilities every designer conversation needs.
//
// These were duplicated -- sometimes byte for byte, sometimes differing by one word --
// between the two windows that were later merged. They live here so that the
// windows, and the streaming path they share, cannot disagree about what a reply says.
namespace DesignerReply {

// Strips reasoning blocks ("<think>...</think>") from a complete reply.
//
// A pure function over the whole text rather than a stateful filter over the token
// stream, and that is the point: the raw reply is what gets stored, so turning the
// thoughts back on is possible. A streaming filter is lossy -- once the reasoning has
// been dropped there is nothing left to show -- which is fine when hiding is mandatory
// and wrong when it is a checkbox.
//
// The scanner still has to be careful in the same way a stream filter is, because a '<'
// can be literal text ("a < b") and a tag can be adjacent to one ("a<think>x</think>b").
// A '<' is only treated as a tag start when the following characters actually spell one.
std::string StripReasoning(const std::string &text);

// The language tag that opens the fenced block a designer must produce.
inline constexpr const char *kJsonFence = "```json";

// The reply with its fenced json block replaced by `marker`, for when the user has
// turned JSON display off.
//
// Only the *display* is filtered: the engine still parses and saves the block
// regardless, so hiding it cannot change what the designer produced.
//
// `marker` is per kind ("blueprint hidden", "sprite hidden") rather than a constant
// because the old duplicate in the sprite window said "tile hidden" even on an object,
// which is exactly the kind of thing two copies produce.
std::string HideJsonBlock(const std::string &text, const std::string &marker);

// The first fenced json block, or empty when there is none. Deliberately tolerant about
// what surrounds it, because models like to add a closing remark, and strict about the
// fence, because that is the one thing the prompt is unambiguous about.
std::string ExtractJsonBlock(const std::string &text);

// The reply's prose: everything before the fenced block. Trimmed, because the fence
// sits on its own line and the prose would otherwise end with the blank line that
// separated them -- which a speech engine reads as a pause.
std::string ProseBeforeJson(const std::string &text);

// What the transcript shows: reasoning and/or JSON hidden according to the two
// checkboxes. Both hidden forms are still fully present in the stored reply, which is
// why toggling either checkbox reveals them retroactively.
std::string DisplayAnswer(const std::string &answer, bool showThoughts, bool showJson,
                          const std::string &marker);

}  // namespace DesignerReply

// The shared conversation shell: one transcript, N conversations, one streaming
// handoff, one log, one speech queue.
//
// Why it exists. The blueprint window and the sprite window had grown the same ~450
// lines of chat plumbing each -- exchange storage, the history cap, the detached-thread
// generateStream call and its empty-token sentinel, the transcript loop, the log, the
// attempt budget, the speech transport bar. Several of those blocks were byte-identical
// apart from a directory name; two were identical apart from one word. The comments in
// both copies called the other one "the proven implementation of the same handoff",
// which is a description of a copy-paste, not of a design.
//
// What it deliberately is NOT: a router, a domain table, or a state machine over the
// kinds of thing a designer can make. It knows about *conversations*, not about tiles,
// objects or buildings. Applying a reply, choosing the retry wording and previewing the
// result stay in the windows, because those are the parts that genuinely differ -- and
// because a session that knew about all of them would have to know about every future
// kind as well.
class DesignerSession {
public:
  // One request/response attempt. A retry is a new entry, so the transcript reads as
  // the conversation that actually happened.
  //
  // One struct rather than two + a variant: the fields a given domain does not use stay
  // empty, which is the same trade the object/tile descriptor makes elsewhere. What is
  // kept apart is the *meaning* of each field, documented per group below, because the
  // failure this replaces was two structs whose common half drifted apart silently.
  struct Exchange {
    // The sprite-document request this exchange answers, when it is one. Inspection runs
    // after later requests may already have been sent, so the identity travels with the
    // exchange instead of being read from the window's current-request brief -- reading
    // the brief is how a reply for one rig layer used to be written under another's name.
    // Kind is an int to keep this header free of the sprite-kinds enum; -1 means "not a
    // sprite-document turn".
    std::string artName;
    int artKind = -1;

    // ---- which conversation this turn belongs to -------------------------------
    //
    // An index into the session's conversation table. Replaces the `isArtDocument`
    // bool the sprite window carried: a bool cannot describe a third conversation, and
    // step 2 of the designer merge adds one.
    int conversation = 0;

    // ---- transport -------------------------------------------------------------
    std::string prompt;
    // The reply exactly as it arrived, reasoning blocks included. Kept raw so the "Show
    // thinking" checkbox can reveal them; everything that reasons about the reply
    // (parsing, validation, history) uses ParsedAnswer() instead.
    std::string answer;
    bool done = false;
    bool failed = false;
    std::string error;
    // The user's own message, as opposed to an engine retry prompt.
    bool fromUser = false;

    // ---- verdict ---------------------------------------------------------------
    //
    // Filled on the main thread once the stream finishes, by the window's inspector
    // (through DesignerSession::Update). `ok` is what a transcript reads as success;
    // `failureReason` is the sentence fed back to the model.
    bool inspected = false;
    bool ok = false;
    std::string failureReason;
    // What was created, and where it landed. `name` is a blueprint id, a tile name or
    // an object key depending on the domain; the transcript is the only place that
    // distinguishes them, and it does so per domain.
    std::string name;
    std::string savedPath;
    bool overwrote = false;
    bool saveFailed = false;
    // The model's own `designerNotes`, which every designer is required to produce.
    std::string notes;
    std::vector<std::string> warnings;

    // ---- sprite domains only ---------------------------------------------------
    //
    // Empty/zero for a blueprint reply, which has no set shape and no art of its own.
    bool wasSet = false;
    std::string setName;
    std::vector<std::string> acceptedNames;
    // One pair per entry: accepted, and the line to show. The flag is kept rather than
    // inferred from the text -- comparing a display string's prefix to decide a verdict
    // is the kind of coupling that breaks the first time the wording changes.
    std::vector<std::pair<bool, std::string>> entryVerdicts;
    // What happened about the reply's *art*, in one line, shown under the verdict.
    std::string artNote;
    // The definition this attempt produced, for the preview pane. Null when the reply
    // never got as far as a definition. Borrowed, not owned: a TileRegistry definition
    // outlives the session; the object half keeps a copy in the window for the reason
    // its own comment gives.
    const TileDef *definition = nullptr;

    // ---- speech ----------------------------------------------------------------
    //
    // `speechId` is the handle the transport bar drives -- kNoSpeechClip when nothing
    // was spoken (speech off, muted, or this attempt was an intermediate refusal that
    // stays silent), in which case the bar offers to speak `spokenText` on demand.
    // `spokenText` is kept for exactly that, and because it is the only record of what
    // was actually read aloud.
    SpeechClipId speechId = kNoSpeechClip;
    std::string spokenText;

    // The reply as the engine reads it when validating: reasoning removed, so the model
    // cannot talk itself past the contract, and the JSON left in place because that is
    // what is being extracted.
    std::string ParsedAnswer() const;
    // What goes back to the model as its own previous turn: reasoning kept (a model
    // revising works better with its earlier thinking in view), the JSON block dropped
    // so a whole definition is not replayed as if fresh. `hiddenMarker` is the session's.
    std::string HistoryAnswer(const std::string &hiddenMarker) const;
  };

  // `backend` is borrowed: Game owns it for the whole run. A null pointer is a supported
  // state (the designer backend failed to construct), and the windows report it rather
  // than crashing.
  //
  // `logDirectory` and `logSuffix` are what the two windows differ by in their logs
  // ("designer"/"-designer.txt", "tile-designer"/"-tile-designer.txt"). They stay apart
  // on purpose: the two conversations are about different vocabularies, and mixing them
  // would make the folder harder to scan. `label` is the word debug messages start with
  // ("Designer", "Sprite designer").
  //
  // `hiddenMarker` is what replaces a JSON block when the transcript hides it
  // ("blueprint hidden", "sprite hidden"). A parameter rather than something derived from
  // `label`: two strings that have to agree is how the old duplicate came to say "tile
  // hidden" under an object.
  DesignerSession(Game *game, AI *backend, std::string modelLabel,
                  std::string logDirectory, std::string logSuffix, std::string label,
                  std::string hiddenMarker);

  // ---- conversations ---------------------------------------------------------
  //
  // A conversation is a system prompt, a history and a provider context. The sprite
  // window needs two (definitions and pixel documents) and the blueprint window one, for
  // the same reason in both: a document must not be replayed into the next definition,
  // and a definition prompt must not be replayed into a document request.
  //
  // The returned index is stable for the run and is what an Exchange carries.
  int AddConversation(std::string contextId, std::string systemPrompt);

  // Rebuilds the system prompt of a conversation. Called when the kind, the shape or the
  // live vocabulary it is generated from changes -- the prompt is a snapshot of the
  // catalog, so it is stale the moment a design is accepted.
  void SetSystemPrompt(int conversation, std::string prompt);
  const std::string &SystemPrompt(int conversation) const;
  // The provider-side context id, for the session header in the log.
  const std::string &ContextId(int conversation) const;

  // ---- requests --------------------------------------------------------------
  //
  // Creates the exchange, snapshots the conversation, and hands the request to the
  // backend on a detached thread. Returns false without sending when there is no
  // backend, the prompt is empty, or a request is already in flight.
  //
  // Completion is the provider's empty-token sentinel and nothing else. OllamaAI's
  // generateStream dispatches its own thread and returns as soon as the request is
  // accepted, so treating the return as completion marks the exchange done while the
  // answer is still empty.
  bool Send(const std::string &prompt, int conversation, bool fromUser,
            bool wasSet = false);

  bool IsBusy() const;
  size_t Size() const;
  // Copies, for callers that must not hold the lock while drawing or inspecting.
  Exchange Get(size_t index) const;
  std::vector<Exchange> Snapshot() const;
  // Reads, mutates and writes an exchange under the transcript lock. The only way an
  // inspector publishes a verdict: writing fields on a copy is the bug this shape
  // exists to prevent.
  void Update(size_t index, const std::function<void(Exchange &)> &write);
  // Appends an exchange without sending it. For the headless self-test, which supplies
  // the reply instead of streaming it.
  size_t Append(Exchange exchange);
  // Records a request the session did not send: the "SENT (request N): ..." line, the
  // history-cap accounting and the clock the reply is measured against. `Send` calls it;
  // the self-test calls it with its synthetic request.
  void NoteSend(const std::vector<ChatMessage> &request);

  // ---- one refusal, one retry ------------------------------------------------
  //
  // The tail every inspector shares: log the refusal, spend one attempt, and either send
  // a corrected-reply request or stop. It lives here because it was written twice -- once
  // per domain -- and the two copies had already drifted over whether a give-up speaks
  // and what the retry calls the thing it wants.
  //
  // `artifactNoun` is what the model must send back ("blueprint", "tile", "object"), not
  // the domain name: "send the corrected building" is a sentence about a person.
  //
  // Returns true when a retry was queued, false when the budget is exhausted -- the
  // caller decides whether an exhausted budget should be spoken aloud.
  bool RetryRefused(int conversation, const std::string &reason,
                    const std::string &artifactNoun, bool wasSet = false);

  // ---- inspection ------------------------------------------------------------
  //
  // The reply verbatim, logged before anything is extracted from it, so a bad parse has
  // something to be diagnosed against. Returns a copy for the inspector to work from.
  Exchange BeginInspection(size_t index);

  // Calls `inspect` once for every exchange that is finished but not yet inspected.
  // Inspection runs on the main thread: validating and writing content from a background
  // thread would mean the ECS mid-frame.
  void Pump(const std::function<void(size_t)> &inspect);

  // ---- attempt budgets, per conversation --------------------------------------
  //
  // Per conversation, not per session: the sprite window retries a refused definition up
  // to `designer.maxDesignAttempts` and a refused pixel document up to
  // `art.generation.dslAttempts`, and one shared counter would let one budget silence the
  // other.
  int Attempts(int conversation) const;
  // Increments and returns the new count.
  int CountAttempt(int conversation);
  void ResetAttempts(int conversation);
  // 0 means "use the run's own cap" (Game::maxDesignAttempts).
  void SetMaxAttempts(int conversation, int maxAttempts);
  int MaxAttempts(int conversation) const;

  // ---- history ---------------------------------------------------------------
  //
  // An empty `assistantText` records the user turn alone, which is what a transport
  // failure leaves behind: there is no answer to replay.
  void PushTurn(int conversation, const std::string &userText,
                const std::string &assistantText);
  // Forgets a conversation's history. Needed when a new design starts a conversation
  // over -- a pixel document belongs to the definition it was asked for, and replaying
  // the previous one's instructions would tell the model what it already did wrong.
  void ClearHistory(int conversation);

  // ---- logging ---------------------------------------------------------------
  //
  // The file is created lazily on the first message rather than in the constructor, so a
  // run that never opens the window leaves no empty file, and the name carries the time
  // of the conversation rather than of the launch.
  //
  // Written as events happen rather than flushed at shutdown: the sessions worth reading
  // are the ones that go wrong, and a buffer that only lands on exit is exactly the log a
  // crash loses.
  void Log(const std::string &text);
  // Writes a header line to the log, timestamped to the second, for the events worth
  // locating in time (a send, a model reply, a verdict, a retry).
  void LogEvent(const std::string &text);
  // The prompt file's own text, quoted into the log, with its size and line count.
  void LogPromptFile(const std::string &path);
  const std::string &ModelLabel() const { return modelLabel; }
  // False when the backend failed to construct, which the windows report rather than
  // crash on.
  bool HasBackend() const { return backend != nullptr; }

  // ---- speech ----------------------------------------------------------------
  //
  // The queue, or null when speech is off, unavailable, or the game is gone.
  SpeechQueue *Speech() const;
  bool SpeechAvailable() const;
  // Stops anything being spoken. Called when a new request is sent -- a forty-second
  // critique of the previous reply is answering a question nobody is asking any more.
  void StopSpeech();
  // Queues one utterance for `index` and remembers it on that exchange, so the bar under
  // the reply and the audio they share one handle.
  void SpeakFor(size_t index, const std::string &text);
  // The transport bar under one reply: play/pause, scrubbable position, elapsed and total
  // time, stop. Handles every state a clip can be in, including the ones with no audio at
  // all, by offering to speak the reply instead.
  void DrawSpeechBar(size_t index, Exchange &exchange);

  // ---- transcript ------------------------------------------------------------
  //
  // Draws the conversation into the current ImGui window/child: the user and retry
  // prompts, the replies with their thinking tint, the streaming ellipsis, the error line,
  // the speech bar and the separator.
  //
  // The verdict between a reply and its error is drawn by `drawVerdict`, because it is the
  // one part that genuinely differs per domain -- "Saved 'x' to y" against a set's
  // per-entry list against a compiled document -- and a callback keeps that difference
  // visible instead of buried in flags.
  // `autoScroll` keeps the newest output in view while a reply streams. The scroll only
  // moves when the transcript actually grew, so an idle transcript is not yanked back
  // from wherever the reader scrolled to.
  void DrawTranscript(bool showThoughts, bool showJson, const std::string &emptyHint,
                      const std::function<void(Exchange &)> &drawVerdict,
                      bool autoScroll);

  // The marker HideJsonBlock substitutes, e.g. "sprite hidden".
  const std::string &HiddenMarker() const { return hiddenMarker; }

private:
  struct Conversation {
    std::string contextId;
    std::string systemPrompt;
    std::vector<ChatMessage> history;
    int attempts = 0;
    // 0 = follow the run's cap.
    int maxAttempts = 0;
  };

  // Conversation indexes are constants owned by the windows, so an out-of-range one is a
  // programming error rather than user input. Guarded anyway, and answered with a no-op or
  // an empty prompt: a mis-indexed conversation must not corrupt the one next to it.
  bool ValidConversation(int index) const;

  Game *game = nullptr;
  AI *backend = nullptr;
  // The transcript size last scrolled for, so auto-scroll reacts to new output rather than
  // fighting a reader every frame.
  size_t autoScrollSignature = 0;
  std::string modelLabel;

  std::string logDirectory;
  std::string logSuffix;
  std::string label;
  std::string hiddenMarker;

  mutable std::mutex transcriptMutex;
  std::vector<Exchange> exchanges;
  std::vector<Conversation> conversations;

  // Counts inspected replies, so a log can be read as "request 1, 2, 3" without relying
  // on the wall clock, which can be identical for two fast attempts.
  int requestNumber = 0;
  // When the current request was handed to the backend. A monotonic clock, because this
  // measures a duration and wall-clock time can step.
  std::chrono::steady_clock::time_point requestStarted;

  std::string logFilePath;
};
