#include "InputHandler.h"
#include "Defaults.h"
#include "raylib.h"

InputHandler::InputHandler(raylib::Camera2D &gameCamera,
                           GameCameraMode &cameraMode, size_t mapWidthPx,
                           size_t mapHeightPx)
    : camera(&gameCamera), cameraMode(&cameraMode), mapWidthPx(mapWidthPx),
      mapHeightPx(mapHeightPx) {};

std::vector<Command *> InputHandler::handleInput() {
  // Actions that do not affect in game 16:19
  // Camera movement, zooming, etc.

  // Toggle camera modes
  if (IsKeyPressed(KEY_H)) {
    GameCameraMode nextMode =
        static_cast<GameCameraMode>((static_cast<int>(*cameraMode) + 1) % 2);
    if (nextMode == GameCameraMode::FreeRoamMode) {
      camera->target.x -= camera->offset.x;
      camera->target.y -= camera->offset.y;
      camera->offset = {0, 0};
    }
    *cameraMode = nextMode;
  }

  if (*cameraMode == GameCameraMode::FreeRoamMode) {
    if (IsKeyDown(KEY_RIGHT)) {
      camera->target.x = std::clamp(
          0.0f, camera->target.x + DEFAULT_INPUT_MOVEMENT,
          static_cast<float>(mapWidthPx - (GetScreenWidth() / camera->zoom)));
    }
    if (IsKeyDown(KEY_LEFT)) {
      camera->target.x = std::clamp(
          0.0f, camera->target.x - DEFAULT_INPUT_MOVEMENT,
          static_cast<float>(mapWidthPx - (GetScreenWidth() / camera->zoom)));
    }
    if (IsKeyDown(KEY_DOWN)) {
      camera->target.y = std::clamp(
          0.0f, camera->target.y + DEFAULT_INPUT_MOVEMENT,
          static_cast<float>(mapHeightPx - (GetScreenHeight() / camera->zoom)));
    }
    if (IsKeyDown(KEY_UP)) {
      camera->target.y = std::clamp(
          0.0f, camera->target.y - DEFAULT_INPUT_MOVEMENT,
          static_cast<float>(mapHeightPx - (GetScreenHeight() / camera->zoom)));
    }
  }
  if (IsKeyPressed((KEY_Z))) {
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
      camera->zoom = std::clamp(DEFAULT_MINIMUM_INPUT_ZOOM,
                                camera->zoom - DEFAULT_INPUT_ZOOM,
                                DEFAULT_MAXIMUM_INPUT_ZOOM);
    } else {
      camera->zoom = std::clamp(DEFAULT_MINIMUM_INPUT_ZOOM,
                                camera->zoom + DEFAULT_INPUT_ZOOM,
                                DEFAULT_MAXIMUM_INPUT_ZOOM);
    }
  }

  std::vector<Command *> commands;

  // Actions that affect game state
  if (IsKeyDown(KEY_W))
    commands.push_back(&velUp);
  if (IsKeyDown(KEY_A))
    commands.push_back(&velLeft);
  if (IsKeyDown(KEY_S))
    commands.push_back(&velDown);
  if (IsKeyDown(KEY_D))
    commands.push_back(&velRight);

  return commands;
}
