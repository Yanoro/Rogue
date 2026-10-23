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
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return std::string();
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
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

  const std::size_t open = text.find('{');
  const std::size_t close = text.rfind('}');
  if (open == std::string::npos || close == std::string::npos || close <= open) {
    return std::string();
  }
  return text.substr(open, close - open + 1);
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
    node["aspects"] = detail::AspectsJson(place, 3);
    node["anchor"] = {place.anchor.x, place.anchor.y};
    node["label"] = PlaceQuery::DisplayName(place);
    node["visited"] = visitedPlaceIds.count(place.id) != 0 ||
                      detail::PlaceWalked(place, walkedCells);
    nodes[key] = std::move(node);
  };

  if (current != nullptr) addPlace(*current);
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
    if (candidate.key.rfind("here:", 0) == 0 &&
        candidate.placeId == (current != nullptr ? current->id : -1)) {
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

// ── The S1 pilot's compact observation ───────────────────────────────────────

inline std::string BuildPilotObservation(
    const SpatialMemory &memory, GamePosition here, const Facing &facing,
    const std::vector<NavigationPlan::Candidate> &candidates,
    const std::vector<std::string> &routeLabels,
    const std::unordered_set<int> &visitedPlaceIds,
    const std::unordered_set<std::uint64_t> &walkedTargets, int radius) {
  const KnownPlace *current = PlaceQuery::At(memory, here);
  const int r = std::clamp(radius, 1, 24);
  const std::string facingWord = SightReport::FacingWord(facing);

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

    for (const KnownPlace &place : memory.places) {
      const GamePosition cell =
          PlaceQuery::NearestWalkableCell(memory, place, here);
      if (cell == here) continue;
      if (cell.x - here.x != 0 && (cell.x - here.x) * dx[i] < 0) continue;
      if (cell.y - here.y != 0 && (cell.y - here.y) * dy[i] < 0) continue;
      if (dx[i] == 0 && cell.x != here.x) continue;
      if (dy[i] == 0 && cell.y != here.y) continue;
      const int distance = detail::Chebyshev(here, cell);
      if (distance > r || distance >= bestDistance) continue;
      bestDistance = distance;
      best = cell;
      label = detail::PlaceLabel(place) + ", " + std::to_string(distance) + " tiles";
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
    if (candidate.key.rfind("here:", 0) == 0 && current != nullptr &&
        candidate.placeId == current->id) {
      hasCurrentRun = true;
      break;
    }
  }
  if (current != nullptr && !hasCurrentRun && !candidates.empty()) {
    out << "\nDEAD END: the place underfoot has no observed continuation; "
           "only other places are reachable.";
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

inline std::string PlanToJson(const Plan &plan) {
  nlohmann::json root;
  root["strategy"] = plan.strategy;
  root["subtask"] = plan.subtask;
  root["monitors"] = nlohmann::json::array();
  for (const Monitor &monitor : plan.monitors) {
    nlohmann::json entry;
    entry["id"] = monitor.id;
    entry["question"] = monitor.question;
    entry["handoff_when"] = monitor.handoffOnYes ? "yes" : "no";
    entry["min_confidence"] = monitor.minConfidence;
    root["monitors"].push_back(std::move(entry));
  }
  return root.dump(2);
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

inline std::string BuildPlanSummary(const Plan &plan) {
  std::ostringstream out;
  out << "strategy: " << plan.strategy;
  if (!plan.subtask.empty()) out << " | subtask: " << plan.subtask;
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
