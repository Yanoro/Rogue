#include "NPC.h"
#include "AI.h"
#include "AgentContextWindow.h"
#include "Components.h"
#include "Map.h"
#include "PathFinding.h"
#include "flecs.h"

#include <any>
#include <memory>
#include <regex>
#include <thread>

AgentBrain::AgentBrain(flecs::entity entity, std::string name,
                       std::string characterBackground, std::shared_ptr<AI> ai)
    : entity(entity), ai(std::move(ai)),
      contextId(std::to_string(reinterpret_cast<uintptr_t>(this))),
      characterBackground(characterBackground) {
  entity.set<WindowOnClick>({WindowType::NPCContextWindowType});
  entity.set_name(name.c_str());
  loopThread = std::jthread(&AgentBrain::Loop, this);
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
    return {NPCCommandType::MOVE_TO, match[1].str()};
  } else if (std::regex_search(msg, match, nothingRegex)) {
    return {NPCCommandType::DO_NOTHING, ""};
  }

  return {NPCCommandType::NONE, ""};
}

std::string AgentBrain::getContext() const {
  std::lock_guard<std::mutex> lock(contextMutex);
  return context;
}

void AgentBrain::appendContext(const std::string &text) {
  std::lock_guard<std::mutex> lock(contextMutex);
  context += text;
}

AI::StreamCallback AgentBrain::getStreamCallback() {
  return [this](const std::string &token) { appendContext(token); };
}

void AgentBrain::sendToAI(std::string msg, std::stop_token stoken) {
  // TODO: Handle the case where the calls to AI fail
  ai->generateStream(contextId, msg, getStreamCallback(), stoken);
}

void AgentBrain::executeActionQueue(float deltaTime, flecs::entity entity) {
  if (action_queue.front()->update(deltaTime, entity)) {
    action_queue.pop();
  }
}

void AgentBrain::update(float deltaTime, flecs::entity entity) {
  if (entity.get<AgentSleepTimer>()) { return; }

  if (!action_queue.empty()) {
    executeActionQueue(deltaTime, entity);
    entity.set<AgentSleepTimer>({10});
    return;
  } 
 
  // AI is generating or executing action queue, wait 10 ms and come back
  if (ai->isBusy(contextId)) { 
    entity.set<AgentSleepTimer>({10});
    return;
  }

  Map *currMap = entity.world().get<MapResource>()->map;

  std::string lastMsg = ai->getLastMessage(contextId);

  MessageCommand msgCmd = ParseMessageCommand(lastMsg);


  switch (msgCmd.type) {
  case (NPCCommandType::DO_NOTHING): {
      action_queue.push(std::make_unique<Nothing_Action>(DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS));
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

}
