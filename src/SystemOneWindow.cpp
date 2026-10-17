#include "SystemOneWindow.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Components.h"
#include "Game.h"
#include "imgui.h"

namespace {

// One line per record, truncating the summary rather than wrapping it: a wrapped
// row makes the list unreadable at a glance, which is the only thing the list is
// for. The detail pane shows the whole thing.
std::string Truncate(const std::string &text, std::size_t maxLength) {
  if (text.size() <= maxLength) return text;
  return text.substr(0, maxLength - 3) + "...";
}

// The recorded latency in the form the header prints. Reports "?" rather than 0
// when the backend did not measure it, matching the AIRequestStats rule that an
// unmeasured value must not be shown as a real one.
std::string LatencyLabel(int64_t latencyMs) {
  if (latencyMs < 0) return "?";
  return std::to_string(latencyMs) + "ms";
}

// A fixed-precision float label. std::to_string gives six decimals, which is
// both noisy in a narrow column and implies more precision than a decision model
// has.
std::string NumberLabel(float value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  return buffer;
}

}  // namespace

SystemOneWindow::SystemOneWindow(Game *game, SystemOne::SystemOneRecorder *recorder)
    : game(game), recorder(recorder) {}

void SystemOneWindow::Draw() {
  if (game == nullptr) return;

  bool show = game->systemOneWindowEntity.has<ActiveWindow>();
  if (!show) return;

  ImGui::SetNextWindowSize(ImVec2(1080, 520), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("System 1 Overview", &show, ImGuiWindowFlags_None)) {
    if (recorder == nullptr) {
      ImGui::TextDisabled(
          "No System 1 recorder is attached, so nothing can be shown.");
      ImGui::End();
      return;
    }

    // Taken once per frame. The call sites record from a worker thread, so this
    // is a copy rather than a reference -- see SystemOneRecorder::Snapshot.
    const std::vector<SystemOne::SystemOneRecord> all = recorder->Snapshot();
    const std::unordered_map<std::string, int> counts = recorder->ScenarioCounts();
    const std::string logPath = recorder->LogFilePath();

    ImGui::Text("Calls this session: %zu", all.size());
    ImGui::SameLine();
    if (recorder->NonAdvisoryCount() > 0) {
      // Red because this is the one state that means a verdict changed behaviour.
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                         "| not advisory: %d", recorder->NonAdvisoryCount());
    } else {
      ImGui::TextDisabled("| all advisory");
    }
    ImGui::SameLine();
    if (logPath.empty()) {
      ImGui::TextDisabled("| log file: none (nothing written)");
    } else {
      ImGui::TextDisabled("| log: %s", logPath.c_str());
    }

    ImGui::TextDisabled(
        "Read-only view. Every row was recorded by a real call site; this window "
        "makes no calls of its own.");

    // --- filters -----------------------------------------------------------
    std::vector<std::string> scenarios;
    scenarios.reserve(counts.size());
    for (const auto &entry : counts) scenarios.push_back(entry.first);
    std::sort(scenarios.begin(), scenarios.end());

    const char *current =
        scenarioFilter.empty() ? "All scenarios" : scenarioFilter.c_str();
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("##scenario", current)) {
      if (ImGui::Selectable("All scenarios", scenarioFilter.empty())) {
        scenarioFilter.clear();
      }
      for (const std::string &scenario : scenarios) {
        const int count = counts.at(scenario);
        const std::string label = scenario + "  (" + std::to_string(count) + ")";
        if (ImGui::Selectable(label.c_str(), scenarioFilter == scenario)) {
          scenarioFilter = scenario;
        }
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Only calls that changed behaviour", &onlyNonAdvisory);
    ImGui::SameLine();
    ImGui::Checkbox("Follow newest", &followNewest);

    ImGui::Separator();

    // --- filtered view -----------------------------------------------------
    std::vector<const SystemOne::SystemOneRecord *> shown;
    shown.reserve(all.size());
    for (const SystemOne::SystemOneRecord &record : all) {
      if (!scenarioFilter.empty() && record.scenario != scenarioFilter) continue;
      if (onlyNonAdvisory && record.advisory) continue;
      shown.push_back(&record);
    }

    if (shown.empty()) {
      // The honest empty state. With no call site wired there is nothing to
      // show, and saying so is better than an empty table that reads as a bug.
      if (all.empty()) {
        ImGui::TextDisabled(
            "No System 1 calls recorded yet. This window shows calls made by the "
            "game; it cannot make any itself.");
        ImGui::TextDisabled(
            "Records appear once a call site uses the recorder.");
      } else {
        ImGui::TextDisabled("No records match the current filter.");
      }
      ImGui::End();
      return;
    }

    // Newest first, so index 0 is the most recent. Kept in range rather than
    // reset: a filter change should not throw away the user's selection unless
    // it actually has to.
    if (selected >= static_cast<int>(shown.size())) {
      selected = static_cast<int>(shown.size()) - 1;
    }
    if (followNewest) {
      selected = 0;
    }

    const float listWidth = 340.0f;
    ImGui::BeginChild("system1_list", ImVec2(listWidth, 0), true);
    for (int i = 0; i < static_cast<int>(shown.size()); ++i) {
      const SystemOne::SystemOneRecord &record = *shown[i];
      const std::string age = record.clockTime.empty() ? "--:--:--" : record.clockTime;
      const std::string label =
          age + "  " + record.scenario + "  " + Truncate(record.summary, 28) +
          (record.error.empty() ? "" : "  [error]") +
          (record.advisory ? "" : "  [live]") +
          // Marked so a hand-written probe is never mistaken for the game's own
          // traffic. Those calls are deliberately kept out of the log file, and
          // this is what says so in the view.
          (record.harness ? "  [probe]" : "");

      // Stable id per row: the label changes as records age out, and ImGui keys
      // selection by id.
      ImGui::PushID(i);
      if (ImGui::Selectable(label.c_str(), selected == i)) {
        selected = i;
        followNewest = false;
      }
      ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("system1_detail", ImVec2(0, 0), true);
    const SystemOne::SystemOneRecord &record = *shown[selected];

    ImGui::Text("Scenario: %s", record.scenario.c_str());
    if (!record.summary.empty()) {
      ImGui::TextWrapped("Summary: %s", record.summary.c_str());
    }
    ImGui::Text("Model: %s", record.model.empty() ? "(unlabelled)" : record.model.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("| %s | prompt %d / output %d tokens",
                        LatencyLabel(record.latencyMs).c_str(),
                        record.promptTokens, record.outputTokens);

    if (!record.error.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Error: %s",
                         record.error.c_str());
    }
    if (!record.advisory) {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                         "Not advisory: this verdict could have changed behaviour.");
    }

    // Answers as a table: the raw number is what a calibration judgement needs, and
    // a rendered sentence hides it. `answer` is the headline value, which differs
    // by primitive -- the winning option's key, the yes/no probability, or the
    // ordered score -- so it is rendered from the raw string rather than through a
    // format specifier that would print a float for a choice.
    if (!record.answers.empty()) {
      ImGui::Separator();
      ImGui::TextDisabled("Parsed answers");
      if (ImGui::BeginTable("system1_answers", 5,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("id");
        ImGui::TableSetupColumn("answer");
        ImGui::TableSetupColumn("probability");
        ImGui::TableSetupColumn("score");
        ImGui::TableSetupColumn("confidence");
        ImGui::TableHeadersRow();
        for (const auto &entry : record.answers) {
          const SystemOne::Answer &answer = entry.second;
          std::string headline;
          if (!answer.choice.empty()) {
            headline = answer.choice;
          } else if (answer.probability > 0.0f) {
            headline = NumberLabel(answer.probability);
          } else if (answer.score > 0.0f) {
            headline = NumberLabel(answer.score);
          } else {
            // Not the same as zero: the model may simply not have answered, and
            // "0.000" would read as a confident no.
            headline = "(none)";
          }

          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(entry.first.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(headline.c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%.3f", answer.probability);
          ImGui::TableNextColumn();
          ImGui::Text("%.3f", answer.score);
          ImGui::TableNextColumn();
          ImGui::Text("%.3f", answer.confidence);
        }
        ImGui::EndTable();
      }
    }

    ImGui::Separator();

    // The payload and the reply, verbatim. Read-only InputTextMultiline rather
    // than TextUnformatted so a long prompt is selectable and copyable -- hand
    // tuning a prompt begins with getting the current one out.
    //
    // Copied into mutable buffers because the ImGui entry point takes `char*`;
    // with ImGuiInputTextFlags_ReadOnly it never writes to them, and a local copy
    // avoids casting the const away from the record.
    std::string requestText = record.requestPayload;
    std::string responseText = record.responseText;

    ImGui::TextDisabled("Request");
    ImGui::InputTextMultiline("##system1_request", requestText.data(),
                              requestText.size() + 1, ImVec2(-1.0f, 120.0f),
                              ImGuiInputTextFlags_ReadOnly);

    ImGui::TextDisabled("Response");
    ImGui::InputTextMultiline("##system1_response", responseText.data(),
                              responseText.size() + 1, ImVec2(-1.0f, 120.0f),
                              ImGuiInputTextFlags_ReadOnly);

    if (ImGui::Button("Copy request and response")) {
      ImGui::SetClipboardText(
          (record.requestPayload + "\n---\n" + record.responseText).c_str());
    }

    ImGui::EndChild();
  }
  ImGui::End();

  if (!show) {
    game->systemOneWindowEntity.remove<ActiveWindow>();
    game->debugWindowState->SetShowSystemOneWindow(false);
  } else {
    game->debugWindowState->SetShowSystemOneWindow(true);
  }
}
