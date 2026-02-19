#pragma once
#include "Command.h"
#include "raylib-cpp.hpp"

class InputHandler {
public:
  InputHandler(raylib::Camera2D &gameCamera);

  Command *handleInput();


private:
  raylib::Camera2D *camera;

  MoveCommand moveUp{0, -1};
  MoveCommand moveDown{0, 1};
  MoveCommand moveLeft{-1, 0};
  MoveCommand moveRight{1, 0};
};
