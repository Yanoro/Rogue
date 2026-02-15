#pragma once
#include "raylib-cpp.hpp"
#include "Map.h"

class Game {
public:
  Game();
  ~Game();

  void Init(std::string mapPath);
  void Update();
  void Draw(); 
  void Shutdown();

  bool shouldClose() const;

  raylib::Window *getWindow() { return &window; }

private:
  raylib::Window window;
  Map *map;
};
