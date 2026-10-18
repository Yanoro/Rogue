#include "VisionChatWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>

#include "Components.h"
#include "Game.h"
#include "ImageEncode.h"
#include "Map.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "raylib-cpp.hpp"

namespace {

// How far out the optional game-state summary looks, and how many entities it
// names. Enough to cover the screen without turning the prompt into a database
// dump; a longer list also makes it harder to tell whether the model is reading
// the image at all.
constexpr int NEARBY_RADIUS_TILES = 14;
constexpr int NEARBY_ENTITY_LIMIT = 24;

// raylib's own tiles are drawn on top of the world before ImGui starts, so a
// world-only capture would otherwise include the FPS counter. The capture point
// in Game::Draw already sits before DrawFPS for this reason.

// One line explaining where a request's time went, ending in the phase that
// dominated. The whole point is that "slow" has three very different causes --
// a cold model, a big image, a rambling answer -- and each needs a different fix,
// so the transcript should name the culprit rather than leave it to guesswork.
std::string FormatTimings(const AIRequestStats &stats) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1);

  out << "timings: total " << stats.totalSeconds << "s"
      << " | load " << stats.loadSeconds << "s"
      << " | read " << stats.promptTokens << " tok in "
      << stats.promptEvalSeconds << "s";
  if (stats.promptEvalSeconds > 0.0 && stats.promptTokens > 0) {
    out << " (" << static_cast<double>(stats.promptTokens) /
                       stats.promptEvalSeconds
        << " tok/s)";
  }

  out << " | wrote " << stats.outputTokens << " tok in " << stats.evalSeconds
      << "s";
  if (stats.evalSeconds > 0.0 && stats.outputTokens > 0) {
    out << " (" << static_cast<double>(stats.outputTokens) / stats.evalSeconds
        << " tok/s)";
  }

  const double phases[3] = {stats.loadSeconds, stats.promptEvalSeconds,
                            stats.evalSeconds};
  const char *names[3] = {"load", "reading the image", "writing the answer"};
  int worst = 0;
  for (int i = 1; i < 3; ++i) {
    if (phases[i] > phases[worst]) {
      worst = i;
    }
  }
  if (phases[worst] > 0.0) {
    out << "  -> mostly " << names[worst];
  }

  return out.str();
}

} // namespace

VisionChatWindow::VisionChatWindow(Game *game, AI *visionAI,
                                   const std::string &modelLabel,
                                   flecs::entity owner)
    : game(game), ai(visionAI), modelLabel(modelLabel), owner(owner),
      contextId("vision-chat-" +
                std::to_string(reinterpret_cast<uintptr_t>(this))) {
  if (ai == nullptr) {
    backendError = "No vision backend. Enable and configure the \"vision\" "
                   "section in settings.json, and make sure Ollama is running.";
  }
}

void VisionChatWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(660, 560), ImGuiCond_FirstUseEver);
  ImGui::Begin("Vision Chat", &isOpen, ImGuiWindowFlags_None);

  if (!isOpen) {
    if (owner.is_alive()) {
      owner.remove<ActiveWindow>();
    }
    // Remember the close the way the other debug windows do, so the toggle in
    // the debug menu and the state file agree with what is on screen.
    game->debugWindowState->SetShowVisionChatWindow(false);
    ImGui::End();
    return;
  }

  const bool busy = IsBusy();

  ImGui::TextDisabled("Vision model: %s",
                      modelLabel.empty() ? "(none)" : modelLabel.c_str());
  if (!backendError.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                       backendError.c_str());
  }
  ImGui::Separator();

  // Everything below the transcript, as a row count: the status line, the
  // capture-mode radios, the checkboxes, and the prompt/buttons row. The child
  // below is sized as "whatever is left after these", so this constant and the
  // widgets after the child have to stay in step -- reserving one row too few is
  // what clips the text field and buttons in half. GetFrameHeightWithSpacing()
  // already includes one ItemSpacing, hence the single extra one for the gap
  // between the child and the first row (same convention as AIChatWindow).
  constexpr int FOOTER_ROWS = 4;
  const float footerHeightToReserve =
      ImGui::GetStyle().ItemSpacing.y +
      ImGui::GetFrameHeightWithSpacing() * static_cast<float>(FOOTER_ROWS);

  if (ImGui::BeginChild("VisionTranscript", ImVec2(0, -footerHeightToReserve),
                        ImGuiChildFlags_Borders)) {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (exchanges.empty()) {
      ImGui::TextWrapped(
          "Type a question about the current screen and press Send. "
          "\"Capture only\" writes what the model would be shown to disk "
          "without calling it.");
    }
    for (std::size_t i = 0; i < exchanges.size(); ++i) {
      const Exchange &ex = exchanges[i];
      ImGui::PushID(static_cast<int>(i));

      if (ex.question.empty()) {
        ImGui::TextDisabled("[capture only]");
      } else {
        ImGui::TextWrapped("Q: %s", ex.question.c_str());
      }

      if (ex.hasImage) {
        std::string meta = "   image " + std::to_string(ex.imageWidth) + "x" +
                           std::to_string(ex.imageHeight) +
                           (ex.wholeWindow ? " (whole window)" : " (world only)");
        if (!ex.savedImagePath.empty()) {
          meta += "  -> " + ex.savedImagePath;
        }
        ImGui::TextDisabled("%s", meta.c_str());
      }

      if (ex.hasStats) {
        // Wrapped with the disabled colour rather than TextDisabled, which does
        // not wrap: the line is long and the window is resizable.
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", FormatTimings(ex.stats).c_str());
        ImGui::PopStyleColor();
      }

      if (ex.failed) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                           ex.error.c_str());
      } else if (ex.answer.empty()) {
        if (!ex.done) {
          ImGui::TextDisabled("A: (thinking)");
        }
      } else {
        ImGui::TextWrapped("A: %s", ex.answer.c_str());
      }

      ImGui::Separator();
      ImGui::PopID();
    }
  }
  ImGui::EndChild();

  if (busy) {
    ImGui::Text("Waiting for the vision model...");
    ImGui::SameLine();
    DrawSpinner();
  } else {
    ImGui::TextDisabled("Each Send takes a fresh screenshot.");
  }

  ImGui::BeginDisabled(ai == nullptr || busy);

  int captureMode = wholeWindow ? 1 : 0;
  ImGui::RadioButton("World only", &captureMode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Whole window", &captureMode, 1);
  wholeWindow = (captureMode == 1);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("World only photographs the game itself, before any UI is "
                      "drawn. Whole window includes every open ImGui window, "
                      "including this one.");
  }

  ImGui::Checkbox("Add text summary of nearby entities",
                  &includeGameStateText);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Appends positions and names to the prompt. Compare answers "
                      "with this on and off to see whether the model reads the "
                      "image or the text.");
  }
  ImGui::SameLine();
  ImGui::Checkbox("Save capture", &saveCaptureToDisk);

  const bool enterPressed = ImGui::InputTextWithHint(
      "##VisionQuestion", "Ask about what is on screen...", &questionBuffer,
      ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  const bool sendPressed = ImGui::Button("Send");
  ImGui::SameLine();
  const bool capturePressed = ImGui::Button("Capture only");

  ImGui::EndDisabled();

  if ((sendPressed || enterPressed) && !questionBuffer.empty()) {
    Submit(true);
  } else if (capturePressed) {
    Submit(false);
  }

  ImGui::End();

  // The whole-window capture has to happen after every ImGui command list has
  // been recorded, including this window's own, or the image would show a
  // half-drawn UI. Deferring it to the foreground draw list runs it inside
  // ImGui::Render() (called by rlImGuiEnd), which is after all windows are
  // recorded and before EndDrawing() swaps the buffers.
  if (pendingCapture == PendingCapture::WholeWindow) {
    ImGui::GetForegroundDrawList()->AddCallback(
        &VisionChatWindow::WholeWindowCaptureCallback, this);
  }

  game->debugWindowState->SetShowVisionChatWindow(true);
}

void VisionChatWindow::WholeWindowCaptureCallback(const ImDrawList *,
                                                  const ImDrawCmd *cmd) {
  auto *self = static_cast<VisionChatWindow *>(cmd->UserCallbackData);
  if (self != nullptr && self->pendingCapture == PendingCapture::WholeWindow) {
    self->PerformCapture(PendingCapture::WholeWindow);
  }
}

void VisionChatWindow::OnWorldFrameRendered() {
  if (pendingCapture == PendingCapture::WorldOnly) {
    PerformCapture(PendingCapture::WorldOnly);
  }
}

void VisionChatWindow::Submit(bool includeQuestion) {
  if (ai == nullptr) {
    backendError = "No vision backend. Enable and configure the \"vision\" "
                   "section in settings.json, and make sure Ollama is running.";
    return;
  }
  // A capture is already queued: the framebuffer has not been read yet, so
  // starting a second one would only photograph the same frame twice.
  if (pendingCapture != PendingCapture::None) {
    return;
  }

  backendError.clear();
  captureOnlyPending = !includeQuestion;
  pendingQuestion = includeQuestion ? questionBuffer : std::string{};

  if (includeQuestion) {
    pendingPrompt = BuildPrompt();
    questionBuffer.clear();
  } else {
    pendingPrompt.clear();
  }

  if (wholeWindow) {
    // Fires later in this same frame, during ImGui::Render().
    pendingCapture = PendingCapture::WholeWindow;
  } else {
    // Game::Draw() picks this up at the end of the next world pass, which is the
    // only point in the frame where the world is on screen and no ImGui is.
    pendingCapture = PendingCapture::WorldOnly;
  }
}

void VisionChatWindow::PerformCapture(PendingCapture mode) {
  pendingCapture = PendingCapture::None;

  const bool whole = (mode == PendingCapture::WholeWindow);
  // An empty rectangle means "the whole framebuffer". World-only crops to the
  // rectangle the game actually blits, which also drops the black border on a
  // screen larger than the 1920x1080 render target.
  const Rectangle region = whole ? Rectangle{0.0f, 0.0f, 0.0f, 0.0f}
                                 : game->GetWorldScreenRect();

  CapturedImage captured;
  if (!CaptureScreenImage(captured, region)) {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    backendError = "Screen capture failed: the framebuffer was empty.";
    pendingPrompt.clear();
    pendingQuestion.clear();
    captureOnlyPending = false;
    return;
  }

  Exchange ex;
  ex.hasImage = true;
  ex.wholeWindow = whole;
  ex.question = pendingQuestion;
  ex.imageWidth = captured.width;
  ex.imageHeight = captured.height;
  ex.done = true; // Flipped back below when a request is actually launched.
  if (saveCaptureToDisk) {
    SaveCaptureToDisk(captured.pngBytes, ex.savedImagePath);
  }

  std::size_t index = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    exchanges.push_back(std::move(ex));
    index = exchanges.size() - 1;
  }

  if (!captureOnlyPending) {
    {
      std::lock_guard<std::mutex> lock(transcriptMutex);
      exchanges[index].done = false;
    }
    // Deliberately outside the lock: the worker may reach its first callback
    // immediately, and there is no reason to make it wait while we spawn it.
    GenerateResponse(index, pendingPrompt, captured.base64Png);
  }

  pendingPrompt.clear();
  pendingQuestion.clear();
  captureOnlyPending = false;
}

std::string VisionChatWindow::BuildPrompt() const {
  if (!includeGameStateText) {
    return questionBuffer;
  }
  return BuildGameStateSummary() + "\nQuestion: " + questionBuffer;
}

std::string VisionChatWindow::BuildGameStateSummary() const {
  std::ostringstream out;
  out << "Game state summary (text, for reference):\n";

  const GamePosition *playerPos = nullptr;
  if (game->playerEntity.is_alive()) {
    playerPos = game->playerEntity.get<GamePosition>();
  }

  if (playerPos != nullptr) {
    out << "- The player is at tile (" << playerPos->x << ", " << playerPos->y
        << ").\n";
  }

  if (game->map) {
    const float tileW = static_cast<float>(game->map->GetTileWidth());
    const float tileH = static_cast<float>(game->map->GetTileHeight());
    const Rectangle worldRect = game->GetWorldScreenRect();
    if (tileW > 0.0f && tileH > 0.0f && worldRect.width > 0.0f) {
      const GamePosition topLeft = game->map->ScreenCoordsToGameCoords(
          worldRect.x, worldRect.y);
      const GamePosition bottomRight = game->map->ScreenCoordsToGameCoords(
          worldRect.x + worldRect.width, worldRect.y + worldRect.height);
      out << "- The screenshot covers roughly tiles x " << topLeft.x << ".."
          << bottomRight.x << ", y " << topLeft.y << ".." << bottomRight.y
          << ".\n";
    }

    // The names the character has, not the map's: what a place is called is
    // something you know, and you can be wrong about it.
    std::vector<std::string> remembered;
    if (game->playerEntity.is_alive()) {
      if (const SpatialMemory *memory =
              game->playerEntity.get<SpatialMemory>()) {
        for (const KnownPlace &place : memory->places) {
          if (!place.labels.empty()) {
            remembered.push_back(place.labels.front());
          }
        }
      }
    }
    if (!remembered.empty()) {
      out << "- Places you remember: ";
      for (std::size_t i = 0; i < remembered.size(); ++i) {
        if (i > 0) {
          out << ", ";
        }
        out << remembered[i];
      }
      out << ".\n";
    }
  }

  if (playerPos != nullptr) {
    struct NearbyEntity {
      std::string label;
      int x;
      int y;
      int distance;
      bool isCharacter;
    };
    std::vector<NearbyEntity> nearby;

    game->ecs.filter<DisplayName, GamePosition>().each(
        [&](flecs::entity entity, const DisplayName &name,
            const GamePosition &pos) {
          if (entity == game->playerEntity) {
            return;
          }
          // Chebyshev distance, because movement and adjacency are tile-based.
          const int distance =
              std::max(std::abs(pos.x - playerPos->x),
                       std::abs(pos.y - playerPos->y));
          if (distance > NEARBY_RADIUS_TILES) {
            return;
          }
          std::string label = name.name;
          if (label.empty()) {
            label = entity.name().c_str() ? entity.name().c_str() : "unnamed";
          }
          nearby.push_back(
              {label, pos.x, pos.y, distance, entity.has<CharacterTag>()});
        });

    std::sort(nearby.begin(), nearby.end(),
              [](const NearbyEntity &a, const NearbyEntity &b) {
                return a.distance < b.distance;
              });
    if (nearby.size() > NEARBY_ENTITY_LIMIT) {
      nearby.resize(NEARBY_ENTITY_LIMIT);
    }

    if (!nearby.empty()) {
      out << "- Entities within " << NEARBY_RADIUS_TILES << " tiles:\n";
      for (const NearbyEntity &entity : nearby) {
        out << "  - " << entity.label
            << (entity.isCharacter ? " (character)" : "") << " at ("
            << entity.x << ", " << entity.y << "), " << entity.distance
            << " tiles away.\n";
      }
    }
  }

  return out.str();
}

bool VisionChatWindow::SaveCaptureToDisk(
    const std::vector<unsigned char> &pngBytes, std::string &outPath) {
  if (pngBytes.empty()) {
    return false;
  }

  std::time_t now = std::time(nullptr);
  std::tm localTime{};
#if defined(_WIN32)
  localtime_s(&localTime, &now);
#else
  localtime_r(&now, &localTime);
#endif
  char stamp[32] = {0};
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &localTime);

  std::error_code error;
  std::filesystem::create_directories("logs", error);

  std::ostringstream path;
  // The counter keeps two captures in the same second from overwriting each
  // other, which is easy to do while probing the model.
  path << "logs/vision_capture_" << stamp << "_" << ++captureCounter << ".png";

  std::ofstream file(path.str(), std::ios::binary);
  if (!file) {
    return false;
  }
  file.write(reinterpret_cast<const char *>(pngBytes.data()),
             static_cast<std::streamsize>(pngBytes.size()));
  if (!file) {
    return false;
  }

  outPath = path.str();
  return true;
}

void VisionChatWindow::GenerateResponse(std::size_t index, std::string prompt,
                                        std::string imageBase64) {
  AI *backend = ai;
  if (backend == nullptr) {
    return;
  }

  // The worker keeps the game loop responsive for providers whose
  // generateStream blocks. OllamaAI's does not -- it returns immediately and
  // streams from its own thread -- which is why completion is signalled by the
  // empty-token sentinel and never by this function returning.
  std::thread([this, index, prompt = std::move(prompt),
               imageBase64 = std::move(imageBase64), backend]() {
    std::vector<ChatMessage> history = {{"user", prompt}};
    ImageList images;
    if (!imageBase64.empty()) {
      images.push_back(imageBase64);
    }

    // Every message is judged on its own screenshot, so it must not be read in
    // the light of the previous one: a remembered context would describe an image
    // the model can no longer see, which is worse than no context at all. Only
    // one request runs at a time (the UI disables itself while busy), so this is
    // the safe moment to clear it.
    backend->resetContext(contextId);

    const bool delivered = backend->generateStream(
        contextId, history,
        [this, index, backend](const std::string &token) {
          // Read the stats before taking the transcript lock. The worker thread
          // publishes them under its own mutex, so nesting the two locks here
          // would invite a deadlock for no benefit -- and only the end-of-stream
          // sentinel (an empty token) is guaranteed to have them.
          AIRequestStats stats;
          const bool haveStats =
              token.empty() && backend->getLastStats(contextId, stats);

          std::lock_guard<std::mutex> lock(transcriptMutex);
          if (index >= exchanges.size()) {
            return;
          }
          Exchange &ex = exchanges[index];
          if (token.empty()) {
            // Every provider uses an empty token as its end-of-stream sentinel.
            ex.done = true;
            if (haveStats) {
              ex.hasStats = true;
              ex.stats = stats;
            }
            if (ex.answer.empty()) {
              ex.failed = true;
              ex.error = "No answer came back. Check that Ollama is running and "
                         "that the model tag is pulled (ollama list).";
            }
          } else {
            ex.answer += token;
          }
        },
        {}, images);

    if (!delivered) {
      std::lock_guard<std::mutex> lock(transcriptMutex);
      if (index < exchanges.size()) {
        Exchange &ex = exchanges[index];
        ex.done = true;
        ex.failed = true;
        ex.error = "The vision backend could not accept the request.";
      }
    }
  }).detach();
}

bool VisionChatWindow::IsBusy() {
  std::lock_guard<std::mutex> lock(transcriptMutex);
  for (const Exchange &ex : exchanges) {
    if (!ex.done) {
      return true;
    }
  }
  return false;
}

void VisionChatWindow::DrawSpinner() {
  ImDrawList *drawList = ImGui::GetWindowDrawList();
  ImVec2 pos = ImGui::GetCursorScreenPos();
  const float radius = 8.0f;
  pos.x += radius;
  pos.y += radius;
  const float time = static_cast<float>(raylib::Window::GetTime());
  for (int i = 0; i < 8; i++) {
    const float angle = time * 4.0f + i * (3.14159265f / 4.0f);
    const float alpha = static_cast<float>(i) / 8.0f;
    drawList->AddCircleFilled(
        ImVec2(pos.x + cosf(angle) * radius, pos.y + sinf(angle) * radius), 2.0f,
        ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)));
  }
  ImGui::Dummy(ImVec2(radius * 2 + 5, radius * 2));
}
