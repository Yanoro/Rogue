#pragma once

// Turning what a character has just seen into prose
// (docs/spatial-memory-design.md sections 7.2 and 12; docs/agent-exploration-task.md gap 5).
//
// A pure function of two things: the cells the vision sweep produced, and the
// character's own memory. It never looks at the map, so it cannot describe ground
// the character did not observe, a thing it did not see, or a place it has not
// derived. The rule from docs/task-system-design.md section 6 holds: the engine
// reports what is there, the agent judges what to do about it. There are no
// directions to take, no importance, and no map-wide knowledge.
//
// `[LOOK]` renders through here once the sampler has taken the sweep -- a single
// direction describes the sectors the cone covers, and `[LOOK around]` describes
// all eight because "where am I" is a question about direction, and one paragraph
// hides the one thing the agent needs: which way the road goes.
//
// Facts are printed in the vocabulary the record already uses: raw tile surfaces,
// object display names, and place names/aspects. Nothing here invents a category.

#include "Components.h"
#include "PlaceQuery.hpp"
#include "VisionQuery.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace SightReport {

namespace detail {

struct Thing {
  std::string name;
  int distance = 0;
};

// What one compass sector holds. A map because the sectors are looked up by the
// word PlaceQuery::CompassWord produces, which the renderer prints in a fixed
// order.
struct Sector {
  int cells = 0;
  std::map<std::string, int> surfaces;
  // The nearest surface that belongs to a place category (a path, water, tilled
  // soil), i.e. the one worth naming beyond "mostly grass".
  std::string notableSurface;
  int notableDistance = -1;
  std::vector<Thing> things;  // nearest first
};

inline int Chebyshev(const GamePosition &a, const GamePosition &b) {
  return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

inline std::string DistancePhrase(int distance) {
  if (distance <= 0) return "here";
  if (distance == 1) return "next to you";
  return std::to_string(distance) + " tiles away";
}

// Man-made surfaces take an article so a line reads as a sentence; natural mass
// nouns ("grass", "water") do not. This is display only -- the name itself is
// always the tile's own.
inline std::string SurfacePhrase(const std::string &surface) {
  if (surface.empty()) return "ground";
  if (surface == "path" || surface == "floor" || surface == "packed earth" ||
      surface == "wooden floor" || surface == "stone floor") {
    return "a " + surface;
  }
  return surface;
}

// The eight words in the order the survey prints them, so two runs over the same
// memory produce the same text.
inline const std::vector<std::string> &SectorOrder() {
  static const std::vector<std::string> order = {
      "north",      "north-east", "east",      "south-east",
      "south",      "south-west", "west",      "north-west"};
  return order;
}

}  // namespace detail

// The facing word for "you end facing ...". A normalized vector turned into the
// same eight sectors the report uses. Empty for a zero vector.
inline std::string FacingWord(const raylib::Vector2 &facing) {
  const int dx = facing.x > 0.35f ? 1 : (facing.x < -0.35f ? -1 : 0);
  const int dy = facing.y > 0.35f ? 1 : (facing.y < -0.35f ? -1 : 0);
  if (dx == 0 && dy == 0) return std::string();
  return PlaceQuery::CompassWord(dx, dy);
}

// `visible` is `Vision::lastVisible` (the cached sweep, never a fresh one),
// `visibleTick` is the tick that sweep belongs to (for the "seen just now"
// object test), and `fullCircle` selects the eight-sector survey. `heading` is
// the direction word of a directional look.
inline std::string Render(const SpatialMemory &memory,
                          const std::vector<GamePosition> &visible,
                          GamePosition here, int visibleTick,
                          const std::string &facingWord, bool fullCircle,
                          const std::string &heading) {
  std::unordered_set<std::uint64_t> visibleKeys;
  visibleKeys.reserve(visible.size() * 2);
  for (const GamePosition &cell : visible) {
    visibleKeys.insert(VisionQuery::CellKey(cell));
  }

  // Ground: one pass over the sweep, bucketed by compass sector.
  std::map<std::string, detail::Sector> sectors;
  for (const GamePosition &cell : visible) {
    const int dx = cell.x - here.x;
    const int dy = cell.y - here.y;
    if (dx == 0 && dy == 0) continue;

    const auto known = memory.cells.find(VisionQuery::CellKey(cell));
    if (known == memory.cells.end()) continue;
    const MemoryCell &record = known->second;

    detail::Sector &sector = sectors[PlaceQuery::CompassWord(dx, dy)];
    ++sector.cells;
    if (!record.surface.empty()) {
      ++sector.surfaces[record.surface];
    }
    if (!record.categories.empty()) {
      const int distance = std::max(std::abs(dx), std::abs(dy));
      if (sector.notableDistance < 0 || distance < sector.notableDistance) {
        sector.notableDistance = distance;
        sector.notableSurface = record.surface;
      }
    }
  }

  // Things: `lastSeenTick == visibleTick` is the exact "seen in this sweep" test,
  // so a character that has walked out of sight, or a tree that was felled, is
  // not reported just because its last-known cell happens to be visible.
  for (const auto &entry : memory.objects) {
    const SeenObject &object = entry.second;
    if (object.vanishedTick >= 0 || object.lastSeenTick != visibleTick) continue;
    const int dx = object.pos.x - here.x;
    const int dy = object.pos.y - here.y;
    if (dx == 0 && dy == 0) continue;

    const std::string word = PlaceQuery::CompassWord(dx, dy);
    const auto found = sectors.find(word);
    if (found == sectors.end()) continue;  // not in the sweep

    detail::Thing thing;
    thing.name = !object.name.empty() ? object.name : object.templateId;
    if (thing.name.empty()) thing.name = "something";
    thing.distance = std::max(std::abs(dx), std::abs(dy));
    found->second.things.push_back(std::move(thing));
  }
  for (auto &entry : sectors) {
    detail::Sector &sector = entry.second;
    std::stable_sort(
        sector.things.begin(), sector.things.end(),
        [](const detail::Thing &a, const detail::Thing &b) {
          return a.distance < b.distance;
        });
  }

  // Places with at least one claimed cell in the sweep, nearest visible cell
  // first. A place is not an entity; membership is the only rule that can answer
  // "is part of it in sight".
  struct Landmark {
    const KnownPlace *place = nullptr;
    GamePosition at{0, 0};
    int distance = 0;
  };
  std::vector<Landmark> landmarks;
  for (const KnownPlace *place : PlaceQuery::ModelPlaces(memory, here)) {
    Landmark landmark;
    landmark.place = place;
    landmark.distance = -1;
    for (const std::uint64_t key : place->memberCells) {
      if (visibleKeys.count(key) == 0) continue;
      const auto cell = memory.cells.find(key);
      if (cell == memory.cells.end()) continue;
      const int distance = detail::Chebyshev(cell->second.pos, here);
      if (landmark.distance < 0 || distance < landmark.distance) {
        landmark.distance = distance;
        landmark.at = cell->second.pos;
      }
    }
    if (landmark.distance >= 0) landmarks.push_back(std::move(landmark));
  }
  std::stable_sort(landmarks.begin(), landmarks.end(),
                   [](const Landmark &a, const Landmark &b) {
                     return a.distance < b.distance;
                   });

  const KnownPlace *herePlace = PlaceQuery::At(memory, here);

  // ---- compose ----------------------------------------------------------
  std::string out;
  if (fullCircle) {
    out = "System: You turn a full circle";
    if (!facingWord.empty()) out += " and end facing " + facingWord;
    out += ".\n";
  } else {
    out = "System: You turn to face " + heading + ".\n";
  }

  if (herePlace != nullptr) {
    out += "You are standing in " + PlaceQuery::DisplayName(*herePlace);
    const std::string aspects = PlaceQuery::AspectsPhrase(*herePlace, 2);
    if (!aspects.empty()) out += " (" + aspects + ")";
    out += ".\n";
  }

  out += fullCircle ? "Around you:\n" : "In sight:\n";

  bool headingPrinted = false;
  for (const std::string &word : detail::SectorOrder()) {
    const auto found = sectors.find(word);
    if (found == sectors.end() || found->second.cells == 0) {
      if (fullCircle) out += "  " + word + ": nothing in sight.\n";
      continue;
    }

    const detail::Sector &sector = found->second;
    std::string dominant;
    int dominantCount = -1;
    for (const auto &[surface, count] : sector.surfaces) {
      if (count > dominantCount) {
        dominantCount = count;
        dominant = surface;
      }
    }

    // A directional look describes what is *ahead*, not the two-tile radar ring
    // that happens to contain grass in every direction. A sector is worth a line
    // when it is the direction looked at, holds something, or is not plain ground;
    // that keeps a peripheral road or tree visible without turning a glance into
    // a survey. The survey prints every sector.
    if (!fullCircle) {
      const bool worthPrinting =
          word == heading || !sector.things.empty() ||
          !sector.notableSurface.empty() || dominant != "grass";
      if (!worthPrinting) continue;
      if (word == heading) headingPrinted = true;
    }

    std::string line = dominant.empty() ? std::string()
                                        : detail::SurfacePhrase(dominant);

    if (!sector.notableSurface.empty() && sector.notableSurface != dominant) {
      if (!line.empty()) line += " and ";
      line += detail::SurfacePhrase(sector.notableSurface);
      if (sector.notableDistance > 0) {
        line += " " + detail::DistancePhrase(sector.notableDistance);
      }
    }

    std::vector<std::string> listed;
    for (const detail::Thing &thing : sector.things) {
      if (listed.size() >= 2) break;
      if (std::find(listed.begin(), listed.end(), thing.name) != listed.end()) {
        continue;
      }
      listed.push_back(thing.name);
      const std::string phrase =
          thing.name + " " + detail::DistancePhrase(thing.distance);
      line += line.empty() ? phrase : "; " + phrase;
    }

    if (line.empty()) line = "nothing in sight";
    out += "  " + word + ": " + line + ".\n";
  }

  if (!fullCircle && !headingPrinted) {
    // The direction looked at may be entirely walled off; say so rather than
    // returning a report that never mentions the direction that was asked about.
    out += "  " + heading + ": nothing in sight.\n";
  }

  // Landmarks: the places in sight, minus whichever one the character stands in.
  // A larger place that also covers this cell has a claimed cell here, so its
  // nearest visible cell is 0 and it is excluded by the same test -- "the village
  // you are standing in" is not a landmark.
  std::string landmarkLine;
  int landmarkCount = 0;
  for (const Landmark &landmark : landmarks) {
    if (landmark.at == here) continue;
    if (landmarkCount >= 3) break;
    if (!landmarkLine.empty()) landmarkLine += "; ";
    landmarkLine += PlaceQuery::DisplayName(*landmark.place) + " " +
                    PlaceQuery::DistancePhrase(here, landmark.at);
    ++landmarkCount;
  }
  if (!landmarkLine.empty()) {
    out += "Landmarks: " + landmarkLine + ".\n";
  }

  return out;
}

}  // namespace SightReport
