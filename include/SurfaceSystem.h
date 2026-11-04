#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <flecs.h>

#include "Components.h"
#include "EffectSystem.h"

// One runtime surface on one map cell. The definition is registry data; this is
// only the state that can differ per instance: remaining duration, the source
// entity, stacks and the stay-tick clock.
struct SurfaceInstance {
  std::string defId;
  // Negative means permanent; otherwise seconds remaining.
  float remaining = -1.0f;
  float stayTimer = 0.0f;
  EffectOrigin origin;
  int stacks = 1;
};

// Sparse surface layer. Cells are keyed by map coordinates, never attached to the
// tile entity, so the tile definition and visual are untouched and several
// surfaces can share one cell.
class SurfaceGrid {
public:
  std::vector<SurfaceInstance> &At(GamePosition position) {
    return cells[{position.x, position.y}];
  }
  const std::vector<SurfaceInstance> *Find(GamePosition position) const {
    auto it = cells.find({position.x, position.y});
    return it == cells.end() ? nullptr : &it->second;
  }
  void Clear() { cells.clear(); }
  bool Empty() const { return cells.empty(); }
  size_t CellCount() const { return cells.size(); }

  std::map<std::pair<int, int>, std::vector<SurfaceInstance>> cells;
};

// A tile change waiting for the surface trigger pass. Effects are processed
// after all movement has settled, so a trigger cannot re-enter movement.
struct SurfaceTransition {
  flecs::entity actor = flecs::entity::null();
  GamePosition from{0, 0};
  GamePosition to{0, 0};
};
