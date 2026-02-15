#include "Game.h"
#include "raylib.h"
#include "rlImGui.h"
#include "imgui.h"

Game::Game() {}

Game::~Game() {
  Shutdown();
}

void Game::Init(std::string mapPath) {
  if (window.IsReady()) return;

  // Use FLAG_BORDERLESS_WINDOWED_MODE for borderless fullscreen
  raylib::Window::SetConfigFlags(FLAG_VSYNC_HINT | FLAG_BORDERLESS_WINDOWED_MODE);

  window.Init(0, 0, "AIRogue");

  map = new Map(mapPath);

  // Ensure it starts on the primary monitor
  window.SetMonitor(0);

  window.SetTargetFPS(60);

  rlImGuiSetup(true);

}

void Game::Update() {
  // Logic updates go here
}

void Game::Draw() {
  map->Draw();
}

void Game::Shutdown() {
  if (!window.IsReady()) return;

  rlImGuiShutdown();
  window.Close();
}

bool Game::shouldClose() const {
  return window.ShouldClose();
}
