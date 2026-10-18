#pragma once

// Reading the place list a character remembers
// (docs/place-derivation-design.md sections 6 and 9).
//
// Everything here is a query over `SpatialMemory`: places are the character's
// own, so a name only resolves if that character has seen the place. There is no
// map-wide list to fall back on -- that is the point of deleting `Location`.

#include "SpatialMemory.hpp"
#include "StringUtils.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace PlaceQuery {

// The place whose claimed ground covers a cell. The smallest extent wins, because
// a named yard inside a bigger claimed area is the more specific answer.
inline const KnownPlace *At(const SpatialMemory &memory, GamePosition cell) {
  const std::uint64_t key = VisionQuery::CellKey(cell);
  const KnownPlace *best = nullptr;
  for (const KnownPlace &place : memory.places) {
    if (std::find(place.memberCells.begin(), place.memberCells.end(), key) ==
        place.memberCells.end()) {
      continue;
    }
    if (best == nullptr || place.memberCells.size() < best->memberCells.size()) {
      best = &place;
    }
  }
  return best;
}

inline const KnownPlace *ById(const SpatialMemory &memory, int id) {
  for (const KnownPlace &place : memory.places) {
    if (place.id == id) {
      return &place;
    }
  }
  return nullptr;
}

// A place by any of its names, case-insensitively. Aliases are equal citizens:
// "the orchard" finds a region called "The Orchard" and "Market Square".
inline const KnownPlace *ByLabel(const SpatialMemory &memory,
                                 const std::string &label) {
  for (const KnownPlace &place : memory.places) {
    for (const std::string &name : place.labels) {
      if (StringUtils::EqualsIgnoreCase(name, label)) {
        return &place;
      }
    }
  }
  return nullptr;
}

// How a place is written when the reader has no name for it: the strongest aspect
// it sees, which is what a person would say ("an unnamed orchard").
inline std::string DisplayName(const KnownPlace &place) {
  if (!place.labels.empty()) {
    return place.labels.front();
  }
  if (!place.primaryAspect.empty() && place.primaryAspect != "named") {
    return "an unnamed " + place.primaryAspect;
  }
  return "place #" + std::to_string(place.id);
}

inline std::string CompassWord(int dx, int dy) {
  // Screen convention: +x east, +y south.
  const int ax = std::abs(dx), ay = std::abs(dy);
  const bool east = dx > 0, south = dy > 0;
  if (ax > ay * 2) return east ? "east" : "west";
  if (ay > ax * 2) return south ? "south" : "north";
  if (east && south) return "south-east";
  if (east) return "north-east";
  if (south) return "south-west";
  return "north-west";
}

inline std::string DistancePhrase(GamePosition from, GamePosition to) {
  const int dx = to.x - from.x, dy = to.y - from.y;
  const int distance = std::max(std::abs(dx), std::abs(dy));
  if (distance == 0) {
    return "you are here";
  }
  if (distance == 1) {
    return "1 tile " + CompassWord(dx, dy);
  }
  return std::to_string(distance) + " tiles " + CompassWord(dx, dy);
}

// The strongest few aspects, for a one-line listing.
inline std::string AspectsPhrase(const KnownPlace &place, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < place.aspects.size() && i < count; ++i) {
    if (!out.empty()) {
      out += ", ";
    }
    out += place.aspects[i].category + " " +
           std::to_string(static_cast<int>(place.aspects[i].peak * 100.0f)) +
           "%";
  }
  return out;
}

// A cell of a place worth walking to: the member cell nearest `from` that the
// character has seen to be walkable. The centre would do for a room but not for a
// ring-shaped place, and an extent is not guaranteed to be convex.
inline GamePosition ApproachCell(const SpatialMemory &memory,
                                 const KnownPlace &place,
                                 GamePosition from) {
  GamePosition best = from;
  int bestDistance = -1;
  for (const std::uint64_t key : place.memberCells) {
    const auto cell = memory.cells.find(key);
    if (cell == memory.cells.end() || !cell->second.walkable) {
      continue;
    }
    const GamePosition pos = cell->second.pos;
    const int distance =
        std::max(std::abs(pos.x - from.x), std::abs(pos.y - from.y));
    if (bestDistance < 0 || distance < bestDistance) {
      bestDistance = distance;
      best = pos;
    }
  }
  return best;
}

}  // namespace PlaceQuery
