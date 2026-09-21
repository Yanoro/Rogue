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
#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include "TimeUtils.hpp"


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
    sourceEntity.get_mut<NPCContext>()->history.push_back({"user", "System: You are now talking to " + targetName + ". What do you say? To leave the conversation, append [EXIT] to the end of your goodbye message, for example: I have to go now, goodbye! [EXIT]\n"});
  } else {
    sourceEntity.get_mut<NPCContext>()->history.push_back({"user", "System: " + targetName + " approaches you to talk and is currently speaking. When it is your turn, you can append [EXIT] to the end of your goodbye message (for example: Talk to you later! [EXIT]) to leave the conversation.\n"});
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
      std::string context = entity.get<AgentBrainWrapper>()->agBrain->getContext();
      entity.set<AIRequest>({"", false, "", false});
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
            std::string warning = "System: Warning - you must stay in character during a TalkAction. Provide conversational dialogue, or append [EXIT] to your message to end the conversation.\n";
            entity.get_mut<NPCContext>()->history.push_back({"user", warning});
            entity.remove<AIRequest>();
            return ActionStatus::Doing;
        }

        std::string myName = entity.get<DisplayName>()->name;
        if (!response.empty()) {
          targetBrain->appendContext("user", myName + " says: " + response + "\n");
        }
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

  std::filesystem::create_directories("logs");
  logFilePath = "logs/" + TimeUtils::GetStartTimeString() + "-" + name + ".txt";
}

void AgentBrain::ForceInterruptAndPush(std::unique_ptr<AgentAction> action) {
  interruptCurrentAction();
  currentAction = std::move(action);
}

#include "GlobalCommandRegistry.h"

ActionThunk AgentBrain::ParseMessageCommand(std::string msg) {
  // Strip out <think> tags before parsing to avoid matching thoughts
  std::regex thinkRegex(R"(<think>[\s\S]*?<\/think>)", std::regex_constants::icase);
  std::string strippedMsg = std::regex_replace(msg, thinkRegex, "");

  // Check for multiple commands
  static std::regex anyCmdRegex(R"(\[[A-Z_]+.*?\])", std::regex_constants::icase);
  auto words_begin = std::sregex_iterator(strippedMsg.begin(), strippedMsg.end(), anyCmdRegex);
  auto words_end = std::sregex_iterator();
  if (std::distance(words_begin, words_end) > 1) {
    appendContext("user", "System: Warning - you issued multiple commands at once. You must only issue one command at a time.\n");
    return [](flecs::entity) { return std::make_unique<InvalidAction>(); };
  }

  for (const auto& cmd : GlobalCommandRegistry::GetCommands()) {
    std::smatch match;
    if (std::regex_search(strippedMsg, match, cmd.pattern)) {
      if (cmd.format != "[GENERIC_INTERACT]") {
        if (entity.has<InteractionTarget>()) {
          entity.remove<InteractionTarget>();
        }
      }
      return cmd.createAction(match);
    }
  }

  return [](flecs::entity) { return std::make_unique<InvalidAction>(); };
}

std::string AgentBrain::getContext() const {
  std::string result = "";
  auto ctx = entity.get<NPCContext>();
  if (ctx) {
    for (const auto& msg : ctx->history) {
      if (msg.role == "assistant") result += "You: " + msg.content;
      else result += msg.content;
    }
  }
  return result;
}

void AgentBrain::appendContext(const std::string &role, const std::string &text) {
  auto ctx = entity.get_mut<NPCContext>();
  bool isNewMessage = true;
  if (!ctx->history.empty() && ctx->history.back().role == role) {
    ctx->history.back().content += text;
    isNewMessage = false;
  } else {
    ctx->history.push_back({role, text});
  }

  std::ofstream out(logFilePath, std::ios::app);
  if (out.is_open()) {
    if (isNewMessage && role == "assistant") {
      out << "You: " << text;
    } else {
      out << text;
    }
  }
}

AI::StreamCallback AgentBrain::getStreamCallback() {
  return [this](const std::string &token) { appendContext("assistant", token); };
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
    appendContext("user", "System: There is no interrupted action to resume.\n");
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
    appendContext("user", currentAction->getSuccessMessage());
    currentAction.reset();
    break;
  }
  case (ActionStatus::Doing): {
    break;
  }
  case (ActionStatus::Failed): {
    appendContext("user", currentAction->getFailureMessage());
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

void AgentBrain::addCmdToQueue(ActionThunk actionThunk) {
  action_queue.push_back(std::move(actionThunk));
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

  if (isStopped) return;

  const AIRequest *request = entity.get<AIRequest>();
  const std::string context = getContext();
  if (request == nullptr) {
    auto ctx = entity.get<NPCContext>();
    if (ctx && ctx->history.size() > 1) {
      entity.set<AIRequest>({"System: What is your next command?\n", false, "", false});
    } else {
      entity.set<AIRequest>({"System: Based on your background and current location, what is your first command?\n", false, "", false});
    }
    entity.set<AgentSleepTimer>({10});
    return;
  }
  if (!request->finished) {
    entity.set<AgentSleepTimer>({10});
    return;
  }

  std::string responseText = request->pendingResponse;
  
  ActionThunk actionThunk = ParseMessageCommand(responseText);
  
  addCmdToQueue(actionThunk);

  entity.remove<AIRequest>();
}
