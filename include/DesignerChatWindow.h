#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <flecs.h>

#include "AI.h"
#include "BlueprintExpansion.hpp"
#include "PromptRewriter.hpp"
#include "RewriteInputBox.hpp"
#include "SpeechQueue.h"
#include "Window.h"

class Game;
class SpeechQueue;

// A development window for talking to the designer agent: the model whose only
// job is to turn a request in plain language into a blueprint.
//
// Why it exists: to find out how good a model is at *designing* before any of
// this is wired to the in-game agents. It is therefore deliberately isolated,
// exactly like VisionChatWindow -- it owns its own contextId and never touches
// NPCContext, AIRequest or the agent thread pipeline. Nothing here can disturb
// the NPC loop.
//
// The contract with the model, which the prompt file states and this class
// enforces:
//
//   1. It replies with prose and exactly one fenced ```json block.
//   2. That block must carry a "designerNotes" string -- the model's own opinion
//      of the design it just made. It is required, not decorative: it is the
//      thing the window exists to collect.
//   3. The engine extracts the block, parses it, validates the materials and
//      expands it. A design that fails any of those gets the reason fed straight
//      back in the same conversation and is retried, up to
//      settings.designer.maxDesignAttempts.
//   4. A design that passes is written to data/blueprints/<id>.json and reloaded,
//      so it appears in the Blueprints window immediately. "designerNotes" is
//      stripped first: it is not part of the blueprint schema.
//
// Every attempt is shown, including the failures. Hiding a rejected attempt would
// make the retry look like one slow success.
class DesignerChatWindow : public Window {
public:
  // `designerAI` is borrowed: Game owns the backend for the whole run. A null
  // pointer is a supported state (the designer backend failed to construct) and
  // is reported in the window rather than crashing.
  DesignerChatWindow(Game *game, AI *designerAI, std::string modelLabel,
                     flecs::entity owner = flecs::entity::null());

  void Draw() override;

private:

  // One request/response attempt. A retry is a new entry, so the transcript reads
  // as the conversation that actually happened.
  struct Exchange {
    // What the transcript shows. Derived from `answer` on demand rather than
    // stored, so the display checkboxes can be toggled after the fact and cannot
    // disagree with the raw reply beneath them.
    std::string DisplayAnswer(bool showThoughts, bool showJson) const;

    // What goes back to the model as its own previous turn: the reasoning is KEPT,
    // because a model reasoning about a revision works better with its earlier
    // reasoning in view, and only the JSON block is dropped -- leaving it in would
    // replay a whole blueprint, and its `designerNotes`, as if freshly produced.
    std::string HistoryAnswer() const;

    // The reply with reasoning removed, for parsing. What the engine reads when
    // deciding whether the design is valid.
    std::string ParsedAnswer() const;
    std::string prompt;   // What was sent. Empty for a non-user-initiated follow-up.
    // The reply exactly as it arrived, reasoning blocks included. Kept raw so the
    // "Show thinking" checkbox can reveal them; everything that reasons about the
    // reply (parsing, validation, history) uses ParsedAnswer() instead.
    std::string answer;
    bool done = false;
    bool failed = false;
    std::string error;
    // The user's own message, as opposed to an engine retry prompt.
    bool fromUser = false;

    // Outcome of validating the answer, filled on the main thread once the
    // stream finishes.
    bool inspected = false;
    bool designOk = false;
    std::string designId;
    std::string savedPath;
    bool overwrote = false;
    std::string designerNotes;
    std::string failureReason;

    // Speech for this reply. `speechId` is the handle the transport bar drives --
    // kNoSpeechClip when nothing was spoken (speech off, muted, or this attempt was
    // an intermediate refusal that stays silent), in which case the bar offers to
    // speak `spokenText` on demand. `spokenText` is kept for exactly that, and
    // because it is the only record of what was actually read aloud.
    SpeechClipId speechId = kNoSpeechClip;
    std::string spokenText;
  };

  void SendUserMessage(const std::string &prompt);
  void GenerateResponse(size_t index, std::string prompt);
  bool IsBusy();

  // Runs on the main thread, once a stream has finished. Extracts the JSON block,
  // validates it, and either saves the design or queues a retry.
  void InspectFinishedExchange(size_t index);

  // Pulls the first fenced json block out of `text`. Empty when there is none.
  static std::string ExtractJsonBlock(const std::string &text);

  // The reply's prose: everything before the fenced block. The complement of
  // ExtractJsonBlock, and what tts.content = prose_and_notes adds to the speech.
  static std::string ProseBeforeJson(const std::string &text);

  // The speech queue, or null when speech is off, unavailable, or the game is
  // gone. Reached through Game rather than passed to the constructor, so this
  // window's signature and every call site stay as they were.
  SpeechQueue *Speech() const;

  // Queues one utterance for `index` and remembers it on that exchange, so the bar
  // under the reply and the audio they share one handle. The single place this window
  // reaches the speech queue, so "there is no speech engine" and "the user muted it"
  // are decided once instead of at every call site.
  void SpeakFor(size_t index, const std::string &text);

  // The transport bar under one reply: play/pause, scrubbable position, elapsed and
  // total time, stop. Handles every state a clip can be in, including the ones with
  // no audio at all, by offering to speak the reply instead -- which is why it needs
  // `index`, to write the new handle back onto the exchange.
  void DrawSpeechBar(size_t index, Exchange &ex);

  // Stops anything being spoken. Called when a new request is sent -- a forty-second
  // critique of the previous reply is answering a question nobody is asking any
  // more. Per-reply stopping is the bar's job now.
  void StopSpeech();

  // The last design that was *parsed*, whether or not it passed validation, plus
  // the expansion drawn from it.
  //
  // Kept even when the design was refused, which is the point: a design that fails
  // validation is exactly the one worth looking at, and the transcript alone does
  // not show what the model thought it was building. The expansion is stored
  // rather than recomputed because rasterising a shape set every frame for a pane
  // that only changes when a design arrives would be wasted work.
  struct PreviewedDesign {
    Blueprint blueprint;
    BlueprintExpansion::Expansion expansion;
    std::string notes;
    bool accepted = false;
    std::string refusal;
  };

  // The left pane: the last attempted design, drawn the way the world draws it.
  // Needs a mutable Game, because resolving tile art goes through the same
  // ResourceManager the world uses.
  void DrawPreviewPane();

  // Appends to the session transcript under logs/designer/.
  //
  // The file is created lazily on the first message rather than in the
  // constructor, so a run that never uses the designer leaves no empty file, and
  // so the name carries the time of the conversation rather than of the launch.
  //
  // Written as events happen rather than flushed at shutdown: the sessions worth
  // reading are the ones that go wrong, and a buffer that only lands on exit is
  // exactly the log a crash loses.
  void Log(const std::string &text);
  // Writes a header line to the log, timestamped to the second, for the events
  // worth locating in time (a send, a model reply, a verdict, a retry).
  void LogEvent(const std::string &text);

  Game *game = nullptr;
  AI *ai = nullptr;
  std::string modelLabel;

  // One per window, so this conversation cannot collide with an NPC's or the
  // vision window's provider-side context.
  std::string contextId;

  mutable std::mutex transcriptMutex;
  std::vector<Exchange> exchanges;

  // Everything sent so far, in ChatMessage form, minus the system prompt. Bounded
  // (see TrimHistory) so a long session does not grow every request.
  std::vector<ChatMessage> history;

  std::string systemPrompt;
  char inputBuffer[2048] = {};

  // Attempts used on the design currently being worked on. Reset when the user
  // sends a new message, so the cap applies per request rather than per session.
  int attempts = 0;
  // Set while a retry for the exchange at this index is in flight, so the main
  // thread starts it exactly once.
  int pendingRetryFor = -1;

  // The design the left pane renders. Empty until the first reply carrying a
  // parseable blueprint.
  std::optional<PreviewedDesign> lastDesign;

  // Whether the model's reasoning ("<think>...</think>") is shown. It is kept in
  // every case -- the reply is stored raw -- so turning this on reveals thoughts
  // from replies that were already received.
  bool showThoughts = false;

  // Whether the blueprint JSON is shown in the transcript, or only the prose
  // around it. Persisted with the layout, because it is a preference about how the
  // window reads rather than about one conversation.
  //
  // Presentation only: the JSON is still parsed out of the reply and saved
  // regardless of this. Hiding it must never change what the designer produced.
  bool showJson = true;

  // Layout, loaded once from the persisted debug window state and written back on
  // every change so it survives a restart.
  //
  // paneWidth is the blueprint pane in pixels; dragging the splitter changes it.
  // zoom is the size of one cell in the pane. 64 is the game's native tile size,
  // which is the only value at which the preview is pixel-for-pixel what the map
  // draws -- the slider exists for looking at a design too large to fit, not as
  // the normal way to view one.
  float paneWidth = 380.0f;
  float zoom = 64.0f;
  // Whether the persisted layout has been read yet. The window is constructed
  // before the state file is loaded in some orders, so this defers the read to the
  // first frame rather than depending on construction order.
  bool layoutLoaded = false;
  // True while the splitter is being dragged.
  bool draggingSplitter = false;

  // The transcript file, empty until the first message creates it.
  // Improving the user's own description before it is sent. The general half
  // (PromptRewriter) and the UI half (RewriteInputBox) are shared with the Tile
  // Designer; this window contributes only its brief and its text buffer.
  PromptRewriter rewriter;
  RewriteInputBox promptBox;

  std::string logFilePath;
  // Counts requests, so a log can be read as "attempt 1, 2, 3" without relying on
  // the wall clock, which can be identical for two fast attempts.
  int requestNumber = 0;
};
