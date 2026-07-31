#pragma once

#include "AI.h"
#include "Components.h"
#include "Map.h"
#include "PathFinding.h"

#include <condition_variable>
#include <flecs.h>
#include <memory>
#include <queue>
#include <thread>

constexpr float DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS = 30.0f;

enum class NPCCommandType {
  INVALID_COMMAND,
  DO_NOTHING,
  MOVE_TO_LOCATION,
  TALK_TO,
  CHARACTERS_QUERY,
};

enum class ActionStatus { Doing, Done, Failed};

class Map;

struct MessageCommand {
  NPCCommandType type;
  std::any params;
};

const std::string DEFAULT_NPC_PROMPT = R"(
System: You are an AI roleplaying as an NPC in a game. You must respond strictly with a single command bracket. Do not include any conversational text, explanations, or dialogue outside the command.

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

Based on your background and current location, what is your first command?
You: )";

class AgentAction {
public:
  // Returns true when the action is completely finished
  virtual ActionStatus update(float deltaTime, flecs::entity entity) = 0;
  virtual std::string getSuccessMessage() = 0;
  virtual std::string getFailureMessage() {
    return "System: Your action has failed. What's next?";
  }
};

class Nothing_Action : public AgentAction {
public:
  Nothing_Action(float time) : time(time) {};

  ActionStatus update(float deltaTime, flecs::entity) override {
    time -= deltaTime;
    if (time <= 0.0f) {
      return ActionStatus::Done;
    }
    return ActionStatus::Doing;
  }

  std::string getSuccessMessage() override {
    return "System: You have awaited for a while, what's next?";
  }

private:
  float time;
};

class Moveto_Action : public AgentAction {
public:
  Moveto_Action(GamePosition target) : targetPos(target) {};

  ActionStatus update(float, flecs::entity entity) override {
    Map *map = entity.world().get<MapResource>()->map;
    const GamePosition startPos = *entity.get<GamePosition>();

    if (startPos == targetPos or Map::AreNeighbours(startPos, targetPos)) {
      return ActionStatus::Done;
    } else if (!entity.has<MOVE_THROUGH_PATH_ACTION>()) {
      entity.set<MOVE_THROUGH_PATH_ACTION>({AStar(map, startPos, targetPos)});
    }
    return ActionStatus::Doing;
  }

  std::string getSuccessMessage() override {
    return "System: You have arrived at your destination. What's next?";
  }

private:
  GamePosition targetPos;
};

class AgentBrain {
public:
  AgentBrain(flecs::entity entity, std::string name);

  MessageCommand ParseMessageCommand(std::string msg);
  void sendToAI(std::string msg, std::stop_token stoken);
  void executeActionQueue(float deltaTime, flecs::entity entity);
  void update(float deltaTime, flecs::entity entity);

  std::string getContext() const;
  void appendContext(const std::string &text);

private:
  flecs::entity entity;

  std::queue<std::unique_ptr<AgentAction>> action_queue;
  AI::StreamCallback getStreamCallback();
};
