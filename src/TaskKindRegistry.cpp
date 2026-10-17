#include "TaskKindRegistry.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "Components.h"
#include "EquipmentRuntime.hpp"
#include "ItemRegistry.h"
#include "Map.h"
#include "RecipeRegistry.h"
#include "StringUtils.hpp"

// The checking predicates (docs/task-system-design.md sections 7 and 8).
//
// Every one of these is a predicate over world state that returns observed facts.
// None of them knows about tiers, importance, or what the agent should want: the
// engine reports, the agent judges (section 6). A fact that says the engine could
// not observe anything is phrased as such ("not ripe yet", "no tilled soil within
// 1 tile") rather than as a failure, because the task is still the agent's to
// keep, drop or replace.
namespace {

// What to call an entity in a fact. Prefers the player-facing name and falls back
// to the content id, so a fact still reads sensibly for an unnamed object.
std::string SubjectLabel(flecs::entity entity) {
  if (const DisplayName *name = entity.get<DisplayName>()) {
    if (!name->name.empty()) return name->name;
  }
  if (const ItemType *type = entity.get<ItemType>()) {
    if (!type->id.empty()) return type->id;
  }
  return "the target";
}

// "a, b or c", the same shape the placement verb uses to list allowed surfaces.
std::string JoinWithOr(const std::vector<std::string> &values) {
  std::string joined;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) joined += (i + 1 == values.size()) ? " or " : ", ";
    joined += values[i];
  }
  return joined;
}

// Whether the agent holds every input of a recipe. Mirrors the claim the craft
// verb itself makes, so a fact can never promise a craft the verb would refuse.
bool HoldsEveryInput(flecs::entity agent, const CraftRecipe &recipe) {
  for (const ItemStack &input : recipe.inputs) {
    int held = 0;
    agent.each<Holds>([&](flecs::entity child) {
      if (held >= input.count || !child.is_alive()) return;
      const ItemType *type = child.get<ItemType>();
      if (type && StringUtils::EqualsIgnoreCase(type->id, input.item)) {
        ++held;
      }
    });
    if (held < input.count) return false;
  }
  return true;
}

// harvest: an object with Harvestable, ripe when it has no stage left to grow
// into. A stage that is still growing has an Evolvable with somewhere to go; the
// final stage either has no Evolvable at all (wheat_mature) or one that is
// inactive, which is what makes "ripe = final stage" a predicate rather than a
// naming convention.
std::vector<std::string> CheckHarvest(flecs::entity, flecs::entity target) {
  if (!target.is_alive()) return {"target not found"};

  const std::string label = SubjectLabel(target);
  if (!target.has<Harvestable>()) {
    return {label + " cannot be harvested"};
  }

  const Evolvable *growth = target.get<Evolvable>();
  const bool stillGrowing =
      growth != nullptr && growth->isActive && !growth->nextStageTemplate.empty();
  if (stillGrowing) {
    return {label + " is not ripe yet"};
  }
  return {label + " is ripe"};
}

// plant: the target is the seed the agent carries; the question is whether any
// ground it could legally be set down on is near. The rule is deliberately the
// same one the placement verb enforces -- within maxDistance, in bounds, not
// blocking, on one of the declared surfaces, and not already occupied -- so a
// fact can never promise a placement the verb would reject.
std::vector<std::string> CheckPlant(flecs::entity agent, flecs::entity target) {
  if (!target.is_alive()) return {"target not found"};

  const std::string label = SubjectLabel(target);
  const Placeable *policy = target.get<Placeable>();
  if (policy == nullptr) {
    return {label + " cannot be planted"};
  }

  const MapResource *mapRes = agent.world().get<MapResource>();
  Map *map = mapRes ? mapRes->map : nullptr;
  const GamePosition *here = agent.get<GamePosition>();
  if (map == nullptr || here == nullptr) {
    return {label + " has nowhere to be planted from here"};
  }

  // Everything already standing on the map, gathered once rather than rescanned
  // per candidate tile.
  std::vector<GamePosition> occupied;
  agent.world().filter<GamePosition, DisplayName>().each(
      [&](flecs::entity other, const GamePosition &pos, const DisplayName &) {
        if (other != agent) occupied.push_back(pos);
      });

  const std::string wanted = policy->surface.empty() ? std::string("open ground")
                                                     : JoinWithOr(policy->surface);

  for (int dx = -policy->maxDistance; dx <= policy->maxDistance; ++dx) {
    for (int dy = -policy->maxDistance; dy <= policy->maxDistance; ++dy) {
      if (dx == 0 && dy == 0) continue;  // [PLACE] refuses "you are standing there"
      const int x = here->x + dx;
      const int y = here->y + dy;
      if (!map->IsInBounds(static_cast<float>(x), static_cast<float>(y))) continue;

      Tile *tile = map->GetTile(x, y);
      if (tile == nullptr || tile->blocksTile) continue;

      const std::string ground = tile->name.empty() ? "unknown ground" : tile->name;
      if (!policy->surface.empty() &&
          std::find(policy->surface.begin(), policy->surface.end(), ground) ==
              policy->surface.end()) {
        continue;
      }

      bool taken = false;
      for (const GamePosition &pos : occupied) {
        if (pos.x == x && pos.y == y) {
          taken = true;
          break;
        }
      }
      if (taken) continue;

      return {ground + " within " + std::to_string(policy->maxDistance) +
              " tile(s) is free"};
    }
  }

  return {"no " + wanted + " within " + std::to_string(policy->maxDistance) +
          " tile(s)"};
}

// equip: a Portable the agent holds. Reports whether it is already worn and, when
// it is not, whether the body has a free slot of the kind the item needs. Slot
// matching is the same string comparison the equip verb makes, so "main_hand" and
// "off_hand" both satisfy a "hand" item.
std::vector<std::string> CheckEquip(flecs::entity agent, flecs::entity target) {
  if (!target.is_alive()) return {"target not found"};

  const std::string label = SubjectLabel(target);
  if (!target.has<Portable>()) {
    return {label + " cannot be equipped"};
  }

  const Equipped *worn = target.get<Equipped>();
  if (worn != nullptr && !worn->slots.empty()) {
    return {label + " is already equipped"};
  }

  const EquipmentSlots *body = agent.get<EquipmentSlots>();
  if (body == nullptr || body->slots.empty()) {
    return {"no body slots to equip " + label + " into"};
  }

  const ItemRegistry *items = ItemsOf(agent.world());
  const ItemType *type = target.get<ItemType>();
  const ItemDef *def =
      (items != nullptr && type != nullptr) ? items->Get(type->id) : nullptr;
  if (def == nullptr || def->slot.empty()) {
    return {label + " does not say which slot it needs"};
  }

  // Which instance ids are already taken, by plain string comparison: occupancy
  // travels with the item (Equipped lives on the item), so nothing has to be
  // trusted to be in sync.
  std::vector<std::string> occupied;
  agent.each<Holds>([&](flecs::entity child) {
    if (!child.is_alive()) return;
    const Equipped *slots = child.get<Equipped>();
    if (slots == nullptr) return;
    occupied.insert(occupied.end(), slots->slots.begin(), slots->slots.end());
  });

  int freeSlots = 0;
  for (const SlotSpec &slot : body->slots) {
    if (slot.accepts != def->slot) continue;
    if (std::find(occupied.begin(), occupied.end(), slot.id) != occupied.end()) {
      continue;
    }
    ++freeSlots;
  }

  if (freeSlots > 0) {
    return {def->slot + " slot is free"};
  }
  return {"every " + def->slot + " slot is taken"};
}

// craft: a Workstation. Names the first recipe the agent could actually start, so
// the fact is an instruction the agent can act on rather than a station
// inventory. A station whose recipes are all out of reach reports the shortage
// rather than nothing, because "missing materials for Flour" is the useful
// observation.
std::vector<std::string> CheckCraft(flecs::entity agent, flecs::entity target) {
  if (!target.is_alive()) return {"target not found"};

  const std::string label = SubjectLabel(target);
  const Workstation *station = target.get<Workstation>();
  if (station == nullptr) {
    return {label + " is not a workstation"};
  }

  const RecipeRegistryResource *regRes =
      agent.world().get<RecipeRegistryResource>();
  const RecipeRegistry *recipes = regRes ? regRes->registry : nullptr;
  if (recipes == nullptr || station->recipes.empty()) {
    return {label + " has no recipes"};
  }

  std::string firstKnown;
  for (const std::string &recipeId : station->recipes) {
    const CraftRecipe *recipe = recipes->Get(recipeId);
    if (recipe == nullptr) continue;
    if (firstKnown.empty()) firstKnown = recipe->name;
    if (HoldsEveryInput(agent, *recipe)) {
      return {"can make " + recipe->name + " here"};
    }
  }

  if (firstKnown.empty()) {
    return {label + " has no known recipes"};
  }
  return {"missing materials for " + firstKnown};
}

}  // namespace

void TaskKindRegistry::RegisterDefaults() {
  // The four kinds the engine can check today, all backed by components that
  // already exist (docs/task-system-design.md section 7). Each carries the
  // subject filter that lets a place-name target resolve to a real thing, and the
  // predicate that turns the resolved target into facts.
  Register({"harvest",
            "gather from an object that yields a resource when ripe or full",
            CheckHarvest, [](flecs::entity e) { return e.has<Harvestable>(); }});
  Register({"plant", "sow a seed you carry into eligible ground", CheckPlant,
            [](flecs::entity e) { return e.has<Placeable>(); }});
  Register({"equip",
            "put a portable item you hold into the body slot that accepts it",
            CheckEquip, [](flecs::entity e) { return e.has<Portable>(); }});
  Register({"craft", "turn materials into something else at a workstation",
            CheckCraft, [](flecs::entity e) { return e.has<Workstation>(); }});
}

void TaskKindRegistry::Register(TaskKind kind) {
  if (kind.id.empty()) return;
  const std::string id = kind.id;
  kinds[id] = std::move(kind);
}

const TaskKind* TaskKindRegistry::Get(const std::string& id) const {
  const auto it = kinds.find(id);
  if (it == kinds.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> TaskKindRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(kinds.size());
  for (const auto& entry : kinds) {
    ids.push_back(entry.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<const TaskKind*> TaskKindRegistry::GetAll() const {
  std::vector<const TaskKind*> all;
  all.reserve(kinds.size());
  for (const std::string& id : GetIds()) {
    all.push_back(Get(id));
  }
  return all;
}
