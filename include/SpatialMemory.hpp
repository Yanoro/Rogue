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

#include <algorithm>
#include <cstdint>
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

// Records the cells just seen. A cell already known is updated in place: the
// memory is a set of observations, not a log of sightings (section 4.4).
inline void ObserveCells(SpatialMemory &memory, Map &map,
                         const std::vector<GamePosition> &visible, int tick) {
  for (const GamePosition &cell : visible) {
    const std::uint64_t key = VisionQuery::CellKey(cell);
    auto known = memory.cells.find(key);
    if (known != memory.cells.end()) {
      // In place: the same observation, refreshed. This is the whole difference
      // between a memory and a transcript.
      known->second.observedTick = tick;
      ++known->second.visitCount;
      continue;
    }

    // A null tile is a cell the map erased rather than painted; there is nothing
    // to remember about it, and recording a blank surface would be a lie.
    Tile *tile = map.GetTile(cell.x, cell.y);
    if (tile == nullptr) {
      continue;
    }

    MemoryCell record;
    record.pos = cell;
    record.surface = tile->name;
    record.walkable = !tile->blocksTile;
    record.observedTick = tick;
    record.visitCount = 1;
    memory.cells.emplace(key, std::move(record));
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

// The whole record as text, newest first. This is the only serialization the
// memory has (section 7.3): it exists so two runs can be diffed, which the window
// cannot do.
inline std::string Dump(const SpatialMemory &memory, const std::string &name,
                        int nowTick) {
  std::ostringstream out;
  out << "Spatial memory -- " << name << "\n";
  out << "tick " << nowTick << " | cells " << memory.cells.size() << "/"
      << DEFAULT_MEMORY_CELLS_MAX << " | objects " << memory.objects.size()
      << "/" << DEFAULT_MEMORY_OBJECTS_MAX << " | places "
      << memory.places.size() << "\n";

  out << "origin: " << memory.origin.anchor << " attribute "
      << (memory.origin.attribute.empty() ? "(none)" : memory.origin.attribute)
      << " first seen " << memory.origin.firstSeenTick << "\n\n";

  std::vector<const MemoryCell *> cells;
  cells.reserve(memory.cells.size());
  for (const auto &[key, cell] : memory.cells) {
    cells.push_back(&cell);
  }
  std::sort(cells.begin(), cells.end(),
            [](const MemoryCell *a, const MemoryCell *b) {
              if (a->observedTick != b->observedTick) {
                return a->observedTick > b->observedTick;
              }
              return a->visitCount > b->visitCount;
            });

  out << "CELLS (" << cells.size() << ", newest first)\n";
  for (const MemoryCell *cell : cells) {
    out << "  tick " << cell->observedTick << "  " << cell->pos << "  "
        << (cell->surface.empty() ? "(unknown)" : cell->surface);
    if (!cell->walkable) {
      out << "  [blocks movement]";
    }
    if (cell->visitCount > 1) {
      out << "  seen " << cell->visitCount << "x";
    }
    out << "\n";
  }
  out << "\n";

  std::vector<const SeenObject *> objects;
  objects.reserve(memory.objects.size());
  for (const auto &[id, object] : memory.objects) {
    objects.push_back(&object);
  }
  std::sort(objects.begin(), objects.end(),
            [](const SeenObject *a, const SeenObject *b) {
              return a->lastSeenTick > b->lastSeenTick;
            });

  out << "OBJECTS (" << objects.size() << ", newest first)\n";
  for (const SeenObject *object : objects) {
    out << "  tick " << object->lastSeenTick << "  "
        << (object->templateId.empty() ? "(no template)"
                                       : object->templateId)
        << "  \"" << object->name << "\"  " << object->pos;
    if (object->character) {
      out << "  [character]";
    }
    out << "\n";
  }
  out << "\n";

  out << "PLACES (" << memory.places.size() << ")\n";
  for (const KnownPlace &place : memory.places) {
    out << "  " << place.anchor << " " << place.width << "x" << place.height
        << "  label \"" << place.label << "\"";
    if (!place.attribute.empty()) {
      out << "  attribute " << place.attribute;
    }
    for (const PlaceFact &fact : place.facts) {
      out << "  " << fact.category << "=" << fact.count;
    }
    out << "\n";
  }

  return out.str();
}

}  // namespace Memory
