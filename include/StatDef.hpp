#pragma once

#include <string>

#include "SourceScaling.hpp"

// Definition of one character statistic, loaded from data/stats/<id>.json.
//
// The file stem is the id, exactly like ObjectFactory templates and crafting
// recipes: "dexterity" comes from dexterity.json. Ids are content-facing -- they
// appear in NPC data, in effect links, and eventually in the agent's prompt --
// so they follow the same snake_case convention as item ids.
//
// `baseline` is the neutral point, and it belongs to the DEFINITION, never to an
// entity or a race. If baseline were per-entity, a racial "+2 Strength" would
// cancel against that race's own baseline and become a purely cosmetic
// character-sheet number. A global baseline is what makes a racial bonus
// mechanically real. See docs/stat-effects-design.md section 3.1.
//
// It is also what makes a MISSING stat neutral (section 3.4): baseline is by
// definition the value that produces zero effect, so a character that does not
// have a stat at all is simply unaffected by it. That is what will let a future
// race have an entirely different stat set with no special-casing anywhere.
//
// Deliberately dependency-free: this header is included by both StatRegistry.h
// and StatLinks.hpp, and putting it here is what keeps those two from including
// each other.
struct StatDef {
  std::string id;
  // Player/AI-facing label, e.g. "Dexterity".
  std::string name;
  // Flavour text for [STATS]. Deliberately NOT a mechanical claim: the "affects
  // harvesting and crafting" clause is generated from the registered links, so
  // it cannot drift from what the code does (section 9.1).
  std::string description;
  // The value that produces zero effect. Global, never per-entity.
  float baseline = 10.0f;
  // Effective units per point. Lets coefficients stay readable across stats with
  // very different ranges (section 3.2). Must be > 0.
  float scale = 1.0f;
  float min = 1.0f;
  float max = 20.0f;

  // How this definition turns a value into an effect. Skills provide the same
  // shape, which is what lets one contribution builder serve both.
  SourceScaling Scaling() const {
    SourceScaling scaling;
    scaling.baseline = baseline;
    scaling.scale = scale;
    scaling.min = min;
    scaling.max = max;
    return scaling;
  }
};
