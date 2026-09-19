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
  LOCATIONS_QUERY,
  OBJECTS_QUERY,
  INVENTORY_QUERY,
  INTERACT,
};

#include "AgentActions.h"

using ActionThunk = std::function<std::unique_ptr<AgentAction>(flecs::entity)>;

class Map;

struct MessageCommand {
  NPCCommandType type;
  std::any params;
};

const std::string DEFAULT_NPC_PROMPT = R"(
System: You are an AI roleplaying as an NPC in a game. When navigating the world, you must respond strictly with a single command bracket and no other text. When in an active conversation, speak naturally in-character (you do not need to use commands unless exiting). You should use asterisks to express your physical actions or emotions (e.g., *sighs* or *looks around nervously*).

AVAILABLE COMMANDS:
[DO_NOTHING]
[MOVE_TO $TARGET]
[INTERACT $NUMBER]
[TALK_TO $CHARACTER]
[CHARACTERS]
[LOCATIONS]
[OBJECTS]
[INVENTORY]

VARIABLES & RULES:
- $TARGET can be a location or an object. Available locations: %LOCATIONS%
- $COMMAND must be chosen from the available commands list.
- To see nearby objects you can move to, issue the [OBJECTS] command.
- If you need to know who is nearby to talk to, issue the [CHARACTERS] command.
- If you need to remind yourself of the available locations in the world, issue the [LOCATIONS] command.
- If you need to see what items you are holding, issue the [INVENTORY] command.

CHARACTER CONTEXT:
- Background: %BACKGROUND%
)";


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
  void appendContext(const std::string &role, const std::string &text);

private:
  flecs::entity entity;
  std::unique_ptr<AgentAction> currentAction;
  std::vector<std::unique_ptr<AgentAction>> actionStack;
  std::deque<ActionThunk> action_queue;
  AI::StreamCallback getStreamCallback();
};
