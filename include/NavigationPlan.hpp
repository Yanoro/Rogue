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
// There are two layers of choice, in one list:
//
//   * **here** -- while standing in a place, the sectors around the character are
//     moves *through* that place or *into* the place next to it. This is the local
//     compass. It exists because a `KnownPlace` is too coarse to act on once you
//     are inside it: "the road" is not a destination when you are on the road,
//     "north along the road" is.
//   * **there** -- the nearest remembered places that are not already on the local
//     compass. A distant place's move walks to its nearest edge; the compass takes
//     over after that.
//
// The local compass is built from observed cells on demand. It is *not* a set of
// temporary `KnownPlace`s: creating those would pollute place ids, [PLACES], the
// overlay and the visited set, and would need lifetimes and merge rules. Nothing
// here is stored; the options disappear after the decision.
//
// Design: docs/navigation-subagent-design.md.

#include "Components.h"
#include "Defaults.h"
#include "PlaceQuery.hpp"
#include "SightReport.hpp"
#include "SystemOneRecorder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NavigationPlan {

// One move the model may choose: a local compass direction or a remembered place.
//
// `key` is the string that comes back from the decision model. A local move is
// `here:north` (the direction, not the place id); a distant place is `p12`. The
// engine resolves either to a target cell, never to a name, so a hallucinated
// label cannot move the character.
struct Candidate {
  // The place the move leads into: the current place for a run, the adjacent
  // place for a compass entry, the place itself for a distant target.
  int placeId = 0;
  // Where to walk. Fixed at decision time; the leg never re-aims.
  GamePosition target{-1, -1};
  // "here:north" or "p12".
  std::string key;
  // Short name for the route and the leg: "an unnamed road (north)".
  std::string label;
  // The line in the request: the verb, the run length, the place's scores.
  std::string description;
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

// The eight sectors, in compass order. Screen convention: +x east, +y south.
struct Sector {
  int dx;
  int dy;
};

inline const std::vector<Sector> &Sectors() {
  static const std::vector<Sector> sectors = {
      {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
  return sectors;
}

// Cells with a non-portable object standing on them. The ground may be walkable
// while the cell is not a legal destination (a stall on packed earth, a tree on
// grass); A* refuses the goal and a leg aimed there would fail every time. The
// compass skips these, exactly as PlaceQuery::WalkableMemberCell does for a
// distant target. Characters are not obstacles: they move.
inline std::unordered_set<std::uint64_t> OccupiedCells(
    const SpatialMemory &memory) {
  std::unordered_set<std::uint64_t> occupied;
  for (const auto &[id, object] : memory.objects) {
    if (object.vanishedTick >= 0 || object.character || object.portable) {
      continue;
    }
    occupied.insert(VisionQuery::CellKey(object.pos));
  }
  return occupied;
}

// The first observed place along a sector, within `radius`. Unobserved ground ends
// the search: the character cannot see through it. A walkable cell that belongs to
// no place is open ground, not a stop -- the place beyond it is still in the
// sector, and walking there goes through the gap.
struct SectorPlace {
  const KnownPlace *place = nullptr;
  GamePosition cell{0, 0};
  int distance = 0;
};

inline SectorPlace FirstPlaceInSector(
    const SpatialMemory &memory, GamePosition here, int dx, int dy, int radius,
    const std::unordered_set<std::uint64_t> &occupied) {
  SectorPlace found;
  for (int step = 1; step <= radius; ++step) {
    const GamePosition cell{here.x + dx * step, here.y + dy * step};
    const std::uint64_t key = VisionQuery::CellKey(cell);
    const auto observed = memory.cells.find(key);
    if (observed == memory.cells.end()) break;
    if (!observed->second.walkable) continue;
    if (occupied.count(key) != 0) continue;
    const KnownPlace *place = PlaceQuery::At(memory, cell);
    if (place == nullptr) continue;
    found.place = place;
    found.cell = cell;
    found.distance = step;
    return found;
  }
  return found;
}

// How far the current place continues from `first` along the sector. The run
// stops at an unobserved cell, a non-walkable one, or a different place (the edge
// of the current one). The result is the last cell of the run and its distance,
// which is the leg's target.
inline std::pair<GamePosition, int> RunAlong(
    const SpatialMemory &memory, GamePosition here, int dx, int dy,
    const KnownPlace &current, GamePosition first, int firstDistance, int radius,
    const std::unordered_set<std::uint64_t> &occupied) {
  GamePosition target = first;
  int run = firstDistance;
  for (int step = firstDistance + 1; step <= radius; ++step) {
    const GamePosition cell{here.x + dx * step, here.y + dy * step};
    const std::uint64_t key = VisionQuery::CellKey(cell);
    const auto observed = memory.cells.find(key);
    if (observed == memory.cells.end()) break;
    if (!observed->second.walkable) break;
    if (occupied.count(key) != 0) break;
    const KnownPlace *place = PlaceQuery::At(memory, cell);
    if (place == nullptr || place->id != current.id) break;
    target = cell;
    run = step;
  }
  return {target, run};
}

}  // namespace detail

// The actions the model is shown: local compass moves first, then distant places,
// capped by `maxOptions` in total and `maxDirections` locally.
//
// `walkedCells` marks both "already walked" and "visited": a place is visited when
// any of its member cells is on a path the character actually walked. The place id
// is not used for this -- ids are re-derived as the memory grows, and a stale id
// made an unentered junction branch read as visited in the `17:32` run. The
// current place is never a distant option; the compass is how you act on it.
inline std::vector<Candidate> BuildCandidates(
    const SpatialMemory &memory, GamePosition here, int maxOptions,
    int maxDirections, int compassRadius,
    const std::unordered_set<std::uint64_t> &walkedCells = {}) {
  const KnownPlace *current = PlaceQuery::At(memory, here);
  const std::unordered_set<std::uint64_t> occupied =
      detail::OccupiedCells(memory);

  std::vector<Candidate> currentRuns;
  std::vector<Candidate> adjacent;
  std::unordered_set<int> localPlaceIds;

  if (current != nullptr) {
    for (const detail::Sector &sector : detail::Sectors()) {
      const detail::SectorPlace first = detail::FirstPlaceInSector(
          memory, here, sector.dx, sector.dy, compassRadius, occupied);
      if (first.place == nullptr) continue;

      const std::string direction =
          PlaceQuery::CompassWord(sector.dx, sector.dy);
      if (first.place->id == current->id) {
        const auto [target, run] = detail::RunAlong(
            memory, here, sector.dx, sector.dy, *current, first.cell,
            first.distance, compassRadius, occupied);
        if (run < DEFAULT_NAVIGATION_MIN_RUN) continue;

        Candidate candidate;
        candidate.placeId = current->id;
        candidate.target = target;
        candidate.key = "here:" + direction;
        candidate.label = PlaceQuery::DisplayName(*current) + " (" + direction + ")";
        candidate.distance = run;

        const std::string aspects = detail::AspectSummary(*current, 2);
        // The verb comes from the shape, not from a mode the model has to learn:
        // a line is followed, an area is crossed. It is the same fact the geometry
        // already uses, not a genre rule.
        candidate.description =
            (PlaceQuery::IsLinearPlace(*current)
                 ? "follow " + PlaceQuery::DisplayName(*current) + " " + direction +
                       " for " + std::to_string(run) + " tiles"
                 : "walk " + direction + " through " +
                       PlaceQuery::DisplayName(*current) + " for " +
                       std::to_string(run) + " tiles");
        if (!aspects.empty()) candidate.description += " (" + aspects + ")";
        if (walkedCells.count(VisionQuery::CellKey(candidate.target)) != 0) {
          candidate.description += ", already walked";
        }
        currentRuns.push_back(std::move(candidate));
      } else {
        Candidate candidate;
        candidate.placeId = first.place->id;
        candidate.target = first.cell;
        candidate.key = "here:" + direction;
        candidate.label = PlaceQuery::DisplayName(*first.place) + " (" + direction + ")";
        candidate.distance = first.distance;
        candidate.description =
            PlaceQuery::DisplayName(*first.place) + ", " +
            PlaceQuery::DistancePhrase(here, first.cell);
        const std::string aspects = detail::AspectSummary(*first.place, 2);
        if (!aspects.empty()) candidate.description += " (" + aspects + ")";
        bool visited = false;
        for (const std::uint64_t cell : first.place->memberCells) {
          if (walkedCells.count(cell) != 0) {
            visited = true;
            break;
          }
        }
        if (visited) candidate.description += ", visited";
        if (walkedCells.count(VisionQuery::CellKey(candidate.target)) != 0) {
          candidate.description += ", already walked";
        }
        adjacent.push_back(std::move(candidate));
      }
    }

    // A wide area can offer eight runs; the longest are the meaningful
    // continuations. Ties keep compass order (stable sort).
    std::stable_sort(currentRuns.begin(), currentRuns.end(),
                     [](const Candidate &a, const Candidate &b) {
                       return a.distance > b.distance;
                     });

    // Several sectors can lead into the same neighbouring place -- a road in a
    // forest sees orchard east, north-east and south-east. One entry is enough;
    // keep the nearest sector.
    std::stable_sort(adjacent.begin(), adjacent.end(),
                     [](const Candidate &a, const Candidate &b) {
                       return a.distance < b.distance;
                     });
    std::vector<Candidate> uniqueAdjacent;
    std::unordered_set<int> seenAdjacent;
    for (Candidate &candidate : adjacent) {
      if (seenAdjacent.insert(candidate.placeId).second) {
        uniqueAdjacent.push_back(std::move(candidate));
      }
    }
    adjacent = std::move(uniqueAdjacent);
  }

  std::vector<Candidate> candidates;
  std::size_t directionLimit =
      maxDirections > 0 ? static_cast<std::size_t>(maxDirections) : 0;
  if (maxOptions > 0) {
    directionLimit = std::min(directionLimit, static_cast<std::size_t>(maxOptions));
  }
  // One adjacent place is guaranteed a slot even in a wide current place: the
  // decision "stay here or leave into that orchard" needs the orchard on the
  // list. The rest of the cap goes to the longest runs of the current place,
  // then to the remaining adjacent places. Without this, four orchard runs can
  // crowd the one-tile road out of the compass and it reappears only as a
  // distant place.
  std::size_t adjacentIndex = 0;
  if (adjacentIndex < adjacent.size() && candidates.size() < directionLimit) {
    localPlaceIds.insert(adjacent[adjacentIndex].placeId);
    candidates.push_back(std::move(adjacent[adjacentIndex]));
    ++adjacentIndex;
  }
  for (Candidate &candidate : currentRuns) {
    if (candidates.size() >= directionLimit) break;
    candidates.push_back(std::move(candidate));
  }
  for (; adjacentIndex < adjacent.size(); ++adjacentIndex) {
    if (candidates.size() >= directionLimit) break;
    localPlaceIds.insert(adjacent[adjacentIndex].placeId);
    candidates.push_back(std::move(adjacent[adjacentIndex]));
  }

  // Distant places: what is left after the local compass, nearest first. A place
  // already offered as a local entry is not repeated; the local move is the way in.
  struct Distant {
    const KnownPlace *place = nullptr;
    GamePosition target{0, 0};
    int distance = 0;
  };
  std::vector<Distant> distant;
  for (const KnownPlace &place : memory.places) {
    if (place.memberCells.empty()) continue;
    if (current != nullptr && place.id == current->id) continue;
    if (localPlaceIds.count(place.id) != 0) continue;
    const GamePosition target = PlaceQuery::NearestWalkableCell(memory, place, here);
    if (target == here) continue;
    Distant entry;
    entry.place = &place;
    entry.target = target;
    entry.distance = detail::Chebyshev(target, here);
    distant.push_back(entry);
  }
  std::stable_sort(distant.begin(), distant.end(),
                   [](const Distant &a, const Distant &b) {
                     return a.distance < b.distance;
                   });

  for (const Distant &entry : distant) {
    if (maxOptions > 0 &&
        candidates.size() >= static_cast<std::size_t>(maxOptions)) {
      break;
    }
    Candidate candidate;
    candidate.placeId = entry.place->id;
    candidate.target = entry.target;
    candidate.key = "p" + std::to_string(entry.place->id);
    candidate.label = PlaceQuery::DisplayName(*entry.place);
    candidate.distance = entry.distance;
    candidate.description =
        PlaceQuery::DisplayName(*entry.place) + ", " +
        PlaceQuery::DistancePhrase(here, entry.target);
    const std::string aspects = detail::AspectSummary(*entry.place, 2);
    if (!aspects.empty()) candidate.description += " (" + aspects + ")";
    bool visited = false;
    for (const std::uint64_t cell : entry.place->memberCells) {
      if (walkedCells.count(cell) != 0) {
        visited = true;
        break;
      }
    }
    if (visited) candidate.description += ", visited";
    candidates.push_back(std::move(candidate));
  }

  return candidates;
}

// The state handed to System 1. It must be readable on its own: System 1 has no
// world model and no memory of the previous step, so the goal, where the
// character is standing, what it has already visited, and the moves are all
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

  out += "\nACTIONS, local first:\n";
  if (candidates.empty()) {
    out += "  (none you could take)\n";
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
      "Which of these moves is most likely to bring the character closer to "
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
      "none of these moves is likely to help; stop and let me reconsider");
  return next;
}

// What the answer means, after the engine has decided whether to trust the
// probability. Keeping this separate from parsing makes the rule testable without
// a network or a world.
struct ChoiceOutcome {
  enum class Kind { Candidate, Arrived, None, Unsure, Invalid };
  Kind kind = Kind::Invalid;
  std::size_t candidateIndex = 0;
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
// (minus the sentinels), so a key that names no action is a model error rather
// than a place.
//
// The probability rule: a choice below `minProbability` or too close to the
// runner-up is `Unsure`, not a bad move. When the backend reports no distribution
// at all there is no evidence to gate on, and the choice is taken as stated -- the
// alternative would be to refuse every answer from a backend that omits the
// field.
//
// The runner-up margin applies to **distant place** moves only. A local compass
// move is a six-tile walk that the next decision can undo; when the model puts
// 0.41 on west and 0.30 on east, walking west is the best available guess and a
// survey is unlikely to resolve it. A distant place is a bigger commitment (and
// was the original one-shot protocol), so it keeps the margin. The absolute
// probability floor applies to both, so a genuinely weak local answer still
// surveys.
inline ChoiceOutcome InterpretChoice(const SystemOne::Answer &answer,
                                     const std::vector<Candidate> &candidates,
                                     float minProbability, float minMargin) {
  ChoiceOutcome outcome;

  auto applyKey = [&outcome, &candidates](const std::string &key) {
    if (key == "arrived") {
      outcome.kind = ChoiceOutcome::Kind::Arrived;
      return true;
    }
    if (key == "none") {
      outcome.kind = ChoiceOutcome::Kind::None;
      return true;
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      if (candidates[i].key == key) {
        outcome.kind = ChoiceOutcome::Kind::Candidate;
        outcome.candidateIndex = i;
        return true;
      }
    }
    return false;
  };

  std::string key = answer.choice;
  if (!applyKey(key)) {
    // An unreadable key is not a decision. If the distribution clearly names a
    // valid option, use that rather than stopping on a parse artefact; otherwise
    // report the malformed answer.
    auto knownKey = [&candidates](const std::string &candidateKey) {
      if (candidateKey == "arrived" || candidateKey == "none") return true;
      for (const Candidate &candidate : candidates) {
        if (candidate.key == candidateKey) return true;
      }
      return false;
    };

    std::string bestKey;
    float bestProbability = -1.0f;
    for (const auto &[option, probability] : answer.probabilities) {
      if (!knownKey(option)) continue;
      if (probability > bestProbability) {
        bestProbability = probability;
        bestKey = option;
      }
    }
    if (bestKey.empty() || !applyKey(bestKey)) {
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

  const bool local =
      outcome.kind == ChoiceOutcome::Kind::Candidate &&
      candidates[outcome.candidateIndex].key.rfind("here:", 0) == 0;
  if (outcome.probability >= 0.0f &&
      (outcome.probability < minProbability ||
       (!local && outcome.margin >= 0.0f && outcome.margin < minMargin))) {
    outcome.kind = ChoiceOutcome::Kind::Unsure;
    return outcome;
  }

  return outcome;
}

// The message the main model wakes to. It carries why the episode stopped, the
// route actually walked, where the character is now, and the nearest remembered
// places in the same form [PLACES] prints -- waking with no idea where you are
// wastes the saving in the first turn.
// A compact, human-readable shape of the walk. The route labels are the leg
// names ("an unnamed road (west)"), so consecutive labels with the same base and
// direction collapse to one run. The main model gets the shape, not a fifteen-row
// arrow list; the full route is still in NavigationDebugState for the debug view.
inline std::string DescribeJourney(const std::vector<std::string> &route) {
  if (route.size() <= 1) return std::string();

  struct Run {
    std::string base;
    std::string direction;
    int count = 1;
  };
  std::vector<Run> runs;
  for (std::size_t i = 1; i < route.size(); ++i) {
    std::string base = route[i];
    std::string direction;
    const std::size_t open = base.rfind(" (");
    if (open != std::string::npos && !base.empty() && base.back() == ')') {
      direction = base.substr(open + 2, base.size() - open - 3);
      base.resize(open);
    }
    if (!runs.empty() && runs.back().base == base &&
        runs.back().direction == direction) {
      ++runs.back().count;
    } else {
      runs.push_back({base, direction, 1});
    }
  }

  std::ostringstream out;
  out << "left " << route.front();
  std::string previousBase;
  for (const Run &run : runs) {
    if (run.direction.empty()) {
      out << "; took " << run.base;
    } else if (run.base == previousBase) {
      out << ", " << run.direction;
      if (run.count > 1) out << " x" << run.count;
    } else {
      out << "; " << run.base << " " << run.direction;
      if (run.count > 1) out << " x" << run.count;
    }
    previousBase = run.base;
  }
  return out.str();
}

inline std::string BuildSummary(const std::string &reason,
                                const std::vector<std::string> &route,
                                const SpatialMemory &memory, GamePosition here,
                                int legs, bool arrived = false) {
  const KnownPlace *current = PlaceQuery::At(memory, here);

  std::string out;
  if (arrived) {
    out = "System: You arrived";
    if (current != nullptr) {
      out += " at " + PlaceQuery::DisplayName(*current);
    }
  } else {
    out = "System: You stopped";
  }
  if (legs == 1) {
    out += " after 1 leg";
  } else if (legs > 1) {
    out += " after " + std::to_string(legs) + " legs";
  }
  if (!arrived) out += ": " + reason;
  out += ".\n";

  if (route.size() > 1) {
    const std::string journey = DescribeJourney(route);
    if (!journey.empty()) out += "Journey: " + journey + ".\n";
  }

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
    // Two derived places can legitimately share a display name (the village edge
    // and the market core both "Village"). The list's #id is not a MOVE_TO target,
    // so name the other one explicitly and give the model the working command.
    if (current != nullptr && !current->labels.empty()) {
      const std::string currentName = PlaceQuery::DisplayName(*current);
      for (std::size_t i = 0; i < shown; ++i) {
        const KnownPlace &other = *nearby[i].place;
        if (other.id == current->id || other.labels.empty()) continue;
        if (PlaceQuery::DisplayName(other) != currentName) continue;
        out += "Another place named " + currentName + ": " +
               PlaceQuery::FormatPlaceLine(nearby[i], here) +
               ". If you mean that one, use [NAME_PLACE #" +
               std::to_string(other.id) + " <name>] first.\n";
        break;
      }
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
