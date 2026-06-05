#include "Window.hpp"
#include "raylib.h"
#include "AI.h"
#include "AIChatWindow.h"
#include "Game.h"
#include "imgui.h"
#include "rlImGui.h"

//TODO: Move this to a better place
raylib::Color DEFAULT_BACKGROUND_COLOR = raylib::Color::Black();

#include <string>

int main(int argc, char* argv[]) {
  std::string mapPath = "./Map2.json";
  if (argc > 1) {
    mapPath = argv[1];
  }

  Game game;
  game.Init(mapPath);

  OllamaAI AI("llama3");
  
  AIChatWindow chatWindow(&AI, 
      "You are an adventurer enjoying a drink at the bar, when a stranger approaches you.\nYou: ");
  raylib::Window *window = game.getWindow();

  // Detect window close button or ESC key
  while (!game.shouldClose()) {    
    game.Update();
    game.handleInput();
  
    window->BeginDrawing();
    window->ClearBackground(DEFAULT_BACKGROUND_COLOR);

    // Draw world
    game.Draw();

    // Draw UI
    rlImGuiBegin();
    game.DrawGameWindows();
    //chatWindow.Draw();
    rlImGuiEnd();

    window->EndDrawing();
  }

  game.Shutdown();
  return 0;
}
