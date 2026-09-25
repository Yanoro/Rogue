#pragma once

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <flecs.h>

#include "Hooks.hpp"
#include "Modifiers.hpp"
#include "SourceDefinitions.hpp"
#include "SourceGatherer.h"
#include "StatLinks.hpp"

// A request to resolve how long one activity takes.
struct DurationRequest {
  // "harvest", "craft", ... The activity is a filter on the point rather than
  // part of it, so adding a new activity costs no engine code (section 4.1).
  std::string activity;
  // Stable content id of the subject, for hooks and for debugging.
  std::string subjectId;
  flecs::entity subject;
  // The authored wait, before anything adjusts it.
  float baseSeconds = 0.0f;
};

struct DurationResolution {
  FoldResult fold;
};

// Durations share one range: never negative, unbounded above.
//
// The floor of 0 is deliberate rather than incidental. It lets a large enough
// modifier make an action instant, which is why the harvest handler branches on
// the *resolved* value instead of the authored one. An authored timer of 0 still
// resolves to 0, since a percentage of zero is zero.
inline FoldRange DurationRange() {
  return FoldRange{0.0f, std::numeric_limits<float>::infinity()};
}

// Whether one declared link applies to this point and activity. "*" is the
// deliberate global, never an omitted activity (section 4.1).
inline bool LinkMatches(const StatLink &link, const std::string &point,
                        const std::string &activity) {
  if (link.point != point) {
    return false;
  }
  return link.activity == activity || link.activity == "*";
}

// How a contribution names its source in a breakdown: "Dexterity 14", "Farming
// 12". The magnitude is rounded because a level is an integer and a stat is not
// worth a decimal in a prompt.
//
// A skill is named by its level rather than its stage on purpose: the number here
// explains an arithmetic result, and the level is what produced it. Stage-first
// naming belongs to the prompt and [SKILLS], where the point is capability.
inline std::string SourceLabel(const SourceDefinitions &definitions,
                               const ActiveSource &source) {
  const std::string name =
      source.label.empty() ? definitions.NameFor(source.id) : source.label;
  return name + " " + std::to_string(std::lround(source.magnitude));
}

// Resolves how long an activity takes for one actor.
//
// Two kinds of contributor feed the same accumulator, and neither the fold nor
// the caller needs to know which it came from:
//   * declarative links, taken from each source the actor carries;
//   * named C++ hooks that a source activates.
//
// Everything goes through GatherSources and the definitions facade, so a new
// source kind (race, class, item, buff) reaches this function with no change
// here at all (section 6).
//
// Pure with respect to the world: it reads the actor's components and returns a
// result. It mutates nothing, which is what lets it run on the craft dry-run
// path safely.
inline DurationResolution ResolveDuration(flecs::entity actor,
                                          const DurationRequest &request,
                                          const SourceDefinitions &definitions,
                                          const HookRegistry &hooks) {
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
      if (!LinkMatches(link, "duration", request.activity)) {
        continue;
      }
      modifiers.Add(MakeContribution(scaling, link, source.magnitude, source.id,
                                     SourceLabel(definitions, source)));
    }
  }

  for (const ActiveSource &source : view.Sources()) {
    for (const std::string &hookId : definitions.HooksFor(source.id)) {
      DurationContext context;
      context.stats = &view;
      context.actor = actor;
      context.activity = request.activity;
      context.subjectId = request.subjectId;
      context.subject = request.subject;
      context.baseSeconds = request.baseSeconds;
      context.sourceId = source.id;

      hooks.ApplyDuration(hookId, context);

      for (const Contributor &contribution : context.out.Entries()) {
        modifiers.Add(contribution);
      }
      // First block wins, matching ModifierList's own rule.
      if (context.out.Blocked() && !modifiers.Blocked()) {
        modifiers.Block(context.out.BlockReason(), source.id);
      }
    }
  }

  DurationResolution resolution;
  resolution.fold = Fold(request.baseSeconds, modifiers, DurationRange());
  return resolution;
}

// A request to resolve what one activity produces.
struct LootRequest {
  std::string activity;
  std::string subjectId;
  flecs::entity subject;
};

struct LootResolution {
  std::vector<LootEntry> entries;
};

// Resolves what a harvest (or any other loot-producing activity) drops.
//
// Declarative `loot` links are numeric, so they adjust every chance uniformly:
// that is the natural reading of "this source is lucky". Anything structural --
// adding an entry, targeting one item type, removing a drop -- is what the named
// hook escape hatch is for, because it is not expressible as a single number.
//
// Reads the world, mutates nothing, and returns the finished table. The authored
// drops are never touched; the builder works on its own copy.
inline LootResolution ResolveLoot(flecs::entity actor,
                                  const LootRequest &request,
                                  const std::vector<LootDrop> &base,
                                  const SourceDefinitions &definitions,
                                  const HookRegistry &hooks) {
  const StatView view = GatherSources(actor, definitions);
  LootTableBuilder builder(base);

  for (const ActiveSource &source : view.Sources()) {
    if (!source.hasMagnitude) {
      continue;
    }
    SourceScaling scaling;
    if (!definitions.ScalingFor(source.id, scaling)) {
      continue;
    }

    for (const StatLink &link : definitions.EffectsFor(source.id)) {
      if (!LinkMatches(link, "loot", request.activity)) {
        continue;
      }
      const Contributor contribution =
          MakeContribution(scaling, link, source.magnitude, source.id,
                           SourceLabel(definitions, source));
      // A percentage scales the chance; a flat amount adds to it. `link.item`
      // picks which entry, and an empty item means every entry -- which is the
      // "this source is simply lucky" reading. Both accumulate inside the builder
      // and are applied once, so the order sources are visited in cannot matter.
      if (contribution.op == Op::Pct) {
        builder.ScaleChance(link.item, 1.0f + contribution.requested, source.id);
      } else {
        builder.AddChance(link.item, contribution.requested, source.id);
      }
    }
  }

  for (const ActiveSource &source : view.Sources()) {
    for (const std::string &hookId : definitions.HooksFor(source.id)) {
      LootContext context;
      context.stats = &view;
      context.actor = actor;
      context.activity = request.activity;
      context.subjectId = request.subjectId;
      context.subject = request.subject;
      context.sourceId = source.id;
      // Seeded with the table as it stands, including everything earlier
      // contributors did.
      context.out = builder;

      hooks.ApplyLoot(hookId, context);

      builder = std::move(context.out);
    }
  }

  LootResolution resolution;
  resolution.entries = builder.Entries();
  return resolution;
}

// "5" for 5.0, "4.6" for 4.6: trailing zeros are noise in a prompt.
inline std::string FormatEffectNumber(float value, int decimals = 1) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals,
                static_cast<double>(value));
  std::string text(buffer);
  if (text.find('.') != std::string::npos) {
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  return text;
}

inline std::string FormatSignedEffect(float value) {
  return (value < 0.0f ? "-" : "+") + FormatEffectNumber(std::fabs(value));
}

// Compact breakdown of how a duration was reached:
//
//   " (base 5s; Dexterity 14 -0.4s)"
//
// Empty when nothing actually contributed, so a character whose sources all sit
// at their neutral value gets the plain message and the agent is not fed noise
// (section 9.3). Superseded overrides and zero-delta contributions are skipped
// for the same reason.
inline std::string FormatDurationBreakdown(const FoldResult &result,
                                           float baseSeconds,
                                           const std::string &unit = "s") {
  std::string detail;

  for (const Contributor &contribution : result.contributions) {
    if (contribution.superseded) {
      continue;
    }
    // A percentage's effect in the outcome's own units is its share of the base,
    // which is what makes "Dexterity 14 -0.4s" comparable to a flat bonus.
    const float delta = contribution.op == Op::Pct
                            ? baseSeconds * contribution.applied
                            : contribution.applied;
    if (delta == 0.0f) {
      continue;
    }

    if (detail.empty()) {
      detail = "base " + FormatEffectNumber(baseSeconds) + unit;
    }
    detail += "; ";
    detail += contribution.sourceLabel.empty() ? contribution.sourceId
                                               : contribution.sourceLabel;
    detail += " " + FormatSignedEffect(delta) + unit;
  }

  if (detail.empty()) {
    return "";
  }
  return " (" + detail + ")";
}
