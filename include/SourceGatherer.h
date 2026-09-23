#pragma once

#include <utility>
#include <vector>

#include <flecs.h>

#include "SkillTypes.h"
#include "SourceDefinitions.hpp"
#include "StatTypes.h"
#include "StatView.hpp"

// Collects the effect-bearing sources an entity carries.
//
// Today that is its StatBlock and its SkillBlock. Races, classes, equipped items
// and timed effects will be added HERE and nowhere else: every resolver goes
// through this one door, so introducing a new source kind touches no call site.
// See docs/stat-effects-design.md section 6.
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

  return StatView(std::move(sources), &definitions);
}
