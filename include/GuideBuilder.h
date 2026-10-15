#pragma once

#include <string>
#include <vector>

#include <flecs.h>

// Derives the [GUIDE] and [INFO] answers from live content data.
//
// Both are read-only queries: nothing here mutates the world, grants a skill or
// stores state, so an answer is recomputed on every call and can never drift out
// of sync with the content. Learning *how* is deliberately kept separate from
// getting *better* at it -- the latter is the progression skill system in
// SkillRegistry and is untouched by this file. See docs/guide-command-design.md.
//
// The two verbs differ only in recursion. [GUIDE] walks the whole crafting chain
// for an activity, down to raw sources. [INFO] describes a single item type and
// stops, so a model that needs one step does not pay for the whole tree. Both
// render a recipe through FormatRecipe, so the two can never disagree about how
// a recipe reads.
//
// Every function returns plain text with no "System: " prefix; the action that
// calls it owns that convention.
namespace GuideBuilder {

// Every activity a [GUIDE] can be asked about, sorted. Topics are the ids of the
// declared progression skills, which is the vocabulary content already uses in a
// recipe's "trains" block.
std::vector<std::string> Topics(flecs::world &world);

// Every item type an [INFO] can be asked about, sorted: the union of what
// recipes produce and what harvestable objects drop.
std::vector<std::string> ItemIds(flecs::world &world);

// Case-insensitive lookup of `query` among `candidates`, returning the canonical
// spelling. Empty when nothing matches, which is the caller's cue to list the
// valid set.
std::string Resolve(const std::vector<std::string> &candidates,
                    const std::string &query);

// The whole chain for `topic`, expanded from every recipe that trains it.
std::string BuildTopicGuide(flecs::world &world, const std::string &topic);

// One hop for `item`: the recipe and station that makes it, or, when no recipe
// produces it, the objects that yield it. Never recurses -- the caller walks
// further by asking about an input.
std::string BuildItemInfo(flecs::world &world, const std::string &item);

} // namespace GuideBuilder
