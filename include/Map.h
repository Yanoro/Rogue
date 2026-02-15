#pragma once
#include <vector>
#include <nlohmann/json.hpp>
#include <string>
#include <fstream>
#include <iostream>
#include "ResourceManager.h"
#include "Tile.h"
#include "raylib-cpp.hpp"

struct TileSet {
  std::string name;
  std::unique_ptr<raylib::Texture> texture;
  unsigned int firstGID;
  unsigned int tileCount;
  unsigned int columns;
  unsigned int tileWidth;
  unsigned int tileHeight;
};

class Map {
public:
  Map(std::string jsonPath);

  //Tile& GetTile(int x, int y);
  void SetTile(int x, int y, Tile tile);

  // Coordinate helpers
  int GetWidth() const { return width; }
  int GetHeight() const { return height; }
  bool IsInBounds(int x, int y) const;

  // Logic
  //void GenerateBasic(int seed);

  // Rendering
  void Draw();

private:
  int width;
  int height;
  std::vector<Tile> tiles;
  std::vector<TileSet> tileSets;
  
  std::vector <unsigned int> mapData; // Stores the gids in the order meant to be drawn
  std::unordered_map<int, TileSet *> gidToTileSetMap; 

  // Utility for 1D indexing
  int GetIndex(int x, int y) const;
  TileSet *gidToTileSet(unsigned int gid);

};
