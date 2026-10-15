#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <flecs.h>

#include "AI.h"
#include "Window.h"

class Game;

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
    std::string prompt;   // What was sent. Empty for a non-user-initiated follow-up.
    std::string answer;   // Streamed in token by token.
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
  };

  void SendUserMessage(const std::string &prompt);
  void GenerateResponse(size_t index, std::string prompt);
  bool IsBusy();

  // Runs on the main thread, once a stream has finished. Extracts the JSON block,
  // validates it, and either saves the design or queues a retry.
  void InspectFinishedExchange(size_t index);

  // Pulls the first fenced json block out of `text`. Empty when there is none.
  static std::string ExtractJsonBlock(const std::string &text);

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

  // The transcript file, empty until the first message creates it.
  std::string logFilePath;
  // Counts requests, so a log can be read as "attempt 1, 2, 3" without relying on
  // the wall clock, which can be identical for two fast attempts.
  int requestNumber = 0;
};
