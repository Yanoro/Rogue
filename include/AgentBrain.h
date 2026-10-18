#pragma once

#include "AI.h"
#include "Components.h"
#include "SystemOneRecorder.h"

#include <atomic>
#include <memory>
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
System: You are an AI roleplaying as an NPC in a game. When navigating the world, you must respond strictly with a single command bracket and no other text. When in an active conversation, speak naturally in-character, and use the trade commands only to make or answer an offer. You should use asterisks to express your physical actions or emotions (e.g., *sighs* or *looks around nervously*).

AVAILABLE COMMANDS:
%COMMANDS_LIST%

VARIABLES & RULES:
- $TARGET can be a character, an object, or a place you remember. Use [PLACES] to see the places you know; a place you have not seen is not on that list.
- $COMMAND must be chosen from the available commands list.

CHARACTER CONTEXT:
- Background: %BACKGROUND%
)";


class AgentBrain {
public:
  AgentBrain(flecs::entity entity, std::string name, DebugLog *debugLog = nullptr);
  ~AgentBrain();
  bool isStopped = false;

  // Stage 5: the System 1 salience probe. A verdict is logged, never acted on:
  // "logged verdicts before behaviour depends on them"
  // (docs/spatial-memory-design.md section 6.2).
  void SetMemorySalience(SystemOne::SystemOneClassifier *classifier,
                         bool enabled);

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
  // The prompt the agent is woken with. When the agent has a TaskList this is
  // the task digest followed by the question; when it has none it is exactly the
  // string this agent received before the task system existed, which is the
  // "no list behaves as before" invariant (docs/task-system-design.md section 2).
  std::string buildWakePrompt(bool firstWake);

  // Asks the classifier how much the current situation matters to this character's
  // goals, at most once a minute, and logs the previous answer on the main thread.
  void ProbeMemorySalience();

  // Report what changed in the agent's task list since it was last observed.
  // Transitions, not states (section 11), so an unchanged list writes nothing.
  //
  // This is deliberately a comparison rather than a notification from whoever
  // made the change: today the only writer is the agent's own [SET_TASKS], later
  // an engine-side verification will be a second one, and one place that diffs
  // the list catches both without either having to know about the transcript.
  void logTaskTransitions();

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

  SystemOne::SystemOneClassifier *salienceClassifier = nullptr;
  bool salienceEnabled = false;
  int lastSalienceProbeTick = -100000;
  std::shared_ptr<std::atomic<float>> salienceScore =
      std::make_shared<std::atomic<float>>(-1.0f);
  std::shared_ptr<std::atomic<int>> salienceArrivals =
      std::make_shared<std::atomic<int>>(0);
  int salienceLogged = 0;
  // The callback may outlive the brain when a world is torn down
  // mid-request; it checks this before touching anything.
  std::shared_ptr<std::atomic<bool>> alive =
      std::make_shared<std::atomic<bool>>(true);
  // The task list as it was when transitions were last reported, so a change is
  // reported once and only when it happened. Invalid until the first observation,
  // which is reported as an initialisation rather than as a flood of additions.
  TaskList taskSnapshot;
  bool taskSnapshotValid = false;
  // Whether the next digest should invite a revision. Set when the plan may have
  // changed -- an action completed or failed -- and cleared once offered, so the
  // agent is not nudged to rewrite its list on every single turn (section 8).
  // Starts true: a freshly spawned agent has life goals and no plan yet.
  bool taskRevisionSuggested = true;
  // Set while waiting out the backoff before retrying an empty response, so the
  // same failure is not re-logged and re-scheduled on every frame.
  bool awaitingRetry = false;
};
