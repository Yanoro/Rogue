#include "NPC.h"
#include "AI.h"
#include "AgentContextWindow.h"
#include "Components.h"
#include "Map.h"
#include "PathFinding.h"
#include "flecs.h"
#include "StringUtils.hpp"

#include <any>
#include <iostream>
#include <memory>
#include <regex>
#include <thread>

AgentBrain::AgentBrain(flecs::entity entity, std::string name)
    : entity(entity) {
  entity.set<WindowOnClick>({WindowType::NPCContextWindowType});
  entity.set_name(name.c_str());
}

MessageCommand AgentBrain::ParseMessageCommand(std::string msg) {
  // Static means that we don't have to recompile every time
  // this function gets run
  static std::regex moveRegex(R"(\[MOVE_TO\s+(.+?)\s*\])",
                              std::regex_constants::icase);
  static std::regex nothingRegex(R"(\[DO_NOTHING\])",
                                 std::regex_constants::icase);
  std::smatch match;

  if (std::regex_search(msg, match, moveRegex)) {
    return {NPCCommandType::MOVE_TO_LOCATION, match[1].str()};
  } else if (std::regex_search(msg, match, nothingRegex)) {
    return {NPCCommandType::DO_NOTHING, ""};
  }

  return {NPCCommandType::INVALID_COMMAND, ""};
}

void AgentBrain::appendContext(const std::string &text) {
  std::cout << "APPEND CONTEXT\n";
  std::string context = entity.get_mut<NPCContext>()->context;
  context += text;
}

AI::StreamCallback AgentBrain::getStreamCallback() {
  return [this](const std::string &token) { appendContext(token); };
}

void AgentBrain::executeActionQueue(float deltaTime, flecs::entity entity) {
  AgentAction *currAction = action_queue.front().get();
  switch (currAction->update(deltaTime, entity)) {
  case (ActionStatus::Done): {
    appendContext(currAction->getSuccessMessage());
    action_queue.pop();
    break;
  }
  case (ActionStatus::Failed): {
    appendContext(currAction->getFailureMessage());
    action_queue.pop();
    break;
  }
  default:
    action_queue.pop();
    break;
  }
}

void AgentBrain::update(float deltaTime, flecs::entity entity) {
  if (entity.get<AgentSleepTimer>()) {
    entity.get_mut<AgentSleepTimer>()->time_remaining_ms -= deltaTime;
    return;
  }

  if (!action_queue.empty()) {
    executeActionQueue(deltaTime, entity);
    entity.set<AgentSleepTimer>({10});
    return;
  }

  const AIRequest *request = entity.get<AIRequest>();
  const std::string context = entity.get<NPCContext>()->context;
  if (request == nullptr) {
    entity.set<AIRequest>({"", false, "", false});
    entity.set<AgentSleepTimer>({10});
    return;
  }
  if (!request->finished) {
    entity.set<AgentSleepTimer>({10});
    return;
  }

  Map *currMap = entity.world().get<MapResource>()->map;

  std::string lastMsg = StringUtils::substringAfterLast(context, "System:");

  MessageCommand msgCmd = ParseMessageCommand(lastMsg);

  switch (msgCmd.type) {
  case (NPCCommandType::DO_NOTHING): {
    action_queue.push(std::make_unique<Nothing_Action>(
        DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS));
    break;
  }
  case (NPCCommandType::MOVE_TO_LOCATION): {
    Location *loc =
        currMap->GetLocation(std::any_cast<std::string>(msgCmd.params));
    action_queue.push(std::make_unique<Moveto_Action>(loc->pos));
    break;
  }
  case (NPCCommandType::INVALID_COMMAND): {
    break;
  }
  default:
    break;
  }

  entity.remove<AIRequest>();
}
