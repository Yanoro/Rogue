#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Permanent removal of the *files* behind a piece of content.
//
// This is deliberately the other half of what the map editor's trash does, and
// the two must not be confused:
//
//   * the trash (Game::removeObjectMode) removes a tile or an object from the
//     world. It edits the map: the cell is cleared or the entity destroyed, and
//     saving writes that. No content file is touched, so the tile or object can
//     be placed again immediately.
//
//   * this module removes the content itself from disk -- the definition JSON
//     and every sprite file that belongs to it. Nothing in the world changes
//     while the map is open (the map owns its own Tile instances and entities),
//     but the content is gone for every map that names it, and it stays gone
//     after a restart. There is no undo and no trash.
//
// Nothing here touches the open map. A map that already uses the content keeps
// drawing it until it is reloaded, which is why the confirmation UI reports the
// affected maps separately from the files: the files vanish now, the maps show
// it later.
enum class ContentKind { Tile, ObjectTemplate };

// "tile definition" / "object template", for prose in the confirmation UI.
const char *ContentKindLabel(ContentKind kind);

// One candidate file. Kept even when it does not exist, so a plan can say "the
// definition has no file on disk" instead of silently listing nothing -- that is
// exactly the case of a tile a map declared inline, where there is no catalog
// entry to delete.
struct ContentFile {
  std::string path;
  bool exists = false;
  std::uintmax_t bytes = 0;
};

// A map file that names the content, and how it names it. Every reference is
// collected rather than the first, because "used by the tile palette" and "held
// by an NPC's starting inventory" are different things to lose.
struct ContentReference {
  std::string mapPath;
  std::string mapName;
  std::vector<std::string> details;
};

struct ContentPlan {
  ContentKind kind = ContentKind::Tile;
  // The tile's name, or the object template's key.
  std::string name;
  // The definition file first, then the sprite files that exist. See
  // PlanTileRemoval / PlanObjectRemoval for exactly what is collected.
  std::vector<ContentFile> files;
  // The maps on disk that reference the content, sorted by file name.
  std::vector<ContentReference> maps;
  // Live usage on the map currently open, counted so an unsaved editor change is
  // reported too -- the map scan above only sees what is on disk.
  size_t liveCells = 0;
  size_t liveObjects = 0;

  size_t RemovableCount() const;
  bool HasAnythingToRemove() const { return RemovableCount() > 0; }
};

// Builds the plan for a tile. `definitionPath` is TileDef::sourcePath -- the file
// the definition was read from, which is the only trustworthy mapping to it,
// since a tile's file stem is not its name ("wooden wall" lives in
// wooden_wall.json). Pass it empty when the definition came from nowhere (a map's
// inline "tileInfo"); the plan then has no definition file to remove and says so.
//
// Sprites collected: the definition's authored "sprite" path, plus every
// convention candidate for TileSpriteId(tileName). See SpriteCandidatePaths.
ContentPlan PlanTileRemoval(const std::string &tileName,
                            const std::string &definitionPath,
                            const std::string &explicitSprite, size_t liveCells,
                            const std::vector<std::string> &mapPaths);

// Builds the plan for an object template. Unlike a tile, the template key *is*
// the file stem, so the definition file is derived as
// `<templateDirectory>/<templateKey>.json`. `templateDirectory` is the directory
// the factory actually loaded from; an empty one means no file can be named.
//
// Sprites collected: the template's authored "sprite" path, plus every convention
// candidate for the template key.
ContentPlan PlanObjectRemoval(const std::string &templateKey,
                              const std::string &templateDirectory,
                              const std::string &explicitSprite,
                              size_t liveObjects,
                              const std::vector<std::string> &mapPaths);

// Every sprite file that belongs to one piece of content, deduplicated.
//
// All existing layers are returned, not just the one that currently resolves.
// Deleting only the winner would let a lower-priority copy -- a generated-layer
// image under an art pack's -- quietly become the content's art, which looks like
// the deletion failed.
std::vector<std::string> SpriteFilesFor(const std::string &explicitSprite,
                                        const std::string &conventionId);

// Every map file in `mapPaths` that references the content, with the references
// it has. Files that cannot be read or parsed are skipped: a malformed map is a
// problem for the map loader to report, not a reason to refuse a deletion.
std::vector<ContentReference> FindMapsReferencing(
    const std::vector<std::string> &mapPaths, ContentKind kind,
    const std::string &name);

struct ContentDeleteResult {
  std::vector<std::string> deleted;
  // "path: reason" per failure, so the UI can show why without another call.
  std::vector<std::string> failed;
};

// Removes every file in `files` that exists. Independent per file and never
// aborts early: a definition that cannot be removed should not stop its sprite
// from being removed, and the caller is told about each outcome separately.
ContentDeleteResult DeleteContentFiles(const std::vector<ContentFile> &files);
