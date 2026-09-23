#pragma once
#include "InputHandler.h"
#include "Map.h"
#include "raylib-cpp.hpp"
#include <filesystem>
#include <flecs.h>
#include <string>
#include <vector>

#include "Components.h"
#include "DebugLog.h"
#include "DebugWindowState.h"
#include "Defaults.h"
#include "MapReloader.h"
#include "AgentBrain.h"
#include "Hooks.hpp"
#include "RecipeRegistry.h"
#include "SkillRegistry.h"
#include "StatRegistry.h"

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

class Game {
public:
  Game();
  ~Game();

  void Init(std::string mapPath);
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

  void Shutdown();

  flecs::entity createNPC(const GamePosition &gamePos, std::string name = "",
                          std::string prompt = "");

  bool shouldClose() const;

  raylib::Window *getWindow() { return &window; }

private:
  raylib::Window window;
  std::unique_ptr<Map> map;
  ObjectFactory objectFactory;
  RecipeRegistry recipeRegistry;
  StatRegistry statRegistry;
  SkillRegistry skillRegistry;
  HookRegistry hookRegistry;

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

  // File the current map was loaded from, needed to write map edits back.
  // Refreshed on every LoadMap so the save always targets the map on screen.
  std::string mapFilePath;

  // Result of the last "Save Map to File" press, shown in the Map Editor window.
  std::string lastMapSaveMessage;

  raylib::RenderTexture2D gameTexture;

  Font gameFont;
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
  flecs::entity aiMenuWindowEntity;
  flecs::entity npcMenuWindowEntity;

  friend class DebugConsoleWindow;
  friend class TileInfoWindow;
  friend class AStarWindow;
  friend class EntityOverviewWindow;
  friend class DebugLogWindow;
  friend class MapReloadWindow;
  friend class DrawAsciiDebugWindow;
  friend class FontSelectionWindow;
  friend class MapEditorWindow;
  friend class LocationNamingWindow;
  friend class AIMenuWindow;
  friend class NPCMenuWindow;

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

  void ECSInit(std::string mapPath);
  void ECSInitRenderSystems();
  void ECSInitPhysicsSystems();
  void ECSInitLogicSystems();
  void ECSInitAgentSystems();
  void ECSInitActionSystems();
};
