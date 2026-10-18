#include "TaskKindRegistry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "Components.h"
#include "EquipmentRuntime.hpp"
#include "ItemRegistry.h"
#include "Map.h"
#include "PlaceQuery.hpp"
#include "RecipeRegistry.h"
#include "StringUtils.hpp"
#include "VisionQuery.hpp"

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
      if (tile == nullptr || map->IsBlocked(x, y)) continue;

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

// reach: the agent has to be *at* something rather than do something to it. The
// target may be an entity (a character, a building) or a remembered place; this
// branch covers the entity, CheckReachPlace the place.
//
// The fact states a distance, never "done": section 6 says the engine reports and
// the agent judges, and "am I close enough" is the judgement the agent is
// actually making. One tile is the arrival radius the move verb already uses.
std::vector<std::string> CheckReach(flecs::entity agent, flecs::entity target) {
  if (!target.is_alive()) return {"target not found"};

  const std::string label = SubjectLabel(target);
  const GamePosition *here = agent.get<GamePosition>();
  const GamePosition *there = target.get<GamePosition>();
  if (here == nullptr || there == nullptr) {
    return {label + " has no place to stand next to"};
  }

  const int dx = there->x - here->x;
  const int dy = there->y - here->y;
  const int distance = std::max(std::abs(dx), std::abs(dy));
  if (distance <= 1) {
    return {label + " is within reach"};
  }
  return {label + " is " + std::to_string(distance) + " tile(s) " +
          PlaceQuery::CompassWord(dx, dy)};
}

// reach, place branch: the agent remembers a place by that name. "Arrived" means
// standing on ground the place claims, which is the only definition that needs no
// map-wide geometry -- the claimed cells are already the agent's own observation
// (place-derivation doc section 9).
std::vector<std::string> CheckReachPlace(flecs::entity agent,
                                         const KnownPlace &place) {
  const std::string label = PlaceQuery::DisplayName(place);
  const GamePosition *here = agent.get<GamePosition>();
  const SpatialMemory *memory = agent.get<SpatialMemory>();
  if (here == nullptr || memory == nullptr) {
    return {"you cannot tell where " + label + " is"};
  }

  const std::uint64_t hereKey = VisionQuery::CellKey(*here);
  if (std::find(place.memberCells.begin(), place.memberCells.end(), hereKey) !=
      place.memberCells.end()) {
    return {"you are in " + label};
  }

  // The nearest claimed cell, preferring one the character has seen to be
  // walkable. Deliberately *not* PlaceQuery::ApproachCell: that answers "where
  // should I walk to", and for a linear place it returns the far end. A reach
  // fact wants the closest part of the place, which is the distance the agent is
  // judging.
  const auto nearestClaimed = [&memory, here, &place](bool walkableOnly) {
    GamePosition best = *here;
    int bestDistance = -1;
    for (const std::uint64_t key : place.memberCells) {
      const auto cell = memory->cells.find(key);
      if (cell == memory->cells.end()) continue;
      if (walkableOnly && !cell->second.walkable) continue;
      const GamePosition &pos = cell->second.pos;
      const int distance =
          std::max(std::abs(pos.x - here->x), std::abs(pos.y - here->y));
      if (bestDistance < 0 || distance < bestDistance) {
        bestDistance = distance;
        best = pos;
      }
    }
    return best;
  };

  GamePosition approach = nearestClaimed(true);
  if (approach == *here) {
    approach = nearestClaimed(false);
  }
  if (approach == *here) {
    return {"you are in " + label};
  }
  return {"the nearest part of " + label + " is " +
          PlaceQuery::DistancePhrase(*here, approach)};
}

}  // namespace

void TaskKindRegistry::RegisterDefaults() {
  // The kinds the engine can check today, all backed by components that already
  // exist (docs/task-system-design.md section 7). Each carries the subject filter
  // that lets a place-name target resolve to a real thing, and the predicate that
  // turns the resolved target into facts.
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
  // reach is the "finding" kind: the target may be a remembered place, which is
  // not an entity, so it carries a place predicate as well as an entity one.
  // [MOVE_TO] answers "how do I get there"; this answers "am I there".
  Register({"reach",
            "go to a place you remember, or to a character or thing you can name",
            CheckReach, nullptr, CheckReachPlace});
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
