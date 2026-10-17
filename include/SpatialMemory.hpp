#pragma once

// Writing and reading a character's spatial memory
// (docs/spatial-memory-design.md section 3.4).
//
// The memory is a consumer of the vision sweep, not a second sampler: the writer
// takes the cells `Vision::lastVisible` already holds and records them. One sweep,
// one geometry, several consumers -- the same rule the vision query itself follows.
//
// Everything here is `inline` and header-only, like TaskList.hpp, so the write
// rules and the dump share one definition with no translation unit to keep in
// sync. Only the file I/O lives elsewhere (Game::DumpSpatialMemory), because that
// is the one part that is not a pure function of the memory.

#include "Components.h"
#include "Map.h"
#include "TaskList.hpp"
#include "VisionQuery.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Memory {

// Creates the `Origin` place on the very first sample. It is the only thing a
// memory can be anchored to when it knows nothing, which is what lets a stranger
// describe everything it later finds as "east of where I started" (section 3.4).
//
// Idempotent: called on every write, and does nothing once an origin exists. The
// `cells.empty()` test is what distinguishes "nothing observed yet" from "an
// origin that happens to be at the map's corner".
inline void EnsureOrigin(SpatialMemory &memory, GamePosition firstCell,
                         int tick) {
  if (!memory.cells.empty()) {
    return;
  }
  memory.origin.origin = PlaceOrigin::Origin;
  memory.origin.source = KnowledgeSource::Observed;
  memory.origin.confidence = 255;
  memory.origin.anchor = firstCell;
  memory.origin.width = 0;  // a point, not an area
  memory.origin.height = 0;
  memory.origin.attribute = "start";
  memory.origin.firstSeenTick = tick;
  memory.origin.lastSeenTick = tick;
}

// The write rule for one observed cell.
//
// Separate from the map lookup so the rule can be exercised without a Map: this
// function is where "did the world change under me" is decided, and that is worth
// being able to test directly.
//
// The important half is the re-observation. A cell previously seen as one surface
// and now seen as another must take the new surface -- terrain changes under a
// character (a field tilled, sand laid over a floor), and a memory that kept its
// first sighting forever would be wrong in exactly the way this feature exists to
// make visible. Section 4.4: overwrite, and mark the contradiction once the digest
// can show it.
inline void RecordCell(SpatialMemory &memory, GamePosition pos,
                       const std::string &surface, bool walkable, int tick) {
  const std::uint64_t key = VisionQuery::CellKey(pos);
  auto known = memory.cells.find(key);
  if (known == memory.cells.end()) {
    MemoryCell record;
    record.pos = pos;
    record.surface = surface;
    record.walkable = walkable;
    record.observedTick = tick;
    record.visitCount = 1;
    memory.cells.emplace(key, std::move(record));
    return;
  }

  // Refreshed in place: the same cell, re-seen. The visit count is what makes a
  // road worth more than a corner when the cap has to drop something.
  MemoryCell &cell = known->second;
  cell.surface = surface;
  cell.walkable = walkable;
  cell.observedTick = tick;
  ++cell.visitCount;
}

// Records the cells just seen, resolving each one's surface through the map.
inline void ObserveCells(SpatialMemory &memory, Map &map,
                         const std::vector<GamePosition> &visible, int tick) {
  for (const GamePosition &cell : visible) {
    // A null tile is a cell the map erased rather than painted; there is nothing
    // to remember about it, and recording a blank surface would be a lie.
    Tile *tile = map.GetTile(cell.x, cell.y);
    if (tile == nullptr) {
      continue;
    }
    RecordCell(memory, cell, tile->name, !tile->blocksTile, tick);
  }
}

// Records the named things standing in the cells just seen. Characters are
// recorded too, flagged, because "who have I seen here" is knowledge of the same
// shape as "what is here" -- just a different kind.
inline void ObserveObjects(SpatialMemory &memory, flecs::world &world,
                           const std::vector<GamePosition> &visible,
                           flecs::entity observer, int tick) {
  std::unordered_set<std::uint64_t> seenCells;
  seenCells.reserve(visible.size() * 2);
  for (const GamePosition &cell : visible) {
    seenCells.insert(VisionQuery::CellKey(cell));
  }

  world.filter<GamePosition, DisplayName>().each(
      [&](flecs::entity other, const GamePosition &pos,
          const DisplayName &name) {
        if (other == observer || name.name.empty()) {
          return;
        }
        if (seenCells.count(VisionQuery::CellKey(pos)) == 0) {
          return;
        }

        auto found = memory.objects.find(other.id());
        if (found == memory.objects.end()) {
          SeenObject record;
          record.templateId =
              other.has<ItemType>() ? other.get<ItemType>()->id : std::string();
          record.character = other.has<CharacterTag>();
          record.firstSeenTick = tick;
          found = memory.objects.emplace(other.id(), std::move(record)).first;
        }

        // Refreshed on every sighting: a name can be changed, a character can
        // move, and a thing that came back is no longer vanished.
        SeenObject &record = found->second;
        record.name = name.name;
        record.pos = pos;
        record.lastSeenTick = tick;
        record.vanishedTick = -1;
      });

  // Vanish pass. Absence is only evidence when the cell is visible *now*: a
  // recorded thing whose cell is in this sweep but whose entity no longer exists
  // has been destroyed (a depleted tree, a consumed item).
  //
  // The two guards are the whole rule. An object out of sight is unseen, not
  // gone; and a thing that is still alive has merely moved, which the pass above
  // records when it is next seen.
  //
  // `flecs::entity_t` carries the generation, so a recycled id (a tree destroyed,
  // an apple spawned into the same slot) is a different key and a stale record
  // cannot be mistaken for a living entity.
  for (auto &[id, object] : memory.objects) {
    if (object.vanishedTick >= 0) {
      continue;
    }
    if (seenCells.count(VisionQuery::CellKey(object.pos)) == 0) {
      continue;
    }
    if (!world.is_alive(id)) {
      object.vanishedTick = tick;
    }
  }
}

// Drops the least valuable cells once the store is over budget. Ranked by visit
// count first and age second: a road walked every day is worth more than a corner
// seen once, and among equals the newer observation wins.
//
// Cell-only for now. Stage 3 adds the rule that a cell inside a durable place is
// never evicted, which matters as soon as there are places to protect.
inline void EnforceCellCap(SpatialMemory &memory) {
  const std::size_t cap = static_cast<std::size_t>(DEFAULT_MEMORY_CELLS_MAX);
  if (memory.cells.size() <= cap) {
    return;
  }

  // (visitCount, observedTick, key): sort ascending, erase the prefix.
  std::vector<std::pair<std::pair<int, int>, std::uint64_t>> ranked;
  ranked.reserve(memory.cells.size());
  for (const auto &[key, cell] : memory.cells) {
    ranked.push_back({{cell.visitCount, cell.observedTick}, key});
  }
  std::sort(ranked.begin(), ranked.end());

  const std::size_t excess = memory.cells.size() - cap;
  for (std::size_t i = 0; i < excess; ++i) {
    memory.cells.erase(ranked[i].second);
  }
}

// Drops the longest-unseen objects once the store is over budget.
inline void EnforceObjectCap(SpatialMemory &memory) {
  const std::size_t cap = static_cast<std::size_t>(DEFAULT_MEMORY_OBJECTS_MAX);
  if (memory.objects.size() <= cap) {
    return;
  }

  std::vector<std::pair<int, flecs::entity_t>> ranked;
  ranked.reserve(memory.objects.size());
  for (const auto &[id, object] : memory.objects) {
    ranked.push_back({object.lastSeenTick, id});
  }
  std::sort(ranked.begin(), ranked.end());

  const std::size_t excess = memory.objects.size() - cap;
  for (std::size_t i = 0; i < excess; ++i) {
    memory.objects.erase(ranked[i].second);
  }
}

// Enum names are part of the serialization, so they live next to it rather than
// in the window: a dump that says "1" is unreadable six months later.
inline const char *PlaceOriginName(PlaceOrigin origin) {
  switch (origin) {
  case PlaceOrigin::Origin: return "Origin";
  case PlaceOrigin::Inferred: return "Inferred";
  case PlaceOrigin::Authored: return "Authored";
  }
  return "Inferred";
}

inline const char *KnowledgeSourceName(KnowledgeSource source) {
  switch (source) {
  case KnowledgeSource::Observed: return "Observed";
  case KnowledgeSource::Told: return "Told";
  case KnowledgeSource::Inferred: return "Inferred";
  }
  return "Observed";
}

// Places (docs/spatial-memory-design.md section 3.3).
//
// A place is durable state with an identity, unlike a shape or a route frontier,
// because a label has to be able to attach to it while its extent is still
// growing. Its extent and facts, however, are recomputed from membership on every
// pass: the members are the truth and the box is a rendering of them, so a place
// can never claim ground it did not observe.

// Chebyshev distance from a cell to a place's extent; 0 when the cell is inside.
inline int DistanceToExtent(const KnownPlace &place, GamePosition cell) {
  const int x0 = place.anchor.x;
  const int x1 = place.anchor.x + place.width - 1;
  const int y0 = place.anchor.y;
  const int y1 = place.anchor.y + place.height - 1;
  const int dx = cell.x < x0 ? x0 - cell.x : (cell.x > x1 ? cell.x - x1 : 0);
  const int dy = cell.y < y0 ? y0 - cell.y : (cell.y > y1 ? cell.y - y1 : 0);
  return dx > dy ? dx : dy;
}

namespace detail {

inline void RecomputeExtent(KnownPlace &place,
                            const std::unordered_map<std::uint64_t, MemoryCell> &cells) {
  if (place.memberCells.empty()) {
    return;
  }
  int minX = 0, minY = 0, maxX = 0, maxY = 0;
  bool first = true;
  for (const std::uint64_t key : place.memberCells) {
    const auto found = cells.find(key);
    if (found == cells.end()) {
      continue;  // evicted under the cap; the member entry is now stale
    }
    const GamePosition pos = found->second.pos;
    if (first) {
      minX = maxX = pos.x;
      minY = maxY = pos.y;
      first = false;
      continue;
    }
    if (pos.x < minX) minX = pos.x;
    if (pos.x > maxX) maxX = pos.x;
    if (pos.y < minY) minY = pos.y;
    if (pos.y > maxY) maxY = pos.y;
  }
  if (first) {
    return;
  }
  place.anchor = {minX, minY};
  place.width = maxX - minX + 1;
  place.height = maxY - minY + 1;
}

// Facts are counted from the raw vocabulary: surfaces from member cells, objects
// from whatever the agent recorded standing on one. Sorting makes the dump stable.
inline void RecomputeFacts(
    SpatialMemory &memory, KnownPlace &place,
    const std::unordered_map<std::uint64_t, int> &placeOfCell) {
  place.facts.clear();

  std::map<std::string, int> surfaces;
  for (const std::uint64_t key : place.memberCells) {
    const auto found = memory.cells.find(key);
    if (found == memory.cells.end()) {
      continue;
    }
    ++surfaces[found->second.surface];
  }

  std::map<std::string, int> objects;
  for (const auto &[id, object] : memory.objects) {
    if (object.character) {
      continue;  // people are not part of a place's furniture
    }
    if (object.vanishedTick >= 0) {
      continue;  // it was here; it is not any more (section 12.5)
    }
    const auto owner = placeOfCell.find(VisionQuery::CellKey(object.pos));
    if (owner == placeOfCell.end() || owner->second != place.id) {
      continue;
    }
    ++objects[object.templateId.empty() ? object.name : object.templateId];
  }

  for (const auto &[name, count] : surfaces) {
    place.facts.push_back({"surface", name, count});
  }
  for (const auto &[name, count] : objects) {
    place.facts.push_back({"object", name, count});
  }
}

// Drops the least substantial place when the cap is reached: fewest member
// cells, then the one seen longest ago. Cells belonging to it stay in memory and
// simply stop being placed -- they are not re-derived, because lastPlaceTick has
// already passed them.
inline void EvictPlace(SpatialMemory &memory) {
  if (memory.places.empty()) {
    return;
  }
  std::size_t worst = 0;
  for (std::size_t i = 1; i < memory.places.size(); ++i) {
    const KnownPlace &candidate = memory.places[i];
    const KnownPlace &current = memory.places[worst];
    if (candidate.memberCells.size() < current.memberCells.size()) {
      worst = i;
    } else if (candidate.memberCells.size() == current.memberCells.size() &&
               candidate.lastSeenTick < current.lastSeenTick) {
      worst = i;
    }
  }
  memory.places.erase(memory.places.begin() + static_cast<std::ptrdiff_t>(worst));
}

}  // namespace detail

// Assigns cells observed since the last pass to a place, creating one when
// nothing claims them.
//
// Authored geometry wins where it overlaps (section 3.3): a cell inside a map
// Location belongs to that Location and never to a cluster. Everything else joins
// the nearest cluster of the same surface within DEFAULT_MEMORY_PLACE_CLUSTER_RADIUS,
// which is what makes two touching but different fields two places rather than one.
//
// The authored label is the map's name, i.e. "on sight" -- the convenient policy
// discussed for place names. The data-driven OnSight/OnTold split is a later
// refinement; `authoringId` is kept separate from `label` precisely so that
// switch does not need a schema change.
inline void DerivePlaces(SpatialMemory &memory, Map &map, int tick) {
  if (memory.lastPlaceTick >= tick) {
    return;
  }

  std::unordered_set<std::uint64_t> placed;
  for (const KnownPlace &place : memory.places) {
    placed.insert(place.memberCells.begin(), place.memberCells.end());
  }

  // Deterministic order: an unordered_map's iteration order would make the same
  // walk cluster differently on two runs.
  std::vector<const MemoryCell *> fresh;
  for (const auto &[key, cell] : memory.cells) {
    if (cell.observedTick > memory.lastPlaceTick && placed.count(key) == 0) {
      fresh.push_back(&cell);
    }
  }
  std::sort(fresh.begin(), fresh.end(),
            [](const MemoryCell *a, const MemoryCell *b) {
              if (a->pos.y != b->pos.y) return a->pos.y < b->pos.y;
              return a->pos.x < b->pos.x;
            });

  for (const MemoryCell *cell : fresh) {
    const std::uint64_t key = VisionQuery::CellKey(cell->pos);
    KnownPlace *target = nullptr;

    Location *authored = map.GetLocation(cell->pos);
    if (authored != nullptr) {
      for (KnownPlace &place : memory.places) {
        if (place.origin == PlaceOrigin::Authored &&
            place.authoringId == authored->name) {
          target = &place;
          break;
        }
      }
      if (target == nullptr) {
        if (memory.places.size() >=
            static_cast<std::size_t>(DEFAULT_MEMORY_PLACES_MAX)) {
          detail::EvictPlace(memory);
        }
        KnownPlace place;
        place.id = memory.nextPlaceId++;
        place.origin = PlaceOrigin::Authored;
        place.source = KnowledgeSource::Observed;
        place.confidence = 255;
        place.anchor = cell->pos;
        place.attribute = "authored";
        place.authoringId = authored->name;
        place.label = authored->name;
        place.firstSeenTick = tick;
        place.lastSeenTick = tick;
        memory.places.push_back(std::move(place));
        target = &memory.places.back();
      }
    } else {
      int best = -1;
      int bestDistance = DEFAULT_MEMORY_PLACE_CLUSTER_RADIUS + 1;
      for (std::size_t i = 0; i < memory.places.size(); ++i) {
        KnownPlace &candidate = memory.places[i];
        if (candidate.origin == PlaceOrigin::Authored) {
          continue;
        }
        if (candidate.attribute != cell->surface) {
          continue;
        }
        const int distance = DistanceToExtent(candidate, cell->pos);
        if (distance > DEFAULT_MEMORY_PLACE_CLUSTER_RADIUS) {
          continue;
        }
        if (distance < bestDistance) {
          best = static_cast<int>(i);
          bestDistance = distance;
        }
      }
      if (best >= 0) {
        target = &memory.places[static_cast<std::size_t>(best)];
      } else {
        if (memory.places.size() >=
            static_cast<std::size_t>(DEFAULT_MEMORY_PLACES_MAX)) {
          detail::EvictPlace(memory);
        }
        KnownPlace place;
        place.id = memory.nextPlaceId++;
        place.origin = PlaceOrigin::Inferred;
        place.source = KnowledgeSource::Observed;
        place.confidence = 255;
        place.anchor = cell->pos;
        place.attribute = cell->surface;
        place.firstSeenTick = tick;
        place.lastSeenTick = tick;
        memory.places.push_back(std::move(place));
        target = &memory.places.back();
      }
    }

    target->memberCells.push_back(key);
    target->lastSeenTick = tick;
    detail::RecomputeExtent(*target, memory.cells);
    placed.insert(key);
  }

  memory.lastPlaceTick = tick;

  // Facts and extents for every place, not only the touched ones: a place whose
  // object was harvested, or whose cell was re-observed as another surface, must
  // lose the stale count on the same pass.
  std::unordered_map<std::uint64_t, int> placeOfCell;
  for (const KnownPlace &place : memory.places) {
    for (const std::uint64_t key : place.memberCells) {
      placeOfCell.emplace(key, place.id);
    }
  }
  for (KnownPlace &place : memory.places) {
    detail::RecomputeExtent(place, memory.cells);
    detail::RecomputeFacts(memory, place, placeOfCell);
  }
}

// A stable colour per place, so the map rectangle and the window swatch agree and
// a place keeps its colour across runs (ids are assigned in discovery order).
//
// Golden-angle hue stepping: consecutive ids land far apart on the wheel, which is
// what makes a dozen overlapping rectangles readable at a glance.
inline Color PlaceColor(int id) {
  const float hue = std::fmod(static_cast<float>(id) * 137.508f, 360.0f);
  return ColorFromHSV(hue, 0.65f, 0.95f);
}

// One axis-aligned run of cells that share a surface and were observed.
//
// Shapes are a DERIVED view, never the store (section 3.4). The decomposition
// below is exact -- the union of the rects is precisely the set of known cells --
// which is why it is allowed to exist at all: it cannot claim ground the agent
// never saw, so it cannot invent a road that is not there.
struct MemoryShape {
  std::string surface;
  GamePosition origin;  // top-left
  int width = 1;
  int height = 1;
  int cellCount = 0;    // always width * height: every cell in the rect is known
  int observedTick = 0; // the newest observation among them
};

// Merges horizontal runs of equal surface, then stacks vertically adjacent runs
// with an identical span. O(n log n) and lossless for rectilinear content.
//
// Expected ratios, honestly: ~30x for a straight road, 2-4x for a field or a
// room, ~1x for a diagonal road or scattered single cells, which decompose into
// 1x1 rects. Fitting circles or curves is not attempted: a coastline is not a
// circle, and a failed fit is a shape worse than the cells it replaced.
inline std::vector<MemoryShape> ToShapes(const SpatialMemory &memory) {
  // surface -> row -> column -> cell
  std::map<std::string, std::map<int, std::map<int, const MemoryCell *>>> rows;
  for (const auto &[key, cell] : memory.cells) {
    rows[cell.surface][cell.pos.y][cell.pos.x] = &cell;
  }

  std::vector<MemoryShape> shapes;

  for (const auto &[surface, byRow] : rows) {
    // Rects still growing downward, keyed by their span. Spans are unique within
    // a row because runs are disjoint, so the key needs no disambiguation.
    struct Growing {
      int x0 = 0;
      int x1 = 0;
      int bottomY = 0;
      int height = 1;
      int observedTick = 0;
    };
    std::map<std::pair<int, int>, Growing> growing;

    for (const auto &[y, byColumn] : byRow) {
      // Horizontal runs in this row.
      std::vector<std::pair<int, int>> runs;
      std::vector<int> runTicks;
      bool haveRun = false;
      int x0 = 0;
      int x1 = 0;
      int tick = 0;
      for (const auto &[x, cell] : byColumn) {
        if (haveRun && x == x1 + 1) {
          x1 = x;
          tick = std::max(tick, cell->observedTick);
          continue;
        }
        if (haveRun) {
          runs.push_back({x0, x1});
          runTicks.push_back(tick);
        }
        x0 = x1 = x;
        tick = cell->observedTick;
        haveRun = true;
      }
      if (haveRun) {
        runs.push_back({x0, x1});
        runTicks.push_back(tick);
      }

      std::map<std::pair<int, int>, Growing> next;
      for (std::size_t i = 0; i < runs.size(); ++i) {
        const std::pair<int, int> span = runs[i];
        const auto found = growing.find(span);
        Growing rect;
        rect.x0 = span.first;
        rect.x1 = span.second;
        rect.bottomY = y;
        rect.height = found == growing.end() ? 1 : found->second.height + 1;
        rect.observedTick =
            found == growing.end()
                ? runTicks[i]
                : std::max(found->second.observedTick, runTicks[i]);
        next[span] = rect;
      }

      // Anything that did not continue on this row is finished here.
      for (const auto &[span, rect] : growing) {
        if (next.count(span) != 0) {
          continue;
        }
        MemoryShape shape;
        shape.surface = surface;
        shape.origin = {rect.x0, rect.bottomY - rect.height + 1};
        shape.width = rect.x1 - rect.x0 + 1;
        shape.height = rect.height;
        shape.cellCount = shape.width * shape.height;
        shape.observedTick = rect.observedTick;
        shapes.push_back(std::move(shape));
      }
      growing = std::move(next);
    }

    for (const auto &[span, rect] : growing) {
      MemoryShape shape;
      shape.surface = surface;
      shape.origin = {rect.x0, rect.bottomY - rect.height + 1};
      shape.width = rect.x1 - rect.x0 + 1;
      shape.height = rect.height;
      shape.cellCount = shape.width * shape.height;
      shape.observedTick = rect.observedTick;
      shapes.push_back(std::move(shape));
    }
  }

  // Deterministic order, so two runs of the same walk produce the same file.
  std::sort(shapes.begin(), shapes.end(),
            [](const MemoryShape &a, const MemoryShape &b) {
              if (a.surface != b.surface) return a.surface < b.surface;
              if (a.origin.y != b.origin.y) return a.origin.y < b.origin.y;
              return a.origin.x < b.origin.x;
            });
  return shapes;
}

// JSON string quoting, borrowed from the same library that used to serialize the
// whole document: a hand-written emitter still must not hand-roll escapes.
inline std::string Quote(const std::string &text) {
  return nlohmann::json(text).dump();
}

// The record as compact JSON. Written by hand rather than by `json::dump`, for
// one reason: nlohmann pretty-prints every nested array, so a six-field cell
// becomes eight lines. Here a cell is one line of a documented tuple, which is
// both compact and diffable -- the two properties the dump exists for.
//
// `includeCells` is the detail switch: shapes and counts by default, the raw cell
// list on request. Shapes are exact (see ToShapes), so the default view is a
// faithful reduction rather than a summary that may lie.
inline std::string DumpJson(const SpatialMemory &memory, const std::string &name,
                            int nowTick, bool includeCells) {
  const std::vector<MemoryShape> shapes = ToShapes(memory);

  // Stable order for the same reason the cells are sorted: a dump is compared
  // between runs, and unordered_map iteration order is not a comparison.
  std::vector<const MemoryCell *> cells;
  cells.reserve(memory.cells.size());
  for (const auto &[key, cell] : memory.cells) {
    cells.push_back(&cell);
  }
  std::sort(cells.begin(), cells.end(),
            [](const MemoryCell *a, const MemoryCell *b) {
              if (a->pos.y != b->pos.y) return a->pos.y < b->pos.y;
              return a->pos.x < b->pos.x;
            });

  std::vector<const SeenObject *> objects;
  objects.reserve(memory.objects.size());
  for (const auto &[id, object] : memory.objects) {
    objects.push_back(&object);
  }
  std::sort(objects.begin(), objects.end(),
            [](const SeenObject *a, const SeenObject *b) {
              if (a->lastSeenTick != b->lastSeenTick) {
                return a->lastSeenTick > b->lastSeenTick;
              }
              return a->name < b->name;
            });

  std::vector<const KnownPlace *> places;
  places.push_back(&memory.origin);
  for (const KnownPlace &place : memory.places) {
    places.push_back(&place);
  }

  std::ostringstream out;
  out << "{\n";
  out << "  \"name\": " << Quote(name) << ",\n";
  out << "  \"tick\": " << nowTick << ",\n";
  out << "  \"caps\": {\"cells\": " << DEFAULT_MEMORY_CELLS_MAX
      << ", \"objects\": " << DEFAULT_MEMORY_OBJECTS_MAX << "},\n";
  out << "  \"counts\": {\"cells\": " << memory.cells.size()
      << ", \"shapes\": " << shapes.size()
      << ", \"objects\": " << memory.objects.size()
      << ", \"places\": " << places.size() << "},\n";
  out << "  \"cellFormat\": \"x, y, surface, walkable, observedTick, visitCount\",\n";

  if (includeCells) {
    out << "  \"cells\": [";
    bool first = true;
    for (const MemoryCell *cell : cells) {
      out << (first ? "\n" : ",\n");
      out << "    [" << cell->pos.x << ", " << cell->pos.y << ", "
          << Quote(cell->surface) << ", "
          << (cell->walkable ? "true" : "false") << ", " << cell->observedTick
          << ", " << cell->visitCount << "]";
      first = false;
    }
    out << "\n  ],\n";
  }

  out << "  \"shapes\": [";
  {
    bool first = true;
    for (const MemoryShape &shape : shapes) {
      out << (first ? "\n" : ",\n");
      out << "    {\"surface\": " << Quote(shape.surface) << ", \"x\": "
          << shape.origin.x << ", \"y\": " << shape.origin.y
          << ", \"w\": " << shape.width << ", \"h\": " << shape.height
          << ", \"cells\": " << shape.cellCount
          << ", \"observedTick\": " << shape.observedTick << "}";
      first = false;
    }
    out << "\n  ],\n";
  }

  out << "  \"objects\": [";
  {
    bool first = true;
    for (const SeenObject *object : objects) {
      out << (first ? "\n" : ",\n");
      out << "    {\"templateId\": " << Quote(object->templateId)
          << ", \"name\": " << Quote(object->name) << ", \"pos\": ["
          << object->pos.x << ", " << object->pos.y << "], \"character\": "
          << (object->character ? "true" : "false")
          << ", \"firstSeenTick\": " << object->firstSeenTick
          << ", \"lastSeenTick\": " << object->lastSeenTick
          << ", \"vanishedTick\": ";
      if (object->vanishedTick >= 0) {
        out << object->vanishedTick;
      } else {
        out << "null";
      }
      out << "}";
      first = false;
    }
    out << "\n  ],\n";
  }

  out << "  \"places\": [";
  {
    bool first = true;
    for (const KnownPlace *place : places) {
      out << (first ? "\n" : ",\n");
      out << "    {\"origin\": " << Quote(PlaceOriginName(place->origin))
          << ", \"source\": " << Quote(KnowledgeSourceName(place->source))
          << ", \"confidence\": " << static_cast<int>(place->confidence)
          << ", \"anchor\": [" << place->anchor.x << ", " << place->anchor.y
          << "], \"w\": " << place->width << ", \"h\": " << place->height
          << ", \"attribute\": " << Quote(place->attribute)
          << ", \"label\": " << Quote(place->label)
          << ", \"authoringId\": " << Quote(place->authoringId)
          << ", \"facts\": [";
      bool firstFact = true;
      for (const PlaceFact &fact : place->facts) {
        out << (firstFact ? "" : ", ");
        out << "{\"category\": " << Quote(fact.category)
            << ", \"name\": " << Quote(fact.name)
            << ", \"count\": " << fact.count << "}";
        firstFact = false;
      }
      out << "], \"firstSeenTick\": " << place->firstSeenTick
          << ", \"lastSeenTick\": " << place->lastSeenTick << "}";
      first = false;
    }
    out << "\n  ],\n";
  }

  out << "  \"notes\": [";
  {
    bool first = true;
    for (const MemoryNote &note : memory.notes) {
      out << (first ? "\n" : ",\n");
      out << "    {\"placeLabel\": " << Quote(note.placeLabel)
          << ", \"text\": " << Quote(note.text) << ", \"tick\": " << note.tick
          << "}";
      first = false;
    }
    out << "\n  ]\n";
  }

  out << "}\n";
  return out.str();
}

}  // namespace Memory
