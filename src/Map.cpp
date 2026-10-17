#include "Map.h"
#include "Defaults.h"
#include "Game.h"
#include "ResourceManager.h"
#include "SpritePaths.hpp"
#include "StartingSkills.hpp"
#include "TileRegistry.h"
#include "raylib.h"
#include "ObjectFactory.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_map>

inline void from_json(const nlohmann::json &j, Color &c) {
  if (j.is_array() && j.size() >= 3) {
    c.r = j[0];
    c.g = j[1];
    c.b = j[2];
    c.a = j.size() > 3 ? j[3].get<unsigned char>() : (unsigned char)255;
  }
}

Map::Map(flecs::entity mapEntity, std::string jsonPath, DebugLog* debugLog,
         ResourceManager* resources)
  : ecs(mapEntity.world()), debugLog(debugLog), resources(resources) {

  // The world's tile vocabulary, if this run has one. Absent is supported: the
  // map's own tileInfo then defines every tile it uses, which is how maps were
  // authored before the catalog existed. Looked up here rather than passed in so
  // that Map's constructor keeps its current signature.
  if (auto *tileRes = ecs.get<TileRegistryResource>()) {
    tileRegistry = tileRes->registry;
  }

  std::ifstream jsonFile;
  jsonFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
  try {
    jsonFile.open(jsonPath);
  } catch (const std::ifstream::failure &e) {
    std::string errStr = "Exception opening/reading file: " + std::string(e.what()) + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    throw;
  }


  try {
    auto json = nlohmann::json::parse(jsonFile);
    width = json["mapWidth"];
    height = json["mapHeight"];
    tileWidth = json["tileWidth"];
    tileHeight = json["tileHeight"];

    // Kept for saving: everything the editor does not own is written back from
    // here, so hand-written keys survive. See Map::BuildMapJson.
    rawMapJson = json;

    tileMap.resize(width * height);

    // The map's tile palette: one entry per uniqueTiles slot, in the order
    // "positions" indexes them. An entry is either a bare catalog name
    // ("grass") or an object naming a definition plus the fields this map
    // overrides ({ "name": "path", "character": ":" }). A map that overrides
    // nothing therefore writes only names, so a save still cannot re-duplicate
    // the shared vocabulary into the map file.
    //
    // Files written before "tilePalette" existed spell the same list
    // "tileInfo", positionally and with no bare-name shorthand. That spelling is
    // read here and never written again: carrying both is precisely the bug
    // BuildMapJson documents.
    const nlohmann::json palette =
        json.contains("tilePalette")
            ? json["tilePalette"]
            : json.value("tileInfo", nlohmann::json::array());

    auto appendTile = [&](const nlohmann::json &entry) {
      // A bare string is a catalog reference with nothing overridden. Everything
      // else is an override object, which is the only spelling that can carry
      // fields, so normalise the string case into one.
      nlohmann::json tileInfo;
      if (entry.is_string()) {
        tileInfo = nlohmann::json::object();
        tileInfo["name"] = entry.get<std::string>();
      } else {
        tileInfo = entry;
      }

      const std::string tileName = tileInfo.value("name", "Unknown name");
      const TileDef *definition =
          tileRegistry ? tileRegistry->Get(tileName) : nullptr;

      // Inherit, then let the map override field by field. `overridden` records
      // whether anything was replaced, which is what BuildMapJson uses to decide
      // whether this palette entry is written as an object or as a bare name.
      bool overridden = definition == nullptr;
      if (definition == nullptr && entry.is_string()) {
        // A named catalog tile that the catalog cannot resolve would draw
        // nothing at all. Same reasoning as Map::CreateTile's refusal: an
        // invisible tile is worse than a visible placeholder.
        const std::string errStr =
            "Map palette names tile '" + tileName +
            "' but no catalog definition was found; it draws as '?'.\n";
        if (debugLog) debugLog->LogWarning(errStr);
        else std::cerr << errStr;
      }
      TileDef resolved;
      if (definition != nullptr) {
        resolved = *definition;
      }

      if (tileInfo.contains("character")) {
        const std::string glyph = tileInfo["character"].get<std::string>();
        resolved.character = glyph.empty() ? '?' : glyph.at(0);
        resolved.hasCharacter = true;
        overridden = true;
      }
      if (tileInfo.contains("characterColor")) {
        resolved.characterColor = tileInfo["characterColor"];
        resolved.hasCharacterColor = true;
        overridden = true;
      }
      if (tileInfo.contains("backgroundColor")) {
        resolved.backgroundColor = tileInfo["backgroundColor"];
        resolved.hasBackgroundColor = true;
        overridden = true;
      }
      if (tileInfo.contains("blocksTile")) {
        resolved.blocksTile = tileInfo.value("blocksTile", resolved.blocksTile);
        overridden = true;
      }
      if (tileInfo.contains("sprite")) {
        resolved.sprite = tileInfo.value("sprite", std::string());
        overridden = true;
      }
      // The hitbox is scaled to this map's tile size, so authoring it is always
      // a per-map decision and never inherited by accident.
      if (tileInfo.contains("hitboxWidth") || tileInfo.contains("hitboxHeight")) {
        overridden = true;
      }

      auto newTile = std::make_unique<Tile>();
      // TODO: Memory leak here
      DrawAscii *newAscii = new DrawAscii();
      newAscii->ch = resolved.hasCharacter ? resolved.character : '?';
      newAscii->characterColor = resolved.characterColor;
      newAscii->backgroundColor = resolved.backgroundColor;
      newAscii->width = tileWidth;
      newAscii->height = tileHeight;

      newTile->name = tileName;
      newTile->blocksTile = resolved.blocksTile;
      newTile->ascii = newAscii;
      const size_t authoredHitbox = resolved.hasHitboxSize ? resolved.hitboxSize : 0;
      newTile->hitbox.width = tileInfo.value(
          "hitboxWidth", authoredHitbox ? authoredHitbox : tileWidth);
      newTile->hitbox.height = tileInfo.value(
          "hitboxHeight", authoredHitbox ? authoredHitbox : tileHeight);
      // Optional authored texture. Absent is normal: the convention path
      // data/sprites/tile_<name>.png is then tried, and the glyph is the last
      // resort, so a tile with no art at all still draws.
      newTile->sprite = resolved.sprite;

      uniqueTiles.push_back(std::move(newTile));
      tileOverridden.push_back(overridden);
      // Kept parallel to uniqueTiles and holding the entry *as authored*, so
      // BuildMapJson writes an override back with exactly the keys it came in
      // with instead of expanding it into a full definition.
      allTileInfo.push_back(std::move(tileInfo));
    };

    for (const auto &entry : palette) {
      appendTile(entry);
    }


    for (const auto &currLoc : json["locations"]) {
      auto location = std::make_unique<Location>();
      location->name = currLoc.value("name", "Unknown location name");
      location->description =
          currLoc.value("description", "Unknown location Description");
      auto locPos = currLoc["position"];
      location->pos = {locPos[0], locPos[1]};
      location->width = currLoc.value("hitboxWidth", currLoc.value("width", width));
      location->height = currLoc.value("hitboxHeight", currLoc.value("height", height));

      mapLocations.push_back(std::move(location));
    }

    if (json.contains("npcs")) {
      for (const auto &npcJson : json["npcs"]) {
        NPCData data;
        data.name = npcJson.value("name", "NPC");
        auto pos = npcJson["position"];
        data.position = {pos[0], pos[1]};
        data.background = npcJson.value("background", "");

        if (npcJson.contains("inventory")) {
          for (const auto &itemJson : npcJson["inventory"]) {
            ItemStack stack;
            stack.item = itemJson.value("item", "");
            stack.count = itemJson.value("count", 1);
            if (stack.item.empty() || stack.count <= 0) {
              std::string warnStr =
                  "Warning: NPC '" + data.name +
                  "' has an invalid inventory entry (empty item or non-positive "
                  "count); skipping it.\n";
              if (debugLog) debugLog->LogWarning(warnStr);
              else std::cerr << warnStr;
              continue;
            }
            data.inventory.push_back(stack);
          }
        }

        // Reporting helper for every authored field below, captured under its
        // own name because the constructor parameter `debugLog` shadows the
        // member and an unqualified read inside a lambda would not resolve.
        auto warn = [log = debugLog](const std::string &message) {
          if (log) log->LogWarning(message);
          else std::cerr << message;
        };

        // Authored starting proficiency. Map stays content-only: the stage name
        // is kept as written and resolved against the SkillRegistry when the NPC
        // is spawned, so an unknown skill is a spawn warning like a bad object
        // type rather than a map-load failure.
        if (npcJson.contains("skills")) {
          ParseStartingSkills(npcJson["skills"], "NPC '" + data.name + "'",
                              data.startingSkills, warn);
        }

        // The body. Like skills, kept as written and resolved against the
        // RaceRegistry when the NPC is spawned, so an unknown race or a
        // malformed slot is a spawn warning rather than a map-load failure.
        data.race = npcJson.value("race", "");

        if (npcJson.contains("slots")) {
          if (!npcJson["slots"].is_array()) {
            warn("Warning: NPC '" + data.name +
                 "' has a non-array 'slots'; ignoring it.\n");
          } else {
            for (const auto &slotJson : npcJson["slots"]) {
              SlotSpec slot;
              if (slotJson.is_string()) {
                // Shorthand for a slot whose kind is its own name, matching the
                // race files: "head" rather than {"id":"head","accepts":"head"}.
                slot.id = slotJson.get<std::string>();
                slot.accepts = slot.id;
              } else if (slotJson.is_object()) {
                slot.id = slotJson.value("id", "");
                slot.accepts = slotJson.value("accepts", slot.id);
              } else {
                warn("Warning: NPC '" + data.name +
                     "' has a slot that is neither a string nor an object; "
                     "skipping it.\n");
                continue;
              }
              if (slot.id.empty()) {
                warn("Warning: NPC '" + data.name +
                     "' has a slot with no id; skipping it.\n");
                continue;
              }
              if (slot.accepts.empty()) {
                slot.accepts = slot.id;
              }
              data.slots.push_back(std::move(slot));
            }
          }
        }

        // Starting equipment, by object template id. Spawned and equipped at
        // spawn time by the same rules [EQUIP] uses, so authored gear cannot
        // reach a state a player could not.
        if (npcJson.contains("equipped")) {
          if (!npcJson["equipped"].is_array()) {
            warn("Warning: NPC '" + data.name +
                 "' has a non-array 'equipped'; ignoring it.\n");
          } else {
            for (const auto &equippedJson : npcJson["equipped"]) {
              if (!equippedJson.is_string() ||
                  equippedJson.get<std::string>().empty()) {
                warn("Warning: NPC '" + data.name +
                     "' has an equipped entry that is not a non-empty template "
                     "id; skipping it.\n");
                continue;
              }
              data.equipped.push_back(equippedJson.get<std::string>());
            }
          }
        }

        // The NPC's own look, by image path. There is no convention path for
        // NPCs: a name-based lookup would break silently on a rename, so a
        // character that should not look like its race has to say so. An entry
        // that does not resolve is reported by AttachSprite at spawn and leaves
        // the race's look in place.
        if (npcJson.contains("sprite")) {
          if (npcJson["sprite"].is_string()) {
            data.sprite = npcJson["sprite"].get<std::string>();
          } else {
            warn("Warning: NPC '" + data.name +
                 "' has a non-string 'sprite'; ignoring it.\n");
          }
        }

        // Life goals, authored per character. Free text kept exactly as written:
        // there is no id to resolve, so unlike skills/race an unknown value
        // cannot fail at spawn. Seeded read-only into the agent's TaskList by
        // createNPC (docs/task-system-design.md section 5).
        if (npcJson.contains("goals")) {
          const auto &goalsJson = npcJson["goals"];
          if (!goalsJson.is_object() || !goalsJson.contains("terminal")) {
            warn("Warning: NPC '" + data.name +
                 "' has a 'goals' block with no 'terminal' array; ignoring it.\n");
          } else if (!goalsJson["terminal"].is_array()) {
            warn("Warning: NPC '" + data.name +
                 "' has a non-array 'goals.terminal'; ignoring it.\n");
          } else {
            for (const auto &goalJson : goalsJson["terminal"]) {
              // Kept exactly as written: free text with no id to resolve, so a
              // whitespace-only entry is the only thing worth rejecting here.
              // SeedTerminalGoals trims and drops those that slip through.
              if (!goalJson.is_string() ||
                  goalJson.get<std::string>().empty()) {
                warn("Warning: NPC '" + data.name +
                     "' has a terminal goal that is not a non-empty string; "
                     "skipping it.\n");
                continue;
              }
              data.terminalGoals.push_back(goalJson.get<std::string>());
            }
          }
        }

        npcs.push_back(data);
      }
    }

    GamePosition currPos = {0, 0};

    // A position is an index into the palette built above. An out-of-range one
    // means the file and its palette disagree -- a hand-edited map, or one saved
    // by a build that produced the two arrays from different sources. It used to
    // index past the end unchecked, which is undefined behaviour rather than a
    // wrong tile, so fall back to the first tile and say so once instead of once
    // per cell.
    bool reportedBadPosition = false;
    for (const int pos : json["positions"]) {
      // A negative index means "no tile here", which is how the editor writes a
      // cell it erased. That is a deliberate state rather than a disagreement
      // between the file and its palette, so it is honoured silently: the cell
      // keeps the null resize() gave it and gets no tile entity. The cursor
      // still advances, so one erased cell cannot shift the rest of the layout.
      if (pos < 0) {
        if (++currPos.y == height) {
          currPos.y = 0;
          currPos.x++;
        }
        continue;
      }

      Tile *currentTile = nullptr;
      if (pos >= 0 && static_cast<size_t>(pos) < uniqueTiles.size()) {
        currentTile = uniqueTiles[pos].get();
      } else if (!uniqueTiles.empty()) {
        currentTile = uniqueTiles.front().get();
        if (!reportedBadPosition) {
          reportedBadPosition = true;
          const std::string errStr =
              "Map \"positions\" holds index " + std::to_string(pos) +
              " but the palette has " + std::to_string(uniqueTiles.size()) +
              " entries; using the first tile for the affected cells.\n";
          if (debugLog) debugLog->LogWarning(errStr);
          else std::cerr << errStr;
        }
      }
      if (currentTile == nullptr) {
        // No palette at all, so nothing can be placed. Stop rather than walk the
        // rest of the layout building null cells.
        break;
      }
      addTileToMap(currentTile, currPos.x, currPos.y);

      flecs::entity tileEntity =
          ecs.entity()
              .child_of(mapEntity)
              .set<DrawAscii>(*currentTile->ascii)
              .set<ScreenPosition>(
                  GameCoordsToScreenCoords(currPos.x, currPos.y));
      // Tagged so the renderer draws every tile before anything standing on it.
      // A tile texture is opaque and full-bleed, so the order is load-bearing.
      // Added rather than set: MapTileVisual is an empty struct, and flecs
      // registers an empty type with size 0, which trips its
      // "cpp_type<T>::size() != 0" assertion the moment a value is set on it.
      // Every other tag in the codebase is added the same way.
      tileEntity.add<MapTileVisual>();
      if (resources != nullptr) {
        AttachSprite(tileEntity, currentTile->sprite,
                     TileSpriteId(currentTile->name), *resources, debugLog);
      }

      if (++currPos.y == height) {
        currPos.y = 0;
        currPos.x++;
      }
    }

    if (json.contains("objects")) {
      auto factoryRes = ecs.get<ObjectFactoryResource>();
      if (factoryRes && factoryRes->factory) {
        for (const auto &objData : json["objects"]) {
          std::string type = objData.value("type", "");
          int x = objData.value("x", 0);
          int y = objData.value("y", 0);
          if (!type.empty()) {
            // authored = true: these are the map's own objects, so the map
            // writer must write them back.
            flecs::entity obj = factoryRes->factory->SpawnObject(
                ecs, mapEntity, this, type, GamePosition{x, y}, true);

            // Optional starting contents, e.g. a chest that begins stocked with
            // flour. Same "inventory" shape NPCs use, so a container's contents
            // are authored in exactly one way. The units become Holds children
            // of the object and are seen by [TAKE]/[STORE]/[CRAFT] normally.
            if (obj.is_alive() && objData.contains("inventory")) {
              std::vector<ItemStack> inventory;
              for (const auto &itemJson : objData["inventory"]) {
                ItemStack stack;
                stack.item = itemJson.value("item", "");
                stack.count = itemJson.value("count", 1);
                if (stack.item.empty() || stack.count <= 0) {
                  std::string warnStr =
                      "Warning: object '" + type +
                      "' at (" + std::to_string(x) + ", " + std::to_string(y) +
                      ") has an invalid inventory entry (empty item or "
                      "non-positive count); skipping it.\n";
                  if (debugLog) debugLog->LogWarning(warnStr);
                  else std::cerr << warnStr;
                  continue;
                }
                inventory.push_back(stack);
              }
              factoryRes->factory->SpawnInventory(ecs, obj, inventory);
            }
          }
        }
      }
    }

  } catch (const std::exception &e) {
    std::string errStr = "JSON Parse Error: " + std::string(e.what()) + " jsonPath: " + jsonPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    throw;
  }
}

nlohmann::json Map::BuildMapJson() const {
  // Start from the file as loaded so section keys the editor does not manage and
  // any hand-written extras are carried through.
  nlohmann::json mapJson = rawMapJson.is_object() ? rawMapJson
                                                  : nlohmann::json::object();

  mapJson["mapWidth"] = width;
  mapJson["mapHeight"] = height;
  mapJson["tileWidth"] = tileWidth;
  mapJson["tileHeight"] = tileHeight;

  // The palette and the layout are two views of one container, uniqueTiles, and
  // a palette index IS the id "positions" stores -- so they cannot disagree.
  //
  // That invariant is the fix. This used to write "positions" by searching
  // uniqueTiles for each cell's pointer while writing "tileInfo" from a
  // *filtered* copy of a parallel array. A save therefore dropped every catalog
  // tile the map had not overridden, leaving the layout's indices addressing the
  // longer list, and the next load renumbered the map -- every wall became floor.
  // Writing both from uniqueTiles removes the possibility rather than repairing
  // an instance of it.
  //
  // An entry is a bare name when the tile is pure catalog and the authored
  // override object otherwise, so saving still cannot re-duplicate the shared
  // vocabulary and an override keeps exactly the keys it was authored with.
  nlohmann::json tilePalette = nlohmann::json::array();
  for (size_t i = 0; i < uniqueTiles.size(); ++i) {
    const bool overridden = i < tileOverridden.size() && tileOverridden[i];
    if (overridden && i < allTileInfo.size()) {
      tilePalette.push_back(allTileInfo[i]);
    } else {
      tilePalette.push_back(uniqueTiles[i]->name);
    }
  }
  mapJson["tilePalette"] = std::move(tilePalette);
  // "tileInfo" was the older positional spelling of the same list. Dropped
  // rather than written alongside the palette, so one map's terrain never has
  // two sources of truth in the file.
  mapJson.erase("tileInfo");

  // Tile layout. tileMap is indexed y*width+x but the file stores it
  // column-major, which is the order Map::Map consumes; see the header note.
  nlohmann::json positions = nlohmann::json::array();
  bool reportedUntrackedCell = false;
  for (int x = 0; x < width; ++x) {
    for (int y = 0; y < height; ++y) {
      const Tile *tile = tileMap[GetIndex(x, y)];
      // An erased cell. -1 is the spelling for "no tile here": it cannot collide
      // with a palette index, and the loader reads it back as a cell with no
      // tile entity. Without this the null fell through to the untracked-cell
      // branch below and a save resurrected tile 0 on every cell the editor
      // had cleared.
      if (tile == nullptr) {
        positions.push_back(-1);
        continue;
      }
      int tileId = 0;
      bool found = false;
      for (size_t i = 0; i < uniqueTiles.size(); ++i) {
        if (uniqueTiles[i].get() == tile) {
          tileId = static_cast<int>(i);
          found = true;
          break;
        }
      }
      // A cell holding a tile that is not in the palette has no correct index,
      // and writing 0 would silently claim it is a different tile. Say so once,
      // so the wrong answer is at least visible.
      if (!found && !reportedUntrackedCell) {
        reportedUntrackedCell = true;
        if (debugLog != nullptr) {
          debugLog->LogWarning(
              "Map save: cell (" + std::to_string(x) + "," + std::to_string(y) +
              ") holds a tile that is not in the palette; writing tile 0.\n");
        }
      }
      positions.push_back(tileId);
    }
  }
  mapJson["positions"] = std::move(positions);

  mapJson["locations"] = nlohmann::json::array();
  for (const auto &loc : mapLocations) {
    mapJson["locations"].push_back({
        {"name", loc->name},
        {"description", loc->description},
        {"position", {loc->pos.x, loc->pos.y}},
        {"hitboxWidth", loc->width},
        {"hitboxHeight", loc->height},
    });
  }

  // Only MapAuthored entities: entities the world spawned at runtime (harvest
  // drops, crafted items, NPC starting inventory) carry no marker and are left
  // out, so simulation state never leaks into the file. Positions are sorted so
  // the output depends on map content, not on ECS allocation order.
  struct AuthoredObject {
    std::string type;
    GamePosition pos;
  };
  std::vector<AuthoredObject> authoredObjects;
  ecs.each([&authoredObjects](flecs::entity, MapAuthored &authored,
                              const GamePosition &pos) {
    authoredObjects.push_back({authored.templateKey, pos});
  });
  std::sort(authoredObjects.begin(), authoredObjects.end(),
            [](const AuthoredObject &a, const AuthoredObject &b) {
              if (a.pos.y != b.pos.y) return a.pos.y < b.pos.y;
              if (a.pos.x != b.pos.x) return a.pos.x < b.pos.x;
              return a.type < b.type;
            });

  mapJson["objects"] = nlohmann::json::array();

  // Authored objects may declare starting contents (e.g. a stocked chest). The
  // live ECS knows only the object entity, not what the file asked it to start
  // with, so the original "inventory" is re-attached here by type+position.
  // Without this a save would silently empty every authored container.
  std::unordered_map<std::string, nlohmann::json> authoredInventories;
  if (rawMapJson.is_object() && rawMapJson.contains("objects") &&
      rawMapJson["objects"].is_array()) {
    for (const auto &rawObj : rawMapJson["objects"]) {
      if (!rawObj.is_object() || !rawObj.contains("inventory")) continue;
      authoredInventories[rawObj.value("type", std::string()) + "|" +
                           std::to_string(rawObj.value("x", 0)) + "|" +
                           std::to_string(rawObj.value("y", 0))] =
          rawObj["inventory"];
    }
  }

  for (const AuthoredObject &obj : authoredObjects) {
    nlohmann::json objJson = {
        {"type", obj.type}, {"x", obj.pos.x}, {"y", obj.pos.y}};
    auto authored = authoredInventories.find(obj.type + "|" +
                                             std::to_string(obj.pos.x) + "|" +
                                             std::to_string(obj.pos.y));
    if (authored != authoredInventories.end()) {
      objJson["inventory"] = authored->second;
    }
    mapJson["objects"].push_back(std::move(objJson));
  }

  // NPCs: the raw entries are already in the base document, so they are only
  // rebuilt when the loaded list no longer matches. Rebuilding unconditionally
  // from NPCData would drop "inventory", which NPCData does not model.
  if (!mapJson.contains("npcs") || !mapJson["npcs"].is_array() ||
      mapJson["npcs"].size() != npcs.size()) {
    mapJson["npcs"] = nlohmann::json::array();
    for (const NPCData &npc : npcs) {
      nlohmann::json npcJson = {
          {"name", npc.name},
          {"position", {npc.position.x, npc.position.y}},
          {"background", npc.background},
      };
      // Starting proficiency, unlike "inventory", is modelled by NPCData, so it
      // is rebuilt here rather than left to the raw document. Written back in
      // the authored stage/level shape, never as the derived absolute level, so
      // a save stays readable and a later levelsPerStage change cannot silently
      // reinterpret it.
      if (!npc.startingSkills.empty()) {
        nlohmann::json skills = nlohmann::json::object();
        for (const StartingSkill &skill : npc.startingSkills) {
          nlohmann::json entry = nlohmann::json::object();
          if (!skill.stage.empty()) {
            entry["stage"] = skill.stage;
          }
          entry["level"] = skill.levelInStage;
          skills[skill.id] = std::move(entry);
        }
        npcJson["skills"] = std::move(skills);
      }

      // Race, authored body and starting equipment are modelled by NPCData too,
      // so like "skills" they are rebuilt here rather than left to the raw
      // document.
      if (!npc.race.empty()) {
        npcJson["race"] = npc.race;
      }
      if (!npc.slots.empty()) {
        nlohmann::json slots = nlohmann::json::array();
        for (const SlotSpec &slot : npc.slots) {
          slots.push_back({{"id", slot.id}, {"accepts", slot.accepts}});
        }
        npcJson["slots"] = std::move(slots);
      }
      if (!npc.equipped.empty()) {
        npcJson["equipped"] = npc.equipped;
      }
      // Life goals are authored data and must round-trip (section 5), so like
      // "skills"/"race"/"equipped" they are rebuilt here when the list is
      // rebuilt rather than left to the raw document.
      if (!npc.terminalGoals.empty()) {
        nlohmann::json goals = nlohmann::json::object();
        goals["terminal"] = npc.terminalGoals;
        npcJson["goals"] = std::move(goals);
      }
      mapJson["npcs"].push_back(std::move(npcJson));
    }
  }

  return mapJson;
}

bool Map::SaveToFile(const std::string &path) {
  try {
    nlohmann::json mapJson = BuildMapJson();

    std::ofstream outFile(path);
    if (!outFile.is_open()) {
      std::string errStr = "Cannot open map file for writing: " + path + "\n";
      if (debugLog) debugLog->LogError(errStr);
      else std::cerr << errStr;
      return false;
    }

    outFile << mapJson.dump(2) << std::endl;
    outFile.close();

    if (debugLog) {
      debugLog->LogInfo("Map editor: wrote map to " + path + " (" +
                        std::to_string(width) + "x" + std::to_string(height) +
                        " tiles, " + std::to_string(mapLocations.size()) +
                        " locations, " +
                        std::to_string(mapJson["objects"].size()) + " objects)");
    }
    return true;
  } catch (const std::exception &e) {
    std::string errStr =
        "Failed to write map file " + path + ": " + std::string(e.what()) + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return false;
  }
}

void Map::addTileToMap(Tile *newTile, int x, int y) {
  tileMap[x + (y * width)] = newTile;
}

Location *Map::AddLocation(const std::string &name,
                           const std::string &description,
                           GamePosition topLeft, int width, int height) {
  auto location = std::make_unique<Location>();
  location->name = name;
  location->description = description;
  location->pos = topLeft;
  location->width = width;
  location->height = height;

  mapLocations.push_back(std::move(location));
  return mapLocations.back().get();
}

std::string Map::RemoveLocationAt(GamePosition pos) {
  for (auto it = mapLocations.begin(); it != mapLocations.end(); ++it) {
    const Location &loc = **it;
    // Same bounds test GetLocation uses, so removal hits exactly what a lookup
    // at this position would return.
    if (pos.x >= loc.pos.x && pos.x <= loc.pos.x + loc.width &&
        pos.y >= loc.pos.y && loc.height + loc.pos.y >= pos.y) {
      std::string removedName = loc.name;
      mapLocations.erase(it);
      return removedName;
    }
  }
  return "";
}

std::string Map::RemoveTileAt(GamePosition pos) {
  if (!IsInBounds(static_cast<float>(pos.x), static_cast<float>(pos.y))) {
    return "";
  }

  Tile *&cell = tileMap[GetIndex(pos.x, pos.y)];
  if (cell == nullptr) {
    return "";
  }

  const std::string removedName = cell->name;
  // Only the cell's pointer is cleared. The Tile stays in uniqueTiles because
  // that vector is the map's palette as well as its ownership: dropping the
  // entry would renumber every palette index "positions" still stores for the
  // other cells.
  cell = nullptr;
  return removedName;
}

std::vector<GamePosition> Map::GetTilePositionsInRect(GamePosition topLeft,
                                                      int rectWidth,
                                                      int rectHeight) const {
  std::vector<GamePosition> tiles;
  for (int x = topLeft.x; x < topLeft.x + rectWidth; ++x) {
    for (int y = topLeft.y; y < topLeft.y + rectHeight; ++y) {
      if (IsInBounds(static_cast<float>(x), static_cast<float>(y))) {
        tiles.push_back({x, y});
      }
    }
  }
  return tiles;
}

Location *Map::GetLocation(GamePosition pos) {
  for (const auto &currLoc : mapLocations) {
    if (pos.x >= currLoc->pos.x && pos.x <= currLoc->pos.x + currLoc->width &&
        pos.y >= currLoc->pos.y && currLoc->height + currLoc->pos.y >= pos.y) {
      return currLoc.get();
    }
  }
  return nullptr;
}

Location *Map::GetLocation(const std::string& name) {
  for (const auto &currLoc : mapLocations) {
    if (currLoc->name == name) {
      return currLoc.get();
    }
  }
  return nullptr;
}

std::vector<std::string> Map::GetAllLocationNames() {
  std::vector<std::string> locations;
  for (const auto &currLoc : mapLocations) {
    locations.push_back(currLoc.get()->name);
  }
  return locations;
}

Tile *Map::GetTile(int x, int y) {
  if (!IsInBounds(x, y)) {
    return nullptr;
  }
  return tileMap[GetIndex(x, y)];
}
int Map::GetIndex(int x, int y) const { return y * width + x; }

void Map::RefreshBlockingObjects() {
  blockingObjects.clear();
  ecs.filter<const Obstacle, const GamePosition>().each(
      [this](flecs::entity, const Obstacle &obstacle, const GamePosition &pos) {
        if (obstacle.blocksMovement && IsInBounds(pos.x, pos.y)) {
          blockingObjects.insert(GetIndex(pos.x, pos.y));
        }
      });
}

bool Map::IsBlocked(int x, int y) const {
  if (!IsInBounds(static_cast<float>(x), static_cast<float>(y))) {
    return true;
  }
  const int index = GetIndex(x, y);
  const Tile *tile = index >= 0 && index < static_cast<int>(tileMap.size())
                         ? tileMap[index]
                         : nullptr;
  if (tile == nullptr || tile->blocksTile) {
    return true;
  }
  return blockingObjects.find(index) != blockingObjects.end();
}

int Map::GetTileId(const Tile *tile) const {
  if (tile == nullptr) return -1;
  for (size_t i = 0; i < uniqueTiles.size(); ++i) {
    if (uniqueTiles[i].get() == tile) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

Tile *Map::CreateTile(const std::string &name) {
  const TileDef *definition = tileRegistry ? tileRegistry->Get(name) : nullptr;
  if (definition == nullptr) {
    const std::string errStr =
        "Cannot create tile '" + name +
        "': it is not in the tile catalog, so it would have no appearance.\n";
    if (debugLog) debugLog->LogWarning(errStr);
    else std::cerr << errStr;
    return nullptr;
  }

  auto newTile = std::make_unique<Tile>();
  // Same ownership shape as the loader: the DrawAscii is heap-allocated and the
  // Tile is owned by uniqueTiles, so this tile outlives every cell that points at
  // it and Map's destructor reclaims both.
  DrawAscii *newAscii = new DrawAscii();
  newAscii->ch = definition->hasCharacter ? definition->character : '?';
  newAscii->characterColor = definition->characterColor;
  newAscii->backgroundColor = definition->backgroundColor;
  newAscii->width = tileWidth;
  newAscii->height = tileHeight;

  newTile->name = definition->name;
  newTile->blocksTile = definition->blocksTile;
  newTile->ascii = newAscii;
  newTile->hitbox.width = definition->hasHitboxSize ? definition->hitboxSize : tileWidth;
  newTile->hitbox.height =
      definition->hasHitboxSize ? definition->hitboxSize : tileHeight;
  newTile->sprite = definition->sprite;

  Tile *raw = newTile.get();
  uniqueTiles.push_back(std::move(newTile));
  // A created tile is a catalog tile with nothing overridden, so it is written
  // as a bare name and resolves from the catalog again on the next load. It
  // needs no override entry to survive: the palette lists every uniqueTiles slot
  // whether or not it was overridden, which is why "positions" can address it.
  // (The entry below exists to keep allTileInfo parallel to uniqueTiles.)
  allTileInfo.push_back({{"name", definition->name}});
  tileOverridden.push_back(false);
  return raw;
}

std::vector<Tile *> Map::GetNeighbours(GamePosition p) {
  static const int dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};

  std::vector<Tile *> neighbors;
  for (int i = 0; i < 8; ++i) {
    neighbors.push_back(GetTile(p.x + dx[i], p.y + dy[i]));
  }
  return neighbors;
}

bool Map::AreNeighbours(GamePosition p1, GamePosition p2) {
  int dx = p1.x - p2.x;
  if (dx < 0) dx = -dx;
  
  int dy = p1.y - p2.y;
  if (dy < 0) dy = -dy;
  
  return (dx <= 1 && dy <= 1) && (dx == 1 || dy == 1);
}

bool Map::IsInBounds(float x, float y) const {
  return (x >= 0 && x < width && y >= 0 && y < height);
}

bool Map::InsideScreenMap(Rectangle rect) const {
  return (rect.x >= 0) && (rect.y >= 0) &&
         ((rect.x + rect.width) <= GetMapWidthPx()) &&
         ((rect.y + rect.height) <= GetMapHeightPx());
}

// TODO: I have no idea why both functions take floats
ScreenPosition Map::GameCoordsToScreenCoords(float x, float y) const {
  return ScreenPosition(std::floor(static_cast<int>(x * tileWidth)),
                        std::floor(static_cast<int>(y * tileHeight)));
}

GamePosition Map::ScreenCoordsToGameCoords(float x, float y) const {
  return GamePosition(std::floor(x / tileWidth), std::floor(y / tileHeight));
}


