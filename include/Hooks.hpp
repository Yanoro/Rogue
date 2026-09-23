#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <flecs.h>

#include "LootTableBuilder.hpp"
#include "Modifiers.hpp"
#include "StatView.hpp"

// Context handed to a `duration` resolver hook.
//
// Hooks CONTRIBUTE; they never compute the final value. If two hooks each
// returned a duration there would be no way to combine them without making
// registration order meaningful, and the result could not be explained
// (section 2.3). Everything a hook wants to do goes into `out`.
//
// A hook must be side-effect free. Resolvers run on paths that may never commit
// -- [CRAFT] does a dry-run claim before the timed action even starts -- so
// awarding anything from here would fire for work that never happened
// (section 2.4). Awarding belongs to the notification points.
struct DurationContext {
  // Effective stats and skills, with the baseline fallback already applied, so a
  // hook never has to handle a missing stat. Null only if a caller builds a
  // context by hand.
  const StatView *stats = nullptr;

  flecs::entity actor;

  // The activity being resolved ("harvest", "craft", ...). A hook is registered
  // against a named id and runs for every activity that id is declared on, so a
  // hook that only cares about harvesting must check this.
  std::string activity;

  // Stable content id of what is being acted on: the object template id for a
  // harvest, the recipe id for a craft. Empty when there is nothing.
  std::string subjectId;
  flecs::entity subject;

  float baseSeconds = 0.0f;

  // Which source activated this hook, for labelling its contributions.
  std::string sourceId;

  // Where this hook's contributions go. By value rather than shared, so the
  // resolver can tell exactly what one hook added, and so one hook blocking does
  // not silently swallow another's work.
  ModifierList out;

  // Effective value of a stat: the actor's own, or its registry baseline when
  // the actor does not have it. This is the accessor hooks should use.
  float Stat(const std::string &id) const {
    return stats ? stats->Get(id) : 0.0f;
  }
};

using DurationHook = std::function<void(DurationContext &)>;

// Context handed to a `loot` resolver hook.
//
// Loot is structural rather than numeric, so a hook edits the table through
// `out` -- appending entries, adjusting chances, removing its own entries --
// instead of contributing a number. The builder is seeded from the authored
// table before any hook runs, and each hook sees the table as it currently
// stands.
//
// The ownership rule is the one thing to respect: a hook may only remove entries
// it appended itself. Adjusting chances is unrestricted, because a debuff that
// lowers every drop rate is a legitimate effect.
//
// Same side-effect-free requirement as DurationContext: this runs on paths that
// may never commit (section 2.4).
struct LootContext {
  const StatView *stats = nullptr;

  flecs::entity actor;
  std::string activity;

  // Stable content id of what is being harvested or produced, e.g.
  // "wheat_mature". The natural thing for a hook to branch on.
  std::string subjectId;
  flecs::entity subject;

  // Which source activated this hook, and the id to pass to `out` so its own
  // entries stay removable by it.
  std::string sourceId;

  LootTableBuilder out;

  // Effective value of a stat: the actor's own, or its registry baseline when
  // the actor does not have it.
  float Stat(const std::string &id) const {
    return stats ? stats->Get(id) : 0.0f;
  }
};

using LootHook = std::function<void(LootContext &)>;

// Named C++ effects, referenced by id from data (`"effects": ["dexterous_grip"]`).
//
// This is the escape hatch of section 4.4: declarative `StatLink`s cover the
// common "a stat scales an outcome" case, and this covers everything else --
// conditional bonuses, gates, effects that read the world, and (once the stream
// question in section 5.3 is settled) procs.
//
// Only hook IDS travel through data, and entities store nothing but stat values,
// so nothing unserializable ever reaches a save. Hooks are registered once at
// startup.
//
// Points are a closed, typed set: one method and one context struct per outcome.
// There is deliberately no universal `Hook(AnyContext&)` -- that would throw the
// context's type away and make every hook start by switching on the point.
class HookRegistry {
public:
  // Several hooks may share one id (a race and an item could both grant the same
  // named effect). They all run, in registration order.
  void OnDuration(std::string hookId, DurationHook hook) {
    durationHooks[std::move(hookId)].push_back(std::move(hook));
  }

  bool HasDurationHook(const std::string &hookId) const {
    auto it = durationHooks.find(hookId);
    return it != durationHooks.end() && !it->second.empty();
  }

  // Same contract as OnDuration: one named id, possibly several handlers.
  void OnLoot(std::string hookId, LootHook hook) {
    lootHooks[std::move(hookId)].push_back(std::move(hook));
  }

  bool HasLootHook(const std::string &hookId) const {
    auto it = lootHooks.find(hookId);
    return it != lootHooks.end() && !it->second.empty();
  }

  // True when ANY point has a handler for this id. The startup check uses this,
  // because an id may legitimately serve a different point than the one being
  // validated.
  bool HasAnyHook(const std::string &hookId) const {
    return HasDurationHook(hookId) || HasLootHook(hookId);
  }

  // Runs every hook registered under this id. An unknown id is a no-op here:
  // Game::ECSInit reports unregistered ids once at startup, rather than every
  // time a character carrying that stat acts.
  void ApplyDuration(const std::string &hookId, DurationContext &context) const {
    auto it = durationHooks.find(hookId);
    if (it == durationHooks.end()) {
      return;
    }
    for (const DurationHook &hook : it->second) {
      hook(context);
    }
  }

  void ApplyLoot(const std::string &hookId, LootContext &context) const {
    auto it = lootHooks.find(hookId);
    if (it == lootHooks.end()) {
      return;
    }
    for (const LootHook &hook : it->second) {
      hook(context);
    }
  }

private:
  std::unordered_map<std::string, std::vector<DurationHook>> durationHooks;
  std::unordered_map<std::string, std::vector<LootHook>> lootHooks;
};
