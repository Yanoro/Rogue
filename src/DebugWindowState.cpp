#include "DebugWindowState.h"
#include <fstream>
#include <iostream>

using json = nlohmann::json;

DebugWindowState::DebugWindowState()
    : showDebugConsole(true), showPlayerInfoWindow(false),
      showTileInfoWindow(false), showAStarWindow(false),
      showEntityOverviewWindow(false), showDebugLogWindow(false),
      showMapReloadWindow(false), showDrawAsciiToggleWindow(false),
      showFontSelectionWindow(false), defaultFontPath("") {}

void DebugWindowState::SaveState(const std::string &filePath) const {
  json state;
  state["debugConsole"] = showDebugConsole;
  state["playerInfo"] = showPlayerInfoWindow;
  state["tileInfo"] = showTileInfoWindow;
  state["aStar"] = showAStarWindow;
  state["entityOverview"] = showEntityOverviewWindow;
  state["debugLog"] = showDebugLogWindow;
  state["mapReload"] = showMapReloadWindow;
  state["drawAsciiToggle"] = showDrawAsciiToggleWindow;
  state["fontSelection"] = showFontSelectionWindow;
  state["defaultFontPath"] = defaultFontPath;

  std::ofstream file(filePath);
  if (file.is_open()) {
    file << state.dump(2);
    file.close();
    std::cout << "Debug window state saved to: " << filePath << std::endl;
  } else {
    std::cerr << "Failed to save debug window state to: " << filePath
              << std::endl;
  }
}

void DebugWindowState::LoadState(const std::string &filePath) {
  if (!std::filesystem::exists(filePath)) {
    std::cout << "Debug window state file not found: " << filePath << std::endl;
    return;
  }

  std::ifstream file(filePath);
  if (file.is_open()) {
    try {
      json state = json::parse(file);
      file.close();

      showDebugConsole = state.value("debugConsole", true);
      showPlayerInfoWindow = state.value("playerInfo", false);
      showTileInfoWindow = state.value("tileInfo", false);
      showAStarWindow = state.value("aStar", false);
      showEntityOverviewWindow = state.value("entityOverview", false);
      showDebugLogWindow = state.value("debugLog", false);
      showMapReloadWindow = state.value("mapReload", false);
      showDrawAsciiToggleWindow = state.value("drawAsciiToggle", false);
      showFontSelectionWindow = state.value("fontSelection", false);
      defaultFontPath = state.value("defaultFontPath", "");

      std::cout << "Debug window state loaded from: " << filePath << std::endl;
    } catch (const std::exception &e) {
      std::cerr << "Failed to parse debug window state JSON: " << e.what()
                << std::endl;
    }
  } else {
    std::cerr << "Failed to open debug window state file: " << filePath
              << std::endl;
  }
}
