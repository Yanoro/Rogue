#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "RaceDef.hpp"

class DebugLog;

// Content database for races, loaded from data/races/<id>.json.
//
// Mirrors StatRegistry/SkillRegistry/ItemRegistry: definitions live here, an
// entity carries only the race id it was spawned as. A character with no race
// and no authored slots falls back to the "human" race, and then to the
// code-level humanoid body, so a map that predates races keeps working.
class RaceRegistry {
public:
  void LoadRaces(const std::string& directoryPath);

  // nullptr for an unknown id. The pointer is stable for the registry's lifetime.
  const RaceDef* Get(const std::string& id) const;

  std::vector<std::string> GetIds() const;

  // Every definition, sorted by id. Used by the startup slot check, which asks
  // whether any playable body can accept an item's slot kind.
  std::vector<const RaceDef*> GetAll() const;

  size_t Count() const { return races.size(); }

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::unordered_map<std::string, RaceDef> races;
  DebugLog* debugLog = nullptr;
};
