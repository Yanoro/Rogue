#pragma once
#include "Components.h"
#include "StartingSkills.hpp"
#include <memory>
#include <flecs.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class DebugLog;
class ResourceManager;
class TileRegistry;

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
  // The NPC's own raster sprite, from "sprite": "data/sprites/farmer.png". This
  // is the only way a character gets a look of its own, because there is no
  // convention path for NPCs -- a name-based data/sprites/Pietro.png would break
  // silently the moment someone renamed the NPC. When it is empty or does not
  // load, the race supplies the look and then the ASCII art does (see
  // SpritePaths.hpp and GiveBody).
  std::string sprite;
};

class Map {
public:
  Map(flecs::entity mapEntity, std::string jsonPath, DebugLog* debugLog = nullptr,
      ResourceManager* resources = nullptr);

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
  // Tile layout, the tile palette, locations and authored objects come from live
  // state, so editor edits are included; the NPC list is carried over from what
  // was loaded, so unknown keys and hand-written fields survive a save.
  //
  // "tilePalette" and "positions" are written from the same container in the same
  // pass -- a palette index IS the id a cell stores -- because writing them from
  // two different sources is what used to renumber a map's terrain on reload.
  // The older positional "tileInfo" spelling is read on load and never written.
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

  // The index of a tile in uniqueTiles, or -1. This is the id the map file's
  // "positions" array stores -- an index into "tilePalette", which is uniqueTiles
  // written out name-wise -- and the id a caller needs when it wants to write a
  // *new* tile (a placed blueprint's wall, for instance) without owning one.
  int GetTileId(const Tile *tile) const;

  // Creates a tile on the map from a definition, and returns it. Used when
  // something needs a tile the map did not already have -- placing a blueprint
  // whose walls the map never used. Returns nullptr when the name is not in the
  // catalog, because inventing a tile with no appearance would paint an
  // invisible wall. A map with no catalog loaded cannot create tiles this way and
  // says so.
  Tile *CreateTile(const std::string &name);

private:
  // Declared in the order the constructor initialises them, so -Wreorder stays
  // quiet: ecs first (it is the world handle), then the two injected pointers.
  flecs::world ecs;
  DebugLog* debugLog;
  // Texture cache for tile sprites, handed in by Game rather than reached for.
  // Null is supported: tiles then keep their ASCII look.
  ResourceManager* resources = nullptr;
  // The world's tile vocabulary, resolved from TileRegistryResource in the
  // constructor. Null means this run has no catalog and every map tileInfo entry
  // is a full definition, which keeps old maps working unchanged. Not part of the
  // initialiser list: it is read out of the world the constructor just received.
  TileRegistry* tileRegistry = nullptr;

  // Ordered one entry per uniqueTiles slot: this IS the map's tile palette, and
  // the index of an entry is the id "positions" stores. Built by the loader from
  // "tilePalette" -- or, for a map written before that key existed, from the
  // positional "tileInfo".
  std::vector<std::unique_ptr<Tile>> uniqueTiles; 
  std::vector<std::unique_ptr<Location>> mapLocations; 
  std::vector<NPCData> npcs;

  std::vector<Tile *> tileMap;

  // The map file exactly as loaded. BuildMapJson starts from this and overwrites
  // only the sections the editor owns (tilePalette, positions, locations,
  // objects, npcs), so any key the loader ignores survives a save untouched.
  // "tileInfo" is the exception and is erased: the palette supersedes it, and a
  // file must not carry two spellings of one map's terrain.
  nlohmann::json rawMapJson;

  // The palette entries exactly as loaded, one per uniqueTiles entry. Kept so
  // BuildMapJson can write back the *authored* fields of each override rather
  // than reconstructing them from the live Tile, which would silently expand a
  // one-key override into a full definition and re-duplicate the catalog into
  // the map file on every save.
  nlohmann::json allTileInfo = nlohmann::json::array();
  // Whether each uniqueTiles entry differs from the catalog. It decides how the
  // entry is spelled in the palette -- the override object when true, a bare
  // catalog name when false -- and never whether the tile is written at all.
  std::vector<bool> tileOverridden;

  int width;
  int height;
  int tileWidth, tileHeight;

  // Utility for 1D indexing
  int GetIndex(int x, int y) const;

};
