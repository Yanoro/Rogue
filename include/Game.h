#pragma once
#include <flecs.h>
#include <filesystem>
#include "raylib-cpp.hpp"
#include "Map.h"
#include "InputHandler.h"


// TODO: Place these structs into appropriate places
struct Render {};

struct Position {
  int x, y;
};

struct BlocksTile {};

struct Frame {
  raylib::Rectangle frameRect;
  double duration;
};

struct CharacterAnimation {
  raylib::Texture *texture;
  unsigned int frameCount;
  std::vector<Frame> frames;
  unsigned int currentFrame = 0;
  double lastFrameTime = 0;

  CharacterAnimation(raylib::Texture *tex = nullptr, unsigned int frameCount = 1)
    : texture(tex), 
    frameCount(frameCount), 
    currentFrame(0), 
    lastFrameTime(0.0) 
  {
    frames.reserve(frameCount);
  }
};

struct Drawable {
  raylib::Texture *texture;
  raylib::Rectangle srcRect;
};

struct Tile {};

const std::string DEFAULT_PLAYER_ENTITY_NAME = "playerCharacter";

class ResourceManager;

class Game {
public:
  Game();
  ~Game();

  void Init(std::string mapPath);
  void handleInput();
  void Update();
  void Draw(); 
  void Shutdown();

  flecs::entity createEntity(const std::string &texturePath, const Position &pos, 
                             std::string entityName);

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
  
  void ECSInit();
};
