#include "InputHandler.h"

InputHandler::InputHandler(raylib::Camera2D &gameCamera) : camera(&gameCamera) {};

std::vector<Command*> InputHandler::handleInput() {
  const int deltaInputMovement = 10;
  const float deltaInputZoom = 0.05f; 

  // Actions that do not affect in game 16:19
  // Camera movement, zooming, etc.
  if (IsKeyDown(KEY_RIGHT)) { camera->target.x += deltaInputMovement; }
  if (IsKeyDown(KEY_LEFT)) { camera->target.x -= deltaInputMovement; }
  if (IsKeyDown(KEY_DOWN)) { camera->target.y += deltaInputMovement; }
  if (IsKeyDown(KEY_UP)) { camera->target.y -= deltaInputMovement; }
  if (IsKeyDown(KEY_EQUAL)) { camera->zoom += deltaInputZoom;}
  if (IsKeyDown(KEY_MINUS)) { camera->zoom -= deltaInputZoom;} 

  std::vector<Command*> commands;

  // Actions that affect game state
  if (IsKeyDown(KEY_W)) commands.push_back(&velUp);
  if (IsKeyDown(KEY_A)) commands.push_back(&velLeft);
  if (IsKeyDown(KEY_S)) commands.push_back(&velDown);
  if (IsKeyDown(KEY_D)) commands.push_back(&velRight);

  return commands; 
}
