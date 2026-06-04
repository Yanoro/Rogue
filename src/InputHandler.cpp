#include "InputHandler.h"
#include "Defaults.h"

InputHandler::InputHandler(raylib::Camera2D &gameCamera) : camera(&gameCamera) {};

std::vector<Command*> InputHandler::handleInput() {
  // Actions that do not affect in game 16:19
  // Camera movement, zooming, etc.
  if (IsKeyDown(KEY_RIGHT)) { camera->target.x += DEFAULT_INPUT_MOVEMENT; }
  if (IsKeyDown(KEY_LEFT)) { camera->target.x -= DEFAULT_INPUT_MOVEMENT; }
  if (IsKeyDown(KEY_DOWN)) { camera->target.y += DEFAULT_INPUT_MOVEMENT; }
  if (IsKeyDown(KEY_UP)) { camera->target.y -= DEFAULT_INPUT_MOVEMENT; }
  if (IsKeyDown(KEY_EQUAL)) { camera->zoom += DEFAULT_INPUT_ZOOM;}
  if (IsKeyDown(KEY_MINUS)) { camera->zoom -= DEFAULT_INPUT_ZOOM;} 

  std::vector<Command*> commands;

  // Actions that affect game state
  if (IsKeyDown(KEY_W)) commands.push_back(&velUp);
  if (IsKeyDown(KEY_A)) commands.push_back(&velLeft);
  if (IsKeyDown(KEY_S)) commands.push_back(&velDown);
  if (IsKeyDown(KEY_D)) commands.push_back(&velRight);

  return commands; 
}
