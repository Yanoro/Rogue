#pragma once
#include <nlohmann/json.hpp>
#include <flecs.h>
#include <string>
#include "ResourceManager.h"
#include "Components.h"

struct TileSet {
  std::string name;
  raylib::Texture *texture;
  unsigned int firstGID;
  unsigned int tileCount;
  unsigned int columns;
};

class Map {
public:
  Map(std::string jsonPath, ResourceManager &rm, flecs::world &ecs);

  // Coordinate helpers
  int GetWidth() const { return width; }
  int GetHeight() const { return height; }
  bool IsInBounds(float x, float y) const;

  ScreenPosition GameCoordsToScreenCoords(float x, float y) const;
  GamePosition ScreenCoordsToGameCoords(float x, float y) const;

  int getTileWidth() const { return tileWidth; }
  int getTileHeight() const { return tileHeight; } 

  // Rendering
  //void Draw();

private:
  ResourceManager &resourceManager;
  flecs::world &ecs;

  int width;
  int height;
  int tileWidth, tileHeight;

  // Utility for 1D indexing
  int GetIndex(int x, int y) const;
  TileSet *gidToTileSet(unsigned int gid);

};
