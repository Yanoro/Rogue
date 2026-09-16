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
  std::string myName = sourceEntity.get<DisplayName>()->name;

  if (StringUtils::EqualsIgnoreCase(myName, targetName)) {
    isTalkingToSelf = true;
    return;
  }

  sourceEntity.world().filter<DisplayName, AgentBrainWrapper>().each(
      [&](flecs::entity e, const DisplayName &name, AgentBrainWrapper & /*wrapper*/) {
        if (StringUtils::EqualsIgnoreCase(name.name, targetName)) {
          this->targetEntity = e;
          found = true;
        }
      });

  if (state == ConversationState::Talking) {
    sourceEntity.get_mut<NPCContext>()->context += "System: You are now talking to " + targetName + ". You MUST explain your reasoning using a <think> block before speaking. What do you say? To leave the conversation, append [EXIT] to the end of your goodbye message, for example: <think>I'm tired of this.</think> I have to go now, goodbye! [EXIT]\n";
  } else {
    sourceEntity.get_mut<NPCContext>()->context += "System: " + targetName + " approaches you to talk and is currently speaking. When it is your turn, you MUST explain your reasoning using a <think> block before speaking. You can append [EXIT] to the end of your goodbye message (for example: <think>I need to leave.</think> Talk to you later! [EXIT]) to leave the conversation.\n";
  }

  if (state == ConversationState::Talking && found) {
    AgentBrain *targetBrain = targetEntity.get_mut<AgentBrainWrapper>()->agBrain.get();
    targetBrain->ForceInterruptAndPush(
        std::make_unique<TalkAction>(targetEntity, myName, ConversationState::Listening)
    );
  }
}

ActionStatus TalkAction::update(float, flecs::entity entity) {
  if (state == ConversationState::Talking) {
    const AIRequest *request = entity.get<AIRequest>();
    if (request == nullptr) {
      std::string context = entity.get<NPCContext>()->context;
      entity.set<AIRequest>({"You: ", false, "", false});
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
        
        std::regex extractThink(R"(<think>([\s\S]*?)<\/think>)", std::regex_constants::icase);
        std::smatch match;
        bool hasValidThink = false;
        if (std::regex_search(response, match, extractThink)) {
          std::string thinkContents = match[1].str();
          if (thinkContents.find_first_not_of(" \n\r\t") != std::string::npos) {
            hasValidThink = true;
          }
        }
        
        if (!hasValidThink) {
          entity.get_mut<NPCContext>()->context += "System: Warning - you MUST explain your reasoning using a <think> block before speaking.\n";
        }

        // Strip out <think> tags so they aren't spoken out loud
        std::regex thinkRegex(R"(<think>[\s\S]*?<\/think>)", std::regex_constants::icase);
        response = std::regex_replace(response, thinkRegex, "");

        // Trim leading and trailing whitespace
        auto start = response.find_first_not_of(" \n\r\t");
        if (start != std::string::npos) {
            response = response.substr(start);
            response.erase(response.find_last_not_of(" \n\r\t") + 1);
        } else {
            response = "";
        }

        bool exiting = false;
        std::regex exitRegex(R"(\[EXIT\])", std::regex_constants::icase);
        if (std::regex_search(response, exitRegex)) {
            exiting = true;
            response = std::regex_replace(response, exitRegex, "");
            
            start = response.find_first_not_of(" \n\r\t");
            if (start != std::string::npos) {
                response = response.substr(start);
                response.erase(response.find_last_not_of(" \n\r\t") + 1);
            } else {
                response = "";
            }
        }

        std::regex anyCommandRegex(R"(\[.*?\])");
        bool hasOtherCommand = std::regex_search(response, anyCommandRegex);

        if ((response.empty() && !exiting) || hasOtherCommand) {
            std::string warning = "System: Warning - you must stay in character during a TalkAction. Any messages including SYSTEM commands besides [EXIT] will not be consired valid. Provide conversational dialogue, or append [EXIT] to your message to end the conversation.\n";
            entity.get_mut<NPCContext>()->context += warning;
            entity.remove<AIRequest>();
            return ActionStatus::Doing;
        }

        std::string myName = entity.get<DisplayName>()->name;
        targetBrain->appendContext(myName + " says: " + response + "\n");
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
    
    // Clear any pending AIRequest that was dispatched before we started listening
    // to prevent the AI from responding to the conversation with a stale command
    if (entity.has<AIRequest>()) {
      entity.remove<AIRequest>();
    }
  } else if (state == ConversationState::Ended) {
    return ActionStatus::Done;
  }

  return ActionStatus::Doing;
}

ActionStatus TalkAction::handleInterruption(flecs::entity entity) {
  if (entity.has<AIRequest>()) {
    entity.remove<AIRequest>();
  }
  return ActionStatus::Interrupted;
}

void TalkAction::resume(flecs::entity) {
  // update() will naturally resume the conversation by re-requesting AI input
}

std::string TalkAction::getSuccessMessage() {
  return "System: You are done talking to " + targetName + ", what's next?\n";
}

std::string TalkAction::getFailureMessage() {
  if (isTalkingToSelf) {
    return "System: You cannot talk to yourself. Please choose another character or use a different command.\n";
  }
  return "System: The person you were trying to talk to is no longer available.\n";
}

AgentBrain::AgentBrain(flecs::entity entity, std::string name)
    : entity(entity) {
  entity.set<WindowOnClick>({WindowType::NPCContextWindowType});
  entity.set_name(name.c_str());
}

void AgentBrain::ForceInterruptAndPush(std::unique_ptr<AgentAction> action) {
  interruptCurrentAction();
  currentAction = std::move(action);
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

void AgentBrain::interruptCurrentAction() {
  if (currentAction) {
    currentAction->handleInterruption(entity);
    actionStack.push_back(std::move(currentAction));
    currentAction = nullptr;
  }
}

void AgentBrain::resumeAction() {
  if (!actionStack.empty()) {
    if (currentAction) {
      interruptCurrentAction();
    }
    currentAction = std::move(actionStack.back());
    actionStack.pop_back();
    currentAction->resume(entity);
  } else {
    appendContext("System: There is no interrupted action to resume.\n");
  }
}

void AgentBrain::executeActionQueue(float deltaTime) {
  if (!currentAction) {
    if (!actionStack.empty()) {
      resumeAction();
    } else if (!action_queue.empty()) {
      currentAction = action_queue.front()(entity);
      action_queue.pop_front();
    } else {
      return;
    }
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
  if (isStopped) return;

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
    entity.set<AIRequest>({"You: ", false, "", false});
    entity.set<AgentSleepTimer>({10});
    return;
  }
  if (!request->finished) {
    entity.set<AgentSleepTimer>({10});
    return;
  }

  std::string responseText = request->pendingResponse;
  
  std::regex extractThink(R"(<think>([\s\S]*?)<\/think>)", std::regex_constants::icase);
  std::smatch match;
  bool hasValidThink = false;
  if (std::regex_search(responseText, match, extractThink)) {
    std::string thinkContents = match[1].str();
    if (thinkContents.find_first_not_of(" \n\r\t") != std::string::npos) {
      hasValidThink = true;
    }
  }

  if (!hasValidThink) {
    appendContext("System: Warning - you MUST include a <think> block with your reasoning before taking an action. For example: <think>I should look around.</think>\n");
  }

  std::string textWithoutThink = std::regex_replace(responseText, std::regex(R"(<think>[\s\S]*?<\/think>)", std::regex_constants::icase), "");
  std::string textWithoutCommand = std::regex_replace(textWithoutThink, std::regex(R"(\[.*?\])"), "");
  
  auto start = textWithoutCommand.find_first_not_of(" \n\r\t");
  if (start != std::string::npos) {
    textWithoutCommand = textWithoutCommand.substr(start);
    textWithoutCommand.erase(textWithoutCommand.find_last_not_of(" \n\r\t") + 1);
  } else {
    textWithoutCommand = "";
  }
  
  if (!textWithoutCommand.empty()) {
    appendContext("System: Warning - you included conversational text or explanations outside of your <think> tags. All reasoning must be strictly enclosed in <think> tags, followed only by your command.\n");
  }

  MessageCommand msgCmd = ParseMessageCommand(responseText);
  
  addCmdToQueue(msgCmd);

  entity.remove<AIRequest>();
}
