#include "AI.h"
#include "Defaults.h"
#include "Game.h"
#include "Settings.h"
#include "Window.hpp"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

#include <string>

int main(int argc, char *argv[]) {
  // Usage:
  //   AIRogue                        -> settings.json, and the map it names
  //   AIRogue <map.json>             -> settings.json, map overridden
  //   AIRogue --settings <file.json> -> a different settings file
  // The map argument keeps its old meaning so existing run scripts still work;
  // --settings is the only new spelling.
  std::string settingsPath = "settings.json";
  std::string mapOverride;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--settings" && i + 1 < argc) {
      settingsPath = argv[++i];
    } else if (mapOverride.empty()) {
      mapOverride = arg;
    }
  }

  // Read once, before anything else, so Game::Init and the ECS see the same
  // values. A missing or malformed file is reported and leaves the built-in
  // defaults in place.
  LoadGameSettings(settingsPath);

  // Applied after the load, because LoadGameSettings resets to defaults first.
  if (!mapOverride.empty()) {
    MutableSettings().map.file = mapOverride;
  }

  Game game;
  game.Init();

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
