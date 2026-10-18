#pragma once

// The model-consolidated digest of a character's memory
// (docs/spatial-memory-design.md section 6.2).
//
// Two halves, deliberately separated:
//
//   * the candidates -- a numbered list built by the engine from the runs and
//     places it already has;
//   * the composition -- the model returns *groups of candidate numbers*, and the
//     engine writes each entry's text from the referenced items' own fields.
//
// The model therefore never writes a label or a fact. It selects, groups, and by
// omission drops; the words are the engine's. That restriction is what keeps the
// guardrail in section 6.4 ("one author per field") true even now that a model is
// in the loop.

#include "PlaceQuery.hpp"
#include "SpatialMemory.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace MemoryDigestBuilder {

struct Candidate {
  std::string kind;  // "place" or "run"
  int ref = -1;      // place id, or index into memory.runs
  // What the model sees: enough to judge it, with the engine's own vocabulary.
  std::string line;
  // What the engine will write into the digest when this candidate is chosen.
  std::string label;
};

// A cheap fingerprint of what a digest would be built from: how much ground is
// known, how it compressed, and which places exist. Two records with the same
// fingerprint would produce the same candidate list, so there is nothing to ask a
// model about.
inline long long Signature(const SpatialMemory &memory) {
  long long signature = static_cast<long long>(memory.cells.size()) * 1000003LL;
  signature += static_cast<long long>(memory.runs.size()) * 10007LL;
  signature += static_cast<long long>(memory.places.size()) * 101LL;
  for (const KnownPlace &place : memory.places) {
    signature += place.id * 7919LL;
    signature += static_cast<long long>(place.memberCells.size());
    signature += static_cast<long long>(place.labels.size()) * 31LL;
  }
  return signature;
}

// One numbered line per place, then per run, nearest and best-ranked first. Places
// come first because a place is what a person would name; runs are the detail.
inline std::vector<Candidate> BuildCandidates(const SpatialMemory &memory,
                                              std::size_t maxItems) {
  std::vector<Candidate> candidates;

  std::vector<const KnownPlace *> places;
  places.reserve(memory.places.size());
  for (const KnownPlace &place : memory.places) {
    places.push_back(&place);
  }
  std::sort(places.begin(), places.end(),
            [](const KnownPlace *a, const KnownPlace *b) {
              return a->id < b->id;
            });

  for (const KnownPlace *place : places) {
    Candidate candidate;
    candidate.kind = "place";
    candidate.ref = place->id;
    candidate.label = PlaceQuery::DisplayName(*place);
    std::string line = "place #" + std::to_string(place->id) + ": " +
                       candidate.label;
    const std::string aspects = PlaceQuery::AspectsPhrase(*place, 3);
    if (!aspects.empty()) {
      line += " (" + aspects + ")";
    }
    line += " -- " + std::to_string(place->memberCells.size()) + " cells";
    if (!place->labels.empty()) {
      line += ", named";
    }
    candidate.line = line;
    candidates.push_back(std::move(candidate));
  }

  std::vector<std::size_t> runOrder(memory.runs.size());
  for (std::size_t i = 0; i < runOrder.size(); ++i) {
    runOrder[i] = i;
  }
  std::sort(runOrder.begin(), runOrder.end(),
            [&memory](std::size_t a, std::size_t b) {
              return memory.runs[a].rank > memory.runs[b].rank;
            });

  for (const std::size_t index : runOrder) {
    if (candidates.size() >= maxItems) {
      break;
    }
    const MemoryRun &run = memory.runs[index];
    Candidate candidate;
    candidate.kind = "run";
    candidate.ref = static_cast<int>(index);
    candidate.label = run.surface;
    std::string line = run.surface + " " + std::to_string(run.width) + "x" +
                       std::to_string(run.height) + " at (" +
                       std::to_string(run.origin.x) + "," +
                       std::to_string(run.origin.y) + ")";
    if (run.placeId >= 0) {
      line += ", in place #" + std::to_string(run.placeId);
    } else {
      line += ", unclaimed ground";
    }
    line += ", seen " + std::to_string(run.visitCount) + "x";
    candidate.line = line;
    candidates.push_back(std::move(candidate));
  }

  return candidates;
}

// The prompt body: the numbered list. Kept here rather than in the job so a test
// can read exactly what would be sent without a backend.
inline std::string RenderCandidateList(const std::vector<Candidate> &candidates) {
  std::string out;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    out += std::to_string(i) + ". " + candidates[i].line + "\n";
  }
  return out;
}

// Parses the model's reply into groups of candidate indices.
//
// Accepted shape: a JSON array of arrays of integers, `[[0,3],[1],[2,4,5]]`. Also
// tolerates a bare array of integers, which is then taken as one group each. Any
// index that is out of range is dropped; a group with nothing left is dropped; a
// candidate that appears twice stays with its first group. Everything is sorted so
// the result does not depend on the model's ordering -- two identically grouped
// replies must produce the same digest.
inline std::vector<std::vector<int>>
ParseGroups(const std::string &reply, std::size_t candidateCount) {
  std::vector<std::vector<int>> groups;
  if (candidateCount == 0) {
    return groups;
  }

  // Find the outermost bracket pair; a model may wrap the array in prose. A
  // reasoning model may also emit a <think> block first, and any bracket inside it
  // is not our array, so start after the block when one is present.
  const std::size_t thinkEnd = reply.find("</think>");
  const std::size_t searchFrom =
      thinkEnd == std::string::npos ? 0 : thinkEnd + 8;
  const std::size_t begin = reply.find('[', searchFrom);
  const std::size_t end = reply.rfind(']');
  if (begin == std::string::npos || end == std::string::npos || end <= begin) {
    return groups;
  }
  const std::string body = reply.substr(begin, end - begin + 1);

  std::vector<char> seen(candidateCount, 0);
  std::vector<int> current;
  long long number = -1;
  bool inNumber = false;

  const auto flushNumber = [&]() {
    if (!inNumber) {
      return;
    }
    if (number >= 0 && static_cast<std::size_t>(number) < candidateCount &&
        seen[static_cast<std::size_t>(number)] == 0) {
      seen[static_cast<std::size_t>(number)] = 1;
      current.push_back(static_cast<int>(number));
    }
    number = -1;
    inNumber = false;
  };

  for (const char c : body) {
    if (c >= '0' && c <= '9') {
      number = (inNumber ? number * 10 : 0) + (c - '0');
      inNumber = true;
      continue;
    }
    flushNumber();
    if (c != ']') {
      continue;
    }
    // Every closing bracket ends a group. A flat array therefore yields one group
    // and a nested one yields one per inner array, which is the whole grammar.
    if (!current.empty()) {
      std::sort(current.begin(), current.end());
      groups.push_back(current);
      current.clear();
    }
  }
  flushNumber();
  if (!current.empty()) {
    std::sort(current.begin(), current.end());
    groups.push_back(current);
  }
  return groups;
}

// Composes the digest from the model's groups. Entries are in candidate order, so
// the digest reads places first, then detail -- the same order a person would use.
inline MemoryDigest Compose(const std::vector<Candidate> &candidates,
                            const std::vector<std::vector<int>> &groups,
                            int tick, const std::string &model) {
  MemoryDigest digest;
  digest.tick = tick;
  digest.model = model;
  digest.candidates = static_cast<int>(candidates.size());

  std::vector<std::size_t> order(groups.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(),
            [&groups](std::size_t a, std::size_t b) {
              return groups[a].front() < groups[b].front();
            });

  for (const std::size_t groupIndex : order) {
    const std::vector<int> &group = groups[groupIndex];
    if (group.empty()) {
      continue;
    }
    MemoryDigestEntry entry;
    entry.kind = candidates[static_cast<std::size_t>(group.front())].kind;
    entry.ref = candidates[static_cast<std::size_t>(group.front())].ref;

    // One line per group, built only from the selected candidates' own fields.
    std::string text;
    for (const int index : group) {
      const Candidate &candidate = candidates[static_cast<std::size_t>(index)];
      if (candidate.kind == "place" && text.empty()) {
        text = candidate.label;
        continue;
      }
      if (!text.empty()) {
        text += ", ";
      }
      text += candidate.label;
    }
    entry.text = std::move(text);
    digest.entries.push_back(std::move(entry));
  }

  return digest;
}

}  // namespace MemoryDigestBuilder
