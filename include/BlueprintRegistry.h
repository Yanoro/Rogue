#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "Blueprint.hpp"

class DebugLog;
class TileRegistry;
class ObjectFactory;

// Content database for blueprints, loaded once at init from
// data/blueprints/*.json. Same shape as the other registries (StatRegistry,
// SkillRegistry, TileRegistry): a plain database loaded from a directory,
// injected where needed, reachable through BlueprintRegistryResource.
//
// What makes this different from the others: a blueprint is *validated against
// the world's vocabulary at load time*, not merely parsed. A "paint" naming a
// tile that does not exist in the catalog (or a "place" naming a template the
// object factory does not have) is a content bug that would otherwise surface as
// a silently empty building, which reads exactly like a working design that
// happens to place nothing. So both registries are handed in and the vocabulary
// is checked here.
//
// The catalog is deliberately consulted instead of a loaded map: a blueprint is
// map-independent by design, and the designer agent has no map at all.
class BlueprintRegistry {
public:
  // Reads every .json in `directoryPath`. Malformed files are reported through
  // the injected DebugLog (or stderr) rather than aborting the load. A missing
  // directory is a warning, not an error: blueprints are optional content, and
  // the debug window simply lists nothing.
  void LoadBlueprints(const std::string& directoryPath);

  // Parses a blueprint from JSON. Used by the loader and by the designer agent's
  // reply, so a runtime-authored design goes through exactly the same path as an
  // authored file. `id` is the identity to register under.
  //
  // Returns false and fills `error` when the JSON is malformed. Vocabulary
  // problems are reported through `warnings` instead: an unknown material makes a
  // design place less, which is worth saying but not worth refusing the design
  // over, and refusing would make the designer agent's mistakes unrecoverable.
  static bool ParseBlueprint(const nlohmann::json& json, const std::string& id,
                             const std::string& sourcePath, Blueprint& out,
                             std::string& error,
                             std::vector<std::string>& warnings);

  // Writes a blueprint to `<directory>/<id>.json` and reloads the registry, so a
  // design written at runtime is immediately usable and visible -- the Blueprints
  // window reads the registry every frame, so a reload is all it takes.
  //
  // Serialised through WriteBlueprint rather than by re-emitting the designer's
  // raw text: the text may carry fields the schema does not define (the designer's
  // own notes, most importantly), and the file on disk should be exactly the
  // schema, not whatever the model felt like adding.
  //
  // Overwrites an existing file with the same id, because iterating on a design
  // produces the same id every time and refusing would make revision impossible.
  struct SaveResult {
    bool ok = false;
    std::string path;
    bool overwrote = false;
    std::string error;
  };
  SaveResult SaveBlueprint(const Blueprint& blueprint);

  // Deletes a design: removes its file from the content directory and reloads, so
  // it disappears from the registry the same way a save makes it appear.
  //
  // Destructive and not undoable, which is why the window confirms first -- there
  // is no trash and no revision history behind this.
  struct DeleteResult {
    bool ok = false;
    std::string path;
    std::string error;
  };
  DeleteResult DeleteBlueprint(const std::string& id);

  // Re-reads the directory, replacing everything loaded. Idempotent, which is
  // what makes it safe to call after a save.
  void Reload();

  // Drops every loaded blueprint. Public because a content reload (the Map Reload
  // window) needs to reread blueprints from scratch rather than accumulate.
  void Clear();

  // The directory the last load read from, and where SaveBlueprint writes.
  const std::string& GetDirectory() const { return directory; }

  // Serialises a blueprint back to the schema. Exposed so the debug window can
  // show exactly what would be written.
  static nlohmann::json WriteBlueprint(const Blueprint& blueprint);

  // nullptr for an unknown id. The pointer is stable only until the registry is
  // reloaded -- that is, until LoadBlueprints, Reload, SaveBlueprint or
  // DeleteBlueprint is called. Nothing may be held across one of those:
  //   const Blueprint* bp = registry.Get(id);
  //   registry.SaveBlueprint(*bp);      // frees the object `bp` points at
  // SaveBlueprint and DeleteBlueprint copy their arguments on entry precisely so
  // that passing a reference into this registry is safe; a held *pointer* is not.
  const Blueprint* Get(const std::string& id) const;

  // Sorted, so debug UIs and prompts are stable.
  std::vector<std::string> GetIds() const;

  // Every blueprint, sorted by id. The pointers share Get's lifetime: valid until
  // the registry reloads. A caller that draws a list while also mutating the
  // registry must copy what it needs (ids, names) rather than reading through
  // these afterwards.
  std::vector<const Blueprint*> GetAll() const;

  size_t Count() const { return blueprints.size(); }

  void SetDebugLog(DebugLog* log) { debugLog = log; }
  // Handed in rather than reached for, so the dependency stays one-way.
  void SetTileRegistry(const TileRegistry* tiles) { tileRegistry = tiles; }
  void SetObjectFactory(const ObjectFactory* factory) { objectFactory = factory; }

private:
  std::unordered_map<std::string, Blueprint> blueprints;
  // Where LoadBlueprints last read from. Kept so SaveBlueprint writes back into
  // the same content root rather than a hardcoded path.
  std::string directory;
  DebugLog* debugLog = nullptr;
  const TileRegistry* tileRegistry = nullptr;
  const ObjectFactory* objectFactory = nullptr;
};
