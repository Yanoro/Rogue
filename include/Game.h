#pragma once
#include "InputHandler.h"
#include "Map.h"
#include "raylib-cpp.hpp"
#include <filesystem>
#include <flecs.h>

#include "Components.h"
#include "Defaults.h"
#include "DebugWindowState.h"
#include "DebugLog.h"
#include "MapReloader.h"

class ResourceManager;

class Game {
public:
  Game();
  ~Game();

  void Init(std::string mapPath);
  void handleInput();

  void UpdateGUI();
  void Update();

  void DrawGameWindows();
  void DrawDebugConsoleWindow();
  void DrawPlayerInfoWindow();
  void DrawTileInfoWindow();
  void DrawAStarWindow();
  void DrawEntityOverviewWindow();
  void DrawDebugLogWindow();
  void DrawMapReloadWindow();
  void DrawDrawAsciiToggleWindow();
  void Draw();

  void BeginDrawingGame();
  void EndDrawingGame();

  void Shutdown();

  flecs::entity createEntity(const GamePosition &gamePos,
                             std::string entityName = "");

  bool shouldClose() const;

  raylib::Window *getWindow() { return &window; }

private:
  raylib::Window window;
  std::unique_ptr<Map> map;

  flecs::world ecs;
  flecs::entity renderPipeline;
  flecs::entity playerEntity;

  flecs::entity selectedTile;
  bool hasClicked = false;
  bool validTileSelected = false;
  GamePosition lastClickedPos{0, 0};

  raylib::RenderTexture2D gameTexture;

  Font gameFont;
  size_t virtualWidth;
  size_t virtualHeight;
  // Astar related variables (Mainly drawing related)
  GamePosition astarStartPos{0, 0};
  GamePosition astarEndPos{0, 0};
  std::vector<GamePosition> astarPath;
  bool isSelectingAStarPath = false;
  int astarClickCount = 0;
  bool isSettingPlayerTarget = false;
  Acceleration lastAccel = {};
  Velocity lastVel = {};

  raylib::Camera2D camera;
  GameCameraMode cameraMode;

  // Debug window visibility state
  bool showDebugConsole = true;  // Console always visible
  bool showPlayerInfoWindow = false;
  bool showTileInfoWindow = false;
  bool showAStarWindow = false;
  bool showEntityOverviewWindow = false;
  bool showDebugLogWindow = false;
  bool showMapReloadWindow = false;
  bool showDrawAsciiToggleWindow = false;

  // Debug systems
  std::unique_ptr<DebugWindowState> debugWindowState;
  std::unique_ptr<DebugLog> debugLog;
  std::unique_ptr<MapReloader> mapReloader;

  std::unique_ptr<InputHandler> inputHandler;
  std::unique_ptr<ResourceManager> resourceManager;

  void ECSInit(std::string mapPath);
  void ECSInitRenderSystems();
  void ECSInitPhysicsSystems();
  void ECSInitLogicSystems();
};
