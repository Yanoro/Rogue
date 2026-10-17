#pragma once
#include <flecs.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include "Components.h"

#include <optional>

class Map;
class DebugLog;
class ItemRegistry;
class ResourceManager;

class ObjectFactory {
public:
  ObjectFactory();
  void LoadTemplates(const std::string& directoryPath);

  // `authored` marks the object as map content (see MapAuthored): the map loader
  // and the map editor pass true, while any runtime spawn (harvest drop, craft
  // output, NPC starting inventory) leaves it false so the map writer ignores it.
  flecs::entity SpawnObject(flecs::world& ecs, flecs::entity parent, Map* map,
                            const std::string& type,
                            std::optional<GamePosition> pos = std::nullopt,
                            bool authored = false);

  // Turns a list of ItemStacks into real entities held by `holder`: one entity
  // per unit, spawned as a child of `holder` and attached with the Holds
  // relationship. That is the same shape harvest drops and crafting outputs use,
  // so [INVENTORY], [TAKE], [STORE] and [CRAFT] see these items with no special
  // casing. Shared by map-defined NPC inventories and map-defined object
  // (container) inventories. An unknown template id is reported by SpawnObject
  // and skipped.
  void SpawnInventory(flecs::world& ecs, flecs::entity holder,
                      const std::vector<ItemStack>& inventory);

  bool ApplyTemplate(flecs::entity obj, const std::string& type, Map* map = nullptr);
  void SetDebugLog(DebugLog* log) { debugLog = log; }

  // Item definitions, used to seed per-instance durability at spawn. Injected
  // rather than included so the factory keeps no link dependency on the item
  // registry; a null registry simply means nothing spawns with durability.
  void SetItemRegistry(const ItemRegistry* registry) { itemRegistry = registry; }

  // Sprite textures, injected for the same reason as the item registry: applying
  // a template is what resolves its "sprite" key, and with no manager nothing
  // resolves and every object keeps its ASCII art. A null manager is therefore a
  // supported state, not a bug.
  void SetResourceManager(ResourceManager* manager) { resourceManager = manager; }

  const std::unordered_map<std::string, nlohmann::json>& GetTemplates() const { return templates; }

  // The directory LoadTemplates read from. Recorded because a template key is its
  // file stem, so this is what turns a key back into a path -- which the map
  // editor's permanent content deletion needs in order to name the file behind a
  // template. BlueprintRegistry keeps its directory for the same reason.
  const std::string& GetDirectory() const { return directory; }

  // When this template entered the factory, for the Map Editor's date sort:
  // the source file's last-write time for a template read from disk, or the
  // moment it was added for one a designer created in memory before its file
  // was written. Zero when the key is unknown and sorts last.
  //
  // Recorded here rather than derived from a path at display time because a
  // designer-created key has no path until ObjectDesigner writes it, and a
  // runtime template does not necessarily live in GetDirectory().
  std::filesystem::file_time_type GetTemplateTime(const std::string& key) const;

  // Drops one template from the factory. Returns false when the key is unknown.
  //
  // This exists for the map editor's permanent content deletion: the file is
  // removed from disk first and this makes memory agree with it, so the palette
  // stops offering a template that can no longer be spawned. Entities already
  // spawned from it are untouched -- they carry copies of its components, which
  // is the point of the factory -- so nothing in the world changes here. That is
  // the deliberate difference from the map editor's trash, which destroys the
  // entities instead.
  //
  // Safe with respect to pointers in a way TileRegistry::RemoveDefinition is not:
  // templates are owned by value inside the map, and nothing hands out a pointer
  // into it. GetTemplates() returns a reference, so a caller must not hold it
  // across this call -- the palette loop is what does, and it re-reads the map
  // every frame.
  bool RemoveTemplate(const std::string& key);

  // ---------------------------------------------------------------------------
  // Runtime registration: what the object designer needs to add a template at run
  // time, mirroring TileRegistry's runtime-add path.
  //
  // Why the factory needs one at all. Templates were loaded once at startup and
  // mutated only by RemoveTemplate, so an object an agent invented had nowhere to
  // live. The tile side solved this in TileRegistry (AddRuntime, IsRuntimeAdded,
  // RuntimeNames, RemoveRuntimeTiles); this is the same contract, with one addition
  // tiles did not need and objects do: **validation by running the real setters**
  // (see AddRuntime below).
  //
  // Nothing here filters by key. A template key is not an id in a namespace -- it
  // doubles as the sprite convention id (`altar` -> data/sprites/altar.png), and the
  // caller decides what may be keyed. In particular a runtime key MAY collide with an
  // item id: data/objects/iron_scythe.json and the iron scythe item already share a
  // key in the hand-authored content, because one thing can legitimately be both a
  // world object and an item. The caller must not assume the two key spaces are
  // disjoint, and must not "fix" a collision by renaming.
  // ---------------------------------------------------------------------------

  // What one attempted runtime add did. `ok` is the verdict; a false `ok` always
  // carries a reason worded to be fed straight back to a model.
  struct RuntimeAddResult {
    bool ok = false;
    std::string key;
    std::string error;
    // Non-blocking observations from the real setters: an art block whose palette
    // misses a character, a 'sprite' that is not a string. Kept on success, because
    // each one is a field that will quietly do nothing in the world.
    std::vector<std::string> warnings;
  };

  // The template key a definition would be registered under: its explicit `key` when it
  // has one, else the slug of its `name`. Empty when it has neither.
  //
  // Exposed because the key is not an implementation detail of registration: it doubles
  // as the sprite convention id, so a caller that wants to *ask* whether a design is a
  // revision -- or which sprite it will look for -- has to compute it the same way
  // AddRuntime does. The one place both use.
  static std::string KeyFor(const nlohmann::json& definition);

  // Validates `definition` by running the same component setters `ApplyTemplate`
  // would, on a throwaway entity in a scratch world, then registers it in memory.
  //
  // **Validation is the real code path on purpose.** A second parser that checked
  // component names against a list and parameter shapes against a schema would be a
  // second thing that can disagree with the loader, and the disagreement would show up
  // as an object that validated and then did nothing. Running the setters makes "the
  // validator accepted it" and "the engine can spawn it" the same statement by
  // construction.
  //
  // The documented limit of that choice: the setters are *lenient by design*, so a
  // parameter of the wrong type is defaulted rather than refused (`"capacity": "lots"`
  // becomes 10). This is about the shape of an object -- unknown components, unusable
  // art, no capabilities at all -- not about type-checking every parameter. The
  // vocabulary (ComponentVocabulary) is what tells the designer the right spellings,
  // and the leniency is exactly why it must.
  //
  // `requireAtLeastOneComponent` is true for the designer's path and false for the
  // overlay loader: the hand-authored content is the ground truth, and a template
  // without capabilities is a judgement the owner is allowed to make.
  //
  // `replaceExisting` allows re-registering a key a previous runtime add or the overlay
  // created, so a design can be iterated on. A hand-authored template is never
  // replaceable here: the catalog is read-only to the designer.
  //
  // Main-thread only, and not safe to call while a `GetTemplates()` reference is held.
  RuntimeAddResult AddRuntime(const nlohmann::json& definition,
                              const std::string& source = "(object designer)",
                              bool replaceExisting = false,
                              bool requireAtLeastOneComponent = true);

  // True when `key` came from AddRuntime rather than from a loaded file.
  bool IsRuntimeAdded(const std::string& key) const;

  // The keys AddRuntime has registered, in sorted order.
  std::vector<std::string> RuntimeNames() const;

  // Drops every runtime-added template, leaving the loaded catalog untouched.
  // Returns how many were dropped. Used to reload the generated overlay.
  size_t RemoveRuntimeTemplates();

  // ---------------------------------------------------------------------------
  // Vocabulary: what a designer is allowed to write.
  // ---------------------------------------------------------------------------

  // One registered component, as the designer needs to see it.
  struct ComponentInfo {
    std::string name;
    // The template keys its setter reads, e.g. Storage -> {"capacity"}, with nested
    // keys spelled dotted ("placement.becomes"). Sorted.
    //
    // Kept beside the setter rather than transcribed into a prompt document, because
    // the first draft of docs/sprite-designer-design.md hand-wrote this list and got
    // the component *count* wrong -- it said eight when ten are registered. A
    // hand-written vocabulary drifts the moment a component is added, and the failure
    // is quiet: the designer writes a key the engine ignores.
    std::vector<std::string> parameters;
    // One line naming what the component does, for the prompt. Prose, and deliberately
    // short: the prompt is a list a model reads, not documentation.
    std::string summary;
  };

  // Every registered component, sorted by name.
  std::vector<ComponentInfo> ComponentVocabulary() const;

  // The component names only, sorted. Used where the parameters are noise.
  std::vector<std::string> ComponentNames() const;

  // Whether `name` is a component this factory can set. The cheap pre-check a window
  // runs before sending a request, so a designer is not asked for a capability the
  // engine does not have.
  bool HasComponent(const std::string& name) const;

  // Every key a definition may legitimately carry: each component's parameters, the
  // display keys ApplyTemplate reads, and the structural keys ("name", "components").
  // Used by the validator to name a key nothing will read.
  std::vector<std::string> KnownFieldNames() const;

  void RegisterDefaultComponents();

  template<typename T>
  void RegisterComponent(const std::string& name, std::function<void(flecs::entity, const nlohmann::json&)> parser) {
    componentSetters[name] = parser;
    componentRemovers[name] = [](flecs::entity e) { e.remove<T>(); };
  }

private:
  // Records the vocabulary in the same call that installs the parser, so there is no
  // way to register a component and forget to describe it.
  void RecordComponentInfo(const std::string& name,
                           std::vector<std::string> parameters,
                           std::string summary);

  std::unordered_map<std::string, std::vector<std::string>> componentParameters;
  std::vector<std::string> componentOrder;
  std::unordered_map<std::string, std::string> componentSummaries;
  // Key -> the source it came from, for IsRuntimeAdded and the log. Separate from
  // `templates` so the loaded catalog stays distinguishable from what a designer made.
  std::unordered_map<std::string, std::string> runtimeKeys;

  std::unordered_map<std::string, nlohmann::json> templates;
  // Key -> when that template entered the factory. See GetTemplateTime.
  std::unordered_map<std::string, std::filesystem::file_time_type> templateTimes;
  // Where the templates came from. See GetDirectory.
  std::string directory;
  DebugLog* debugLog = nullptr;
  const ItemRegistry* itemRegistry = nullptr;
  ResourceManager* resourceManager = nullptr;

  std::unordered_map<std::string, std::function<void(flecs::entity, const nlohmann::json&)>> componentSetters;
  std::unordered_map<std::string, std::function<void(flecs::entity)>> componentRemovers;
};
