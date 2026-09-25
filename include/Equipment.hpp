#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "EquipmentTypes.h"

// The pure half of equipment: given a body and what is already worn, decide
// which slot instances an item takes and which worn items must come off.
//
// Deliberately free of flecs, raylib and the filesystem, like Modifiers.hpp and
// ItemStack.hpp, so the decision rules are testable without a world. The
// flecs-side half (finding the entities, mutating their components) lives in
// EquipmentRuntime.hpp.

// Instance ids accepting `kind`, in the order the body declared them. Authored
// order is meaningful: the first free one is the one an item takes, so a body
// reads left to right.
inline std::vector<std::string> SlotsAccepting(const std::vector<SlotSpec>& slots,
                                               const std::string& kind) {
  std::vector<std::string> ids;
  for (const SlotSpec& slot : slots) {
    if (slot.accepts == kind) {
      ids.push_back(slot.id);
    }
  }
  return ids;
}

// The outcome of trying to equip one item.
struct EquipPlan {
  bool ok = false;
  // Why it failed, phrased for a player ("you have no free hand slot"). Empty
  // when ok.
  std::string reason;
  // Indices into the worn list that must be taken off entirely. A whole item is
  // always removed rather than one of its slots, because a two-handed weapon
  // half-equipping would be a state with no meaning.
  std::vector<int> unequip;
  // Instance ids the new item takes.
  std::vector<std::string> claim;
};

// Plans equipping an item that needs `occupies` instances of `requiredKind`.
//
// `wornSlotSets[i]` is the slot instances the i-th worn item currently occupies.
// The auto-swap rule: if there are not enough free slots, whole conflicting
// items are taken off, earliest-authored candidate first, until there are. With
// `occupies == 1` that is "wearing a new hat replaces the old one"; with
// `occupies == 2` it is "a greatsword clears both hands".
//
// Fails when the body simply has fewer than `occupies` instances of the kind,
// which is the honest answer for a one-handed item on a body with no hands.
inline EquipPlan PlanEquip(const std::vector<SlotSpec>& characterSlots,
                           const std::vector<std::vector<std::string>>& wornSlotSets,
                           const std::string& requiredKind, int occupies) {
  EquipPlan plan;
  if (occupies < 1) {
    occupies = 1;
  }
  if (requiredKind.empty()) {
    plan.reason = "this item does not name a slot";
    return plan;
  }

  const std::vector<std::string> candidates =
      SlotsAccepting(characterSlots, requiredKind);
  if (static_cast<int>(candidates.size()) < occupies) {
    plan.reason = "it needs " + std::to_string(occupies) + " " + requiredKind +
                  " slot" + (occupies == 1 ? "" : "s") + " and you have " +
                  std::to_string(candidates.size());
    return plan;
  }

  // Which worn item holds this instance, ignoring items already marked for
  // removal. -1 when it is free.
  auto holderOf = [&](const std::string& instanceId) -> int {
    for (size_t i = 0; i < wornSlotSets.size(); ++i) {
      if (std::find(plan.unequip.begin(), plan.unequip.end(),
                    static_cast<int>(i)) != plan.unequip.end()) {
        continue;
      }
      const std::vector<std::string>& worn = wornSlotSets[i];
      if (std::find(worn.begin(), worn.end(), instanceId) != worn.end()) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  auto freeCandidates = [&]() {
    std::vector<std::string> free;
    for (const std::string& id : candidates) {
      if (holderOf(id) < 0) {
        free.push_back(id);
      }
    }
    return free;
  };

  std::vector<std::string> free = freeCandidates();
  while (static_cast<int>(free.size()) < occupies) {
    // Take off the item holding the earliest candidate, so the choice is
    // deterministic and the resulting layout is the authored one.
    int chosen = -1;
    for (const std::string& id : candidates) {
      const int holder = holderOf(id);
      if (holder >= 0) {
        chosen = holder;
        break;
      }
    }
    if (chosen < 0) {
      break;
    }
    plan.unequip.push_back(chosen);
    free = freeCandidates();
  }

  if (static_cast<int>(free.size()) < occupies) {
    plan.reason = "you have no free " + requiredKind + " slot";
    return plan;
  }

  plan.claim.assign(free.begin(), free.begin() + occupies);
  plan.ok = true;
  return plan;
}

// "main_hand, off_hand" for a listing. Empty input yields an empty string.
inline std::string JoinSlotIds(const std::vector<std::string>& slotIds) {
  std::string joined;
  for (const std::string& id : slotIds) {
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += id;
  }
  return joined;
}

// The first required capability tag that no provided tag list carries, or an
// empty string when every requirement is met.
//
// Pure, and the whole of the "you need a scythe" rule: a requirement names tags
// and each worn item offers a tag list, so the gate is a set-membership question
// with no knowledge of what a scythe is.
inline std::string FirstMissingTag(
    const std::vector<std::string>& required,
    const std::vector<std::vector<std::string>>& provided) {
  for (const std::string& tag : required) {
    bool found = false;
    for (const std::vector<std::string>& tags : provided) {
      if (std::find(tags.begin(), tags.end(), tag) != tags.end()) {
        found = true;
        break;
      }
    }
    if (!found) {
      return tag;
    }
  }
  return "";
}

// Whether a definition that declares `consumedBy` is a tool for this activity.
//
// "The equipment you used" is answered by content, not by slots or by which
// effects happened to fire: an item is a harvesting tool because its data says
// so, which keeps a lucky ring from wearing out every time you cut wheat.
inline bool IsConsumedBy(const std::vector<std::string>& consumedBy,
                         const std::string& activity) {
  return std::find(consumedBy.begin(), consumedBy.end(), activity) !=
         consumedBy.end();
}

// One item's durability after a single use.
struct WearOutcome {
  int remaining = 0;
  bool broke = false;
};

// Applies one use's cost. PURE: the caller persists `remaining` and destroys the
// item when `broke`. Clamping to 0 is what lets a message read "broke" instead
// of printing a negative number, and is why 0 durability means broken rather
// than merely low.
inline WearOutcome WearDurability(int current, int cost) {
  WearOutcome outcome;
  if (cost < 0) {
    cost = 0;
  }
  outcome.remaining = current - cost;
  if (outcome.remaining <= 0) {
    outcome.remaining = 0;
    outcome.broke = true;
  }
  return outcome;
}
