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
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <stop_token>
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

// ── One leg's gate ──────────────────────────────────────────────────────────
//
// The navigator's gate lives inside its episode action because that action has a
// phase to return to. Continuous navigation has no phase, so the gate is a small
// object the walking action owns: Begin it when the leg starts, Update it while
// the leg walks, Poll it for the verdict, and let the caller decide what "stop"
// means. The facts, the question and the verdict parsing are the ones above, so
// the two modes stay comparable in the records.

// The state the classifier sees for a walk. There is no goal to state in
// continuous navigation, so the plan (if the agent wrote one) is what carries
// intention, and `where` keeps the facts grounded.
inline std::string BuildWalkGateState(const std::string &plan,
                                      const std::string &watch,
                                      const std::string &where,
                                      const GateReport &report,
                                      const std::string &lastResult) {
  std::ostringstream state;
  if (!plan.empty()) state << "PLAN: " << plan << "\n";
  state << "WATCH: " << (watch.empty() ? "(none)" : watch) << "\n";
  if (!where.empty()) state << "YOU ARE: " << where << "\n";
  if (!lastResult.empty()) state << "LAST: " << lastResult << "\n";
  state << "\n" << report.text;
  state << "\nThe watch is a priority, not a filter. Stop for anything a person "
           "would point out or that would change the plan, even if it is not in "
           "the watch.\n";
  return state.str();
}

// The one-line version for call attribution. Kept free-standing so a call site
// that is not the navigator does not have to re-derive it.
inline std::string GateSummaryOf(const GateReport &report) {
  std::string text = report.text;
  for (char &c : text) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  while (!text.empty() && text.front() == ' ') text.erase(text.begin());
  if (text.size() > 160) text.resize(160);
  return text;
}

class WalkGate {
public:
  struct Verdict {
    GateVerdict kind = GateVerdict::Note;
    std::string report;
  };

  // Takes the character's current state as the baseline for "new": a group it
  // has seen before this leg is not news, even if it was seen on another leg.
  void Begin(flecs::entity entity) {
    Abandon();
    noted.clear();
    reportText.clear();
    calls = 0;
    lastScan = Clock::now();
    SpatialMemory *memory = entity.get_mut<SpatialMemory>();
    if (memory == nullptr) return;
    lastGateTick = memory->lastVisionTick;
    knownGroups.clear();
    for (const auto &[id, object] : memory->objects) {
      (void)id;
      if (object.vanishedTick >= 0 || object.character) continue;
      knownGroups.insert(ObjectContentKey(object));
    }
  }

  bool Busy() const { return pending != nullptr; }
  const std::string &Noted() const { return noted; }

  void AppendNote(const std::string &text) {
    constexpr std::size_t kMaxNoted = 2000;
    if (noted.size() >= kMaxNoted) return;
    const std::size_t room = kMaxNoted - noted.size();
    if (text.size() <= room) {
      noted += text;
    } else {
      noted.append(text, 0, room);
    }
  }

  // Scans for percepts new since Begin and asks the classifier when there are
  // some. Returns true when the caller should pause the leg while the answer is
  // in flight: the character visibly stops to look, exactly as in an episode.
  bool Update(flecs::entity entity, const std::string &plan,
              const std::string &watch, const std::string &where,
              const std::string &lastResult) {
    if (watch.empty()) return false;
    if (pending != nullptr) return false;
    if (calls >= kMaxCalls) return false;
    if (SecondsSince(lastScan) < kScanSeconds) return false;
    lastScan = Clock::now();

    const SpatialMemory *memory = entity.get<SpatialMemory>();
    if (memory == nullptr) return false;
    const GateReport report =
        BuildGateReport(*memory, knownGroups, lastGateTick);
    if (!report.hasNew) return false;

    ++calls;
    for (const std::string &group : report.newGroups) {
      knownGroups.insert(group);
    }

    const SystemOneClassifierResource *resource =
        entity.world().get<SystemOneClassifierResource>();
    const bool available = resource != nullptr &&
                           resource->classifier != nullptr &&
                           resource->classifier->Available();
    if (!available) {
      // Degrade, never block: the report surfaces with the leg's result instead.
      AppendNote("not judged (System 1 unavailable):\n" + report.text);
      if (SpatialMemory *mutableMemory = entity.get_mut<SpatialMemory>()) {
        lastGateTick = mutableMemory->lastVisionTick;
      }
      return false;
    }

    reportText = report.text;
    pending = std::make_shared<Pending>();
    stop = std::stop_source();
    startedAt = Clock::now();

    const std::string state =
        BuildWalkGateState(plan, watch, where, report, lastResult);
    std::vector<SystemOne::Option> questions{BuildGateQuestion()};
    std::shared_ptr<Pending> shared = pending;
    SystemOne::Attribution attribution;
    attribution.scenario = "attention_gate";
    attribution.summary = AgentNameOf(entity) + ", gate: " +
                          GateSummaryOf(report);
    attribution.advisory = true;
    resource->classifier->Ask(
        state, questions,
        [shared](const std::vector<SystemOne::Answer> &answers,
                 const SystemOne::CallTrace &trace) {
          std::lock_guard<std::mutex> lock(shared->mutex);
          if (shared->abandoned) return;
          shared->answers = answers;
          shared->trace = trace;
          shared->ready = true;
        },
        stop.get_token(), attribution);
    return true;
  }

  // True when an answer (or a timeout) was consumed. On `note` the report is
  // appended to Noted() and the caller keeps walking.
  bool Poll(flecs::entity entity, Verdict &verdict) {
    if (pending == nullptr) return false;

    if (SecondsSince(startedAt) > kTimeoutSeconds) {
      verdict.kind = GateVerdict::Note;
      verdict.report = reportText;
      FinishCall(entity);
      AppendNote("not judged (timed out):\n" + verdict.report);
      return true;
    }

    std::vector<SystemOne::Answer> answers;
    SystemOne::CallTrace trace;
    {
      std::lock_guard<std::mutex> lock(pending->mutex);
      if (!pending->ready) return false;
      answers = pending->answers;
      trace = pending->trace;
    }
    verdict.kind = trace.Ok() ? ParseGateVerdict(answers) : GateVerdict::Note;
    verdict.report = reportText;
    FinishCall(entity);
    if (verdict.kind == GateVerdict::Note) AppendNote(verdict.report);
    return true;
  }

  void Abandon() {
    if (pending != nullptr) {
      {
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->abandoned = true;
        pending->ready = false;
      }
      stop.request_stop();
      pending.reset();
    }
  }

private:
  struct Pending {
    std::mutex mutex;
    bool ready = false;
    bool abandoned = false;
    std::vector<SystemOne::Answer> answers;
    SystemOne::CallTrace trace;
  };

  using Clock = std::chrono::steady_clock;

  static constexpr int kMaxCalls = 24;
  static constexpr double kScanSeconds = 0.2;
  static constexpr double kTimeoutSeconds = 2.0;

  static double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
  }

  static std::string AgentNameOf(flecs::entity entity) {
    if (const DisplayName *name = entity.get<DisplayName>()) {
      if (!name->name.empty()) return name->name;
    }
    const std::string named = entity.name().c_str();
    return named.empty() ? std::string("character") : named;
  }

  void FinishCall(flecs::entity entity) {
    pending.reset();
    stop = std::stop_source();
    lastScan = Clock::now();
    if (SpatialMemory *memory = entity.get_mut<SpatialMemory>()) {
      lastGateTick = memory->lastVisionTick;
    }
    reportText.clear();
  }

  std::shared_ptr<Pending> pending;
  std::stop_source stop;
  Clock::time_point startedAt = Clock::now();
  Clock::time_point lastScan = Clock::now();
  int lastGateTick = -1;
  int calls = 0;
  std::string reportText;
  std::string noted;
  std::unordered_set<std::string> knownGroups;
};
