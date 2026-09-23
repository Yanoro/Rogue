#pragma once
#include <flecs.h>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include "Components.h"

#include <optional>

class Map;
class DebugLog;

class ObjectFactory {
public:
  ObjectFactory();
  void LoadTemplates(const std::string& directoryPath);

  // `authored` marks the object as map content (see MapAuthored): the map loader
  // and the map editor pass true, while any runtime spawn (harvest drop, craft
  // output, NPC starting inventory) leaves it false so the map writer ignores it.
  flecs::entity SpawnObject(flecs::world& ecs, flecs::entity parent, Map* map,
                            const std::string& type,
                            std::optional<GamePosition> pos = std::nullopt,
                            bool authored = false);

  // Turns a list of ItemStacks into real entities held by `holder`: one entity
  // per unit, spawned as a child of `holder` and attached with the Holds
  // relationship. That is the same shape harvest drops and crafting outputs use,
  // so [INVENTORY], [TAKE], [STORE] and [CRAFT] see these items with no special
  // casing. Shared by map-defined NPC inventories and map-defined object
  // (container) inventories. An unknown template id is reported by SpawnObject
  // and skipped.
  void SpawnInventory(flecs::world& ecs, flecs::entity holder,
                      const std::vector<ItemStack>& inventory);

  bool ApplyTemplate(flecs::entity obj, const std::string& type, Map* map = nullptr);
  void SetDebugLog(DebugLog* log) { debugLog = log; }
  const std::unordered_map<std::string, nlohmann::json>& GetTemplates() const { return templates; }

  void RegisterDefaultComponents();

  template<typename T>
  void RegisterComponent(const std::string& name, std::function<void(flecs::entity, const nlohmann::json&)> parser) {
    componentSetters[name] = parser;
    componentRemovers[name] = [](flecs::entity e) { e.remove<T>(); };
  }

private:
  std::unordered_map<std::string, nlohmann::json> templates;
  DebugLog* debugLog = nullptr;

  std::unordered_map<std::string, std::function<void(flecs::entity, const nlohmann::json&)>> componentSetters;
  std::unordered_map<std::string, std::function<void(flecs::entity)>> componentRemovers;
};
