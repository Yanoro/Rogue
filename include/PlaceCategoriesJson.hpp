#pragma once

// Parsing {"orchard": 1.0, "poor": 0.4} into weighted seed categories.
//
// Shared by object templates and tile definitions so the two cannot disagree
// about what the key means -- and so a genre pack writes it the same way in both
// files. Unknown category ids are kept: the taxonomy loads separately, and
// derivation ignores an id it cannot resolve (the startup pass warns and names the
// template, which is where a typo becomes loud).

#include "Components.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

inline std::vector<SeedCategory>
ParsePlaceCategories(const nlohmann::json &value) {
  std::vector<SeedCategory> categories;
  if (!value.is_object()) {
    return categories;
  }
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (!it.value().is_number()) {
      continue;
    }
    SeedCategory category;
    category.category = it.key();
    // A weight is an intensity, so it lives in 0..1. Out-of-range is clamped
    // rather than refused: an author writing 2.0 means "very", not "a bug".
    category.weight = std::clamp(it.value().get<float>(), 0.0f, 1.0f);
    if (category.weight > 0.0f) {
      categories.push_back(std::move(category));
    }
  }
  return categories;
}
