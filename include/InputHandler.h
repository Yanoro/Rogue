#pragma once
#include "Camera2D.hpp"
#include "Command.h"
#include "Defaults.h"
#include <vector>

class InputHandler {
public:
  InputHandler(raylib::Camera2D &gameCamera, GameCameraMode &cameraMode, size_t mapWidthPx, size_t mapHeightPx);

  std::vector<Command*> handleInput();

  // Raw movement intent, used by cast movement locks to tell "still holding the
  // key from before the cast" apart from a new movement input.
  static bool MovementInputDown();

private:
  raylib::Camera2D *camera;
  GameCameraMode *cameraMode;

  size_t mapWidthPx;
  size_t mapHeightPx;

  ChangeVelocityCommand velUp{0.0f, -DEFAULT_VELOCITY};
  ChangeVelocityCommand velDown{0.0f, DEFAULT_VELOCITY};
  ChangeVelocityCommand velLeft{-DEFAULT_VELOCITY, 0.0f};
  ChangeVelocityCommand velRight{DEFAULT_VELOCITY, 0.0f};
};
