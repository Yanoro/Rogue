#pragma once

#include "Components.h"
#include "Map.h"
#include "PathFinding.h"
#include "Defaults.h"
#include "ObjectFactory.h"
#include "InteractionRegistry.h"

#include <flecs.h>
#include <string>
#include "StringUtils.hpp"

enum class ActionStatus { Doing, Done, Failed, Interrupted };

class AgentAction {
public:
  virtual ~AgentAction() = default;

  // Returns true when the action is completely finished
  virtual ActionStatus update(float deltaTime, flecs::entity entity) = 0;
  virtual ActionStatus handleInterruption(flecs::entity entity) = 0;
  virtual void resume(flecs::entity entity) = 0;
  virtual std::string getSuccessMessage() = 0;
  virtual std::string getFailureMessage() {
          return "System: Your action has failed. What's next?\n";
        }
      };

      class InvalidAction : public AgentAction {
      public:
        InvalidAction() : time(10.0f * 1000.0f) {};

        ActionStatus update(float deltaTime, flecs::entity) override {
          time -= deltaTime;
          if (time <= 0.0f) {
            return ActionStatus::Done;
          }
          return ActionStatus::Doing;
        }

        std::string getSuccessMessage() override {
          return "System: Your previous action was invalid or unrecognized. Please remember to use one of the available commands: [DO_NOTHING], [MOVE_TO $LOCATION], [TALK_TO $CHARACTER], [CHARACTERS], or [LOCATIONS].\n";
        }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        float time;
      };

      class WaitAction : public AgentAction {
      public:
        // time is passed in as seconds, but our deltaTime is in milliseconds
        WaitAction(float time_seconds) : time(time_seconds * 1000.0f) {};

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

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

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

        ActionStatus handleInterruption(flecs::entity entity) override {
          entity.remove<MOVE_THROUGH_PATH_ACTION>();
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      protected:
        GamePosition targetPos;
      };

      class MoveToObjectAction : public MoveAction {
      public:
        MoveToObjectAction(GamePosition target, flecs::entity obj) 
            : MoveAction(target), targetObject(obj) {};

        ActionStatus update(float dt, flecs::entity entity) override {
          ActionStatus status = MoveAction::update(dt, entity);
          if (status == ActionStatus::Done && targetObject.is_alive()) {
            entity.set<InteractionTarget>({targetObject});
          }
          return status;
        }

        std::string getSuccessMessage() override {
          if (!targetObject.is_alive()) return "System: The object is no longer here.\n";
          
          std::string objName = "Object";
          if (targetObject.has<DisplayName>()) {
            objName = targetObject.get<DisplayName>()->name;
          }
          std::string options = "System: You have arrived at the " + objName + ". Available actions:\n";
          int optionNum = 1;
          if (targetObject.has<Harvestable>()) {
             options += std::to_string(optionNum++) + ") Harvest\n";
          }
          if (targetObject.has<Workstation>()) {
             options += std::to_string(optionNum++) + ") Craft\n";
          }
          options += std::to_string(optionNum++) + ") Examine\n";
          options += "To choose, use [INTERACT $NUMBER].\n";
          
          return options;
        }
      private:
        flecs::entity targetObject;
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

        ActionStatus handleInterruption(flecs::entity entity) override {
          entity.remove<MOVE_THROUGH_PATH_ACTION>();
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

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
        ActionStatus handleInterruption(flecs::entity entity) override;
        void resume(flecs::entity entity) override;
        std::string getSuccessMessage() override;
        std::string getFailureMessage() override;

        ConversationState state;
      private:
        std::string targetName;
        flecs::entity targetEntity;
        bool isTalkingToSelf = false;
      };

      class CharactersAction : public AgentAction {
      public:
        ActionStatus update(float, flecs::entity entity) override {
          std::string characterNames;
          auto name_filter = entity.world().filter<DisplayName, CharacterTag>();
          
          const DisplayName* myNameComp = entity.get<DisplayName>();
          std::string myName = myNameComp ? myNameComp->name : "";

          name_filter.each([&characterNames, &myName](const DisplayName &displayName, const CharacterTag &) {
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

  ActionStatus handleInterruption(flecs::entity) override {
    return ActionStatus::Interrupted;
  }

  void resume(flecs::entity) override {}

private:
  std::string response;
};
      class LocationsAction : public AgentAction {
      public:
        ActionStatus update(float, flecs::entity entity) override {
          std::string locationsList;
          Map* currMap = entity.world().get<MapResource>()->map;
          
          if (currMap) {
            auto names = currMap->GetAllLocationNames();
            for (const auto& name : names) {
              locationsList += name + ", ";
            }
            if (locationsList.length() >= 2) {
              locationsList.erase(locationsList.length() - 2);
            }
          }

          if (locationsList.empty()) {
            response = "System: There are no mapped locations available.\n";
          } else {
            response = "System: The following locations are available to move to: " + locationsList + "\n";
          }

          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override {
          return response;
        }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string response;
      };

      class ObjectsAction : public AgentAction {
      public:
        ActionStatus update(float, flecs::entity entity) override {
          if (isFirstUpdate) {
            isFirstUpdate = false;
            successMsg = "System: Nearby objects:\n";
            const GamePosition currentPos = *entity.get<GamePosition>();
            bool found = false;

            entity.world().filter<Interactable, DisplayName, GamePosition>().each(
                [&](flecs::entity obj, const Interactable&, const DisplayName& dName, const GamePosition& pos) {
                  if (std::abs(pos.x - currentPos.x) <= DEFAULT_OBJECT_SEARCH_RADIUS && std::abs(pos.y - currentPos.y) <= DEFAULT_OBJECT_SEARCH_RADIUS) {
                    successMsg += "- " + dName.name + "\n";
                    found = true;
                  }
                });

            if (!found) {
              successMsg = "System: There are no objects nearby.\n";
            }
          }
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override {
          return successMsg;
        }
        
        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }
        
        void resume(flecs::entity) override {}

      private:
        std::string successMsg;
        bool isFirstUpdate = true;
      };

      class InventoryAction : public AgentAction {
      public:
        ActionStatus update(float, flecs::entity entity) override {
          std::string inventoryList;
          entity.each<Holds>([&](flecs::entity child) {
            if (child.is_alive() && child.has<DisplayName>()) {
              inventoryList += "- " + child.get<DisplayName>()->name + "\n";
            }
          });

          if (inventoryList.empty()) {
            response = "System: Your inventory is empty.\n";
          } else {
            response = "System: You are currently holding:\n" + inventoryList;
          }

          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override {
          return response;
        }
        
        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }
        
        void resume(flecs::entity) override {}

      private:
        std::string response;
      };

      class InteractAction : public AgentAction {
      public:
        InteractAction(int option) : option(option) {}
        ActionStatus update(float, flecs::entity entity) override {
          if (isFirstUpdate) {
            isFirstUpdate = false;
            auto target = entity.get<InteractionTarget>();
            if (!target || !target->targetEntity.is_alive()) {
              successMsg = "System: The object is no longer here.\n";
              return ActionStatus::Done;
            }

            flecs::entity targetObj = target->targetEntity;
            auto interactions = InteractionRegistry::GetAvailableInteractions(targetObj);

            if (option < 1 || option > interactions.size()) {
              successMsg = "System: Invalid option selected.\n";
            } else {
              successMsg = interactions[option - 1].execute(entity, targetObj);
            }
          }
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override {
          return successMsg;
        }
        
        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }
        
        void resume(flecs::entity) override {}

      private:
        int option;
        bool isFirstUpdate = true;
        std::string successMsg;
      };

