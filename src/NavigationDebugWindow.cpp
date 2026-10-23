#include "NavigationDebugWindow.h"

#include "Components.h"
#include "Settings.h"

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

// A spatial drawing of the strategist's observed place/object graph. Nodes are
// at their observed world positions, so the picture is the world the agent has
// built, not a force-directed abstraction of it.
void DrawLocationGraph(const std::string &graphJson, float height) {
  ImGui::SeparatorText("Agent location graph");
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
    node.x = (*position)[0].get<float>();
    node.y = (*position)[1].get<float>();
    node.label = jsonNode.value("label", std::string());
    if (node.label.empty()) node.label = jsonNode.value("name", std::string());
    if (node.label.empty()) node.label = jsonNode.value("category", std::string());
    if (node.label.empty()) node.label = node.key;
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

  float minX = nodes[0].x;
  float maxX = nodes[0].x;
  float minY = nodes[0].y;
  float maxY = nodes[0].y;
  for (const GraphNodeView &node : nodes) {
    minX = std::min(minX, node.x);
    maxX = std::max(maxX, node.x);
    minY = std::min(minY, node.y);
    maxY = std::max(maxY, node.y);
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
    std::string label = node.label;
    constexpr std::size_t kMaxLabel = 30;
    if (label.size() > kMaxLabel) label = label.substr(0, kMaxLabel) + "...";
    draw->AddText(ImVec2(centre.x + radius + 4.0f, centre.y - 6.0f),
                  IM_COL32(225, 230, 238, 235), label.c_str());
    if (isCurrent) {
      draw->AddText(ImVec2(centre.x + radius + 4.0f, centre.y + 8.0f),
                    IM_COL32(90, 240, 150, 235), "you are here");
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

      std::string graphJson;
      if (selectedCall != nullptr) {
        graphJson = selectedCall->graphJson;
      } else {
        // No call history (e.g. the strategist is disabled): show the latest
        // observation's graph so the panel is still useful.
        const nlohmann::json observation =
            nlohmann::json::parse(state->strategistObservation, nullptr, false);
        if (!observation.is_discarded() && observation.contains("graph")) {
          graphJson = observation["graph"].dump();
        }
      }
      DrawLocationGraph(graphJson, 300.0f);

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
      ImGui::EndTabItem();
    }

    // ── S1 responses ──────────────────────────────────────────────────────
    if (ImGui::BeginTabItem("S1 Responses")) {
      ImGui::TextDisabled(
          "Compact answers, newest first. p/conf is the monitor probability and "
          "confidence (2*|p-0.5|); ! means it triggered a handoff.");

      if (state->s1Responses.empty()) {
        ImGui::TextDisabled("(no S1 answers yet)");
      } else {
        const int count = static_cast<int>(state->s1Responses.size());
        if (ImGui::BeginTable("NavigationS1Responses", 8,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollX |
                                  ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0.0f, 190.0f))) {
          ImGui::TableSetupColumn("Decision", ImGuiTableColumnFlags_WidthFixed,
                                  65.0f);
          ImGui::TableSetupColumn("Next", ImGuiTableColumnFlags_WidthFixed,
                                  110.0f);
          ImGui::TableSetupColumn("Outcome", ImGuiTableColumnFlags_WidthFixed,
                                  70.0f);
          ImGui::TableSetupColumn("p", ImGuiTableColumnFlags_WidthFixed, 50.0f);
          ImGui::TableSetupColumn("Margin", ImGuiTableColumnFlags_WidthFixed,
                                  60.0f);
          ImGui::TableSetupColumn("Monitors");
          ImGui::TableSetupColumn("Handoff");
          ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 50.0f);
          ImGui::TableHeadersRow();
          for (int i = count - 1; i >= 0; --i) {
            const NavigationDebugS1Response &response = state->s1Responses[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", response.decision);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(response.choice.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(response.outcome.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", static_cast<double>(response.probability));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", static_cast<double>(response.margin));
            ImGui::TableNextColumn();
            const std::string monitors = MonitorSummary(response);
            ImGui::TextWrapped("%s", monitors.empty() ? "-" : monitors.c_str());
            ImGui::TableNextColumn();
            if (!response.ok) {
              ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "error");
              if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", response.error.c_str());
              }
            } else if (response.handoff.empty()) {
              ImGui::TextDisabled("-");
            } else {
              ImGui::TextWrapped("%s", response.handoff.c_str());
            }
            ImGui::TableNextColumn();
            if (response.latencyMs >= 0) {
              ImGui::Text("%lld", response.latencyMs);
              if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("prompt %d, output %d tokens",
                                  response.promptTokens,
                                  response.outputTokens);
              }
            } else {
              ImGui::TextDisabled("-");
            }
          }
          ImGui::EndTable();
        }
      }

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
