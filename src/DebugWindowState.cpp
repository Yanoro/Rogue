#include "DebugWindowState.h"
#include <fstream>
#include <iostream>

using json = nlohmann::json;

#include "DebugLog.h"

DebugWindowState::DebugWindowState(DebugLog* debugLog) 
  : debugLog(debugLog),
    showDebugConsole(true),
    showEntityInfoWindow(true),
    showTileInfoWindow(false),
    showAStarWindow(false),
    showEntityOverviewWindow(false),
    showDebugLogWindow(false),
    showMapReloadWindow(false),
    showDrawAsciiToggleWindow(false),
    showFontSelectionWindow(false),
    showMapEditorWindow(false),
    showAIMenuWindow(false),
    showNPCMenuWindow(false),
    showLocations(false),
    stopAllAI(false),
    defaultFontPath("") {}

void DebugWindowState::SaveState(const std::string &filePath) const {
  json state;
  state["debugConsole"] = showDebugConsole;
  state["entityInfo"] = showEntityInfoWindow;
  state["tileInfo"] = showTileInfoWindow;
  state["aStar"] = showAStarWindow;
  state["entityOverview"] = showEntityOverviewWindow;
  state["debugLog"] = showDebugLogWindow;
  state["mapReload"] = showMapReloadWindow;
  state["drawAsciiToggle"] = showDrawAsciiToggleWindow;
  state["fontSelection"] = showFontSelectionWindow;
  state["mapEditor"] = showMapEditorWindow;
  state["aiMenu"] = showAIMenuWindow;
  state["npcMenu"] = showNPCMenuWindow;
  state["showLocations"] = showLocations;
  state["stopAllAI"] = stopAllAI;
  state["defaultFontPath"] = defaultFontPath;

  std::ofstream file(filePath);
  if (file.is_open()) {
    file << state.dump(2);
    file.close();
    std::cout << "Debug window state saved to: " << filePath << std::endl;
  } else {
    std::string errStr = "Failed to save debug window state to: " + filePath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
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
      showEntityInfoWindow = state.value("entityInfo", false);
      showTileInfoWindow = state.value("tileInfo", false);
      showAStarWindow = state.value("aStar", false);
      showEntityOverviewWindow = state.value("entityOverview", false);
      showDebugLogWindow = state.value("debugLog", false);
      showMapReloadWindow = state.value("mapReload", false);
      showDrawAsciiToggleWindow = state.value("drawAsciiToggle", false);
      showFontSelectionWindow = state.value("fontSelection", false);
      showMapEditorWindow = state.value("mapEditor", false);
      showAIMenuWindow = state.value("aiMenu", false);
      showNPCMenuWindow = state.value("npcMenu", false);
      showLocations = state.value("showLocations", false);
      stopAllAI = state.value("stopAllAI", false);
      defaultFontPath = state.value("defaultFontPath", "");

      std::cout << "Debug window state loaded from: " << filePath << std::endl;
    } catch (const std::exception &e) {
      std::string errStr = "Failed to parse debug window state JSON: " + std::string(e.what()) + "\n";
      if (debugLog) debugLog->LogError(errStr);
      else std::cerr << errStr;
    }
  } else {
    std::string errStr = "Failed to open debug window state file: " + filePath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
  }
}
