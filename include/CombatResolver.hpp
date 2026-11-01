#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include <flecs.h>

#include "Components.h"
#include "EffectResolver.hpp"
#include "ItemTypes.h"
#include "Modifiers.hpp"
#include "SourceDefinitions.hpp"
#include "SourceGatherer.h"
#include "StatLinks.hpp"

// Combat arithmetic, resolved through the same fold everything else uses.
//
// The point of this header is that combat invents no resolution model of its
// own: a weapon's damage is a base value, every stat, skill and item that wants
// to change it contributes through a `damage` StatLink, and Fold combines them.
// The result carries its contributions, so the log can say why a hit was 9
// rather than 12.
//
// Randomness, when it arrives, belongs on the gate side: a proc decides whether
// a contribution is in the list, never what Fold does with it. That keeps an
// exchange replayable and explainable.

// A weapon a character is actually wearing.
//
// Two-handed items occupy both hand slots but are still one item, so the first
// equipped weapon is the one that swings. Empty when the character is unarmed.
struct EquippedWeapon {
  bool found = false;
  std::string id;
  const ItemDef *definition = nullptr;
};

inline EquippedWeapon FindEquippedWeapon(flecs::entity actor,
                                         const SourceDefinitions &definitions) {
  EquippedWeapon weapon;
  actor.each<Holds>([&](flecs::entity item) {
    if (weapon.definition != nullptr || !item.is_alive() ||
        !item.has<Equipped>()) {
      return;
    }
    const ItemType *type = item.get<ItemType>();
    if (type == nullptr) {
      return;
    }
    const ItemDef *def = definitions.FindItem(type->id);
    if (def == nullptr || def->damage <= 0.0f) {
      return;
    }
    weapon.found = true;
    weapon.id = type->id;
    weapon.definition = def;
  });
  return weapon;
}

// Flat protection against one hit: innate Armor plus every worn item's armor.
//
// Flat rather than a percentage because it gives the player a number they can
// check: "leather vest, 3" is visible in the target panel, and a hit for 6 landing
// for 3 is legible. The floor of 1 is applied by the caller, not here.
inline float ArmorOf(flecs::entity target, const ItemRegistry *items) {
  float total = 0.0f;
  if (const Armor *armor = target.get<Armor>()) {
    total += armor->value;
  }
  if (items != nullptr) {
    target.each<Holds>([&](flecs::entity item) {
      if (!item.is_alive() || !item.has<Equipped>()) {
        return;
      }
      const ItemType *type = item.get<ItemType>();
      if (type == nullptr) {
        return;
      }
      if (const ItemDef *def = items->Get(type->id)) {
        total += def->armor;
      }
    });
  }
  return std::max(0.0f, total);
}

// The hit points a character has before anything has hurt them.
//
// A constant base plus the `health` links a source declares, so Constitution (or
// a race, or a future class) raises it through data rather than a formula here.
// Resolved on demand: a maximum cached at spawn would disagree with the
// components the moment a stat or an item changed.
inline constexpr float kBaseMaxHealth = 60.0f;

inline FoldResult ResolveMaxHealth(flecs::entity actor,
                                   const SourceDefinitions &definitions) {
  const StatView view = GatherSources(actor, definitions);
  ModifierList modifiers;

  for (const ActiveSource &source : view.Sources()) {
    if (!source.hasMagnitude) {
      continue;
    }
    SourceScaling scaling;
    if (!definitions.ScalingFor(source.id, scaling)) {
      continue;
    }
    for (const StatLink &link : definitions.EffectsFor(source.id)) {
      if (!LinkMatches(link, "health", "*")) {
        continue;
      }
      modifiers.Add(MakeContribution(scaling, link, source.magnitude, source.id,
                                     SourceLabel(definitions, source)));
    }
  }

  // A character always has at least one point of health, so a sufficiently
  // punitive Constitution can make someone frail but never dead-on-arrival.
  const FoldRange range{1.0f, std::numeric_limits<float>::infinity()};
  return Fold(kBaseMaxHealth, modifiers, range);
}

// One attack's worth of damage before it reaches a target.
struct DamageRequest {
  // "attack" today. Kept explicit because the link vocabulary filters on it, so
  // a future "spell" or "backstab" activity costs no new resolver.
  std::string activity = "attack";
  // Stable content id of what is hitting, for the breakdown and for debugging.
  std::string subjectId;
  // Weapon damage, or the skill's own baseDamage when unarmed.
  float baseDamage = 0.0f;
};

// Resolves the damage of one hit.
//
// Pure with respect to the world: it reads the attacker's sources and returns a
// fold. The target's mitigation would be a second fold on the same number, which
// is why this never takes a defender yet -- see the design doc's "armour" phase.
inline FoldResult ResolveDamage(flecs::entity attacker,
                                const DamageRequest &request,
                                const SourceDefinitions &definitions) {
  const StatView view = GatherSources(attacker, definitions);
  ModifierList modifiers;

  for (const ActiveSource &source : view.Sources()) {
    if (!source.hasMagnitude) {
      continue;
    }
    SourceScaling scaling;
    if (!definitions.ScalingFor(source.id, scaling)) {
      continue;
    }
    for (const StatLink &link : definitions.EffectsFor(source.id)) {
      if (!LinkMatches(link, "damage", request.activity)) {
        continue;
      }
      modifiers.Add(MakeContribution(scaling, link, source.magnitude, source.id,
                                     SourceLabel(definitions, source)));
    }
  }

  // A hit always does at least one point: a zero-damage attack that still spends
  // a cooldown is worse than a miss, and the range makes "armour flattened it"
  // something the log can report rather than a silent no-op.
  const FoldRange range{1.0f, std::numeric_limits<float>::infinity()};
  return Fold(request.baseDamage, modifiers, range);
}
