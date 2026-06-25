#pragma once

#include "AI.h"
#include <atomic>
#include <condition_variable>
#include <flecs.h>
#include <memory>
#include <thread>
#include <utility>

constexpr unsigned int DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS = 30;

enum class NPCCommandType { NONE, DO_NOTHING, MOVE_TO };

class Map;

struct MessageCommand {
  NPCCommandType type;
  std::string target;
};

const std::string DEFAULT_NPC_PROMPT = R"(
  World: You are an AI controlling an NPC in a game. 
  World: You are capable of three commands: 
  World: [DO_NOTHING]
  World: [MOVE_TO $LOCATION]
  World: [TALK_TO $CHARACTER]
  World: Where $LOCATION is one of the following locations: %LOCATIONS%
  World: Where $CHARACTER is one of the following names: %CHARACTERS%
  World: This is your character background: %BACKGROUND%
  World: What is your first command?
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
