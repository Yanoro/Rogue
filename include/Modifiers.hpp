#pragma once

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// How a contribution combines with the value being resolved.
enum class Op {
  // Added after percentages, in absolute units of the outcome (seconds, items).
  Flat,
  // A fraction of the base: 0.1 is "+10%", -0.2 is "-20%".
  Pct,
  // Sets the value outright. The escape hatch for "this finishes instantly".
  // Needs a priority; highest wins. See docs/stat-effects-design.md section 3.3.
  Override,
};

// One thing trying to change an outcome.
//
// `requested` is what the contributor asked for, `applied` is what the fold
// actually used. They differ only for a superseded Op::Override, so the debug UI
// and the agent can be told something was overruled instead of it being silently
// dropped.
struct Contributor {
  std::string sourceId;
  std::string sourceLabel;
  Op op = Op::Pct;
  float requested = 0.0f;
  int priority = 0; // only meaningful for Op::Override
  std::string flavor;

  // Filled in by Fold. Never set these by hand.
  float applied = 0.0f;
  bool superseded = false;
};

// Valid range for the resolved value. Each point declares its own: a duration is
// never negative, a chance is never above 1.
struct FoldRange {
  float min = -std::numeric_limits<float>::infinity();
  float max = std::numeric_limits<float>::infinity();
};

struct FoldResult {
  float value = 0.0f;
  // True when the range, rather than the contributions, decided the value.
  bool clamped = false;
  // Gates (section 3.7): the outcome is refused outright rather than adjusted.
  // Callers must check this before using `value`.
  bool blocked = false;
  std::string blockReason;
  // Sorted by sourceId, so two identical actions print identically.
  std::vector<Contributor> contributions;
};

// The accumulator that contributors write into.
//
// Contributing is deliberately not the same as deciding: nothing here touches a
// base value. The fold is the only thing that combines contributions, which is
// what makes the result independent of the order they arrived in.
class ModifierList {
public:
  void Add(Contributor contributor) {
    entries.push_back(std::move(contributor));
  }

  void Flat(float amount, std::string sourceId, std::string sourceLabel = {},
            std::string flavor = {}) {
    Contributor c;
    c.sourceId = std::move(sourceId);
    c.sourceLabel = std::move(sourceLabel);
    c.op = Op::Flat;
    c.requested = amount;
    c.flavor = std::move(flavor);
    entries.push_back(std::move(c));
  }

  void Pct(float fraction, std::string sourceId, std::string sourceLabel = {},
           std::string flavor = {}) {
    Contributor c;
    c.sourceId = std::move(sourceId);
    c.sourceLabel = std::move(sourceLabel);
    c.op = Op::Pct;
    c.requested = fraction;
    c.flavor = std::move(flavor);
    entries.push_back(std::move(c));
  }

  void Override(float value, int priority, std::string sourceId,
                std::string sourceLabel = {}, std::string flavor = {}) {
    Contributor c;
    c.sourceId = std::move(sourceId);
    c.sourceLabel = std::move(sourceLabel);
    c.op = Op::Override;
    c.requested = value;
    c.priority = priority;
    c.flavor = std::move(flavor);
    entries.push_back(std::move(c));
  }

  // Refuses the outcome. The first block wins: being blocked is not something a
  // later, larger contribution can out-vote.
  void Block(std::string reason, std::string sourceId) {
    if (blocked) {
      return;
    }
    blocked = true;
    blockReason = std::move(reason);
    blockSourceId = std::move(sourceId);
  }

  const std::vector<Contributor> &Entries() const { return entries; }
  bool Empty() const { return entries.empty(); }
  bool Blocked() const { return blocked; }
  const std::string &BlockReason() const { return blockReason; }
  const std::string &BlockSourceId() const { return blockSourceId; }

private:
  std::vector<Contributor> entries;
  bool blocked = false;
  std::string blockReason;
  std::string blockSourceId;
};

// Applies contributions to a base value:
//
//   v = base * (1 + sum of pct) + sum of flat
//   v = clamp(v, range)
//   v = winning override, if any
//
// PURE: no randomness, no world, no time. Randomness decides WHETHER a
// contribution is in the list -- a proc either fires or it does not (section
// 5.2) -- and never what the fold does with it. That is what keeps this
// testable, replayable and order-independent.
//
// Percentages are ADDITIVE, so opposing effects cancel exactly and registration
// order cannot matter. They scale the base value; flats are absolute and land
// afterwards, so a percentage can never amplify a flat bonus (section 3.3).
//
// The caller must check `blocked` before using `value`; when blocked, `value` is
// whatever the arithmetic produced and carries no meaning.
inline FoldResult Fold(float base, std::vector<Contributor> contributors,
                       const FoldRange &range = {}) {
  FoldResult result;

  float pct = 0.0f;
  float flat = 0.0f;
  int winner = -1;

  for (size_t i = 0; i < contributors.size(); ++i) {
    Contributor &c = contributors[i];
    switch (c.op) {
    case Op::Pct:
      c.applied = c.requested;
      pct += c.applied;
      break;
    case Op::Flat:
      c.applied = c.requested;
      flat += c.applied;
      break;
    case Op::Override:
      // Highest priority wins. Ties go to the earlier contributor, so anything
      // relying on a tie is relying on insertion order: give overrides distinct
      // priorities.
      if (winner < 0 || c.priority > contributors[winner].priority) {
        if (winner >= 0) {
          contributors[winner].superseded = true;
        }
        winner = static_cast<int>(i);
      } else {
        c.superseded = true;
      }
      break;
    }
  }

  float value = base * (1.0f + pct) + flat;

  if (value < range.min) {
    value = range.min;
    result.clamped = true;
  } else if (value > range.max) {
    value = range.max;
    result.clamped = true;
  }

  if (winner >= 0) {
    Contributor &w = contributors[winner];
    value = w.requested;
    // Clamped like everything else. The range is the outcome's contract -- a
    // duration is never negative -- and an override that could break it would
    // make the contract worthless.
    if (value < range.min) {
      value = range.min;
      result.clamped = true;
    } else if (value > range.max) {
      value = range.max;
      result.clamped = true;
    }
    w.applied = value;
  }

  // The fold is order-independent by construction, but the *printed* list should
  // not shuffle between two otherwise identical actions. stable_sort keeps the
  // arrival order within one source.
  std::stable_sort(contributors.begin(), contributors.end(),
                   [](const Contributor &a, const Contributor &b) {
                     return a.sourceId < b.sourceId;
                   });

  result.value = value;
  result.contributions = std::move(contributors);
  return result;
}

// Convenience overload that carries the block state along with the entries.
inline FoldResult Fold(float base, const ModifierList &modifiers,
                       const FoldRange &range = {}) {
  FoldResult result = Fold(base, modifiers.Entries(), range);
  result.blocked = modifiers.Blocked();
  result.blockReason = modifiers.BlockReason();
  return result;
}
