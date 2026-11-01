#include "ProjectileRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

#include "DebugLog.h"

void ProjectileRegistry::LoadProjectiles(const std::string &directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    const std::string errStr =
        "Warning: Projectile directory not found: " + directoryPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return;
  }

  auto warn = [this](const std::string &message) {
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };

  for (const auto &entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() != ".json") continue;

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    try {
      nlohmann::json j;
      file >> j;

      ProjectileDef def;
      def.id = entry.path().stem().string();
      def.sprite = j.value("sprite", "");
      def.speed = j.value("speed", 420.0f);
      def.size = j.value("size", 24.0f);

      if (def.sprite.empty()) {
        warn("Warning: Projectile '" + def.id +
             "' declares no 'sprite', so it is ignored.");
        continue;
      }
      if (def.speed <= 0.0f) {
        warn("Warning: Projectile '" + def.id +
             "' has speed <= 0; using 420 instead.");
        def.speed = 420.0f;
      }
      if (def.size <= 0.0f) {
        warn("Warning: Projectile '" + def.id +
             "' has size <= 0; using 24 instead.");
        def.size = 24.0f;
      }

      projectiles[def.id] = std::move(def);
    } catch (const std::exception &e) {
      warn("Failed to parse projectile " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
}

const ProjectileDef *ProjectileRegistry::Get(const std::string &id) const {
  auto it = projectiles.find(id);
  return it == projectiles.end() ? nullptr : &it->second;
}

std::vector<std::string> ProjectileRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(projectiles.size());
  for (const auto &pair : projectiles) {
    ids.push_back(pair.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<const ProjectileDef *> ProjectileRegistry::GetAll() const {
  std::vector<const ProjectileDef *> defs;
  for (const std::string &id : GetIds()) {
    defs.push_back(Get(id));
  }
  return defs;
}
