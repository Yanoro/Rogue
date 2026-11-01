#include "ContentKinds.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "ActorSpriteRegistry.h"
#include "Game.h"
#include "ObjectFactory.h"
#include "SpritePaths.hpp"
#include "TileDesigner.hpp"
#include "TileRegistry.h"

namespace ContentKinds {

namespace {

// ---- tiles -----------------------------------------------------------------

std::string TileOrigin(const TileDef &definition, const TileRegistry &tiles) {
  if (tiles.IsRuntimeAdded(definition.name)) return "generated overlay";
  if (!definition.sourcePath.empty()) return definition.sourcePath;
  return "session only";
}

std::vector<Entry> ListTiles(const World &world) {
  std::vector<Entry> out;
  for (const TileDef *definition : world.tiles.GetAll()) {
    out.push_back({definition->name, definition->name,
                   TileOrigin(*definition, world.tiles)});
  }
  return out;
}

nlohmann::json ReadTile(const World &world, const std::string &id) {
  const TileDef *definition = world.tiles.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return TileDesigner::WriteTileDef(*definition);
}

bool ApplyTile(const World &world, const nlohmann::json &document, std::string *error,
               std::vector<std::string> *warnings) {
  const TileRegistry::AddResult result =
      world.tiles.OverrideDefinition(document, "(content editor)");
  if (!result.ok) {
    if (error != nullptr) *error = result.error;
    return false;
  }
  if (warnings != nullptr) *warnings = result.warnings;
  return true;
}

std::string TileFileName(const nlohmann::json &document) {
  return TileDesigner::SlugifyFileName(document.value("name", std::string()));
}

std::string TileSprite(const std::string &id) { return ::TileSpriteId(id); }

// ---- objects ---------------------------------------------------------------

std::vector<Entry> ListObjects(const World &world) {
  std::vector<Entry> out;
  for (const auto &entry : world.objects.GetTemplates()) {
    const std::string name = entry.second.value("name", entry.first);
    out.push_back({entry.first, name,
                   world.objects.IsRuntimeAdded(entry.first) ? "generated overlay"
                                                             : "catalog"});
  }
  std::sort(out.begin(), out.end(),
            [](const Entry &a, const Entry &b) { return a.label < b.label; });
  return out;
}

nlohmann::json ReadObject(const World &world, const std::string &id) {
  const auto &templates = world.objects.GetTemplates();
  const auto found = templates.find(id);
  if (found == templates.end()) return nlohmann::json::object();
  return found->second;
}

bool ApplyObject(const World &world, const nlohmann::json &document, std::string *error,
                 std::vector<std::string> *warnings) {
  const ObjectFactory::RuntimeAddResult result = world.objects.AddRuntime(
      document, "(content editor)", /*replaceExisting=*/true,
      /*requireAtLeastOneComponent=*/false, /*allowCatalogReplace=*/true);
  if (!result.ok) {
    if (error != nullptr) *error = result.error;
    return false;
  }
  if (warnings != nullptr) *warnings = result.warnings;
  return true;
}

std::string ObjectFileName(const nlohmann::json &document) {
  return ObjectFactory::KeyFor(document);
}

std::string ObjectSprite(const std::string &id) { return id; }

// ---- actor sprites ---------------------------------------------------------

// The catalogue is about appearance, not about beings: `Actor` is the ECS tag, and these
// entries say only what something looks like.
std::vector<Entry> ListActorSprites(const World &world) {
  std::vector<Entry> out;
  for (const ActorSpriteDef *definition : world.actorSprites.GetAll()) {
    out.push_back({definition->id, definition->name, "actor sprite"});
  }
  return out;
}

nlohmann::json ReadActorSprite(const World &world, const std::string &id) {
  const ActorSpriteDef *definition = world.actorSprites.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.actorSprites.ToJson(*definition);
}

bool ApplyActorSprite(const World &world, const nlohmann::json &document,
                      std::string *error, std::vector<std::string> *warnings) {
  return world.actorSprites.AddOrReplace(document, "(content editor)", error, warnings);
}

std::string ActorSpriteFileName(const nlohmann::json &document) {
  return ActorSpriteRegistry::IdFor(document);
}

std::string ActorSpriteSprite(const std::string &id) { return id; }

}  // namespace

bool WriteOverrideFile(const Kind &kind, const std::string &root,
                       const nlohmann::json &document, std::string *path,
                       std::string *error) {
  const std::string stem = kind.fileName != nullptr ? kind.fileName(document) : "";
  if (stem.empty()) {
    if (error != nullptr) {
      *error = "the document has no name, so there is no file to save it under";
    }
    return false;
  }

  const std::filesystem::path directory = std::filesystem::path(root) / kind.id;
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "could not create " + directory.string() + ": " + ec.message();
    }
    return false;
  }

  const std::filesystem::path file = directory / (stem + ".json");
  std::ofstream out(file);
  if (!out.is_open()) {
    if (error != nullptr) *error = "could not open " + file.string() + " for writing";
    return false;
  }
  out << document.dump(2) << "\n";
  out.close();
  if (!out) {
    if (error != nullptr) *error = "writing " + file.string() + " failed";
    return false;
  }
  if (path != nullptr) *path = file.string();
  return true;
}

const std::vector<Kind> &All() {
  static const std::vector<Kind> kinds = {
      {
          "tiles",
          "Tiles",
          /*hasSprites=*/true,
          &ListTiles,
          &ReadTile,
          &ApplyTile,
          &TileFileName,
          &TileSprite,
      },
      {
          "objects",
          "Objects",
          /*hasSprites=*/true,
          &ListObjects,
          &ReadObject,
          &ApplyObject,
          &ObjectFileName,
          &ObjectSprite,
      },
      {
          "actors",
          "Actor sprites",
          /*hasSprites=*/true,
          &ListActorSprites,
          &ReadActorSprite,
          &ApplyActorSprite,
          &ActorSpriteFileName,
          &ActorSpriteSprite,
      },
  };
  return kinds;
}

const Kind *For(const std::string &id) {
  for (const Kind &kind : All()) {
    if (kind.id == id) return &kind;
  }
  return nullptr;
}

}  // namespace ContentKinds
