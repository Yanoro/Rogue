#pragma once

#include "AI.h"
#include "raylib-cpp.hpp"
#include <cstdint>
#include <flecs.h>
#include <memory>
#include <string>
#include <vector>

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
