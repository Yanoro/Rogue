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

TalkAction::TalkAction(flecs::entity sourceEntity, std::string targetName, ConversationState state)
    : state(state), targetName(targetName) {
  bool found = false;
  sourceEntity.world().filter<DisplayName, AgentBrainWrapper>().each(
      [&](flecs::entity e, const DisplayName &name, AgentBrainWrapper) {
        if (StringUtils::EqualsIgnoreCase(name.name, targetName)) {
          this->targetEntity = e;
          found = true;
        }
      });

  if (state == ConversationState::Talking) {
    sourceEntity.get_mut<NPCContext>()->context += "System: You are now talking to " + targetName + ". What do you say? To leave the conversation, append [EXIT] to the end of your goodbye message, for example: I have to go now, goodbye! [EXIT]\n";
  } else {
    sourceEntity.get_mut<NPCContext>()->context += "System: " + targetName + " approaches you to talk and is currently speaking. When it is your turn, you can append [EXIT] to the end of your goodbye message (for example: Talk to you later! [EXIT]) to leave the conversation.\n";
  }

  if (state == ConversationState::Talking && found) {
    AgentBrain *targetBrain = targetEntity.get_mut<AgentBrainWrapper>()->agBrain.get();
    std::string myName = sourceEntity.get<DisplayName>()->name;
    targetBrain->injectNextAction(
        [myName](flecs::entity targetEnt) {
          return std::make_unique<TalkAction>(targetEnt, myName, ConversationState::Listening);
        });
  }
}

ActionStatus TalkAction::update(float, flecs::entity entity) {
  if (state == ConversationState::Talking) {
    const AIRequest *request = entity.get<AIRequest>();
    if (request == nullptr) {
      std::string context = entity.get<NPCContext>()->context;
      entity.set<AIRequest>({context, false, "", false});
    } else if (request->finished) {
      
      if (!targetEntity.is_alive() || !targetEntity.has<AgentBrainWrapper>()) {
        entity.remove<AIRequest>();
        return ActionStatus::Failed;
      }

      bool targetReady = false;
      TalkAction *targetTalk = nullptr;
      AgentBrain *targetBrain = targetEntity.get_mut<AgentBrainWrapper>()->agBrain.get();
      
      if (targetBrain && targetBrain->getCurrentAction()) {
        targetTalk = dynamic_cast<TalkAction *>(targetBrain->getCurrentAction());
        if (targetTalk && targetTalk->state == ConversationState::Listening) {
          targetReady = true;
        }
      }

      // We ONLY swap states and transfer the message if they are ready!
      // Otherwise, we just wait and check again next frame.
      if (targetReady) {
        std::string response = request->pendingResponse;
        bool exiting = false;
        
        std::regex exitRegex(R"(\[EXIT\])", std::regex_constants::icase);
        if (std::regex_search(response, exitRegex)) {
            exiting = true;
            response = std::regex_replace(response, exitRegex, "");
            // clean trailing whitespaces just in case
            response.erase(response.find_last_not_of(" \n\r\t") + 1);
        }

        std::string myName = entity.get<DisplayName>()->name;
        targetBrain->appendContext(myName + " says: " + response + "\n");
        entity.get_mut<NPCContext>()->context += "You said: " + response + "\n";
        entity.remove<AIRequest>();

        if (exiting) {
          targetTalk->state = ConversationState::Ended;
          return ActionStatus::Done;
        }

        this->state = ConversationState::Listening;
        targetTalk->state = ConversationState::Talking;
      }
    }
  } else if (state == ConversationState::Listening) {
    if (!targetEntity.is_alive() || !targetEntity.has<AgentBrainWrapper>()) {
      return ActionStatus::Failed;
    }
  } else if (state == ConversationState::Ended) {
    return ActionStatus::Done;
  }

  return ActionStatus::Doing;
}

std::string TalkAction::getSuccessMessage() {
  return "System: You are done talking to " + targetName + ", what's next?\n";
}

std::string TalkAction::getFailureMessage() {
  return "System: The person you were trying to talk to is no longer available.\n";
}

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
  if (!currentAction) {
    if (action_queue.empty()) return;
    currentAction = action_queue.front()(entity);
    action_queue.pop_front();
  }

  switch (currentAction->update(deltaTime, entity)) {
  case (ActionStatus::Done): {
    appendContext(currentAction->getSuccessMessage());
    currentAction.reset();
    break;
  }
  case (ActionStatus::Doing): {
    break;
  }
  case (ActionStatus::Failed): {
    appendContext(currentAction->getFailureMessage());
    currentAction.reset();
    break;
  }
  default:
    currentAction.reset();
    break;
  }
}

void AgentBrain::injectNextAction(ActionThunk actionThunk) {
  if (action_queue.empty()) {
    action_queue.push_back(std::move(actionThunk));
  } else {
    action_queue.insert(action_queue.begin(), std::move(actionThunk));
  }
}

void AgentBrain::addCmdToQueue(MessageCommand msgCmd) {
  switch (msgCmd.type) {
  case (NPCCommandType::DO_NOTHING): {
    action_queue.push_back([](flecs::entity) {
      return std::make_unique<WaitAction>(DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS);
    });
    break;
  }
  case (NPCCommandType::MOVE_TO_LOCATION): {
    Map *currMap = entity.world().get<MapResource>()->map;
    Location *loc =
        currMap->GetLocation(std::any_cast<std::string>(msgCmd.params));
    if (loc != nullptr) {
      GamePosition targetPos = loc->pos;
      action_queue.push_back([targetPos](flecs::entity) {
        return std::make_unique<MoveAction>(targetPos);
      });
    } else {
      addCmdToQueue({NPCCommandType::INVALID_COMMAND, ""});
    }
    break;
  }
  case (NPCCommandType::TALK_TO): {
    std::string targetName = std::any_cast<std::string>(msgCmd.params);
    
    bool found = false;
    entity.world().filter<DisplayName>().each(
        [&](const DisplayName &name) {
          if (StringUtils::EqualsIgnoreCase(name.name, targetName)) {
            found = true;
          }
        });

    if (found) {
      action_queue.push_back([targetName](flecs::entity) {
        return std::make_unique<MoveToEntityAction>(targetName);
      });
      action_queue.push_back([targetName](flecs::entity sourceEnt) {
        return std::make_unique<TalkAction>(sourceEnt, targetName, ConversationState::Talking);
      });
    } else {
      addCmdToQueue({NPCCommandType::INVALID_COMMAND, ""});
    }
    break;
  }
  case (NPCCommandType::CHARACTERS_QUERY): {
    action_queue.push_back([](flecs::entity) {
      return std::make_unique<CharactersAction>();
    });
    break;
  }
  case (NPCCommandType::INVALID_COMMAND): {
    action_queue.push_back([](flecs::entity) {
      return std::make_unique<InvalidAction>();
    });
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

  if (currentAction || !action_queue.empty()) {
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
