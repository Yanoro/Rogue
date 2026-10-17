#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "Components.h"

class DebugLog;

// Content database for tile *definitions* -- what a **ground** tile named
// "wooden floor" is, independently of any map that uses it. Walls, doors and
// props are objects and live in data/objects, never here.
//
// Why this exists. Tile definitions used to live only inside each map's
// "tileInfo" array, and cells are indices into that array (Map::uniqueTiles).
// That made a tile a property of one map rather than of the world, with three
// consequences that only became visible once something wanted to *reference* a
// tile instead of owning it:
//
//   1. Every map re-declared the same tiles, and they could disagree. In this
//      repo they already did: "path" is '.' on Map_Farm and ':' on Map_Smithy,
//      and "wooden floor" uses characterColor 186,146,96 on one map and a
//      different tone on another.
//   2. A blueprint or an agent that is not inside a map has no vocabulary to
//      name a surface with. The designer agent is global by decision, so it
//      could not answer "what can I build with?" at all.
//   3. A genre pack cannot ship a tile vocabulary, because there was no
//      cross-map place for one to live.
//
// So the catalog is the world's vocabulary and a map's "tileInfo" is demoted to
// per-map *overrides*. A map that wants the smithy's ':' path still says so;
// it just no longer has to restate the colors, and every map that says nothing
// now agrees. Map::BuildMapJson writes only the overrides back, so a save does
// not re-duplicate the catalog into the map file.
//
// Same shape as StatRegistry / SkillRegistry: a plain content database loaded
// from a directory of .json files, injected where needed, reachable through a
// resource component (TileRegistryResource).
class TileRegistry {
public:
  // Reads every .json in `directoryPath`. Malformed files are reported through
  // the injected DebugLog (or stderr) rather than aborting the load. A missing
  // directory is a warning, not an error: a map that declares every tile it
  // uses inline still works, which is how every map predates this class.
  void LoadTiles(const std::string& directoryPath);

  // nullptr for an unknown name.
  //
  // The pointer is stable for the registry's lifetime, and that promise is load
  // bearing rather than incidental: callers hold it across later inserts (a
  // blueprint validator holds one while the designer adds a tile, Map::CreateTile
  // holds one while building a Tile). It is kept by owning each definition
  // through a unique_ptr, so growth relocates the bucket array without moving the
  // definition itself -- storing TileDef by value in the map would invalidate
  // every outstanding pointer on a rehash.
  const TileDef* Get(const std::string& name) const;

  // Sorted, so a material palette can be built deterministically for prompts
  // and debug UIs.
  std::vector<std::string> GetNames() const;

  // Every definition, sorted by name.
  std::vector<const TileDef*> GetAll() const;

  size_t Count() const { return tiles.size(); }

  // ---------------------------------------------------------------------------
  // Runtime additions (the generated overlay)
  // ---------------------------------------------------------------------------

  // A validated runtime addition. Returned rather than inferred from `ok`, because
  // the caller has to be able to hand the failure back to the model in prose --
  // "the tile was refused" teaches an agent nothing.
  struct AddResult {
    bool ok = false;
    // The blocking problem, exactly as it should be fed back to a designer agent.
    // Empty when the addition succeeded.
    std::string error;
    // Non-blocking observations (a shadowed glyph, a description that is kept but
    // means nothing). Worth surfacing; never a reason to refuse.
    std::vector<std::string> warnings;
    // The stored definition on success; null on failure. Stable for the
    // registry's lifetime, like Get.
    const TileDef* definition = nullptr;
  };

  // Adds one definition to the catalog on behalf of a designer agent.
  //
  // Why this exists. LoadTiles is a one-shot directory read: it fills the catalog
  // and there is no path for a definition invented while the game is running to
  // enter it, so Map::CreateTile -- which resolves through this catalog -- cannot
  // see an agent-created tile no matter what the agent writes to disk. This is
  // that path, and it is deliberately the only one: nothing else may add to a
  // loaded catalog.
  //
  // Where the definition belongs. Per docs/asset-generation-design.md sections
  // 4.3-4.6, agent-created content never enters the hand-authored
  // data/tiles/*.json catalog; it lives in a generated overlay directory. This
  // call is agnostic about where the JSON came from -- it takes a parsed object --
  // so the overlay loader, the headless --tile-design mode and the tile designer
  // window can all drive it identically.
  //
  // Validation, and the shape of it. A tile carries no economic component, so
  // there is no allowlist to enforce here (that problem is objects only). What
  // matters for tiles is *identity*: a name that collides silently redefines
  // terrain a map already references, and a definition with no appearance paints
  // an invisible patch of ground. So the checks are:
  //
  //   - `name` present, trimmed, non-empty;
  //   - no existing definition under that name, unless `replaceExisting` (which is
  //     for reloading the overlay, not for an agent amending the catalog);
  //   - a glyph or a sprite, so the tile is visible;
  //   - a glyph is a single character;
  //   - colours are [r, g, b] or [r, g, b, a] with each component 0-255 (a
  //     negative or out-of-range value is a schema mistake, not a colour);
  //   - `hitboxSize` is a positive whole number (a zero or negative hitbox is a
  //     tile that occupies nothing, which no map size makes sensible);
  //   - `sprite` is a relative path with no "..";
  //   - unknown top-level keys are refused, in both this path and the directory
  //     loader. A model that invents "blocksMovement" or "glyph" would otherwise
  //     get a tile that quietly ignores the field it thought it was setting.
  //
  // A *duplicate glyph* is not an error, only a warning. Glyph collisions are
  // parked by the owner on purpose -- the existing catalog already shares '#', '.',
  // '\'' and others (see the [SURROUNDINGS] note in
  // .agents/tile-designer-handover.md) -- so this reports them without refusing.
  //
  // `sourcePath` is recorded on the definition so a later duplicate-name warning
  // can name where the original came from. On failure the definition is not
  // stored and nothing about the registry changes.
  // The name in `json` must be free, or `replaceExisting` must be set -- and even
  // then only a definition this registry itself added may be replaced, never one
  // that came from the hand-authored catalog. Refreshing the overlay on reload is
  // the only thing that flag is for.
  AddResult AddRuntime(const nlohmann::json& json,
                       const std::string& sourcePath,
                       bool replaceExisting = false);

  // True when `name` was added through AddRuntime rather than loaded from the
  // catalog directory. There is no persistent marker on disk: the catalog and the
  // overlay are different directories, so which directory a definition came from
  // *is* the distinction, and reloading is "remove the runtime layer, then re-read
  // the overlay".
  bool IsRuntimeAdded(const std::string& name) const;

  // Removes every runtime-added definition. Hand-authored catalog entries are
  // untouched. Returns how many were removed, so a caller can log "reloaded 3
  // generated definitions" without diffing names itself.
  //
  // This is what makes the overlay reloadable: the overlay is not the catalog, so
  // dropping the layer and re-adding it cannot leave a half-updated vocabulary.
  // Every pointer previously returned for a runtime definition dangles.
  size_t RemoveRuntimeTiles();

  // Drops one definition from the catalog, whatever layer it came from. Returns
  // false when the name is unknown.
  //
  // This exists for the map editor's permanent content deletion: the file is
  // removed from disk first and this makes memory agree with it, so the palette
  // stops offering a tile that can no longer be painted and Map::CreateTile
  // stops resolving a name nothing defines. Derived views are invalidated through
  // catalogRevision, exactly like an add.
  //
  // Pointers handed out earlier stay VALID but stop resolving: the definition is
  // retired rather than freed, because TileDesignerWindow holds a TileDef* for the
  // tile it is editing across frames and a deletion made in another window must
  // not turn its next frame into a use-after-free. TouchRuntime on a retired
  // definition therefore returns false, which is the correct answer -- it is no
  // longer in the catalog to edit.
  //
  // A map that already draws the tile is unaffected until it reloads: the map
  // owns its own Tile instances, resolved when it was loaded.
  bool RemoveDefinition(const std::string& name);

  // Marks a runtime definition as edited in place, so derived views are rebuilt
  // and the change is worth persisting.
  //
  // Deliberately not a general "mutate anything": the caller hands back the
  // pointer Get/AddRuntime gave it, and a factory definition is refused. It
  // exists because a tile designer iterates -- the first reply is a draft, and
  // nudging a colour or a glyph must not cost another model round trip. Returns
  // false when the name is unknown or is not a runtime definition.
  bool TouchRuntime(const TileDef* definition);

  // Bumped whenever the catalog's contents change (a runtime add or a removal).
  // The catalog is cached into derived views -- the material palette spliced into
  // the designer's prompt is the one that matters -- and a cache that cannot tell
  // the vocabulary changed would keep offering a name that no longer resolves.
  uint64_t CatalogRevision() const { return catalogRevision; }

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  // Owned by pointer, not stored by value, purely so Get's stability promise
  // survives insertion. The rehash still moves buckets; it cannot move what a
  // bucket points at.
  std::unordered_map<std::string, std::unique_ptr<TileDef>> tiles;
  // Definitions removed through RemoveDefinition: out of the catalog, out of the
  // lookup, but still owned so that pointers already handed out stay valid.
  // Reclaimed with the registry, so its size is bounded by how many definitions a
  // session removes, not by anything a game does.
  std::vector<std::unique_ptr<TileDef>> retired;
  // Names added through AddRuntime, which is the generated overlay's layer.
  std::unordered_set<std::string> runtimeAdded;
  uint64_t catalogRevision = 0;
  DebugLog* debugLog = nullptr;
};
