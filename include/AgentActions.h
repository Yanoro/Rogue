#pragma once

#include "Components.h"
#include "Map.h"
#include "PathFinding.h"
#include "Defaults.h"
#include "ObjectFactory.h"
#include "InteractionRegistry.h"

#include <flecs.h>
#include <string>
#include <utility>
#include "StringUtils.hpp"
#include "GlobalCommandRegistry.h"

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

  // Tag-style name of the action (e.g. "MOVE_TO", "HARVEST"). Shown by the debug UI.
  virtual std::string getActionName() const { return "ACTION"; }
      };

      class InvalidAction : public AgentAction {
      public:
        InvalidAction() : time(2.0f * 1000.0f) {};

        std::string getActionName() const override { return "INVALID"; }

        ActionStatus update(float deltaTime, flecs::entity) override {
          time -= deltaTime;
          if (time <= 0.0f) {
            return ActionStatus::Done;
          }
          return ActionStatus::Doing;
        }

        std::string getSuccessMessage() override {
          std::string msg = "System: Your previous action was invalid or unrecognized. Please remember to use one of the available commands: ";
          bool first = true;
          for (const auto& cmd : GlobalCommandRegistry::GetCommands()) {
            // [GENERIC_INTERACT] is internal, skip it
            if (cmd.format == "[GENERIC_INTERACT]" || cmd.format.empty()) continue;
            
            if (!first) msg += ", ";
            msg += cmd.format;
            first = false;
          }
          msg += ".\n";
          return msg;
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

        std::string getActionName() const override { return "WAIT"; }

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

      class MoveToEntityAction : public AgentAction {
      public:
        MoveToEntityAction(std::string targetName) : targetName(targetName), lastKnownTargetPos({-1, -1}) {};

        std::string getActionName() const override { return "MOVE_TO"; }

        ActionStatus update(float, flecs::entity entity) override {
          Map *map = entity.world().get<MapResource>()->map;
          const GamePosition startPos = *entity.get<GamePosition>();

          GamePosition targetPos = {-1, -1};
          flecs::entity resolvedTarget = flecs::entity::null();
          bool found = false;

          entity.world().filter<DisplayName, GamePosition>().each(
            [&](flecs::entity other, const DisplayName &name, const GamePosition &pos) {
              if (StringUtils::EqualsIgnoreCase(name.name, targetName)) {
                targetPos = pos;
                resolvedTarget = other;
                found = true;
              }
          });

          if (!found) {
            if (map) {
              for (const auto& loc : map->GetLocations()) {
                if (StringUtils::EqualsIgnoreCase(loc->name, targetName)) {
                  targetPos = loc->pos;
                  found = true;
                  break;
                }
              }
            }
          }

          // A location is not an entity, so this stays null for those.
          targetEntity = resolvedTarget;

          if (!found) {
            entity.remove<MOVE_THROUGH_PATH_ACTION>();
            return ActionStatus::Failed;
          }

          if (startPos == targetPos || Map::AreNeighbours(startPos, targetPos)) {
            entity.remove<MOVE_THROUGH_PATH_ACTION>();
            // Objects expose their verbs through InteractionTarget so a
            // follow-up [STORE] / [HARVEST] resolves against this target.
            // Characters are talked to, so they are deliberately excluded.
            if (targetEntity.is_alive() && !targetEntity.has<CharacterTag>()) {
              entity.set<InteractionTarget>({targetEntity});
            }
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
          std::string message =
              "System: You have arrived next to " + targetName + ".\n";

          if (!targetEntity.is_alive() || targetEntity.has<CharacterTag>()) {
            return message;
          }

          auto interactions =
              InteractionRegistry::GetAvailableInteractions(targetEntity);
          if (interactions.empty()) {
            return message;
          }

          message += "You can interact with the " + targetName + " using:\n";
          for (const auto& interaction : interactions) {
            message += FormatInteractionForAI(StringUtils::ToUpper(interaction.name),
                                              interaction.description);
          }
          return message;
        }

        std::string getFailureMessage() override {
          return "System: Could not find character or location " + targetName + " to move to. What's next?\n";
        }

        ActionStatus handleInterruption(flecs::entity entity) override {
          entity.remove<MOVE_THROUGH_PATH_ACTION>();
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string targetName;
        GamePosition lastKnownTargetPos;
        // Resolved while pathing so the arrival message can list this object's
        // interactions, and so InteractionTarget can point at it.
        flecs::entity targetEntity = flecs::entity::null();
      };

      enum class ConversationState {
        Talking,
        Listening,
        Ended
      };

      class TalkAction : public AgentAction {
      public:
        TalkAction(flecs::entity sourceEntity, std::string targetName, ConversationState state = ConversationState::Talking);

        std::string getActionName() const override { return "TALK_TO"; }

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
        // Backoff in milliseconds after an empty response, so a failing provider
        // is not asked again once per frame.
        float retryCooldownMs = 0.0f;
      };

      class CharactersAction : public AgentAction {
      public:
        std::string getActionName() const override { return "CHARACTERS"; }

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
        std::string getActionName() const override { return "LOCATIONS"; }

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

      inline std::string GetSurroundingsRadar(flecs::entity entity, int radius) {
        const GamePosition currentPos = *entity.get<GamePosition>();
        Map *map = entity.world().get<MapResource>()->map;

        std::string grid = "";
        std::map<char, std::string> legend;

        // Mark the agent
        legend['@'] = "You";

        for (int y = currentPos.y - radius; y <= currentPos.y + radius; ++y) {
          for (int x = currentPos.x - radius; x <= currentPos.x + radius; ++x) {
            if (x == currentPos.x && y == currentPos.y) {
              grid += "@";
              continue;
            }

            char tileChar = ' ';
            if (map && map->IsInBounds(x, y)) {
              auto tile = map->GetTile(x, y);
              if (tile) {
                tileChar = tile->ascii->ch;
              }
            }

            bool entityFound = false;
            entity.world().filter<GamePosition, DrawAscii, DisplayName>().each(
              [&](flecs::entity other, const GamePosition& pos, const DrawAscii& ascii, const DisplayName& dName) {
                if (pos.x == x && pos.y == y) {
                  tileChar = ascii.ch;
                  legend[tileChar] = dName.name;
                  entityFound = true;
                }
              }
            );

            grid += tileChar;
          }
          grid += "\n";
        }

        std::string result = "System: Surroundings (" + std::to_string(radius*2+1) + "x" + std::to_string(radius*2+1) + " grid):\n```\n" + grid + "```\nLegend:\n";
        for (const auto& pair : legend) {
           result += std::string(1, pair.first) + " = " + pair.second + "\n";
        }
        return result;
      }

      class SurroundingsAction : public AgentAction {
      public:
        std::string getActionName() const override { return "SURROUNDINGS"; }

        ActionStatus update(float, flecs::entity entity) override {
          if (isFirstUpdate) {
            isFirstUpdate = false;
            successMsg = GetSurroundingsRadar(entity, 2);
            successMsg += "\nSystem: To interact with an object close to you, you may use the [MOVE_TO $OBJECT] command.\n";
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
        bool isFirstUpdate = true;
        std::string successMsg;
      };

      class InventoryAction : public AgentAction {
      public:
        std::string getActionName() const override { return "INVENTORY"; }

        ActionStatus update(float, flecs::entity entity) override {
          // Stack identical items: one line per distinct item name, with "(N)"
          // appended when more than one is held.
          std::vector<std::pair<std::string, int>> itemCounts;
          entity.each<Holds>([&](flecs::entity child) {
            if (child.is_alive() && child.has<DisplayName>()) {
              const std::string& name = child.get<DisplayName>()->name;
              for (auto& entry : itemCounts) {
                if (entry.first == name) {
                  entry.second++;
                  return;
                }
              }
              itemCounts.emplace_back(name, 1);
            }
          });

          if (itemCounts.empty()) {
            response = "System: Your inventory is empty.\n";
          } else {
            std::string inventoryList;
            for (const auto& entry : itemCounts) {
              inventoryList += "- " + entry.first;
              if (entry.second > 1) {
                inventoryList += " (" + std::to_string(entry.second) + ")";
              }
              inventoryList += "\n";
            }
            response = "System: You are currently holding:\n" + inventoryList + 
                       "\nYou can use [EXAMINE_ITEM $ITEM_NAME] to see what actions are available for an item.";
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

      class ExamineItemAction : public AgentAction {
      public:
        ExamineItemAction(std::string itemName) : itemName(itemName) {}

        std::string getActionName() const override { return "EXAMINE_ITEM"; }

        ActionStatus update(float, flecs::entity entity) override {
          flecs::entity targetItem = flecs::entity::null();
          
          entity.each<Holds>([&](flecs::entity child) {
            if (child.is_alive() && child.has<DisplayName>()) {
              if (StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, itemName)) {
                targetItem = child;
              }
            }
          });

          if (!targetItem.is_alive()) {
            response = "System: Invalid item name. You don't have an item named " + itemName + ".\n";
            return ActionStatus::Done;
          }

          std::string objName = targetItem.has<DisplayName>() ? targetItem.get<DisplayName>()->name : "Item";
          response = "System: Available actions for " + objName + ":\n";

          auto interactions = ItemInteractionRegistry::GetAvailableInteractions(targetItem);
          if (interactions.empty()) {
            response += "- None\n";
          } else {
            for (const auto& interaction : interactions) {
              response += FormatInteractionForAI(StringUtils::ToUpper(interaction.name),
                                                 interaction.description);
            }
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
        std::string itemName;
        std::string response;
      };

      class GenericInteractAction : public AgentAction {
      public:
        GenericInteractAction(std::string commandName, std::string commandArgs) : commandName(commandName), commandArgs(commandArgs) {}

        // Surfaces the actual verb the model emitted, e.g. "HARVEST" or "STORE".
        std::string getActionName() const override { return StringUtils::ToUpper(commandName); }
        ActionStatus update(float, flecs::entity entity) override {
          if (isFirstUpdate) {
            isFirstUpdate = false;
            bool found = false;

            // 1. Try to see if this is an Item Interaction (Inventory)
            if (!commandArgs.empty()) {
              flecs::entity targetItem = flecs::entity::null();
              entity.each<Holds>([&](flecs::entity child) {
                if (child.is_alive() && child.has<DisplayName>()) {
                  if (StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, commandArgs)) {
                    targetItem = child;
                  }
                }
              });

              if (targetItem.is_alive()) {
                auto itemInteractions = ItemInteractionRegistry::GetAvailableInteractions(targetItem);
                for (const auto& interaction : itemInteractions) {
                  if (StringUtils::EqualsIgnoreCase(interaction.name, commandName)) {
                    successMsg = interaction.execute(entity, targetItem);
                    found = true;
                    break;
                  }
                }
              }
            }

            // 2. Fall back to World Interaction
            if (!found) {
              auto target = entity.get<InteractionTarget>();
              if (!target || !target->targetEntity.is_alive()) {
                successMsg = "System: Invalid command, or you are not near any interactive object. You must use [MOVE_TO $TARGET] first to interact with world objects.\n";
                return ActionStatus::Done;
              }

              flecs::entity targetObj = target->targetEntity;
              auto interactions = InteractionRegistry::GetAvailableInteractions(targetObj);

              for (const auto& interaction : interactions) {
                if (StringUtils::EqualsIgnoreCase(interaction.name, commandName)) {
                  successMsg = interaction.execute(entity, targetObj, commandArgs);
                  found = true;
                  break;
                }
              }

              if (!found) {
                successMsg = "System: Invalid command or action not available for this object.\n";
              }
            }

            // The interaction may have started a timed action (e.g. [HARVEST] sets
            // Busy). Component writes are deferred inside an ECS system, so that
            // Busy is not visible yet: hold this frame and let the poll below
            // decide from the next tick. Returning Done here would drop the action
            // immediately and the agent would be prompted again mid-harvest.
            return ActionStatus::Doing;
          }

          if (entity.has<Busy>()) {
            return ActionStatus::Doing;
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
        std::string commandName;
        std::string commandArgs;
        bool isFirstUpdate = true;
        std::string successMsg;
      };

      class PlantAtAction : public AgentAction {
      public:
        PlantAtAction(std::string coordsStr, std::string seedName) : coordsStr(coordsStr), seedName(seedName) {}

        std::string getActionName() const override { return "PLANT_AT"; }

        ActionStatus update(float, flecs::entity entity) override {
          if (isFirstUpdate) {
            isFirstUpdate = false;
            
            // Parse coordinates
            std::vector<std::pair<int, int>> coords;
            std::istringstream iss(coordsStr);
            std::string coord;
            while (iss >> coord) {
              size_t comma = coord.find(',');
              if (comma != std::string::npos) {
                try {
                  int x = std::stoi(coord.substr(0, comma));
                  int y = std::stoi(coord.substr(comma + 1));
                  coords.push_back({x, y});
                } catch (...) {
                  // ignore invalid
                }
              }
            }

            if (coords.empty()) {
              successMsg = "System: Invalid coordinates format. Use [PLANT_AT X,Y ... " + seedName + "]\n";
              return ActionStatus::Done;
            }

            int plantedCount = 0;
            GamePosition currentPos = *entity.get<GamePosition>();

            for (auto& c : coords) {
              flecs::entity seedItem = flecs::entity::null();
              entity.each<Holds>([&](flecs::entity child) {
                if (!seedItem.is_alive() && child.is_alive() && child.has<DisplayName>()) {
                  if (StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, seedName)) {
                    seedItem = child;
                  }
                }
              });

              if (!seedItem.is_alive()) {
                successMsg += "System: You ran out of " + seedName + " after planting " + std::to_string(plantedCount) + ".\n";
                break;
              }

              // Plant it
              entity.remove<Holds>(seedItem);
              seedItem.remove(flecs::ChildOf, entity);
              seedItem.remove<Portable>();
              if (seedItem.has<Evolvable>()) {
                seedItem.get_mut<Evolvable>()->isActive = true;
              }
              
              GamePosition pos = {currentPos.x + c.first, currentPos.y + c.second};
              seedItem.set<GamePosition>(pos);
              seedItem.set<ScreenPosition>(entity.world().get<MapResource>()->map->GameCoordsToScreenCoords(pos.x, pos.y));
              
              plantedCount++;
            }

            if (plantedCount > 0) {
              successMsg += "System: You successfully planted " + std::to_string(plantedCount) + " " + seedName + ".\n";
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
        std::string coordsStr;
        std::string seedName;
        bool isFirstUpdate = true;
        std::string successMsg;
      };

