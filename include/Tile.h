#pragma once

enum class TileType {
  Floor,
  Wall,
  Void
};

// TODO: Currently each tile has a pointer to a texture, that could probably be optmized
struct Tile {
  TileType type;
  unsigned int gid;

  bool isWalkable;
  bool isTransparent;
  bool isExplored;

  // Helper to create common tiles
  static Tile Floor(unsigned int gid) { return { TileType::Floor, gid, true, true, false }; }
  static Tile Wall(unsigned int gid) { return { TileType::Wall, gid, false, false, false }; }
  static Tile Void(unsigned int gid) { return { TileType::Void, gid, false, true, false }; }
};
