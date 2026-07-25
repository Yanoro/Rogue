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
  virtual bool update(float deltaTime, flecs::entity entity) = 0;
};

class Nothing_Action : public AgentAction {
public:
  Nothing_Action(float time) : time(time) {};

  bool update(float deltaTime, flecs::entity) override {
    time -= deltaTime;
    return time <= 0.0f;
  }

private:
  float time;
};

class Moveto_Action : public AgentAction {
public:
  Moveto_Action(GamePosition target) : targetPos(target) {};

  bool update(float, flecs::entity entity) override {
    Map *map = entity.world().get<MapResource>()->map;
    const GamePosition startPos = *entity.get<GamePosition>();

    if (startPos == targetPos or Map::AreNeighbours(startPos, targetPos)) {
      return true;
    } else if (entity.get<MOVE_THROUGH_PATH_ACTION>() == nullptr) {
      entity.set<MOVE_THROUGH_PATH_ACTION>({AStar(map, startPos, targetPos)});
    }
  }

private:
  GamePosition targetPos;
};

class AgentBrain {
public:
  AgentBrain(flecs::entity entity, std::string name,
             std::string characterBackground, std::shared_ptr<AI> ai);

  MessageCommand ParseMessageCommand(std::string msg);
  void sendToAI(std::string msg, std::stop_token stoken);
  void executeActionQueue(float deltaTime, flecs::entity entity);
  void update(float deltaTime, flecs::entity entity);

  std::string getContextID() const { return contextId; }
  std::string getContext() const;
  void appendContext(const std::string &text);
  std::shared_ptr<AI> getAI() const { return ai; }

private:
  flecs::entity entity;
  std::shared_ptr<AI> ai;
  std::string contextId;
  std::string context;
  mutable std::mutex contextMutex;
  std::string characterBackground;

  std::queue<std::unique_ptr<AgentAction>> action_queue;

  AI::StreamCallback getStreamCallback();
};
