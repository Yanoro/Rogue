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
#include "NPC.h"

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
  //  void DrawEntityInfoWindow(flecs::entity entity);
  void DrawTileInfoWindow();
  void DrawAStarWindow();
  void DrawEntityOverviewWindow();
  void DrawDebugLogWindow();
  void DrawMapReloadWindow();
  void DrawDrawAsciiToggleWindow();
  void DrawFontSelectionWindow();
  void Draw();

  void BeginDrawingGame();
  void EndDrawingGame();

  void Shutdown();

  std::shared_ptr<NPC> createNPC(std::shared_ptr<AI> ai,
                                 const GamePosition &gamePos,
                                 std::string name = "", 
                                 std::string prompt = "");

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
  // Available fonts for selection
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

  // Debug window visibility state
  bool showDebugConsole = true; // Console always visible
  bool showEntityInfoWindow = false;
  bool showTileInfoWindow = false;
  bool showAStarWindow = false;
  bool showEntityOverviewWindow = false;
  bool showDebugLogWindow = false;
  bool showMapReloadWindow = false;
  bool showDrawAsciiToggleWindow = false;
  bool showFontSelectionWindow = false;

  // Debug systems
  std::unique_ptr<DebugWindowState> debugWindowState;
  std::unique_ptr<DebugLog> debugLog;
  std::unique_ptr<MapReloader> mapReloader;

  std::unique_ptr<InputHandler> inputHandler;

  void ECSInit(std::string mapPath);
  void ECSInitRenderSystems();
  void ECSInitPhysicsSystems();
  void ECSInitLogicSystems();
};
