#pragma once

#include <cmath>
#include <string>
#include <utility>

#include "Modifiers.hpp"
#include "StatDef.hpp"

// Where a stat measures its deviation from. RegistryDefault is the normal case;
// the others are the escape hatch for a stat whose neutral point genuinely is
// not the registry baseline. See docs/stat-effects-design.md sections 3.1, 3.2.
enum class BaselineMode { RegistryDefault, Zero, Min };

// The shape of a stat's influence, selected per link.
enum class Curve {
  // coef * d
  Linear,
  // coef * sign(d) * |d|^exponent: diminishing returns in both directions, so a
  // very high stat cannot run away with the outcome. sign-preserving so a
  // penalty stays a penalty.
  Diminishing,
};

// One declared effect: "this stat adjusts this outcome for this activity".
//
// The point is an outcome noun ("duration", "loot") and the activity is a
// declared filter ("harvest", "craft", or "*" for a deliberate global). The two
// are orthogonal on purpose: adding a new activity costs no engine code, and a
// generic effect is one declaration rather than one per activity. See section 4.
struct StatLink {
  std::string stat;
  std::string point;
  std::string activity;
  Op op = Op::Pct;
  // Effect per effective unit of deviation, so the same coefficient reads
  // sensibly across stats with very different ranges (section 3.2).
  float coef = 0.0f;
  // Curve::Diminishing only. 2.0 gives a square-root-shaped response.
  float exponent = 2.0f;
  Curve curve = Curve::Linear;
  BaselineMode baseline = BaselineMode::RegistryDefault;
  // Op::Override only, same meaning as Contributor::priority.
  int priority = 0;
  // Optional line shown in the contribution breakdown.
  std::string flavor;
};

// Normalised distance from the neutral point: (value - baseline) / scale.
//
// Dividing by `scale` is what lets one coefficient stay readable across sources
// with very different ranges: a stat that runs 1-20 and a skill that runs 0-45
// both express coefficients as "effect per effective unit". A zero or negative
// scale would be a division by zero, so it is treated as 1 here as well as being
// rejected at load.
inline float Deviation(float value, const SourceScaling &scaling,
                       BaselineMode mode) {
  float baseline = scaling.baseline;
  switch (mode) {
  case BaselineMode::Zero:
    baseline = 0.0f;
    break;
  case BaselineMode::Min:
    baseline = scaling.min;
    break;
  case BaselineMode::RegistryDefault:
    break;
  }
  const float scale = scaling.scale > 0.0f ? scaling.scale : 1.0f;
  return (value - baseline) / scale;
}

// Convenience for the common case: a stat definition already describes itself.
inline float Deviation(float value, const StatDef &def, BaselineMode mode) {
  return Deviation(value, def.Scaling(), mode);
}

// Applies the link's curve to a deviation. Sign-preserving, so a negative
// deviation is never flipped positive by an even exponent.
inline float ApplyCurve(float deviation, const StatLink &link) {
  if (link.curve != Curve::Diminishing) {
    return deviation;
  }
  const float magnitude = std::pow(std::fabs(deviation), link.exponent);
  return deviation < 0.0f ? -magnitude : magnitude;
}

// "Dexterity 14" -- the label the debug UI and the agent see next to a
// contribution, so a breakdown says who did what rather than just a number.
inline std::string StatSourceLabel(const StatDef &def, float value) {
  return def.name + " " + std::to_string(static_cast<long>(std::lround(value)));
}

// Same, for a skill, where the value that matters is the level. The stage is the
// thing worth naming, but the stage is a progression concern and this header has
// no business knowing about it: callers that want "Farming (Journeyman)" build
// that label themselves from the SkillDef.

// The contribution one link makes at a given magnitude.
//
// `sourceId`/`sourceLabel` identify WHO is contributing. For a registry-side
// link that is the source itself, but the same source can also be boosted by an
// item, a race or a buff, and the breakdown has to stay readable when it is.
inline Contributor MakeContribution(const SourceScaling &scaling,
                                    const StatLink &link, float value,
                                    std::string sourceId,
                                    std::string sourceLabel) {
  Contributor contribution;
  contribution.sourceId = std::move(sourceId);
  contribution.sourceLabel = std::move(sourceLabel);
  contribution.op = link.op;
  contribution.priority = link.priority;
  contribution.flavor = link.flavor;
  contribution.requested =
      link.coef * ApplyCurve(Deviation(value, scaling, link.baseline), link);
  return contribution;
}

// Convenience for the common case: a stat definition already describes itself.
inline Contributor MakeContribution(const StatDef &def, const StatLink &link,
                                    float statValue, std::string sourceId,
                                    std::string sourceLabel) {
  return MakeContribution(def.Scaling(), link, statValue, std::move(sourceId),
                          std::move(sourceLabel));
}
