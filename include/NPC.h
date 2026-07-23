#pragma once

#include "AI.h"

#include <condition_variable>
#include <flecs.h>
#include <memory>
#include <thread>

constexpr unsigned int DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS = 30;

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

class NPC {
public:
  NPC(flecs::entity entity, std::string name, std::string characterBackground,
      std::shared_ptr<AI> ai);

  MessageCommand ParseMessageCommand(std::string msg);
  void sendToAI(std::string msg, std::stop_token stoken);
  void Loop(std::stop_token stoken);

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

  std::condition_variable_any sleepCV;
  std::mutex sleepMutex;
  std::jthread loopThread;

  AI::StreamCallback getStreamCallback();
};
