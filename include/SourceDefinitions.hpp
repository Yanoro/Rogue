#pragma once

#include <string>
#include <vector>

#include "SkillDef.hpp"
#include "SkillRegistry.h"
#include "SourceScaling.hpp"
#include "StatDef.hpp"
#include "StatLinks.hpp"
#include "StatRegistry.h"

// One door for looking up any source, whether it is a stat or a skill.
//
// The effect system does not care which it is: a source is an id with a neutral
// point, a scale, and some declared effects. This class is the ONLY place that
// knows two registries exist, which is what keeps the resolvers untouched when a
// third kind of source (a race, a class, an equipped item, a buff) arrives.
//
// Both pointers may be null, and a null registry simply has nothing to offer.
// That is deliberate: a half-initialised world then resolves with no effects
// rather than refusing to resolve or crashing, which keeps the failure mode
// "nothing changes" instead of "nothing works".
class SourceDefinitions {
public:
  SourceDefinitions() = default;
  SourceDefinitions(const StatRegistry *stats, const SkillRegistry *skills)
      : stats(stats), skills(skills) {}

  // True when the id names a known source, filling `out` with how it scales.
  // A skill and a stat cannot share an id, but if they ever did the stat would
  // win; ids are content and the two registries are separate files.
  bool ScalingFor(const std::string &id, SourceScaling &out) const {
    if (stats) {
      if (const StatDef *def = stats->Get(id)) {
        out = def->Scaling();
        return true;
      }
    }
    if (skills) {
      if (const SkillDef *def = skills->Get(id)) {
        out = def->Scaling();
        return true;
      }
    }
    return false;
  }

  // The neutral value of a source the entity does not carry. 0 for an id nobody
  // defines, which is the same neutral answer StatView gave before skills
  // existed.
  float BaselineFor(const std::string &id) const {
    SourceScaling scaling;
    return ScalingFor(id, scaling) ? scaling.baseline : 0.0f;
  }

  // Player/AI-facing name, or the id itself when nothing defines it. Used for
  // contribution breakdowns and, later, for the prompt's stat and skill block.
  std::string NameFor(const std::string &id) const {
    if (stats) {
      if (const StatDef *def = stats->Get(id)) {
        return def->name;
      }
    }
    if (skills) {
      if (const SkillDef *def = skills->Get(id)) {
        return def->name;
      }
    }
    return id;
  }

  const SkillDef *FindSkill(const std::string &id) const {
    return skills ? skills->Get(id) : nullptr;
  }

  // Declared effects for a source. Empty for an unknown id and for a source that
  // declares none.
  const std::vector<StatLink> &EffectsFor(const std::string &id) const {
    static const std::vector<StatLink> none;
    if (stats) {
      const std::vector<StatLink> &links = stats->GetEffects(id);
      if (!links.empty()) {
        return links;
      }
    }
    if (skills) {
      const std::vector<StatLink> &links = skills->GetEffects(id);
      if (!links.empty()) {
        return links;
      }
    }
    return none;
  }

  // Named C++ hooks a source activates.
  const std::vector<std::string> &HooksFor(const std::string &id) const {
    static const std::vector<std::string> none;
    if (stats) {
      const std::vector<std::string> &hooks = stats->GetHookIds(id);
      if (!hooks.empty()) {
        return hooks;
      }
    }
    if (skills) {
      const std::vector<std::string> &hooks = skills->GetHookIds(id);
      if (!hooks.empty()) {
        return hooks;
      }
    }
    return none;
  }

private:
  const StatRegistry *stats = nullptr;
  const SkillRegistry *skills = nullptr;
};
