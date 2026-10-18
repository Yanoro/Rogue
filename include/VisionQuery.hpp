#pragma once

// The one implementation of "what can this character see"
// (docs/spatial-memory-design.md section 4.2).
//
// One function, many consumers: the sampler caches its result, the debug overlay
// draws that cache, and stage 2 will write memory from the same set. They must not
// grow their own copies of the geometry -- the same argument
// docs/placement-design.md makes for the placement gate and the executor sharing
// one validator. An observation and an advertisement that drift apart are worse
// than either alone.
//
// Sight is blocked by *objects that carry BlocksVision*, never by terrain. A tile
// that blocks movement says nothing about what can be seen across it: a river is
// impassable and transparent. See the component for the whole rule.

#include "Components.h"
#include "Map.h"

#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace VisionQuery {

// A cell's identity in the per-sample set of opaque cells. Packed into one
// integer rather than kept as a pair, because a sweep probes this set once per
// line-of-sight step and a tuple would be built per probe for no benefit.
inline std::uint64_t CellKey(GamePosition pos) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.x)) << 32) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.y));
}

// The inverse of CellKey. Needed because a place's membership outlives the cell
// detail store: the cell cap can evict a tile while the object standing on it is
// still remembered, and the coordinate is in the key even then.
inline GamePosition CellFromKey(std::uint64_t key) {
  GamePosition pos;
  pos.x = static_cast<int>(key >> 32);
  pos.y = static_cast<int>(key & 0xFFFFFFFFu);
  return pos;
}

// True when any cell *strictly between* `from` and `to` on the Bresenham line is
// opaque. Both endpoints are excluded on purpose, and each exclusion is a rule:
//
//   * the origin, because standing on a spot must not blind you;
//   * the destination, because a character can see the barrel that hides what is
//     behind it. Excluding it is also what makes "the wall blocks" mean "you
//     cannot see the tile past the wall" rather than "you cannot see the wall".
//
// Bresenham is copied in spirit from src/BlueprintExpansion.cpp's InsertLine. It
// is a debug-grade ray, not a shadow caster: it is obviously correct, which is
// what stage 1 needs, and recursive shadowcasting is the upgrade if it ever
// profiles badly (section 4.3).
inline bool PathIsOpaque(const std::unordered_set<std::uint64_t> &opaque,
                         GamePosition from, GamePosition to) {
  if (opaque.empty()) {
    return false;
  }

  int x = from.x;
  int y = from.y;
  const int dx = std::abs(to.x - from.x);
  const int dy = -std::abs(to.y - from.y);
  const int stepX = from.x < to.x ? 1 : -1;
  const int stepY = from.y < to.y ? 1 : -1;
  int err = dx + dy;

  for (;;) {
    const bool isEndpoint =
        (x == from.x && y == from.y) || (x == to.x && y == to.y);
    if (!isEndpoint && opaque.count(CellKey({x, y})) != 0) {
      return true;
    }
    if (x == to.x && y == to.y) {
      break;
    }
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x += stepX;
    }
    if (e2 <= dx) {
      err += dx;
      y += stepY;
    }
  }
  return false;
}

// Every cell `origin` can see, appended to `out` in no particular order, starting
// with the origin itself.
//
// A cell is visible when it is inside the radar ring, or inside the cone, and in
// either case has a clear line to the origin. `facing` is normalized here rather
// than trusted, so a caller that passes an unnormalized vector gets the same
// answer instead of a silently widened cone.
inline void VisibleCells(const Map &map, GamePosition origin,
                         raylib::Vector2 facing, float range,
                         float angleDegrees, int radarRadius,
                         const std::unordered_set<std::uint64_t> &opaque,
                         std::vector<GamePosition> &out) {
  out.clear();
  out.push_back(origin);

  const float rangeSq = range * range;
  const float radarSq = static_cast<float>(radarRadius * radarRadius);

  // A cell whose centre sits exactly on the cone's edge counts as inside, so the
  // four diagonals of a 90-degree cone are not lost to floating-point rounding.
  const float cosLimit = std::cos(angleDegrees * 0.5f * 0.01745329252f) - 0.0001f;

  const float facingLength =
      std::sqrt(facing.x * facing.x + facing.y * facing.y);
  const int reach = static_cast<int>(std::ceil(range));

  for (int dy = -reach; dy <= reach; ++dy) {
    for (int dx = -reach; dx <= reach; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      const float distSq = static_cast<float>(dx * dx + dy * dy);
      if (distSq > rangeSq) {
        continue;
      }

      // Outside the radar ring the cone decides; inside it, everything is seen.
      // The radar exists because a cone tested against cell centres is lumpy at
      // one to three tiles, which is exactly the range where someone walking into
      // a room must not miss the barrel beside the door.
      if (distSq > radarSq) {
        if (facingLength <= 0.0f) {
          continue;  // No direction: the radar is all there is to see with.
        }
        const float dist = std::sqrt(distSq);
        const float alignment =
            (dx * facing.x + dy * facing.y) / (dist * facingLength);
        if (alignment < cosLimit) {
          continue;
        }
      }

      const GamePosition cell{origin.x + dx, origin.y + dy};
      if (!map.IsInBounds(static_cast<float>(cell.x),
                          static_cast<float>(cell.y))) {
        continue;
      }
      if (PathIsOpaque(opaque, origin, cell)) {
        continue;
      }
      out.push_back(cell);
    }
  }
}

}  // namespace VisionQuery
