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

constexpr float DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS = 10.0f;

enum class NPCCommandType {
  INVALID_COMMAND,
  DO_NOTHING,
  MOVE_TO_LOCATION,
  TALK_TO,
  CHARACTERS_QUERY,
};

#include "AgentActions.h"

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


class AgentBrain {
public:
  AgentBrain(flecs::entity entity, std::string name);

  MessageCommand ParseMessageCommand(std::string msg);
  void addCmdToQueue(MessageCommand msgCmd);
  void sendToAI(std::string msg, std::stop_token stoken);
  void executeActionQueue(float deltaTime);
  void update(float deltaTime);

  std::string getContext() const;
  void appendContext(const std::string &text);

private:
  flecs::entity entity;

  std::queue<std::unique_ptr<AgentAction>> action_queue;
  AI::StreamCallback getStreamCallback();
};
