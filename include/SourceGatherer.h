#pragma once

#include <utility>
#include <vector>

#include <flecs.h>

#include "ItemTypes.h"
#include "SkillTypes.h"
#include "SourceDefinitions.hpp"
#include "StatTypes.h"
#include "StatView.hpp"

// Collects the effect-bearing sources an entity carries.
//
// Today that is its StatBlock, its SkillBlock, and the items it has equipped.
// Races, classes and timed effects will be added HERE and nowhere else: every
// resolver goes through this one door, so introducing a new source kind touches
// no call site. See docs/stat-effects-design.md section 6.
//
// A source the entity lacks is deliberately NOT filled in from the definitions
// here. Get() falls back to the baseline on demand, so omission is safe by
// construction and there is no "default block" to keep in sync.
//
// Note this reads the actor's own long-lived components, not anything created
// during the current system iteration: a just-spawned entity's components are
// not readable until the sync point (docs section 8.1).
inline StatView GatherSources(flecs::entity entity,
                              const SourceDefinitions &definitions) {
  std::vector<ActiveSource> sources;

  if (const StatBlock *block = entity.get<StatBlock>()) {
    sources.reserve(block->values.size());
    for (const StatValue &value : block->values) {
      ActiveSource source;
      source.id = value.id;
      source.kind = SourceKind::Stat;
      source.magnitude = value.value;
      source.hasMagnitude = true;
      sources.push_back(std::move(source));
    }
  }

  // A skill's magnitude is its level, exactly as a stat's is its value. Level 0
  // is a real source with a magnitude of 0, which contributes nothing -- the same
  // neutrality as an absent stat, arrived at from the other direction.
  if (const SkillBlock *block = entity.get<SkillBlock>()) {
    sources.reserve(sources.size() + block->values.size());
    for (const SkillValue &value : block->values) {
      ActiveSource source;
      source.id = value.id;
      source.kind = SourceKind::Skill;
      source.magnitude = static_cast<float>(value.level);
      source.hasMagnitude = true;
      sources.push_back(std::move(source));
    }
  }

  // Equipped items. An item is a magnitude-bearing source whose magnitude is its
  // tier, so its declarative links flow through the ordinary fold with no
  // resolver change; the explicit Equipped marker is what stops every carried
  // item from stacking (section 6.3).
  //
  // The label is resolved HERE as well as the magnitude because this is the one
  // place that has both the item's definition (through the facade) and the
  // entity's own display name, which is the fallback the definition defers to.
  entity.each<Holds>([&](flecs::entity item) {
    if (!item.is_alive() || !item.has<Equipped>()) {
      return;
    }
    const ItemType *type = item.get<ItemType>();
    if (!type) {
      return;
    }

    ActiveSource source;
    source.id = type->id;
    source.kind = SourceKind::Item;
    source.magnitude = definitions.ItemMagnitudeFor(type->id);
    source.hasMagnitude = true;
    source.label = definitions.ItemNameFor(type->id);
    if (source.label.empty()) {
      if (const DisplayName *display = item.get<DisplayName>()) {
        source.label = display->name;
      }
    }
    sources.push_back(std::move(source));
  });

  return StatView(std::move(sources), &definitions);
}
