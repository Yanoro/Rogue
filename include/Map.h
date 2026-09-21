#pragma once
#include "Components.h"
#include <memory>
#include <flecs.h>
#include <nlohmann/json.hpp>
#include <string>

class DebugLog;

struct NPCData {
  std::string name;
  GamePosition position;
  std::string background;
};

class Map {
public:
  Map(flecs::entity mapEntity, std::string jsonPath, DebugLog* debugLog = nullptr);

  // Coordinate helpers
  int GetWidth() const { return width; }
  int GetHeight() const { return height; }
  bool IsInBounds(float x, float y) const;
  bool InsideScreenMap(Rectangle rect) const;

  ScreenPosition GameCoordsToScreenCoords(float x, float y) const;
  GamePosition ScreenCoordsToGameCoords(float x, float y) const;

  int GetTileWidth() const { return tileWidth; }
  int GetTileHeight() const { return tileHeight; }

  int GetMapWidthPx() const { return GetWidth() * GetTileWidth();}
  int GetMapHeightPx() const { return GetHeight() * GetTileHeight();}

  Location *GetLocation(GamePosition pos);
  Location *GetLocation(const std::string& name);
  std::vector<std::string> GetAllLocationNames(); 
  const std::vector<std::unique_ptr<Location>>& GetLocations() const { return mapLocations; }
  const std::vector<std::unique_ptr<Tile>>& GetUniqueTiles() const { return uniqueTiles; }
  const std::vector<NPCData>& GetNPCs() const { return npcs; }

  void addTileToMap(Tile *newTile, int x, int y);
  Tile *GetTile(int x, int y); 
  std::vector<Tile*> GetNeighbours(GamePosition p); 
  static bool AreNeighbours(GamePosition p1, GamePosition p2);

private:
  DebugLog* debugLog;
  flecs::world ecs;

  std::vector<std::unique_ptr<Tile>> uniqueTiles; 
  std::vector<std::unique_ptr<Location>> mapLocations; 
  std::vector<NPCData> npcs;

  std::vector<Tile *> tileMap;

  int width;
  int height;
  int tileWidth, tileHeight;

  // Utility for 1D indexing
  int GetIndex(int x, int y) const;

};
