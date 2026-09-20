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
  flecs::entity SpawnObject(flecs::world& ecs, flecs::entity parent, Map* map, const std::string& type, std::optional<GamePosition> pos = std::nullopt);
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
