#pragma once
#include <flecs.h>
#include <filesystem>
#include "raylib-cpp.hpp"
#include "Map.h"
#include "InputHandler.h"

#include "Components.h"

const std::string DEFAULT_PLAYER_ENTITY_NAME = "playerCharacter";

class ResourceManager;

class Game {
public:
  Game();
  ~Game();

  void Init(std::string mapPath);
  void handleInput();
  void Update();

  void DrawGameWindows();
  void DrawPlayerInfoWindow();
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

  raylib::Camera2D camera;

  std::unique_ptr<InputHandler> inputHandler;
  std::unique_ptr<ResourceManager> resourceManager;
  
  void ECSInit(std::string mapPath);
};
