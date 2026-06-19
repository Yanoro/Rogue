#include "AI.h"
#include "AIChatWindow.h"
#include "Game.h"
#include "Window.hpp"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

#include <string>

int main(int argc, char *argv[]) {
  std::string mapPath = "./Map2.json";
  if (argc > 1) {
    mapPath = argv[1];
  }

  Game game;
  game.Init(mapPath);

  auto ai = std::make_shared<OllamaAI>("llama3");

  AIChatWindow chatWindow(ai, "You are an adventurer enjoying a drink at the "
                              "bar, when a stranger approaches you.\nYou: ");
  raylib::Window *window = game.getWindow();

  // Detect window close button or ESC key
  while (!game.shouldClose()) {
    game.Update();
    game.handleInput();

    window->BeginDrawing();
    window->ClearBackground(BLACK);

    // Draw world

    game.Draw();

    window->EndDrawing();
  }

  game.Shutdown();
  return 0;
}
