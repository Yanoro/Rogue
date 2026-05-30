#pragma once
#include "Camera2D.hpp"
#include "Command.h"
#include <vector>

const float DEFAULT_VELOCITY = 50.0f;

class InputHandler {
public:
  InputHandler(raylib::Camera2D &gameCamera);

  std::vector<Command*> handleInput();


private:
  raylib::Camera2D *camera;

  ChangeVelocityCommand velUp{0.0f, -DEFAULT_VELOCITY};
  ChangeVelocityCommand velDown{0.0f, DEFAULT_VELOCITY};
  ChangeVelocityCommand velLeft{-DEFAULT_VELOCITY, 0.0f};
  ChangeVelocityCommand velRight{DEFAULT_VELOCITY, 0.0f};
};
