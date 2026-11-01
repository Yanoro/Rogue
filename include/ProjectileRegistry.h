#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class DebugLog;

// One flying object: what it looks like and how fast it crosses the world.
//
// Deliberately NOT an "effect". A projectile is a DELIVERY -- how an ability
// reaches its target -- and it knows nothing about damage, healing or conditions.
// The impact stays on the skill's activation (base damage today; an ImpactRegistry
// when a second kind of outcome exists). Keeping the two apart is what lets one
// arrow visual carry a poison payload and one fireball visual carry an area blast
// without either record growing a union of optional fields.
struct ProjectileDef {
  std::string id;
  // Explicit path or sprite convention id, resolved through the ordinary
  // art-pack -> generated -> data/sprites order.
  std::string sprite;
  // World pixels per second. A tile is 32 world pixels, so 420 is roughly
  // thirteen tiles a second.
  float speed = 420.0f;
  // Drawn size in world pixels, both directions.
  float size = 24.0f;
};

// Content database for projectiles, the counterpart of PortraitRegistry.
class ProjectileRegistry {
public:
  void LoadProjectiles(const std::string &directoryPath);

  const ProjectileDef *Get(const std::string &id) const;
  std::vector<std::string> GetIds() const;
  std::vector<const ProjectileDef *> GetAll() const;

  size_t Count() const { return projectiles.size(); }

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, ProjectileDef> projectiles;
  DebugLog *debugLog = nullptr;
};
