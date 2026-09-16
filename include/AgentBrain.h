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

constexpr float DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS = 10.0f;

enum class NPCCommandType {
  INVALID_COMMAND,
  DO_NOTHING,
  MOVE_TO_LOCATION,
  TALK_TO,
  CHARACTERS_QUERY,
};

#include "AgentActions.h"

using ActionThunk = std::function<std::unique_ptr<AgentAction>(flecs::entity)>;

class Map;

struct MessageCommand {
  NPCCommandType type;
  std::any params;
};

const std::string DEFAULT_NPC_PROMPT = R"(
System: You are an AI roleplaying as an NPC in a game. Before making any decision, you MUST explain your reasoning inside a <think> block. After the <think> block, you may respond strictly with a single command bracket. Do not include any text outside of the <think> block and the command bracket.

Example:
<think>
I need to talk to Bob about the quest.
</think>
[TALK_TO Bob]

AVAILABLE COMMANDS:
[DO_NOTHING]
[MOVE_TO $LOCATION]
[TALK_TO $CHARACTER]
[CHARACTERS]

VARIABLES & RULES:
- $LOCATION must be chosen from this list: %LOCATIONS%
- $COMMAND must be chosen from the available commands list.
- If you need to know who is nearby to talk to, issue the [CHARACTERS] command.

CHARACTER CONTEXT:
- Background: %BACKGROUND%

Based on your background and current location, what is your first command?)";


class AgentBrain {
public:
  AgentBrain(flecs::entity entity, std::string name);
  bool isStopped = false;

  void ForceInterruptAndPush(std::unique_ptr<AgentAction> action);

  MessageCommand ParseMessageCommand(std::string msg);
  void addCmdToQueue(MessageCommand msgCmd);
  void interruptCurrentAction();
  void resumeAction();
  void injectNextAction(ActionThunk actionThunk);
  void sendToAI(std::string msg, std::stop_token stoken);
  void executeActionQueue(float deltaTime);
  void update(float deltaTime);
  AgentAction* getCurrentAction() const { return currentAction.get(); }

  std::string getContext() const;
  void appendContext(const std::string &text);

private:
  flecs::entity entity;
  std::unique_ptr<AgentAction> currentAction;
  std::vector<std::unique_ptr<AgentAction>> actionStack;
  std::deque<ActionThunk> action_queue;
  AI::StreamCallback getStreamCallback();
};
