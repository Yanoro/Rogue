#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include <flecs.h>

#include "AI.h"
#include "Window.h"

class Game;
struct ImDrawCmd;
struct ImDrawList;

// A development window that sends a screenshot of the game to a local vision
// model (moondream over Ollama) together with a question, and shows the answer.
//
// Why it exists: to find out whether a VLM is useful as a helper for the in-game
// agents *before* any of that machinery is built. It is therefore deliberately
// isolated -- it owns its own contextId and never touches NPCContext, AIRequest
// or the agent thread pipeline. Nothing here can disturb the NPC loop.
//
// What it sends is the framebuffer, not the world state, so what the model sees
// is what the player sees. The two capture modes differ only in *when* the read
// happens; see Draw() and OnWorldFrameRendered().
class VisionChatWindow : public Window {
public:
  // `visionAI` is borrowed: the backend is owned by Game for the whole run. A
  // null pointer is a supported state (vision disabled in settings, or the
  // backend failed to construct) and is reported inside the window rather than
  // crashing.
  VisionChatWindow(Game *game, AI *visionAI, const std::string &modelLabel,
                   flecs::entity owner = flecs::entity::null());

  void Draw() override;

  // Called by Game::Draw() at the very end of the world pass, i.e. after the
  // render texture has been blitted to the screen and before any ImGui of this
  // frame exists in the framebuffer. Only does something when a world-only
  // capture was requested during the *previous* frame's UI pass (the click is
  // only observable inside the ImGui frame, which runs after the world).
  void OnWorldFrameRendered();

private:
  enum class PendingCapture { None, WorldOnly, WholeWindow };

  // One question/answer pair. The image that went with the question is described
  // rather than kept: the transcript stays small, and the PNG on disk is the
  // record of what the model was shown.
  struct Exchange {
    bool hasImage = false;
    bool wholeWindow = false; // Which capture mode produced the image.
    std::string question;     // Empty for a capture-only entry.
    std::string answer;
    bool done = false;
    bool failed = false;
    std::string error;
    std::string savedImagePath;
    int imageWidth = 0;
    int imageHeight = 0;
    // Where this request's time went, when the backend reports it. False means
    // "not measured", not "instant": only Ollama fills this in.
    bool hasStats = false;
    AIRequestStats stats;
  };

  // ImGui invokes this during ImGui::Render(), which rlImGuiEnd() calls before
  // EndDrawing() swaps the buffers -- the only moment the framebuffer holds the
  // world *and* every ImGui window, with none of the frame left to draw.
  static void WholeWindowCaptureCallback(const ImDrawList *parentList,
                                         const ImDrawCmd *cmd);

  void Submit(bool includeQuestion);
  void RequestWholeWindowCapture();
  void PerformCapture(PendingCapture mode);

  std::string BuildPrompt() const;
  std::string BuildGameStateSummary() const;
  bool SaveCaptureToDisk(const std::vector<unsigned char> &pngBytes,
                         std::string &outPath);

  // Runs the request on a detached thread, appending to exchanges[index] as
  // tokens arrive. Mirrors AIChatWindow::generateResponse, minus the artificial
  // per-character delay: a vision answer should appear as soon as it exists.
  void GenerateResponse(std::size_t index, std::string prompt,
                        std::string imageBase64);

  void DrawSpinner();
  bool IsBusy();

  Game *game;
  AI *ai;
  std::string modelLabel;
  flecs::entity owner;

  // Unique per instance, so this conversation never shares Ollama's server-side
  // context with another window or an NPC.
  std::string contextId;

  mutable std::mutex transcriptMutex;
  std::vector<Exchange> exchanges;

  std::string questionBuffer;
  // Include the ImGui overlays in the screenshot. Off by default: a question
  // about the game world is about the world, and the window would otherwise
  // photograph itself.
  bool wholeWindow = false;
  // Append a textual summary of nearby entities to the prompt. This is the
  // control condition for judging the VLM: the same question with and without it
  // shows how much it reads from the pixels versus from the text.
  bool includeGameStateText = false;
  bool saveCaptureToDisk = true;

  PendingCapture pendingCapture = PendingCapture::None;
  // Captured at Submit() time (i.e. when the button was pressed) and consumed by
  // PerformCapture(), which for a world-only capture happens on the next frame.
  std::string pendingPrompt;
  std::string pendingQuestion;
  bool captureOnlyPending = false;
  std::size_t captureCounter = 0;

  std::string backendError;
};
