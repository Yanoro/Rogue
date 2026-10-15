#include "TileRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "DebugLog.h"

namespace {

// Same shape as Map.cpp's local from_json for Color: [r, g, b] or [r, g, b, a]
// with alpha defaulting to opaque. Duplicated rather than shared because Map.cpp
// keeps it in an anonymous namespace and a two-line parser is not worth a common
// header -- but if a third caller appears, this becomes ColorParsing.h.
bool ParseColor(const nlohmann::json& j, Color& out) {
  if (!j.is_array() || j.size() < 3) return false;
  out.r = j[0];
  out.g = j[1];
  out.b = j[2];
  out.a = j.size() > 3 ? j[3].get<unsigned char>() : static_cast<unsigned char>(255);
  return true;
}

} // namespace

void TileRegistry::LoadTiles(const std::string& directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    // Not an error. A map that declares every tile it uses inline is exactly how
    // every map worked before this class existed, so a run without a catalog is
    // a supported configuration, not a broken one.
    const std::string warnStr =
        "Warning: Tile catalog directory not found: " + directoryPath +
        " (maps must declare their own tileInfo)\n";
    if (debugLog) debugLog->LogWarning(warnStr);
    else std::cerr << warnStr;
    return;
  }

  auto warn = [this](const std::string& message) {
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };

  for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() != ".json") continue;

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    try {
      nlohmann::json j;
      file >> j;

      TileDef def;
      // Keyed by the tile's NAME, not the file stem, and this is the opposite of
      // every other registry on purpose. For recipes, skills and items the id is
      // the filename and that id is what content references ("iron_scythe").
      // A tile has no such id: what references it is a human-readable name, from
      // a map's tileInfo, a blueprint's "paint", or a material palette. So
      // data/tiles/wooden_wall.json defines "wooden wall", and Get("wooden wall")
      // has to find it.
      //
      // Keying on the stem instead made every multi-word tile unresolvable --
      // "wooden wall" is not "wooden_wall" -- which surfaced as blueprints
      // reporting that wooden walls and floors were missing from the catalog.
      const std::string stem = entry.path().stem().string();
      def.name = j.value("name", stem);
      if (def.name.empty()) {
        warn("Warning: Tile definition " + entry.path().string() +
             " has an empty 'name'; skipping it.");
        continue;
      }
      const auto existing = tiles.find(def.name);
      if (existing != tiles.end()) {
        warn("Warning: Tile '" + def.name + "' is defined by both " +
             existing->second.sourcePath + " and " + entry.path().string() +
             "; keeping the first.");
        continue;
      }
      def.sourcePath = entry.path().string();
      def.blocksTile = j.value("blocksTile", false);
      def.sprite = j.value("sprite", "");

      if (j.contains("hitboxSize")) {
        def.hitboxSize = j["hitboxSize"].get<size_t>();
        def.hasHitboxSize = true;
      }

      if (j.contains("character")) {
        const std::string glyph = j["character"].get<std::string>();
        if (glyph.empty()) {
          warn("Warning: Tile '" + def.name +
               "' has an empty 'character'; using '?'.");
          def.character = '?';
        } else {
          def.character = glyph.at(0);
        }
        def.hasCharacter = true;
      }

      if (j.contains("characterColor")) {
        if (!ParseColor(j["characterColor"], def.characterColor)) {
          warn("Warning: Tile '" + def.name +
               "' has a malformed 'characterColor'; ignoring it.");
        } else {
          def.hasCharacterColor = true;
        }
      }

      if (j.contains("backgroundColor")) {
        if (!ParseColor(j["backgroundColor"], def.backgroundColor)) {
          warn("Warning: Tile '" + def.name +
               "' has a malformed 'backgroundColor'; ignoring it.");
        } else {
          def.hasBackgroundColor = true;
        }
      }

      // A tile with no glyph and no sprite renders as nothing, which reads as a
      // hole in the map rather than as a content bug. One of the two is required.
      if (!def.hasCharacter && def.sprite.empty()) {
        warn("Warning: Tile '" + def.name +
             "' declares neither a 'character' nor a 'sprite', so it will not "
             "be visible when drawn.");
      }

      tiles[def.name] = std::move(def);
    } catch (const std::exception& e) {
      warn("Warning: Failed to parse tile definition " + entry.path().string() +
           ": " + std::string(e.what()));
    }
  }
}

const TileDef* TileRegistry::Get(const std::string& name) const {
  const auto it = tiles.find(name);
  return it == tiles.end() ? nullptr : &it->second;
}

std::vector<std::string> TileRegistry::GetNames() const {
  std::vector<std::string> names;
  names.reserve(tiles.size());
  for (const auto& pair : tiles) {
    names.push_back(pair.first);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<const TileDef*> TileRegistry::GetAll() const {
  std::vector<const TileDef*> all;
  all.reserve(tiles.size());
  for (const auto& name : GetNames()) {
    all.push_back(&tiles.at(name));
  }
  return all;
}
