#pragma once
#include <flecs.h>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include "Components.h"

#include <optional>

class Map;

class ObjectFactory {
public:
  void LoadTemplates(const std::string& directoryPath);
  flecs::entity SpawnObject(flecs::world& ecs, flecs::entity parent, Map* map, const std::string& type, std::optional<GamePosition> pos = std::nullopt);

private:
  std::unordered_map<std::string, nlohmann::json> templates;
};
