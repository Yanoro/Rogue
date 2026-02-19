#pragma once
#include <vector>
#include <nlohmann/json.hpp>
#include <string>
#include <tuple>
#include <fstream>
#include <iostream>
#include "ResourceManager.h"
#include "Tile.h"
#include "raylib-cpp.hpp"

struct TileSet {
  std::string name;
  raylib::Texture *texture;
  unsigned int firstGID;
  unsigned int tileCount;
  unsigned int columns;
};

using ScreenCoords = std::pair<int, int>;

class Map {
public:
  Map(std::string jsonPath, ResourceManager &rm);

  //Tile& GetTile(int x, int y);
  void SetTile(int x, int y, Tile tile);

  // Coordinate helpers
  int GetWidth() const { return width; }
  int GetHeight() const { return height; }
  bool IsInBounds(int x, int y) const;

  ScreenCoords MapCoordsToScreenCoords(int x, int y) const;

  // Logic
  //void GenerateBasic(int seed);

  // Rendering
  void Draw();

private:
  ResourceManager &resourceManager;
  int width;
  int height;
  int tileWidth, tileHeight;
  std::vector<Tile> tiles;
  std::vector<TileSet> tileSets;
  
  std::vector <unsigned int> mapData; // Stores the gids in the order meant to be drawn
  std::unordered_map<int, TileSet *> gidToTileSetMap; 

  int getTileWidth() const { return tileWidth; }
  int getTileHeight() const { return tileHeight; }

  // Utility for 1D indexing
  int GetIndex(int x, int y) const;
  TileSet *gidToTileSet(unsigned int gid);

};
