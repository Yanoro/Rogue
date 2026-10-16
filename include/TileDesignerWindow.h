#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <flecs.h>

#include "AI.h"
#include "TileDesigner.hpp"
#include "TileRegistry.h"
#include "Window.h"

class Game;

// A development window for talking to the tile designer agent: the model whose
// job is to invent one new tile -- a glyph, two colours and whether it blocks
// movement -- that the engine can then resolve.
//
// It exists to answer one question: can an agent extend this world's vocabulary at
// runtime? Everything else is in service of being able to see the answer. So the
// window is deliberately isolated, exactly like DesignerChatWindow and
// VisionChatWindow: it owns its own provider contextId and never touches
// NPCContext, AIRequest or the agent thread pipeline. Nothing here can disturb the
// NPC loop.
//
// The contract with the model, which TILEDESIGNER.md states and this class
// enforces:
//
//   1. It replies with prose and exactly one fenced ```json block.
//   2. That block carries a "designerNotes" string -- required, not decorative;
//      it is the model's own judgement of the tile it invented.
//   3. TileDesigner::ApplyReply extracts the block, validates it through the same
//      TileRegistry runtime-add path the headless `--tile-design` mode uses, and
//      writes it into the generated overlay. A tile that fails gets the reason
//      fed straight back in the same conversation and retried, up to
//      settings.designer.maxDesignAttempts.
//   4. A tile that passes is live immediately: it is in the catalog, so a
//      blueprint can paint with it and the material palette in the *other*
//      designer's prompt lists it on the next prompt build.
//
// Every attempt is shown, including the failures, because hiding a rejected
// attempt would make the retry look like one slow success.
//
// The one thing it does not do is generate art. A created tile is a glyph and two
// colours; the sprite field is honoured if a model volunteers one but nothing
// here asks for it or produces one. Art is hot-swapped in later.
class TileDesignerWindow : public Window {
public:
  // `designerAI` is borrowed, not owned: it is the same backend the Designer Chat
  // window uses (Game owns it for the whole run), because producing a valid
  // definition is the same skill as producing a valid design and asking for a
  // second API key to do it would be a worse trade than sharing one. A null
  // pointer is a supported state -- the backend failed to construct -- and is
  // reported in the window rather than crashing.
  TileDesignerWindow(Game *game, AI *designerAI, std::string modelLabel,
                     flecs::entity owner = flecs::entity::null());

  void Draw() override;

private:
  // One request/response attempt. A retry is a new entry, so the transcript reads
  // as the conversation that actually happened.
  struct Exchange {
    std::string DisplayAnswer(bool showThoughts, bool showJson) const;
    // What goes back to the model as its own previous turn: reasoning kept (a
    // model revising works better with its earlier thinking in view), the JSON
    // block dropped so a whole tile definition is not replayed as if fresh.
    std::string HistoryAnswer() const;
    std::string ParsedAnswer() const;

    std::string prompt;
    std::string answer;
    bool done = false;
    bool failed = false;
    std::string error;
    bool fromUser = false;

    // Outcome of applying the answer, filled on the main thread once the stream
    // finishes.
    bool inspected = false;
    bool ok = false;
    std::string failureReason;
    std::string tileName;
    std::string savedPath;
    std::string notes;
    std::vector<std::string> warnings;
    bool saveFailed = false;
    // The definition this attempt produced, for the preview pane. Null when the
    // reply never got as far as a definition.
    const TileDef *definition = nullptr;
  };

  void SendUserMessage(const std::string &prompt);
  void GenerateResponse(size_t index, std::string prompt);
  bool IsBusy();

  // Runs on the main thread once a stream has finished. Applies the reply through
  // TileDesigner and either accepts the tile or queues a retry.
  void InspectFinishedExchange(size_t index);

  // The left pane: the tile from the latest attempt, the live editor for it, and
  // the list of everything the runtime layer currently holds.
  void DrawPreviewPane();

  // The tile being shown in the editor, if any. Not a copy: it points at the
  // definition in the registry, which is what makes an in-place colour or glyph
  // tweak take effect everywhere at once. Mutable because that is exactly the
  // point -- the editor writes through it and then tells the registry.
  TileDef *edited = nullptr;
  bool editedIsRuntime = false;
  // True once the editor has changed something that is not on disk yet, so the
  // pane can say so instead of implying every edit was saved.
  bool editedIsDirty = false;

  // Editor scratch. Kept out of the definition so a half-typed glyph (an empty
  // box, a two-character paste) cannot exist in the catalog even for one frame.
  char glyphBuffer[8] = {};
  int foreground[3] = {255, 255, 255};
  int background[4] = {0, 0, 0, 0};
  bool editorBlocks = false;

  // Pulls the editor buffers back from `definition`. Called when a new tile
  // arrives, never every frame -- that would fight the user's typing.
  void LoadEditorFrom(const TileDef &definition);

  // Opens the overlay file for the edited tile again. Returns an empty string on
  // success, or the reason it failed. Writes only for runtime definitions: a
  // catalog tile is not ours to rewrite.
  std::string SaveEditedTile();

  // The art half of the pane: request generation, or show what the queue is doing.
  // Drawn under the glyph editor because it is about the *appearance* of the tile
  // that was just defined, not about the definition.
  void DrawArtSection();

  // One line describing the art queue's state, for the transcript status area and
  // the art section. Kept in one place because it is the only surface that tells
  // the owner what has been spent, and two versions of it would disagree.
  std::string DescribeArtState() const;

  // The overlay directory this window writes to, or an empty string when there is
  // none (a run with no generated layer configured). Empty means "live for this
  // session only", which is reported in the status line rather than hidden.
  std::string OverlayDirectory() const;

  // Appends to the session transcript under logs/tile-designer/. Created lazily on
  // the first message, so a run that never opens the window leaves no empty file
  // and the name carries the time of the conversation rather than of the launch.
  void Log(const std::string &text);
  void LogEvent(const std::string &text);

  Game *game = nullptr;
  AI *ai = nullptr;
  std::string modelLabel;

  // One per window instance, so this conversation cannot collide with an NPC's,
  // the blueprint designer's or the vision window's provider-side context.
  std::string contextId;

  mutable std::mutex transcriptMutex;
  std::vector<Exchange> exchanges;
  std::vector<ChatMessage> history;
  std::string systemPrompt;
  char inputBuffer[2048] = {};

  int attempts = 0;
  bool showThoughts = false;
  bool showJson = true;
  // Whether the JSON is shown in the transcript is persisted with the layout; the
  // reasoning checkbox is not, because it is a debugging aid rather than a
  // preference and should not survive a restart.
  bool layoutLoaded = false;

  // The last status line shown under the transcript, e.g. the result of an editor
  // save. Cleared by the next action so a stale success cannot be mistaken for a
  // fresh one.
  std::string statusLine;
  bool statusIsError = false;

  std::string logFilePath;
  int requestNumber = 0;
};
