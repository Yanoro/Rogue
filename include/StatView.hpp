#pragma once

#include <string>
#include <utility>
#include <vector>

#include "SourceDefinitions.hpp"

// What kind of thing is granting a character an effect.
//
// Stats and skills are gathered today. The rest are reserved now so that adding
// races, classes, equipment and buffs later is a change in SourceGatherer alone
// and never a change in the resolvers that consume a StatView.
//
// `kind` exists for display and debugging. Resolution must never branch on it: a
// stat, a skill and a ring all reach the fold as "an id, optionally a magnitude,
// and whatever effects are registered against that id".
enum class SourceKind { Stat, Skill, Race, Class, Item, Effect };

// One source an entity actually carries.
struct ActiveSource {
  std::string id;
  SourceKind kind = SourceKind::Stat;
  float magnitude = 1.0f;
  // False for pure-effect sources (race traits, items, buffs), which contribute
  // registered effects without having a numeric value of their own.
  bool hasMagnitude = true;
};

// The sources an entity carries, plus the definitions needed to fill in
// defaults.
//
// Built fresh for each resolution rather than cached: resolution happens at
// action start and commit, a handful of times per action, so a cache would buy
// nothing measurable and would add a whole class of invalidation bugs.
// See docs/stat-effects-design.md section 6.1.
//
// Free of flecs on purpose -- gathering (which does need an entity) lives in
// SourceGatherer.h, so this class and its rules stay testable on their own.
class StatView {
public:
  StatView() = default;
  StatView(std::vector<ActiveSource> sources,
           const SourceDefinitions *definitions)
      : sources(std::move(sources)), definitions(definitions) {}

  // Effective value of a source: the entity's own magnitude when it carries one,
  // otherwise the definition's baseline.
  //
  // The fallback is what makes a missing source neutral rather than an error: the
  // baseline is the value that produces zero effect, so a character without this
  // stat -- or untrained in this skill -- is simply unaffected by it, with no
  // null checks at the call site.
  //
  // Returns 0 for an id nobody defines. That case is a content error in whoever
  // registered the effect, and the resolver skips sources it cannot find a
  // definition for anyway, so a silent neutral value is the right default.
  float Get(const std::string &id) const {
    for (const ActiveSource &source : sources) {
      if (source.hasMagnitude && source.id == id) {
        return source.magnitude;
      }
    }
    if (definitions) {
      return definitions->BaselineFor(id);
    }
    return 0.0f;
  }

  // True only when the entity itself carries this source. Get() may still return
  // a useful value when this is false, so prefer Get() for arithmetic and Has()
  // for "does this character actually have it?" questions.
  bool Has(const std::string &id) const {
    for (const ActiveSource &source : sources) {
      if (source.hasMagnitude && source.id == id) {
        return true;
      }
    }
    return false;
  }

  // Every source the entity carries, in the order the gatherer produced them
  // (which is stable: StatBlock then SkillBlock, each seeded from a sorted
  // registry). Effect dispatch walks this; Get() is for arithmetic.
  const std::vector<ActiveSource> &Sources() const { return sources; }

  bool Empty() const { return sources.empty(); }

private:
  std::vector<ActiveSource> sources;
  const SourceDefinitions *definitions = nullptr;
};
