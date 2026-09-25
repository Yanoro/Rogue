#pragma once

#include "Components.h"
#include "Map.h"
#include "PathFinding.h"
#include "Defaults.h"
#include "EquipmentRuntime.hpp"
#include "ObjectFactory.h"
#include "InteractionRegistry.h"

#include <algorithm>
#include <cstdio>
#include <flecs.h>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
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

  // What the action acts on (e.g. the destination of a MOVE_TO, the partner of
  // a TALK_TO), shown next to the name by the debug UI. Empty when the action
  // has no interesting subject. Called once per frame while the NPC menu is
  // open, so keep it cheap and side-effect free.
  virtual std::string getActionContext() const { return ""; }
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
            // Hidden verbs (the trade commands, [PLACE], [GENERIC_INTERACT])
            // are surfaced contextually rather than advertised here.
            if (cmd.hidden || cmd.format.empty()) continue;
            
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

        // The only interesting thing about a wait is how much of it is left.
        // `time` is milliseconds, see the constructor.
        std::string getActionContext() const override {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%.1fs left", time / 1000.0f);
          return buf;
        }

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

        std::string getActionContext() const override {
          // Prefer the resolved entity's name so the label shows the canonical
          // spelling. Locations have no entity, so fall back to the request.
          if (targetEntity.is_alive() && targetEntity.has<DisplayName>()) {
            return targetEntity.get<DisplayName>()->name;
          }
          return targetName;
        }

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

        std::string getActionContext() const override;

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
        // Consecutive free-look [INVENTORY] calls within the current turn. Reset
        // whenever the turn ends, so the cap only bounds one agent stalling.
        int consecutivePeeks = 0;
      };

      // A trade verb emitted outside a conversation has nothing to act on. This
      // exists purely to tell the model to start a conversation, instead of
      // handing it the generic "invalid command" reply.
      class TradeOutOfContextAction : public AgentAction {
      public:
        explicit TradeOutOfContextAction(std::string verbName)
            : verb(verbName) {}

        std::string getActionName() const override { return verb; }

        // The verb alone does not explain why this action exists: it is always
        // the "you are not in a conversation" reply to a trade verb.
        std::string getActionContext() const override {
          return "outside a conversation";
        }

        ActionStatus update(float, flecs::entity) override {
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override {
          return "System: [" + verb +
                 "] only works while you are in a conversation with someone. "
                 "Use [TALK_TO $TARGET] to start talking, then use the trade "
                 "commands inside the conversation.\n";
        }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string verb;
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

      // Glyph grid of the tiles around the agent. Kept for the placement hint,
      // where [PLACE] consumes relative coordinates and a picture of the ground
      // is exactly what the model needs. Agents asking what is around them want
      // GetSurroundingsList instead: a grid renders one character per tile, so it
      // cannot show an item lying on the agent's own tile, drops anything without
      // a DrawAscii, and gives no coordinates to anchor offsets on.
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

            char tileChar = '?';
            if (map && map->IsInBounds(x, y)) {
              auto tile = map->GetTile(x, y);
              if (tile) {
                tileChar = tile->ascii->ch;
                // Terrain is what placement validation checks against ("tilled
                // soil", "path"), so the grid has to name it. Without this the
                // agent can only see that two tiles look different, not which
                // one it is allowed to place on.
                legend[tileChar] = tile->name;
              }
            } else {
              legend['?'] = "outside the map";
            }

            entity.world().filter<GamePosition, DrawAscii, DisplayName>().each(
              [&](flecs::entity, const GamePosition& pos, const DrawAscii& ascii, const DisplayName& dName) {
                if (pos.x == x && pos.y == y) {
                  tileChar = ascii.ch;
                  legend[tileChar] = dName.name;
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

      // What is close enough to walk to and interact with, as exact names plus
      // relative offsets. Names are the DisplayName verbatim, because that is the
      // string [MOVE_TO] matches on; offsets are relative to the agent, +X East
      // and +Y South, matching the convention [PLACE] uses.
      inline std::string GetSurroundingsList(flecs::entity entity, int radius = DEFAULT_SURROUNDINGS_RADIUS) {
        const GamePosition currentPos = *entity.get<GamePosition>();
        Map *map = nullptr;
        if (auto *mapRes = entity.world().get<MapResource>()) {
          map = mapRes->map;
        }

        struct Nearby {
          std::string name;
          int dx;
          int dy;
          int distance;
        };
        std::vector<Nearby> objects;
        std::vector<Nearby> items;
        std::vector<Nearby> characters;

        // One pass over every named thing, keeping whatever falls inside the
        // radius. Chebyshev distance, so the four diagonal neighbours count as
        // one tile away rather than two.
        entity.world().filter<GamePosition, DisplayName>().each(
            [&](flecs::entity other, const GamePosition &pos, const DisplayName &displayName) {
              if (other == entity || displayName.name.empty()) {
                return;
              }
              const int dx = pos.x - currentPos.x;
              const int dy = pos.y - currentPos.y;
              const int distanceX = dx < 0 ? -dx : dx;
              const int distanceY = dy < 0 ? -dy : dy;
              const int distance = distanceX > distanceY ? distanceX : distanceY;
              if (distance > radius) {
                return;
              }
              const Nearby found{displayName.name, dx, dy, distance};
              if (other.has<CharacterTag>()) {
                characters.push_back(found);
              } else if (other.has<Portable>()) {
                items.push_back(found);
              } else {
                objects.push_back(found);
              }
            });

        const auto closestFirst = [](const Nearby &a, const Nearby &b) {
          if (a.distance != b.distance) return a.distance < b.distance;
          if (a.dy != b.dy) return a.dy < b.dy;
          if (a.dx != b.dx) return a.dx < b.dx;
          return a.name < b.name;
        };
        std::sort(objects.begin(), objects.end(), closestFirst);
        std::sort(items.begin(), items.end(), closestFirst);
        std::sort(characters.begin(), characters.end(), closestFirst);

        const auto describe = [](const Nearby &found) {
          std::string line = "- " + found.name + " at (" + std::to_string(found.dx) + "," +
                             std::to_string(found.dy) + ")";
          if (found.distance == 0) {
            line += ", on your tile";
          } else if (found.distance == 1) {
            line += ", 1 tile away";
          } else {
            line += ", " + std::to_string(found.distance) + " tiles away";
          }
          return line;
        };

        std::string result = "System: You are at ";
        if (Location *location = map ? map->GetLocation(currentPos) : nullptr) {
          result += location->name;
        } else {
          result += "an unnamed spot";
        }
        result += " (" + std::to_string(currentPos.x) + "," + std::to_string(currentPos.y) + ").\n";

        if (objects.empty() && items.empty() && characters.empty()) {
          result += "System: Nothing within " + std::to_string(radius) +
                    " tiles to move to or interact with. Use [LOCATIONS] for named places you "
                    "can travel to.\n";
          return result;
        }

        result += "Nearby within " + std::to_string(radius) +
                  " tiles (offsets from you: +X East, +Y South):\n";
        const auto appendSection = [&](const std::string &title, const std::vector<Nearby> &list) {
          if (list.empty()) {
            return;
          }
          result += title + ":\n";
          for (const auto &found : list) {
            result += describe(found) + "\n";
          }
        };
        appendSection("Objects", objects);
        appendSection("Items on the ground", items);
        appendSection("Characters", characters);
        result += "System: Use a name exactly as written with [MOVE_TO $NAME] to walk to it. "
                  "Directions such as \"east\" and bare coordinates are not valid targets.\n";
        return result;
      }

      class SurroundingsAction : public AgentAction {
      public:
        std::string getActionName() const override { return "SURROUNDINGS"; }

        ActionStatus update(float, flecs::entity entity) override {
          if (isFirstUpdate) {
            isFirstUpdate = false;
            successMsg = GetSurroundingsList(entity);
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
          // Stack identical items: one entry per distinct name, with "(N)"
          // appended when more than one is held. Shared with the chest examine
          // text so both readings of a container agree.
          //
          // A worn item is annotated rather than hidden: it is still carried, and
          // the annotation is what makes "the equipped scythe" distinguishable
          // from "a second scythe in the pack" (which stacks as its own entry).
          std::vector<std::string> itemNames;
          entity.each<Holds>([&](flecs::entity child) {
            if (child.is_alive() && child.has<DisplayName>()) {
              itemNames.push_back(child.get<DisplayName>()->name +
                                  EquippedSuffix(child));
            }
          });

          if (itemNames.empty()) {
            response = "System: Your inventory is empty.\n";
          } else {
            std::string inventoryList;
            for (const auto& entry : StringUtils::StackNames(itemNames)) {
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

        std::string getActionContext() const override { return itemName; }

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
          if (const Durability* durability = targetItem.get<Durability>()) {
            response += "System: " + objName + " has " +
                        std::to_string(durability->current) + " of " +
                        std::to_string(durability->max) +
                        " durability left; it breaks at 0.\n";
          }

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

      // Puts a carried item on. The slot rules -- which slot kind, how many
      // instances, what to take off to make room -- live in Equipment.hpp and
      // the item definition, so this verb knows nothing about particular items.
      class EquipAction : public AgentAction {
      public:
        EquipAction(std::string itemName) : itemName(std::move(itemName)) {}

        std::string getActionName() const override { return "EQUIP"; }

        std::string getActionContext() const override { return itemName; }

        ActionStatus update(float, flecs::entity entity) override {
          flecs::entity item = FindHeldItem(entity, itemName);
          if (!item.is_alive()) {
            response = "System: You don't have an item named " + itemName + ".\n";
            return ActionStatus::Done;
          }
          response = EquipItem(entity, item, ItemsOf(entity.world())).message;
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string itemName;
        std::string response;
      };

      // Takes one worn item off, leaving it in inventory.
      class UnequipAction : public AgentAction {
      public:
        UnequipAction(std::string itemName) : itemName(std::move(itemName)) {}

        std::string getActionName() const override { return "UNEQUIP"; }

        std::string getActionContext() const override { return itemName; }

        ActionStatus update(float, flecs::entity entity) override {
          flecs::entity item = FindHeldItem(entity, itemName);
          if (!item.is_alive()) {
            response = "System: You don't have an item named " + itemName + ".\n";
            return ActionStatus::Done;
          }
          response = UnequipItem(entity, item);
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string itemName;
        std::string response;
      };

      // Lists what the character is wearing and where.
      class EquipmentAction : public AgentAction {
      public:
        std::string getActionName() const override { return "EQUIPMENT"; }

        ActionStatus update(float, flecs::entity entity) override {
          response = FormatEquipment(entity);
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string response;
      };

      class GenericInteractAction : public AgentAction {
      public:
        GenericInteractAction(std::string commandName, std::string commandArgs) : commandName(commandName), commandArgs(commandArgs) {}

        // Surfaces the actual verb the model emitted, e.g. "HARVEST" or "STORE".
        std::string getActionName() const override { return StringUtils::ToUpper(commandName); }

        // update() resolves the verb against an inventory item first and the
        // world InteractionTarget second; report whichever it landed on, plus
        // any extra argument (e.g. a crafting recipe) it was called with.
        std::string getActionContext() const override {
          if (resolvedTargetName.empty()) {
            return commandArgs;
          }
          if (commandArgs.empty()) {
            return resolvedTargetName;
          }
          return resolvedTargetName + " (" + commandArgs + ")";
        }

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
                if (targetItem.has<DisplayName>()) {
                  resolvedTargetName = targetItem.get<DisplayName>()->name;
                }
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
              if (targetObj.has<DisplayName>()) {
                resolvedTargetName = targetObj.get<DisplayName>()->name;
              }
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
        // Name of the object/item update() matched the verb against. Empty until
        // the first update, so the debug label fills in a frame later.
        std::string resolvedTargetName;
      };

      // Sets a carried item down in the world at one or more relative
      // coordinates. What may be placed, where it may go and what it becomes are
      // declared by the item template's "placement" block, so this verb knows
      // nothing about particular items: "planting" is just placement where the
      // placed form happens to grow. See docs/placement-design.md.
      class PlaceAtAction : public AgentAction {
      public:
        PlaceAtAction(std::string itemName, std::string coordsStr)
            : itemName(itemName), coordsStr(coordsStr) {}

        std::string getActionName() const override { return "PLACE"; }

        // Which item is going down; the coordinates are already in the command
        // the model wrote.
        std::string getActionContext() const override { return itemName; }

        ActionStatus update(float, flecs::entity entity) override {
          if (isFirstUpdate) {
            isFirstUpdate = false;
            Place(entity);
          }
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return successMsg; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        // A coordinate that passed every rule, paired with the ground it will
        // land on so the reply can name it.
        struct Ready {
          std::pair<int, int> offset;
          std::string ground;
        };

        static std::string Offset(int dx, int dy) {
          return "(" + std::to_string(dx) + "," + std::to_string(dy) + ")";
        }

        static std::string JoinList(const std::vector<std::string>& values) {
          std::string joined;
          for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) joined += (i + 1 == values.size()) ? " or " : ", ";
            joined += values[i];
          }
          return joined;
        }

        void Place(flecs::entity entity) {
          std::vector<std::pair<int, int>> requested;
          int malformed = 0;
          std::istringstream iss(coordsStr);
          std::string coord;
          while (iss >> coord) {
            const size_t comma = coord.find(',');
            if (comma == std::string::npos) {
              ++malformed;
              continue;
            }
            try {
              requested.push_back({std::stoi(coord.substr(0, comma)),
                                   std::stoi(coord.substr(comma + 1))});
            } catch (...) {
              ++malformed;
            }
          }

          if (requested.empty()) {
            successMsg = "System: No usable coordinates. Use [PLACE " + itemName +
                         " AT X,Y ...], for example [PLACE " + itemName + " AT 1,0].\n";
            return;
          }

          Map *map = nullptr;
          if (auto *mapRes = entity.world().get<MapResource>()) {
            map = mapRes->map;
          }
          if (!map) {
            successMsg = "System: There is no map here to place anything on.\n";
            return;
          }

          // Collect first, then move: removing Holds mutates the relationship
          // this scan walks, and this action runs inside a system iteration,
          // which is a deferred context, so re-querying mid-loop would hand back
          // the same item on every pass. Same shape as the [STORE] handler.
          std::vector<flecs::entity> held;
          entity.each<Holds>([&](flecs::entity child) {
            if (child.is_alive() && child.has<DisplayName>() &&
                StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, itemName)) {
              held.push_back(child);
            }
          });

          if (held.empty()) {
            successMsg = "System: You are not carrying any " + itemName + ".\n";
            return;
          }

          // Resolve the display name to a single item type. The policy lives on
          // the template and the project matches on ItemType rather than the
          // display name, so a same-named item of another type is never consumed
          // by this command.
          std::vector<flecs::entity> candidates;
          std::string targetType;
          for (flecs::entity item : held) {
            if (!item.has<Placeable>()) {
              continue;
            }
            const std::string type =
                item.has<ItemType>() ? item.get<ItemType>()->id : std::string();
            if (candidates.empty()) {
              targetType = type;
            } else if (type != targetType) {
              continue;
            }
            candidates.push_back(item);
          }

          if (candidates.empty()) {
            successMsg = "System: " + itemName + " cannot be placed in the world.\n";
            return;
          }

          const Placeable policy = *candidates.front().get<Placeable>();
          const GamePosition currentPos = *entity.get<GamePosition>();

          // Everything already standing on the map, gathered in one pass rather
          // than rescanning per coordinate. Placements queued earlier in this
          // same command are not visible here (deferred context), which is what
          // the duplicate check below covers.
          std::map<std::pair<int, int>, std::string> occupied;
          entity.world().filter<GamePosition, DisplayName>().each(
              [&](flecs::entity other, const GamePosition &pos, const DisplayName &name) {
                if (other != entity) {
                  occupied.emplace(std::make_pair(pos.x, pos.y), name.name);
                }
              });

          std::vector<Ready> ready;
          std::vector<std::string> rejected;
          std::set<std::pair<int, int>> seen;

          for (const auto &c : requested) {
            const std::string at = Offset(c.first, c.second);

            if (!seen.insert(c).second) {
              rejected.push_back(at + ": duplicate coordinate");
              continue;
            }

            const int distanceX = c.first < 0 ? -c.first : c.first;
            const int distanceY = c.second < 0 ? -c.second : c.second;
            const int distance = distanceX > distanceY ? distanceX : distanceY;
            if (distance == 0) {
              rejected.push_back(at + ": you are standing there");
              continue;
            }
            if (distance > policy.maxDistance) {
              rejected.push_back(at + ": " + std::to_string(distance) + " tiles away (max " +
                                 std::to_string(policy.maxDistance) + ")");
              continue;
            }

            const int x = currentPos.x + c.first;
            const int y = currentPos.y + c.second;
            if (!map->IsInBounds(x, y)) {
              rejected.push_back(at + ": outside the map");
              continue;
            }

            Tile *tile = map->GetTile(x, y);
            const std::string ground =
                (tile && !tile->name.empty()) ? tile->name : "unknown ground";
            if (tile && tile->blocksTile) {
              rejected.push_back(at + ": " + ground + " cannot be placed on");
              continue;
            }

            if (!policy.surface.empty()) {
              bool allowed = false;
              for (const auto &surface : policy.surface) {
                if (surface == ground) {
                  allowed = true;
                  break;
                }
              }
              if (!allowed) {
                rejected.push_back(at + ": " + ground + " is not " +
                                   JoinList(policy.surface));
                continue;
              }
            }

            const auto occupant = occupied.find(std::make_pair(x, y));
            if (occupant != occupied.end()) {
              rejected.push_back(at + ": occupied by " + occupant->second);
              continue;
            }

            ready.push_back({c, ground});
          }

          // Everything above is validation, so a rejected coordinate never costs
          // an item. Only now is anything consumed.
          ObjectFactory *factory = nullptr;
          if (auto *factoryRes = entity.world().get<ObjectFactoryResource>()) {
            factory = factoryRes->factory;
          }

          int placedCount = 0;
          std::string placedDetail;
          size_t next = 0;
          for (const Ready &target : ready) {
            if (next >= candidates.size()) {
              rejected.push_back(Offset(target.offset.first, target.offset.second) +
                                 ": no " + itemName + " left");
              continue;
            }
            flecs::entity item = candidates[next++];

            // Setting an item down takes it off first: the entity is reused as
            // the placed object, so a leftover Equipped marker would ride along.
            DropEquipped(item);
            entity.remove<Holds>(item);
            item.remove(flecs::ChildOf, entity);
            if (factory) {
              factory->ApplyTemplate(item, policy.becomes, map);
            }

            const GamePosition pos = {currentPos.x + target.offset.first,
                                      currentPos.y + target.offset.second};
            item.set<GamePosition>(pos);
            item.set<ScreenPosition>(map->GameCoordsToScreenCoords(pos.x, pos.y));

            placedDetail += "- " + Offset(target.offset.first, target.offset.second) + " " +
                            target.ground + "\n";
            ++placedCount;
          }

          if (malformed > 0) {
            rejected.push_back(std::to_string(malformed) +
                               " coordinate(s) could not be read");
          }

          if (placedCount > 0) {
            successMsg = "System: Placed " + std::to_string(placedCount) + " of " +
                         std::to_string(requested.size()) + " " + itemName + ":\n" +
                         placedDetail;
          } else {
            successMsg = "System: Nothing was placed.\n";
          }
          if (!rejected.empty()) {
            successMsg += "System: Not placed:\n";
            for (const auto &reason : rejected) {
              successMsg += "- " + reason + "\n";
            }
          }
        }

        std::string itemName;
        std::string coordsStr;
        bool isFirstUpdate = true;
        std::string successMsg;
      };

