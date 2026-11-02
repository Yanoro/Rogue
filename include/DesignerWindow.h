#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "raylib-cpp.hpp"

#include "DesignerSession.h"
#include "PixelDSL.hpp"
#include "TileArtQueue.hpp"
#include "Blueprint.hpp"
#include "BlueprintExpansion.hpp"
#include "BlueprintNeed.hpp"
#include "TileDesigner.hpp"
#include "ObjectDesigner.hpp"
#include "DesignerDomains.h"
#include "MapDesigner.hpp"
#include "MapPreview.hpp"
#include "RigDesigner.hpp"
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
// window is deliberately isolated, exactly like VisionChatWindow: it owns its own
// provider contextId and never touches
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
class DesignerWindow : public Window {
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
  DesignerWindow(Game *game, AI *designerAI, std::string modelLabel,
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
      DesignerDomains::Domain domain = DesignerDomains::Domain::Tile,
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

  // ---- automatic asset creation (the "Art automatically" switch) -------------------
  //
  // Runs the assets a design asked for one at a time, with no input: for each need the
  // definition turn, then (through the normal acceptance path) its sprite document and
  // compile, then the next. A refused asset is recorded and skipped rather than stopping
  // the run, because one bad request should not cost the rest.
  //
  // The order is the pipeline's own requirement, not a convenience: a sprite for a
  // template nobody can spawn is a picture of nothing, so the definition has to exist
  // first -- which is also why this cannot simply fire the document turn.
  //
  // Public because the Designer window starts it when it saves a design that asked for
  // something; it is the only caller.
  void StartAutoArt(const Blueprint &design, const std::vector<BlueprintNeed> &needs);
  bool AutoArtRunning() const { return autoRunning; }

  // "Give me art for this asset", for a caller that is not this window -- the Content
  // Editor's Regenerate button. It is the existing document turn with its existing rules:
  // an existing document is compiled as-is and the pack's copy is never overwritten; only
  // a missing one is written by the model.
  //
  // The document conversation is pumped by this window's Draw, so a caller must also make
  // the window visible before asking, or the reply is never processed. That coupling is
  // the reason this is a one-line wrapper rather than a service of its own.
  void AskForSpriteArt(const std::string &name, SpriteKinds::Kind kind);

private:
  // The two conversations this window runs. A definition turn answers "what is this";
  // a pixel-document turn answers "how is it drawn". Named constants rather than bare 0
  // and 1 because an index spelled out at its use site is an index that will eventually
  // be spelled wrong.
  static constexpr int kDefinitionConversation = 0;
  static constexpr int kArtConversation = 1;

  void SendUserMessage(const std::string &prompt);

  // ---- the pixel document turn (art.generation.source == "dsl") -------------------
  //
  // A second conversation, not a second mode: the definition turn answers "what is
  // this", this one answers "how is it drawn". Keeping the histories apart matters
  // in both directions -- a document must not be replayed into the next definition,
  // and a definition prompt must not be replayed into a document request.
  //
  // Requests the document for `name`. If one already exists (hand-authored in the
  // pack, or left by an earlier turn) it is used as-is and the art is queued; the
  // pack's copy is never overwritten.
  void RequestSpriteDocument(const std::string &name, SpriteKinds::Kind kind,
                             bool replaceGenerated = false,
                             const std::string &surroundings = std::string(),
                             const std::string &requirement = std::string());
  // The gap offer at the top of the window, when Game has one: the crop the vision pass
  // read, what it read, and the Send button. Nothing is sent until it is pressed.
  void DrawGapOffer();

  // The user turn for "create this asset", shared with the manual Send path so the two
  // cannot drift about what the model is asked. Private: only the two paths that create
  // an asset use it.
  std::string DefinitionPromptFor(const std::string &name, SpriteKinds::Kind kind,
                                  const std::string &requirement) const;

  void SendArtTurn(const std::string &prompt);
  void InspectFinishedArtDocument(size_t index,
                                  const DesignerSession::Exchange &finished);
  // True when settings say the generation source is "dsl".
  bool DslArtSourceActive() const;
  // Where a document written by this turn would go. Empty when no generated layer is
  // configured, in which case there is nowhere an agent may write.
  std::string ArtDocumentPath(const std::string &name, SpriteKinds::Kind kind) const;

  // The automatic run: the queue, how far through it we are, and what could not be made.
  // `autoStarted` is what distinguishes "this item is in flight" from "this item has not
  // begun", because the window is idle between the two.
  void PumpAutoArt();
  std::vector<BlueprintNeed> autoNeeds;
  size_t autoIndex = 0;
  bool autoRunning = false;
  bool autoStarted = false;
  int autoIdleFrames = 0;
  std::vector<std::string> autoFailures;

  TileDesigner::SpriteDocumentBrief artBrief;
  // The last compile verdict, kept for the pane: "compiled" is not the interesting
  // half, "refused at line 7 because a tile cannot be transparent" is.
  std::string artVerdict;

  // The DSL panes: the document, its compile verdict, and the sprite it produces.
  // Cached against the document's path *and* modification time, for the same reason
  // the thumbnail cache is: re-asking reuses the filename, so the path alone cannot
  // tell a new document from the previous one -- and a pane that kept showing the
  // first compile of a document the agent had since replaced would be worse than no
  // pane at all.
  // The document, its compile verdict, and the compiled sprite, for either kind.
  // `kind` decides only how the convention id and the target are derived -- the
  // panes themselves are the same file, verdict and image for a tile and an object.
  void DrawDslPanels(const std::string &name, SpriteKinds::Kind kind);
  std::string dslCacheKey;
  std::string dslText;
  PixelDSL::Result dslResult;
  bool dslCompiled = false;

  // Runs on the main thread once a stream has finished. Applies the reply through
  // TileDesigner and either accepts the tile or queues a retry.
  void InspectFinishedExchange(size_t index);

  // The left pane: the tile from the latest attempt, the live editor for it, and
  // the list of everything the runtime layer currently holds.
  void DrawPreviewPane();

  // ---- the building half ---------------------------------------------------------
  //
  // A blueprint reply is parsed, expanded and saved by the same code the Blueprints
  // window uses (BlueprintRegistry::ParseBlueprint / SaveBlueprint, BlueprintExpansion);
  // what is here is the designer's own view of the attempt -- the transcript verdict, the
  // refusal fed back to the model, and the preview of a design that was refused.
  void InspectBlueprintReply(size_t index,
                             const DesignerSession::Exchange &finished);

  // The left pane for a building: the last attempted design drawn the way the world
  // draws it, its designer notes, and the sprites it needs. Shared with the Blueprints
  // window through BlueprintPreview and SpriteGapPanel so the two previews cannot
  // disagree about what a design looks like or what it is missing.
  void DrawBlueprintPreview();

  // The tile being shown in the editor, if any. Not a copy: it points at the
  // definition in the registry, which is what makes an in-place colour or glyph
  // tweak take effect everywhere at once. Mutable because that is exactly the
  // point -- the editor writes through it and then tells the registry.
  TileDef *edited = nullptr;
  bool editedIsRuntime = false;

  // ---- which kind is being designed ------------------------------------------------
  //
  // The domain the next request is built for, and the domain the reply in flight was
  // asked for. `mode` is what the user or the router chose; `designMode` is what a reply
  // was validated as, which is the same thing unless the dropdown moves mid-flight.
  //
  // `designMode` exists because a reply is inspected on a later frame than it was
  // requested: flipping the dropdown while a request is in flight must not make the
  // reply be validated against the wrong domain's grammar -- a tile reply judged by the
  // blueprint parser, or a building judged as an object. The verdict says which domain
  // it was asked for, and that is what is honoured.
  //
  // A *domain*, not a `SpriteKinds::Kind`: one of the three is a building, which has no
  // sprite kind. Sprite code reaches the sprite half through SpriteKindOf(), and only
  // after asking IsSprite().
  DesignerDomains::Domain mode = DesignerDomains::Domain::Tile;
  DesignerDomains::Domain designMode = DesignerDomains::Domain::Tile;

  // The sprite kind for a domain, for the code paths that only sprites have. A building
  // has none, so every caller must have tested IsSprite() first; this returns Tile rather
  // than dereferencing a null, which is `DesignerDomains::SpriteOf`'s contract.
  static SpriteKinds::Kind SpriteKindOf(DesignerDomains::Domain domain);

  // Whether the next request asks for a set, and how big. Two pieces of state because they
  // come from different places: the router proposes both (`shape` and `size`), and the
  // checkbox lets a person override the shape without a classifier.
  //
  // Latched into the Exchange at send time, like `designMode`, because a reply is inspected
  // on a later frame and must be judged by the shape it was asked for.
  bool requestSet = false;
  int requestSetSize = 0;

  // The last building design that was *parsed*, whether or not it passed validation,
  // plus the expansion drawn from it.
  //
  // Kept even when the design was refused, which is the point: a design that fails
  // validation is exactly the one worth looking at, and the transcript alone does not
  // show what the model thought it was building. The expansion is stored rather than
  // recomputed because rasterising a shape set every frame for a pane that only changes
  // when a design arrives would be wasted work.
  struct PreviewedDesign {
    Blueprint blueprint;
    BlueprintExpansion::Expansion expansion;
    std::string notes;
    // The assets this design asked the world for (the reply's `needs`). Not part of the
    // saved blueprint -- stripped before saving, like the notes -- but kept here so the
    // preview can offer each one to the sprite designer.
    std::vector<BlueprintNeed> needs;
    bool accepted = false;
    std::string refusal;
  };
  std::optional<PreviewedDesign> lastDesign;

  // ---- what the left pane is showing ------------------------------------------------
  //
  // The pane used to have exactly one slot per domain -- `lastDesign`, `edited`,
  // `editedObject` -- each overwritten by the next result of that domain, and it chose
  // which to draw from `mode`. Two consequences, both reported: creating two tiles lost
  // the first from the pane, and looking at another domain meant changing the domain the
  // *next request* would be built for.
  //
  // A result is captured here when a reply is accepted, so the history holds only things
  // that exist. A refused attempt is kept separately and shown only while it is the newest
  // thing that happened: the pane has always shown refused designs on purpose, and that is
  // not the same as being part of a history you can walk.
  struct ViewedResult {
    DesignerDomains::Domain domain = DesignerDomains::Domain::Tile;
    // The design's name, for the toolbar and the list. For a set, the set's name.
    std::string label;
    bool accepted = true;

    // Blueprint: exactly what DrawBlueprintPreview draws.
    std::optional<PreviewedDesign> design;

    // Object: the copies the object pane shows, because ObjectFactory owns its templates
    // by value and a pointer into that map is invalidated by the next runtime add.
    nlohmann::json object;
    std::string objectKey;
    std::string objectOverlayPath;

    // Map: the expanded document, because a map is not a small definition and the
    // pane draws it whole. It is the only artifact that is its own file.
    std::optional<MapDesigner::Draft> map;

    // Rig: the definition plus its per-part briefs. The art is requested separately, so
    // the draft is the thing worth keeping in the history.
    std::optional<RigDesigner::Draft> rig;

    // Tile: by name, because the editor writes *through* a pointer into the registry.
    // The entry is therefore the asset's identity rather than a snapshot: re-accepting
    // "stone floor" updates this view too, which is the right answer for a name that
    // identifies one tile.
    std::string tileName;
    bool tileIsRuntime = false;
  };

  // Newest last. `viewedIndex` is what the pane draws; the arrows and the list move it.
  std::vector<ViewedResult> viewed;
  size_t viewedIndex = 0;
  // The domain the pane draws. Deliberately separate from `mode`, which is what the next
  // request is built for -- the two were the same value before, and that is precisely what
  // made "look at the blueprint without designing a building" impossible.
  DesignerDomains::Domain paneDomain = DesignerDomains::Domain::Tile;
  // A refused attempt, drawn only while it is the newest thing and only for a building
  // (the sprite panes have never shown refusals in the left pane; the transcript has them).
  std::optional<ViewedResult> refusedAttempt;

  // The map the pane is showing, and the parsed preview of it. Parsed from the draft
  // rather than from a file, and cached because a map is 4096 cells.
  std::optional<MapDesigner::Draft> mapDraft;

  // The rig draft, the queue of art requests it still owes, and the last file time seen
  // per layer (a redraw changes the file, and the texture memo has to be told).
  void DrawRigPreview();
  void QueueRigPartArt(const RigDesigner::Part &part);
  void QueueWornArt(const std::string &itemId);
  void PumpRigArtQueue();
  void RefreshRigArtMemo();
  void SaveRig();

  struct RigArtRequest {
    std::string sprite;
    std::string requirement;
    std::string label;
  };
  std::optional<RigDesigner::Draft> rigDraft;
  std::string rigError;
  std::string rigArtMessage;
  std::deque<RigArtRequest> rigArtQueue;
  std::map<std::string, std::filesystem::file_time_type> rigArtStamps;
  std::set<std::string> rigPreviewWorn;
  char rigNameBuffer[96] = {};
  bool mapPreviewValid = false;
  MapPreview::Info mapPreviewInfo;
  std::string mapPreviewError;
  float mapPreviewZoom = 0.0f;
  std::string mapSaveMessage;
  bool mapSaveIsError = false;

  // Appends an accepted result and selects it, dropping the oldest past the cap.
  void CaptureViewed(ViewedResult result);

  // Repopulates the drawing slots from `viewed[index]` and points `paneDomain` at it.
  void ShowViewed(size_t index);

  // The arrows and the "Showing:" list at the top of the left pane. Drawn inside the pane's
  // child so it cannot drift away from what it controls.
  void DrawViewedToolbar();

  // The manual domain picker. Only drawn when automatic routing is off: with it on the
  // classifier decides the domain per request, so a combo the next Send overwrites would
  // be a control that lies about what it controls.
  void DrawDomainPicker(const DesignerDomains::Descriptor &current);

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

  // The domain's descriptor for the current mode.
  const DesignerDomains::Descriptor &ModeDescriptor() const;

  // The mode name for prose and the log: "building", "tile" or "object".
  const char *ModeName() const;

  // The overlay an accepted *sprite* design of the current mode is written to, resolved
  // from the settings. Empty means "session only", which the pane reports, and also what a
  // building gets: a blueprint is saved by the blueprint registry, not into an overlay.
  std::string ModeOverlayDirectory() const;

  // The same for an explicitly named domain. Inspection uses this rather than
  // ModeOverlayDirectory, because a reply is written to the overlay of the domain it was
  // *asked* for, which the dropdown may have moved away from while the request was in
  // flight. Writing an object into the tile overlay would be a silent corruption of the
  // wrong directory.
  std::string ModeOverlayDirectoryFor(DesignerDomains::Domain domain) const;

  // Asks the classifier which domain the typed request is for. No-op when there is no
  // classifier or one is already in flight. The answer is picked up by Draw.
  //
  // `automatic` only changes what is logged: when routing is on, this runs from Send
  // rather than from a button, and a log that cannot tell the two apart cannot answer
  // "why did it design a building when I asked for a chair".
  void RequestRoute(const std::string &text, bool automatic = false);
  void ApplyRouteVerdict(const SpriteRouter::Verdict &verdict);

  // With `designer.autoRoute` on, Send classifies first and waits: the prompt has to be
  // built for the domain the classifier picks, so the request cannot go out until the
  // verdict lands. The prompt is parked here for that one frame.
  //
  // Empty means "nothing waiting". A non-empty value is also what disables Send, so two
  // requests cannot queue behind one classification.
  // Returns false when the prompt was not taken (a route is already in flight), so the
  // caller can leave the text in the box instead of eating it.
  bool BeginAutoRoute(const std::string &prompt);
  std::string autoSendPrompt;

  // The "create a being's sprite" request: the text is a description, the reply is a
  // sprite document, and the name typed beside the mode becomes its filename.
  void SendActorSpriteRequest(const std::string &description);
  char actorSpriteNameBuffer[64] = {};

  // Applies the rewritten brief for the current mode to the Improve button, so an
  // improved request is enriched inside the vocabulary it will actually be built with.
  void RefreshRewriteBrief();

  // The object half of the preview: read-only, because an object has more shape than a
  // tile and an editor for it is a larger surface than this phase needs to claim.
  void DrawObjectPreview();
  // The map domain's pane: the draft drawn with the map reload window's own preview
  // (drag, zoom, objects), and the one control that turns it into a file.
  void DrawMapPreview();

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

  // Appends to the session transcript under logs/tile-designer/. The file, Log and
  // LogEvent are DesignerSession's; this window is what the log is *about*.

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
  // (The generic log plumbing -- the file, Log and LogEvent -- is in DesignerSession.)

  Game *game = nullptr;
  std::string modelLabel;

  // The chat half: exchanges, the two conversations, the streaming handoff, the log,
  // the per-conversation attempt budgets and the transcript. Shared with the blueprint
  // window so there is one copy of the handoff that both depend on. See DesignerSession.
  std::unique_ptr<DesignerSession> session;

  char inputBuffer[2048] = {};
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

  // The splitter position and the blueprint pane's cell size, both persisted.
  //
  // `blueprintZoom` is 64 by default -- the game's native tile size, the only value at
  // which a design preview is pixel-for-pixel what the map draws. It is the half of the
  // old Designer window's layout worth keeping; the splitter is the other.
  float paneWidth = 380.0f;
  float blueprintZoom = 64.0f;

  // The last status line shown under the transcript, e.g. the result of an editor
  // save. Cleared by the next action so a stale success cannot be mistaken for a
  // fresh one.
  std::string statusLine;
  bool statusIsError = false;

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
  // Test hook; see the constructor.
  bool selfTest = false;
};
