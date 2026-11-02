#pragma once

// The LLM strategist + System 1 pilot layer (docs/llm-s1-navigation-design.md).
//
// Pure-ish pieces:
//   * the strategist's JSON plan parser and validator,
//   * the monitor questions/evaluation (System 1 Score answers),
//   * the observation the strategist sees (local JSON grid + minimal observed
//     graph),
//   * the compact observation/pilot preamble the S1 sees,
//   * the strategist prompt.
//
// Nothing here calls a model. `NavigateAction` owns the async AI call, the S1
// Ask, the handoff and the budgets; this header only formats and parses.

#include "Components.h"
#include "NavigationPlan.hpp"
#include "PlaceQuery.hpp"
#include "SightReport.hpp"
#include "SystemOneRecorder.h"
#include "VisionQuery.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NavigationStrategist {

// A monitor is a yes/no question the strategist wants the S1 to answer at every
// decision point. `handoffOnYes` is the trigger; `minConfidence` is the minimum
// confidence (2*|p-0.5|) before the answer is allowed to count.
struct Monitor {
  std::string id;
  std::string question;
  bool handoffOnYes = true;
  float minConfidence = 0.6f;
};

struct Plan {
  std::string strategy;
  std::string subtask;
  // Optional brief for the mid-leg attention gate: what the navigator wants to
  // be interrupted for while it is walking. It is a priority, not a filter.
  std::string watch;
  std::vector<Monitor> monitors;
  bool giveUp = false;
  std::string giveUpReason;
  int version = 0;
};

struct Handoff {
  bool triggered = false;
  std::string reason;
};

namespace detail {

inline std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

inline bool Contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

inline void ReplaceAll(std::string &text, const std::string &from,
                       const std::string &to) {
  if (from.empty()) {
    return;
  }
  std::size_t position = 0;
  while ((position = text.find(from, position)) != std::string::npos) {
    text.replace(position, from.size(), to);
    position += to.size();
  }
}

inline int Chebyshev(const GamePosition &a, const GamePosition &b) {
  return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

inline std::string PlaceLabel(const KnownPlace &place) {
  std::string label = PlaceQuery::DisplayName(place);
  const std::string aspects = PlaceQuery::AspectsPhrase(place, 2);
  if (!aspects.empty()) label += " (" + aspects + ")";
  return label;
}

// A single character for the observation grid. Trees/buildings/signs/characters
// are the categories the strategist's questions ask about; everything else is
// generic so a genre pack does not need new C++ codes.
inline std::string ObjectCode(const SeenObject &object) {
  const std::string text = Lower(object.templateId + " " + object.name);
  if (Contains(text, "sign")) return "S";
  if (object.character) return "C";
  if (Contains(text, "tree") || Contains(text, "oak") ||
      Contains(text, "pine") || Contains(text, "apple") ||
      Contains(text, "bush")) {
    return "T";
  }
  if (object.structural || Contains(text, "building") ||
      Contains(text, "house") || Contains(text, "workshop") ||
      Contains(text, "mill") || Contains(text, "smith") ||
      Contains(text, "well") || Contains(text, "wall") ||
      Contains(text, "stall") || Contains(text, "market")) {
    return "B";
  }
  if (Contains(text, "water") || Contains(text, "lake") ||
      Contains(text, "river") || Contains(text, "pond")) {
    return "~";
  }
  return "O";
}

inline std::string ObjectLabel(const SeenObject &object) {
  if (!object.name.empty()) return object.name;
  if (!object.templateId.empty()) return object.templateId;
  return object.character ? "person" : "object";
}

inline bool PlaceWalked(const KnownPlace &place,
                        const std::unordered_set<std::uint64_t> &walkedCells) {
  for (const std::uint64_t cell : place.memberCells) {
    if (walkedCells.count(cell) != 0) return true;
  }
  return false;
}

inline nlohmann::json AspectsJson(const KnownPlace &place, std::size_t count) {
  nlohmann::json aspects = nlohmann::json::object();
  std::vector<const PlaceAspect *> sorted;
  for (const PlaceAspect &aspect : place.aspects) {
    if (aspect.mean > 0.0f) sorted.push_back(&aspect);
  }
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const PlaceAspect *a, const PlaceAspect *b) {
                     return a->mean > b->mean;
                   });
  for (std::size_t i = 0; i < sorted.size() && i < count; ++i) {
    aspects[sorted[i]->category] =
        static_cast<double>(static_cast<int>(sorted[i]->mean * 100.0f)) / 100.0;
  }
  return aspects;
}

inline std::string DirectionPhrase(GamePosition from, GamePosition to) {
  if (from == to) return "here";
  return PlaceQuery::CompassWord(to.x - from.x, to.y - from.y) + " " +
         std::to_string(Chebyshev(from, to)) + " tiles";
}

}  // namespace detail

// ── Plan parsing ─────────────────────────────────────────────────────────────

// The strategist is asked for a short <think>...</think> before its JSON. The
// engine ignores it for planning; the Navigation window shows it. An unclosed
// block is the streaming case: return the text so far so the live view updates.
inline std::string ExtractThinking(const std::string &raw) {
  const std::string lower = detail::Lower(raw);
  const std::size_t open = lower.find("<think>");
  if (open == std::string::npos) return std::string();
  const std::size_t start = open + 7;
  const std::size_t close = lower.find("</think>", start);
  std::size_t end = close == std::string::npos ? raw.size() : close;
  // If the stream has already moved on to the plan JSON without a closing tag,
  // the live thought must not leak the JSON into the thoughts view.
  if (close == std::string::npos) {
    const std::size_t json = lower.find("{\"strategy\"", start);
    if (json != std::string::npos && json < end) end = json;
  }
  std::string text = raw.substr(start, end - start);
  // Models sometimes put the JSON (or a fenced JSON block) inside the think
  // block. Keep the thought as prose; StripToJson separately recovers the JSON
  // from the raw reply.
  const std::size_t fence = text.find("```");
  if (fence != std::string::npos) text.resize(fence);
  const std::size_t brace = text.find('{');
  if (brace != std::string::npos) text.resize(brace);
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return std::string();
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

// Thought extraction with a fallback for models that ignore <think> tags. The
// latest logs showed phi4-mini writing a prose "THINK:" block and then JSON; the
// tagged extractor returned empty, so the debug window had nothing to show after
// streaming ended. This keeps only the prose before the JSON/code fence.
inline std::string ExtractThought(const std::string &raw) {
  const std::string tagged = ExtractThinking(raw);
  if (!tagged.empty()) return tagged;

  std::string text = raw;
  const std::size_t fence = text.find("```");
  if (fence != std::string::npos) text.resize(fence);
  const std::size_t brace = text.find('{');
  if (brace != std::string::npos) text.resize(brace);
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return std::string();
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  text = text.substr(first, last - first + 1);
  // Strip a leading "THINK:" heading if the model wrote one.
  if (text.rfind("THINK:", 0) == 0) {
    text.erase(0, 6);
    const std::size_t content = text.find_first_not_of(" \t\r\n");
    text = content == std::string::npos ? std::string() : text.substr(content);
  }
  return text;
}

// Model output is prose with a JSON island more often than it is a JSON
// document. Remove <think> blocks and code fences, then take the outermost
// braces. An invalid reply is a parse error, not a silent empty plan.
inline std::string StripToJson(const std::string &raw) {
  std::string text = raw;
  try {
    const std::regex think(R"(<think>[\s\S]*?</think>)",
                           std::regex_constants::icase);
    text = std::regex_replace(text, think, "");
  } catch (const std::regex_error &) {
  }

  const std::size_t fence = text.find("```");
  if (fence != std::string::npos) {
    std::size_t start = text.find('\n', fence);
    if (start == std::string::npos) return std::string();
    ++start;
    const std::size_t end = text.find("```", start);
    text = end == std::string::npos ? text.substr(start)
                                    : text.substr(start, end - start);
  }

  const auto braces = [](const std::string &candidate) {
    const std::size_t open = candidate.find('{');
    const std::size_t close = candidate.rfind('}');
    if (open == std::string::npos || close == std::string::npos ||
        close <= open) {
      return std::string();
    }
    return candidate.substr(open, close - open + 1);
  };

  std::string json = braces(text);
  if (!json.empty()) return json;
  // The JSON may be inside the <think> block (or inside a fence there). The
  // first/last braces in the raw reply still delimit it; stripping the block
  // first was throwing the only JSON away.
  return braces(raw);
}

inline bool ParsePlan(const std::string &raw, Plan &plan, std::string &error) {
  const std::string jsonText = StripToJson(raw);
  if (jsonText.empty()) {
    error = "no JSON object in the strategist reply";
    return false;
  }

  nlohmann::json root;
  try {
    root = nlohmann::json::parse(jsonText);
  } catch (const std::exception &e) {
    error = std::string("invalid JSON: ") + e.what();
    return false;
  }
  if (!root.is_object()) {
    error = "the strategist reply is not a JSON object";
    return false;
  }

  plan = Plan{};
  if (root.contains("give_up") && root["give_up"].is_boolean() &&
      root["give_up"].get<bool>()) {
    plan.giveUp = true;
    plan.giveUpReason = root.value("reason", std::string());
    return true;
  }

  if (!root.contains("strategy") || !root["strategy"].is_string() ||
      root["strategy"].get<std::string>().empty()) {
    error = "missing non-empty strategy";
    return false;
  }
  plan.strategy = root["strategy"].get<std::string>();
  if (plan.strategy.size() > 800) plan.strategy.resize(800);
  plan.subtask = root.value("subtask", std::string());
  if (plan.subtask.size() > 240) plan.subtask.resize(240);
  if (plan.subtask.empty()) plan.subtask = "make progress toward the goal";
  if (root.contains("watch")) {
    if (!root["watch"].is_string()) {
      error = "watch is not a string";
      return false;
    }
    plan.watch = root["watch"].get<std::string>();
    if (plan.watch.size() > 400) plan.watch.resize(400);
  }

  if (root.contains("monitors")) {
    if (!root["monitors"].is_array()) {
      error = "monitors is not an array";
      return false;
    }
    std::unordered_set<std::string> ids;
    for (const auto &entry : root["monitors"]) {
      if (!entry.is_object()) {
        error = "a monitor is not an object";
        return false;
      }
      Monitor monitor;
      monitor.id = entry.value("id", std::string());
      if (monitor.id.empty()) {
        error = "a monitor has no id";
        return false;
      }
      if (ids.count(monitor.id) != 0) {
        error = "duplicate monitor id '" + monitor.id + "'";
        return false;
      }
      ids.insert(monitor.id);
      monitor.question = entry.value("question", std::string());
      if (monitor.question.empty()) {
        error = "monitor '" + monitor.id + "' has no question";
        return false;
      }
      if (monitor.question.size() > 240) monitor.question.resize(240);

      const std::string when = detail::Lower(entry.value("handoff_when", "yes"));
      if (when == "yes" || when == "true") {
        monitor.handoffOnYes = true;
      } else if (when == "no" || when == "false") {
        monitor.handoffOnYes = false;
      } else {
        error = "monitor '" + monitor.id + "' handoff_when must be yes or no";
        return false;
      }
      monitor.minConfidence = entry.value("min_confidence", 0.6f);
      monitor.minConfidence = std::clamp(monitor.minConfidence, 0.0f, 1.0f);
      plan.monitors.push_back(std::move(monitor));
    }
  }
  return true;
}

// ── Monitors ─────────────────────────────────────────────────────────────────

// The S1 is asked the local `next` choice plus one Score question per monitor in
// the same request. Score is the protocol's yes/no likelihood; a low confidence
// (near 0.5) is the unknown case that hands back.
inline std::vector<SystemOne::Option> BuildMonitorQuestions(const Plan &plan,
                                                           int maxMonitors) {
  std::vector<SystemOne::Option> questions;
  const std::size_t count = std::min<std::size_t>(
      plan.monitors.size(),
      maxMonitors > 0 ? static_cast<std::size_t>(maxMonitors) : 0);
  for (std::size_t i = 0; i < count; ++i) {
    SystemOne::Option option;
    option.id = "monitor:" + plan.monitors[i].id;
    option.type = SystemOne::OptionType::Score;
    option.instructions = plan.monitors[i].question;
    questions.push_back(std::move(option));
  }
  return questions;
}

inline const SystemOne::Answer *FindAnswer(
    const std::vector<SystemOne::Answer> &answers, const std::string &id) {
  for (const SystemOne::Answer &answer : answers) {
    if (answer.id == id) return &answer;
  }
  return nullptr;
}

// Per-monitor evaluation, for the handoff decision and for the debug view. Both
// use the same confidence rule, so the window cannot disagree with the engine.
struct MonitorReport {
  std::string id;
  float probability = 0.5f;
  float confidence = 0.0f;
  bool triggered = false;
  bool unsure = false;
  bool answered = false;
  bool handoffOnYes = true;
};

inline std::vector<MonitorReport> ReportMonitors(
    const Plan &plan, const std::vector<SystemOne::Answer> &answers,
    float defaultMinConfidence, int maxMonitors) {
  std::vector<MonitorReport> reports;
  const std::size_t count = std::min<std::size_t>(
      plan.monitors.size(),
      maxMonitors > 0 ? static_cast<std::size_t>(maxMonitors) : 0);
  for (std::size_t i = 0; i < count; ++i) {
    const Monitor &monitor = plan.monitors[i];
    MonitorReport report;
    report.id = monitor.id;
    report.handoffOnYes = monitor.handoffOnYes;
    const SystemOne::Answer *answer =
        FindAnswer(answers, "monitor:" + monitor.id);
    if (answer == nullptr) {
      // A missing answer is recorded as unknown, not treated as an event.
      // Treating it as a trigger would block the leg on a protocol miss.
      report.triggered = false;
      report.unsure = true;
      reports.push_back(std::move(report));
      continue;
    }
    report.answered = true;
    report.probability = std::clamp(answer->probability, 0.0f, 1.0f);
    // Score has no separate confidence; a probability near 0.5 is the model
    // saying "I cannot tell", and that is what hands back.
    report.confidence = std::abs(report.probability - 0.5f) * 2.0f;
    const float threshold =
        monitor.minConfidence > 0.0f ? monitor.minConfidence
                                     : defaultMinConfidence;
    report.unsure = report.confidence < threshold;
    const bool yes = report.probability >= 0.5f;
    // Only a confident answer that matches handoff_when is a handoff. An
    // unknown/low answer is recorded for the debug view and the strategist's
    // next context, but it must not block the leg: a small model answers vague
    // yes/no monitors near 0.5 often, and gating on that stopped navigation
    // entirely while the pilot's local choice was confident.
    report.triggered = !report.unsure && (monitor.handoffOnYes ? yes : !yes);
    reports.push_back(std::move(report));
  }
  return reports;
}

inline Handoff EvaluateMonitors(
    const Plan &plan, const std::vector<SystemOne::Answer> &answers,
    float defaultMinConfidence, int maxMonitors) {
  for (const MonitorReport &report :
       ReportMonitors(plan, answers, defaultMinConfidence, maxMonitors)) {
    if (!report.triggered) continue;
    // `triggered` is only set for a confident answer, so the unsure/missing
    // branches are unreachable here; kept as a guard if ReportMonitors changes.
    if (!report.answered) continue;
    if (report.unsure) continue;
    const bool yes = report.probability >= 0.5f;
    return {true, "monitor '" + report.id + "' answered " +
                      std::string(yes ? "yes" : "no") + " (p=" +
                      std::to_string(report.probability) + ")"};
  }
  return {};
}

// ── Observation: local JSON grid ─────────────────────────────────────────────

inline nlohmann::json BuildGrid(
    const SpatialMemory &memory, GamePosition here, const Facing &facing,
    int radius, int maxNodes, int maxEdges) {
  (void)maxNodes;
  (void)maxEdges;
  const int r = std::clamp(radius, 1, 24);
  const int width = 2 * r + 1;
  const int height = 2 * r + 1;
  const GamePosition origin{here.x - r, here.y - r};

  std::vector<std::string> rows(static_cast<std::size_t>(height),
                                std::string(static_cast<std::size_t>(width), '?'));
  std::map<std::string, std::string> legend = {
      {"?", "unknown"}, {".", "open"}, {"#", "blocked"},
      {"~", "water"},   {"@", "you"}};

  // Objects first, so the object code overrides the ground code.
  for (const auto &[id, object] : memory.objects) {
    (void)id;
    if (object.vanishedTick >= 0 || object.portable) continue;
    const int x = object.pos.x - origin.x;
    const int y = object.pos.y - origin.y;
    if (x < 0 || y < 0 || x >= width || y >= height) continue;
    const std::string code = detail::ObjectCode(object);
    rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = code[0];
    if (legend.count(code) == 0) legend[code] = detail::ObjectLabel(object);
  }

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const GamePosition pos{origin.x + x, origin.y + y};
      if (pos == here) {
        rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = '@';
        continue;
      }
      const char current = rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
      if (current != '?') continue;  // an object already marked it
      const auto cell = memory.cells.find(VisionQuery::CellKey(pos));
      if (cell == memory.cells.end()) continue;
      const std::string surface = detail::Lower(cell->second.surface);
      if (!cell->second.walkable) {
        rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = '#';
      } else if (detail::Contains(surface, "water") ||
                 detail::Contains(surface, "lake") ||
                 detail::Contains(surface, "river") ||
                 detail::Contains(surface, "pond")) {
        rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = '~';
      } else {
        rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = '.';
      }
    }
  }

  nlohmann::json grid;
  grid["legend"] = legend;
  grid["origin"] = {origin.x, origin.y};
  grid["scale"] = 1;
  grid["radius"] = r;
  const std::string facingWord = SightReport::FacingWord(facing);
  grid["facing"] = facingWord.empty() ? "unknown" : facingWord;
  nlohmann::json rowArray = nlohmann::json::array();
  for (const std::string &row : rows) rowArray.push_back(row);
  grid["rows"] = std::move(rowArray);
  return grid;
}

// ── Observation: minimal observed place/relation graph ───────────────────────

inline nlohmann::json BuildGraph(
    const SpatialMemory &memory, GamePosition here,
    const std::vector<NavigationPlan::Candidate> &candidates,
    const std::vector<int> &routePlaceIds,
    const std::vector<std::string> &routeLabels,
    const std::unordered_set<int> &visitedPlaceIds,
    const std::unordered_set<std::uint64_t> &walkedCells,
    const std::unordered_set<std::uint64_t> &walkedTargets,
    int radius, int maxNodes, int maxEdges) {
  nlohmann::json graph;
  const KnownPlace *current = PlaceQuery::At(memory, here);

  std::map<std::string, nlohmann::json> nodes;
  std::vector<nlohmann::json> edges;
  std::unordered_set<int> placeIds;

  auto addPlace = [&](const KnownPlace &place) {
    if (maxNodes > 0 && static_cast<int>(nodes.size()) >= maxNodes) return;
    const std::string key = "place:" + std::to_string(place.id);
    if (nodes.count(key) != 0) return;
    placeIds.insert(place.id);
    nlohmann::json node;
    node["key"] = key;
    node["kind"] = "place";
    node["category"] = place.primaryAspect.empty() ? "place" : place.primaryAspect;
    node["regionLevel"] = place.regionLevel;
    node["aspects"] = detail::AspectsJson(place, 3);
    // A stable position: the region uses its grid-cell centre, which does not
    // move as the observed extent grows; a semantic place uses its extent
    // centre. The top-left `anchor` was the old source of the jump between calls.
    const GamePosition graphPosition =
        (place.regionLevel >= 0 && place.regionSize > 0)
            ? GamePosition{place.regionCell.x + place.regionSize / 2,
                           place.regionCell.y + place.regionSize / 2}
            : GamePosition{place.anchor.x + place.width / 2,
                           place.anchor.y + place.height / 2};
    node["anchor"] = {graphPosition.x, graphPosition.y};
    node["label"] = PlaceQuery::DisplayName(place);
    node["visited"] = visitedPlaceIds.count(place.id) != 0 ||
                      detail::PlaceWalked(place, walkedCells);
    nodes[key] = std::move(node);
  };

  if (current != nullptr) addPlace(*current);
  // The semantic place underfoot wins the current key, but the base region is
  // still the ground the character is standing on. Keep it in the graph so the
  // region's stable cell centre is visible even when a village/road is current.
  const KnownPlace *currentRegion = PlaceQuery::RegionAt(memory, here);
  if (currentRegion != nullptr && currentRegion != current) {
    addPlace(*currentRegion);
  }
  for (const NavigationPlan::Candidate &candidate : candidates) {
    if (const KnownPlace *place = PlaceQuery::ById(memory, candidate.placeId)) {
      addPlace(*place);
    }
  }
  for (const int id : routePlaceIds) {
    if (const KnownPlace *place = PlaceQuery::ById(memory, id)) addPlace(*place);
  }

  auto addEdge = [&](const std::string &from, const std::string &to,
                     const std::string &rel, const std::string &direction,
                     int distance, const std::string &source) {
    if (maxEdges > 0 && static_cast<int>(edges.size()) >= maxEdges) return;
    nlohmann::json edge;
    edge["from"] = from;
    edge["to"] = to;
    edge["rel"] = rel;
    edge["direction"] = direction;
    edge["distance"] = distance;
    edge["source"] = source;
    edges.push_back(std::move(edge));
  };

  // Objects/landmarks near the agent are nodes; they are what the strategist's
  // "is there a signpost/building/lake" questions are about.
  for (const auto &[id, object] : memory.objects) {
    if (object.vanishedTick >= 0 || object.portable) continue;
    if (detail::Chebyshev(object.pos, here) > radius) continue;
    if (maxNodes > 0 && static_cast<int>(nodes.size()) >= maxNodes) break;
    const std::string key = "obj:" + std::to_string(id);
    if (nodes.count(key) != 0) continue;
    nlohmann::json node;
    node["key"] = key;
    node["kind"] = "object";
    node["category"] = object.templateId.empty() ? object.name : object.templateId;
    node["name"] = detail::ObjectLabel(object);
    node["cell"] = {object.pos.x, object.pos.y};
    node["structural"] = object.structural;
    node["character"] = object.character;
    nodes[key] = std::move(node);
    if (current != nullptr) {
      const std::string from = "place:" + std::to_string(current->id);
      addEdge(from, key, "visible-from",
              detail::DirectionPhrase(here, object.pos),
              detail::Chebyshev(here, object.pos), "observed");
    }
  }

  const std::string currentKey =
      current != nullptr ? "place:" + std::to_string(current->id) : "";

  // Candidate edges: the current place to every local/distant place.
  std::unordered_set<std::string> seenEdges;
  for (const NavigationPlan::Candidate &candidate : candidates) {
    if (current == nullptr) break;
    const KnownPlace *place = PlaceQuery::ById(memory, candidate.placeId);
    if (place == nullptr) continue;
    if (place->id == current->id) continue;
    const std::string to = "place:" + std::to_string(place->id);
    const std::string from = currentKey;
    const std::string direction =
        candidate.key.rfind("here:", 0) == 0
            ? (candidate.key.size() > 5 ? candidate.key.substr(5) : "")
            : PlaceQuery::CompassWord(candidate.target.x - here.x,
                                      candidate.target.y - here.y);
    const std::string edgeKey = from + ">" + to + ">" + direction;
    if (!seenEdges.insert(edgeKey).second) continue;
    addEdge(from, to, "adjacent", direction, candidate.distance, "observed");
  }

  // The walked route, consecutive places, is the traversed part of the graph.
  for (std::size_t i = 1; i < routePlaceIds.size(); ++i) {
    if (routePlaceIds[i - 1] == routePlaceIds[i]) continue;
    const KnownPlace *from = PlaceQuery::ById(memory, routePlaceIds[i - 1]);
    const KnownPlace *to = PlaceQuery::ById(memory, routePlaceIds[i]);
    if (from == nullptr || to == nullptr) continue;
    addEdge("place:" + std::to_string(from->id),
            "place:" + std::to_string(to->id), "traversed", "",
            detail::Chebyshev(from->anchor, to->anchor), "walked");
  }

  // The frontier is what the compass offers that has not been visited. A place
  // already walked is not a frontier even if it is still an action.
  nlohmann::json frontier = nlohmann::json::array();
  std::unordered_set<std::string> frontierKeys;
  for (const NavigationPlan::Candidate &candidate : candidates) {
    const bool local = candidate.key.rfind("here:", 0) == 0;
    if (!local && candidate.key.rfind("p", 0) != 0) continue;
    // A local run is an open direction inside the current place, not a new
    // place: it is a frontier even when the current place itself is visited.
    if (!local && visitedPlaceIds.count(candidate.placeId) != 0) continue;
    if (walkedTargets.count(VisionQuery::CellKey(candidate.target)) != 0) {
      continue;
    }
    if (!frontierKeys.insert(candidate.key).second) continue;
    nlohmann::json entry;
    entry["key"] = candidate.key;
    entry["kind"] = local ? "open" : "place";
    entry["place"] = "place:" + std::to_string(candidate.placeId);
    entry["label"] = candidate.label;
    entry["description"] = candidate.description;
    entry["distance"] = candidate.distance;
    entry["direction"] = local
                             ? (candidate.key.size() > 5 ? candidate.key.substr(5)
                                                         : std::string())
                             : PlaceQuery::CompassWord(candidate.target.x - here.x,
                                                       candidate.target.y - here.y);
    frontier.push_back(std::move(entry));
  }

  // A dead end is the current place with no current-place continuation in any
  // sector. That is the same condition the compass reports as no here: run.
  bool hasCurrentRun = false;
  for (const NavigationPlan::Candidate &candidate : candidates) {
    if (candidate.key.rfind("here:", 0) == 0) {
      hasCurrentRun = true;
      break;
    }
  }
  nlohmann::json deadEnds = nlohmann::json::array();
  if (current != nullptr && !hasCurrentRun && !candidates.empty()) {
    nlohmann::json entry;
    entry["key"] = currentKey;
    entry["reason"] = "no continuation observed";
    deadEnds.push_back(std::move(entry));
  }

  nlohmann::json nodeArray = nlohmann::json::array();
  for (auto &[key, node] : nodes) nodeArray.push_back(std::move(node));
  graph["current"] = currentKey;
  graph["nodes"] = std::move(nodeArray);
  graph["edges"] = std::move(edges);
  graph["frontier"] = std::move(frontier);
  graph["route"] = routeLabels;
  graph["dead_ends"] = std::move(deadEnds);
  return graph;
}

inline nlohmann::json BuildObservation(
    const SpatialMemory &memory, GamePosition here, const Facing &facing,
    const std::vector<NavigationPlan::Candidate> &candidates,
    const std::vector<int> &routePlaceIds,
    const std::vector<std::string> &routeLabels,
    const std::unordered_set<int> &visitedPlaceIds,
    const std::unordered_set<std::uint64_t> &walkedCells,
    const std::unordered_set<std::uint64_t> &walkedTargets, int radius,
    int maxNodes, int maxEdges) {
  nlohmann::json observation;
  observation["grid"] =
      BuildGrid(memory, here, facing, radius, maxNodes, maxEdges);
  observation["graph"] = BuildGraph(
      memory, here, candidates, routePlaceIds, routeLabels, visitedPlaceIds,
      walkedCells, walkedTargets, radius, maxNodes, maxEdges);
  return observation;
}

// Stage 2's grouped object summary replaces per-object graph nodes/edges. The
// graph still carries the place structure; keeping the object copy as well was
// pure prompt bloat (18 nodes + 18 edges in the first village test) and made the
// small navigator ramble. Only the single navigator calls this.
inline void StripObjectGraph(nlohmann::json &observation) {
  const auto graphIt = observation.find("graph");
  if (graphIt == observation.end() || !graphIt->is_object()) return;
  nlohmann::json &graph = *graphIt;

  const auto isObjectKey = [](const nlohmann::json &value) {
    return value.is_string() &&
           value.get_ref<const std::string &>().rfind("obj:", 0) == 0;
  };
  const auto stripArray = [&](const char *field, auto keep) {
    const auto it = graph.find(field);
    if (it == graph.end() || !it->is_array()) return;
    nlohmann::json kept = nlohmann::json::array();
    for (const nlohmann::json &entry : *it) {
      if (keep(entry)) kept.push_back(entry);
    }
    *it = std::move(kept);
  };

  stripArray("nodes", [&](const nlohmann::json &node) {
    if (!node.is_object()) return true;
    const auto key = node.find("key");
    return key == node.end() || !isObjectKey(*key);
  });
  stripArray("edges", [&](const nlohmann::json &edge) {
    if (!edge.is_object()) return true;
    const auto to = edge.find("to");
    const auto from = edge.find("from");
    if (to != edge.end() && isObjectKey(*to)) return false;
    if (from != edge.end() && isObjectKey(*from)) return false;
    return true;
  });
  stripArray("frontier", [&](const nlohmann::json &entry) {
    if (!entry.is_object()) return true;
    const auto key = entry.find("key");
    return key == entry.end() || !isObjectKey(*key);
  });
}

// ── The S1 pilot's compact observation ───────────────────────────────────────

// A small agent-centred ASCII map for the S1. The S1 is a classifier and a full
// 17x17 strategist grid is too much; this is the 13x13 experiment: terrain,
// objects, semantic-place initials, candidate target digits and the @. The
// target list below the map is the action vocabulary; the map alone is context.
inline std::string BuildPilotMap(
    const SpatialMemory &memory, GamePosition here, const Facing &facing,
    const std::vector<NavigationPlan::Candidate> &candidates, int radius) {
  const int r = std::clamp(radius, 3, 12);
  const int width = 2 * r + 1;
  const int height = width;
  const GamePosition origin{here.x - r, here.y - r};
  std::vector<std::string> rows(static_cast<std::size_t>(height),
                                std::string(static_cast<std::size_t>(width), '?'));
  const auto inside = [width, height](int x, int y) {
    return x >= 0 && y >= 0 && x < width && y < height;
  };

  for (const auto &[key, cell] : memory.cells) {
    (void)key;
    const int x = cell.pos.x - origin.x;
    const int y = cell.pos.y - origin.y;
    if (!inside(x, y)) continue;
    char symbol = '.';
    if (!cell.walkable) {
      symbol = '#';
    } else {
      const std::string surface = detail::Lower(cell.surface);
      if (detail::Contains(surface, "water") ||
          detail::Contains(surface, "lake") ||
          detail::Contains(surface, "river") ||
          detail::Contains(surface, "pond")) {
        symbol = '~';
      }
    }
    rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = symbol;
  }

  for (const auto &[id, object] : memory.objects) {
    (void)id;
    if (object.vanishedTick >= 0 || object.portable) continue;
    const int x = object.pos.x - origin.x;
    const int y = object.pos.y - origin.y;
    if (!inside(x, y)) continue;
    const std::string code = detail::ObjectCode(object);
    if (!code.empty()) {
      rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = code[0];
    }
  }

  // Semantic-place initials fill open ground only: an object code stays visible.
  for (const KnownPlace &place : memory.places) {
    if (place.origin == PlaceOrigin::Region) continue;
    std::string name = !place.labels.empty() ? place.labels.front()
                                              : place.primaryAspect;
    if (name.empty()) continue;
    char symbol = static_cast<char>(
        std::toupper(static_cast<unsigned char>(name.front())));
    if (symbol < 'A' || symbol > 'Z') symbol = 'P';
    for (const std::uint64_t key : place.memberCells) {
      const GamePosition pos = VisionQuery::CellFromKey(key);
      const int x = pos.x - origin.x;
      const int y = pos.y - origin.y;
      if (!inside(x, y)) continue;
      char &cell = rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
      if (cell == '.' || cell == '~' || cell == '#') cell = symbol;
    }
  }

  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const NavigationPlan::Candidate &candidate = candidates[i];
    const int x = candidate.target.x - origin.x;
    const int y = candidate.target.y - origin.y;
    if (!inside(x, y)) continue;
    rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
        static_cast<char>('1' + static_cast<int>(i % 9));
  }

  const int hereX = here.x - origin.x;
  const int hereY = here.y - origin.y;
  if (inside(hereX, hereY)) {
    rows[static_cast<std::size_t>(hereY)][static_cast<std::size_t>(hereX)] = '@';
  }

  std::ostringstream out;
  out << "MAP " << width << "x" << height
      << " north-up (@=you .=open #=blocked ~=water T=tree B=building S=sign "
         "C=character O=other ?=unknown; letters=semantic place initials; "
         "digits=targets)";
  const std::string facingWord = SightReport::FacingWord(facing);
  if (!facingWord.empty()) out << " facing " << facingWord;
  out << ":";
  for (const std::string &row : rows) {
    out << "\n  " << row;
  }
  out << "\nMAP TARGETS:";
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const NavigationPlan::Candidate &candidate = candidates[i];
    out << "\n  " << (i + 1) << "  " << candidate.key << "  "
        << candidate.label << " (" << candidate.distance << " tiles)";
  }
  return out.str();
}

inline std::string BuildPilotObservation(
    const SpatialMemory &memory, GamePosition here, const Facing &facing,
    const std::vector<NavigationPlan::Candidate> &candidates,
    const std::vector<std::string> &routeLabels,
    const std::unordered_set<int> &visitedPlaceIds,
    const std::unordered_set<std::uint64_t> &walkedTargets, int radius,
    const std::string &mapMode = "off", int mapRadius = 6) {
  const KnownPlace *current = PlaceQuery::At(memory, here);
  const int r = std::clamp(radius, 1, 24);
  const std::string facingWord = SightReport::FacingWord(facing);

  const std::string mode =
      (mapMode == "append" || mapMode == "replace") ? mapMode : "off";
  const std::string map = mode == "off"
                              ? std::string()
                              : BuildPilotMap(memory, here, facing,
                                              candidates, mapRadius);
  if (mode == "replace") {
    std::ostringstream mapOut;
    mapOut << map << "\nROUTE:";
    if (routeLabels.empty()) {
      mapOut << " nothing walked yet";
    } else {
      for (std::size_t i = 0; i < routeLabels.size(); ++i) {
        if (i > 0) mapOut << " -> ";
        mapOut << routeLabels[i];
      }
    }
    bool hasCurrentRun = false;
    for (const NavigationPlan::Candidate &candidate : candidates) {
      if (candidate.key.rfind("here:", 0) == 0) {
        hasCurrentRun = true;
        break;
      }
    }
    if (current != nullptr && !hasCurrentRun && !candidates.empty()) {
      mapOut << "\nDEAD END: the place underfoot has no observed "
                "continuation; only other places are reachable.";
    }
    return mapOut.str();
  }

  std::ostringstream out;
  out << "SURROUNDINGS:";
  if (current != nullptr) {
    out << "\n  here: " << detail::PlaceLabel(*current);
  } else {
    out << "\n  here: open ground, not in any place you know";
  }
  if (!facingWord.empty()) out << "\n  facing: " << facingWord;

  static const int dx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
  static const int dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
  for (int i = 0; i < 8; ++i) {
    const std::string direction = PlaceQuery::CompassWord(dx[i], dy[i]);
    GamePosition best{0, 0};
    std::string label;
    int bestDistance = r + 1;
    bool bestWater = false;

    for (const KnownPlace *place : PlaceQuery::ModelPlaces(memory, here)) {
      const GamePosition cell =
          PlaceQuery::NearestWalkableCell(memory, *place, here);
      if (cell == here) continue;
      if (cell.x - here.x != 0 && (cell.x - here.x) * dx[i] < 0) continue;
      if (cell.y - here.y != 0 && (cell.y - here.y) * dy[i] < 0) continue;
      if (dx[i] == 0 && cell.x != here.x) continue;
      if (dy[i] == 0 && cell.y != here.y) continue;
      const int distance = detail::Chebyshev(here, cell);
      if (distance > r || distance >= bestDistance) continue;
      bestDistance = distance;
      best = cell;
      label = detail::PlaceLabel(*place) + ", " + std::to_string(distance) +
              " tiles";
      bestWater = false;
    }

    for (const auto &[id, object] : memory.objects) {
      if (object.vanishedTick >= 0 || object.portable) continue;
      const int ox = object.pos.x - here.x;
      const int oy = object.pos.y - here.y;
      if (ox != 0 && ox * dx[i] < 0) continue;
      if (oy != 0 && oy * dy[i] < 0) continue;
      if (dx[i] == 0 && ox != 0) continue;
      if (dy[i] == 0 && oy != 0) continue;
      const int distance = detail::Chebyshev(here, object.pos);
      if (distance > r || distance >= bestDistance) continue;
      bestDistance = distance;
      best = object.pos;
      label = detail::ObjectLabel(object) + ", " + std::to_string(distance) +
              " tiles";
      bestWater = false;
    }

    for (const auto &[key, cell] : memory.cells) {
      (void)key;
      const int cx = cell.pos.x - here.x;
      const int cy = cell.pos.y - here.y;
      if (cx != 0 && cx * dx[i] < 0) continue;
      if (cy != 0 && cy * dy[i] < 0) continue;
      if (dx[i] == 0 && cx != 0) continue;
      if (dy[i] == 0 && cy != 0) continue;
      const std::string surface = detail::Lower(cell.surface);
      if (!detail::Contains(surface, "water") &&
          !detail::Contains(surface, "lake") &&
          !detail::Contains(surface, "river") &&
          !detail::Contains(surface, "pond")) {
        continue;
      }
      const int distance = detail::Chebyshev(here, cell.pos);
      if (distance > r || distance >= bestDistance) continue;
      bestDistance = distance;
      best = cell.pos;
      label = "water, " + std::to_string(distance) + " tiles";
      bestWater = true;
    }

    if (!label.empty()) {
      out << "\n  " << direction << ": " << label;
      if (bestWater) out << " (water)";
      (void)best;
    }
  }

  out << "\nROUTE:";
  if (routeLabels.empty()) {
    out << " nothing walked yet";
  } else {
    for (std::size_t i = 0; i < routeLabels.size(); ++i) {
      if (i > 0) out << " -> ";
      out << routeLabels[i];
    }
  }

  std::ostringstream frontier;
  bool first = true;
  for (const NavigationPlan::Candidate &candidate : candidates) {
    const bool local = candidate.key.rfind("here:", 0) == 0;
    if (!local && candidate.key.rfind("p", 0) != 0) continue;
    if (!local && visitedPlaceIds.count(candidate.placeId) != 0) continue;
    if (walkedTargets.count(VisionQuery::CellKey(candidate.target)) != 0) {
      continue;
    }
    if (!first) frontier << ", ";
    first = false;
    frontier << candidate.key << " " << candidate.label << " ("
             << candidate.distance << " tiles)";
  }
  out << "\nFRONTIER: " << (first ? "none" : frontier.str());

  bool hasCurrentRun = false;
  for (const NavigationPlan::Candidate &candidate : candidates) {
    if (candidate.key.rfind("here:", 0) == 0) {
      hasCurrentRun = true;
      break;
    }
  }
  if (current != nullptr && !hasCurrentRun && !candidates.empty()) {
    out << "\nDEAD END: the place underfoot has no observed continuation; "
           "only other places are reachable.";
  }
  if (mode == "append") {
    out << "\n" << map;
  }
  return out.str();
}

// ── Strategist prompt ────────────────────────────────────────────────────────

inline std::string BuildStrategistSystemPrompt() {
  return R"(You are the navigation strategist for an NPC in a tile-based game.
You see only what the character has observed: a local grid and a graph of
observed places/objects. You have no map, no authored place names you have not
seen, and no coordinates the character has not observed. The character is a
small local pilot (System 1) that will execute the next moves and answer your
monitor questions from this observation.

Before the plan JSON, write a brief <think>...</think> block (2-5 sentences)
explaining what you observed and why this strategy. The engine ignores it for
planning; the debugger shows it. Do not put anything else outside the think
block and the JSON.

Your job is the long-term strategy: what to do for the next several moves, and
what should make the character stop and ask you again.

After the think block, reply with ONE JSON object and no other prose:
{
  "strategy": "a few sentences the main character could follow",
  "subtask": "a short instruction for the pilot, e.g. follow the road east",
  "monitors": [
    {
      "id": "short_id",
      "question": "a yes/no question the pilot can answer from the observation",
      "handoff_when": "yes",
      "min_confidence": 0.6
    }
  ]
}

Rules:
- 1 to 3 monitors. Each is a yes/no question grounded in the observation.
- handoff_when is "yes" or "no". min_confidence is 0..1: the monitor only
  triggers a handoff when the pilot's answer is confident (2*|p-0.5| at least
  that value) and matches handoff_when. An unknown/low answer is recorded but
  does not hand back and must not block the pilot's move; the pilot's own
  uncertainty or no-move is what asks you again. Do not rely on a vague monitor
  to stop the pilot.
- The pilot's answer is evidence, not truth; prefer specific, observable
  questions (a signpost, a junction, a dead end, water, a building) over vague
  ones.
- The strategy must be reachable from what is observed. Do not plan through
  unknown cells as if they were open; the engine validates movement.
- If the observation cannot support the goal, reply:
  {"give_up": true, "reason": "..."}.
- There is no fixed step budget inside the strategy. Keep the plan alive while it
  is working; a monitor or the pilot's uncertainty is what brings control back.
- The goal/task text is the only context from the main character. It may contain
  what the character learned from a sign or a person. Keep it in mind, but do not
  invent places that are not in the observation.
)";
}

inline std::string PlanToJson(const Plan &plan, bool pretty = true) {
  nlohmann::json root;
  root["strategy"] = plan.strategy;
  root["subtask"] = plan.subtask;
  if (!plan.watch.empty()) root["watch"] = plan.watch;
  if (!plan.monitors.empty()) {
    root["monitors"] = nlohmann::json::array();
    for (const Monitor &monitor : plan.monitors) {
      nlohmann::json entry;
      entry["id"] = monitor.id;
      entry["question"] = monitor.question;
      entry["handoff_when"] = monitor.handoffOnYes ? "yes" : "no";
      entry["min_confidence"] = monitor.minConfidence;
      root["monitors"].push_back(std::move(entry));
    }
  }
  return pretty ? root.dump(2) : root.dump();
}

inline std::string BuildStrategistUserPrompt(
    const std::string &goal, const std::string &taskDigest,
    const nlohmann::json &observation, const Plan *previous,
    const std::string &handoffReason, int strategiesUsed, int maxStrategies) {
  std::ostringstream out;
  out << "GOAL: " << goal << "\n";
  if (!taskDigest.empty()) {
    out << "\nTASK CONTEXT:\n" << taskDigest << "\n";
  }
  if (previous != nullptr) {
    out << "\nPREVIOUS STRATEGY:\n" << PlanToJson(*previous) << "\n";
    out << "\nHANDOFF REASON: " << handoffReason << "\n";
  }
  out << "\nSTRATEGIES USED: " << strategiesUsed << " / " << maxStrategies
      << "\n";
  out << "\nOBSERVATION (observed only):\n" << observation.dump(2) << "\n";
  out << "\nReturn the JSON object now.\n";
  return out.str();
}

// ── Single-context navigator prompts ─────────────────────────────────────────

// One main-model context holds the whole navigation: the map, graph, plan, route
// and previous results. This prompt is intentionally richer than the pilot's
// one-step prompt: the model is the strategist and the pilot at once.
inline std::string BuildSingleSystemPrompt(bool concise = false,
                                            bool includeGraph = true,
                                            bool includeMap = true) {
  std::string prompt = R"(You are the navigator for a character in a tile-based game. You are the
only navigation model: there is no separate strategist or pilot. You see only
what the character has observed in SpatialMemory; you have no map, no authored
name you have not seen, and no coordinates the character has not observed.

Each turn the engine gives you:
- OBSERVATION: the observed JSON grid and minimal place/object graph.
- MAP: a north-up ASCII map; @ is you, digits are candidate target cells,
  letters are semantic-place initials; the legend is on the MAP line.
- STATE: where you are, the goal, the route and the candidate actions.
- PLAN: your previous plan (or none).
- JOURNEY/ROUTE, AREA, GATE/NOTED and LAST RESULT: what the character has
  walked, the current area the engine knows, anything the mid-walk attention
  gate decided you should see, and what just happened.

Your job is one decision: keep or update the plan, and choose the next move.
Think about the long-term route and the local geometry, not just the next tile.

Two ways to move:
- An ACTIONS key (here:<dir>, p<id>) is one leg. A here:<dir> on a linear place
  is a single compass run (often six tiles), not a commitment to the feature.
- PRIMITIVES are commitments: follow / follow:<direction> follows the linear
  place underfoot (road, river, wall) through forced turns to the next fork,
  junction or end; back retraces the last leg to where it started; to:<n> walks
  to MAP TARGETS number n.
If the ACTIONS list keeps offering the same linear feature (for example
here:east "walk an unnamed road east for 6 tiles" again and again), use
follow:east. A follow is one decision, but it does not skip choices: it stops at
the next fork, junction or end and hands back.

Reply with a short <think>...</think> block (2-6 sentences, and never more
than 30 words), then ONE JSON object. The JSON must be after the closing
</think> tag, not inside it, and must not be wrapped in a code fence:
{
  "plan": {
    "strategy": "a few sentences the main character could follow",
    "subtask": "a short instruction for the next few moves",
    "watch": "what you want to be interrupted for while moving; a priority, not a filter"
  },
  "move": "here:east",
  "confidence": 0.7,
  "done": false,
  "unsure": false,
  "reason": "one short sentence"
}

Rules:
- `move` must be one of the action keys offered for this decision, one of the
  primitives below, one of the object-target forms in STATE, or `arrived`,
  `none`, `unsure` or `look_around`. Never invent a coordinate or a place name.
- Primitives (not in the action list; the engine resolves them):
  `follow` / `follow:<direction>` follows the linear place underfoot (road,
  river, wall) through forced turns and stops at the next fork, junction with
  another linear feature, or end; without a direction it uses your facing.
  `back` retraces the last leg to where it started. `to:<n>` walks to MAP
  TARGETS number n; `to:<key>` does the same by candidate key.
- Object targets: when STATE contains OBJECT TARGETS, `objects:all` expands the
  object groups without moving, `inspect:<group>` (where <group> is a `key=`
  value from OBJECTS SEEN) lists instance aliases,
  `approach:<alias>` walks to one instance, and `<verb>:<alias>` walks and uses a
  no-argument verb such as `examine` or `harvest`. Aliases come only from an
  inspect result; never use raw entity ids. The legacy EXAMINE section, when
  present, lists examine:<id> keys instead.
- `inspect` and `objects:all` do not move. A timed verb starts its action and
  hands control back to the main agent; do not wait for it.
- The JSON has exactly these top-level keys: plan, move, confidence, done,
  unsure, reason. Do not use action, verb, alias or think at the top level.
  `move` is always a string, never an object.
- Place keys such as `p17` and `here:north` go directly in `move`. Object
  actions are strings such as `inspect:apple_tree`, `approach:apple_tree-1` or
  `examine:apple_tree-1`. Only type-number aliases returned by inspect are
  object aliases; `p17` is not one.
- Start with `<think>` and end with `</think>`; do not write a `THINK:` heading
  and do not put a `think` field inside the JSON. Keep the think to at most two
  sentences.
- `arrived` is only for when the goal is actually satisfied; the engine verifies
  the current place. `done` is an alias for arrived.
- `unsure` or `none` hands control back; do not use them to avoid a decision when
  a usable move exists. When you choose a concrete move (`arrived`, `look_around`
  or an action key), set `unsure` to false.
- `look_around` asks for a full-circle survey; use it only when the map is too
  thin to choose.
- The plan is yours to keep or replace. If the map changed enough to invalidate
  it, update it in this response.
- GATE means the attention gate stopped you mid-walk because something new was
  worth your attention. React to it; you may continue the old plan.
- NOTED means something new was seen while walking but did not stop you.
  Consider it; it may be behind you now.
- `watch` is your mid-walk attention brief: say what should make you stop while
  moving. It is a priority, not a filter; the gate also stops for anything a
  person would point out or that would change the plan, even if it is not in the
  watch. It may be empty; an empty watch means no mid-walk gate.
- The engine resolves the chosen key to a target cell and validates the walk.
- Do not plan through unknown cells as if they were open.
- Reply concisely. The JSON is authoritative; no prose outside the think block
  and the JSON.
)";
  if (!includeGraph) {
    detail::ReplaceAll(
        prompt, "the observed JSON grid and minimal place/object graph.",
        "the observed JSON grid.");
  }
  if (!includeMap) {
    detail::ReplaceAll(
        prompt,
        "- MAP: a north-up ASCII map; @ is you, digits are candidate target cells,\n"
        "  letters are semantic-place initials; the legend is on the MAP line.\n",
        "");
    detail::ReplaceAll(prompt, "the map is too", "the state is too");
    detail::ReplaceAll(prompt, "If the map changed", "If the state changed");
  }
  if (concise) {
    detail::ReplaceAll(prompt,
                       "short <think>...</think> block (2-6 sentences)",
                       "short <think>...</think> block (1-2 sentences)");
    detail::ReplaceAll(
        prompt,
        "\"strategy\": \"a few sentences the main character could follow\"",
        "\"strategy\": \"one sentence the main character could follow\"");
    detail::ReplaceAll(
        prompt, "\"subtask\": \"a short instruction for the next few moves\"",
        "\"subtask\": \"a few words for the next move\"");
  }
  return prompt;
}

inline std::string BuildSingleUserPrompt(
    const std::string &goal, const std::string &taskDigest,
    const nlohmann::json &observation, const std::string &map,
    const std::string &state, const Plan *plan,
    const std::vector<std::string> &route, const std::string &lastResult,
    int calls, int maxCalls, bool compactJson = true,
    bool includeMap = true, const std::string &journey = "",
    const std::string &areaCard = "", const std::string &gate = "",
    const std::string &noted = "") {
  std::ostringstream out;
  out << "GOAL: " << goal << "\n";
  if (!taskDigest.empty()) {
    out << "\nTASK CONTEXT:\n" << taskDigest << "\n";
  }
  out << "\nOBSERVATION (observed only):\n"
      << (compactJson ? observation.dump() : observation.dump(2)) << "\n";
  if (includeMap) out << "\n" << map << "\n";
  out << "\n" << state << "\n";
  out << "\nPRIMITIVES (commitments; use these when an ACTIONS line repeats the "
         "same feature):\n"
         "  follow / follow:<direction>   follow the linear place underfoot "
         "(road, river, wall) through forced turns to the next fork, junction "
         "or end; follow uses your facing\n"
         "  back                          retrace the last leg to where it "
         "started\n"
         "  to:<n>                        walk to MAP TARGETS number n; "
         "to:<key> also works\n"
         "  A here:<dir> ACTIONS line is one compass run, not the follow "
         "primitive.\n";
  out << "\nPREVIOUS PLAN:\n";
  if (plan != nullptr) {
    out << PlanToJson(*plan, !compactJson) << "\n";
  } else {
    out << "none\n";
  }
  if (!journey.empty()) {
    out << "\nJOURNEY: " << journey << "\n";
  } else {
    out << "\nROUTE:";
    if (route.empty()) {
      out << " nothing walked yet";
    } else {
      for (std::size_t i = 0; i < route.size(); ++i) {
        if (i > 0) out << " -> ";
        out << route[i];
      }
    }
    out << "\n";
  }
  if (!areaCard.empty()) {
    out << "\n" << areaCard;
  }
  if (!gate.empty()) {
    out << "\nGATE:\n" << gate << "\n";
  }
  if (!noted.empty()) {
    out << "\nNOTED:\n" << noted << "\n";
  }
  out << "\nLAST RESULT: " << (lastResult.empty() ? "episode start" : lastResult)
      << "\n";
  out << "DECISION: " << (calls + 1) << " / " << maxCalls << "\n";
  out << "MOVE: an ACTIONS key or a primitive (follow / follow:<direction> / "
         "back / to:<n>).\n";
  out << "\nJSON shape: {\"plan\":{\"strategy\":\"...\","
         "\"subtask\":\"...\",\"watch\":\"...\"},\"move\":\"p17\","
         "\"confidence\":0.5,\"done\":false,\"unsure\":false,"
         "\"reason\":\"...\"}. Only plan/move/confidence/done/unsure/reason; "
         "`move` is a string. Object moves: \"inspect:apple_tree\", "
         "\"examine:apple_tree-1\". Place keys like p17 go directly in move. "
         "Primitives go directly in move too: \"follow:east\", \"back\", "
         "\"to:3\".\n";
  out << "\nReturn the think block and JSON now.\n";
  return out.str();
}

// ── Continuous navigation's wake block (navigation.agent = "inline") ────────
//
// The no-episode experiment: the main agent's ordinary turn carries the same
// facts the single navigator's STATE/ACTIONS block carries, but the choice
// belongs to the ordinary command loop rather than to a delegated decision.
// There is no goal to state, so the block answers only "where am I and what
// steps are offered"; the character's intention stays in its task list and its
// conversation, which is the point of putting the spatial state here at all --
// the agent that walks is the agent that remembers the walk.
//
// The first line is a marker. AgentBrain prunes old copies of the block by
// prefix (`navigation.inline.spatialHistory`), so the two spellings must not
// drift; they share this constant.
inline constexpr const char *SPATIAL_BLOCK_MARKER = "SPATIAL: where you are";

inline std::string BuildSpatialBlock(
    const SpatialMemory &memory, GamePosition here, const Facing &facing,
    const std::vector<NavigationPlan::Candidate> &candidates, bool canFollow,
    bool canBack, const std::string &watch, bool includeActions,
    bool includeMap, int mapRadius) {
  std::ostringstream out;
  out << SPATIAL_BLOCK_MARKER << "\n";
  out << "YOU ARE: " << NavigationPlan::DescribeWhere(memory, here, facing)
      << ".\n";

  if (includeActions) {
    out << "STEPS (one leg each; your other commands still work, and you do "
           "not have to move -- type the whole [GO ...] form):\n";
    bool any = false;
    for (const NavigationPlan::Candidate &candidate : candidates) {
      if (candidate.examine) continue;
      out << "  [GO " << candidate.key << "]  " << candidate.description
          << "\n";
      any = true;
    }
    if (canFollow) {
      out << "  [GO follow]  follow the linear feature underfoot to the next "
             "junction or end; add :<direction> to choose the way\n";
    }
    if (canBack) {
      out << "  [GO back]  retrace the step you just walked\n";
    }
    if (!any && !canFollow && !canBack) {
      out << "  (no step is offered from here; [MOVE_TO] still works for a "
             "place or person you can name)\n";
    }
  }

  out << "WATCH: ";
  if (watch.empty()) {
    out << "none. Set one with [WATCH <what should make you stop and look "
           "while you walk>].\n";
  } else {
    out << watch << "\n";
  }

  if (includeMap) {
    out << "\n" << BuildPilotMap(memory, here, facing, candidates, mapRadius)
        << "\n";
  }
  return out.str();
}

inline std::string BuildPlanSummary(const Plan &plan) {
  std::ostringstream out;
  out << "strategy: " << plan.strategy;
  if (!plan.subtask.empty()) out << " | subtask: " << plan.subtask;
  if (!plan.watch.empty()) out << " | watch: " << plan.watch;
  out << " | monitors:";
  if (plan.monitors.empty()) {
    out << " none";
  } else {
    for (const Monitor &monitor : plan.monitors) {
      out << " " << monitor.id << "[handoff "
          << (monitor.handoffOnYes ? "yes" : "no") << ", min_conf "
          << monitor.minConfidence << "]";
    }
  }
  return out.str();
}

inline std::string BuildHandoffSummary(const Handoff &handoff) {
  return handoff.triggered ? handoff.reason : std::string();
}

}  // namespace NavigationStrategist
