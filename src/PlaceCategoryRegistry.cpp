#include "PlaceCategoryRegistry.h"

#include "DebugLog.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

void PlaceCategoryRegistry::Warn(const std::string &message) const {
  if (debugLog) {
    debugLog->LogWarning(message);
  } else {
    std::cerr << message << std::endl;
  }
}

void PlaceCategoryRegistry::Load(const std::string &filePath) {
  categories.clear();
  byId.clear();

  std::ifstream file(filePath);
  if (!file.is_open()) {
    Warn("Place categories: cannot open " + filePath +
         "; no places will form until it exists.");
    return;
  }

  nlohmann::json root;
  try {
    root = nlohmann::json::parse(file);
  } catch (const std::exception &error) {
    Warn("Place categories: " + filePath + " is not valid JSON: " + error.what());
    return;
  }

  if (!root.is_object()) {
    Warn("Place categories: " + filePath +
         " must be an object keyed by category id.");
    return;
  }

  for (auto it = root.begin(); it != root.end(); ++it) {
    // Keys starting with an underscore are the file's own commentary -- the same
    // convention settings.json uses for "_readme" -- not categories.
    if (!it.key().empty() && it.key()[0] == '_') {
      continue;
    }
    if (!it.value().is_object()) {
      Warn("Place categories: '" + it.key() +
           "' must be an object with formsPlace/minSeeds/radius.");
      continue;
    }

    PlaceCategory category;
    category.id = it.key();
    category.formsPlace = it.value().value("formsPlace", false);
    category.minSeeds = it.value().value("minSeeds", 1);
    category.radius = it.value().value("radius", 4);
    category.split = it.value().value("split", std::string());
    category.linksAcross = it.value().value("linksAcross", true);

    // A seed count of zero would make an empty component a place, so it is
    // clamped rather than trusted.
    if (category.minSeeds < 1) {
      category.minSeeds = 1;
    }
    if (category.radius < 1) {
      category.radius = 1;
    }

    byId[category.id] = categories.size();
    categories.push_back(std::move(category));
  }

  if (categories.empty()) {
    Warn("Place categories: " + filePath + " defines no categories.");
  }
}

const PlaceCategory *PlaceCategoryRegistry::Get(const std::string &id) const {
  const auto found = byId.find(id);
  if (found == byId.end()) {
    return nullptr;
  }
  return &categories[found->second];
}
