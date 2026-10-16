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
#include "TileArtQueue.hpp"
#include "SpeechQueue.h"
#include "StatRegistry.h"
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

  // Object removal, the "trash" mode (map editor). Armed from the Map Editor
  // window, then a left click deletes the authored object on that tile.
  bool removeObjectMode = false;
  std::string lastRemovedObjectName;

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
  friend class VisionChatWindow;

  // Debug systems
  std::unique_ptr<DebugWindowState> debugWindowState;
  std::unique_ptr<DebugLog> debugLog;
  std::unique_ptr<MapReloader> mapReloader;

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
  // that settled. Swaps the sprite in and reports what changed.
  //
  // A member rather than work done in the callback itself: the callback is a free
  // function so its address can be handed to the queue, and `debugLog` is private
  // -- this keeps the world-facing half inside Game where it belongs, and the
  // callback a one-line forwarder.
  void HandleTileArtGenerated(const TileArtQueue::Job &job);

private:

  void ECSInitPhysicsSystems();
  void ECSInitLogicSystems();
  void ECSInitAgentSystems();
  void ECSInitActionSystems();

  // Registers the tile-art queue's main-thread pump. Separate from the agent
  // systems because it is not part of the AI path -- it mirrors that path's shape,
  // it does not share its globals (see TileArtQueue.hpp).
  void ECSInitTileArtSystems();

  // Reads art.generation.* into the queue and wires its completion callback. Called
  // from ECSInit; separate only so the queue's configuration and the world-facing
  // completion live in the same file as the queue's owner.
  void ECSConfigureTileArtQueue();
};
