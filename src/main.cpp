#include "AI.h"
#include "Game.h"
#include "Window.hpp"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

#include <string>

int main(int argc, char *argv[]) {
  std::string mapPath = "./Map_Trade.json";
  if (argc > 1) {
    mapPath = argv[1];
  }

  Game game;
  game.Init(mapPath);

  raylib::Window *window = game.getWindow();

  // Detect window close button or ESC key
  while (!game.shouldClose()) {
    game.Update();
    game.handleInput();

    window->BeginDrawing();
    window->ClearBackground(BLACK);

    // Draw world
    game.Draw();

    // Draw UI
    rlImGuiBegin();
    game.DrawGameWindows();
    rlImGuiEnd();

    window->EndDrawing();
  }

  game.Shutdown();
  return 0;
}
