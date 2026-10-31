#pragma once

// Chunk geometry and per-agent handles (docs/chunk-design.md).
//
// Two things live here and they are deliberately separate:
//
//   * the world geometry: (cx, cy), bounds, size. A chunk exists whether or not
//     any agent has observed it. ChunkDebugId and ChunkAreaInWorld are
//     engine/debug only and must never reach the agent prompt.
//   * the per-agent handle: "area-N", allocated when the agent first observes
//     the chunk and stored in SpatialMemory. The number is discovery order, not
//     position, so it does not reveal the world origin or the map bounds.
//
// The agent-facing card and route strip are built from SpatialMemory, not from
// the map. Nothing in this file reads map contents or map dimensions for the
// agent view; ChunkAreaInWorld exists only for engine/debug use.

#include "Components.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

struct ChunkCoord {
  int cx = 0;
  int cy = 0;

  bool operator==(const ChunkCoord &other) const {
    return cx == other.cx && cy == other.cy;
  }
  bool operator!=(const ChunkCoord &other) const { return !(*this == other); }
};

struct ChunkBounds {
  GamePosition min;           // inclusive
  GamePosition maxExclusive;  // exclusive
};

namespace chunk_detail {

// Floor division for negative world coordinates; the shipped maps are
// non-negative, but the geometry should not silently truncate toward zero.
inline int FloorDiv(int value, int divisor) {
  const int quotient = value / divisor;
  const int remainder = value % divisor;
  if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
    return quotient - 1;
  }
  return quotient;
}

// 0 -> "A", 25 -> "Z", 26 -> "AA". Engine/debug IDs only.
inline std::string ColumnLetters(int cx) {
  if (cx < 0) {
    return "?";
  }
  std::string out;
  int value = cx + 1;
  while (value > 0) {
    const int remainder = (value - 1) % 26;
    out.insert(out.begin(), static_cast<char>('A' + remainder));
    value = (value - 1) / 26;
  }
  return out;
}

}  // namespace chunk_detail

inline ChunkCoord ChunkAt(GamePosition cell, int size) {
  if (size < 1) {
    size = 1;
  }
  return {chunk_detail::FloorDiv(cell.x, size),
          chunk_detail::FloorDiv(cell.y, size)};
}

inline ChunkBounds ChunkBoundsOf(ChunkCoord chunk, int size) {
  if (size < 1) {
    size = 1;
  }
  return {{chunk.cx * size, chunk.cy * size},
          {(chunk.cx + 1) * size, (chunk.cy + 1) * size}};
}

inline bool ChunkContains(ChunkCoord chunk, GamePosition cell, int size) {
  const ChunkBounds bounds = ChunkBoundsOf(chunk, size);
  return cell.x >= bounds.min.x && cell.x < bounds.maxExclusive.x &&
         cell.y >= bounds.min.y && cell.y < bounds.maxExclusive.y;
}

// Engine/debug only. The letter and number encode world position; never put
// this in the agent prompt (that is the A1/C3 leak the handle exists to avoid).
inline std::string ChunkDebugId(ChunkCoord chunk) {
  if (chunk.cx < 0 || chunk.cy < 0) {
    return "?";
  }
  return chunk_detail::ColumnLetters(chunk.cx) +
         std::to_string(chunk.cy + 1);
}

inline bool ParseChunkDebugId(std::string_view id, ChunkCoord &out) {
  std::size_t index = 0;
  int column = 0;
  bool anyColumn = false;
  while (index < id.size() &&
         std::isalpha(static_cast<unsigned char>(id[index])) != 0) {
    const char c = static_cast<char>(
        std::toupper(static_cast<unsigned char>(id[index])));
    if (c < 'A' || c > 'Z') {
      return false;
    }
    column = column * 26 + (c - 'A' + 1);
    anyColumn = true;
    ++index;
  }
  if (!anyColumn) {
    return false;
  }
  int row = 0;
  bool anyRow = false;
  while (index < id.size() &&
         std::isdigit(static_cast<unsigned char>(id[index])) != 0) {
    row = row * 10 + (id[index] - '0');
    anyRow = true;
    ++index;
  }
  if (!anyRow || index != id.size() || row < 1) {
    return false;
  }
  out = {column - 1, row - 1};
  return true;
}

// Engine/debug only: the part of the chunk that lies inside the map. The agent
// card deliberately reports an observed-cell count instead, so the map edge is
// not revealed to the agent.
inline int ChunkAreaInWorld(ChunkCoord chunk, int size, int mapWidth,
                            int mapHeight) {
  if (mapWidth <= 0 || mapHeight <= 0) {
    return 0;
  }
  const ChunkBounds bounds = ChunkBoundsOf(chunk, size);
  const int minX = std::max(bounds.min.x, 0);
  const int minY = std::max(bounds.min.y, 0);
  const int maxX = std::min(bounds.maxExclusive.x, mapWidth);
  const int maxY = std::min(bounds.maxExclusive.y, mapHeight);
  if (maxX <= minX || maxY <= minY) {
    return 0;
  }
  return (maxX - minX) * (maxY - minY);
}

inline std::uint64_t ChunkKey(ChunkCoord chunk) {
  return (static_cast<std::uint64_t>(
              static_cast<std::uint32_t>(chunk.cx))
          << 32) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.cy));
}

// The only mutation: assign the next per-agent handle if this chunk has none.
// Call it when the agent's chunk view first needs the chunk (normally on its
// first observed cell). The handle is stable for the memory's lifetime.
inline int EnsureChunkHandle(SpatialMemory &memory, ChunkCoord chunk) {
  const std::uint64_t key = ChunkKey(chunk);
  const auto found = memory.chunkHandles.find(key);
  if (found != memory.chunkHandles.end()) {
    return found->second;
  }
  const int handle = memory.nextChunkHandle++;
  memory.chunkHandles.emplace(key, handle);
  return handle;
}

inline int ChunkHandleFor(const SpatialMemory &memory, ChunkCoord chunk) {
  const auto found = memory.chunkHandles.find(ChunkKey(chunk));
  return found == memory.chunkHandles.end() ? 0 : found->second;
}

// The model-facing label. "area-3" says nothing about where the chunk is.
inline std::string ChunkHandleLabel(int handle) {
  if (handle <= 0) {
    return "area";
  }
  return "area-" + std::to_string(handle);
}

// Future naming hook: v1 has no chunkNames store, so this returns the handle.
// Keeping the display path here means names can be added later without
// rewriting the card and strip renderers.
inline std::string ChunkDisplayLabel(const SpatialMemory &, int handle) {
  return ChunkHandleLabel(handle);
}
