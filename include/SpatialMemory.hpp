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
#include "PlaceCategoryRegistry.h"
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
  memory.origin.primaryAspect = "start";
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
                       const std::string &surface, bool walkable,
                       const std::vector<SeedCategory> &categories, int tick) {
  const std::uint64_t key = VisionQuery::CellKey(pos);
  auto known = memory.cells.find(key);
  if (known == memory.cells.end()) {
    MemoryCell record;
    record.pos = pos;
    record.surface = surface;
    record.walkable = walkable;
    record.categories = categories;
    record.observedTick = tick;
    record.visitCount = 1;
    memory.cells.emplace(key, std::move(record));
    return;
  }

  // Refreshed in place: the same cell, re-seen. The visit count is what makes a
  // road worth more than a corner when the cap has to drop something, and the
  // categories are refreshed because a tile can be repainted under a character --
  // a path turned into a road is the same cell with a different part to play.
  MemoryCell &cell = known->second;
  cell.surface = surface;
  cell.walkable = walkable;
  // Compared before assigning: most tiles have no categories, and an assignment
  // per visible cell per sample was an allocation per cell for no change.
  if (cell.categories != categories) {
    cell.categories = categories;
  }
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
    RecordCell(memory, cell, tile->name, !tile->blocksTile,
               tile->placeCategories, tick);
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
          record.character = other.has<CharacterTag>();
          record.firstSeenTick = tick;
          found = memory.objects.emplace(other.id(), std::move(record)).first;
        }

        // Everything template-derived is refreshed on every sighting, not only
        // when the record is created. The entity can change under a stable id:
        // the map editor overwrites whatever stands on a cell through
        // ApplyTemplate, and Evolvable growth re-applies a template in place. A
        // record that kept its first sighting's categories would be a chest whose
        // seed data says "wooden wall" -- visible, and never a place.
        SeenObject &record = found->second;
        record.templateId =
            other.has<ItemType>() ? other.get<ItemType>()->id : std::string();
        record.name = name.name;
        record.pos = pos;
        record.lastSeenTick = tick;
        record.vanishedTick = -1;
        record.portable = other.has<Portable>();
        record.structural = other.has<Structural>();
        if (const PlaceCategories *categories = other.get<PlaceCategories>()) {
          if (record.categories != categories->categories) {
            record.categories = categories->categories;
          }
        } else if (!record.categories.empty()) {
          record.categories.clear();
        }
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
  case KnowledgeSource::Authored: return "Authored";
  case KnowledgeSource::Observed: return "Observed";
  case KnowledgeSource::Told: return "Told";
  case KnowledgeSource::Inferred: return "Inferred";
  }
  return "Observed";
}

// Places (docs/place-derivation-design.md).
//
// A place is a cluster of seeds that share one cluster-forming category. The
// partition is recomputed from memory on every sample, so arrival order cannot
// matter and two clusters that grow into each other merge. The only thing carried
// across passes is identity -- id, label, firstSeenTick -- matched by overlap.

namespace detail {

// Chebyshev distance: the grid's own notion of "near", and the same metric the
// vision code uses for adjacency.
inline int Chebyshev(GamePosition a, GamePosition b) {
  const int dx = a.x > b.x ? a.x - b.x : b.x - a.x;
  const int dy = a.y > b.y ? a.y - b.y : b.y - a.y;
  return dx > dy ? dx : dy;
}

// Euclidean distance, for the aspect field. Round influence reads better than a
// square one, and the field is about how a place *feels* from a cell.
inline float Distance(GamePosition a, GamePosition b) {
  const float dx = static_cast<float>(a.x - b.x);
  const float dy = static_cast<float>(a.y - b.y);
  return std::sqrt(dx * dx + dy * dy);
}

// Linear decay to zero at the source's influence radius
// (docs/place-derivation-design.md section 6.1).
inline float Decay(float distance, float radius) {
  if (radius <= 0.0f) {
    return 0.0f;
  }
  const float t = distance / radius;
  return t >= 1.0f ? 0.0f : 1.0f - t;
}

inline void RecomputeExtent(
    KnownPlace &place,
    const std::unordered_map<std::uint64_t, MemoryCell> &cells) {
  if (place.memberCells.empty()) {
    return;
  }
  int minX = 0, minY = 0, maxX = 0, maxY = 0;
  bool first = true;
  for (const std::uint64_t key : place.memberCells) {
    const auto found = cells.find(key);
    // The key packs the position, so an evicted cell still contributes its
    // coordinate to the box; the surface is what was lost, not the location.
    const GamePosition pos =
        found == cells.end() ? VisionQuery::CellFromKey(key) : found->second.pos;
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
// from whatever was recorded standing on one. Sorting makes the dump stable.
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
  std::unordered_set<std::uint64_t> memberKeys;
  for (const auto &[id, object] : memory.objects) {
    if (object.character || object.vanishedTick >= 0) {
      continue;
    }
    if (object.structural) {
      continue;  // counted below, by whether it touches the place
    }
    const auto owner = placeOfCell.find(VisionQuery::CellKey(object.pos));
    if (owner == placeOfCell.end() || owner->second != place.id) {
      continue;
    }
    ++objects[object.templateId.empty() ? object.name : object.templateId];
  }

  // A wall is part of what a place is made of even though it is never claimed
  // ground, so it is counted by contact rather than by membership.
  memberKeys.reserve(place.memberCells.size() * 2);
  for (const std::uint64_t key : place.memberCells) {
    memberKeys.insert(key);
  }
  for (const auto &[id, object] : memory.objects) {
    if (!object.structural || object.vanishedTick >= 0) {
      continue;
    }
    const std::uint64_t left =
        VisionQuery::CellKey({object.pos.x - 1, object.pos.y});
    const std::uint64_t right =
        VisionQuery::CellKey({object.pos.x + 1, object.pos.y});
    const std::uint64_t up =
        VisionQuery::CellKey({object.pos.x, object.pos.y - 1});
    const std::uint64_t down =
        VisionQuery::CellKey({object.pos.x, object.pos.y + 1});
    if (memberKeys.count(left) == 0 && memberKeys.count(right) == 0 &&
        memberKeys.count(up) == 0 && memberKeys.count(down) == 0) {
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

// Union-find over seed indices. Plain path halving; the sets never exceed the
// 1024-object cap.
struct DisjointSet {
  std::vector<int> parent;

  explicit DisjointSet(std::size_t size) : parent(size) {
    for (std::size_t i = 0; i < size; ++i) {
      parent[i] = static_cast<int>(i);
    }
  }

  int Find(int x) {
    while (parent[static_cast<std::size_t>(x)] != x) {
      const int grandparent =
          parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
      parent[static_cast<std::size_t>(x)] = grandparent;
      x = grandparent;
    }
    return x;
  }

  void Union(int a, int b) {
    a = Find(a);
    b = Find(b);
    if (a != b) {
      parent[static_cast<std::size_t>(b)] = a;
    }
  }
};

// The observed ground a cluster covers: flood out from the seeds over cells the
// character actually observed, no further than `radius` from the nearest seed,
// and never across a structural cell.
inline std::vector<GamePosition> FloodExtent(
    const SpatialMemory &memory, const std::vector<GamePosition> &seeds,
    int radius, const std::unordered_set<std::uint64_t> &structural) {
  std::vector<GamePosition> extent;
  std::unordered_set<std::uint64_t> visited;
  std::vector<GamePosition> queue;

  // A seed's own cell is part of the extent even when the cell record has been
  // evicted under the cell cap: seeing the object is seeing the ground it stands
  // on. Without this a place could disappear because the detail store forgot the
  // tile.
  for (const GamePosition &seed : seeds) {
    const std::uint64_t key = VisionQuery::CellKey(seed);
    if (visited.insert(key).second) {
      queue.push_back(seed);
    }
  }

  const auto withinRadius = [&seeds, radius](GamePosition cell) {
    for (const GamePosition &seed : seeds) {
      if (Chebyshev(cell, seed) <= radius) {
        return true;
      }
    }
    return false;
  };

  static const int dx[4] = {1, -1, 0, 0};
  static const int dy[4] = {0, 0, 1, -1};

  for (std::size_t head = 0; head < queue.size(); ++head) {
    const GamePosition cell = queue[head];
    extent.push_back(cell);

    for (int i = 0; i < 4; ++i) {
      const GamePosition next{cell.x + dx[i], cell.y + dy[i]};
      const std::uint64_t key = VisionQuery::CellKey(next);
      if (visited.count(key) != 0) {
        continue;
      }
      const auto found = memory.cells.find(key);
      if (found == memory.cells.end()) {
        continue;  // unobserved ground is not claimed
      }
      if (structural.count(key) != 0 || !found->second.walkable) {
        continue;  // a wall or an impassable tile bounds the place
      }
      if (!withinRadius(next)) {
        continue;
      }
      visited.insert(key);
      queue.push_back(next);
    }
  }

  return extent;
}

// Turns a place's seeds into its aspect field and the summaries a reader needs.
//
// Sources are every category of every owned seed -- forming ones included, so a
// market's own stalls decay too and its edge is less "market" than its middle.
inline void BuildAspects(KnownPlace &place, const SpatialMemory &memory,
                         const PlaceCategoryRegistry &registry) {
  place.aspectSources.clear();
  place.aspects.clear();
  place.primaryAspect.clear();

  for (const flecs::entity_t id : place.memberSeeds) {
    const auto found = memory.objects.find(id);
    if (found == memory.objects.end()) {
      continue;
    }
    for (const SeedCategory &seed : found->second.categories) {
      const PlaceCategory *category = registry.Get(seed.category);
      if (category == nullptr) {
        continue;  // unknown ids are reported once at load
      }
      AspectSource source;
      source.category = seed.category;
      source.pos = found->second.pos;
      source.weight = seed.weight;
      source.radius = static_cast<float>(category->radius);
      place.aspectSources.push_back(std::move(source));
    }
  }

  // Tile sources: one per cell would be hundreds for a lake or a field, and the
  // field is what gets queried per cell by the overlay. A deterministic
  // subsample per category keeps the shape (the decay still comes from real
  // positions) at a bounded cost. Object sources are not subsampled: there are
  // few of them and each is a distinct thing.
  {
    std::map<std::string, std::vector<const MemoryCell *>> byCategory;
    for (const std::uint64_t key : place.memberCells) {
      const auto found = memory.cells.find(key);
      if (found == memory.cells.end() || found->second.categories.empty()) {
        continue;
      }
      for (const SeedCategory &seed : found->second.categories) {
        byCategory[seed.category].push_back(&found->second);
      }
    }

    for (const auto &[categoryId, cells] : byCategory) {
      const PlaceCategory *definition = registry.Get(categoryId);
      if (definition == nullptr || cells.empty()) {
        continue;
      }
      float weight = 0.0f;
      for (const MemoryCell *cell : cells) {
        for (const SeedCategory &seed : cell->categories) {
          if (seed.category == categoryId && seed.weight > weight) {
            weight = seed.weight;
          }
        }
      }
      const std::size_t stride =
          (cells.size() + DEFAULT_PLACE_TILE_SOURCES_MAX - 1) /
          DEFAULT_PLACE_TILE_SOURCES_MAX;
      for (std::size_t i = 0; i < cells.size(); i += stride) {
        AspectSource source;
        source.category = categoryId;
        source.pos = cells[i]->pos;
        source.weight = weight;
        source.radius = static_cast<float>(definition->radius);
        place.aspectSources.push_back(std::move(source));
      }
    }
  }

  // Structural sources: walls that *touch* the claimed ground (section 3.4).
  //
  // Orthogonal contact only -- a corner touch is not an enclosure -- and both
  // sides of a shared wall inherit it, because the wall is the material of the
  // boundary rather than a property of one room. Same subsampling as tile
  // sources: a hall's walls are dozens of cells and each would otherwise cost a
  // query.
  {
    std::unordered_set<std::uint64_t> memberKeys(place.memberCells.begin(),
                                                 place.memberCells.end());
    std::map<std::string, std::vector<std::pair<GamePosition, float>>> walls;
    for (const auto &[id, object] : memory.objects) {
      if (!object.structural || object.vanishedTick >= 0 ||
          object.categories.empty()) {
        continue;
      }
      const std::uint64_t left =
          VisionQuery::CellKey({object.pos.x - 1, object.pos.y});
      const std::uint64_t right =
          VisionQuery::CellKey({object.pos.x + 1, object.pos.y});
      const std::uint64_t up =
          VisionQuery::CellKey({object.pos.x, object.pos.y - 1});
      const std::uint64_t down =
          VisionQuery::CellKey({object.pos.x, object.pos.y + 1});
      if (memberKeys.count(left) == 0 && memberKeys.count(right) == 0 &&
          memberKeys.count(up) == 0 && memberKeys.count(down) == 0) {
        continue;
      }
      for (const SeedCategory &seed : object.categories) {
        if (seed.weight <= 0.0f || registry.Get(seed.category) == nullptr) {
          continue;
        }
        walls[seed.category].push_back({object.pos, seed.weight});
      }
    }

    for (auto &[categoryId, cells] : walls) {
      const PlaceCategory *definition = registry.Get(categoryId);
      // Sorted before subsampling so two passes pick the same cells.
      std::sort(cells.begin(), cells.end(),
                [](const std::pair<GamePosition, float> &a,
                   const std::pair<GamePosition, float> &b) {
                  if (a.first.y != b.first.y) return a.first.y < b.first.y;
                  return a.first.x < b.first.x;
                });
      const std::size_t stride =
          (cells.size() + DEFAULT_PLACE_TILE_SOURCES_MAX - 1) /
          DEFAULT_PLACE_TILE_SOURCES_MAX;
      for (std::size_t i = 0; i < cells.size(); i += stride) {
        AspectSource source;
        source.category = categoryId;
        source.pos = cells[i].first;
        source.weight = cells[i].second;
        source.radius = static_cast<float>(definition->radius);
        place.aspectSources.push_back(std::move(source));
      }
    }
  }

  if (place.aspectSources.empty() || place.memberCells.empty()) {
    return;
  }

  std::map<std::string, std::vector<const AspectSource *>> byCategory;
  for (const AspectSource &source : place.aspectSources) {
    byCategory[source.category].push_back(&source);
  }

  for (const auto &[category, sources] : byCategory) {
    PlaceAspect summary;
    summary.category = category;

    // The peak is a source's own cell, where decay is 1.
    for (const AspectSource *source : sources) {
      if (source->weight > summary.peak) {
        summary.peak = source->weight;
        summary.peakPos = source->pos;
      }
    }

    // The mean is the area average over the claimed ground: the number that tells
    // "strong at one edge" apart from "strong everywhere". Sampled, not exhaustive:
    // it is a summary for a reader, and evaluating every cell against every source
    // was the largest single cost of deriving places every sample.
    double total = 0.0;
    std::size_t count = 0;
    const std::size_t stride =
        (place.memberCells.size() + DEFAULT_PLACE_MEAN_SAMPLES - 1) /
        DEFAULT_PLACE_MEAN_SAMPLES;
    for (std::size_t i = 0; i < place.memberCells.size(); i += stride) {
      const std::uint64_t key = place.memberCells[i];
      const auto found = memory.cells.find(key);
      const GamePosition cell = found == memory.cells.end()
                                    ? VisionQuery::CellFromKey(key)
                                    : found->second.pos;
      float best = 0.0f;
      for (const AspectSource *source : sources) {
        const float value =
            source->weight *
            Decay(Distance(cell, source->pos), source->radius);
        if (value > best) {
          best = value;
        }
      }
      total += best;
      ++count;
    }
    summary.mean = count == 0
                       ? 0.0f
                       : static_cast<float>(total / static_cast<double>(count));

    place.aspects.push_back(std::move(summary));
  }

  std::sort(place.aspects.begin(), place.aspects.end(),
            [](const PlaceAspect &a, const PlaceAspect &b) {
              return a.peak > b.peak;
            });

  // Display only: the forming category that covers the most ground, not the one
  // with the single strongest seed. A market with one apple tree in it is a market
  // -- the peak says "orchard" while the area says otherwise, and the area is what
  // a reader means by "what is this place".
  float bestMean = -1.0f;
  for (const PlaceAspect &aspect : place.aspects) {
    const PlaceCategory *category = registry.Get(aspect.category);
    if (category == nullptr || !category->formsPlace) {
      continue;
    }
    if (aspect.mean > bestMean) {
      bestMean = aspect.mean;
      place.primaryAspect = aspect.category;
    }
  }
  if (place.primaryAspect.empty() && !place.aspects.empty()) {
    place.primaryAspect = place.aspects.front().category;
  }
}

// Cuts a road network into stretches between junctions and endpoints
// (docs/place-derivation-design.md section 4.2).
//
// A cell with any number of neighbours other than two is a cut: three or more is a
// junction, one is an endpoint. Each maximal stretch between cuts becomes a place,
// and a cut cell belongs to exactly one stretch -- the one holding its lowest
// (y, x) neighbour -- so no cell is claimed twice and none is orphaned.
inline std::vector<std::vector<GamePosition>>
SplitAtJunctions(const std::vector<GamePosition> &cells) {
  std::unordered_set<std::uint64_t> present;
  present.reserve(cells.size() * 2);
  for (const GamePosition &cell : cells) {
    present.insert(VisionQuery::CellKey(cell));
  }

  // A cell connects to its *orthogonal* road neighbours. Diagonals are used only
  // for a cell with no orthogonal neighbour, which is how a diagonal road stays
  // connected without the cell below a T junction reading as a four-way crossing
  // -- it is diagonally adjacent to both arms of the T, and counting those made a
  // one-cell stretch out of the junction's shadow.
  const auto neighbours = [&present](GamePosition cell) {
    std::vector<GamePosition> orthogonal;
    static const int ox[4] = {1, -1, 0, 0};
    static const int oy[4] = {0, 0, 1, -1};
    for (int i = 0; i < 4; ++i) {
      const GamePosition next{cell.x + ox[i], cell.y + oy[i]};
      if (present.count(VisionQuery::CellKey(next)) != 0) {
        orthogonal.push_back(next);
      }
    }
    if (!orthogonal.empty()) {
      return orthogonal;
    }

    std::vector<GamePosition> diagonal;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 || dy == 0) continue;
        const GamePosition next{cell.x + dx, cell.y + dy};
        if (present.count(VisionQuery::CellKey(next)) != 0) {
          diagonal.push_back(next);
        }
      }
    }
    return diagonal;
  };

  std::unordered_set<std::uint64_t> cuts;
  for (const GamePosition &cell : cells) {
    if (neighbours(cell).size() != 2) {
      cuts.insert(VisionQuery::CellKey(cell));
    }
  }
  if (cuts.empty()) {
    return {cells};  // a loop with no junction or endpoint: one stretch
  }

  // Stretches: components of the cells that are not cuts.
  std::unordered_set<std::uint64_t> visited;
  std::vector<std::vector<GamePosition>> stretches;
  std::unordered_map<std::uint64_t, std::size_t> stretchOf;

  for (const GamePosition &start : cells) {
    const std::uint64_t startKey = VisionQuery::CellKey(start);
    if (cuts.count(startKey) != 0 || visited.count(startKey) != 0) {
      continue;
    }
    std::vector<GamePosition> stretch;
    std::vector<GamePosition> queue{start};
    visited.insert(startKey);
    for (std::size_t head = 0; head < queue.size(); ++head) {
      const GamePosition cell = queue[head];
      stretch.push_back(cell);
      stretchOf.emplace(VisionQuery::CellKey(cell), stretches.size());
      for (const GamePosition &next : neighbours(cell)) {
        const std::uint64_t key = VisionQuery::CellKey(next);
        if (cuts.count(key) != 0 || visited.count(key) != 0) {
          continue;
        }
        visited.insert(key);
        queue.push_back(next);
      }
    }
    stretches.push_back(std::move(stretch));
  }

  // Hand each cut cell to the stretch of its lowest (y, x) neighbour. A cut with
  // no ordinary neighbour (a one-cell road) is its own stretch.
  for (const GamePosition &cut : cells) {
    if (cuts.count(VisionQuery::CellKey(cut)) == 0) {
      continue;
    }
    GamePosition best{-1, -1};
    std::size_t bestStretch = stretches.size();
    for (const GamePosition &next : neighbours(cut)) {
      const auto owner = stretchOf.find(VisionQuery::CellKey(next));
      if (owner == stretchOf.end()) {
        continue;
      }
      if (bestStretch == stretches.size() || next.y < best.y ||
          (next.y == best.y && next.x < best.x)) {
        best = next;
        bestStretch = owner->second;
      }
    }
    if (bestStretch < stretches.size()) {
      stretches[bestStretch].push_back(cut);
    } else {
      stretches.push_back({cut});
    }
  }

  // Deterministic order and cell order, so two passes produce the same places.
  for (std::vector<GamePosition> &stretch : stretches) {
    std::sort(stretch.begin(), stretch.end(),
              [](const GamePosition &a, const GamePosition &b) {
                if (a.y != b.y) return a.y < b.y;
                return a.x < b.x;
              });
  }
  std::sort(stretches.begin(), stretches.end(),
            [](const std::vector<GamePosition> &a,
               const std::vector<GamePosition> &b) {
              const GamePosition &first = a.front();
              const GamePosition &other = b.front();
              if (first.y != other.y) return first.y < other.y;
              return first.x < other.x;
            });
  return stretches;
}

}  // namespace detail

// The aspect field evaluated at a cell: the maximum over that category's sources
// of weight x decay(distance) (section 6.1).
//
// Point evaluation is the whole correction to the earlier model. A market beside
// an orchard is an orchard only near the trees; asking this function at the
// market's far corner returns zero.
inline float AspectAt(const KnownPlace &place, GamePosition cell,
                      const std::string &category) {
  float best = 0.0f;
  for (const AspectSource &source : place.aspectSources) {
    if (source.category != category) {
      continue;
    }
    const float value =
        source.weight * detail::Decay(detail::Distance(cell, source.pos),
                                      source.radius);
    if (value > best) {
      best = value;
    }
  }
  return best;
}

// The strongest aspect at a cell, and its category. Used by the overlay's tint,
// where a place identity colour would hide the field.
inline float DominantAspectAt(const KnownPlace &place, GamePosition cell,
                              std::string *category) {
  float best = 0.0f;
  if (category != nullptr) {
    category->clear();
  }
  for (const AspectSource &source : place.aspectSources) {
    const float value =
        source.weight * detail::Decay(detail::Distance(cell, source.pos),
                                      source.radius);
    if (value > best) {
      best = value;
      if (category != nullptr) {
        *category = source.category;
      }
    }
  }
  return best;
}

// A stable colour per aspect category, so the tint means the same thing on every
// pass and in every place.
inline Color CategoryColor(const std::string &category) {
  std::size_t hash = 1469598103934665603ull;
  for (const char c : category) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  const float hue = static_cast<float>(hash % 3600u) / 10.0f;
  return ColorFromHSV(hue, 0.6f, 0.95f);
}

// Rebuilds `memory.places` from the seeds the character has recorded.
//
// One region per connected cluster across categories, so a market beside an
// orchard is one place whose two aspect fields overlap and decay. Linear terrain
// (roads, water) is insular: it links only with its own kind, because otherwise a
// path across the village chains every object within the link radius into a single
// place (section 5.1).
inline void DerivePlaces(SpatialMemory &memory,
                         const PlaceCategoryRegistry &registry, int tick) {
  if (memory.lastPlaceTick >= tick) {
    return;
  }

  struct Seed {
    GamePosition pos;
    flecs::entity_t entity = 0;  // 0 for a tile: it is not an entity
    const std::vector<SeedCategory> *categories = nullptr;
    bool object = false;
    bool insular = false;  // carries a forming category that does not link across
  };

  std::vector<Seed> seeds;
  std::unordered_set<std::uint64_t> structural;

  for (const auto &[id, object] : memory.objects) {
    if (object.vanishedTick >= 0 || object.character) {
      continue;
    }
    if (object.structural) {
      structural.insert(VisionQuery::CellKey(object.pos));
      continue;
    }
    if (object.portable || object.categories.empty()) {
      continue;
    }
    Seed seed;
    seed.pos = object.pos;
    seed.entity = id;
    seed.categories = &object.categories;
    seed.object = true;
    for (const SeedCategory &category : object.categories) {
      const PlaceCategory *definition = registry.Get(category.category);
      if (definition != nullptr && definition->formsPlace &&
          !definition->linksAcross) {
        seed.insular = true;
      }
    }
    seeds.push_back(std::move(seed));
  }

  for (const auto &[key, cell] : memory.cells) {
    if (cell.categories.empty()) {
      continue;
    }
    Seed seed;
    seed.pos = cell.pos;
    seed.entity = 0;
    seed.categories = &cell.categories;
    seed.object = false;
    for (const SeedCategory &category : cell.categories) {
      const PlaceCategory *definition = registry.Get(category.category);
      if (definition != nullptr && definition->formsPlace &&
          !definition->linksAcross) {
        seed.insular = true;
      }
    }
    seeds.push_back(std::move(seed));
  }

  std::sort(seeds.begin(), seeds.end(), [](const Seed &a, const Seed &b) {
    if (a.pos.y != b.pos.y) return a.pos.y < b.pos.y;
    if (a.pos.x != b.pos.x) return a.pos.x < b.pos.x;
    if (a.object != b.object) return a.object < b.object;
    return a.entity < b.entity;
  });

  const auto sharesForming = [&registry](const Seed &a, const Seed &b) {
    for (const SeedCategory &left : *a.categories) {
      const PlaceCategory *leftDefinition = registry.Get(left.category);
      if (leftDefinition == nullptr || !leftDefinition->formsPlace) {
        continue;
      }
      for (const SeedCategory &right : *b.categories) {
        const PlaceCategory *rightDefinition = registry.Get(right.category);
        if (rightDefinition == nullptr || !rightDefinition->formsPlace) {
          continue;
        }
        if (left.category == right.category) {
          return true;
        }
      }
    }
    return false;
  };

  detail::DisjointSet sets(seeds.size());
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    for (std::size_t j = i + 1; j < seeds.size(); ++j) {
      if (detail::Chebyshev(seeds[i].pos, seeds[j].pos) >
          DEFAULT_PLACE_LINK_RADIUS) {
        continue;
      }
      const bool across = !seeds[i].insular && !seeds[j].insular;
      if (across || sharesForming(seeds[i], seeds[j])) {
        sets.Union(static_cast<int>(i), static_cast<int>(j));
      }
    }
  }

  struct Candidate {
    std::vector<GamePosition> extent;
    std::vector<flecs::entity_t> seedIds;
  };
  std::vector<Candidate> candidates;

  std::map<int, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    groups[sets.Find(static_cast<int>(i))].push_back(i);
  }

  for (const auto &[root, group] : groups) {
    // A cluster is a place only when at least one forming category of it meets its
    // own threshold. A single stall among trees is an aspect, not a place.
    std::map<std::string, int> forming;
    int extentRadius = 0;
    bool split = false;
    for (const std::size_t index : group) {
      for (const SeedCategory &seed : *seeds[index].categories) {
        const PlaceCategory *category = registry.Get(seed.category);
        if (category == nullptr || !category->formsPlace || seed.weight <= 0.0f) {
          continue;
        }
        ++forming[seed.category];
        if (category->radius > extentRadius) {
          extentRadius = category->radius;
        }
        if (category->split == "junctions") {
          split = true;
        }
      }
    }

    bool qualifies = false;
    for (const auto &[category, count] : forming) {
      const PlaceCategory *definition = registry.Get(category);
      if (definition != nullptr && count >= definition->minSeeds) {
        qualifies = true;
        break;
      }
    }
    if (!qualifies || extentRadius <= 0) {
      continue;
    }

    if (split) {
      // Roads: the stretches between junctions and endpoints are separate places.
      // Only tile seeds of the splitting category are cut; a road component is
      // insular, so it holds nothing else.
      std::vector<GamePosition> roadCells;
      for (const std::size_t index : group) {
        if (!seeds[index].object) {
          roadCells.push_back(seeds[index].pos);
        }
      }
      for (std::vector<GamePosition> &stretch :
           detail::SplitAtJunctions(roadCells)) {
        Candidate candidate;
        candidate.extent = std::move(stretch);
        candidates.push_back(std::move(candidate));
      }
      continue;
    }

    // Extent: the ground reachable from object seeds, plus every tile seed cell.
    // A lake is its water cells and nothing else; an orchard spreads over the
    // ground it stands on.
    Candidate candidate;
    std::vector<GamePosition> objectSeeds;
    for (const std::size_t index : group) {
      if (seeds[index].object) {
        objectSeeds.push_back(seeds[index].pos);
      }
    }
    if (!objectSeeds.empty()) {
      candidate.extent =
          detail::FloodExtent(memory, objectSeeds, extentRadius, structural);
    }
    std::unordered_set<std::uint64_t> inExtent;
    for (const GamePosition &cell : candidate.extent) {
      inExtent.insert(VisionQuery::CellKey(cell));
    }
    for (const std::size_t index : group) {
      if (seeds[index].object) {
        candidate.seedIds.push_back(seeds[index].entity);
        continue;
      }
      const std::uint64_t key = VisionQuery::CellKey(seeds[index].pos);
      if (inExtent.insert(key).second) {
        candidate.extent.push_back(seeds[index].pos);
      }
    }
    if (candidate.extent.empty()) {
      continue;
    }
    std::sort(candidate.extent.begin(), candidate.extent.end(),
              [](const GamePosition &a, const GamePosition &b) {
                if (a.y != b.y) return a.y < b.y;
                return a.x < b.x;
              });
    candidates.push_back(std::move(candidate));
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &a, const Candidate &b) {
              const GamePosition &first = a.extent.front();
              const GamePosition &other = b.extent.front();
              if (first.y != other.y) return first.y < other.y;
              return first.x < other.x;
            });

  std::vector<char> claimed(memory.places.size(), 0);
  std::vector<KnownPlace> places;
  places.reserve(candidates.size());

  for (Candidate &candidate : candidates) {
    std::unordered_set<std::uint64_t> candidateCells;
    candidateCells.reserve(candidate.extent.size() * 2);
    for (const GamePosition &cell : candidate.extent) {
      candidateCells.insert(VisionQuery::CellKey(cell));
    }

    // Identity by overlap: the existing place sharing the most cells keeps its id
    // and therefore its labels (section 5.2).
    std::size_t best = memory.places.size();
    std::size_t bestOverlap = 0;
    for (std::size_t i = 0; i < memory.places.size(); ++i) {
      if (claimed[i] != 0) {
        continue;
      }
      std::size_t overlap = 0;
      for (const std::uint64_t key : memory.places[i].memberCells) {
        if (candidateCells.count(key) != 0) {
          ++overlap;
        }
      }
      if (overlap > bestOverlap) {
        bestOverlap = overlap;
        best = i;
      }
    }

    KnownPlace place;
    if (best < memory.places.size()) {
      place = memory.places[best];  // keeps id, labels, firstSeenTick
      claimed[best] = 1;
    } else {
      place = KnownPlace{};
      place.id = memory.nextPlaceId++;
      place.firstSeenTick = tick;
      place.source = KnowledgeSource::Observed;
      place.confidence = 255;
    }

    place.origin = PlaceOrigin::Inferred;
    place.seedKind = SeedKind::Object;
    for (const std::uint64_t key : candidateCells) {
      const auto cell = memory.cells.find(key);
      if (cell != memory.cells.end() && !cell->second.categories.empty()) {
        place.seedKind = SeedKind::Tile;
      }
    }
    place.lastSeenTick = tick;
    place.memberCells.clear();
    place.memberCells.reserve(candidate.extent.size());
    for (const GamePosition &cell : candidate.extent) {
      place.memberCells.push_back(VisionQuery::CellKey(cell));
    }
    place.memberSeeds = candidate.seedIds;
    detail::RecomputeExtent(place, memory.cells);
    detail::BuildAspects(place, memory, registry);
    places.push_back(std::move(place));
  }

  // Which place owns a cell, for names and facts.
  std::unordered_map<std::uint64_t, int> placeOfCell;
  const auto indexPlaces = [&places, &placeOfCell]() {
    placeOfCell.clear();
    for (const KnownPlace &place : places) {
      for (const std::uint64_t key : place.memberCells) {
        placeOfCell.emplace(key, place.id);
      }
    }
  };
  indexPlaces();

  for (const PlaceNameSeed &seed : memory.nameSeeds) {
    const std::uint64_t anchorKey = VisionQuery::CellKey(seed.anchor);
    const auto owner = placeOfCell.find(anchorKey);

    if (owner != placeOfCell.end()) {
      for (KnownPlace &place : places) {
        if (place.id != owner->second) {
          continue;
        }
        if (std::find(place.labels.begin(), place.labels.end(), seed.label) ==
            place.labels.end()) {
          place.labels.push_back(seed.label);
        }
        break;
      }
      continue;
    }

    if (!seed.createsPlace) {
      continue;
    }
    if (memory.cells.count(anchorKey) == 0) {
      continue;  // a name is not an observation
    }

    const std::vector<GamePosition> extent = detail::FloodExtent(
        memory, {seed.anchor}, seed.radius, structural);
    if (extent.empty()) {
      continue;
    }

    KnownPlace place;
    bool reused = false;
    for (const KnownPlace &existing : memory.places) {
      if (existing.seedKind != SeedKind::Named) {
        continue;
      }
      if (std::find(existing.labels.begin(), existing.labels.end(), seed.label) ==
          existing.labels.end()) {
        continue;
      }
      place = existing;
      reused = true;
      break;
    }

    if (!reused) {
      place = KnownPlace{};
      place.id = memory.nextPlaceId++;
      place.firstSeenTick = tick;
      place.source = seed.source;
      place.confidence = 255;
    }
    place.origin = PlaceOrigin::Inferred;
    place.seedKind = SeedKind::Named;
    place.primaryAspect = "named";
    place.labels.clear();
    place.labels.push_back(seed.label);
    place.lastSeenTick = tick;
    place.memberCells.clear();
    place.memberCells.reserve(extent.size());
    for (const GamePosition &cell : extent) {
      place.memberCells.push_back(VisionQuery::CellKey(cell));
    }
    place.memberSeeds.clear();
    detail::RecomputeExtent(place, memory.cells);
    // A named area is ground like any other: its walls are what it is made of.
    // `primaryAspect` stays "named" because no category formed it -- the label is
    // its reason for existing.
    detail::BuildAspects(place, memory, registry);
    place.primaryAspect = "named";
    places.push_back(std::move(place));
    indexPlaces();
  }

  if (places.size() > static_cast<std::size_t>(DEFAULT_MEMORY_PLACES_MAX)) {
    std::stable_sort(places.begin(), places.end(),
                     [](const KnownPlace &a, const KnownPlace &b) {
                       return a.memberCells.size() > b.memberCells.size();
                     });
    places.resize(static_cast<std::size_t>(DEFAULT_MEMORY_PLACES_MAX));
  }

  std::sort(places.begin(), places.end(),
            [](const KnownPlace &a, const KnownPlace &b) { return a.id < b.id; });

  indexPlaces();
  for (KnownPlace &place : places) {
    detail::RecomputeFacts(memory, place, placeOfCell);
  }

  memory.places = std::move(places);
  memory.lastPlaceTick = tick;
}

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

namespace detail {

// One rectangle produced by the row merge, with the statistics both shapes and
// runs need. Shared so the overlay and the consolidation pass cannot disagree
// about what a rectangle is.
struct MergedRect {
  int x0 = 0;
  int y0 = 0;
  int width = 1;
  int height = 1;
  int firstSeenTick = 0;
  int lastSeenTick = 0;
  int visitCount = 0;
};

// Merges horizontal runs of equal key -- the caller has already grouped the cells
// -- then stacks vertically adjacent runs with an identical span.
inline std::vector<MergedRect>
MergeRowsToRects(const std::map<int, std::map<int, const MemoryCell *>> &byRow) {
  struct Growing {
    int x0 = 0;
    int x1 = 0;
    int bottomY = 0;
    int height = 1;
    int firstSeen = 0;
    int lastSeen = 0;
    int visits = 0;
  };

  std::map<std::pair<int, int>, Growing> growing;
  std::vector<MergedRect> rects;

  const auto finish = [&rects](const Growing &g) {
    MergedRect rect;
    rect.x0 = g.x0;
    rect.y0 = g.bottomY - g.height + 1;
    rect.width = g.x1 - g.x0 + 1;
    rect.height = g.height;
    rect.firstSeenTick = g.firstSeen;
    rect.lastSeenTick = g.lastSeen;
    rect.visitCount = g.visits;
    rects.push_back(rect);
  };

  for (const auto &[y, byColumn] : byRow) {
    std::vector<std::pair<std::pair<int, int>, Growing>> runs;
    bool haveRun = false;
    std::pair<int, int> span{0, 0};
    Growing meta;
    for (const auto &[x, cell] : byColumn) {
      if (haveRun && x == span.second + 1) {
        span.second = x;
        meta.firstSeen = std::min(meta.firstSeen, cell->observedTick);
        meta.lastSeen = std::max(meta.lastSeen, cell->observedTick);
        meta.visits = std::max(meta.visits, cell->visitCount);
        continue;
      }
      if (haveRun) {
        runs.push_back({span, meta});
      }
      span = {x, x};
      meta = Growing{};
      meta.firstSeen = cell->observedTick;
      meta.lastSeen = cell->observedTick;
      meta.visits = cell->visitCount;
      haveRun = true;
    }
    if (haveRun) {
      runs.push_back({span, meta});
    }

    std::map<std::pair<int, int>, Growing> next;
    for (const auto &[runSpan, runMeta] : runs) {
      const auto found = growing.find(runSpan);
      Growing rect;
      rect.x0 = runSpan.first;
      rect.x1 = runSpan.second;
      rect.bottomY = y;
      rect.height = found == growing.end() ? 1 : found->second.height + 1;
      rect.firstSeen = found == growing.end()
                           ? runMeta.firstSeen
                           : std::min(found->second.firstSeen, runMeta.firstSeen);
      rect.lastSeen = found == growing.end()
                          ? runMeta.lastSeen
                          : std::max(found->second.lastSeen, runMeta.lastSeen);
      rect.visits = std::max(
          found == growing.end() ? 0 : found->second.visits, runMeta.visits);
      next[runSpan] = rect;
    }

    for (const auto &[finishSpan, rect] : growing) {
      if (next.count(finishSpan) == 0) {
        finish(rect);
      }
    }
    growing = std::move(next);
  }

  for (const auto &[span, rect] : growing) {
    finish(rect);
  }
  return rects;
}

// The same merge as MergeRowsToRects, but over a group of cells already sorted by
// (y, x). One linear pass, no per-cell node allocation: this is what runs every
// sample over the whole store, where the map-of-maps version is measurable at the
// game's unoptimised build settings.
inline std::vector<MergedRect>
MergeSortedCellsToRects(const std::vector<const MemoryCell *> &cells) {
  std::vector<MergedRect> rects;
  if (cells.empty()) {
    return rects;
  }

  struct Active {
    int x0 = 0;
    int x1 = 0;
    int bottomY = 0;
    int height = 1;
    int firstSeen = 0;
    int lastSeen = 0;
    int visits = 0;
  };
  std::vector<Active> active;   // spans from the previous row, ascending x
  std::vector<Active> running;  // spans from the current row

  std::size_t index = 0;
  while (index < cells.size()) {
    const int y = cells[index]->pos.y;

    running.clear();
    while (index < cells.size() && cells[index]->pos.y == y) {
      Active run;
      run.x0 = run.x1 = cells[index]->pos.x;
      run.bottomY = y;
      run.firstSeen = run.lastSeen = cells[index]->observedTick;
      run.visits = cells[index]->visitCount;
      ++index;
      while (index < cells.size() && cells[index]->pos.y == y &&
             cells[index]->pos.x == run.x1 + 1) {
        run.x1 = cells[index]->pos.x;
        run.firstSeen = std::min(run.firstSeen, cells[index]->observedTick);
        run.lastSeen = std::max(run.lastSeen, cells[index]->observedTick);
        run.visits = std::max(run.visits, cells[index]->visitCount);
        ++index;
      }
      running.push_back(run);
    }

    std::vector<Active> next;
    next.reserve(running.size());
    for (const Active &run : running) {
      const Active *grown = nullptr;
      for (const Active &previous : active) {
        if (previous.x0 == run.x0 && previous.x1 == run.x1) {
          grown = &previous;
          break;
        }
      }
      Active rect = run;
      if (grown != nullptr) {
        rect.height = grown->height + 1;
        rect.firstSeen = std::min(grown->firstSeen, run.firstSeen);
        rect.lastSeen = std::max(grown->lastSeen, run.lastSeen);
        rect.visits = std::max(grown->visits, run.visits);
      }
      next.push_back(rect);
    }

    for (const Active &previous : active) {
      bool continued = false;
      for (const Active &candidate : next) {
        if (candidate.x0 == previous.x0 && candidate.x1 == previous.x1) {
          continued = true;
          break;
        }
      }
      if (continued) {
        continue;
      }
      MergedRect rect;
      rect.x0 = previous.x0;
      rect.y0 = previous.bottomY - previous.height + 1;
      rect.width = previous.x1 - previous.x0 + 1;
      rect.height = previous.height;
      rect.firstSeenTick = previous.firstSeen;
      rect.lastSeenTick = previous.lastSeen;
      rect.visitCount = previous.visits;
      rects.push_back(rect);
    }
    active = std::move(next);
  }

  for (const Active &previous : active) {
    MergedRect rect;
    rect.x0 = previous.x0;
    rect.y0 = previous.bottomY - previous.height + 1;
    rect.width = previous.x1 - previous.x0 + 1;
    rect.height = previous.height;
    rect.firstSeenTick = previous.firstSeen;
    rect.lastSeenTick = previous.lastSeen;
    rect.visitCount = previous.visits;
    rects.push_back(rect);
  }
  return rects;
}

}  // namespace detail

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
    for (const detail::MergedRect &rect : detail::MergeRowsToRects(byRow)) {
      MemoryShape shape;
      shape.surface = surface;
      shape.origin = {rect.x0, rect.y0};
      shape.width = rect.width;
      shape.height = rect.height;
      shape.cellCount = shape.width * shape.height;
      shape.observedTick = rect.lastSeenTick;
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

// The consolidation pass (docs/spatial-memory-design.md section 6.1).
//
// Two jobs, both deterministic and model-free:
//
//   1. Compress the cell store into runs: rectangles of one surface inside one
//      place. Runs shadow the cells; nothing is deleted by building them.
//   2. Enforce the cell cap by dropping whole runs that no place depends on,
//      lowest rank first. Ground a place claims is never dropped, so the ledger
//      can say "over budget" instead of breaking a place to meet a number.
//
// Ranks order a drop and nothing else: recency, plus what repeated visits are
// worth, plus a bonus for being place ground.
inline void Consolidate(SpatialMemory &memory, int tick) {
  std::unordered_map<std::uint64_t, int> placeOfCell;
  for (const KnownPlace &place : memory.places) {
    for (const std::uint64_t key : place.memberCells) {
      placeOfCell.emplace(key, place.id);
    }
  }

  // Group by (surface, place): a run never spans a place boundary, so "which
  // place is this part of" is always answerable without inspecting cells.
  //
  // Sorted vectors rather than a map of maps keyed by string: this runs over the
  // whole store every sample, and at the game's unoptimised build settings the
  // node allocation was the cost, not the merge.
  std::unordered_map<std::string, int> surfaceIds;
  std::vector<std::string> surfaceNames;
  struct Entry {
    int surface = 0;
    int place = -1;
    const MemoryCell *cell = nullptr;
  };
  std::vector<Entry> entries;
  entries.reserve(memory.cells.size());
  for (const auto &[key, cell] : memory.cells) {
    const auto known = surfaceIds.find(cell.surface);
    int surfaceId = 0;
    if (known == surfaceIds.end()) {
      surfaceId = static_cast<int>(surfaceNames.size());
      surfaceIds.emplace(cell.surface, surfaceId);
      surfaceNames.push_back(cell.surface);
    } else {
      surfaceId = known->second;
    }
    const auto owner = placeOfCell.find(key);
    entries.push_back(
        {surfaceId, owner == placeOfCell.end() ? -1 : owner->second, &cell});
  }
  std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
    if (a.place != b.place) return a.place < b.place;
    if (a.surface != b.surface) return a.surface < b.surface;
    if (a.cell->pos.y != b.cell->pos.y) return a.cell->pos.y < b.cell->pos.y;
    return a.cell->pos.x < b.cell->pos.x;
  });

  std::vector<MemoryRun> runs;
  runs.reserve(entries.size() / 4 + 1);
  std::vector<const MemoryCell *> block;
  for (std::size_t i = 0; i < entries.size();) {
    const int surfaceId = entries[i].surface;
    const int placeId = entries[i].place;
    block.clear();
    while (i < entries.size() && entries[i].surface == surfaceId &&
           entries[i].place == placeId) {
      block.push_back(entries[i].cell);
      ++i;
    }
    for (const detail::MergedRect &rect :
         detail::MergeSortedCellsToRects(block)) {
      MemoryRun run;
      run.surface = surfaceNames[surfaceId];
      run.placeId = placeId;
      run.origin = {rect.x0, rect.y0};
      run.width = rect.width;
      run.height = rect.height;
      run.firstSeenTick = rect.firstSeenTick;
      run.lastSeenTick = rect.lastSeenTick;
      run.visitCount = rect.visitCount;
      // Recency *minus* what visits are worth, the same sign as the per-cell
      // score: higher is better, and a run seen now outranks one walked over long
      // ago however often it was walked. Getting this sign wrong is not subtle --
      // it drops the ground the character is standing on.
      run.rank = static_cast<long long>(run.lastSeenTick) -
                 static_cast<long long>(run.visitCount) *
                     DEFAULT_MEMORY_VISIT_BONUS_TICKS +
                 (run.placeId >= 0 ? DEFAULT_MEMORY_PLACE_RANK_BONUS : 0);
      runs.push_back(std::move(run));
    }
  }

  // Deterministic order: place ground first, then by surface and position.
  std::sort(runs.begin(), runs.end(), [](const MemoryRun &a, const MemoryRun &b) {
    if (a.placeId != b.placeId) {
      if (a.placeId < 0) return false;
      if (b.placeId < 0) return true;
      return a.placeId < b.placeId;
    }
    if (a.surface != b.surface) return a.surface < b.surface;
    if (a.origin.y != b.origin.y) return a.origin.y < b.origin.y;
    return a.origin.x < b.origin.x;
  });

  ConsolidationRecord record;
  record.tick = tick;
  record.cellsBefore = static_cast<int>(memory.cells.size());
  record.runsOut = static_cast<int>(runs.size());

  const std::size_t cap = static_cast<std::size_t>(DEFAULT_MEMORY_CELLS_MAX);
  std::vector<char> dropped(runs.size(), 0);
  if (memory.cells.size() > cap) {
    // Candidate runs for a drop: stray ground only, worst rank first. Ties break
    // on position so the choice cannot depend on iteration order.
    std::vector<std::size_t> order;
    order.reserve(runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i) {
      if (runs[i].placeId < 0) {
        order.push_back(i);
      }
    }
    std::sort(order.begin(), order.end(),
              [&runs](std::size_t a, std::size_t b) {
                if (runs[a].rank != runs[b].rank) {
                  return runs[a].rank < runs[b].rank;
                }
                if (runs[a].origin.y != runs[b].origin.y) {
                  return runs[a].origin.y < runs[b].origin.y;
                }
                return runs[a].origin.x < runs[b].origin.x;
              });

    for (const std::size_t index : order) {
      if (memory.cells.size() <= cap) {
        break;
      }
      MemoryRun &run = runs[index];
      const int excess =
          static_cast<int>(memory.cells.size() - cap);
      const int runCells = run.width * run.height;

      // Whole run when it fits the excess. Otherwise only its top rows, so a
      // single vast field cannot free ten times the room that was needed -- the
      // ledger would be honest and the loss still absurd.
      const bool whole = runCells <= excess;
      const int rowsToDrop =
          whole ? run.height
                : std::max(1, std::min(run.height,
                                       (excess + run.width - 1) / run.width));

      for (int row = 0; row < rowsToDrop; ++row) {
        const int y = run.origin.y + row;
        for (int x = run.origin.x; x < run.origin.x + run.width; ++x) {
          const auto found = memory.cells.find(VisionQuery::CellKey({x, y}));
          if (found != memory.cells.end() &&
              found->second.surface == run.surface) {
            memory.cells.erase(found);
            ++record.freedCells;
          }
        }
      }

      if (whole) {
        dropped[index] = 1;
        ++record.droppedRuns;
      } else {
        // The run keeps its index entry, one rectangle shorter.
        run.origin.y += rowsToDrop;
        run.height -= rowsToDrop;
        ++record.partialDrops;
      }
    }
    record.overBudget = memory.cells.size() > cap;
  }

  // Dropped runs are gone from the index too; only the record remains.
  std::vector<MemoryRun> kept;
  kept.reserve(runs.size());
  for (std::size_t i = 0; i < runs.size(); ++i) {
    if (dropped[i] != 0) {
      continue;
    }
    kept.push_back(std::move(runs[i]));
  }
  memory.runs = std::move(kept);
  record.cellsAfter = static_cast<int>(memory.cells.size());

  memory.consolidationLog.push_back(record);
  if (memory.consolidationLog.size() >
      static_cast<std::size_t>(DEFAULT_CONSOLIDATION_LOG_MAX)) {
    memory.consolidationLog.erase(memory.consolidationLog.begin());
  }
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
      << ", \"runs\": " << memory.runs.size()
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

  // The compressed view and the ledger. Runs are an index over the cells above,
  // so the two sections together are the whole story of a consolidation pass.
  out << "  \"runs\": [";
  {
    bool first = true;
    for (const MemoryRun &run : memory.runs) {
      out << (first ? "\n" : ",\n");
      out << "    {\"surface\": " << Quote(run.surface) << ", \"origin\": ["
          << run.origin.x << ", " << run.origin.y << "], \"w\": " << run.width
          << ", \"h\": " << run.height << ", \"placeId\": " << run.placeId
          << ", \"firstSeenTick\": " << run.firstSeenTick
          << ", \"lastSeenTick\": " << run.lastSeenTick
          << ", \"visits\": " << run.visitCount << ", \"rank\": " << run.rank
          << "}";
      first = false;
    }
    out << "\n  ],\n";
  }

  out << "  \"consolidation\": [";
  {
    bool first = true;
    for (const ConsolidationRecord &record : memory.consolidationLog) {
      out << (first ? "\n" : ",\n");
      out << "    {\"tick\": " << record.tick
          << ", \"cellsBefore\": " << record.cellsBefore
          << ", \"cellsAfter\": " << record.cellsAfter
          << ", \"runsOut\": " << record.runsOut
          << ", \"droppedRuns\": " << record.droppedRuns
          << ", \"partialDrops\": " << record.partialDrops
          << ", \"freedCells\": " << record.freedCells
          << ", \"overBudget\": " << (record.overBudget ? "true" : "false")
          << "}";
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
          << ", \"portable\": " << (object->portable ? "true" : "false")
          << ", \"structural\": " << (object->structural ? "true" : "false")
          << ", \"categories\": [";
      {
        bool firstCategory = true;
        for (const SeedCategory &seed : object->categories) {
          out << (firstCategory ? "" : ", ");
          out << "{\"category\": " << Quote(seed.category)
              << ", \"weight\": " << seed.weight << "}";
          firstCategory = false;
        }
      }
      out << "]" 
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
      out << "    {\"id\": " << place->id
          << ", \"origin\": " << Quote(PlaceOriginName(place->origin))
          << ", \"source\": " << Quote(KnowledgeSourceName(place->source))
          << ", \"confidence\": " << static_cast<int>(place->confidence)
          << ", \"seedKind\": "
          << Quote(place->seedKind == SeedKind::Tile     ? "Tile"
                   : place->seedKind == SeedKind::Named  ? "Named"
                                                          : "Object")
          << ", \"primaryAspect\": " << Quote(place->primaryAspect)
          << ", \"anchor\": [" << place->anchor.x << ", " << place->anchor.y
          << "], \"w\": " << place->width << ", \"h\": " << place->height
          << ", \"labels\": [";
      {
        bool firstLabel = true;
        for (const std::string &label : place->labels) {
          out << (firstLabel ? "" : ", ") << Quote(label);
          firstLabel = false;
        }
      }
      out << "], \"aspects\": [";
      {
        bool firstAspect = true;
        for (const PlaceAspect &aspect : place->aspects) {
          out << (firstAspect ? "" : ", ");
          out << "{\"category\": " << Quote(aspect.category)
              << ", \"peak\": " << aspect.peak << ", \"peakPos\": ["
              << aspect.peakPos.x << ", " << aspect.peakPos.y
              << "], \"mean\": " << aspect.mean << "}";
          firstAspect = false;
        }
      }
      out << "], \"memberCells\": " << place->memberCells.size()
          << ", \"memberSeeds\": " << place->memberSeeds.size()
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
          << ", \"lastSeenTick\": " << place->lastSeenTick;
      if (includeCells) {
        out << ", \"aspectSources\": [";
        bool firstSource = true;
        for (const AspectSource &source : place->aspectSources) {
          out << (firstSource ? "" : ", ");
          out << "{\"category\": " << Quote(source.category)
              << ", \"pos\": [" << source.pos.x << ", " << source.pos.y
              << "], \"weight\": " << source.weight
              << ", \"radius\": " << source.radius << "}";
          firstSource = false;
        }
        out << "]";
      }
      out << "}";
      first = false;
    }
    out << "\n  ],\n";
  }

  out << "  \"nameSeeds\": [";
  {
    bool first = true;
    for (const PlaceNameSeed &seed : memory.nameSeeds) {
      out << (first ? "\n" : ",\n");
      out << "    {\"label\": " << Quote(seed.label) << ", \"anchor\": ["
          << seed.anchor.x << ", " << seed.anchor.y << "], \"source\": "
          << Quote(KnowledgeSourceName(seed.source))
          << ", \"createsPlace\": " << (seed.createsPlace ? "true" : "false")
          << ", \"radius\": " << seed.radius << "}";
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
