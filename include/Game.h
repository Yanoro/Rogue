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

enum class EditorSelectionType {
  None,
  Tile,
  ObjectTemplate
};

struct EditorSelection {
  EditorSelectionType type = EditorSelectionType::None;
  std::string name;
};

class Game {
public:
  Game();
  ~Game();

  void Init(std::string mapPath);
  void handleInput();

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

  flecs::world ecs;
  flecs::entity renderPipeline;
  flecs::entity playerEntity;

  flecs::entity selectedTile;
  bool hasClicked = false;
  bool validTileSelected = false;
  EditorSelection editorSelection;
  GamePosition lastClickedPos{0, 0};
  double lastLeftClickTime = 0.0;

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

  friend class DebugConsoleWindow;
  friend class TileInfoWindow;
  friend class AStarWindow;
  friend class EntityOverviewWindow;
  friend class DebugLogWindow;
  friend class MapReloadWindow;
  friend class DrawAsciiDebugWindow;
  friend class FontSelectionWindow;
  friend class MapEditorWindow;
  friend class AIMenuWindow;

  // Debug systems
  std::unique_ptr<DebugWindowState> debugWindowState;
  std::unique_ptr<DebugLog> debugLog;
  std::unique_ptr<MapReloader> mapReloader;

  std::unique_ptr<InputHandler> inputHandler;

  void LoadMap(std::string mapPath, bool spawnNPCs = false);

  void ECSInit(std::string mapPath);
  void ECSInitRenderSystems();
  void ECSInitPhysicsSystems();
  void ECSInitLogicSystems();
  void ECSInitAgentSystems();
  void ECSInitActionSystems();
};
