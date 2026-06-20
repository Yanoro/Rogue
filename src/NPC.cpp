#include "NPC.h"
#include "AI.h"
#include "AgentContextWindow.h"
#include "Components.h"
#include "Map.h"
#include "PathFinding.h"
#include "flecs.h"

#include <regex>
#include <thread>

NPC::NPC(flecs::entity entity, std::string characterBackground,
         std::shared_ptr<AI> ai)
    : entity(entity), ai(std::move(ai)),
      contextId(std::to_string(reinterpret_cast<uintptr_t>(this))),
      characterBackground(characterBackground) {
  entity.set<WindowOnClick>({WindowType::NPCContextWindowType});
  loopThread = std::jthread(&NPC::Loop, this);
}

MessageCommand NPC::ParseMessageCommand(std::string msg) {
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

std::string NPC::getContext() const {
  std::lock_guard<std::mutex> lock(contextMutex);
  return context;
}

void NPC::appendContext(const std::string &text) {
  std::lock_guard<std::mutex> lock(contextMutex);
  context += text;
}

AI::StreamCallback NPC::getStreamCallback() {
  return [this](const std::string &token) { appendContext(token); };
}

void NPC::sendToAI(std::string msg, std::stop_token stoken) {
  // TODO: Handle the case where the calls to AI fail
  ai->generateStream(contextId, msg, getStreamCallback(), stoken);
}

void NPC::Loop(std::stop_token stoken) {
  std::string locations = "";
  Map *currMap = entity.world().get<MapResource>()->map;

  for (const auto &currName : currMap->GetAllLocationNames()) {
    locations += currName + ", ";
  }

  if (!locations.empty()) {
    locations.pop_back();
  }

  std::regex re1("%LOCATIONS%");
  std::regex re2("%BACKGROUND%");

  std::string startingPrompt =
      std::regex_replace(DEFAULT_NPC_PROMPT, re1, locations);
  startingPrompt = std::regex_replace(startingPrompt, re2, characterBackground);
  appendContext(startingPrompt);

  sendToAI(startingPrompt, stoken);
  std::string contextId = getContextID();
  while (!stoken.stop_requested()) {
    if (ai->isBusy(contextId)) {
      std::unique_lock<std::mutex> lock(sleepMutex);
      sleepCV.wait_for(lock, stoken, std::chrono::milliseconds(10),
                       [] { return false; });
      continue;
    }

    Map *currMap = entity.world().get<MapResource>()->map;

    std::string lastMsg = ai->getLastMessage(contextId);

    MessageCommand msgCmd = ParseMessageCommand(lastMsg);

    std::string response = "\n";

    switch (msgCmd.type) {
    case (NPCCommandType::DO_NOTHING): {
      std::unique_lock<std::mutex> lock(sleepMutex);
      sleepCV.wait_for(
          lock, stoken,
          std::chrono::seconds(DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS),
          [] { return false; });

      response += "WORLD: You have awaited for a while, what's next?\n";
      break;
    }
    case (NPCCommandType::MOVE_TO): {
      Location *loc = currMap->GetLocation(msgCmd.target);
      if (loc != nullptr) {
        const GamePosition start = *entity.get<GamePosition>();
        const GamePosition target = loc->pos;
        entity.set<TargetPath>({AStar(currMap, start, target)});
        while (!stoken.stop_requested()) {
          if (!entity.has<TargetPath>()) {
            break;
          }
          std::unique_lock<std::mutex> lock(sleepMutex);
          sleepCV.wait_for(lock, stoken, std::chrono::seconds(1),
                           [] { return false; });
        }
        response +=
            "WORLD: You have arrived at your destination, what's next?\n";
      } else {
        response += "WORLD: You tried to move to an unknown location.\n";
      }
      break;
    }
    case (NPCCommandType::NONE): {
      break;
    }
    default:
      break;
    }

    appendContext(response);

    if (!stoken.stop_requested()) {
      sendToAI(response, stoken);
    }
  }
}
