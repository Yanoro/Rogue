#pragma once

#include <string>
#include <vector>

// One named statistic on a character. The id indexes StatRegistry; the value is
// the entity's own, before any comparison against the registry baseline.
struct StatValue {
  std::string id;
  float value = 0.0f;
};

// The stats a character actually has.
//
// Deliberately a vector rather than a map: std_vector_support in Components.h
// already provides the reflection helper flecs serialization needs, and a
// character has a handful of stats, so linear lookup is not worth a container
// with worse reflection support. The same shape as ItemStack.
//
// A character lists ONLY the stats its kind has. A stat that is absent is not an
// error and needs no special-casing anywhere: resolution falls back to the
// registry baseline, which by definition produces a zero effect. That is what
// lets a future race have a different stat set with no engine changes.
// See docs/stat-effects-design.md sections 3.4 and 6.2.
//
// Kept free of flecs and raylib, like ItemStack.hpp, so the resolution logic and
// its test stay independent of the game's rendering stack.
struct StatBlock {
  std::vector<StatValue> values;
};
