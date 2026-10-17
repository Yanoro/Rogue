#include "ContentFiles.h"

#include "SpritePaths.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

// Fills in size and existence. Never throws: a file that cannot be stat'd is
// reported as absent, because "we could not look" and "it is not there" lead to
// the same decision -- there is nothing to remove.
ContentFile DescribeFile(const std::string &path) {
  ContentFile file;
  file.path = path;
  if (path.empty()) {
    return file;
  }

  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(status)) {
    return file;
  }
  if (std::filesystem::is_directory(status)) {
    // A directory where a file was expected is not something this tool removes.
    // Reported as absent rather than recursed into.
    return file;
  }

  file.exists = true;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  file.bytes = ec ? 0 : size;
  return file;
}

// The definition is always listed, existing or not: "there is no file behind
// this tile" is a fact the confirmation dialog has to be able to state.
void AddDefinitionFile(ContentPlan &plan, const std::string &path) {
  if (path.empty()) {
    return;
  }
  plan.files.push_back(DescribeFile(path));
}

// Sprites are listed only when they exist. SpriteCandidatePaths names up to
// three possible locations whether or not any of them is populated, and printing
// two hypothetical paths next to every deletion would train the reader to ignore
// the list.
void AddExistingSpriteFiles(ContentPlan &plan, const std::string &explicitSprite,
                            const std::string &conventionId) {
  std::vector<std::string> seen;
  for (const std::string &path : SpriteFilesFor(explicitSprite, conventionId)) {
    if (std::find(seen.begin(), seen.end(), path) != seen.end()) {
      continue;
    }
    seen.push_back(path);

    ContentFile file = DescribeFile(path);
    if (file.exists) {
      plan.files.push_back(std::move(file));
    }
  }
}

// Returns by value, not by reference: this is called with a temporary fallback
// ("unnamed npc"), and handing back a reference to it would outlive the
// full-expression that created it.
std::string JsonString(const nlohmann::json &object, const char *key,
                       const std::string &fallback) {
  if (!object.is_object() || !object.contains(key)) return fallback;
  const nlohmann::json &value = object[key];
  if (!value.is_string()) return fallback;
  return value.get<std::string>();
}

// A map's tile palette is what names a tile definition: a bare string is a
// catalog reference, an object is a per-map override that still names it. The
// older positional "tileInfo" spelling is read too -- Map::Map still accepts it,
// so a map that uses it is still a map that would break.
void CollectTileReferences(const nlohmann::json &map, const std::string &name,
                           std::vector<std::string> &details) {
  const char *keys[] = {"tilePalette", "tileInfo"};
  for (const char *key : keys) {
    if (!map.contains(key) || !map[key].is_array()) continue;
    for (const auto &entry : map[key]) {
      bool matches = false;
      if (entry.is_string()) {
        matches = entry.get_ref<const std::string &>() == name;
      } else if (entry.is_object()) {
        matches = JsonString(entry, "name", std::string()) == name;
      }
      if (matches) {
        details.push_back(std::string(key) == "tilePalette"
                              ? "tile palette"
                              : "tileInfo (older spelling)");
        break;
      }
    }
  }
}

// Object templates are referenced in four places, and all four matter: a placed
// object, an object's authored starting contents, an NPC's starting inventory and
// an NPC's equipped gear. Each is reported separately because they fail
// differently -- a missing placed object is a hole in the map, a missing equipped
// item is a character without its tool.
void CollectObjectReferences(const nlohmann::json &map, const std::string &key,
                             std::vector<std::string> &details) {
  const auto inventoryMentions = [&](const nlohmann::json &inventory,
                                     const std::string &what) {
    if (!inventory.is_array()) return;
    for (const auto &entry : inventory) {
      if (!entry.is_object()) continue;
      if (JsonString(entry, "item", std::string()) == key) {
        details.push_back(what);
        return;
      }
    }
  };

  if (map.contains("objects") && map["objects"].is_array()) {
    for (const auto &object : map["objects"]) {
      if (!object.is_object()) continue;
      if (JsonString(object, "type", std::string()) == key) {
        details.push_back("placed object");
      }
      if (object.contains("inventory")) {
        inventoryMentions(object["inventory"], "object starting contents");
      }
    }
  }

  if (map.contains("npcs") && map["npcs"].is_array()) {
    for (const auto &npc : map["npcs"]) {
      if (!npc.is_object()) continue;
      const std::string npcName = JsonString(npc, "name", std::string("unnamed npc"));

      if (npc.contains("inventory")) {
        inventoryMentions(npc["inventory"], "npc '" + npcName + "' inventory");
      }

      if (npc.contains("equipped") && npc["equipped"].is_array()) {
        for (const auto &equipped : npc["equipped"]) {
          if (equipped.is_string() &&
              equipped.get_ref<const std::string &>() == key) {
            details.push_back("npc '" + npcName + "' equipped");
            break;
          }
        }
      }
    }
  }
}

// Removes duplicates while keeping the order, so a map that places three copies
// of a template reads "placed object" once.
void Deduplicate(std::vector<std::string> &values) {
  std::vector<std::string> unique;
  for (std::string &value : values) {
    if (std::find(unique.begin(), unique.end(), value) == unique.end()) {
      unique.push_back(std::move(value));
    }
  }
  values = std::move(unique);
}

} // namespace

const char *ContentKindLabel(ContentKind kind) {
  switch (kind) {
  case ContentKind::Tile:
    return "tile definition";
  case ContentKind::ObjectTemplate:
    return "object template";
  }
  return "content";
}

size_t ContentPlan::RemovableCount() const {
  return static_cast<size_t>(
      std::count_if(files.begin(), files.end(),
                    [](const ContentFile &file) { return file.exists; }));
}

std::vector<std::string> SpriteFilesFor(const std::string &explicitSprite,
                                        const std::string &conventionId) {
  std::vector<std::string> paths;
  // Authored first: it is the one the content actually points at, and it is not
  // required to be a convention path at all (four catalog tiles name one under
  // data/sprites/ directly).
  if (!explicitSprite.empty()) {
    paths.push_back(explicitSprite);
  }
  for (const std::string &candidate : SpriteCandidatePaths(conventionId)) {
    paths.push_back(candidate);
  }
  return paths;
}

std::vector<ContentReference> FindMapsReferencing(
    const std::vector<std::string> &mapPaths, ContentKind kind,
    const std::string &name) {
  std::vector<ContentReference> references;
  if (name.empty()) {
    return references;
  }

  for (const std::string &mapPath : mapPaths) {
    std::ifstream file(mapPath);
    if (!file.is_open()) {
      continue;
    }

    nlohmann::json map;
    try {
      file >> map;
    } catch (const std::exception &) {
      // A malformed map is the loader's problem to report, not a reason to abort
      // a deletion. Skipped silently so a broken file elsewhere cannot make every
      // deletion look like it failed.
      continue;
    }
    if (!map.is_object()) {
      continue;
    }

    std::vector<std::string> details;
    if (kind == ContentKind::Tile) {
      CollectTileReferences(map, name, details);
    } else {
      CollectObjectReferences(map, name, details);
    }
    if (details.empty()) {
      continue;
    }
    Deduplicate(details);

    ContentReference reference;
    reference.mapPath = mapPath;
    reference.mapName = std::filesystem::path(mapPath).filename().string();
    reference.details = std::move(details);
    references.push_back(std::move(reference));
  }

  std::sort(references.begin(), references.end(),
            [](const ContentReference &a, const ContentReference &b) {
              return a.mapName < b.mapName;
            });
  return references;
}

ContentPlan PlanTileRemoval(const std::string &tileName,
                            const std::string &definitionPath,
                            const std::string &explicitSprite, size_t liveCells,
                            const std::vector<std::string> &mapPaths) {
  ContentPlan plan;
  plan.kind = ContentKind::Tile;
  plan.name = tileName;
  plan.liveCells = liveCells;

  AddDefinitionFile(plan, definitionPath);
  AddExistingSpriteFiles(plan, explicitSprite, TileSpriteId(tileName));
  plan.maps = FindMapsReferencing(mapPaths, ContentKind::Tile, tileName);
  return plan;
}

ContentPlan PlanObjectRemoval(const std::string &templateKey,
                              const std::string &templateDirectory,
                              const std::string &explicitSprite,
                              size_t liveObjects,
                              const std::vector<std::string> &mapPaths) {
  ContentPlan plan;
  plan.kind = ContentKind::ObjectTemplate;
  plan.name = templateKey;
  plan.liveObjects = liveObjects;

  // The template key is the file stem (ObjectFactory::LoadTemplates), so unlike a
  // tile this path is derivable. It is still only a candidate: the check is
  // DescribeFile's, and a template with no file simply reports none.
  if (!templateDirectory.empty() && !templateKey.empty()) {
    AddDefinitionFile(
        plan, (std::filesystem::path(templateDirectory) / (templateKey + ".json"))
                  .string());
  }
  AddExistingSpriteFiles(plan, explicitSprite, templateKey);
  plan.maps = FindMapsReferencing(mapPaths, ContentKind::ObjectTemplate, templateKey);
  return plan;
}

ContentDeleteResult DeleteContentFiles(const std::vector<ContentFile> &files) {
  ContentDeleteResult result;

  for (const ContentFile &file : files) {
    // A file that was already gone when the plan was built is not a failure: the
    // caller asked for it to not be there.
    if (!file.exists) {
      continue;
    }

    std::error_code ec;
    if (std::filesystem::remove(file.path, ec)) {
      result.deleted.push_back(file.path);
      continue;
    }
    result.failed.push_back(file.path + ": " +
                            (ec ? ec.message() : std::string("could not remove it")));
  }

  return result;
}
