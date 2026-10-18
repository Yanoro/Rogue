#include "TileRegistry.h"

#include "PlaceCategoriesJson.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "DebugLog.h"

namespace {

// Every key a tile definition may carry -- the union of what
// data/tiles/*.json actually uses and what this class stores.
//
// Used to *refuse* unknown keys, which the directory loader used to ignore. The
// reason is that the runtime-add path is fed by a language model: a model that
// writes "blocksMovement", "glyph", "color" or "hitbox" instead of the real
// spelling would otherwise get a tile that silently ignores the field it thought
// it was setting, and no one would find out until the tile walked wrong. A typo
// in a hand-authored file is the same bug with the same fix. "description" is
// accepted and dropped on purpose: four catalog files carry it, nothing reads it,
// and rejecting files that already exist would be the wrong kind of strict.
constexpr const char* kKnownTileKeys[] = {
    "name",        "character",     "characterColor", "backgroundColor",
    "blocksTile",  "sprite",        "hitboxSize",     "description",
    "placeCategories",
};

bool IsKnownTileKey(const std::string& key) {
  for (const char* known : kKnownTileKeys) {
    if (key == known) return true;
  }
  return false;
}

// A colour, strictly: [r, g, b] or [r, g, b, a] with every component a whole
// number in 0-255. Map.cpp keeps a looser implicit-conversion version of the same
// two-line parser for the map's own "tileInfo" overrides; this one has to *judge*
// rather than merely read, because it is also fed by a language model.
//
// The distinction is not pedantry. Silently wrapping a -20 to 236 would hide a
// schema mistake behind a colour that is almost what was asked for, and the
// runtime path has no human reading the file to catch it.
bool ParseColorStrict(const nlohmann::json& j, Color& out, std::string& error) {
  if (!j.is_array() || j.size() < 3 || j.size() > 4) {
    error = "a colour must be [r, g, b] or [r, g, b, a]";
    return false;
  }
  unsigned char* channels[4] = {&out.r, &out.g, &out.b, &out.a};
  for (size_t i = 0; i < j.size(); ++i) {
    if (!j[i].is_number_integer()) {
      error = "colour component " + std::to_string(i) +
              " is not a whole number (0-255)";
      return false;
    }
    const long long value = j[i].get<long long>();
    if (value < 0 || value > 255) {
      error = "colour component " + std::to_string(i) + " is " +
              std::to_string(value) + ", outside 0-255";
      return false;
    }
    *channels[i] = static_cast<unsigned char>(value);
  }
  if (j.size() == 3) {
    out.a = 255;
  }
  return true;
}

std::string Trim(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

// True when `sprite` is a relative path that stays inside the tree.
//
// An absolute path is not a sprite convention, it is a filesystem reach: the
// texture would be read from wherever the model pointed, including outside the
// project. ".." is refused for the same reason. Both are refusals rather than
// warnings because a definition that gets through is later handed to
// ResourceManager and read from disk.
bool IsSafeSpritePath(const std::string& sprite) {
  if (sprite.empty()) return true;
  if (sprite.front() == '/' || sprite.front() == '\\') return false;
  // A Windows drive prefix, which is absolute too.
  if (sprite.size() >= 2 && std::isalpha(static_cast<unsigned char>(sprite[0])) &&
      sprite[1] == ':') {
    return false;
  }
  size_t start = 0;
  while (start <= sprite.size()) {
    const size_t slash = sprite.find_first_of("/\\", start);
    const std::string part =
        sprite.substr(start, slash == std::string::npos ? std::string::npos
                                                        : slash - start);
    if (part == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

// When a definition entered the catalog. A real file's last-write time is what
// the Map Editor's date sort means by "creation date"; a sourcePath that does
// not resolve -- a designer's in-memory add, whose file is written a moment
// later -- falls back to now, which is the moment it was created.
std::filesystem::file_time_type SourceTimeOrNow(const std::string& sourcePath) {
  std::error_code ec;
  const auto time = std::filesystem::last_write_time(sourcePath, ec);
  return ec ? std::filesystem::file_time_type::clock::now() : time;
}

// Reads one definition object from JSON, applying the same schema and the same
// validation whether it came from a file in the catalog directory or from a model
// at runtime.
//
// Return false only for a *blocking* problem, which is written to `error`.
// Everything worth knowing but not worth refusing -- a shadowed glyph, a
// description with nothing behind it -- goes to `warnings`.
//
// `name` is taken from the JSON, never defaulted from a filename: a tile is
// referenced by its human-readable name (a map's palette entry, a blueprint's
// "paint"), so a definition whose name is absent has no identity to file it
// under. The directory loader's old "default to the file stem" behaviour existed
// to paper over exactly that and is gone; all 20 catalog files name themselves.
bool ParseTileDef(const nlohmann::json& j, const std::string& sourcePath,
                  TileDef& out, std::string& error,
                  std::vector<std::string>& warnings) {
  if (!j.is_object()) {
    error = "a tile definition must be a JSON object";
    return false;
  }

  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!IsKnownTileKey(it.key())) {
      error = "unknown field \"" + it.key() +
              "\". A tile takes only: name, character, characterColor, "
              "backgroundColor, blocksTile, sprite, hitboxSize.";
      return false;
    }
  }

  if (!j.contains("name") || !j["name"].is_string()) {
    error = "a tile definition needs a \"name\" string";
    return false;
  }
  out.name = Trim(j["name"].get<std::string>());
  if (out.name.empty()) {
    error = "the tile's \"name\" is empty";
    return false;
  }

  try {
    out.blocksTile = j.value("blocksTile", false);
    out.sprite = j.value("sprite", std::string());
    if (j.contains("placeCategories")) {
      out.placeCategories = ParsePlaceCategories(j["placeCategories"]);
    }

    if (j.contains("hitboxSize")) {
      if (!j["hitboxSize"].is_number_integer()) {
        error = "\"hitboxSize\" must be a whole number of cells";
        return false;
      }
      const long long size = j["hitboxSize"].get<long long>();
      if (size <= 0) {
        error = "\"hitboxSize\" is " + std::to_string(size) +
                "; a tile with no size occupies nothing";
        return false;
      }
      out.hitboxSize = static_cast<size_t>(size);
      out.hasHitboxSize = true;
    }

    if (j.contains("character")) {
      if (!j["character"].is_string()) {
        error = "\"character\" must be a string holding one glyph";
        return false;
      }
      const std::string glyph = j["character"].get<std::string>();
      if (glyph.empty()) {
        // A wire format that cannot spell "an empty glyph" apart from "no glyph
        // key": refuse rather than guess, because guessing wrong paints nothing.
        error = "\"character\" is an empty string; leave the key out instead of "
                "setting it empty";
        return false;
      }
      // Valid UTF-8 is multi-byte, so a length check alone would refuse "é". The
      // glyph is drawn one *char* at a time by the ASCII renderer, so a non-ASCII
      // glyph is accepted on its first byte and reported as a collision risk.
      if (glyph.size() > 1 && (static_cast<unsigned char>(glyph[0]) & 0x80) == 0) {
        error = "\"character\" is \"" + glyph +
                "\"; a tile is one character, not ";
        error += std::to_string(glyph.size()) + ".";
        return false;
      }
      out.character = glyph.at(0);
      out.hasCharacter = true;
    }

    if (j.contains("characterColor")) {
      std::string colorError;
      if (!ParseColorStrict(j["characterColor"], out.characterColor, colorError)) {
        error = "\"characterColor\" is malformed: " + colorError;
        return false;
      }
      out.hasCharacterColor = true;
    }

    if (j.contains("backgroundColor")) {
      std::string colorError;
      if (!ParseColorStrict(j["backgroundColor"], out.backgroundColor, colorError)) {
        error = "\"backgroundColor\" is malformed: " + colorError;
        return false;
      }
      out.hasBackgroundColor = true;
    }
  } catch (const std::exception& e) {
    // The strict checks above make this nearly unreachable, but the directory
    // loader ports arbitrary hand-written files through here and a type error
    // must be a refusal, not an exception out of a registry load.
    error = std::string("the tile definition could not be read: ") + e.what();
    return false;
  }

  if (!IsSafeSpritePath(out.sprite)) {
    error = "\"sprite\" is \"" + out.sprite +
            "\"; it must be a relative path inside the project (no leading \"/\" "
            "and no \"..\")";
    return false;
  }

  // A tile with no glyph and no sprite renders as nothing at all, which reads as
  // a hole in the world rather than as a content bug. One of the two is required.
  if (!out.hasCharacter && out.sprite.empty()) {
    error = "the tile has neither a \"character\" nor a \"sprite\", so it would "
            "be invisible when drawn";
    return false;
  }

  if (j.contains("description")) {
    warnings.push_back("the \"description\" field is not read by anything; it is "
                       "kept in the file but has no effect");
  }

  out.sourcePath = sourcePath;
  out.sourceTime = SourceTimeOrNow(sourcePath);
  return true;
}

}  // namespace

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
      std::string error;
      std::vector<std::string> warnings;
      if (!ParseTileDef(j, entry.path().string(), def, error, warnings)) {
        warn("Warning: Tile definition " + entry.path().string() +
             " is not usable: " + error + "; skipping it.");
        continue;
      }
      for (const std::string& warning : warnings) {
        warn("Warning: Tile '" + def.name + "': " + warning + ".");
      }

      // Keyed by the tile's NAME, not the file stem, and this is the opposite of
      // every other registry on purpose. For recipes, skills and items the id is
      // the filename and that id is what content references ("iron_scythe").
      // A tile has no such id: what references it is a human-readable name, from
      // a map's tileInfo, a blueprint's "paint", or a material palette. So
      // data/tiles/stone_floor_tile.json defines "stone floor", and
      // Get("stone floor") has to find it.
      //
      // Keying on the stem instead made every multi-word tile unresolvable --
      // "stone floor" is not "stone_floor_tile" -- which surfaced as blueprints
      // reporting that their floor and wall materials were missing from the
      // catalog. (That report also exposed the separate tile/object mistake:
      // walls were never tile materials in the first place.)
      const auto existing = tiles.find(def.name);
      if (existing != tiles.end()) {
        warn("Warning: Tile '" + def.name + "' is defined by both " +
             existing->second->sourcePath + " and " + entry.path().string() +
             "; keeping the first.");
        continue;
      }

      // The key is copied out BEFORE the definition is moved into the map, and
      // that is not style: assignment sequences its right-hand side before its
      // left-hand side, so writing `tiles[def.name] = make_unique<TileDef>(
      // std::move(def))` evaluates the move first and looks the key up in a
      // moved-from (empty) string. Every file then filed itself under "" and all
      // twenty definitions collapsed into one entry that the next iteration's
      // duplicate check refused. Silently -- the only symptom was a catalog with
      // one tile in it.
      const std::string name = def.name;
      tiles[name] = std::make_unique<TileDef>(std::move(def));
      ++catalogRevision;
    } catch (const std::exception& e) {
      warn("Warning: Failed to parse tile definition " + entry.path().string() +
           ": " + std::string(e.what()));
    }
  }
}

const TileDef* TileRegistry::Get(const std::string& name) const {
  const auto it = tiles.find(name);
  return it == tiles.end() ? nullptr : it->second.get();
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
    all.push_back(tiles.at(name).get());
  }
  return all;
}

TileRegistry::AddResult TileRegistry::AddRuntime(const nlohmann::json& json,
                                                 const std::string& sourcePath,
                                                 bool replaceExisting) {
  AddResult result;

  TileDef def;
  if (!ParseTileDef(json, sourcePath, def, result.error, result.warnings)) {
    // The definition is reported, never stored: a refused tile must leave the
    // catalog exactly as it found it, or a retry would be competing with the
    // attempt it is correcting.
    result.warnings.push_back("refused and not added");
    return result;
  }

  const auto existing = tiles.find(def.name);
  if (existing != tiles.end()) {
    if (!replaceExisting) {
      result.error = "a tile named \"" + def.name + "\" already exists (from " +
                     existing->second->sourcePath +
                     "). Pick a different name: replacing it would change the "
                     "meaning of terrain that already uses this name.";
      result.warnings.push_back("refused and not added");
      return result;
    }
    // Refreshing the overlay on reload. The existing pointer is left dangling on
    // purpose by the erase below, which is why the caller must not hold one
    // across this call -- the same contract RemoveRuntimeTiles states.
    if (runtimeAdded.count(def.name) == 0) {
      result.error = "a tile named \"" + def.name +
                     "\" comes from the hand-authored catalog (" +
                     existing->second->sourcePath +
                     "), and the generated overlay may only replace its own";
      result.warnings.push_back("refused and not added");
      return result;
    }
    tiles.erase(existing);
  }

  // A duplicate glyph is what the owner parked, so it is reported and not
  // refused. Checked against the live catalog, which includes previously added
  // runtime definitions -- a model that invents two tiles on the same character
  // should at least be told, even though the collision is tolerated.
  if (def.hasCharacter) {
    for (const auto& pair : tiles) {
      if (pair.second->hasCharacter && pair.second->character == def.character) {
        result.warnings.push_back(
            std::string("the glyph '") + def.character + "' is already used by \"" +
            pair.second->name +
            "\"; glyph collisions are tolerated for now (see the parked note in "
            ".agents/tile-designer-handover.md)");
        break;
      }
    }
  }

  auto owned = std::make_unique<TileDef>(std::move(def));
  const TileDef* stored = owned.get();
  tiles[stored->name] = std::move(owned);
  runtimeAdded.insert(stored->name);
  ++catalogRevision;

  result.ok = true;
  result.definition = stored;
  return result;
}

bool TileRegistry::IsRuntimeAdded(const std::string& name) const {
  return runtimeAdded.count(name) > 0;
}

bool TileRegistry::TouchRuntime(const TileDef* definition) {
  if (definition == nullptr) return false;
  const auto it = tiles.find(definition->name);
  // Identity is checked by pointer, not by name: a caller that built its own
  // TileDef with a matching name must not be able to alias a registry entry and
  // then bump the revision as if it had edited it.
  if (it == tiles.end() || it->second.get() != definition) return false;
  if (runtimeAdded.count(definition->name) == 0) return false;
  ++catalogRevision;
  return true;
}

size_t TileRegistry::RemoveRuntimeTiles() {
  size_t removed = 0;
  for (const std::string& name : runtimeAdded) {
    // erase returns 0 for a name that is somehow gone: counting the actual
    // removals keeps the return value honest rather than restating the layer size.
    removed += tiles.erase(name);
  }
  runtimeAdded.clear();
  if (removed > 0) {
    ++catalogRevision;
  }
  return removed;
}

bool TileRegistry::RemoveDefinition(const std::string& name) {
  const auto it = tiles.find(name);
  if (it == tiles.end()) {
    return false;
  }

  // Ownership is moved into `retired`, not dropped. Get/GetAll/Count stop seeing
  // the definition from here on -- which is the point -- but the storage outlives
  // the removal, because some of this program holds a TileDef* across frames:
  // TileDesignerWindow keeps one for the tile it is editing (`edited`), and
  // TileDesigner::ApplyResult hands one to a preview pane. Freeing here would let
  // a deletion in the Map Editor turn another window's next frame into a
  // use-after-free, which is not a trade worth making for the memory.
  //
  // A retired definition is unreachable through the public API, so nothing can
  // paint it, spawn it or resolve it by name; it is reclaimed with the registry.
  retired.push_back(std::move(it->second));
  tiles.erase(it);
  // Also cleared from the runtime layer: an overlay tile that has been deleted
  // must not be resurrected by the next overlay reload, and leaving the name here
  // would do exactly that.
  runtimeAdded.erase(name);
  ++catalogRevision;
  return true;
}
