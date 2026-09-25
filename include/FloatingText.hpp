#pragma once

// Floating change numbers: a small value that pops over an entity, drifts up and
// fades out. XP gains use it today; damage (and anything else that changes a
// character) is meant to be a one-line call away.
//
// The component and its systems are deliberately event-agnostic: nothing here
// knows what the number means. A caller hands over a target entity and some
// text plus a colour, and this module owns the motion, the lifetime and the
// stacking. The semantic wrappers at the bottom (SpawnFloatingXp,
// SpawnFloatingDamage) exist only to keep the sign and the palette in one place,
// so adding a new event never means reimplementing the animation.
//
// Each number lives on its own throwaway entity that points at the character it
// describes, rather than on the character itself. An entity can therefore carry
// several at once (a harvest that trains two skills, a future multi-hit attack)
// and the visual never outlives or mutates the thing it is about.

#include <string>
#include <utility>

#include <flecs.h>

#include "Components.h"
#include "Defaults.h"
#include "FloatingTextMotion.hpp"

struct FloatingText {
  // The entity the number hangs above. Its ScreenPosition is read every frame so
  // the number follows a walking character.
  flecs::entity target;

  // Already-formatted, drawn verbatim: "+3", "-7". Formatting stays with the
  // caller because only the caller knows what the number means.
  std::string text;

  Color color = WHITE;

  // Seconds since spawn; advanced by FloatingTextLifetimeSystem.
  float elapsed = 0.0f;

  float lifetime = DEFAULT_FLOATING_TEXT_LIFETIME;
  float rise = DEFAULT_FLOATING_TEXT_RISE;

  // How many numbers already hung over this target when this one spawned. The
  // renderer fans them out sideways with it, so simultaneous numbers stay
  // readable instead of piling onto one pixel.
  int stackIndex = 0;

  // Last known position of the target, in world pixels. Updated while the target
  // is alive and positioned, and kept afterwards so a killing blow still shows
  // its number where the victim fell.
  Vector2 anchor = {0.0f, 0.0f};
  bool anchored = false;
};

// Spawns one floating number over `target` and returns its carrier entity, or
// flecs::entity::null() when there is nothing to show. The world is taken from
// the target, so the two can never disagree about where the number lives.
//
// `extraStack` is the caller's slot within a batch of numbers spawned in the
// same deferred frame: entities created earlier in that frame are not visible to
// the count query below, so a caller that spawns several at once (one per trained
// skill) should pass 0, 1, 2 ... to keep them apart.
inline flecs::entity SpawnFloatingText(flecs::entity target, std::string text,
                                       Color color, int extraStack = 0) {
  if (!target.is_alive() || text.empty()) {
    return flecs::entity::null();
  }

  flecs::world world = target.world();

  int aliveOnTarget = 0;
  world.filter<FloatingText>().each(
      [&](flecs::entity, const FloatingText &other) {
        if (other.target == target) {
          ++aliveOnTarget;
        }
      });

  FloatingText floating;
  floating.target = target;
  floating.text = std::move(text);
  floating.color = color;
  floating.stackIndex = aliveOnTarget + extraStack;

  // Anchor immediately rather than waiting for the first update tick, so the
  // number is drawable on its first frame even if the target dies before then.
  if (const ScreenPosition *screenPos = target.get<ScreenPosition>()) {
    floating.anchor = {screenPos->x, screenPos->y};
    floating.anchored = true;
  }

  flecs::entity carrier = world.entity();
  carrier.set<FloatingText>(std::move(floating));
  return carrier;
}

// "+N" over an entity that earned `amount` XP. A grant that only banks progress
// toward the next level is still worth showing, which is why the caller reports
// the raw amount rather than only level-ups.
inline flecs::entity SpawnFloatingXp(flecs::entity target, int amount,
                                     int extraStack = 0) {
  if (amount <= 0) {
    return flecs::entity::null();
  }
  return SpawnFloatingText(target, "+" + std::to_string(amount),
                           DEFAULT_FLOATING_TEXT_XP_COLOR, extraStack);
}

// "-N" over an entity that took `amount` damage. Combat does not exist yet; this
// is the call it will make, so no combat-specific code has to be added to the
// renderer later. Pair it with the same-target `extraStack` rule when one attack
// lands several hits in a single frame.
inline flecs::entity SpawnFloatingDamage(flecs::entity target, int amount,
                                         int extraStack = 0) {
  if (amount <= 0) {
    return flecs::entity::null();
  }
  return SpawnFloatingText(target, "-" + std::to_string(amount),
                           DEFAULT_FLOATING_TEXT_DAMAGE_COLOR, extraStack);
}
