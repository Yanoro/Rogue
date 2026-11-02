#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class DebugLog;

// One non-LLM combat tactic: how a brainless Aggressive NPC positions itself.
//
// CombatStrategy is deliberately narrower than "AI". The LLM brain owns goals,
// memory and conversation; this registry owns only the small C++ layer that
// decides where a body stands while it fights. Attack damage, interval and
// range stay on the NPC template/spawner for now; a future mage strategy can add
// a ranged skill reference without turning this record into a union of every
// possible behaviour.
struct CombatStrategyDef {
  std::string id;
  std::string name;
  std::string description;
  // "chase" today. The loader warns and falls back to chase for anything this
  // build does not implement, rather than silently pretending to kite.
  std::string behavior = "chase";
  // How close the body wants to be, in tiles. The existing melee controller
  // attacks at its own range; this is the stance it tries to hold.
  float preferredRange = 1.0f;
  // Seconds between A* recomputes while pursuing. Small enough to follow a
  // moving player, large enough not to path every frame.
  float repathSeconds = 0.5f;
};

// Combat-strategy catalogue, loaded from data/combat_strategies/<id>.json.
//
// Keyed by id and exposed through AddOrReplace/ToJson like the other registries,
// so it can be listed and (later) edited through the same Content Editor table.
class CombatStrategyRegistry {
public:
  void Load(const std::string &directoryPath);

  const CombatStrategyDef *Get(const std::string &id) const;
  std::vector<const CombatStrategyDef *> GetAll() const;
  size_t Count() const { return strategies.size(); }

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, CombatStrategyDef> strategies;
  DebugLog *debugLog = nullptr;
};
