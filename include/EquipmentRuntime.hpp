#pragma once

#include <string>
#include <vector>

#include <flecs.h>

#include "Components.h"
#include "Equipment.hpp"
#include "ItemRegistry.h"
#include "StringUtils.hpp"

// The flecs-side half of equipment: finding the entities and mutating their
// components. The decision rules themselves live in Equipment.hpp, which has no
// flecs dependency and is unit tested on its own.
//
// Included by the game build only (AgentActions.h and GameECS.cpp); the
// resolution tests exercise Equipment.hpp and SourceGatherer.h directly, so they
// never pull in raylib.

// A carried item, by its player-facing name. Only Holds children are searched,
// so an item sitting in a chest is not "held" and cannot be equipped from there.
inline flecs::entity FindHeldItem(flecs::entity actor, const std::string& name) {
  flecs::entity found = flecs::entity::null();
  actor.each<Holds>([&](flecs::entity child) {
    if (!child.is_alive() || !child.has<DisplayName>()) return;
    if (StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, name)) {
      found = child;
    }
  });
  return found;
}

// Every held item that is currently worn, in inventory order. Inventory order is
// the order Holds yields, which is stable enough to print.
inline std::vector<flecs::entity> EquippedItems(flecs::entity actor) {
  std::vector<flecs::entity> worn;
  actor.each<Holds>([&](flecs::entity child) {
    if (child.is_alive() && child.has<Equipped>()) {
      worn.push_back(child);
    }
  });
  return worn;
}

// The capability tags of every worn item, one list per item, in inventory order.
inline std::vector<std::vector<std::string>> EquippedItemTags(
    flecs::entity actor, const ItemRegistry* items) {
  std::vector<std::vector<std::string>> tags;
  if (!items) {
    return tags;
  }
  actor.each<Holds>([&](flecs::entity child) {
    if (!child.is_alive() || !child.has<Equipped>()) return;
    const ItemType* type = child.get<ItemType>();
    const ItemDef* def = type ? items->Get(type->id) : nullptr;
    tags.push_back(def ? def->tags : std::vector<std::string>{});
  });
  return tags;
}

// The first required tag the actor's worn gear does not provide, or empty when
// the requirement is met. This is what an object's `requires` gate asks.
inline std::string FirstMissingEquippedTag(
    flecs::entity actor, const std::vector<std::string>& required,
    const ItemRegistry* items) {
  if (required.empty()) {
    return "";
  }
  return FirstMissingTag(required, EquippedItemTags(actor, items));
}

// The item registry from the world, or nullptr in a half-initialised world. The
// fallback keeps the failure mode "nothing can be equipped" rather than a crash.
inline const ItemRegistry* ItemsOf(flecs::world world) {
  auto res = world.get<ItemRegistryResource>();
  return (res && res->registry) ? res->registry : nullptr;
}

// What to call an item in a message. Prefers the object's display name and
// falls back to its content id, so an unnamed item still reads sensibly.
inline std::string ItemLabel(flecs::entity item) {
  if (item.has<DisplayName>()) {
    return item.get<DisplayName>()->name;
  }
  if (const ItemType* type = item.get<ItemType>()) {
    return type->id;
  }
  return "item";
}

// " (equipped: main_hand, off_hand)", with the wear state appended when the item
// has any, or empty for an unworn item. Used by [INVENTORY] so worn gear is
// still listed but clearly marked.
inline std::string EquippedSuffix(flecs::entity item) {
  const Equipped* equipped = item.get<Equipped>();
  if (!equipped || equipped->slots.empty()) {
    return "";
  }
  std::string suffix = " (equipped: " + JoinSlotIds(equipped->slots);
  if (const Durability* durability = item.get<Durability>()) {
    suffix += "; " + std::to_string(durability->current) + "/" +
              std::to_string(durability->max) + " durability";
  }
  suffix += ")";
  return suffix;
}

// The two things a caller wants from an equip attempt: whether it happened, and
// what to tell the actor either way. A bool plus a message rather than one or
// the other, because the [EQUIP] command reports both outcomes to the agent
// while a spawn-time equip only cares whether to log a warning.
struct EquipOutcome {
  bool ok = false;
  std::string message;
};

// Equips an already-held item, auto-swapping whatever occupies the slots it
// needs (the PlanEquip rule).
inline EquipOutcome EquipItem(flecs::entity actor, flecs::entity item,
                              const ItemRegistry* items) {
  const std::string name = ItemLabel(item);

  if (item.has<Equipped>()) {
    return {false, "System: You are already wearing " + name + ".\n"};
  }
  if (!items) {
    return {false, "System: You cannot equip " + name + " right now.\n"};
  }

  const ItemType* type = item.get<ItemType>();
  const ItemDef* def = type ? items->Get(type->id) : nullptr;
  if (!def) {
    return {false, "System: " + name + " cannot be equipped.\n"};
  }

  const EquipmentSlots* body = actor.get<EquipmentSlots>();
  if (!body || body->slots.empty()) {
    return {false, "System: You have nowhere to wear " + name + ".\n"};
  }

  const std::vector<flecs::entity> worn = EquippedItems(actor);
  std::vector<std::vector<std::string>> wornSlots;
  wornSlots.reserve(worn.size());
  for (flecs::entity w : worn) {
    const Equipped* equipped = w.get<Equipped>();
    wornSlots.push_back(equipped ? equipped->slots : std::vector<std::string>{});
  }

  const EquipPlan plan =
      PlanEquip(body->slots, wornSlots, def->slot, def->occupies);
  if (!plan.ok) {
    return {false, "System: You cannot equip " + name + ": " + plan.reason +
                       ".\n"};
  }

  std::string removed;
  for (int index : plan.unequip) {
    if (index < 0 || index >= static_cast<int>(worn.size())) continue;
    flecs::entity w = worn[static_cast<size_t>(index)];
    w.remove<Equipped>();
    if (!removed.empty()) {
      removed += ", ";
    }
    removed += ItemLabel(w);
  }

  Equipped equipped;
  equipped.slots = plan.claim;
  item.set<Equipped>(equipped);

  std::string msg =
      "System: You equipped " + name + " (" + JoinSlotIds(plan.claim) + ").\n";
  if (!removed.empty()) {
    msg += "System: You took off " + removed + " to make room.\n";
  }
  return {true, msg};
}

// Takes off a worn item. The item stays in inventory; only the marker goes.
inline std::string UnequipItem(flecs::entity, flecs::entity item) {
  const std::string name = ItemLabel(item);
  if (!item.has<Equipped>()) {
    return "System: You are not wearing " + name + ".\n";
  }
  item.remove<Equipped>();
  return "System: You took off " + name + ".\n";
}

// What the actor currently has on, one line per item.
inline std::string FormatEquipment(flecs::entity actor) {
  const std::vector<flecs::entity> worn = EquippedItems(actor);
  if (worn.empty()) {
    return "System: You are not wearing anything.\n";
  }

  std::string msg = "System: You are wearing:\n";
  for (flecs::entity item : worn) {
    msg += "- " + ItemLabel(item);
    const Equipped* equipped = item.get<Equipped>();
    if (equipped && !equipped->slots.empty()) {
      msg += " (" + JoinSlotIds(equipped->slots) + ")";
    }
    if (const Durability* durability = item.get<Durability>()) {
      msg += " " + std::to_string(durability->current) + "/" +
             std::to_string(durability->max);
    }
    msg += "\n";
  }
  return msg;
}

// The items that broke while being used, by display name.
struct WearResult {
  std::vector<std::string> broken;
};

// Charges `cost` durability to every equipped item whose definition declares it
// is consumed by `activity`, and destroys the ones that reach zero.
//
// Every used item pays the full cost: the object's cost is the price of the
// action, not a budget to divide between tools. `cost <= 0` and a missing
// registry are no-ops, so a world without items behaves exactly as before.
//
// Charged from the commit path, never from a resolver, so a cancelled action and
// a dry run cost nothing -- the same rule that keeps skill XP honest.
inline WearResult WearEquipment(flecs::entity actor, const std::string& activity,
                                int cost, const ItemRegistry* items) {
  WearResult result;
  if (!items || cost <= 0) {
    return result;
  }

  // Collect first: the walk below both mutates components and (later) destroys
  // entities, and [STORE] already documents why that must not happen mid-scan.
  std::vector<flecs::entity> used;
  actor.each<Holds>([&](flecs::entity child) {
    if (!child.is_alive() || !child.has<Equipped>()) return;
    const ItemType* type = child.get<ItemType>();
    const ItemDef* def = type ? items->Get(type->id) : nullptr;
    if (!def || !IsConsumedBy(def->consumedBy, activity)) return;
    used.push_back(child);
  });

  std::vector<flecs::entity> broken;
  for (flecs::entity item : used) {
    Durability* durability = item.get_mut<Durability>();
    if (!durability) {
      continue; // no durability field: this item never wears out
    }
    const WearOutcome outcome = WearDurability(durability->current, cost);
    durability->current = outcome.remaining;
    if (outcome.broke) {
      result.broken.push_back(ItemLabel(item));
      broken.push_back(item);
    }
  }

  if (!broken.empty()) {
    flecs::world world = actor.world();
    world.defer([broken]() {
      for (flecs::entity item : broken) {
        if (item.is_alive()) {
          item.destruct();
        }
      }
    });
  }
  return result;
}

// An item that leaves its holder stops being worn. Called by every transfer site
// -- store, take, trade -- so worn state can never follow an item into a chest
// or into another character's hands.
//
// Deleting an item needs no such call: Equipped lives on the item, so it cannot
// dangle.
inline void DropEquipped(flecs::entity item) {
  if (item.is_alive() && item.has<Equipped>()) {
    item.remove<Equipped>();
  }
}
