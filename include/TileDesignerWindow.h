#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <flecs.h>

#include "raylib-cpp.hpp"

#include "AI.h"
#include "TileArtQueue.hpp"
#include "TileDesigner.hpp"
#include "ObjectDesigner.hpp"
#include "SpriteKinds.hpp"
#include "SpriteRouter.hpp"
#include "TileRegistry.h"
#include "PromptRewriter.hpp"
#include "RewriteInputBox.hpp"
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
  //
  // `selfTest` swaps the conversation for one canned response, so the whole
  // logging and inspection path can be exercised with no backend at all. It exists
  // because that path is otherwise unreachable without a live model: the log is the
  // only record of a conversation and cannot be verified by reading it. A
  // constructor parameter rather than a settings key, because it is a test hook and
  // nothing that should be configurable.
  TileDesignerWindow(Game *game, AI *designerAI, std::string modelLabel,
                     bool selfTest = false,
                     flecs::entity owner = flecs::entity::null());

  // Runs the canned conversation once and returns how many exchanges it inspected.
  // Only meaningful when constructed with `selfTest`.
  //
  // `requestArt` turns on the automatic art request for the run, so the accept-then-
  // request path can be exercised without a backend. Left false by the log test,
  // which is about the transcript and should not depend on generation being set up.
  size_t RunSelfTestConversation(
      bool requestArt = false,
      SpriteKinds::Kind kind = SpriteKinds::Kind::Tile,
      bool setTest = false);

  // What one cache lookup resolved to: the GL texture it would draw and the file
  // identity the cache is keyed on. Returned together because the texture id alone
  // cannot say whether the image was reloaded -- raylib reuses ids, so a reload can
  // land on the same number -- while a changed `stamp` is exactly "the cache accepted
  // that the file is new".
  struct ThumbnailLookup {
    unsigned int textureId = 0;
    std::filesystem::file_time_type stamp{};
    bool hasStamp = false;
  };

  // Loads `path` as a pipeline thumbnail and reports what the cache resolved. Exists
  // so the panel cache can be tested headlessly: the cache is invisible from outside,
  // and the bug it had -- keyed on the path alone, so a regenerated stage kept drawing
  // the previous batch's image, and with it the previous palette verdict and stage
  // list -- is the kind of thing that looks correct on screen until you regenerate.
  //
  // Deliberately the same entry point the panels use, so a test exercises the cache
  // rather than a copy of its logic.
  ThumbnailLookup InspectThumbnailForTest(const std::string &path);

  // The same probe for the pixel view, which had the identical stale-cache bug for
  // the placed sprite. `hash` is the fingerprint of the pixels the view currently
  // holds, so a caller can tell a reload from a cache hit without being able to read
  // the view's private pixel buffer.
  struct PixelViewLookup {
    size_t hash = 0;
    size_t distinctColours = 0;
  };
  PixelViewLookup InspectPixelViewForTest(const std::string &spritePath);

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
    // What happened about the tile's *art*, in one line, shown under the verdict.
    //
    // Exists because the feature's failure mode is silence: a tile is accepted, no
    // sprite appears, and without this the transcript looks identical whether art
    // generation is off, misconfigured, refused, or quietly working. Every value
    // here says which, and the reasons are actionable.
    std::string artNote;
    // For a set: the group's name, the entries that were accepted, and one display line per
    // entry so a partial set reads as "five of six, and here is the sixth". Kept on the
    // exchange rather than recomputed because the transcript is the record of the attempt.
    std::string setName;
    std::vector<std::string> acceptedNames;
    // One pair per entry: accepted, and the line to show. The flag is kept rather than
    // inferred from the text -- comparing a display string's prefix to decide a verdict is
    // the kind of coupling that breaks the first time the wording changes.
    std::vector<std::pair<bool, std::string>> entryVerdicts;
    // The shape this attempt asked for.
    bool wasSet = false;
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

  // ---- which kind is being designed ------------------------------------------------
  //
  // The mode the next request is built for, and the mode the last accepted design was
  // for. `mode` is what the user or the router chose; `designMode` is what a reply was
  // validated as, which is the same thing unless the user changes the dropdown mid-flight.
  //
  // `designMode` exists because a reply is inspected on a later frame than it was
  // requested: flipping the dropdown while a request is in flight must not make the
  // reply be validated against the wrong kind's grammar. The verdict says which kind it
  // was asked for, and that is what is honoured.
  SpriteKinds::Kind mode = SpriteKinds::Kind::Tile;
  SpriteKinds::Kind designMode = SpriteKinds::Kind::Tile;

  // Whether the next request asks for a set, and how big. Two pieces of state because they
  // come from different places: the router proposes both (`shape` and `size`), and the
  // checkbox lets a person override the shape without a classifier.
  //
  // Latched into the Exchange at send time, like `designMode`, because a reply is inspected
  // on a later frame and must be judged by the shape it was asked for.
  bool requestSet = false;
  int requestSetSize = 0;

  // The accepted object, as the definition rather than a pointer into the factory.
  //
  // A copy, unlike `edited`, and the difference is not an oversight: TileRegistry stores
  // its definitions behind unique_ptrs so a pointer stays valid for the run, while
  // ObjectFactory owns its templates by value in a map that the next runtime add can
  // rehash. Handing out a pointer into that would be a use-after-free waiting for the
  // second object of a session.
  nlohmann::json editedObject;
  std::string editedObjectKey;
  std::string editedObjectOverlayPath;

  // The router's verdict, once it arrives. Filled on the classifier's worker thread and
  // consumed on the main thread, so it is guarded and latched: the button starts a
  // request, `Draw` picks the answer up on a later frame.
  std::mutex routeMutex;
  std::optional<SpriteRouter::Verdict> routeVerdict;
  bool routeInFlight = false;
  std::string routeNote;

  // The kind's descriptor for the current mode, or null when there is no game.
  const SpriteKinds::Descriptor *ModeDescriptor() const;

  // The mode name for prose and the log: "tile" or "object".
  const char *ModeName() const;

  // The overlay an accepted design of the current mode is written to, resolved from the
  // settings. Empty means "session only", which the pane reports.
  std::string ModeOverlayDirectory() const;

  // The same for an explicitly named kind. Inspection uses this rather than
  // ModeOverlayDirectory, because a reply is written to the overlay of the kind it was
  // *asked* for, which the dropdown may have moved away from while the request was in
  // flight. Writing an object into the tile overlay would be a silent corruption of the
  // wrong directory.
  std::string ModeOverlayDirectoryFor(SpriteKinds::Kind kind) const;

  // Asks the classifier which kind the typed request is for. No-op when there is no
  // classifier or one is already in flight. The answer is picked up by Draw.
  void RequestRoute(const std::string &text);
  void ApplyRouteVerdict(const SpriteRouter::Verdict &verdict);

  // Applies the rewritten brief for the current mode to the Improve button, so an
  // improved request is enriched inside the vocabulary it will actually be built with.
  void RefreshRewriteBrief();

  // The object half of the preview: read-only, because an object has more shape than a
  // tile and an editor for it is a larger surface than this phase needs to claim.
  void DrawObjectPreview();

  // Convenience: the object factory, or null when there is no game.
  class ObjectFactory *Objects() const;
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

  // ---------------------------------------------------------------------------
  // Seeing the pipeline (the art section's three sub-panels)
  // ---------------------------------------------------------------------------

  // The queue's record for the tile currently in the editor, or nullopt.
  //
  // Returned by value, not as a pointer into the queue. `TileArtQueue::Snapshot()`
  // hands back a fresh vector every call, so a pointer into it dangles the moment
  // the full expression ends -- which is a heap-use-after-free the three panels
  // below then read through on the next line. A copy of one small struct is the
  // right price for not having that bug.
  std::optional<TileArtQueue::Job> JobForEditedTile() const;

  // One artifact the art pipeline produced, in pipeline order.
  struct Artifact {
    std::string label;
    std::string path;
    // Source dimensions for an image, so the filmstrip can say what it was before
    // it was scaled down to a thumbnail.
    int width = 0;
    int height = 0;
    bool isImage = false;
  };

  // The tile's position within its batch, which is what names its per-cell stage
  // images. -1 when it cannot be determined, in which case the panels show what is
  // unambiguous (the sheet, the prompt, the sprite) and omit the rest.
  int CellIndexInWorklist(const TileArtQueue::Job &job) const;

  // Every artifact produced for `job`, as label and path, in pipeline order. Empty
  // when the job never reached the worker -- nothing was written -- or when the
  // files have since been removed.
  std::vector<Artifact> ArtifactsFor(const TileArtQueue::Job &job) const;

  // What the model was actually asked, and where everything it produced lives.
  // Shown as text, not drawn: the prompt is what a bad sheet has to be judged
  // against, and nothing else surfaces it.
  void DrawArtifactsPanel(const TileArtQueue::Job &job);

  // The placed sprite drawn as one filled rectangle per pixel, so it is crisp at
  // any zoom and the palette question is answerable by eye. Includes the palette
  // with the entries this sprite uses marked, and any pixel outside it called out
  // -- the check that otherwise exists only as "0.0%" in a log line.
  void DrawPixelViewPanel(const TileArtQueue::Job &job);

  // The pipeline's stages as thumbnails: sheet, per-cell crop, trim, palette lock,
  // final 32x32. This is the panel that shows the *processing*, and it is the only
  // view of the model's raw output at any size.
  void DrawFilmstripPanel(const TileArtQueue::Job &job);

  // Loads the placed sprite into the pixel/colour cache when it is not already
  // there. Main thread only: it loads an image, which needs the renderer.
  void EnsurePixelView(const std::string &spritePath);

  // The palette the lock targets, loaded from `data/palette.json` on first use.
  // Empty when none has been derived, which is a real state and reported as one.
  void EnsurePaletteLoaded();

  // Nearest entry in `paletteColours` to `colour`, by squared RGB distance. -1 when
  // there is no palette.
  int NearestPaletteIndex(unsigned char r, unsigned char g, unsigned char b) const;

  // Loads `path` as a thumbnail texture, downscaling to about `maxSide` first so a
  // 1024px sheet does not cost a full-size texture for a 160px slot. Nearest when
  // the image is being reduced by 4x or more -- which is the pixel-art case -- and
  // bilinear otherwise, because nearest-neighbour *downscaling* of a busy texture
  // aliases into noise rather than sampling it.
  //
  // Returns nullptr when the file is missing or unreadable; the caller draws the
  // failure rather than a blank slot.
  raylib::Texture *LoadThumbnail(const std::string &path, int maxSide);

  // The cached pixel view of one sprite. Held as a texture plus a CPU-side copy of
  // the pixels, because the panels need both: the texture for nothing (the pixel
  // map is drawn from the colours) and the colours for the palette comparison.
  struct PixelView {
    std::string path;
    std::unique_ptr<raylib::Texture> texture;
    int width = 0;
    int height = 0;
    // RGBA, row major, 4 bytes per pixel.
    std::vector<unsigned char> pixels;
    // The distinct opaque colours, and how many of them are not in the palette.
    size_t distinctColours = 0;
    size_t outsidePalette = 0;
    // A cheap fingerprint of the pixel block. Its job is to be *different* for
    // different images so the cache's reload can be asserted: the path is reused by a
    // regeneration, and two readings of the same path that produce the same fingerprint
    // mean the second reading served the first image.
    size_t imageHash = 0;
  };

  // The overlay directory this window writes to, or an empty string when there is
  // none (a run with no generated layer configured). Empty means "live for this
  // session only", which is reported in the status line rather than hidden.
  std::string OverlayDirectory() const;

  // Appends to the session transcript under logs/tile-designer/. Created lazily on
  // the first message, so a run that never opens the window leaves no empty file
  // and the name carries the time of the conversation rather than of the launch.
  void Log(const std::string &text);
  void LogEvent(const std::string &text);

  // Writes the once-per-session header: backend, context, the prompt file and the
  // system prompt that was *actually sent*.
  //
  // The system prompt is the half of a request no other line records, and it is not
  // recoverable from the transcript: it is generated from the live catalog (the
  // glyph table, the tile list) plus a file that can be edited between runs. A log
  // with the replies but not the instructions cannot answer "why did it invent
  // that", which is the question a designer log exists to answer.
  void LogSessionHeader();

  // The prompt file's own text, quoted into the log, with its size and line count.
  void LogPromptFile(const std::string &path);

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
  // Ask for art as soon as a tile is accepted, instead of waiting for the button.
  //
  // Off by default and persisted with the layout: it is the one preference in this
  // window that spends money without a further click, so switching it on has to be
  // deliberate, and it should not quietly reset every launch once it is.
  bool autoRequestArt = false;
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

  // Improving the user's own description before it is sent. Shared with the Designer
  // Chat window: PromptRewriter is the general half and RewriteInputBox is the UI,
  // so this window contributes only its brief -- what the rewriter may and may not
  // promise about tiles.
  //
  // Its own provider context, so a rewrite cannot disturb the conversation's.
  PromptRewriter rewriter;
  RewriteInputBox promptBox;
  // Whether the Improve row is expanded. A checkbox rather than a permanent row, so
  // the input area stays where it was for anyone who does not use it.
  bool showRewriter = true;

  // ---- the pipeline view's caches. All main-thread only, all invalidated by
  // comparing what is being asked for against what was last loaded, because a
  // regenerated sprite changes on disk underneath a window that is still open.
  std::string pixelViewPath;
  PixelView pixelView;
  // The modification time `pixelView` was read from. A regenerated tile overwrites
  // its sprite in place, so the path alone cannot tell a new image from the cached
  // one and the pixel view would keep showing the first generation's pixels and
  // palette verdict. Zero when the file did not exist, which is a state that must
  // be retried rather than cached.
  std::filesystem::file_time_type pixelViewStamp{};
  std::vector<std::array<unsigned char, 3>> paletteColours;
  bool paletteLoaded = false;
  // Path -> thumbnail. Small and bounded (five stages per tile viewed), so it is
  // never evicted; a session that regenerates hundreds of tiles would want an LRU,
  // and that is the point at which to add one.
  std::map<std::string, std::unique_ptr<raylib::Texture>> thumbnails;
  // Path -> the modification time the cached thumbnail was loaded from. Kept beside
  // the texture rather than folded into its key so the map keeps one entry per file:
  // regenerating a tile overwrites the *same* stage filenames, so a path-only cache
  // (and equally a key that included the stamp, never erased) would keep drawing the
  // previous batch's images forever and make a fresh generation look like nothing
  // happened -- which is exactly the reported "the pipeline stages only show the old
  // tile" symptom.
  //
  // The unique_ptr above stays keyed by the bare path, because the raw pointers
  // LoadThumbnail hands out are compared against the cache elsewhere and must remain
  // valid for as long as the entry exists.
  std::map<std::string, std::filesystem::file_time_type> thumbnailStamps;
  // Which cell of the sheet the edited tile is, once its worklist has been read.
  // Cached because the panels ask per frame and the worklist is a file read.
  // Which cell of its batch a tile occupies, cached against the worklist file's
  // identity (path + mtime) because the panels ask per frame and reading the file
  // every frame would be a disk hit for a value that changes once per generation.
  //
  // `mutable` because the lookup happens inside a const method: the cache is an
  // implementation detail of answering a question, not observable state.
  mutable std::string worklistCellCacheKey;
  mutable int worklistCellIndex = -1;
  // Whether the session header has been written. The file is created on the first
  // message, so the header goes out with that message rather than in a constructor
  // that runs whether or not the window is ever used.
  bool headerLogged = false;
  // When the current request was handed to the backend. A monotonic clock, because
  // this measures a duration and wall-clock time can step.
  std::chrono::steady_clock::time_point requestStarted;
  // Test hook; see the constructor.
  bool selfTest = false;
};
