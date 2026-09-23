#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "Components.h"
#include "SkillXp.hpp"

class DebugLog;

// A crafting recipe definition loaded from data/recipes/<id>.json. The file stem
// is the id (e.g. "flour"), and that id is what a Workstation stores in its
// `recipes` list.
struct CraftRecipe {
  std::string id;
  std::string name;              // player/AI-facing label, e.g. "Flour"
  std::vector<ItemStack> inputs; // consumed, all-or-nothing
  // Spawned on success. LootDrop is reused from harvest drops so byproducts can
  // carry a chance; a normal output uses chance 1.0.
  std::vector<LootDrop> outputs;
  float craftTimeSeconds = 0.0f; // 0 = instant; > 0 starts a timed CraftAction

  // Skills this recipe teaches, and by how much. The amount lives here rather
  // than on the skill because it is a fact about the work, and naming the skill
  // here is what stops a blacksmithing skill from gaining XP for baking bread --
  // bread.json names cooking and never mentions blacksmithing, so there is no
  // matching to get wrong.
  std::vector<SkillXp> trains;
};

// Content database for crafting, mirroring how ObjectFactory holds object
// templates: load once from a data directory, then look recipes up by id. The
// Workstation component stores ids, never definitions, so one recipe can be
// shared by any number of stations and can be enumerated for UIs and prompts.
class RecipeRegistry {
public:
  void LoadRecipes(const std::string& directoryPath);

  // Returns nullptr for an unknown id. The pointer is stable for the registry's
  // lifetime; do not cache it across a reload.
  const CraftRecipe* Get(const std::string& id) const;

  std::vector<std::string> GetIds() const;

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::unordered_map<std::string, CraftRecipe> recipes;
  DebugLog* debugLog = nullptr;
};
