#pragma once

#include <cstddef>
#include <cmath>
#include <string>

#include "RecipeRegistry.h"

// Recipe rendering shared by the station interaction, the craft flow and the
// [GUIDE] / [INFO] lookups.
//
// These two helpers used to live in the anonymous namespace in GameECS.cpp. They
// moved here when the guide needed to describe a recipe from AgentActions.h,
// which cannot see a file-local namespace. Behaviour is unchanged: FormatRecipe
// is still the one place that decides how a recipe reads, so a station and a
// guide can never describe the same recipe differently.

// Durations are written into messages a player and a model read, where
// "3.000000 seconds" is noise. Whole seconds print bare, anything else keeps one
// decimal: 3 -> "3", 2.5 -> "2.5".
inline std::string FormatSeconds(float seconds) {
  std::string text = std::to_string(seconds);
  text.erase(text.find_last_not_of('0') + 1);
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  return text;
}

// One line per recipe, written with content ids rather than display names,
// because the id is the exact token [CRAFT ...] and the recipe files use.
inline std::string FormatRecipe(const CraftRecipe &recipe) {
  std::string line = recipe.id + " (";
  for (size_t i = 0; i < recipe.inputs.size(); ++i) {
    if (i > 0) line += " + ";
    line += std::to_string(recipe.inputs[i].count) + " " + recipe.inputs[i].item;
  }
  line += " -> ";
  for (size_t i = 0; i < recipe.outputs.size(); ++i) {
    if (i > 0) line += " + ";
    const LootDrop &output = recipe.outputs[i];
    line += output.itemType;
    if (output.chance < 1.0f) {
      line += " (" + std::to_string(std::lround(output.chance * 100.0f)) +
              "% chance)";
    }
  }
  line += ")";
  // Surface the wait, because it differs per recipe and the model needs it to
  // plan around being busy.
  if (recipe.craftTimeSeconds > 0.0f) {
    line += " takes " + FormatSeconds(recipe.craftTimeSeconds) + "s";
  }
  return line;
}
