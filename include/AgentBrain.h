#pragma once

#include "AI.h"
#include "Components.h"
#include "Map.h"
#include "PathFinding.h"

#include <condition_variable>
#include <flecs.h>
#include <deque>
#include <functional>
#include <memory>
#include <thread>

class DebugLog;

constexpr float DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS = 10.0f;

// Backoff before re-issuing a request whose response came back empty, so a
// failing provider is not hammered in a tight loop.
constexpr float DEFAULT_EMPTY_RESPONSE_RETRY_MS = 2000.0f;

#include "AgentActions.h"

using ActionThunk = std::function<std::unique_ptr<AgentAction>(flecs::entity)>;

class Map;

const std::string DEFAULT_NPC_PROMPT = R"(
System: You are an AI roleplaying as an NPC in a game. When navigating the world, you must respond strictly with a single command bracket and no other text. When in an active conversation, speak naturally in-character (you do not need to use commands unless exiting). You should use asterisks to express your physical actions or emotions (e.g., *sighs* or *looks around nervously*).

AVAILABLE COMMANDS:
%COMMANDS_LIST%

VARIABLES & RULES:
- $TARGET can be a location or an object. Available locations: %LOCATIONS%
- $COMMAND must be chosen from the available commands list.
%COMMANDS_RULES%

CHARACTER CONTEXT:
- Background: %BACKGROUND%
)";


class AgentBrain {
public:
  AgentBrain(flecs::entity entity, std::string name, DebugLog *debugLog = nullptr);
  bool isStopped = false;

  void ForceInterruptAndPush(std::unique_ptr<AgentAction> action);

  ActionThunk ParseMessageCommand(std::string msg);
  void addCmdToQueue(ActionThunk actionThunk);
  void interruptCurrentAction();
  void resumeAction();
  void injectNextAction(ActionThunk actionThunk);
  void sendToAI(std::string msg, std::stop_token stoken);
  void executeActionQueue(float deltaTime);
  void update(float deltaTime);
  AgentAction* getCurrentAction() const { return currentAction.get(); }

  std::string getContext() const;
  // Add a message to the agent's context, continuing the current turn when the
  // previous message has the same role. Mirrored to the transcript log.
  void appendContext(const std::string &role, const std::string &text);
  // Add a message as its own turn even when the previous turn shares its role.
  // Use this for entries that must stay distinct (prompts, conversation state).
  void pushContext(const std::string &role, const std::string &text);
  // Record one streamed assistant token. An empty token marks end of stream and
  // closes the transcript line. Going through here (rather than writing history
  // directly) is what keeps the transcript in sync with NPCContext.
  void recordAssistantStream(const std::string &token);
  // Surface a problem in the Debug Log window. Const so actions that only hold
  // a read-only brain pointer (e.g. TalkAction) can still report failures.
  void logWarning(const std::string &text) const;
  // Write to the transcript log without adding a turn to the AI's context, so
  // bookkeeping notes do not wake the model. See its use for timed harvests.
  void logNote(const std::string &text);

private:
  flecs::entity entity;
  std::unique_ptr<AgentAction> currentAction;
  std::vector<std::unique_ptr<AgentAction>> actionStack;
  std::deque<ActionThunk> action_queue;
  AI::StreamCallback getStreamCallback();
  // The single writer for NPCContext::history. Every context mutation must go
  // through here so the transcript file records exactly what the model sees.
  void recordContext(const std::string &role, const std::string &text,
                     bool continueTurn);
  void writeTranscript(const std::string &text);
  std::string logFilePath;
  DebugLog *debugLog = nullptr;
  // Set while waiting out the backoff before retrying an empty response, so the
  // same failure is not re-logged and re-scheduled on every frame.
  bool awaitingRetry = false;
};
