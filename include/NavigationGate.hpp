#pragma once

// The navigation attention gate (docs/agent-attention-design.md, §4).
//
// The engine reports *facts*: which object groups, places and characters the
// agent has newly observed since the last check. A System 1 model turns those
// facts into one small decision:
//
//   stop   -- pause the leg and wake the agent now;
//   note   -- keep walking, but surface the report at the next decision;
//   ignore -- keep walking; the thing stays in memory and the chunk strip.
//
// There is deliberately no confidence and no per-monitor id. A small model is a
// classifier here, not a calibrated probability source, and the watch brief is a
// priority for a general attention question rather than a checklist that bounds
// what can be noticed.

#include "Components.h"
#include "PlaceQuery.hpp"
#include "SystemOneRecorder.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class GateVerdict { Ignore, Note, Stop };

struct GateReport {
  bool hasNew = false;
  std::string text;
  // Content keys of new object groups, so the caller can remember them and not
  // report the same group again. New places/characters are deduped by tick.
  std::vector<std::string> newGroups;
};

inline GateReport BuildGateReport(
    const SpatialMemory &memory,
    const std::unordered_set<std::string> &knownGroups,
    int lastGateTick) {
  GateReport report;

  struct Group {
    std::string name;
    int count = 0;
  };
  std::unordered_map<std::string, Group> groups;
  std::vector<std::string> characters;
  std::vector<std::string> places;
  std::unordered_set<std::string> seenPlaceNames;

  for (const auto &[id, object] : memory.objects) {
    (void)id;
    if (object.vanishedTick >= 0) continue;
    if (object.character) {
      if (object.firstSeenTick > lastGateTick) {
        characters.push_back(object.name.empty() ? "a person" : object.name);
      }
      continue;
    }
    const std::string key = ObjectContentKey(object);
    if (knownGroups.count(key) != 0) continue;
    Group &group = groups[key];
    if (group.count == 0) {
      group.name = object.name.empty() ? key : object.name;
    }
    ++group.count;
  }

  for (const KnownPlace &place : memory.places) {
    if (place.firstSeenTick <= lastGateTick) continue;
    const std::string name = PlaceQuery::DisplayName(place);
    if (name.empty() || !seenPlaceNames.insert(name).second) continue;
    places.push_back(name);
  }
  for (const KnownPlace &region : memory.regions) {
    if (region.firstSeenTick <= lastGateTick) continue;
    const std::string name = PlaceQuery::DisplayName(region);
    if (name.empty() || !seenPlaceNames.insert(name).second) continue;
    places.push_back(name);
  }

  if (groups.empty() && characters.empty() && places.empty()) {
    return report;
  }
  report.hasNew = true;
  for (const auto &[key, group] : groups) {
    (void)group;
    report.newGroups.push_back(key);
  }

  std::ostringstream text;
  text << "NEW SINCE LAST CHECK:\n";
  constexpr std::size_t kMaxGroups = 6;
  constexpr std::size_t kMaxPlaces = 3;
  constexpr std::size_t kMaxCharacters = 3;

  if (!groups.empty()) {
    std::vector<std::pair<std::string, Group>> ordered(groups.begin(),
                                                       groups.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &a, const auto &b) {
                if (a.second.count != b.second.count) {
                  return a.second.count > b.second.count;
                }
                return a.second.name < b.second.name;
              });
    text << "  objects: ";
    for (std::size_t i = 0; i < ordered.size(); ++i) {
      if (i == kMaxGroups) {
        text << ", and " << (ordered.size() - kMaxGroups) << " more";
        break;
      }
      if (i > 0) text << ", ";
      text << ordered[i].second.name << " x" << ordered[i].second.count;
    }
    text << "\n";
  }
  if (!places.empty()) {
    text << "  places: ";
    for (std::size_t i = 0; i < places.size(); ++i) {
      if (i == kMaxPlaces) {
        text << ", and " << (places.size() - kMaxPlaces) << " more";
        break;
      }
      if (i > 0) text << ", ";
      text << places[i];
    }
    text << "\n";
  }
  if (!characters.empty()) {
    text << "  characters: ";
    for (std::size_t i = 0; i < characters.size(); ++i) {
      if (i == kMaxCharacters) {
        text << ", and " << (characters.size() - kMaxCharacters) << " more";
        break;
      }
      if (i > 0) text << ", ";
      text << characters[i];
    }
    text << "\n";
  }
  report.text = text.str();
  return report;
}

inline std::string BuildGateState(const std::string &goal,
                                  const std::string &subtask,
                                  const std::string &watch,
                                  const GateReport &report,
                                  const std::string &journey,
                                  const std::string &areaCard) {
  std::ostringstream state;
  state << "GOAL: " << goal << "\n";
  if (!subtask.empty()) state << "SUBTASK: " << subtask << "\n";
  state << "WATCH: " << (watch.empty() ? "(none)" : watch) << "\n\n";
  state << report.text;
  if (!journey.empty()) state << "\nROUTE: " << journey << "\n";
  if (!areaCard.empty()) state << "\n" << areaCard;
  state << "\nThe watch is a priority, not a filter. Stop for anything a person "
           "would point out or that would change the plan, even if it is not in "
           "the watch.\n";
  return state.str();
}

inline SystemOne::Option BuildGateQuestion() {
  SystemOne::Option question;
  question.id = "gate";
  question.type = SystemOne::OptionType::Choice;
  question.instructions =
      "Decide the character's attention: stop now and wake them, note this for "
      "the next decision but keep moving, or ignore it. The watch is a priority, "
      "not a filter; stop for anything a person would point out or that would "
      "change the plan, even if it is not in the watch.";
  question.options = {
      {"stop", "stop and wake the character now"},
      {"note", "keep walking; surface this at the next decision"},
      {"ignore", "not worth interrupting"},
  };
  return question;
}

inline GateVerdict ParseGateVerdict(
    const std::vector<SystemOne::Answer> &answers) {
  for (const SystemOne::Answer &answer : answers) {
    if (answer.id != "gate") continue;
    if (answer.choice == "stop") return GateVerdict::Stop;
    if (answer.choice == "ignore") return GateVerdict::Ignore;
    if (answer.choice == "note") return GateVerdict::Note;
    return GateVerdict::Note;
  }
  // No answer, or an answer we did not ask for: keep moving and surface it at
  // the next decision rather than dropping the percept on the floor.
  return GateVerdict::Note;
}
