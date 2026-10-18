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

// A cell of a place worth walking to.
//
// For an area (a room, a market square) that is the nearest walkable member cell:
// the centre would do for a room but not for a ring-shaped place, and an extent is
// not guaranteed to be convex.
//
// For a **linear** place it is the *farthest* walkable member cell instead. A
// road is travelled along, not entered, and its nearest cell is usually the one
// under the character's feet -- so "nearest" makes [MOVE_TO the road] a no-op and
// the agent can never follow it. `linear` is the same shape test the brief in
// docs/agent-exploration-task.md names: a long bounding box with few claimed
// cells in it (a road, a wall of water), as opposed to a filled area.
inline GamePosition ApproachCell(const SpatialMemory &memory,
                                 const KnownPlace &place,
                                 GamePosition from) {
  const int longSide = std::max(place.width, place.height);
  const int shortSide = std::max(1, std::min(place.width, place.height));
  const long long area =
      static_cast<long long>(std::max(1, place.width)) *
      static_cast<long long>(std::max(1, place.height));
  // Two shapes of "a line rather than an area": a thin ribbon (a road running
  // north-south or east-west, whose bounding box is nearly all claimed cells) and
  // a sparse diagonal (whose box is mostly unclaimed). A filled square fails
  // both.
  const bool thin = longSide >= 3 * shortSide;
  const bool sparse =
      static_cast<long long>(place.memberCells.size()) * 2 < area;
  const bool linear = longSide >= 8 && (thin || sparse);

  GamePosition best = from;
  int bestDistance = -1;

  // A member cell with something standing on it is not a place to walk to. The
  // cell's `walkable` flag describes the ground only, so a stall on packed earth
  // reads as walkable and A* would refuse the goal -- which, for an action that
  // re-aims every frame, is a character that never arrives. Characters are not
  // obstacles (they move), so they are not skipped.
  std::unordered_set<std::uint64_t> occupied;
  for (const auto &[id, object] : memory.objects) {
    if (object.vanishedTick >= 0 || object.character || object.portable) {
      continue;
    }
    occupied.insert(VisionQuery::CellKey(object.pos));
  }

  for (const std::uint64_t key : place.memberCells) {
    const auto cell = memory.cells.find(key);
    if (cell == memory.cells.end() || !cell->second.walkable) {
      continue;
    }
    if (occupied.count(key) != 0) {
      continue;
    }
    const GamePosition pos = cell->second.pos;
    const int distance =
        std::max(std::abs(pos.x - from.x), std::abs(pos.y - from.y));
    // Nearest for an area, farthest for a line. Both are "the first one" until
    // bestDistance has been set, which is why it starts at -1.
    const bool better = linear ? (distance > bestDistance)
                               : (bestDistance < 0 || distance < bestDistance);
    if (better) {
      bestDistance = distance;
      best = pos;
    }
  }
  return best;
}

}  // namespace PlaceQuery
