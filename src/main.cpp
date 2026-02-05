#include <raylib.h>
#include <iostream>
#include "AI.h"
#include "AIChatWindow.h"
#include "imgui.h"
#include "rlImGui.h"

int main() {
  // Initialization
  SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_HIDDEN);
  ToggleBorderlessWindowed();
  InitWindow(1920, 1080, "AIRogue");

  int monitor = 0; // Main monitor
  SetWindowMonitor(monitor);

  ClearWindowState(FLAG_WINDOW_HIDDEN);

  SetTargetFPS(60);

  rlImGuiSetup(true);
 
  OllamaAI AI("llama3");
  
  AIChatWindow chatWindow(&AI, 
      "You are an adventurer enjoying a drink at the bar, when a stranger approaches you.\nYou: ");

  // Detect window close button or ESC key
  while (!WindowShouldClose()) {    

    BeginDrawing();
    ClearBackground(RAYWHITE);

    rlImGuiBegin();

    ImGui::ShowDemoWindow();

    chatWindow.Draw();

    rlImGuiEnd();

    EndDrawing();
  }


  rlImGuiShutdown();
  CloseWindow();        // Close window and OpenGL context

  return 0;
}
