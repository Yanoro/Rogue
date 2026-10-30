#include "NavigationDebugWindow.h"

#include "Components.h"
#include "Settings.h"

#include "AgentBrain.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::string FormatUnit(float value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
  return buffer;
}

// A read-only, scrollable block with a copy button. The observation and the
// brief are JSON that a reader will want to paste into a ticket or a diff, so
// copying is the one action offered.
void DrawTextBlock(const char *label, const std::string &text, float height) {
  ImGui::SeparatorText(label);
  const std::string copyLabel = std::string("Copy##copy_") + label;
  if (ImGui::Button(copyLabel.c_str())) {
    ImGui::SetClipboardText(text.c_str());
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu bytes", text.size());

  const std::string childLabel = std::string("##block_") + label;
  if (ImGui::BeginChild(childLabel.c_str(), ImVec2(0.0f, height),
                        ImGuiChildFlags_Borders)) {
    if (text.empty()) {
      ImGui::TextDisabled("(empty)");
    } else {
      // TextUnformatted keeps the observation/brief/raw reply literal, but on
      // its own it never wraps: the child grows horizontally and the only way
      // to read a long line is to widen the window. Wrapping at the child's
      // content edge also breaks long unbroken JSON tokens at the border.
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(text.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  ImGui::EndChild();
}

// The compact S1 answer: "signpost 0.95/0.90!" means p=0.95, confidence=0.90,
// and it triggered the handoff. A missing answer is "id ?".
std::string MonitorSummary(const NavigationDebugS1Response &response) {
  std::string out;
  for (const NavigationDebugMonitorAnswer &monitor : response.monitors) {
    if (!out.empty()) out += "; ";
    out += monitor.id;
    out += " ";
    if (!monitor.answered) {
      out += "?";
      continue;
    }
    out += FormatUnit(monitor.probability);
    out += "/";
    out += FormatUnit(monitor.confidence);
    if (monitor.triggered) out += "!";
  }
  return out;
}

std::string CandidateKind(const NavigationDebugCandidate &candidate) {
  return candidate.key.rfind("here:", 0) == 0 ? "local" : "place";
}

struct GraphNodeView {
  std::string key;
  std::string label;
  std::string kind;
  float x = 0.0f;
  float y = 0.0f;
  bool visited = false;
  int regionLevel = -1;
};

// The direction strings the graph uses ("north", "south-east", ...), turned into
// a unit vector in screen space. +y is south, matching CompassWord.
ImVec2 DirectionVector(const std::string &direction) {
  ImVec2 d(0.0f, 0.0f);
  if (direction.find("north") != std::string::npos) d.y -= 1.0f;
  if (direction.find("south") != std::string::npos) d.y += 1.0f;
  if (direction.find("east") != std::string::npos) d.x += 1.0f;
  if (direction.find("west") != std::string::npos) d.x -= 1.0f;
  const float length = std::sqrt(d.x * d.x + d.y * d.y);
  if (length > 0.0f) {
    d.x /= length;
    d.y /= length;
  }
  return d;
}

void IncludeGraphBounds(const nlohmann::json &graph, GraphBounds &bounds) {
  if (!graph.is_object() || !graph.contains("nodes") ||
      !graph["nodes"].is_array()) {
    return;
  }
  for (const nlohmann::json &jsonNode : graph["nodes"]) {
    if (!jsonNode.is_object()) continue;
    const nlohmann::json *position = nullptr;
    if (jsonNode.contains("anchor") && jsonNode["anchor"].is_array() &&
        jsonNode["anchor"].size() >= 2) {
      position = &jsonNode["anchor"];
    } else if (jsonNode.contains("cell") && jsonNode["cell"].is_array() &&
               jsonNode["cell"].size() >= 2) {
      position = &jsonNode["cell"];
    }
    if (position == nullptr) continue;
    const float x = (*position)[0].get<float>();
    const float y = (*position)[1].get<float>();
    if (!bounds.valid) {
      bounds.valid = true;
      bounds.minX = bounds.maxX = x;
      bounds.minY = bounds.maxY = y;
      continue;
    }
    bounds.minX = std::min(bounds.minX, x);
    bounds.maxX = std::max(bounds.maxX, x);
    bounds.minY = std::min(bounds.minY, y);
    bounds.maxY = std::max(bounds.maxY, y);
  }
}

GraphBounds CollectGraphBounds(const NavigationDebugState &state) {
  GraphBounds bounds;
  for (const NavigationDebugStrategistThought &thought :
       state.strategistThoughts) {
    if (thought.graphJson.empty()) continue;
    const nlohmann::json graph =
        nlohmann::json::parse(thought.graphJson, nullptr, false);
    IncludeGraphBounds(graph, bounds);
  }
  // The latest observation may include the current call's graph before/after
  // the thought record; include it so the live view shares the same frame.
  const nlohmann::json observation =
      nlohmann::json::parse(state.strategistObservation, nullptr, false);
  if (!observation.is_discarded() && observation.contains("graph")) {
    IncludeGraphBounds(observation["graph"], bounds);
  }
  return bounds;
}

// The union of every graph up to `lastIndex`, with the latest live observation
// appended when `includeObservation` is true.
//
// A graph snapshot only contains what the agent could see and fit at that call
// (radius, maxNodes/maxEdges, candidates). Drawing one snapshot therefore made a
// node disappear as soon as the character walked away from it, even though the
// navigator still remembered it. The graph view is a picture of the episode's
// accumulated knowledge, so nodes are merged by key and never dropped: the first
// sighting establishes the node, a later sighting refreshes its label/position
// and preserves the monotonic `visited` flag. Edges are merged the same way.
//
// `current`, `frontier` and `dead_ends` come from the newest graph in the merge,
// not from the union: they describe the state at the selected decision, while the
// nodes describe everything seen up to it.
nlohmann::json MergeGraphSnapshots(
    const std::vector<NavigationDebugStrategistThought> &thoughts,
    int lastIndex, const nlohmann::json &latestObservation,
    bool includeObservation) {
  nlohmann::json nodes = nlohmann::json::array();
  nlohmann::json edges = nlohmann::json::array();
  std::unordered_map<std::string, std::size_t> nodeIndex;
  std::unordered_map<std::string, std::size_t> edgeIndex;
  nlohmann::json meta = nlohmann::json::object();

  const auto mergeGraph = [&](const nlohmann::json &graph) {
    if (!graph.is_object()) return;
    if (graph.contains("nodes") && graph["nodes"].is_array()) {
      for (const nlohmann::json &jsonNode : graph["nodes"]) {
        if (!jsonNode.is_object()) continue;
        const std::string key = jsonNode.value("key", std::string());
        if (key.empty()) continue;
        const auto found = nodeIndex.find(key);
        if (found == nodeIndex.end()) {
          nodeIndex.emplace(key, nodes.size());
          nodes.push_back(jsonNode);
          continue;
        }
        nlohmann::json &existing = nodes[found->second];
        const bool wasVisited = existing.value("visited", false);
        existing = jsonNode;
        if (wasVisited) existing["visited"] = true;
      }
    }

    if (graph.contains("edges") && graph["edges"].is_array()) {
      for (const nlohmann::json &jsonEdge : graph["edges"]) {
        if (!jsonEdge.is_object()) continue;
        const std::string from = jsonEdge.value("from", std::string());
        const std::string to = jsonEdge.value("to", std::string());
        if (from.empty() || to.empty()) continue;
        const std::string key =
            from + ">" + to + ">" + jsonEdge.value("rel", std::string()) + ">" +
            jsonEdge.value("direction", std::string()) + ">" +
            jsonEdge.value("source", std::string());
        const auto found = edgeIndex.find(key);
        if (found == edgeIndex.end()) {
          edgeIndex.emplace(key, edges.size());
          edges.push_back(jsonEdge);
        } else {
          edges[found->second] = jsonEdge;
        }
      }
    }

    for (const char *field : {"current", "frontier", "dead_ends", "route"}) {
      if (graph.contains(field)) {
        meta[field] = graph[field];
      }
    }
  };

  for (int i = 0; i <= lastIndex && i < static_cast<int>(thoughts.size());
       ++i) {
    if (thoughts[i].graphJson.empty()) continue;
    const nlohmann::json graph =
        nlohmann::json::parse(thoughts[i].graphJson, nullptr, false);
    if (!graph.is_discarded()) mergeGraph(graph);
  }
  if (includeObservation && latestObservation.is_object() &&
      latestObservation.contains("graph")) {
    mergeGraph(latestObservation["graph"]);
  }

  if (nodes.empty()) {
    return nlohmann::json::object();
  }
  nlohmann::json merged = nlohmann::json::object();
  merged["nodes"] = std::move(nodes);
  merged["edges"] = std::move(edges);
  for (auto &[field, value] : meta.items()) {
    merged[field] = std::move(value);
  }
  return merged;
}

// A spatial drawing of the strategist's observed place/object graph. Nodes are
// at their observed world positions, so the picture is the world the agent has
// built, not a force-directed abstraction of it.
void DrawLocationGraph(const std::string &graphJson, float height,
                       bool &hideObjects, bool &hideNames,
                       const GraphBounds &sharedBounds) {
  ImGui::SeparatorText("Agent location graph");
  ImGui::Checkbox("Hide objects", &hideObjects);
  ImGui::SameLine();
  ImGui::Checkbox("Hide names", &hideNames);
  if (hideNames) {
    ImGui::SameLine();
    ImGui::TextDisabled("(hover a node for its name)");
  }
  if (graphJson.empty()) {
    ImGui::TextDisabled("(no graph snapshot for this call)");
    return;
  }

  const nlohmann::json graph =
      nlohmann::json::parse(graphJson, nullptr, false);
  if (graph.is_discarded() || !graph.is_object() || !graph.contains("nodes") ||
      !graph["nodes"].is_array()) {
    ImGui::TextDisabled("(the graph snapshot could not be read)");
    return;
  }

  std::vector<GraphNodeView> nodes;
  for (const nlohmann::json &jsonNode : graph["nodes"]) {
    if (!jsonNode.is_object()) continue;
    const nlohmann::json *position = nullptr;
    if (jsonNode.contains("anchor") && jsonNode["anchor"].is_array() &&
        jsonNode["anchor"].size() >= 2) {
      position = &jsonNode["anchor"];
    } else if (jsonNode.contains("cell") && jsonNode["cell"].is_array() &&
               jsonNode["cell"].size() >= 2) {
      position = &jsonNode["cell"];
    }
    if (position == nullptr) continue;
    GraphNodeView node;
    node.key = jsonNode.value("key", std::string());
    node.kind = jsonNode.value("kind", std::string("place"));
    node.visited = jsonNode.value("visited", false);
    node.regionLevel = jsonNode.value("regionLevel", -1);
    node.x = (*position)[0].get<float>();
    node.y = (*position)[1].get<float>();
    node.label = jsonNode.value("label", std::string());
    if (node.label.empty()) node.label = jsonNode.value("name", std::string());
    if (node.label.empty()) node.label = jsonNode.value("category", std::string());
    if (node.label.empty()) node.label = node.key;
    if (hideObjects && node.kind == "object") continue;
    nodes.push_back(std::move(node));
  }
  if (nodes.empty()) {
    ImGui::TextDisabled("(the graph has no placed nodes yet)");
    return;
  }

  std::unordered_map<std::string, std::size_t> byKey;
  for (std::size_t i = 0; i < nodes.size(); ++i) byKey[nodes[i].key] = i;

  const std::string currentKey = graph.value("current", std::string());
  std::unordered_set<std::string> frontierPlaces;
  std::unordered_set<std::string> deadEnds;
  std::vector<nlohmann::json> frontierFromCurrent;
  if (graph.contains("frontier") && graph["frontier"].is_array()) {
    for (const nlohmann::json &entry : graph["frontier"]) {
      if (!entry.is_object()) continue;
      const std::string place = entry.value("place", std::string());
      if (!place.empty()) frontierPlaces.insert(place);
      if (place == currentKey && entry.contains("direction") &&
          entry["direction"].is_string()) {
        frontierFromCurrent.push_back(entry);
      }
    }
  }
  if (graph.contains("dead_ends") && graph["dead_ends"].is_array()) {
    for (const nlohmann::json &entry : graph["dead_ends"]) {
      if (entry.is_object()) {
        deadEnds.insert(entry.value("key", std::string()));
      }
    }
  }

  float minX = sharedBounds.valid ? sharedBounds.minX : nodes[0].x;
  float maxX = sharedBounds.valid ? sharedBounds.maxX : nodes[0].x;
  float minY = sharedBounds.valid ? sharedBounds.minY : nodes[0].y;
  float maxY = sharedBounds.valid ? sharedBounds.maxY : nodes[0].y;
  if (!sharedBounds.valid) {
    for (const GraphNodeView &node : nodes) {
      minX = std::min(minX, node.x);
      maxX = std::max(maxX, node.x);
      minY = std::min(minY, node.y);
      maxY = std::max(maxY, node.y);
    }
  }

  ImGui::BeginChild("NavigationLocationGraph", ImVec2(0.0f, height),
                    ImGuiChildFlags_Borders);
  const ImVec2 canvas = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(canvas,
                      ImVec2(canvas.x + size.x, canvas.y + size.y),
                      IM_COL32(18, 20, 24, 255));

  const float pad = 32.0f;
  const float worldW = std::max(1.0f, maxX - minX);
  const float worldH = std::max(1.0f, maxY - minY);
  const float scale = std::min((size.x - 2.0f * pad) / worldW,
                               (size.y - 2.0f * pad) / worldH);
  const float originX = canvas.x + (size.x - worldW * scale) * 0.5f;
  const float originY = canvas.y + (size.y - worldH * scale) * 0.5f;
  auto screen = [&](float x, float y) {
    return ImVec2(originX + (x - minX) * scale,
                  originY + (y - minY) * scale);
  };

  if (graph.contains("edges") && graph["edges"].is_array()) {
    for (const nlohmann::json &edge : graph["edges"]) {
      if (!edge.is_object()) continue;
      const auto from = byKey.find(edge.value("from", std::string()));
      const auto to = byKey.find(edge.value("to", std::string()));
      if (from == byKey.end() || to == byKey.end()) continue;
      const ImVec2 a = screen(nodes[from->second].x, nodes[from->second].y);
      const ImVec2 b = screen(nodes[to->second].x, nodes[to->second].y);
      const bool walked = edge.value("source", std::string()) == "walked";
      draw->AddLine(a, b,
                    walked ? IM_COL32(80, 200, 255, 220)
                           : IM_COL32(125, 132, 145, 170),
                    walked ? 2.2f : 1.2f);
    }
  }

  const auto currentIt = byKey.find(currentKey);
  if (currentIt != byKey.end()) {
    const GraphNodeView &current = nodes[currentIt->second];
    const ImVec2 centre = screen(current.x, current.y);
    for (const nlohmann::json &entry : frontierFromCurrent) {
      const ImVec2 direction =
          DirectionVector(entry.value("direction", std::string()));
      if (direction.x == 0.0f && direction.y == 0.0f) continue;
      draw->AddLine(centre,
                    ImVec2(centre.x + direction.x * 24.0f,
                           centre.y + direction.y * 24.0f),
                    IM_COL32(240, 200, 60, 230), 2.5f);
    }
  }

  const auto nodeLabel = [](const GraphNodeView &node) {
    std::string label = node.label;
    if (node.regionLevel >= 0) {
      label += node.regionLevel == 0   ? " [R8]"
               : node.regionLevel == 1 ? " [R16]"
                                       : " [R32]";
    }
    return label;
  };

  for (const GraphNodeView &node : nodes) {
    const ImVec2 centre = screen(node.x, node.y);
    const bool isCurrent = node.key == currentKey;
    const bool isFrontier = frontierPlaces.count(node.key) != 0;
    const bool isDeadEnd = deadEnds.count(node.key) != 0;
    const ImU32 fill =
        node.kind == "object"
            ? IM_COL32(215, 145, 65, 235)
            : (node.visited ? IM_COL32(72, 130, 190, 235)
                            : IM_COL32(178, 188, 202, 235));
    const float radius = isCurrent ? 9.0f : 6.0f;
    draw->AddCircleFilled(centre, radius, fill);
    if (isFrontier) {
      draw->AddCircle(centre, radius + 3.0f, IM_COL32(240, 200, 60, 255), 0,
                      2.0f);
    }
    if (isDeadEnd) {
      draw->AddCircle(centre, radius + 3.0f, IM_COL32(230, 80, 70, 255), 0,
                      2.0f);
    }
    if (isCurrent) {
      draw->AddCircle(centre, radius + 4.0f, IM_COL32(90, 240, 150, 255), 0,
                      2.0f);
    }
    if (!hideNames) {
      std::string label = nodeLabel(node);
      constexpr std::size_t kMaxLabel = 34;
      if (label.size() > kMaxLabel) label = label.substr(0, kMaxLabel) + "...";
      draw->AddText(ImVec2(centre.x + radius + 4.0f, centre.y - 6.0f),
                    IM_COL32(225, 230, 238, 235), label.c_str());
      if (isCurrent) {
        draw->AddText(ImVec2(centre.x + radius + 4.0f, centre.y + 8.0f),
                      IM_COL32(90, 240, 150, 235), "you are here");
      }
    }
  }

  if (hideNames && ImGui::IsWindowHovered()) {
    const ImVec2 mouse = ImGui::GetMousePos();
    const GraphNodeView *hovered = nullptr;
    float bestDistance = 1.0e9f;
    for (const GraphNodeView &node : nodes) {
      const ImVec2 centre = screen(node.x, node.y);
      const float radius = node.key == currentKey ? 9.0f : 6.0f;
      const float dx = mouse.x - centre.x;
      const float dy = mouse.y - centre.y;
      const float distance = dx * dx + dy * dy;
      const float reach = radius + 4.0f;
      if (distance <= reach * reach && distance < bestDistance) {
        bestDistance = distance;
        hovered = &node;
      }
    }
    if (hovered != nullptr) {
      ImGui::SetTooltip("%s", nodeLabel(*hovered).c_str());
    }
  }

  draw->AddText(ImVec2(canvas.x + 8.0f, canvas.y + 6.0f),
                IM_COL32(170, 180, 195, 220), "N");
  draw->AddText(ImVec2(canvas.x + size.x - 16.0f, canvas.y + 6.0f),
                IM_COL32(170, 180, 195, 220), "E");
  ImGui::EndChild();

  ImGui::TextDisabled(
      "green = current; yellow ring/tick = frontier; blue = visited place; "
      "orange = object; red ring = dead end; cyan edge = walked.");
}

void DrawS1ResponsesSection(
    const NavigationDebugState &state,
    const NavigationDebugStrategistThought *selectedCall) {
  ImGui::SeparatorText("S1 responses");

  std::vector<const NavigationDebugS1Response *> responses;
  if (selectedCall != nullptr) {
    responses.reserve(selectedCall->s1Responses.size());
    for (auto it = selectedCall->s1Responses.rbegin();
         it != selectedCall->s1Responses.rend(); ++it) {
      responses.push_back(&*it);
    }
    ImGui::TextDisabled("Answers that followed Call %d.", selectedCall->index);
  } else {
    responses.reserve(state.s1Responses.size());
    for (auto it = state.s1Responses.rbegin();
         it != state.s1Responses.rend(); ++it) {
      responses.push_back(&*it);
    }
    ImGui::TextDisabled("Recent S1 answers (no strategist call selected).");
  }

  if (responses.empty()) {
    ImGui::TextDisabled(selectedCall != nullptr
                            ? "(no S1 answers for this call)"
                            : "(no S1 answers yet)");
    return;
  }

  if (ImGui::BeginTable("NavigationS1Responses", 8,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0.0f, 190.0f))) {
    ImGui::TableSetupColumn("Decision", ImGuiTableColumnFlags_WidthFixed,
                            65.0f);
    ImGui::TableSetupColumn("Next", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Outcome", ImGuiTableColumnFlags_WidthFixed,
                            70.0f);
    ImGui::TableSetupColumn("p", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("Margin", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Monitors");
    ImGui::TableSetupColumn("Handoff");
    ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableHeadersRow();
    for (const NavigationDebugS1Response *response : responses) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%d", response->decision);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(response->choice.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(response->outcome.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%.2f", static_cast<double>(response->probability));
      ImGui::TableNextColumn();
      ImGui::Text("%.2f", static_cast<double>(response->margin));
      ImGui::TableNextColumn();
      const std::string monitors = MonitorSummary(*response);
      ImGui::TextWrapped("%s", monitors.empty() ? "-" : monitors.c_str());
      ImGui::TableNextColumn();
      if (!response->ok) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "error");
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", response->error.c_str());
        }
      } else if (response->handoff.empty()) {
        ImGui::TextDisabled("-");
      } else {
        ImGui::TextWrapped("%s", response->handoff.c_str());
      }
      ImGui::TableNextColumn();
      if (response->latencyMs >= 0) {
        ImGui::Text("%lld", response->latencyMs);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("prompt %d, output %d tokens",
                            response->promptTokens, response->outputTokens);
        }
      } else {
        ImGui::TextDisabled("-");
      }
    }
    ImGui::EndTable();
  }
}

}  // namespace

NavigationDebugWindow::NavigationDebugWindow(flecs::entity npc,
                                             flecs::entity owner)
    : npc(npc), owner(owner) {}

void NavigationDebugWindow::Draw() {
  const bool alive = npc.is_alive();
  std::string name = "Unknown";
  if (alive) {
    if (const DisplayName *display = npc.get<DisplayName>()) {
      name = display->name;
    } else if (npc.name().c_str() != nullptr) {
      name = npc.name().c_str();
    }
  }

  char title[192];
  std::snprintf(title, sizeof(title),
                "Navigation: %s###navigation_debug_%llu", name.c_str(),
                static_cast<unsigned long long>(alive ? npc.id() : 0));

  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(900, 680), ImGuiCond_FirstUseEver);
  ImGui::Begin(title, &isOpen, ImGuiWindowFlags_None);

  if (!isOpen) {
    if (owner.is_alive()) {
      owner.remove<ActiveWindow>();
    }
    ImGui::End();
    return;
  }

  if (!alive) {
    ImGui::TextDisabled("This character is gone.");
    ImGui::End();
    return;
  }

  const NavigationDebugState *state = npc.get<NavigationDebugState>();
  if (state == nullptr) {
    ImGui::TextDisabled(
        "No navigation has run for this character yet. Use [NAVIGATE] and the "
        "episode's strategist, pilot and S1 answers will appear here.");
    ImGui::End();
    return;
  }

  const GameSettings &settings = Settings();
  const GameSettings::SystemOneNavigationSection &nav = settings.system1.navigation;
  const GameSettings::NavigationStrategistSection &strategist =
      settings.navigation.strategist;

  // Pause/Resume is per NPC. It interrupts the current navigation action
  // (cancelling its in-flight S1/strategist call) and parks it on the brain's
  // action stack; Resume continues the same episode.
  AgentBrain *brain = nullptr;
  if (AgentBrainWrapper *wrapper = npc.get_mut<AgentBrainWrapper>()) {
    brain = wrapper->agBrain.get();
  }
  const bool paused = brain != nullptr && brain->IsPaused();
  const bool canPause = brain != nullptr && (state->active || paused);
  if (canPause) {
    if (ImGui::Button(paused ? "Resume navigator" : "Pause navigator")) {
      if (paused) {
        brain->Resume();
      } else {
        brain->Pause();
      }
    }
    if (paused) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "PAUSED");
    }
  } else {
    ImGui::BeginDisabled();
    ImGui::Button("Pause navigator");
    ImGui::EndDisabled();
  }
  ImGui::SameLine();

  // ── Status ───────────────────────────────────────────────────────────────
  if (state->active) {
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "ACTIVE");
  } else {
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                       "LAST SNAPSHOT (inactive)");
  }
  ImGui::SameLine();
  ImGui::Text("phase: %s", state->phase.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("goal: %s", state->goal.c_str());

  ImGui::Text("Strategy v%d | %d/%d strategies | %d handoffs | %d decisions | "
              "%d legs | %d surveys | ready: %s | fallback: %s",
              state->strategyVersion, state->strategiesUsed,
              strategist.maxStrategies, state->handoffs, state->decisions,
              state->legs, state->surveysUsed,
              state->strategyReady ? "yes" : "no",
              state->strategistFallback ? "yes" : "no");
  ImGui::TextDisabled("Resource guards: %.0f s, %d surveys, %d strategies, "
                      "%d monitors, min conf %.2f",
                      static_cast<double>(nav.maxSeconds), nav.maxSurveys,
                      strategist.maxStrategies, strategist.maxMonitors,
                      static_cast<double>(strategist.monitorMinConfidence));

  if (!state->lastHandoff.empty()) {
    ImGui::TextWrapped("Last handoff: %s", state->lastHandoff.c_str());
  } else {
    ImGui::TextDisabled("Last handoff: none");
  }
  if (!state->repetition.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Repetition signal: %s",
                       state->repetition.c_str());
  }
  if (!state->route.empty()) {
    ImGui::TextWrapped("Route: %s", state->route.c_str());
  }
  if (!state->visited.empty()) {
    ImGui::TextWrapped("Visited: %s", state->visited.c_str());
  }
  ImGui::Separator();

  if (ImGui::BeginTabBar("NavigationDebugTabs",
                         ImGuiTabBarFlags_None)) {
    // ── Overview ───────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem("Overview")) {
      // The Overview is a view of one strategist call. Without any call history
      // (e.g. the strategist is disabled) fall back to the latest live plan.
      const NavigationDebugStrategistThought *selectedCall = nullptr;
      if (!state->strategistThoughts.empty()) {
        const int callCount =
            static_cast<int>(state->strategistThoughts.size());
        if (followLatestThought) {
          selectedCall = &state->strategistThoughts[callCount - 1];
        } else if (selectedThought >= 0 && selectedThought < callCount) {
          selectedCall = &state->strategistThoughts[selectedThought];
        }
      }
      const std::string strategyText =
          selectedCall != nullptr ? selectedCall->strategy : state->strategy;
      const std::string subtaskText =
          selectedCall != nullptr ? selectedCall->subtask : state->subtask;
      const std::vector<NavigationDebugMonitor> &monitors =
          selectedCall != nullptr ? selectedCall->monitors : state->monitors;
      const std::vector<NavigationDebugCandidate> &candidates =
          selectedCall != nullptr ? selectedCall->candidates : state->candidates;

      ImGui::SeparatorText("Strategy");
      if (strategyText.empty()) {
        ImGui::TextDisabled(selectedCall != nullptr
                                ? "(no plan for this call)"
                                : "(no strategist plan)");
      } else {
        ImGui::TextWrapped("%s", strategyText.c_str());
      }
      if (!subtaskText.empty()) {
        ImGui::TextDisabled("Subtask: %s", subtaskText.c_str());
      }

      ImGui::Checkbox("Full context", &showFullContext);
      if (showFullContext) {
        if (state->fullContext.empty()) {
          ImGui::TextDisabled(
              "(no single-navigator message context in this snapshot)");
        } else {
          ImGui::TextDisabled(
              "The latest single-navigator decision, including the system "
              "prompt and every retry correction.");
          DrawTextBlock("Navigator context", state->fullContext, 280.0f);
        }
      }

      // Stage 1 debug view: the grouped object profiles the navigator will use
      // once the prompt switches to the new grammar. The current prompt still
      // carries the per-instance examine list, so this is informational only.
      ImGui::SeparatorText("Object groups (debug)");
      if (state->objectGroups.empty()) {
        ImGui::TextDisabled("(none)");
      } else if (ImGui::BeginTable("NavigationObjectGroups", 4,
                                   ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Group");
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed,
                                55.0f);
        ImGui::TableSetupColumn("Verbs");
        ImGui::TableSetupColumn("Nearest", ImGuiTableColumnFlags_WidthFixed,
                                110.0f);
        ImGui::TableHeadersRow();
        for (const NavigationDebugObjectGroup &group : state->objectGroups) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(group.label.c_str());
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("key: %s", group.key.c_str());
          }
          if (group.structural) {
            ImGui::SameLine();
            ImGui::TextDisabled("(structure)");
          }
          ImGui::TableNextColumn();
          ImGui::Text("x%d", group.count);
          ImGui::TableNextColumn();
          if (group.interactions.empty()) {
            ImGui::TextDisabled("-");
          } else {
            std::string verbs;
            for (std::size_t i = 0; i < group.interactions.size(); ++i) {
              if (i > 0) verbs += ", ";
              verbs += group.interactions[i];
            }
            ImGui::TextUnformatted(verbs.c_str());
          }
          ImGui::TableNextColumn();
          if (group.nearestAlias.empty()) {
            ImGui::TextDisabled("-");
          } else {
            ImGui::TextUnformatted(group.nearestAlias.c_str());
          }
          ImGui::TextDisabled("%s %d", group.nearestDirection.c_str(),
                              group.nearestDistance);
        }
        ImGui::EndTable();
      }

      // The graph is the accumulated picture up to the selected call, not that
      // one snapshot: walking away from a node must not erase it from the view.
      // A pinned older call shows the episode as it stood at that call; the
      // latest view also folds in the live observation.
      const int thoughtCount =
          static_cast<int>(state->strategistThoughts.size());
      int lastGraphIndex = -1;
      bool includeObservation = state->strategistThoughts.empty();
      if (selectedCall != nullptr) {
        lastGraphIndex =
            followLatestThought ? thoughtCount - 1 : selectedThought;
        includeObservation = followLatestThought;
      }
      if (lastGraphIndex < 0 && !state->strategistThoughts.empty()) {
        lastGraphIndex = thoughtCount - 1;
        includeObservation = true;
      }
      const nlohmann::json latestObservation =
          nlohmann::json::parse(state->strategistObservation, nullptr, false);
      if (state->snapshotVersion != cachedGraphBoundsVersion) {
        cachedGraphBounds = CollectGraphBounds(*state);
        cachedGraphBoundsVersion = state->snapshotVersion;
      }
      if (state->snapshotVersion != cachedMergedGraphVersion ||
          lastGraphIndex != cachedMergedGraphLastIndex ||
          includeObservation != cachedMergedGraphIncludeObservation) {
        const nlohmann::json merged = MergeGraphSnapshots(
            state->strategistThoughts, lastGraphIndex, latestObservation,
            includeObservation);
        cachedMergedGraphJson = merged.empty() ? std::string() : merged.dump();
        cachedMergedGraphVersion = state->snapshotVersion;
        cachedMergedGraphLastIndex = lastGraphIndex;
        cachedMergedGraphIncludeObservation = includeObservation;
      }
      DrawLocationGraph(cachedMergedGraphJson, 300.0f, hideGraphObjects,
                        hideGraphNames, cachedGraphBounds);

      // The raw assistant stream, before it has been parsed into thought/plan/
      // move. This updates without republishing the snapshot, so it follows the
      // model token by token while it is thinking.
      if (!state->liveThought.empty()) {
        ImGui::SeparatorText("Live navigator output");
        DrawTextBlock("Streaming", state->liveThought, 240.0f);
      }

      ImGui::SeparatorText("Navigator thoughts");
      if (state->strategistThoughts.empty()) {
        ImGui::TextDisabled("(no strategist call yet)");
      } else {
        const int count = static_cast<int>(state->strategistThoughts.size());
        if (selectedThought >= count) selectedThought = count - 1;
        if (ImGui::Button("Latest##thought")) {
          followLatestThought = true;
          selectedThought = -1;
        }
        for (int i = 0; i < count; ++i) {
          ImGui::SameLine();
          const NavigationDebugStrategistThought &record =
              state->strategistThoughts[i];
          const std::string label =
              "Call " + std::to_string(record.index) + "##thought" +
              std::to_string(i);
          const bool selected =
              followLatestThought ? (i == count - 1) : (i == selectedThought);
          if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.20f, 0.38f, 0.62f, 1.0f));
          }
          if (ImGui::Button(label.c_str())) {
            followLatestThought = false;
            selectedThought = i;
          }
          if (selected) ImGui::PopStyleColor();
          if (ImGui::IsItemHovered() && !record.reason.empty()) {
            ImGui::SetTooltip("%s", record.reason.c_str());
          }
        }

        const int index = followLatestThought ? count - 1 : selectedThought;
        if (index >= 0 && index < count) {
          const NavigationDebugStrategistThought &record =
              state->strategistThoughts[index];
          if (!record.reason.empty()) {
            ImGui::TextDisabled("Call reason: %s", record.reason.c_str());
          }
          if (!record.complete) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "Thinking...");
          }
          if (record.failed) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "This call failed: %s",
                               record.error.empty() ? "(no reason)"
                                                    : record.error.c_str());
          }
          if (record.failed && !record.attempts.empty()) {
            if (selectedAttemptThought != index) {
              selectedAttemptThought = index;
              selectedAttempt = -1;
            }
            ImGui::SeparatorText("Failed attempts");
            for (std::size_t attemptIndex = 0;
                 attemptIndex < record.attempts.size(); ++attemptIndex) {
              if (attemptIndex > 0) ImGui::SameLine();
              const NavigationDebugAttempt &attempt =
                  record.attempts[attemptIndex];
              const std::string label =
                  "Attempt " + std::to_string(attempt.number) + "##attempt" +
                  std::to_string(index) + "_" + std::to_string(attemptIndex);
              const bool selected =
                  selectedAttempt == static_cast<int>(attemptIndex);
              if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.42f, 0.26f, 0.16f, 1.0f));
              }
              if (ImGui::Button(label.c_str())) {
                selectedAttempt = static_cast<int>(attemptIndex);
              }
              if (selected) ImGui::PopStyleColor();
              if (ImGui::IsItemHovered() && !attempt.error.empty()) {
                ImGui::SetTooltip("%s", attempt.error.c_str());
              }
            }
            if (selectedAttempt >= 0 &&
                selectedAttempt < static_cast<int>(record.attempts.size())) {
              const NavigationDebugAttempt &attempt =
                  record.attempts[selectedAttempt];
              if (!attempt.error.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                                   "Error: %s", attempt.error.c_str());
              }
              const std::string label =
                  "Attempt " + std::to_string(attempt.number);
              DrawTextBlock(label.c_str(), attempt.response, 220.0f);
            }
          }
          if (record.thought.empty()) {
            if (record.complete && !record.failed) {
              ImGui::TextDisabled(
                  "No reasoning was emitted for this call.");
            }
          } else {
            DrawTextBlock("Thought", record.thought, 180.0f);
          }
        }
      }

      DrawS1ResponsesSection(*state, selectedCall);

      ImGui::SeparatorText("Monitors");
      if (monitors.empty()) {
        ImGui::TextDisabled("(none)");
      } else if (ImGui::BeginTable("NavigationMonitors", 4,
                                   ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Question");
        ImGui::TableSetupColumn("Handoff", ImGuiTableColumnFlags_WidthFixed,
                                70.0f);
        ImGui::TableSetupColumn("Min conf", ImGuiTableColumnFlags_WidthFixed,
                                70.0f);
        ImGui::TableHeadersRow();
        for (const NavigationDebugMonitor &monitor : monitors) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(monitor.id.c_str());
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s", monitor.question.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(monitor.handoffWhen.c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%.2f", static_cast<double>(monitor.minConfidence));
        }
        ImGui::EndTable();
      }

      ImGui::SeparatorText("Pilot candidates");
      if (candidates.empty()) {
        ImGui::TextDisabled("(none)");
      } else if (ImGui::BeginTable("NavigationCandidates", 4,
                                   ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Label");
        ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed,
                                70.0f);
        ImGui::TableHeadersRow();
        for (const NavigationDebugCandidate &candidate : candidates) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(candidate.key.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(CandidateKind(candidate).c_str());
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s", candidate.label.c_str());
          if (!candidate.description.empty() &&
              ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", candidate.description.c_str());
          }
          ImGui::TableNextColumn();
          ImGui::Text("%d", candidate.distance);
        }
        ImGui::EndTable();
      }

      if (!state->lastSummary.empty()) {
        ImGui::SeparatorText("Last summary");
        ImGui::TextWrapped("%s", state->lastSummary.c_str());
      }
      ImGui::EndTabItem();
    }

    // ── Strategist ─────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem("Strategist")) {
      if (!state->strategistError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s",
                           state->strategistError.c_str());
      }
      DrawTextBlock("Observation (JSON)", state->strategistObservation, 270.0f);
      DrawTextBlock("Brief", state->strategistBrief, 190.0f);
      DrawTextBlock("Raw reply", state->strategistResponse, 150.0f);
      ImGui::EndTabItem();
    }

    // ── Pilot ──────────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem("Pilot")) {
      DrawTextBlock("Pilot state (exact S1 input)", state->pilotState, 460.0f);
      if (!state->lastS1Error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Last S1 error: %s",
                           state->lastS1Error.c_str());
      }
      DrawTextBlock("Latest raw S1 response", state->lastS1Raw, 170.0f);
      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::End();
}
