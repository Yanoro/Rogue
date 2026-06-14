#pragma once

#include "AI.h"
#include <flecs.h>
#include <memory>
#include <thread>
#include <atomic>
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
  World: You are capable of two commands: 
  World: [DO_NOTHING]
  World: [MOVE_TO $LOCATION]
  World: Where $LOCATION is one of the following locations: %LOCATIONS%
  World: This is your character background: %BACKGROUND%
  World: What is your first command?
  You: )";

class NPC {
public:
  NPC(flecs::entity entity, std::string prompt, Map *map,
      std::shared_ptr<AI> ai);

  MessageCommand ParseMessageCommand(std::string msg);
  void sendToAI(std::string msg);
  void Loop();

  std::string getContextID() const { return contextId; }

  ~NPC();

private:
  flecs::entity entity;
  std::shared_ptr<AI> ai;
  std::string contextId;
  std::string characterBackground;

  std::atomic<bool> keepRunning{true};
  std::jthread loopThread;

  Map *map;
};
