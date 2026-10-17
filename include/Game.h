#pragma once
#include "InputHandler.h"
#include "Map.h"
#include "raylib-cpp.hpp"
#include <filesystem>
#include <flecs.h>
#include <memory>
#include <string>
#include <vector>

#include "Components.h"
#include "AsciiArtFonts.hpp"
#include "DebugLog.h"
#include "DebugWindowState.h"
#include "Defaults.h"
#include "MapReloader.h"
#include "AgentBrain.h"
#include "Blueprint.hpp"
#include "BlueprintRegistry.h"
#include "Hooks.hpp"
#include "ItemRegistry.h"
#include "RaceRegistry.h"
#include "RecipeRegistry.h"
#include "ResourceManager.h"
#include "SkillRegistry.h"
#include "SpriteKinds.hpp"
#include "TileArtQueue.hpp"
#include "SpeechQueue.h"
#include "StatRegistry.h"
#include "SystemOneProfiles.hpp"
#include "SystemOneRecorder.h"
#include "TaskKindRegistry.h"
#include "TextToSpeech.h"
#include "TileRegistry.h"

enum class EditorSelectionType {
  None,
  Tile,
  ObjectTemplate
};

struct EditorSelection {
  EditorSelectionType type = EditorSelectionType::None;
  std::string name;
};

// A rectangle the user has dragged out in the map editor that is waiting to be
// named before it becomes a Location. Only alive while pendingLocation is true.
struct PendingLocation {
  GamePosition topLeft{0, 0};
  GamePosition bottomRight{0, 0};
  std::string name;
  std::string description;
};

// One cell of a blueprint's preview or placement: which cell it lands on, what
// it paints or places, and whether that cell is a legal site right now. The
// legality is what the debug window tints green or red, so the user sees why a
// site is refused before clicking rather than after.
struct BlueprintGhostCell {
  GamePosition position{0, 0};
  std::string tile;    // tile name to paint, empty when it places or carves
  std::string object;  // object template to spawn, empty otherwise
  bool carve = false;
  bool legal = true;
  // Why the cell is illegal, for the window's report. Empty when legal.
  std::string reason;
};

// A blueprint expanded onto the map at one anchor, with a per-cell legality
// verdict. Produced by Game::BuildBlueprintPlacement and consumed by both the
// mouse-following preview and the click that commits it, so the picture and the
// placement cannot disagree about what a click will do.
struct BlueprintPlacement {
  std::vector<BlueprintGhostCell> cells;
  bool allLegal = true;
  std::string summary;
};

class Game {
public:
  Game();
  ~Game();

  // Brings up the window, the debug systems and the ECS world using the settings
  // loaded by LoadGameSettings (see Settings.h). Reads them through Settings()
  // rather than taking a copy, so there is one source of truth for the run.
  void Init();
  void handleInput();

  // Rewrites the map file with every map-editor change: tile layout, locations
  // and authored objects. Returns false (and logs) if the file cannot be
  // written.
  bool SaveMapToFile();

  void UpdateGUI();
  void Update();

  void DrawGameWindows();
  void Draw();

  void BeginDrawingGame();
  void EndDrawingGame();

  // The on-screen rectangle the world is blitted into: the 1920x1080 render
  // target, clipped to the window. It is what the Vision Chat window crops to
  // for a "world only" screenshot, and what a future region-select tool would
  // clamp its selection to.
  Rectangle GetWorldScreenRect() const;

  void Shutdown();

  // Spawns an NPC from its authored map data: name, background, starting skills,
  // body slots/race and starting equipment. Passing the whole NPCData keeps a
  // new authored field from needing a new parameter here.
  flecs::entity createNPC(const NPCData &data);

  bool shouldClose() const;

  raylib::Window *getWindow() { return &window; }

  // The designer's system prompt: its instructions from settings.designer.promptFile
  // (data/blueprints/DESIGNER.md by default) with the material palette appended,
  // generated from the live tile catalog and object templates.
  //
  // The palette is generated rather than written into the prompt file because it
  // has to match the registry exactly. A hand-written list drifts the moment a
  // tile is added, and the failure is quiet: the designer asks for a material
  // that does not exist and every design is refused.
  std::string BuildDesignerSystemPrompt() const;

  // The tile designer's system prompt: settings.content.tileDesignerPromptFile
  // (data/tiles/TILEDESIGNER.md by default) with the glyph table and the tile
  // list substituted for %GLYPHS% and %TILES%.
  //
  // Generated rather than written into the prompt file for the same reason the
  // material palette is: the glyph table has to match the live catalog, and a
  // hand-written one drifts the moment a tile is created -- which then has an
  // agent pick a glyph that is already in use, or a name that is already taken.
  std::string BuildTileDesignerSystemPrompt() const;

  // The same prompt for an arbitrary sprite kind: the tile catalog's glyphs and names
  // for a tile, the object factory's capabilities for an object.
  //
  // Generated rather than written into the prompt file for the same reason the tile
  // one is: the vocabulary has to match the live world, and a hand-written list drifts
  // the moment a tile or a component is added -- and the failure is quiet, because the
  // model then designs against a vocabulary that no longer exists.
  std::string BuildSpriteSystemPrompt(
      SpriteKinds::Kind kind,
      bool set = false) const;

  // Whether an object template is loaded in the *running game's* factory, and whether it
  // came from the generated overlay or from the hand-authored catalog.
  //
  // This exists because the game's own report of loading the overlay goes through
  // DebugLog, which is an in-memory ring buffer that nothing prints -- so "the overlay
  // loaded" was unverifiable from outside the process. A check that reads the live factory
  // after Init is the honest version, and it is the only way to tell a wired overlay load
  // from a log line nobody can see. Used by `--object-check <key>`.
  struct ObjectLookup {
    bool found = false;
    bool fromGeneratedOverlay = false;  // false when hand-authored or absent
    std::string name;
  };
  ObjectLookup LookupObject(const std::string &key) const;

  // The template keys the running factory holds, sorted. Reported alongside a lookup so a
  // near-miss ("did the overlay load at all?") is answerable without guessing.
  std::vector<std::string> ObjectKeys() const;

  // settings.designer.maxDesignAttempts. Read by the designer window so the cap
  // lives in one place.
  int maxDesignAttempts() const;

private:
  raylib::Window window;
  std::unique_ptr<Map> map;
  ObjectFactory objectFactory;
  RecipeRegistry recipeRegistry;
  StatRegistry statRegistry;
  SkillRegistry skillRegistry;
  ItemRegistry itemRegistry;
  RaceRegistry raceRegistry;
  HookRegistry hookRegistry;
  TileRegistry tileRegistry;
  BlueprintRegistry blueprintRegistry;
  TaskKindRegistry taskKindRegistry;

  // Sprite textures for the whole process, keyed by path and cached including
  // failures (see ResourceManager). Declared before `ecs` on purpose: members are
  // destroyed in reverse order, so the ECS world -- whose Sprite components name
  // these paths -- goes first and nothing can outlive the cache it refers to.
  ResourceManager resourceManager;

  flecs::world ecs;
  flecs::entity renderPipeline;
  flecs::entity playerEntity;

  flecs::entity selectedTile;
  bool hasClicked = false;
  bool validTileSelected = false;
  EditorSelection editorSelection;
  GamePosition lastClickedPos{0, 0};
  double lastLeftClickTime = 0.0;

  // Location drawing (map editor). Armed from the Map Editor window, then a
  // left-button drag marks the tiles and the release opens the naming prompt.
  bool createLocationMode = false;
  bool isDraggingLocation = false;
  GamePosition locationDragStart{0, 0};
  GamePosition locationDragEnd{0, 0};
  PendingLocation pendingLocation;
  bool pendingLocationWantsFocus = false;
  bool pendingLocationHasFocus = false;

  // Location removal (map editor). Armed from the Map Editor window, then a
  // left click deletes the location under the cursor. In-memory only.
  bool removeLocationMode = false;
  std::string lastRemovedLocationName;

  // Object and tile removal, the "trash" mode (map editor). Armed from the Map
  // Editor window, then a left click deletes the authored object on that tile,
  // or -- when nothing is placed there -- the tile itself. The object is taken
  // first so clearing a tree never also erases the ground under it.
  bool removeObjectMode = false;
  std::string lastRemovedObjectName;
  std::string lastRemovedTileName;

  // Blueprint placement (the Blueprint window). Non-empty means a design is
  // selected and the mouse is carrying its preview; a left click on a legal site
  // commits it. Empty means the mode is off, which is how the window deselects.
  //
  // Held here rather than in the window because Game::handleInput is what sees
  // the mouse, exactly like the map editor's selection.
  std::string selectedBlueprintId;
  // Where the preview's origin currently sits, in game coordinates. Recomputed
  // every frame from the mouse so the ghost stays under the cursor.
  GamePosition blueprintGhostOrigin{0, 0};
  // Persist painted terrain to the map file when the design is placed. Objects
  // are always marked MapAuthored; this checkbox covers the tiles, which is the
  // half that needs a save to survive. Off by default: a debug placement is
  // usually throwaway, and writing the map on every experiment is not.
  bool blueprintPersistTerrain = false;
  // Result of the last placement, shown in the window.
  std::string lastBlueprintPlacementMessage;

  // File the current map was loaded from, needed to write map edits back.
  // Refreshed on every LoadMap so the save always targets the map on screen.
  std::string mapFilePath;
  // Result of the last "Save Map to File" press, shown in the Map Editor window.
  std::string lastMapSaveMessage;

  raylib::RenderTexture2D gameTexture;

  Font gameFont;
  // One rasterization of gameFont per art cell size, so an art cell is drawn at
  // the size it was rasterized at instead of being a downhill resample of the
  // 32px gameFont. See AsciiArtFonts.hpp.
  AsciiArtFonts artFonts;
  size_t virtualWidth;
  size_t virtualHeight;

  flecs::entity contextMenuTarget;
  bool openContextMenu = false;
  Vector2 contextMenuScreenPos;

  // Camera settings for selection
  std::vector<std::string> availableFontPaths;
  int selectedFontIndex = 0;
  // Astar related variables (Mainly drawing related)
  GamePosition astarStartPos{0, 0};
  GamePosition astarEndPos{0, 0};
  std::vector<GamePosition> astarPath;
  bool isSelectingAStarPath = false;
  int astarClickCount = 0;
  int currentMonitor = 0;
  bool isSettingPlayerTarget = false;
  Acceleration lastAccel = {};
  Velocity lastVel = {};

  raylib::Camera2D camera;
  GameCameraMode cameraMode;

  // Debug window entities in ECS
  flecs::entity debugConsoleWindowEntity;
  flecs::entity tileInfoWindowEntity;
  flecs::entity astarWindowEntity;
  flecs::entity entityOverviewWindowEntity;
  flecs::entity debugLogWindowEntity;
  flecs::entity mapReloadWindowEntity;
  flecs::entity drawAsciiToggleWindowEntity;
  flecs::entity fontSelectionWindowEntity;
  flecs::entity mapEditorWindowEntity;
  flecs::entity blueprintWindowEntity;
  flecs::entity aiMenuWindowEntity;
  flecs::entity npcMenuWindowEntity;
  flecs::entity taskBoardWindowEntity;
  flecs::entity systemOneWindowEntity;
  // Stage 0 of docs/spatial-memory-design.md: holds the facing-marker checkbox
  // and the counts that prove every character got a direction.
  flecs::entity visionDebugWindowEntity;

  // The Vision Chat window is created once and kept alive for the whole run
  // rather than rebuilt whenever it is toggled on, unlike the other debug
  // windows: a rebuilt window would lose its transcript, and Game::Draw needs a
  // stable pointer to hand the world-only capture to. ActiveWindow holds the same
  // shared_ptr, so it must never be the only owner.
  std::shared_ptr<class VisionChatWindow> visionChatWindow;
  flecs::entity visionChatWindowEntity;
  // Second, independent backend for that window (see Settings.h: VisionSection).
  // Kept alive here so the window can borrow a raw pointer safely.
  std::unique_ptr<AI> visionAI;

  // The designer's backend and window, for the same reasons as the vision pair:
  // the backend is owned for the whole run and the window is created once so its
  // conversation survives a toggle.
  std::unique_ptr<AI> designerAI;
  // The backend behind the Improve button, shared by every window that offers it.
  //
  // Its own instance even when it uses the designer's model settings, for two reasons:
  // OllamaAI keys conversation context by contextId alone, so one instance cannot
  // serve two models; and the rewriter wants reasoning *off*, because a short rewrite
  // has no use for a chain of thought and a backend that spends its completion budget
  // on one returns no answer at all (see Settings.h: RewriterSection). Null when no
  // backend could be built, which the windows report rather than crash on.
  std::unique_ptr<AI> rewriterAI;
  std::shared_ptr<class DesignerChatWindow> designerWindow;
  flecs::entity designerWindowEntity;

  // The tile designer's window. Same lifetime rules as the pair above: created
  // once and kept so a tile conversation survives a toggle, borrowing
  // `designerAI` with its own provider context. It owns no backend of its own --
  // see the comment where it is constructed in Game::ECSInit.
  std::shared_ptr<class TileDesignerWindow> tileDesignerWindow;
  flecs::entity tileDesignerWindowEntity;

  // Live tile-art generation. Owned by Game rather than held in file-scope globals
  // the way the AI path's response queue is (see TileArtQueue.hpp and
  // .agents/known-issues.md #13): the queue is per-world by construction, so a
  // second `flecs::world` cannot share one by accident.
  //
  // Declared here, next to the windows that use it, and configured in ECSInit. The
  // worker thread it may own is joined in Shutdown, before the ECS goes down, or a
  // batch in flight would outlive the world its completion touches.
  TileArtQueue tileArtQueue;
  // Set once at Init. Not read anywhere else, so it is not exposed.
  bool tileArtCompletionWired = false;

  // Set by `--tile-designer-logtest` before Init, which constructs the tile designer
  // window in its self-test mode so the conversation logging can be exercised with
  // no backend. Public: main() sets it before Init runs, and the rest of the class
  // below this point stays private.
public:
  bool tileDesignerLogSelfTest = false;
  // Which kind the canned self-test conversation designs for. A member rather than a
  // parameter because the test hook is set from main() before Init, and the window is
  // constructed later inside ECSInit.
  SpriteKinds::Kind tileDesignerSelfTestKind = SpriteKinds::Kind::Tile;
  // Whether the canned conversation asks for a *set*. Separate from the kind because the two
  // axes are independent and the point of the set case is the per-entry verdicts.
  bool tileDesignerSelfTestSet = false;
  // Set by `--tile-designer-arttest`, which is the same canned conversation with the
  // window's automatic art request switched on -- so accepting a tile and asking for
  // its art in one step is exercised without a backend and without spending
  // anything, provided art.generation.command is a stub.
  bool tileDesignerArtSelfTest = false;

private:

  // Reading the designer's reply aloud. Owned for the whole run for the same
  // reason as the pair above, and borrowed by the window through `game`.
  //
  // `speechBackend` is null when designer.tts.enabled is false, or when the piper
  // binary or the voice model could not be found. That is a supported state -- the
  // window says so in its status line instead of the game failing to start, which
  // is the same treatment a null designer backend already gets.
  std::unique_ptr<TextToSpeech> speechBackend;
  std::unique_ptr<SpeechQueue> speechQueue;

  // Why speech is unavailable, when it is. Empty means either it is working or it
  // was never asked for (tts.enabled is false). The Designer window shows this, so
  // a missing voice or a missing piper binary is visible in the UI rather than
  // only in the console the game may not have been launched from.
  std::string speechUnavailableReason;

  friend class DebugConsoleWindow;
  friend class TileInfoWindow;
  friend class AStarWindow;
  friend class EntityOverviewWindow;
  friend class DebugLogWindow;
  friend class MapReloadWindow;
  friend class DrawAsciiDebugWindow;
  friend class FontSelectionWindow;
  friend class MapEditorWindow;
  friend class BlueprintWindow;
  friend class DesignerChatWindow;
  friend class TileDesignerWindow;
  friend class LocationNamingWindow;
  friend class AIMenuWindow;
  friend class NPCMenuWindow;
  friend class TaskBoardWindow;
  friend class SystemOneWindow;
  friend class VisionChatWindow;
  friend class VisionDebugWindow;

  // Debug systems
  std::unique_ptr<DebugWindowState> debugWindowState;
  std::unique_ptr<DebugLog> debugLog;
  std::unique_ptr<MapReloader> mapReloader;

  // The run's record of every System 1 call, and the sole source of what the
  // System 1 Overview window displays.
  //
  // Owned by Game rather than by the window, for the reason the recorder is a
  // separate type at all: the window is rebuilt on every toggle, and a record
  // that vanished with it would defeat the point of collecting one. It also
  // outlives any particular backend, so a model can be swapped without losing the
  // observations that justified the swap.
  //
  // Both the recorder and the System 1 classifier are declared here so the
  // lifetime is explicit; the classifier is constructed in ECSInit once the
  // settings and content registries exist (see SystemOneRecorder.h).
  std::unique_ptr<SystemOne::SystemOneRecorder> systemOneRecorder;

  // The live System 1 provider, built in Game::Init once the settings exist. Owned by
  // Game rather than by a window because a window can be toggled several times in a run:
  // a provider per window would re-register cURL globals each toggle and would lose any
  // request still in flight when the window closed.
  //
  // Null when no API key is configured, which is a supported state -- the sprite
  // designer's router reports "no classifier" rather than offering a button that cannot
  // work. This is the only live classifier in the game; `--sprite-route` and
  // `--systemone-test` build their own for a single headless call.
  std::shared_ptr<SystemOne::SystemOneClassifier> systemOneClassifier;

  // Every backend this run can switch to, resolved once from `system1` in the
  // settings. Resolved at startup rather than per switch so the picker, the resolver
  // and the game cannot disagree about what a profile means.
  std::vector<SystemOne::ModelProfile> systemOneProfiles;

  // Which one is serving calls now. An id from `systemOneProfiles`, never a guess:
  // `SwitchSystemOneProfile` only assigns after the target has been resolved and
  // probed.
  std::string activeSystemOneProfile;

  // Last-known health per profile id, for the picker. Deliberately not persisted: a
  // reachability result from the previous run says nothing about this one, and a stale
  // "Unreachable" would make a working profile look broken.
  std::unordered_map<std::string, SystemOne::SystemOneProfileStatus> systemOneStatus;

public:
  // Switches the backend serving System 1 calls.
  //
  // Returns an empty string on success, or a reason on failure -- in which case the
  // current backend is left untouched, so a failed switch can never leave the game
  // with no classifier.
  //
  // A switch is immediate: any request in flight is cancelled by the old provider's
  // destructor (see JevClassifier's in-flight guard), which is a deliberate trade --
  // an instant, deterministic switch over waiting up to ~40s for a local model to
  // finish. The cancellation is visible as an error on that one record, so it is
  // never a silent loss.
  //
  // Resolves the profile, refuses when it has no key or its endpoint cannot be
  // reached at transport level, probes, then swaps. The probe is TCP-level on
  // purpose, so a local server that is still loading its model (503) stays
  // selectable while one that is not running is refused.
  std::string SwitchSystemOneProfile(const std::string &id);

  // The active profile, or null when neither the resolved list nor the active id
  // survived (which is a startup bug rather than a runtime state).
  const SystemOne::ModelProfile *ActiveSystemOneProfile() const;

  // Health of one profile, defaulting to "untested" for an id nobody has probed.
  SystemOne::SystemOneProfileStatus SystemOneStatusFor(const std::string &id) const;

private:

  std::unique_ptr<InputHandler> inputHandler;

  void LoadMap(std::string mapPath, bool spawnNPCs = false);


  // Opens a read-only window showing the items the entity holds (a Storage
  // container's contents or a character's inventory). Each entity gets its own
  // window, so several can be open at once; re-opening one that is already on
  // screen is a no-op.
  void OpenStorageWindow(flecs::entity container);

  // Opens the read-only window showing a character's name, starting context,
  // stats and skills. Like OpenStorageWindow it gets its own carrier entity, so
  // several can be open at once and reopening one already on screen is a no-op.
  void OpenCharacterStatusWindow(flecs::entity character);

  void ECSInit();
  void ECSInitRenderSystems();

  // The render systems that draw over the tile and entity layers (name tags and
  // other overlays). Called at the end of ECSInitRenderSystems so the pipeline,
  // which runs systems in registration order, keeps tiles under entities under
  // overlays.
  void ECSInitOverlaySystems();

  // Draws one entity's look -- its plate, then its sprite or its ASCII art -- and
  // the debug rectangles over it. Shared by the two render systems so the tile
  // layer and the entity layer cannot drift apart; see MapTileVisual.
  void DrawEntityVisual(flecs::entity e, const DrawAscii &ascii,
                        const ScreenPosition &screenPos);

  // The blueprint placement path, shared by the preview and the commit so the
  // two cannot disagree: whatever the ghost tints green is exactly what a click
  // will write. Both are public because the placement window draws the preview
  // while Game::handleInput performs the click.
  //
  // BuildBlueprintPlacement expands a design at an anchor and marks each cell
  // legal or not. "Legal" is deliberately cheap -- in bounds, and not already
  // occupied by an object or by a blocking tile -- because the full structural
  // validator is not written yet and a design that fails it still has to be
  // placeable so a broken design can be eyeballed.
  BlueprintPlacement BuildBlueprintPlacement(const Blueprint &blueprint,
                                             GamePosition origin);
  // Writes a placement to the world. Returns a one-line report for the window.
  std::string PlaceBlueprintAt(const Blueprint &blueprint, GamePosition origin);

  // Paints one tile onto one cell: points the map at a tile of that name,
  // creating it from the catalog when the map did not already have one, and
  // refreshes the cell's visual entity. Shared with the map editor.
  bool PaintTile(GamePosition position, const std::string &tileName);

  // The inverse of PaintTile: erases the cell's tile and destroys the render
  // entity that was drawing it, so the tile map and the ECS cannot drift apart.
  // Returns false when there was nothing to erase. The palette entry survives,
  // so the same tile can be painted back. Shared with the map editor's trash.
  bool RemoveTileAt(GamePosition position);

  // The tile cell's own render entity -- the child of the map entity that draws
  // this cell's ground. Null when the map entity or the cell's visual is missing.
  flecs::entity FindTileEntity(GamePosition position);

public:
  // Gives every map cell drawing the tile named `tileName` its sprite, now.
  //
  // This is the "attach Sprite to the entities that already exist" half of the
  // hot-swap (docs/tile-art-wiring-design.md §3, check 2). It is needed because
  // render entities are created once, when the map loads, and `Sprite` is only
  // attached when an image actually resolves at that moment -- so a tile whose art
  // arrives later has entities that will never grow one on their own.
  //
  // Deliberately keyed on the *tile*, not on a position: a generated sprite belongs
  // to a tile definition, and every cell drawing that tile should gain it at once.
  // Looking the cells up through the map, rather than matching `DrawAscii` values,
  // is what makes that reliable -- several walls share the glyph '#' (see the parked
  // glyph-collision note in .agents/tile-designer-handover.md), so a glyph match
  // would repaint cells belonging to a different tile.
  //
  // Safe to call when the tile is not on the map at all: it does nothing and
  // returns 0, which is the common case for a tile that was created and never
  // painted. Must be called from the main thread -- it touches the ECS.
  //
  // Public because the swap has to be drivable from outside the render path: the
  // tile designer's window on the main thread, and the headless mode that exists
  // precisely so this can be exercised without one.
  //
  // Returns the number of cells whose visual changed.
  size_t RefreshTileVisuals(const std::string &tileName);

  // Whether the cell's render entity is currently drawing a sprite, and which
  // texture id it holds. Exists for the headless paint harness: the memoised
  // absence bug only shows up as "the Sprite component is missing", and nothing
  // else outside Game can see inside the render entity. Also the honest way to
  // assert a hot-swap from a test -- RefreshTileVisuals reporting 9 changed cells
  // and the cells actually carrying a Sprite are different claims.
  //
  // `tileName` is what the *map* says is on the cell, so a caller can attribute the
  // count to one tile: the render entity carries only display state, and a loaded map
  // is full of cells that legitimately have no art.
  struct CellSpriteState {
    bool hasVisual = false;
    bool hasSprite = false;
    std::string tileName;
    std::string spriteId;
  };
  CellSpriteState InspectCellSprite(GamePosition position);

  // How many cells drawing `tileName` are showing art and how many are still
  // showing their glyph. The count that answers "did the hot-swap actually land",
  // asked of the render entities rather than of the function that performs the
  // swap -- so a test can tell a real swap from a no-op that was merely re-run.
  // Returns false when the map is not loaded, which is the only case with no answer.
  bool TileCellSpriteCount(const std::string &tileName, int *withArt,
                           int *withGlyph);

  // Asks for generated art for `tileName`, if generation is enabled. Returns false
  // and fills `reason` when it is not: off, already queued, already generated, or
  // the session's request cap is reached.
  //
  // Never blocks and never talks to the network. The request joins a batch window
  // and a worker thread draws it; the sprite is swapped in from the main-thread
  // drain when it lands, so the tile renders as its glyph until then -- which is a
  // legitimate rendering, not a placeholder (phase 1).
  //
  // The one way to spend money in this feature, which is why it is gated on
  // art.generation.enabled and capped by art.generation.maxRequestsPerSession.
  bool RequestTileArt(const std::string &tileName, std::string *reason = nullptr);

  // Which kind owns a given definition name, by asking the live registries.
  //
  // Derived rather than plumbed because the request path is name-based all the way down
  // from the window (which knows the name it just accepted) and re-deriving here is one
  // map lookup at the moment the sprite is asked for. The registries answer for
  // runtime-added definitions too, so this is right for a design created seconds ago.
  //
  // The two namespaces overlap on purpose -- the world ships an 'anvil' that is both a tile
  // and an object -- so this cannot be a single lookup and the order is documented at the
  // definition.
  SpriteKinds::Kind KindForSpriteName(const std::string &name) const;

  // The queue, for the window that shows its state. Const because the window only
  // reads; requesting goes through RequestTileArt so the gating lives in one place.
  const TileArtQueue &TileArtQueueState() const { return tileArtQueue; }

  // The main-thread half of the queue, called once per frame by the ECS system
  // registered in ECSInitTileArtSystems. Public because the headless queue test
  // drives it directly.
  //
  // `deltaSeconds` advances the batch window. Returns the number of jobs whose
  // swap completed and the number of new batch requests started, so a caller can
  // tell "nothing happened" from "something was spent".
  struct TileArtPumpResult {
    size_t completed = 0;
    bool startedBatch = false;
  };
  TileArtPumpResult PumpTileArtQueue(float deltaSeconds);

  // Called from the queue's completion callback, on the main thread, for each job
  // that settled. Swaps the sprite in and returns how many cells changed -- zero
  // when the tile is not on the map, which is not a failure.
  //
  // A member rather than work done in the callback itself: the callback is a free
  // function so its address can be handed to the queue, and `debugLog` is private
  // -- this keeps the world-facing half inside Game where it belongs, and the
  // callback a one-line forwarder.
  size_t HandleTileArtGenerated(const TileArtQueue::Job &job);

  // Gives every live entity spawned from `templateKey` its sprite, now. The object
  // counterpart of RefreshTileVisuals, and it exists because the two are not the same
  // operation: a tile is painted onto map cells, and an object is a spawned entity, so
  // "the cells that draw this tile" and "the entities spawned from this template" are
  // different queries.
  //
  // Bug this fixes: an object's art was generated, written, and reported `Done`, and the
  // placed object kept drawing its ASCII art -- because the completion swap only ever
  // looked for map cells. Nothing was wrong with the file or the naming; nothing was
  // looking at entities.
  //
  // Matching is on `ItemType::id`, which is the template key the entity was spawned from
  // and is restamped when a template is re-applied (an Evolvable object that grew), so the
  // swap always targets the template the entity is *currently* using.
  //
  // Returns how many entities changed. Zero is normal and not a failure: an object may have
  // been designed and not yet spawned anywhere, and the next spawn resolves the art anyway
  // because ApplyTemplate resolves at spawn time.
  //
  // Main thread only -- it touches the ECS.
  size_t RefreshObjectVisuals(const std::string &templateKey);

  // Proves the object half of the hot-swap: spawns one entity from `templateKey`, checks it
  // starts on its ASCII art, drops a sprite into the generated layer, and checks the
  // refresh attaches it.
  //
  // A self-test rather than a claim because the bug it exists for was invisible: art was
  // generated, written and reported `Done` while the placed object kept drawing its art,
  // and nothing about the file, the naming or the log said so. Used by
  // `--object-art-swap <key>`.
  int RunObjectArtSelfTest(const std::string &templateKey);

  // Drives the tile designer's logged conversation once, with no backend, and
  // returns how many exchanges were inspected. Used by `--tile-designer-logtest`;
  // the window does the work, this only finds it and reports.
  size_t RunTileDesignerLogSelfTest();

  // Proves the tile designer's pipeline-artifact caches notice a file that changed
  // underneath them. Writes a two-colour PNG, loads it as a thumbnail, rewrites the
  // same path with different pixels and loads it again; a cache that is keyed on the
  // path alone returns the first texture the second time, which is the bug that made
  // a regenerated tile's stages look unchanged. Used by `--art-cache-test`.
  //
  // Returns 0 on success, non-zero with a reason printed to stderr otherwise. Needs
  // a window, because it loads a real GPU texture.
  int RunArtCacheSelfTest();

  // Repaints one cell through the *paint* path -- the same PaintTile the map editor
  // and the tile palette use -- and reports whether the cell's render entity came out
  // of it carrying a Sprite. Used by `--paint-memo-test`, which is the only way to
  // reach the memoised-absence bug: PaintTile is private, and the bug is a property of
  // that call, not of the shared refresh helper it happens to duplicate.
  //
  // Public for the same reason RefreshTileVisuals is: a headless mode has to be able
  // to drive the behaviour it verifies without a display.
  bool PaintTileForTest(GamePosition position, const std::string &tileName,
                        bool *hasSprite);

  // The memoised-absence regression for generated tile art, driven on a tile that is
  // already on the map. Hides the tile's generated sprite, refreshes (which is what
  // records the absence in the ResourceManager's cache -- the same thing PaintTile
  // does for a tile painted before its art existed), restores the file and refreshes
  // again. Before the targeted ForgetPath the second refresh resolved from the cached
  // absence, so the cells kept drawing their glyph forever.
  //
  // Used by `--paint-memo-test`. Needs a window (it loads real textures) and a loaded
  // map with at least one cell drawing the tile it is given.
  int RunPaintMemoSelfTest(const std::string &tileName);

  // Drawn proof that a sprite's transparent pixels show the map tile, not a plate.
  //
  // Two things were wrong and either one alone left the symptom, so this checks the
  // whole visual, from pixels:
  //
  //   * the renderer painted the entity's `backgroundColor` behind every visual, so a
  //     cut-out sprite revealed that plate instead of the tile (fixed in
  //     DrawEntityVisual);
  //   * the generated object's own background was never keyed out, because the
  //     magenta guard rejected what the model actually drew (fixed in
  //     tools/gen_tile_art.py).
  //
  // It draws the real frame into the real render target, reads the framebuffer back
  // with LoadImageFromScreen, and compares:
  //
  //   1. the frame without the object against the frame with it, at every pixel the
  //      sprite leaves fully transparent -- those must be identical;
  //   2. the glyph/art fallback (Sprite and then AsciiArt removed) against the
  //      template's own background colour -- an opaque plate under ink must survive;
  //   3. a tile that draws a sprite against that sprite's own pixels, and a painted
  //      tile with no art against its plate + glyph;
  //
  // Exists because none of this is visible to a headless test otherwise, and a log
  // line is not evidence: a previous session recorded "Object overlay: loaded 1
  // created object(s)" as proof while the sprite was still opaque. Used by
  // `--draw-probe <templateKey>`. Needs a window; spends nothing.
  int RunDrawProbe(const std::string &templateKey);

private:

  void ECSInitPhysicsSystems();
  void ECSInitLogicSystems();
  void ECSInitAgentSystems();
  void ECSInitActionSystems();
  void ECSInitTaskSystems();

  // Samples `Vision` into its own cache (docs/spatial-memory-design.md section
  // 4.4). Separate from the action systems because it is the acquisition layer,
  // not an action: it fills the set the memory writer consumes.
  void ECSInitVisionSystems();

  // Records what each character has seen into its `SpatialMemory` (section 3.4).
  // A consumer of the sweep above, never a second sampler.
  void ECSInitMemorySystems();

  // Writes one character's record to logs/ on demand (section 7.3).
  void DumpSpatialMemory(flecs::entity entity);

  // Registers the tile-art queue's main-thread pump. Separate from the agent
  // systems because it is not part of the AI path -- it mirrors that path's shape,
  // it does not share its globals (see TileArtQueue.hpp).
  void ECSInitTileArtSystems();

  // Reads art.generation.* into the queue and wires its completion callback. Called
  // from ECSInit; separate only so the queue's configuration and the world-facing
  // completion live in the same file as the queue's owner.
  void ECSConfigureTileArtQueue();
};
