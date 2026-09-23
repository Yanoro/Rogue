#include "RecipeRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "DebugLog.h"
#include "TrainingParsing.h"

void RecipeRegistry::LoadRecipes(const std::string& directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    std::string errStr =
        "Warning: Recipe directory not found: " + directoryPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return;
  }

  auto warn = [this](const std::string& message) {
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };

  for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() != ".json") continue;

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    try {
      nlohmann::json j;
      file >> j;

      // Filename without extension is the id, exactly like ObjectFactory's
      // templates. Workstations and AI commands reference this id.
      CraftRecipe recipe;
      recipe.id = entry.path().stem().string();
      recipe.name = j.value("name", recipe.id);
      recipe.craftTimeSeconds = j.value("craftTime", 0.0f);

      if (j.contains("inputs")) {
        for (const auto& in : j["inputs"]) {
          ItemStack stack;
          stack.item = in.value("item", "");
          stack.count = in.value("count", 1);
          if (stack.item.empty() || stack.count <= 0) {
            warn("Warning: Recipe '" + recipe.id +
                 "' has an invalid input (empty item or non-positive count); "
                 "skipping that input.");
            continue;
          }
          recipe.inputs.push_back(stack);
        }
      }

      if (j.contains("outputs")) {
        for (const auto& out : j["outputs"]) {
          LootDrop drop;
          drop.itemType = out.value("item", "");
          drop.chance = out.value("chance", 1.0f);
          if (drop.itemType.empty()) {
            warn("Warning: Recipe '" + recipe.id +
                 "' has an output with no item; skipping that output.");
            continue;
          }
          recipe.outputs.push_back(drop);
        }
      }

      if (j.contains("trains")) {
        ParseTrainingGrants(j["trains"], "Recipe '" + recipe.id + "'",
                            recipe.trains, warn);
      }

      // A recipe that cannot be satisfied or cannot produce anything is content
      // authoring error, not a runtime state: report it loudly instead of
      // letting a station advertise a dead recipe.
      if (recipe.inputs.empty()) {
        warn("Warning: Recipe '" + recipe.id + "' has no inputs.");
      }
      if (recipe.outputs.empty()) {
        warn("Warning: Recipe '" + recipe.id + "' has no outputs.");
      }

      recipes[recipe.id] = recipe;
    } catch (const std::exception& e) {
      warn("Failed to parse recipe " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
}

const CraftRecipe* RecipeRegistry::Get(const std::string& id) const {
  auto it = recipes.find(id);
  if (it == recipes.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> RecipeRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(recipes.size());
  for (const auto& pair : recipes) {
    ids.push_back(pair.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}
