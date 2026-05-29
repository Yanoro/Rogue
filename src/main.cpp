#include "Window.hpp"
#include "raylib.h"
#include "AI.h"
#include "AIChatWindow.h"
#include "Game.h"
#include "imgui.h"
#include "rlImGui.h"

int main() {
  Game game;
  game.Init("./testMap.json");

  OllamaAI AI("llama3");
  
  AIChatWindow chatWindow(&AI, 
      "You are an adventurer enjoying a drink at the bar, when a stranger approaches you.\nYou: ");
  raylib::Window *window = game.getWindow();

  // Detect window close button or ESC key
  while (!game.shouldClose()) {    
    game.Update();
    game.handleInput();
  
    window->BeginDrawing();
    window->ClearBackground(raylib::Color::RayWhite());

    // Draw world
    game.Draw();

    // Draw UI
    rlImGuiBegin();
    game.DrawGameWindows();
    ImGui::ShowDemoWindow();
    chatWindow.Draw();
    rlImGuiEnd();

    window->EndDrawing();
  }

  game.Shutdown();
  return 0;
}
