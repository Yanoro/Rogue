#pragma once

#include "Components.h"
#include "Map.h"
#include "PathFinding.h"

#include <flecs.h>
#include <string>
#include "StringUtils.hpp"

enum class ActionStatus { Doing, Done, Failed };

class AgentAction {
public:
  virtual ~AgentAction() = default;

  // Returns true when the action is completely finished
  virtual ActionStatus update(float deltaTime, flecs::entity entity) = 0;
  virtual std::string getSuccessMessage() = 0;
  virtual std::string getFailureMessage() {
    return "System: Your action has failed. What's next?\n";
  }
};

class InvalidAction : public AgentAction {
public:
  InvalidAction() {};

  ActionStatus update(float, flecs::entity) override {
    return ActionStatus::Done;
  }

  std::string getSuccessMessage() override {
    return "System: Your previous action action was invalid, please try again\n";
  }
};

class WaitAction : public AgentAction {
public:
  WaitAction(float time) : time(time) {};

  ActionStatus update(float deltaTime, flecs::entity) override {
    time -= deltaTime;
    if (time <= 0.0f) {
      return ActionStatus::Done;
    }
    return ActionStatus::Doing;
  }

  std::string getSuccessMessage() override {
    return "System: You have awaited for a while, what's next?\n";
  }

private:
  float time;
};

class MoveAction : public AgentAction {
public:
  MoveAction(GamePosition target) : targetPos(target) {};

  ActionStatus update(float, flecs::entity entity) override {
    Map *map = entity.world().get<MapResource>()->map;
    const GamePosition startPos = *entity.get<GamePosition>();

    if (startPos == targetPos or Map::AreNeighbours(startPos, targetPos)) {
      return ActionStatus::Done;
    } else if (!entity.has<MOVE_THROUGH_PATH_ACTION>()) {
      entity.set<MOVE_THROUGH_PATH_ACTION>({AStar(map, startPos, targetPos)});
    }
    return ActionStatus::Doing;
  }

  std::string getSuccessMessage() override {
    return "System: You have arrived at your destination. What's next?\n";
  }

private:
  GamePosition targetPos;
};

class MoveToEntityAction : public AgentAction {
public:
  MoveToEntityAction(std::string targetName) : targetName(targetName), lastKnownTargetPos({-1, -1}) {};

  ActionStatus update(float, flecs::entity entity) override {
    Map *map = entity.world().get<MapResource>()->map;
    const GamePosition startPos = *entity.get<GamePosition>();

    GamePosition targetPos = {-1, -1};
    bool found = false;

    entity.world().filter<DisplayName, GamePosition>().each(
      [&](const DisplayName &name, const GamePosition &pos) {
        if (StringUtils::EqualsIgnoreCase(name.name, targetName)) {
          targetPos = pos;
          found = true;
        }
    });

    if (!found) {
      entity.remove<MOVE_THROUGH_PATH_ACTION>();
      return ActionStatus::Failed;
    }

    if (startPos == targetPos || Map::AreNeighbours(startPos, targetPos)) {
      entity.remove<MOVE_THROUGH_PATH_ACTION>();
      return ActionStatus::Done;
    } 
    
    // If we don't have a path, OR the target has moved since we last calculated the path:
    if (!entity.has<MOVE_THROUGH_PATH_ACTION>() || !(lastKnownTargetPos == targetPos)) {
      entity.set<MOVE_THROUGH_PATH_ACTION>({AStar(map, startPos, targetPos)});
      lastKnownTargetPos = targetPos;
    }

    return ActionStatus::Doing;
  }

  std::string getSuccessMessage() override {
    return "System: You have arrived next to " + targetName + ".\n";
  }

  std::string getFailureMessage() override {
    return "System: Could not find character " + targetName + " to move to. What's next?\n";
  }

private:
  std::string targetName;
  GamePosition lastKnownTargetPos;
};

enum class ConversationState {
  Talking,
  Listening,
  Ended
};

class TalkAction : public AgentAction {
public:
  TalkAction(flecs::entity sourceEntity, std::string targetName, ConversationState state = ConversationState::Talking);

  ActionStatus update(float deltaTime, flecs::entity entity) override;
  std::string getSuccessMessage() override;
  std::string getFailureMessage() override;

  ConversationState state;
private:
  std::string targetName;
  flecs::entity targetEntity;
};

class CharactersAction : public AgentAction {
public:
  ActionStatus update(float, flecs::entity entity) override {
    std::string characterNames;
    auto name_filter = entity.world().filter<DisplayName>();
    
    const DisplayName* myNameComp = entity.get<DisplayName>();
    std::string myName = myNameComp ? myNameComp->name : "";

    name_filter.each([&characterNames, &myName](const DisplayName &displayName) {
      if (!displayName.name.empty() && displayName.name != myName) {
        characterNames += displayName.name + ", ";
      }
    });

    if (characterNames.length() >= 2) {
      characterNames.erase(characterNames.length() - 2);
    }

    if (characterNames.empty()) {
      response = "System: There are no other characters nearby.\n";
    } else {
      response = "System: The following characters are nearby: " + characterNames + "\n";
    }

    return ActionStatus::Done;
  }

  std::string getSuccessMessage() override {
    return response;
  }

private:
  std::string response;
};
