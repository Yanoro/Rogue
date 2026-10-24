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
#include <cstdlib>
#include <string>
#include <vector>

namespace PlaceQuery {

// Every remembered place: semantic places first, then base regions. The two
// layers are stored separately because `DerivePlaces` rebuilds the semantic list;
// queries that mean "anything the character knows as a place" use this view.
inline std::vector<const KnownPlace *> AllPlaces(const SpatialMemory &memory) {
  std::vector<const KnownPlace *> all;
  all.reserve(memory.places.size() + memory.regions.size());
  for (const KnownPlace &place : memory.places) {
    all.push_back(&place);
  }
  for (const KnownPlace &region : memory.regions) {
    all.push_back(&region);
  }
  return all;
}

inline const KnownPlace *At(const SpatialMemory &memory, GamePosition cell);
inline const KnownPlace *RegionAt(const SpatialMemory &memory, GamePosition cell);

// The model-facing view: all semantic places, the current region, and the nearest
// `maxNearRegions` base regions. A coarser parent is omitted when a finer selected
// region is already contained in it, so the capture does not show R16 and its four
// R8 children as five competing places. The current region is always included.
inline std::vector<const KnownPlace *> ModelPlaces(
    const SpatialMemory &memory, GamePosition here,
    int maxNearRegions = DEFAULT_REGION_MODEL_NEAR_COUNT) {
  std::vector<const KnownPlace *> out;
  out.reserve(memory.places.size() + 1 +
              static_cast<std::size_t>(std::max(0, maxNearRegions)));
  for (const KnownPlace &place : memory.places) {
    out.push_back(&place);
  }

  const KnownPlace *currentRegion = RegionAt(memory, here);
  std::vector<const KnownPlace *> selected;
  const auto addRegion = [&out, &selected](const KnownPlace *region) {
    if (region == nullptr) return;
    out.push_back(region);
    selected.push_back(region);
  };
  addRegion(currentRegion);

  const auto contained = [](const KnownPlace *child,
                            const KnownPlace *parent) {
    if (child == parent || child->regionLevel < 0 ||
        parent->regionLevel < 0 ||
        child->regionLevel >= parent->regionLevel) {
      return false;
    }
    for (const std::uint64_t key : child->memberCells) {
      if (std::find(parent->memberCells.begin(), parent->memberCells.end(),
                    key) == parent->memberCells.end()) {
        return false;
      }
    }
    return true;
  };

  std::vector<const KnownPlace *> regions;
  regions.reserve(memory.regions.size());
  for (const KnownPlace &region : memory.regions) {
    if (&region == currentRegion) continue;
    regions.push_back(&region);
  }
  std::stable_sort(regions.begin(), regions.end(),
                   [&](const KnownPlace *a, const KnownPlace *b) {
                     const int da = std::max(
                         std::abs(a->anchor.x + a->width / 2 - here.x),
                         std::abs(a->anchor.y + a->height / 2 - here.y));
                     const int db = std::max(
                         std::abs(b->anchor.x + b->width / 2 - here.x),
                         std::abs(b->anchor.y + b->height / 2 - here.y));
                     return da < db;
                   });

  const int limit = std::max(0, maxNearRegions);
  for (const KnownPlace *region : regions) {
    if (static_cast<int>(selected.size()) >= limit) break;
    bool covered = false;
    for (const KnownPlace *chosen : selected) {
      if (contained(chosen, region)) {
        covered = true;
        break;
      }
    }
    if (covered) continue;
    addRegion(region);
  }
  return out;
}

// The place whose claimed ground covers a cell. The smallest extent wins, because
// a named yard inside a bigger claimed area is the more specific answer.
inline const KnownPlace *At(const SpatialMemory &memory, GamePosition cell) {
  const std::uint64_t key = VisionQuery::CellKey(cell);
  // A semantic place underfoot wins even when it is larger than a base region:
  // inside a village, the village is the current place, not the 8x8 region that
  // happens to be smaller. The smallest semantic place wins among semantic
  // places; the region is the fallback when there is no semantic place at all.
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
  if (best != nullptr) {
    return best;
  }
  return RegionAt(memory, cell);
}

inline const KnownPlace *RegionAt(const SpatialMemory &memory,
                                  GamePosition cell) {
  const std::uint64_t key = VisionQuery::CellKey(cell);
  const KnownPlace *best = nullptr;
  for (const KnownPlace &region : memory.regions) {
    if (std::find(region.memberCells.begin(), region.memberCells.end(), key) ==
        region.memberCells.end()) {
      continue;
    }
    if (best == nullptr ||
        region.memberCells.size() < best->memberCells.size()) {
      best = &region;
    }
  }
  return best;
}

inline const KnownPlace *ById(const SpatialMemory &memory, int id) {
  for (const KnownPlace *place : AllPlaces(memory)) {
    if (place->id == id) {
      return place;
    }
  }
  return nullptr;
}

// A place by any of its names, case-insensitively. Aliases are equal citizens:
// "the orchard" finds a region called "The Orchard" and "Market Square".
inline const KnownPlace *ByLabel(const SpatialMemory &memory,
                                 const std::string &label) {
  for (const KnownPlace *place : AllPlaces(memory)) {
    for (const std::string &name : place->labels) {
      if (StringUtils::EqualsIgnoreCase(name, label)) {
        return place;
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

// Whether a place is a line rather than an area: a long bounding box with few
// claimed cells in it (a road, a wall of water), or a thin ribbon whose box is
// nearly all claimed. A filled square fails both. Used by ApproachCell and by
// navigation's mode vocabulary (follow vs cross).
inline bool IsLinearPlace(const KnownPlace &place) {
  const int longSide = std::max(place.width, place.height);
  const int shortSide = std::max(1, std::min(place.width, place.height));
  const long long area =
      static_cast<long long>(std::max(1, place.width)) *
      static_cast<long long>(std::max(1, place.height));
  const bool thin = longSide >= 3 * shortSide;
  const bool sparse =
      static_cast<long long>(place.memberCells.size()) * 2 < area;
  return longSide >= 8 && (thin || sparse);
}

// One walkable member cell, nearest or farthest from `from`. Empty ground, an
// occupied cell and a non-walkable cell are not targets; `from` is returned when
// there is nothing to walk to (the caller treats that as "no move").
//
// A member cell with something standing on it is not a place to walk to. The
// cell's `walkable` flag describes the ground only, so a stall on packed earth
// reads as walkable and A* would refuse the goal -- which, for an action that
// re-aims every frame, is a character that never arrives. Characters are not
// obstacles (they move), so they are not skipped.
inline GamePosition WalkableMemberCell(const SpatialMemory &memory,
                                       const KnownPlace &place,
                                       GamePosition from, bool farthest) {
  GamePosition best = from;
  int bestDistance = -1;

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
    const bool better = farthest
                            ? (distance > bestDistance)
                            : (bestDistance < 0 || distance < bestDistance);
    if (better) {
      bestDistance = distance;
      best = pos;
    }
  }
  return best;
}

inline GamePosition NearestWalkableCell(const SpatialMemory &memory,
                                        const KnownPlace &place,
                                        GamePosition from) {
  return WalkableMemberCell(memory, place, from, false);
}

// A cell of a place worth walking to for the direct [MOVE_TO] verb.
//
// For an area (a room, a market square) that is the nearest walkable member cell:
// the centre would do for a room but not for a ring-shaped place, and an extent is
// not guaranteed to be convex.
//
// For a **linear** place it is the *farthest* walkable member cell instead. A
// road is travelled along, not entered, and its nearest cell is usually the one
// under the character's feet -- so "nearest" makes [MOVE_TO the road] a no-op and
// the agent can never follow it. Navigation instead walks to the nearest cell
// (NearestWalkableCell) and then offers the local compass once inside; this
// heuristic remains the default for the direct [MOVE_TO] verb.
inline GamePosition ApproachCell(const SpatialMemory &memory,
                                 const KnownPlace &place,
                                 GamePosition from) {
  return WalkableMemberCell(memory, place, from, IsLinearPlace(place));
}

// One row of the place list, as `[PLACES]` sorts and prints it.
//
// `SortedPlaces` is the one ordering and `FormatPlaceLine` the one line format,
// so the plain place list and the navigation summary cannot disagree about a
// name, an aspect score or a distance. The sort key is the Chebyshev distance to
// the place's centre, which is what the list has always used: a large place's
// edge can be closer than a small place's centre, and changing that would move
// the rows the agent already learned to expect.
struct PlaceLine {
  const KnownPlace *place = nullptr;
  GamePosition centre{0, 0};
  int distance = 0;
};

inline std::vector<PlaceLine> SortedPlaces(const SpatialMemory &memory,
                                           GamePosition here) {
  std::vector<PlaceLine> lines;
  lines.reserve(memory.places.size() + memory.regions.size());
  for (const KnownPlace *place : AllPlaces(memory)) {
    PlaceLine line;
    line.place = place;
    line.centre = {place->anchor.x + place->width / 2,
                   place->anchor.y + place->height / 2};
    line.distance = std::max(std::abs(line.centre.x - here.x),
                             std::abs(line.centre.y - here.y));
    lines.push_back(line);
  }
  // Stable, so two places the same distance away keep the id order the memory
  // already stores; an unstable sort made the list depend on initial capacity.
  std::stable_sort(lines.begin(), lines.end(),
                   [](const PlaceLine &a, const PlaceLine &b) {
                     return a.distance < b.distance;
                   });
  return lines;
}

// "#12 Village -- market 100% -- 4 tiles north-east", without the leading
// indent. Callers that print a list add their own indentation.
inline std::string FormatPlaceLine(const PlaceLine &line, GamePosition here) {
  const KnownPlace &place = *line.place;
  std::string out = "#" + std::to_string(place.id) + " " + DisplayName(place);
  const std::string aspects = AspectsPhrase(place, 3);
  if (!aspects.empty()) {
    out += " -- " + aspects;
  }
  out += " -- " + DistancePhrase(here, line.centre);
  return out;
}

}  // namespace PlaceQuery
