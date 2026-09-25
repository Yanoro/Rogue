#pragma once
#include "Components.h"
#include "StartingSkills.hpp"
#include <memory>
#include <flecs.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class DebugLog;

struct NPCData {
  std::string name;
  GamePosition position;
  std::string background;
  // Items this NPC is spawned already holding, from the map's
  // "inventory": [{"item": "wheat_seed", "count": 5}, ...]. Entries are
  // ObjectFactory template keys and each unit becomes its own entity, matching
  // how harvest drops and crafting outputs are held (see ItemStack).
  std::vector<ItemStack> inventory;
  // Proficiency this NPC starts with, from the map's
  // "skills": {"farming": {"stage": "Master", "level": 1}} object. The stage is
  // a name from the skill's own data and the level is the position within it;
  // createNPC derives the absolute level and stamps it over the untrained block
  // every character is given (see StartingSkills.hpp).
  std::vector<StartingSkill> startingSkills;
  // The people this NPC belongs to, from the map's "race": "human". Its slots
  // are the NPC's body when the NPC authors none of its own. Empty means the
  // human default, so a map written before races existed still works.
  std::string race;
  // Body slots authored directly on the NPC, from
  // "slots": ["head", {"id": "main_hand", "accepts": "hand"}]. Overrides the
  // race's slots entirely when present, which is what lets a one-off creature
  // have a body no race file describes.
  std::vector<SlotSpec> slots;
  // Object template ids spawned already worn, from
  // "equipped": ["iron_scythe"]. Each is spawned as a held item and equipped by
  // the same rules the [EQUIP] command uses, so authored gear cannot produce a
  // state a player could not.
  std::vector<std::string> equipped;
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

  // Registers a new area location and returns a pointer to it. The pointer is
  // owned by the map and stays valid as long as the map does.
  Location *AddLocation(const std::string& name, const std::string& description,
                        GamePosition topLeft, int width, int height);

  // Removes the location covering this tile and returns its name. Returns an
  // empty string when no location covers the position. In-memory only: the map
  // file is not touched by this.
  std::string RemoveLocationAt(GamePosition pos);

  // Builds the complete map as JSON, exactly as the map file format expects it.
  // Tile layout, locations and authored objects come from live state, so editor
  // edits are included; tileInfo and the NPC list are carried over from what was
  // loaded, so unknown keys and hand-written fields survive a save.
  //
  // The "positions" array is written in the order Map::Map consumes it, which is
  // COLUMN-major (index = x * height + y), not row-major. Emitting it row-major
  // silently transposes the map on the next load.
  nlohmann::json BuildMapJson() const;

  // Writes BuildMapJson() to a file. Returns false (and reports through the
  // injected DebugLog) when the file cannot be opened or written.
  bool SaveToFile(const std::string &path);

  Location *GetLocation(GamePosition pos);
  Location *GetLocation(const std::string& name);
  std::vector<std::string> GetAllLocationNames(); 
  const std::vector<std::unique_ptr<Location>>& GetLocations() const { return mapLocations; }
  const std::vector<std::unique_ptr<Tile>>& GetUniqueTiles() const { return uniqueTiles; }
  const std::vector<NPCData>& GetNPCs() const { return npcs; }

  // Every in-bounds tile inside [topLeft, topLeft + size), row major. Used by
  // the map editor to show exactly which tiles a dragged location covers.
  std::vector<GamePosition> GetTilePositionsInRect(GamePosition topLeft, int width,
                                                   int height) const;

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

  // The map file exactly as loaded. BuildMapJson starts from this and overwrites
  // only the four sections the editor owns (positions, locations, objects, npcs),
  // so tileInfo and any other key the loader ignores survive a save untouched.
  nlohmann::json rawMapJson;

  int width;
  int height;
  int tileWidth, tileHeight;

  // Utility for 1D indexing
  int GetIndex(int x, int y) const;

};
