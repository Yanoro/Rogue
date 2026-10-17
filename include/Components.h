#pragma once

#include "AI.h"
#include "raylib-cpp.hpp"
#include <cstdint>
#include <flecs.h>
#include <memory>
#include <string>
#include <vector>

#include "AsciiArtTypes.hpp"
#include "ItemStack.hpp"
#include "ItemTypes.h"
#include "LootDrop.hpp"
#include "SkillTypes.h"
#include "SkillXp.hpp"
#include "StatTypes.h"
#include "Window.h"

struct Render {};

struct ScreenPosition : public raylib::Vector2 {
  ScreenPosition(float x = 0.0f, float y = 0.0f) : raylib::Vector2(x, y) {}
  ScreenPosition(const raylib::Vector2 &v) : raylib::Vector2(v) {}

  bool operator==(const ScreenPosition &other) const {
    return x == other.x and y == other.y;
  }
};

struct GamePosition {
  int x;
  int y;

  operator raylib::Vector2() const {
    return raylib::Vector2(static_cast<float>(x), static_cast<float>(y));
  }

  bool operator==(const GamePosition &other) const {
    return x == other.x && y == other.y;
  }
  GamePosition &operator+=(const GamePosition &other) {
    this->x += other.x;
    this->y += other.y;
    return *this;
  }
  GamePosition operator+(const GamePosition &other) const {
    GamePosition result = *this;
    result += other;
    return result;
  }
  GamePosition &operator-=(const GamePosition &other) {
    this->x -= other.x;
    this->y -= other.y;
    return *this;
  }
  GamePosition operator-(const GamePosition &other) const {
    GamePosition result = *this;
    result -= other;
    return result;
  }

  friend std::ostream &operator<<(std::ostream &os, const GamePosition &pos) {
    os << "(" << pos.x << ", " << pos.y << ")";
    return os;
  }
};

struct MaxSpeed {
  float value;
};

struct Velocity : public raylib::Vector2 {
  Velocity(float x = 0.0f, float y = 0.0f) : raylib::Vector2(x, y) {}
  Velocity(const raylib::Vector2 &v) : raylib::Vector2(v) {}
  Velocity(const ::Vector2 &v) : raylib::Vector2(v) {}

  bool operator==(const Velocity &other) const {
    return x == other.x and y == other.y;
  }
};

struct Acceleration : public raylib::Vector2 {
  Acceleration(float x = 0.0f, float y = 0.0f) : raylib::Vector2(x, y) {}
  Acceleration(const raylib::Vector2 &v) : raylib::Vector2(v) {}
  Acceleration(const ::Vector2 &v) : raylib::Vector2(v) {}

  bool operator==(const Acceleration &other) const {
    return x == other.x and y == other.y;
  }
};

struct Friction {
  float value;
};

struct BlocksTile {};

// Entity can walk through Blocked tiles
struct Intangible {};

struct Frame {
  raylib::Rectangle frameRect;
  double duration;
};

struct DrawAscii {
  char ch;
  Color characterColor;
  Color backgroundColor;
  size_t width;
  size_t height;
};

// AsciiArt (multi-line art drawn inside the DrawAscii box) lives in
// AsciiArtTypes.hpp: RaceDef carries one too, and the race headers are included
// by this file, so it cannot be defined here.

// A raster sprite for an entity, drawn instead of its ASCII look.
//
// This component exists only when the image actually loaded. Resolution happens
// once at spawn in AttachSprite (SpritePaths.hpp), which attaches it on success
// and does nothing on failure -- which is what makes the ASCII art the fallback
// rather than a special case in the renderer. An entity with no sprite has no
// Sprite, so it falls through to AsciiArt and then to the single-glyph DrawAscii
// exactly as it did before sprites existed.
//
// Only the path is stored, never a texture pointer: the renderer resolves it
// through ResourceManager, which caches successes *and* failures, so entity and
// texture lifetimes cannot dangle against one another and a bad path cannot spam
// the log once per frame. Runtime state, so it is deliberately left out of
// RegisterComponents -- like the flecs::entity handles in ActionActor.
struct Sprite {
  std::string path;
};

// Marks the map's own ground tiles, so the renderer can draw them before
// everything that stands on them.
//
// This exists because drawing order became load-bearing the moment a tile could
// have a sprite: a tile texture is full-bleed and opaque, so a floor drawn after
// a character would hide it outright, where the old half-transparent glyph plate
// would only have dimmed it. The render pipeline runs its systems in registration
// order, so one system draws the entities carrying this tag and a second draws
// the rest (see Game::ECSInitRenderSystems).
struct MapTileVisual {};

// Whats actually taken into account when handling collision
struct Hitbox {
  size_t width;
  size_t height;
};

// TODO: Right now it's not very clear who owns drawAscii,
// probably should create a resourceManager class in the future
struct Tile {
  std::string name;
  bool blocksTile;
  DrawAscii *ascii;
  Hitbox hitbox;
  // The tile's own raster texture, from a "sprite" key in the catalog entry or
  // the map's tileInfo override. Empty is the normal case: the tile then falls
  // back to data/sprites/tile_<slugified name>.png and, failing that, to its
  // ASCII glyph, exactly like an object template. See SpritePaths.hpp.
  std::string sprite;
};

// What a tile *is*, independently of the map that uses it: the entry in
// data/tiles/<slug>.json, loaded by TileRegistry.
//
// Kept separate from Tile on purpose. Tile is one map's live instance -- it owns
// a heap DrawAscii, a hitbox already scaled to that map's tile size, and an
// identity that Map::tileMap indexes by pointer. TileDef is the shared, map-free
// vocabulary: what a blueprint or a global agent means when it names a surface.
// A map's "tileInfo" entries are field-level overrides on top of a definition,
// which is why every field here is optional and has a "was it set" flag.
//
// hitboxSize is a TileDef-only field: a definition states the logical size and
// Map scales it to whatever the map's tile size is, so one definition serves
// maps with different tile dimensions.
struct TileDef {
  std::string name;
  bool blocksTile = false;
  std::string sprite;
  size_t hitboxSize = 0;
  // The file this definition came from, for reporting. Set by TileRegistry; a
  // duplicate-name warning names both files, which is what makes it fixable.
  std::string sourcePath;

  // Absent fields are how an override says "leave the catalog's value alone",
  // so presence has to survive parsing. An empty string or a zero alpha would
  // otherwise be indistinguishable from "not authored".
  bool hasCharacter = false;
  char character = '?';
  bool hasCharacterColor = false;
  Color characterColor = {255, 255, 255, 255};
  bool hasBackgroundColor = false;
  Color backgroundColor = {0, 0, 0, 0};
  bool hasHitboxSize = false;
};

struct Location {
  std::string name;
  std::string description;
  GamePosition pos;
  int width;
  int height;

  friend std::ostream &operator<<(std::ostream &os, const Location &loc) {
    os << "Location(name: " << loc.name << ", pos: " << loc.pos
       << ", size: " << loc.width << "x" << loc.height << ")";
    return os;
  }
};

enum class WindowType {
  AIChatWindowType,
  EntityInfoWindowType,
  NPCContextWindowType
};

struct WindowOnClick {
  WindowType type;
};

class Map;
struct MapResource {
  Map *map;
};

struct NameTagColor {
  Color color;
};

// Short blurb shown by the [EXAMINE] interaction. Loaded from the "description"
// key of an object's JSON template (see ObjectFactory::ApplyTemplate). Named
// ObjectDescription, not Description, to stay clear of flecs::Description.
struct ObjectDescription {
  std::string text;
};

// Authored colour for the guide: a fact about obtaining this object that the
// data model does not hold, loaded from the optional "hint" key of an object's
// JSON template (see ObjectFactory::ApplyTemplate). [INFO] prints it beside the
// raw-source line, so a raw material -- which has no recipe, and therefore
// nowhere in data/recipes to carry a hint -- can still explain itself.
//
// Deliberately absent rather than empty when a template declares none, so
// "no hint" and "a hint that happens to be empty" stay distinguishable.
struct Hint {
  std::string text;
};

// Marks an object as authored map content: it came either from the map file's
// "objects" array or from a placement made in the map editor. Deliberately NOT
// set on entities the world spawns at runtime (harvest drops, crafting output,
// NPC starting inventory), which is what lets the map writer persist the
// authored map without leaking simulation state into the file.
//
// `templateKey` is kept separately from ItemType because ItemType is restamped
// whenever a template is re-applied: an Evolvable object that grew from
// "wheat_sprout" to "wheat_mature" reports the grown id in ItemType, but the
// authored entry should still be written back as the sprout it was placed as.
struct MapAuthored {
  std::string templateKey;
};

struct ActiveWindow {
  std::shared_ptr<Window> ptr;
};

// Marks the entity that carries one inventory window's ActiveWindow, tying it
// back to the container/character it displays. Each open inventory has its own
// such entity, which is how several stay on screen at once; OpenStorageWindow
// also uses this to find an already-open window instead of stacking a duplicate.
struct StorageWindowTarget {
  flecs::entity container;
};

// Same idea for the character status window: one carrier per character, because
// a character's own ActiveWindow slot is already taken by whichever chat or
// entity-info window is open on it. Kept so reopening reuses the carrier instead
// of stacking a second window on the same character.
struct CharacterStatusWindowTarget {
  flecs::entity character;
};

struct AIBackend {
  std::unique_ptr<AI> ptr;
};

struct AgentSleepTimer {
  float time_remaining_ms;
};

struct CharacterTag {};

struct NPCContext {
  std::vector<ChatMessage> history;
  std::string contextID;
};

struct AIRequest {
  std::string prompt;
  bool finished = false;
  std::string pendingResponse;
  bool dispatched = false;
  std::stop_source stopSource;
};

struct MovingTowards {};

struct Action {};

struct DO_NOTHING_ACTION {
  float time_remaining; // In seconds
};

struct MOVE_THROUGH_PATH_ACTION {
  std::vector<GamePosition> path;
}; 

struct MOVE_TO_CHARACTER_ACTION {
  std::string name;
};

struct TALK_TO_ACTION {
  std::string name;
};

struct MOVE_TO_LOCATION_ACTION {
  Location *location;
};

class AgentBrain;

struct AgentBrainWrapper {
  std::unique_ptr<AgentBrain> agBrain;
};

struct CHARACTERS_QUERY {};

struct INVALID_ACTION {};

// ---------------------------------------------------------------------------
// Agent task lists: declared intention, at three horizons.
//
// See docs/task-system-design.md. This is *intent*, not execution: AgentAction
// (AgentActions.h) is what the agent is actually doing, TaskList is what it
// says it is trying to do. The two are deliberately separate and both exist,
// so the engine can render, validate and debug intention without touching the
// action path. Nothing here schedules anything, and an entity with no TaskList
// behaves exactly as it did before this component existed.
// ---------------------------------------------------------------------------

// How far ahead a goal reaches. Terminal and LongTerm goals carry a description
// and a parent edge but no kind and no target -- only Short tasks name
// something the engine can resolve and check. Terminal goals are authored in
// the map JSON and are read-only to the agent; the agent owns everything below
// them.
enum class GoalHorizon { Terminal, LongTerm, Short };

// Named buckets, not a numeric scale: a model chooses between labels more
// reliably than it produces calibrated numbers, and a bounded enum is
// checkable, loggable and diffable. Set by the agent, never rewritten by the
// engine -- violations are reported (see ruleWarning), not applied.
enum class TaskPriority { Critical, High, Normal, Low, Background };

struct AgentGoal {
  // Agent-authored, free text. This is what a human reads in the debug window.
  std::string description;

  GoalHorizon horizon = GoalHorizon::Short;

  // Index into TaskList::goals, or -1. "This goal serves that one." Proposed
  // by the agent -- the engine never derives this edge, because "I greet people
  // because I want standing, because I want to matter" is reasoning about
  // meaning, not a predicate over world state. The engine's only involvement is
  // structural: does the index resolve, and does the chain terminate.
  int parent = -1;

  // ---- Authored by the agent ----
  // Empty for Terminal and LongTerm goals: only Short tasks name a kind and a
  // target. An unknown kind degrades to "no facts", never to "task rejected",
  // so the agent can still express an intent the engine has not anticipated.
  std::string kind;    // task kind, from TaskKindRegistry
  std::string target;  // human-readable target, resolved by the engine
  TaskPriority tier = TaskPriority::Normal;

  // ---- Written by the engine. The agent may not set these. ----
  // Observed world-state predicates, never priorities.
  std::vector<std::string> facts;
  // Rule violation report (e.g. the anti-inflation rule), never a corrected
  // value: the engine reports, the agent judges, and `tier` above keeps
  // whatever the agent said.
  std::string ruleWarning;
  int createdTick = 0;
  int lastAdvancedTick = 0;
  int id = 0;  // stable identity; TaskList::activeTaskId refers to this
};

struct TaskList {
  std::vector<AgentGoal> goals;  // terminal first, then long-term, then short
  int activeTaskId = -1;         // the goal the current activity is serving

  // List-level rule violation, written by the engine, never a corrected value.
  // Entry-level violations use AgentGoal::ruleWarning instead; the two are
  // separate fields precisely because coherence is a property of the list, not
  // of one entry.
  std::string ruleWarning;
};

// Object Components
struct Interactable {
  bool active = true;
};

struct LootTable {
  std::vector<LootDrop> drops;
};

struct Harvestable {
  int amountRemaining;
  LootTable lootTable;
  float timer = 0.0f;
  // Durability removed from each tool used for one harvest of this object.
  // Stamped by ObjectFactory from the template's "durabilityCost", defaulting to
  // DEFAULT_HARVEST_DURABILITY_COST, so every harvestable has a cost without
  // every template having to say one.
  int durabilityCost = 0;
};

struct ActionActor { flecs::entity actor; };
struct ActionTarget { flecs::entity target; };
struct ActionTimer {
  // Seconds left on the action. ActionTimerSystem owns counting this down; it is
  // the only mutable part of the timer.
  float timeRemaining;

  ActionTimer() : timeRemaining(0.0f), duration(0.0f) {}

  // An action always starts full, so its initial remaining time *is* the duration.
  // Callers only pass the total time and duration is filled in automatically.
  ActionTimer(float seconds) : timeRemaining(seconds), duration(seconds) {}

  // 0 when the action just started, 1 when the timer runs out.
  // Deliberately the ONLY reader of `duration`.
  float Progress() const {
    if (duration <= 0.0f) {
      return 1.0f;
    }
    float progress = 1.0f - timeRemaining / duration;
    if (progress < 0.0f) {
      return 0.0f;
    }
    if (progress > 1.0f) {
      return 1.0f;
    }
    return progress;
  }

private:
  // A snapshot of timeRemaining taken when the action started. This is NOT
  // independent state and must never be edited after construction: it exists
  // solely so Progress() can turn the countdown into a fill fraction. It is
  // private on purpose, so that "editing the duration" is a compile error
  // instead of a silent animation bug.
  float duration;
};

struct ActionCompleted {};
struct ActionCanceled {};

struct HarvestAction {};

// A timed craft in progress, on the action entity that Busy points at. Unlike
// HarvestAction this is not a bare tag: the recipe definition lives only in
// data/recipes/*.json, so the id is carried here and resolved again when the
// timer runs out (CraftResolutionSystem). An id, never a CraftRecipe*: the
// registry is allowed to reload and invalidate its pointers.
//
// `count` is how many times the recipe is executed as one batch, so a single
// timed craft can yield e.g. 3 loaves. It is at least 1. The ActionTimer is
// already the *total* wait (recipe.craftTimeSeconds * count), and
// CraftResolutionSystem spends the inputs and rolls the outputs `count` times.
struct CraftAction {
  std::string recipeId;
  int count = 1;
};

struct Busy { flecs::entity actionEntity; };

struct Workstation {
  std::vector<std::string> recipes;
};

struct Obstacle {
  bool blocksMovement = true;
};

struct Storage {
  int capacity = 10;
};


struct Portable {
  bool canBePickedUp = true;
};

// Declared by a template's "placement" block and stamped by ObjectFactory. Its
// presence marks the item as something [PLACE] can set down in the world; the
// fields are the rules. The verb itself knows nothing about particular items, so
// "planting" is just placement where the placed form happens to grow. See
// docs/placement-design.md.
struct Placeable {
  // Template id the item becomes once placed. It must not itself be Portable,
  // or the object would stay an item and could be picked up again.
  std::string becomes;
  // Allowed Tile::name values ("tilled soil", "grass", ...). Empty means any
  // tile that does not block.
  std::vector<std::string> surface;
  // Chebyshev distance from the actor the target tile must fall within. Filled
  // in by ObjectFactory from DEFAULT_PLACEMENT_MAX_DISTANCE when the template
  // does not say.
  int maxDistance = 1;
};

// Capability gate declared by an object template's "requires" block. A verb that
// honours it refuses to run until the actor satisfies it, rather than silently
// doing nothing -- which is what "you need a scythe to cut wheat" should be.
//
// The equipment clause is expressed in item TAGS (see ItemDef::tags), not item
// ids or slot names, so a requirement names a capability ("scythe") and any item
// carrying that tag satisfies it. This is the "requires" form section 3.7 of the
// effect design reserved for exactly this case; skill and stat clauses attach
// here later.
struct Requires {
  // Item tags the actor must have equipped. Empty means the requirement has no
  // equipment clause (and is therefore always met).
  std::vector<std::string> equippedTags;
};

struct InteractionTarget {
  flecs::entity targetEntity;
};

struct PendingPlayerInteraction {
  flecs::entity targetEntity;
  std::string interactionName;
};

struct Evolvable {
  float timeRemaining;
  std::string nextStageTemplate;
  bool isActive = true;
};

// What harvesting this object teaches, and by how much. The subject-side half of
// skill progression: the skill itself only declares which activities may train
// it, while the amount and the choice of skill live next to the thing being
// harvested. Absent on objects that teach nothing.
//
// Same shape and purpose as CraftRecipe::trains, which is the recipe-side half.
struct Trains {
  std::vector<SkillXp> grants;
};


struct LastObjectsQuery {
  std::vector<flecs::entity> objects;
};

class ObjectFactory;
struct ObjectFactoryResource {
  ObjectFactory* factory;
};

class RecipeRegistry;
// Crafting recipe definitions, loaded once at init from data/recipes/*.json and
// read by whoever resolves a Workstation's recipe ids (see RecipeRegistry.h).
// Same resource-component pattern as ObjectFactoryResource.
struct RecipeRegistryResource {
  RecipeRegistry* registry;
};

class StatRegistry;
// Character stat definitions, loaded once at init from data/stats/*.json and
// read through StatView (see StatRegistry.h, StatView.hpp). Same
// resource-component pattern as RecipeRegistryResource: components carry stat
// ids and values, definitions live only in the registry.
struct StatRegistryResource {
  StatRegistry* registry;
};

class SkillRegistry;
// Skill definitions, loaded once at init from data/skills/*.json (see
// SkillRegistry.h). Same resource-component pattern as StatRegistryResource.
struct SkillRegistryResource {
  SkillRegistry* registry;
};

class HookRegistry;
// Named C++ effects, registered once at init and referenced by id from stat data
// (see Hooks.hpp). Same resource-component pattern as StatRegistryResource.
struct HookRegistryResource {
  HookRegistry* hooks;
};

class TileRegistry;
// The world's tile vocabulary, loaded once at init from data/tiles/*.json (see
// TileRegistry.h). Read by Map when it builds its uniqueTiles (a map's tileInfo
// is a set of overrides on top of these definitions) and by anything that needs
// to name a surface without owning a map -- blueprints, and the designer agent.
// Same resource-component pattern as StatRegistryResource.
struct TileRegistryResource {
  TileRegistry* registry;
};

class BlueprintRegistry;
// Building designs, loaded once at init from data/blueprints/*.json (see
// BlueprintRegistry.h). Read by the blueprint placement window, and later by the
// designer agent and by any agent that wants to build. Same resource-component
// pattern as TileRegistryResource.
struct BlueprintRegistryResource {
  BlueprintRegistry* registry;
};

class TaskKindRegistry;
// The task shapes the engine knows how to check, populated once at init (see
// TaskKindRegistry.h). Not loaded from JSON like the content registries: a check
// is C++, so this is where a genre extends the task vocabulary in code. Same
// resource-component pattern as BlueprintRegistryResource.
struct TaskKindRegistryResource {
  TaskKindRegistry* registry;
};

class ItemRegistry;
// Equippable item definitions, loaded once at init from data/items/*.json (see
// ItemRegistry.h). Same resource-component pattern as StatRegistryResource:
// components carry item type ids, definitions live only in the registry.
struct ItemRegistryResource {
  ItemRegistry* registry;
};

class RaceRegistry;
// Race definitions, loaded once at init from data/races/*.json (see
// RaceRegistry.h). Supplies the body slots a character is born with, which is
// what makes an NPC's slots a data question rather than an engine one.
struct RaceRegistryResource {
  RaceRegistry* registry;
};

// A live trade proposal between exactly two agents. Runtime-only: it holds
// entity handles, is never serialized, and is deliberately NOT registered with
// ecs.component<>. It lives on the *offerer*; the responder finds it through its
// conversation partner.
//
// Terms are ItemStacks resolved against real inventories only when the offer is
// accepted, so an item consumed in the meantime cannot leave a dangling handle.
// One offer per conversation: a counter-offer replaces the previous one.
struct TradeOffer {
  flecs::entity offerer;
  flecs::entity responder;
  std::vector<ItemStack> give;    // offerer -> responder if accepted
  std::vector<ItemStack> receive; // responder -> offerer if accepted
};

// Seed for all gameplay randomness, plus a counter that hands each engine-owned
// roll group its own stream.
//
// Every roll is derived from this rather than from a process-global generator,
// so a run is reproducible from the logged seed and no call site can perturb
// another's outcome. `groups` advances once per roll group: deriving each group
// from its own counter value is what stops adding a new roll site from changing
// the results of every existing one.
//
// Plain integers rather than an engine object on purpose: the component stays
// trivially copyable and holds nothing that could not be serialized.
struct WorldRandomness {
  uint64_t seed = 0;
  uint64_t groups = 0;
};

// Reusable reflection support for std::vector
template <typename Elem, typename Vector = std::vector<Elem>>
inline flecs::opaque<Vector, Elem> std_vector_support(flecs::world &world) {
  return flecs::opaque<Vector, Elem>()
      .as_type(world.vector<Elem>())
      .serialize([](const flecs::serializer *s, const Vector *data) {
        for (const auto &el : *data) {
          s->value(el);
        }
        return 0;
      })
      .count([](const Vector *data) { return data->size(); })
      .resize([](Vector *data, size_t size) { data->resize(size); })
      .ensure_element([](Vector *data, size_t elem) {
        if (data->size() <= elem) {
          data->resize(elem + 1);
        }
        return &data->data()[elem];
      });
}

inline void RegisterComponents(flecs::world &ecs) {
  // std::string must be registered before any component that reflects a
  // std::string member. flecs resolves a member's type at registration time, so
  // registering "X.member<std::string>" first leaves the member pointing at a
  // type with no EcsMetaType and permanently breaks serialization of X with
  // "missing EcsMetaType for type std.__cxx11.basic_string<char>".
  ecs.component<std::string>()
      .opaque(flecs::String)
      .serialize([](const flecs::serializer *s, const std::string *data) {
        const char *str = data->c_str();
        return s->value(flecs::String, &str);
      })
      .assign_string(
          [](std::string *data, const char *value) { *data = value; });

  ecs.component<Render>();

  ecs.component<raylib::Vector2>().member<float>("x").member<float>("y");

  ecs.component<ScreenPosition>().member<float>("x").member<float>("y");

  ecs.component<GamePosition>().member<int>("x").member<int>("y");

  ecs.component<MaxSpeed>().member<float>("value");

  ecs.component<Velocity>().member<float>("x").member<float>("y");

  ecs.component<Acceleration>().member<float>("x").member<float>("y");

  ecs.component<Friction>().member<float>("value");

  ecs.component<BlocksTile>();
  ecs.component<Intangible>();

  ecs.component<raylib::Rectangle>()
      .member<float>("x")
      .member<float>("y")
      .member<float>("width")
      .member<float>("height");

  ecs.component<Frame>()
      .member<raylib::Rectangle>("frameRect")
      .member<double>("duration");

  ecs.component<std::vector<GamePosition>>().opaque(
      std_vector_support<GamePosition>);

  ecs.component<MOVE_THROUGH_PATH_ACTION>().member<std::vector<GamePosition>>("path");

  ecs.component<std::vector<Frame>>().opaque(std_vector_support<Frame>);

  // StatBlock reflection. StatValue is registered before the vector that holds
  // it, for the same reason std::string is registered before anything that
  // reflects it: flecs resolves a member's type at registration time, so
  // reflecting vector<StatValue> first would leave it pointing at a type with no
  // EcsMetaType.
  ecs.component<StatValue>().member<std::string>("id").member<float>("value");
  ecs.component<std::vector<StatValue>>().opaque(
      std_vector_support<StatValue>);
  ecs.component<StatBlock>().member<std::vector<StatValue>>("values");

  // SkillBlock reflection, registered on the same terms and for the same reason:
  // SkillValue before the vector that holds it.
  ecs.component<SkillValue>()
      .member<std::string>("id")
      .member<int>("level")
      .member<int>("xp");
  ecs.component<std::vector<SkillValue>>().opaque(
      std_vector_support<SkillValue>);
  ecs.component<SkillBlock>().member<std::vector<SkillValue>>("values");

  ecs.component<SkillXp>().member<std::string>("skillId").member<int>("xp");
  ecs.component<std::vector<SkillXp>>().opaque(std_vector_support<SkillXp>);
  ecs.component<Trains>().member<std::vector<SkillXp>>("grants");

  // Equipment reflection. SlotSpec is registered before the vector that holds
  // it for the same reason as StatValue above: flecs resolves a member's type at
  // registration time, so reflecting vector<SlotSpec> first would leave it
  // pointing at a type with no EcsMetaType.
  ecs.component<SlotSpec>()
      .member<std::string>("id")
      .member<std::string>("accepts");
  ecs.component<std::vector<SlotSpec>>().opaque(std_vector_support<SlotSpec>);
  ecs.component<EquipmentSlots>().member<std::vector<SlotSpec>>("slots");

  // The instances an item occupies, by slot id. A plain vector<string>, so it is
  // registered here once and shared by every future component that needs one.
  ecs.component<std::vector<std::string>>().opaque(
      std_vector_support<std::string>);
  ecs.component<Equipped>().member<std::vector<std::string>>("slots");
  ecs.component<Requires>().member<std::vector<std::string>>("equippedTags");
  ecs.component<Durability>().member<int>("current").member<int>("max");

  // Task-list reflection (docs/task-system-design.md section 10), registered on
  // the same terms as the blocks above. The two enum types are registered
  // before AgentGoal, and AgentGoal before the vector that holds it, because
  // flecs resolves a member's type at registration time -- reflecting
  // vector<AgentGoal> first would leave it pointing at a type with no
  // EcsMetaType. This block sits below the vector<string> registration above
  // for the same reason: AgentGoal::facts is a vector<string>.
  ecs.component<GoalHorizon>();
  ecs.component<TaskPriority>();
  ecs.component<AgentGoal>()
      .member<std::string>("description")
      .member<GoalHorizon>("horizon")
      .member<int>("parent")
      .member<std::string>("kind")
      .member<std::string>("target")
      .member<TaskPriority>("tier")
      .member<std::vector<std::string>>("facts")
      .member<std::string>("ruleWarning")
      .member<int>("createdTick")
      .member<int>("lastAdvancedTick")
      .member<int>("id");
  ecs.component<std::vector<AgentGoal>>().opaque(std_vector_support<AgentGoal>);
  ecs.component<TaskList>()
      .member<std::vector<AgentGoal>>("goals")
      .member<int>("activeTaskId")
      .member<std::string>("ruleWarning");

  ecs.component<Color>()
      .member<unsigned char>("r")
      .member<unsigned char>("g")
      .member<unsigned char>("b")
      .member<unsigned char>("a");

  ecs.component<ActiveWindow>();
  ecs.component<StorageWindowTarget>();
  ecs.component<CharacterStatusWindowTarget>();
  ecs.component<PendingPlayerInteraction>();
  ecs.component<LastObjectsQuery>();

  // Singleton resource: registered so it exists as a named type before ECSInit
  // seeds it. Members are not reflected, so nothing here needs the std::string
  // registration above.
  ecs.component<WorldRandomness>();

  ecs.component<MapAuthored>().member<std::string>("templateKey");

  ecs.component<Tile>()
      .member<std::string>("name")
      .member<char>("ch")
      .member<Color>("characterColor")
      .member<Color>("backgroundColor");
}
