#pragma once
#include <flecs.h>
#include <filesystem>
#include "raylib-cpp.hpp"
#include "Map.h"
#include "InputHandler.h"

#include "Components.h"
#include "Defaults.h"

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
  void DrawPlayerInfoWindow();
  void DrawTileInfoWindow();
  void DrawAStarWindow();
  void DrawEntityOverviewWindow();
  void Draw(); 

  void Shutdown();

  flecs::entity createEntity(const std::string &texturePath, const GamePosition &gamePos,
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
  
  // Astar related variables (Mainly drawing related)
  GamePosition astarStartPos{0, 0};
  GamePosition astarEndPos{0, 0};
  std::vector<GamePosition> astarPath;
  bool isSelectingAStarPath = false;
  int astarClickCount = 0;
  bool isSettingPlayerTarget = false;
  Acceleration lastAccel = {};

  raylib::Camera2D camera;

  std::unique_ptr<InputHandler> inputHandler;
  std::unique_ptr<ResourceManager> resourceManager;
  
  void ECSInit(std::string mapPath);
  void ECSInitRenderSystems();
  void ECSInitPhysicsSystems();
  void ECSInitLogicSystems();
};
