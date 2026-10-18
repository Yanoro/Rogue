#pragma once

// Turning a character's own place memory into the one `choice` question System 1
// answers, and turning the answer back into a walk.
//
// Everything here is pure: memory, position and goal in, strings or a choice out.
// No ECS query, no map, no network. That is deliberate --
//
//   * the candidate set can only be what the character remembers, because the
//     only input is `SpatialMemory`;
//   * the state, the question and the summary can be unit-tested without a world;
//   * "the engine supplies facts, System 1 supplies the choice" is enforceable by
//     inspection: nothing in this file knows what a village is, or prefers roads.
//
// Design: docs/navigation-subagent-design.md. Stage 1 is one decision and one leg;
// the loop, survey recovery and interruption state machine live in
// `NavigateAction` (AgentActions.h), not here.

#include "Components.h"
#include "Defaults.h"
#include "PlaceQuery.hpp"
#include "SightReport.hpp"
#include "SystemOneRecorder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

namespace NavigationPlan {

// One remembered place the model may choose. `key` is the string that comes back
// from the decision model; the engine resolves it to `placeId`, never to a name,
// so a hallucinated label cannot move the character.
struct Candidate {
  int placeId = 0;
  std::string key;          // "p12"
  std::string description;  // "an unnamed road, 9 tiles north-east, road 100%"
  int distance = 0;
};

namespace detail {

// Aspect scores by mean, not peak: peak is one seed, and one apple tree would make
// an orchard read as strongly as the whole area. Docs/place-derivation §6.2.
inline std::string AspectSummary(const KnownPlace &place, std::size_t count) {
  std::vector<const PlaceAspect *> aspects;
  aspects.reserve(place.aspects.size());
  for (const PlaceAspect &aspect : place.aspects) {
    if (aspect.mean > 0.0f) aspects.push_back(&aspect);
  }
  std::stable_sort(aspects.begin(), aspects.end(),
                   [](const PlaceAspect *a, const PlaceAspect *b) {
                     return a->mean > b->mean;
                   });

  std::string out;
  for (std::size_t i = 0; i < aspects.size() && i < count; ++i) {
    if (!out.empty()) out += ", ";
    out += aspects[i]->category + " " +
           std::to_string(static_cast<int>(aspects[i]->mean * 100.0f)) + "%";
  }
  return out;
}

inline int Chebyshev(const GamePosition &a, const GamePosition &b) {
  return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

}  // namespace detail

// The places the model is shown, nearest first, capped. A place whose approach
// cell is the cell underfoot is not a move and is skipped; a linear place the
// character is standing on is *not* skipped, because its far end is the useful
// target (PlaceQuery::ApproachCell already returns that far end).
inline std::vector<Candidate> BuildCandidates(
    const SpatialMemory &memory, GamePosition here, int maxCandidates,
    const std::unordered_set<int> &visitedPlaceIds = {}) {
  std::vector<Candidate> candidates;
  candidates.reserve(memory.places.size());

  for (const KnownPlace &place : memory.places) {
    if (place.memberCells.empty()) continue;

    const GamePosition approach = PlaceQuery::ApproachCell(memory, place, here);
    const int distance = detail::Chebyshev(approach, here);
    if (distance <= 0) continue;  // no move: the approach cell is where we stand

    Candidate candidate;
    candidate.placeId = place.id;
    candidate.key = "p" + std::to_string(place.id);
    candidate.distance = distance;

    candidate.description = PlaceQuery::DisplayName(place) + ", " +
                            PlaceQuery::DistancePhrase(here, approach);
    const std::string aspects = detail::AspectSummary(place, 2);
    if (!aspects.empty()) candidate.description += ", " + aspects;
    if (visitedPlaceIds.count(place.id) != 0) candidate.description += ", visited";

    candidates.push_back(std::move(candidate));
  }

  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate &a, const Candidate &b) {
                     return a.distance < b.distance;
                   });
  if (maxCandidates > 0 &&
      candidates.size() > static_cast<std::size_t>(maxCandidates)) {
    candidates.resize(static_cast<std::size_t>(maxCandidates));
  }
  return candidates;
}

// The state handed to System 1. It must be readable on its own: System 1 has no
// world model and no memory of the previous step, so the goal, where the
// character is standing, what it has already visited, and the candidates are all
// explicit.
//
// The two sentinels are not repeated here; they are options, and the option list
// is the definitive set of things the model may answer with.
inline std::string BuildState(const SpatialMemory &memory, GamePosition here,
                              const Facing &facing, const std::string &agentName,
                              const std::string &goal, int step,
                              const std::vector<std::string> &visited,
                              const std::vector<Candidate> &candidates) {
  std::string out;
  out += "AGENT: " + agentName + "\n";

  const KnownPlace *current = PlaceQuery::At(memory, here);
  out += "YOU ARE: ";
  if (current != nullptr) {
    out += "in " + PlaceQuery::DisplayName(*current);
    const std::string aspects = detail::AspectSummary(*current, 2);
    if (!aspects.empty()) out += " (" + aspects + ")";
  } else {
    out += "not standing in any place you know";
  }
  const std::string facingWord = SightReport::FacingWord(facing);
  if (!facingWord.empty()) out += ", facing " + facingWord;
  out += ".\n";

  out += "GOAL: " + goal + "\n";
  out += "STEPS: " + std::to_string(step);
  if (!visited.empty()) {
    out += ". Visited: ";
    for (std::size_t i = 0; i < visited.size(); ++i) {
      if (i > 0) out += ", ";
      out += visited[i];
    }
  }
  out += ".\n";

  out += "\nPLACES YOU REMEMBER, nearest first:\n";
  if (candidates.empty()) {
    out += "  (none you could walk to)\n";
  } else {
    for (const Candidate &candidate : candidates) {
      out += "  " + candidate.key + "  " + candidate.description + "\n";
    }
  }
  return out;
}

// The one question asked per decision. The two sentinels are always present and
// always last, so "I have arrived" and "none of these" are never the answer the
// model is forced to improvise around.
inline SystemOne::Option BuildQuestion(const SpatialMemory &memory,
                                       GamePosition here,
                                       const std::vector<Candidate> &candidates) {
  SystemOne::Option next;
  next.id = "next";
  next.type = SystemOne::OptionType::Choice;
  next.instructions =
      "Which of these places is most likely to bring the character closer to "
      "the goal?";

  for (const Candidate &candidate : candidates) {
    next.options.emplace_back(candidate.key, candidate.description);
  }

  const KnownPlace *current = PlaceQuery::At(memory, here);
  if (current != nullptr) {
    next.options.emplace_back(
        "arrived", "you are already where the goal asks you to be (in " +
                       PlaceQuery::DisplayName(*current) + ")");
  } else {
    next.options.emplace_back(
        "arrived",
        "you have not arrived: you are not standing in any place you know");
  }
  next.options.emplace_back(
      "none",
      "none of these places is likely to help; stop and let me reconsider");
  return next;
}

// What the answer means, after the engine has decided whether to trust the
// probability. Keeping this separate from parsing makes the rule testable without
// a network or a world.
struct ChoiceOutcome {
  enum class Kind { Place, Arrived, None, Unsure, Invalid };
  Kind kind = Kind::Invalid;
  int placeId = 0;
  // For the debug log and the summary; -1 means "the backend did not report one".
  float probability = -1.0f;
  float margin = -1.0f;
};

namespace detail {

// The probability the backend attached to `key`, or -1 when it reported none.
// Hosted Jev's choice answers carry `probabilities` even though they carry no
// confidence field, so that map is the primary source; confidence is the fallback
// for a backend that only fills it.
inline float ProbabilityOf(const SystemOne::Answer &answer,
                           const std::string &key) {
  const auto found = answer.probabilities.find(key);
  if (found != answer.probabilities.end()) return found->second;
  if (answer.confidence > 0.0f) return answer.confidence;
  return -1.0f;
}

inline float RunnerUp(const SystemOne::Answer &answer, const std::string &key) {
  float best = -1.0f;
  for (const auto &[option, probability] : answer.probabilities) {
    if (option == key) continue;
    if (probability > best) best = probability;
  }
  return best;
}

}  // namespace detail

// Turn the raw answer into a decision. `candidates` is what the question offered
// (minus the sentinels), so a key that names no candidate is a model error rather
// than a place.
//
// The probability rule: a choice below `minProbability` or too close to the
// runner-up is `Unsure`, not a bad move. When the backend reports no distribution
// at all there is no evidence to gate on, and the choice is taken as stated -- the
// alternative would be to refuse every answer from a backend that omits the
// field.
inline ChoiceOutcome InterpretChoice(const SystemOne::Answer &answer,
                                     const std::vector<Candidate> &candidates,
                                     float minProbability, float minMargin) {
  ChoiceOutcome outcome;

  auto isCandidate = [&candidates](const std::string &key) {
    for (const Candidate &candidate : candidates) {
      if (candidate.key == key) return true;
    }
    return false;
  };

  std::string key = answer.choice;
  if (key != "arrived" && key != "none" && !isCandidate(key)) {
    // An unreadable key is not a decision. If the distribution clearly names a
    // valid option, use that rather than stopping on a parse artefact; otherwise
    // report the malformed answer.
    std::string bestKey;
    float bestProbability = -1.0f;
    for (const auto &[option, probability] : answer.probabilities) {
      if (option != "arrived" && option != "none" && !isCandidate(option)) {
        continue;
      }
      if (probability > bestProbability) {
        bestProbability = probability;
        bestKey = option;
      }
    }
    if (bestKey.empty()) {
      outcome.kind = ChoiceOutcome::Kind::Invalid;
      return outcome;
    }
    key = bestKey;
  }

  outcome.probability = detail::ProbabilityOf(answer, key);
  const float runnerUp = detail::RunnerUp(answer, key);
  outcome.margin =
      outcome.probability >= 0.0f && runnerUp >= 0.0f
          ? outcome.probability - runnerUp
          : -1.0f;

  if (outcome.probability >= 0.0f &&
      (outcome.probability < minProbability ||
       (outcome.margin >= 0.0f && outcome.margin < minMargin))) {
    outcome.kind = ChoiceOutcome::Kind::Unsure;
    return outcome;
  }

  if (key == "arrived") {
    outcome.kind = ChoiceOutcome::Kind::Arrived;
    return outcome;
  }
  if (key == "none") {
    outcome.kind = ChoiceOutcome::Kind::None;
    return outcome;
  }

  for (const Candidate &candidate : candidates) {
    if (candidate.key == key) {
      outcome.kind = ChoiceOutcome::Kind::Place;
      outcome.placeId = candidate.placeId;
      return outcome;
    }
  }
  outcome.kind = ChoiceOutcome::Kind::Invalid;
  return outcome;
}

// The message the main model wakes to. It carries why the episode stopped, the
// route actually walked, where the character is now, and the nearest remembered
// places in the same form [PLACES] prints -- waking with no idea where you are
// wastes the saving in the first turn.
inline std::string BuildSummary(const std::string &reason,
                                const std::vector<std::string> &route,
                                const SpatialMemory &memory, GamePosition here,
                                int legs) {
  std::string out = "System: You stopped";
  if (legs == 1) {
    out += " after 1 leg";
  } else if (legs > 1) {
    out += " after " + std::to_string(legs) + " legs";
  }
  out += ": " + reason + ".\n";

  if (!route.empty()) {
    out += "Route: ";
    for (std::size_t i = 0; i < route.size(); ++i) {
      if (i > 0) out += " -> ";
      out += route[i];
    }
    out += ".\n";
  }

  const KnownPlace *current = PlaceQuery::At(memory, here);
  if (current != nullptr) {
    out += "You are in " + PlaceQuery::DisplayName(*current);
    const std::string aspects = detail::AspectSummary(*current, 3);
    if (!aspects.empty()) out += " (" + aspects + ")";
    out += ".\n";
  } else {
    out += "You are not standing in any place you know.\n";
  }

  const std::vector<PlaceQuery::PlaceLine> nearby =
      PlaceQuery::SortedPlaces(memory, here);
  if (!nearby.empty()) {
    out += "Nearby places you remember:\n";
    const std::size_t shown = std::min<std::size_t>(
        nearby.size(), static_cast<std::size_t>(DEFAULT_NAVIGATION_NEARBY_PLACES));
    for (std::size_t i = 0; i < shown; ++i) {
      out += "  " + PlaceQuery::FormatPlaceLine(nearby[i], here) + "\n";
    }
    out += "You can [MOVE_TO] any of these, or use [PLACES] for the full list.\n";
  }
  return out;
}

// A place-independent name for the starting point of a route: the place the
// character is standing in, or "open ground" when it remembers none.
inline std::string StartPlaceName(const SpatialMemory &memory,
                                  GamePosition here) {
  if (const KnownPlace *place = PlaceQuery::At(memory, here)) {
    return PlaceQuery::DisplayName(*place);
  }
  return "open ground";
}

}  // namespace NavigationPlan
