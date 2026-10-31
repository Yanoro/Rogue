#pragma once

// The agent-facing chunk view (docs/chunk-design.md):
//
//   * BuildChunkKnowledge / RenderAreaCard -- the current/stopped area card,
//     built from the agent's SpatialMemory only. Observed count, no denominator.
//   * BuildLegTrace / LegTrace::ToProse -- the last leg's route strip, built from
//     the actual walked path plus the memory before/after.
//   * Journey / RenderJourney -- the cumulative chunk route of the episode.
//
// Nothing here reads the map, the map dimensions, or the world contents layer.
// The per-agent handle (area-N) is the only chunk name the model sees; the world
// key (cx,cy) and the debug ID stay engine-side.

#include "Chunk.hpp"
#include "Defaults.h"
#include "PlaceQuery.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ChunkViewSettings {
  bool enabled = true;
  int size = DEFAULT_CHUNK_SIZE;
  int maxTraversedAreas = DEFAULT_CHUNK_MAX_TRAVERSED_AREAS;
  int maxJourneySegments = DEFAULT_CHUNK_MAX_JOURNEY_SEGMENTS;
  int maxNewAreas = DEFAULT_CHUNK_MAX_NEW_AREAS;
  int maxFeaturesPerArea = DEFAULT_CHUNK_MAX_FEATURES_PER_AREA;
  int cardAspects = DEFAULT_CHUNK_CARD_ASPECTS;
  int cardObjectGroups = DEFAULT_CHUNK_CARD_OBJECT_GROUPS;
  int cardPlaces = DEFAULT_CHUNK_CARD_PLACES;
  bool includeCharacters = DEFAULT_CHUNK_INCLUDE_CHARACTERS;
};

enum class ChunkCrossingState { Observed, Blocked, Unknown };

struct ChunkCrossing {
  int dx = 0;
  int dy = 0;
  ChunkCrossingState state = ChunkCrossingState::Unknown;
  std::string surface;
  int crossingPairs = 0;
  int neighbourHandle = 0;
};

struct ChunkObjectGroup {
  std::string key;
  int count = 0;
};

struct ChunkCharacter {
  std::string name;
  int distance = 0;
  std::string direction;
};

struct ChunkKnowledge {
  ChunkCoord coord;
  int handle = 0;
  int observedCells = 0;
  int firstSeenTick = 0;
  int lastSeenTick = 0;
  std::string description;
  std::vector<std::pair<std::string, int>> surfaces;
  std::vector<const KnownPlace *> places;
  std::vector<ChunkObjectGroup> objectGroups;
  std::vector<ChunkCharacter> characters;
  int characterCount = 0;
  std::array<ChunkCrossing, 4> crossings{};  // N, E, S, W
};

namespace chunk_view_detail {

inline int Chebyshev(GamePosition a, GamePosition b) {
  return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

inline std::string SideOf(GamePosition from, GamePosition to) {
  return PlaceQuery::CompassWord(to.x - from.x, to.y - from.y);
}

inline bool Intersects(const KnownPlace &place,
                       const std::unordered_set<std::uint64_t> &cellKeys) {
  for (const std::uint64_t key : place.memberCells) {
    if (cellKeys.count(key) != 0) {
      return true;
    }
  }
  return false;
}

inline void AddFeature(std::vector<std::string> &order,
                       std::unordered_set<std::string> &seen,
                       const std::string &text, int cap) {
  if (static_cast<int>(order.size()) >= cap) {
    return;
  }
  if (!seen.insert(text).second) {
    return;
  }
  order.push_back(text);
}

inline std::string TopSurface(const std::map<std::string, int> &surfaces) {
  std::string top;
  int best = 0;
  for (const auto &[surface, count] : surfaces) {
    if (count > best) {
      best = count;
      top = surface;
    }
  }
  return top;
}

inline std::string SummaryFrom(const std::string &place,
                               const std::map<std::string, int> &surfaces) {
  if (!place.empty()) {
    return place;
  }
  const std::string top = TopSurface(surfaces);
  if (top.empty()) {
    return "open ground";
  }
  return top + " ground";
}

}  // namespace chunk_view_detail

inline ChunkKnowledge BuildChunkKnowledge(SpatialMemory &memory,
                                          ChunkCoord chunk,
                                          const ChunkViewSettings &settings,
                                          GamePosition here) {
  ChunkKnowledge out;
  out.coord = chunk;
  out.handle = EnsureChunkHandle(memory, chunk);

  std::unordered_set<std::uint64_t> chunkCellKeys;
  std::map<std::string, int> surfaces;
  bool firstTick = false;
  for (const auto &[key, cell] : memory.cells) {
    if (!ChunkContains(chunk, cell.pos, settings.size)) {
      continue;
    }
    ++out.observedCells;
    chunkCellKeys.insert(key);
    ++surfaces[cell.surface];
    if (!firstTick || cell.observedTick < out.firstSeenTick) {
      out.firstSeenTick = cell.observedTick;
    }
    firstTick = true;
    out.lastSeenTick = std::max(out.lastSeenTick, cell.observedTick);
  }

  for (const auto &[surface, count] : surfaces) {
    (void)count;
    out.surfaces.emplace_back(surface, count);
  }
  std::sort(out.surfaces.begin(), out.surfaces.end(),
            [](const auto &a, const auto &b) { return a.second > b.second; });
  if (static_cast<int>(out.surfaces.size()) > settings.cardAspects) {
    out.surfaces.resize(static_cast<std::size_t>(settings.cardAspects));
  }

  std::unordered_set<int> seenPlaces;
  const auto addPlace = [&](const KnownPlace &place) {
    if (static_cast<int>(out.places.size()) >= settings.cardPlaces) {
      return;
    }
    if (seenPlaces.insert(place.id).second) {
      out.places.push_back(&place);
    }
  };
  for (const KnownPlace &place : memory.places) {
    if (chunk_view_detail::Intersects(place, chunkCellKeys)) {
      addPlace(place);
    }
  }
  for (const KnownPlace &region : memory.regions) {
    if (chunk_view_detail::Intersects(region, chunkCellKeys)) {
      addPlace(region);
    }
  }

  std::unordered_map<std::string, int> groups;
  for (const auto &[id, object] : memory.objects) {
    (void)id;
    if (object.vanishedTick >= 0) {
      continue;
    }
    if (!ChunkContains(chunk, object.pos, settings.size)) {
      continue;
    }
    if (object.character) {
      ++out.characterCount;
      if (settings.includeCharacters) {
        const int distance =
            chunk_view_detail::Chebyshev(object.pos, here);
        if (out.characters.empty() ||
            distance < out.characters.front().distance) {
          ChunkCharacter entry;
          entry.name = object.name.empty() ? "a person" : object.name;
          entry.distance = distance;
          entry.direction =
              object.pos == here
                  ? "here"
                  : PlaceQuery::CompassWord(object.pos.x - here.x,
                                            object.pos.y - here.y);
          out.characters.clear();
          out.characters.push_back(std::move(entry));
        }
      }
      continue;
    }
    ++groups[ObjectContentKey(object)];
  }
  for (auto &[key, count] : groups) {
    out.objectGroups.push_back({key, count});
  }
  std::sort(out.objectGroups.begin(), out.objectGroups.end(),
            [](const ChunkObjectGroup &a, const ChunkObjectGroup &b) {
              if (a.count != b.count) return a.count > b.count;
              return a.key < b.key;
            });
  if (static_cast<int>(out.objectGroups.size()) > settings.cardObjectGroups) {
    out.objectGroups.resize(
        static_cast<std::size_t>(settings.cardObjectGroups));
  }

  const ChunkBounds bounds = ChunkBoundsOf(chunk, settings.size);
  static const int directions[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
  for (int i = 0; i < 4; ++i) {
    ChunkCrossing &crossing = out.crossings[static_cast<std::size_t>(i)];
    crossing.dx = directions[i][0];
    crossing.dy = directions[i][1];
    const ChunkCoord neighbour{chunk.cx + crossing.dx,
                               chunk.cy + crossing.dy};
    crossing.neighbourHandle = ChunkHandleFor(memory, neighbour);

    bool allObserved = true;
    int pairs = 0;
    std::map<std::string, int> pairSurfaces;
    const auto checkPair = [&](GamePosition inside, GamePosition outside) {
      const auto a = memory.cells.find(VisionQuery::CellKey(inside));
      const auto b = memory.cells.find(VisionQuery::CellKey(outside));
      if (a == memory.cells.end() || b == memory.cells.end()) {
        allObserved = false;
        return;
      }
      if (!a->second.walkable || !b->second.walkable) {
        return;
      }
      ++pairs;
      ++pairSurfaces[a->second.surface];
      ++pairSurfaces[b->second.surface];
    };
    if (crossing.dy < 0) {
      const int y = bounds.min.y;
      for (int x = bounds.min.x; x < bounds.maxExclusive.x; ++x) {
        checkPair({x, y}, {x, y - 1});
      }
    } else if (crossing.dx > 0) {
      const int x = bounds.maxExclusive.x - 1;
      for (int y = bounds.min.y; y < bounds.maxExclusive.y; ++y) {
        checkPair({x, y}, {x + 1, y});
      }
    } else if (crossing.dy > 0) {
      const int y = bounds.maxExclusive.y - 1;
      for (int x = bounds.min.x; x < bounds.maxExclusive.x; ++x) {
        checkPair({x, y}, {x, y + 1});
      }
    } else {
      const int x = bounds.min.x;
      for (int y = bounds.min.y; y < bounds.maxExclusive.y; ++y) {
        checkPair({x, y}, {x - 1, y});
      }
    }
    crossing.crossingPairs = pairs;
    if (pairs > 0) {
      crossing.state = ChunkCrossingState::Observed;
      crossing.surface = chunk_view_detail::TopSurface(pairSurfaces);
      // The crossing pair proves the neighbour chunk has an observed cell, so
      // it has (or can now get) a handle. Without this the card said "an
      // unexplored area" for a neighbour the agent had already seen.
      if (crossing.neighbourHandle == 0) {
        crossing.neighbourHandle = EnsureChunkHandle(memory, neighbour);
      }
    } else if (allObserved) {
      crossing.state = ChunkCrossingState::Blocked;
    } else {
      crossing.state = ChunkCrossingState::Unknown;
    }
  }

  if (!out.places.empty()) {
    out.description = PlaceQuery::DisplayName(*out.places.front());
  } else if (!out.surfaces.empty()) {
    out.description = out.surfaces.front().first + " ground";
  } else {
    out.description = "open ground";
  }
  return out;
}

inline std::string RenderAreaCard(const ChunkKnowledge &knowledge,
                                  const SpatialMemory &memory,
                                  const ChunkViewSettings &settings) {
  std::ostringstream out;
  out << "AREA " << ChunkDisplayLabel(memory, knowledge.handle);
  if (!knowledge.description.empty()) {
    out << " (" << knowledge.description << ")";
  }
  out << " -- " << knowledge.observedCells << " cells observed, last seen "
      << (knowledge.lastSeenTick == memory.lastVisionTick ? "now" : "earlier")
      << "\n";

  if (!knowledge.surfaces.empty()) {
    out << "  ground: ";
    for (std::size_t i = 0; i < knowledge.surfaces.size(); ++i) {
      if (i > 0) out << ", ";
      out << knowledge.surfaces[i].first;
    }
    out << "\n";
  }

  if (!knowledge.places.empty()) {
    out << "  places: ";
    for (std::size_t i = 0; i < knowledge.places.size(); ++i) {
      if (i > 0) out << "; ";
      out << PlaceQuery::DisplayName(*knowledge.places[i]);
      const std::string aspects =
          PlaceQuery::AspectsPhrase(*knowledge.places[i], 2);
      if (!aspects.empty()) out << " (" << aspects << ")";
    }
    out << "\n";
  }

  if (!knowledge.objectGroups.empty()) {
    out << "  objects: ";
    for (std::size_t i = 0; i < knowledge.objectGroups.size(); ++i) {
      if (i > 0) out << ", ";
      out << knowledge.objectGroups[i].count << " "
          << knowledge.objectGroups[i].key;
    }
    out << "\n";
  }

  if (settings.includeCharacters && knowledge.characterCount > 0) {
    out << "  characters: " << knowledge.characterCount;
    if (!knowledge.characters.empty()) {
      const ChunkCharacter &nearest = knowledge.characters.front();
      out << " (" << nearest.name;
      if (nearest.direction != "here") {
        out << ", " << nearest.distance << " tiles " << nearest.direction;
      }
      out << ")";
    }
    out << "\n";
  }

  static const char *kSides[4] = {"north", "east", "south", "west"};
  out << "  crossings: ";
  for (std::size_t i = 0; i < knowledge.crossings.size(); ++i) {
    if (i > 0) out << "; ";
    const ChunkCrossing &crossing = knowledge.crossings[i];
    out << kSides[i] << ": ";
    if (crossing.state == ChunkCrossingState::Observed) {
      if (crossing.neighbourHandle > 0) {
        out << ChunkDisplayLabel(memory, crossing.neighbourHandle);
      } else {
        out << "an unexplored area";
      }
      if (!crossing.surface.empty()) out << " (" << crossing.surface << ")";
    } else if (crossing.state == ChunkCrossingState::Blocked) {
      out << "no crossing observed";
    } else {
      out << "unknown";
    }
  }
  out << "\n";
  return out.str();
}

struct ChunkSegment {
  ChunkCoord coord;
  int handle = 0;
  std::string summary;
  std::string entrySide;
  std::string exitSide;
  int tiles = 0;
  bool knownBefore = false;
  bool firstObserved = false;
  std::vector<std::string> features;
};

struct LegTrace {
  std::string target;
  ChunkCoord startChunk;
  int tiles = 0;
  std::string outcome;
  std::vector<ChunkSegment> traversed;
  std::vector<int> newlyObserved;  // handles, off-path
  std::string ToProse(const SpatialMemory &memory,
                      const ChunkViewSettings &settings) const;
  nlohmann::json ToJson() const;
};

inline LegTrace BuildLegTrace(const std::vector<GamePosition> &path,
                              GamePosition start, SpatialMemory &memory,
                              const std::unordered_set<std::uint64_t> &chunksBefore,
                              int legStartTick, const std::string &target,
                              const std::string &outcome,
                              const ChunkViewSettings &settings) {
  LegTrace trace;
  trace.target = target;
  trace.startChunk = ChunkAt(start, settings.size);
  trace.tiles = static_cast<int>(path.size());
  trace.outcome = outcome;
  if (path.empty()) {
    return trace;
  }

  struct SegmentWork {
    ChunkSegment segment;
    std::vector<GamePosition> cells;
    std::map<std::string, int> surfaces;
    std::string place;
    std::vector<std::string> featureOrder;
    std::unordered_set<std::string> featureSet;
  };

  std::vector<SegmentWork> works;
  SegmentWork current;
  bool have = false;
  GamePosition previous = start;

  const auto finish = [&](SegmentWork &work, const std::string &exitSide) {
    work.segment.exitSide = exitSide;
    work.segment.summary =
        chunk_view_detail::SummaryFrom(work.place, work.surfaces);
    work.segment.features = work.featureOrder;
    works.push_back(std::move(work));
  };

  for (std::size_t i = 0; i < path.size(); ++i) {
    const GamePosition cell = path[i];
    const ChunkCoord coord = ChunkAt(cell, settings.size);
    if (!have || coord != current.segment.coord) {
      if (have) {
        finish(current,
               chunk_view_detail::SideOf(previous, cell));
      }
      current = SegmentWork{};
      current.segment.coord = coord;
      current.segment.handle = EnsureChunkHandle(memory, coord);
      current.segment.knownBefore =
          chunksBefore.count(ChunkKey(coord)) != 0;
      current.segment.firstObserved = !current.segment.knownBefore;
      current.segment.entrySide =
          (i == 0 && ChunkAt(start, settings.size) == coord)
              ? "started here"
              : chunk_view_detail::SideOf(previous, cell);
      have = true;
    }
    ++current.segment.tiles;
    current.cells.push_back(cell);

    const auto cellIt = memory.cells.find(VisionQuery::CellKey(cell));
    if (cellIt != memory.cells.end()) {
      ++current.surfaces[cellIt->second.surface];
    }
    if (current.place.empty()) {
      if (const KnownPlace *place = PlaceQuery::At(memory, cell)) {
        current.place = PlaceQuery::DisplayName(*place);
      }
    }

    const GamePosition prevCell = i == 0 ? start : path[i - 1];
    const GamePosition nextCell =
        i + 1 < path.size() ? path[i + 1] : cell;
    const KnownPlace *currentPlace = PlaceQuery::At(memory, cell);
    static const int directions[8][2] = {{0, -1},  {1, -1}, {1, 0},  {1, 1},
                                         {0, 1},   {-1, 1}, {-1, 0}, {-1, -1}};
    for (const auto &direction : directions) {
      const GamePosition neighbour{cell.x + direction[0],
                                   cell.y + direction[1]};
      if (neighbour == prevCell || neighbour == nextCell ||
          neighbour == cell) {
        continue;
      }
      const auto it = memory.cells.find(VisionQuery::CellKey(neighbour));
      if (it == memory.cells.end() || !it->second.walkable) {
        continue;
      }
      const KnownPlace *neighbourPlace = PlaceQuery::At(memory, neighbour);
      if (neighbourPlace == nullptr) {
        continue;
      }
      if (currentPlace != nullptr && currentPlace->id == neighbourPlace->id) {
        continue;
      }
      // A region boundary is just the ground continuing; only a semantic place
      // is a named side path/opening. Without this, crossing an open field
      // listed every adjacent field as a "side path".
      if (neighbourPlace->regionLevel >= 0) {
        continue;
      }
      const std::string directionWord =
          PlaceQuery::CompassWord(direction[0], direction[1]);
      const std::string feature =
          PlaceQuery::IsLinearPlace(*neighbourPlace)
              ? "a side path " + directionWord
              : "an opening " + directionWord + " (" +
                    PlaceQuery::DisplayName(*neighbourPlace) + ")";
      chunk_view_detail::AddFeature(current.featureOrder, current.featureSet,
                                    feature, settings.maxFeaturesPerArea);
    }
    previous = cell;
  }
  if (have) {
    finish(current, "stopped here");
  }

  std::unordered_set<int> traversedHandles;
  for (SegmentWork &work : works) {
    for (const auto &[id, object] : memory.objects) {
      (void)id;
      if (object.vanishedTick >= 0 || object.lastSeenTick < legStartTick) {
        continue;
      }
      bool near = false;
      for (const GamePosition &cell : work.cells) {
        if (chunk_view_detail::Chebyshev(object.pos, cell) <= 1) {
          near = true;
          break;
        }
      }
      if (!near) {
        continue;
      }
      if (object.character) {
        chunk_view_detail::AddFeature(
            work.featureOrder, work.featureSet,
            "a " + (object.name.empty() ? std::string("person") : object.name),
            settings.maxFeaturesPerArea);
      } else {
        chunk_view_detail::AddFeature(work.featureOrder, work.featureSet,
                                      "a " + ObjectContentKey(object),
                                      settings.maxFeaturesPerArea);
      }
    }
    work.segment.features = work.featureOrder;
  }
  for (const SegmentWork &work : works) {
    trace.traversed.push_back(work.segment);
    traversedHandles.insert(work.segment.handle);
  }

  std::unordered_set<std::uint64_t> traversedChunks;
  for (const ChunkSegment &segment : trace.traversed) {
    traversedChunks.insert(ChunkKey(segment.coord));
  }
  std::unordered_set<int> seenNew;
  for (const auto &[key, cell] : memory.cells) {
    (void)key;
    const ChunkCoord coord = ChunkAt(cell.pos, settings.size);
    const std::uint64_t chunkKey = ChunkKey(coord);
    if (chunksBefore.count(chunkKey) != 0 ||
        traversedChunks.count(chunkKey) != 0) {
      continue;
    }
    const int handle = EnsureChunkHandle(memory, coord);
    if (seenNew.insert(handle).second) {
      trace.newlyObserved.push_back(handle);
    }
  }
  return trace;
}

inline std::string LegTrace::ToProse(const SpatialMemory &memory,
                                     const ChunkViewSettings &settings) const {
  std::ostringstream out;
  out << outcome << "\n";
  int shown = 0;
  for (const ChunkSegment &segment : traversed) {
    if (shown >= settings.maxTraversedAreas) {
      out << "  ... " << (traversed.size() - static_cast<std::size_t>(shown))
          << " more areas\n";
      break;
    }
    ++shown;
    out << "  " << ChunkDisplayLabel(memory, segment.handle) << "  "
        << segment.summary;
    if (segment.entrySide != "started here" && !segment.entrySide.empty()) {
      out << "; entered " << segment.entrySide;
    }
    if (segment.exitSide == "stopped here") {
      out << "; stopped here";
    } else if (!segment.exitSide.empty()) {
      out << "; left " << segment.exitSide;
    }
    out << ". " << (segment.firstObserved ? "New." : "Known.");
    for (const std::string &feature : segment.features) {
      out << " " << feature << ".";
    }
    out << "\n";
  }
  if (!newlyObserved.empty()) {
    out << "NEWLY SEEN: ";
    const int limit = std::min<int>(
        static_cast<int>(newlyObserved.size()), settings.maxNewAreas);
    for (int i = 0; i < limit; ++i) {
      if (i > 0) out << ", ";
      out << ChunkDisplayLabel(memory, newlyObserved[static_cast<std::size_t>(i)]);
    }
    if (static_cast<int>(newlyObserved.size()) > limit) {
      out << ", and " << (newlyObserved.size() - static_cast<std::size_t>(limit))
          << " more";
    }
    out << " (off the path)\n";
  }
  return out.str();
}

inline nlohmann::json LegTrace::ToJson() const {
  nlohmann::json root;
  root["target"] = target;
  root["tiles"] = tiles;
  root["outcome"] = outcome;
  root["start"] = {{"cx", startChunk.cx}, {"cy", startChunk.cy}};
  root["traversed"] = nlohmann::json::array();
  for (const ChunkSegment &segment : traversed) {
    nlohmann::json entry;
    entry["chunk"] = {{"cx", segment.coord.cx}, {"cy", segment.coord.cy}};
    entry["handle"] = segment.handle;
    entry["summary"] = segment.summary;
    entry["entry"] = segment.entrySide;
    entry["exit"] = segment.exitSide;
    entry["tiles"] = segment.tiles;
    entry["knownBefore"] = segment.knownBefore;
    entry["firstObserved"] = segment.firstObserved;
    entry["features"] = segment.features;
    root["traversed"].push_back(std::move(entry));
  }
  root["newlyObserved"] = newlyObserved;
  return root;
}

struct JourneySegment {
  ChunkCoord from;
  ChunkCoord to;
  int fromHandle = 0;
  int toHandle = 0;
  std::string direction;
  int tiles = 0;
};

struct Journey {
  std::vector<JourneySegment> segments;
};

inline void AppendLeg(Journey &journey, const LegTrace &trace,
                      SpatialMemory &memory) {
  ChunkCoord previous = trace.startChunk;
  int previousHandle = EnsureChunkHandle(memory, previous);
  for (const ChunkSegment &segment : trace.traversed) {
    // A leg can stay inside one chunk; record it as a same-area stretch so the
    // journey never claims "nothing walked yet" after a real leg.
    JourneySegment entry;
    entry.from = previous;
    entry.to = segment.coord;
    entry.fromHandle = previousHandle;
    entry.toHandle = segment.handle;
    if (segment.coord != previous) {
      entry.direction = PlaceQuery::CompassWord(
          segment.coord.cx - previous.cx, segment.coord.cy - previous.cy);
    }
    entry.tiles = segment.tiles;
    journey.segments.push_back(std::move(entry));
    previous = segment.coord;
    previousHandle = segment.handle;
  }
}

inline std::string RenderJourney(const Journey &journey,
                                 const SpatialMemory &memory,
                                 const ChunkViewSettings &settings) {
  if (journey.segments.empty()) {
    return "nothing walked yet";
  }
  const int size = static_cast<int>(journey.segments.size());
  const int start = std::max(0, size - settings.maxJourneySegments);
  std::ostringstream out;
  if (start > 0) {
    int skippedTiles = 0;
    for (int i = 0; i < start; ++i) {
      skippedTiles += journey.segments[static_cast<std::size_t>(i)].tiles;
    }
    out << "earlier: " << start << " stretches, " << skippedTiles
        << " tiles; ";
  }
  for (int i = start; i < size; ++i) {
    if (i > start) out << "; ";
    const JourneySegment &segment =
        journey.segments[static_cast<std::size_t>(i)];
    const std::string fromLabel =
        ChunkDisplayLabel(memory, segment.fromHandle);
    const std::string toLabel =
        ChunkDisplayLabel(memory, segment.toHandle);
    if (segment.fromHandle == segment.toHandle) {
      out << fromLabel << " (" << segment.tiles
          << (segment.tiles == 1 ? " tile)" : " tiles)");
    } else {
      out << fromLabel << " -> " << toLabel << " (" << segment.direction
          << " " << segment.tiles << ")";
    }
  }
  return out.str();
}
