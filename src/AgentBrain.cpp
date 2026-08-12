#include "AgentBrain.h"
#include "AI.h"
#include "AgentContextWindow.h"
#include "Components.h"
#include "Map.h"
#include "PathFinding.h"
#include "StringUtils.hpp"
#include "flecs.h"

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
  static std::regex charactersRegex(R"(\[CHARACTERS\])",
                                    std::regex_constants::icase);
  static std::regex talkToRegex(R"(\[TALK_TO\s+(.+?)\s*\])",
                                std::regex_constants::icase);
  std::smatch match;

  if (std::regex_search(msg, match, moveRegex)) {
    return {NPCCommandType::MOVE_TO_LOCATION, match[1].str()};
  } else if (std::regex_search(msg, match, nothingRegex)) {
    return {NPCCommandType::DO_NOTHING, ""};
  } else if (std::regex_search(msg, match, charactersRegex)) {
    return {NPCCommandType::CHARACTERS_QUERY, ""};
  } else if (std::regex_search(msg, match, talkToRegex)) {
    return {NPCCommandType::TALK_TO, match[1].str()};
  }

  return {NPCCommandType::INVALID_COMMAND, ""};
}

void AgentBrain::appendContext(const std::string &text) {
  entity.get_mut<NPCContext>()->context += text;
}

AI::StreamCallback AgentBrain::getStreamCallback() {
  return [this](const std::string &token) { appendContext(token); };
}

void AgentBrain::executeActionQueue(float deltaTime) {
  AgentAction *currAction = action_queue.front().get();
  switch (currAction->update(deltaTime, entity)) {
  case (ActionStatus::Done): {
    appendContext(currAction->getSuccessMessage());
    action_queue.pop();
    break;
  }
  case (ActionStatus::Doing): {
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

void AgentBrain::addCmdToQueue(MessageCommand msgCmd) {
  switch (msgCmd.type) {
  case (NPCCommandType::DO_NOTHING): {
    action_queue.push(std::make_unique<WaitAction>(
        DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS));
    break;
  }
  case (NPCCommandType::MOVE_TO_LOCATION): {
    Map *currMap = entity.world().get<MapResource>()->map;
    Location *loc =
        currMap->GetLocation(std::any_cast<std::string>(msgCmd.params));
    if (loc != nullptr) {
      action_queue.push(std::make_unique<MoveAction>(loc->pos));
    } else {
      addCmdToQueue({NPCCommandType::INVALID_COMMAND, ""});
    }
    break;
  }
  case (NPCCommandType::TALK_TO): {
    std::string targetName = std::any_cast<std::string>(msgCmd.params);
    action_queue.push(std::make_unique<MoveToEntityAction>(targetName));
    action_queue.push(std::make_unique<TalkAction>(targetName));
    break;
  }
  case (NPCCommandType::CHARACTERS_QUERY): {
    action_queue.push(std::make_unique<CharactersAction>());
    break;
  }
  case (NPCCommandType::INVALID_COMMAND): {
    action_queue.push(std::make_unique<InvalidAction>());
    break;
  }
  default:
    break;
  }
}

void AgentBrain::update(float deltaTime) {
  if (entity.get<AgentSleepTimer>()) {
    AgentSleepTimer *timer = entity.get_mut<AgentSleepTimer>();
    timer->time_remaining_ms -= deltaTime;
    if (timer->time_remaining_ms <= 0) {
      entity.world().defer([&] { entity.remove<AgentSleepTimer>(); });
    }
    return;
  }

  if (!action_queue.empty()) {
    executeActionQueue(deltaTime);
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

  std::string lastMsg = StringUtils::substringAfterLast(context, "System:");

  MessageCommand msgCmd = ParseMessageCommand(lastMsg);
  addCmdToQueue(msgCmd);

  entity.remove<AIRequest>();
}
