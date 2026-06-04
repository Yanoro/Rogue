#pragma once
#include "Components.h"
#include "RenderTexture.hpp"
#include "ResourceManager.h"
#include <flecs.h>
#include <nlohmann/json.hpp>
#include <string>

struct TileSet {
  std::string name;
  raylib::Texture *texture;
  unsigned int firstGID;
  unsigned int tileCount;
  unsigned int columns;
};


// TODO: For easy of development the map uses the ecs,
// but an easy speed up would make it into a static variable
// to avoid expensive lookups and make tile access constant
class Map {
public:
  Map(std::string jsonPath, flecs::world &ecs);

  // Coordinate helpers
  int GetWidth() const { return width; }
  int GetHeight() const { return height; }
  bool IsInBounds(float x, float y) const;

  ScreenPosition GameCoordsToScreenCoords(float x, float y) const;
  GamePosition ScreenCoordsToGameCoords(float x, float y) const;

  int GetTileWidth() const { return tileWidth; }
  int GetTileHeight() const { return tileHeight; }

  int GetMapWidthPx() const { return GetWidth() * GetTileWidth();}
  int GetMapHeightPx() const { return GetHeight() * GetTileHeight();}

  // Rendering
  // void Draw();

private:
  flecs::world &ecs;

  int width;
  int height;
  int tileWidth, tileHeight;

  // Utility for 1D indexing
  int GetIndex(int x, int y) const;
  TileSet *gidToTileSet(unsigned int gid);
};
