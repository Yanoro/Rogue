#include "ArtRequestsPanel.h"

#include <algorithm>
#include <vector>

#include "SpriteKinds.hpp"
#include "TileArtQueue.hpp"
#include "imgui.h"

namespace ArtRequestsPanel {

namespace {

const char *StateName(TileArtQueue::State state) {
  switch (state) {
    case TileArtQueue::State::Queued:
      return "queued";
    case TileArtQueue::State::Running:
      return "running";
    case TileArtQueue::State::Done:
      return "done";
    case TileArtQueue::State::Failed:
      return "failed";
    case TileArtQueue::State::Skipped:
      return "skipped";
  }
  return "?";
}

const char *KindName(SpriteKinds::Kind kind) {
  switch (kind) {
    case SpriteKinds::Kind::Tile:
      return "tile";
    case SpriteKinds::Kind::Object:
      return "object";
    case SpriteKinds::Kind::Actor:
      return "actor";
  }
  return "?";
}

ImVec4 StateColour(TileArtQueue::State state) {
  switch (state) {
    case TileArtQueue::State::Queued:
      return ImVec4(0.70f, 0.80f, 1.00f, 1.0f);
    case TileArtQueue::State::Running:
      return ImVec4(1.00f, 0.85f, 0.45f, 1.0f);
    case TileArtQueue::State::Done:
      return ImVec4(0.55f, 1.00f, 0.60f, 1.0f);
    case TileArtQueue::State::Failed:
      return ImVec4(1.00f, 0.50f, 0.45f, 1.0f);
    case TileArtQueue::State::Skipped:
      return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
  }
  return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

}  // namespace

void Draw(const TileArtQueue &queue, const std::string &highlight) {
  const size_t queued = queue.CountInState(TileArtQueue::State::Queued);
  const size_t running = queue.CountInState(TileArtQueue::State::Running);
  const size_t done = queue.CountInState(TileArtQueue::State::Done);
  const size_t failed = queue.CountInState(TileArtQueue::State::Failed);
  const size_t skipped = queue.CountInState(TileArtQueue::State::Skipped);
  const size_t total = queued + running + done + failed + skipped;

  const std::string unavailable = queue.UnavailableReason();
  if (!unavailable.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "Art generation is off");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", unavailable.c_str());
  } else {
    ImGui::TextDisabled("%d/%d request(s) used this session", queue.RequestsUsed(),
                        queue.GetConfig().maxRequestsPerSession);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("| queued %zu, running %zu, done %zu, failed %zu, skipped %zu",
                      queued, running, done, failed, skipped);

  if (total == 0) {
    ImGui::TextDisabled("No art requests yet in this session.");
    return;
  }

  // Newest first: the job someone is waiting on is the one they just made, and it would
  // otherwise be at the bottom of a list that grows all session.
  std::vector<TileArtQueue::Job> jobs = queue.Snapshot();
  std::reverse(jobs.begin(), jobs.end());

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingStretchProp;
  if (!ImGui::BeginTable("art_requests", 4, kFlags)) return;
  ImGui::TableSetupColumn("asset", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed, 56.0f);
  ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, 70.0f);
  ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableHeadersRow();

  for (const TileArtQueue::Job &job : jobs) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    const bool isHighlighted = !highlight.empty() && job.tileName == highlight;
    if (isHighlighted) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.5f, 1.0f));
    ImGui::TextUnformatted(job.tileName.c_str());
    if (isHighlighted) ImGui::PopStyleColor();

    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", KindName(job.kind));

    ImGui::TableNextColumn();
    ImGui::TextColored(StateColour(job.state), "%s", StateName(job.state));

    ImGui::TableNextColumn();
    // What the row can tell you, in the order it matters: why it failed, where it landed,
    // or what it is waiting for.
    if (!job.error.empty()) {
      ImGui::TextWrapped("%s", job.error.c_str());
    } else if (!job.path.empty()) {
      ImGui::TextDisabled("%s", job.path.c_str());
    } else if (job.state == TileArtQueue::State::Queued) {
      ImGui::TextDisabled("batched with anything asked for in the next %.0fs",
                          static_cast<double>(queue.GetConfig().batchDelaySeconds));
    } else {
      ImGui::TextDisabled("%s", job.conventionId.c_str());
    }
  }
  ImGui::EndTable();
}

}  // namespace ArtRequestsPanel
