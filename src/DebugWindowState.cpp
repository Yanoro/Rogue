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
    showBlueprintWindow(false),
    showDesignerWindow(false),
    showTileDesignerWindow(false),
    showAIMenuWindow(false),
    showNPCMenuWindow(false),
    showTaskBoardWindow(false),
    showSystemOneWindow(false),
    showVisionDebugWindow(false),
    showFacingMarkers(false),
    showVisionOverlay(false),
    showVisionChatWindow(false),
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
  // Stored as ids ("name", "date", "usage", "source") so a future enum reorder
  // cannot reinterpret an existing file. The direction is explicit because the
  // same metric means different things at each end (newest/oldest, most/least).
  state["mapEditorTileSort"] = ContentSortMetricId(tileSortMetric);
  state["mapEditorTileSortDescending"] = tileSortDescending;
  state["mapEditorObjectSort"] = ContentSortMetricId(objectSortMetric);
  state["mapEditorObjectSortDescending"] = objectSortDescending;
  state["blueprint"] = showBlueprintWindow;
  state["designer"] = showDesignerWindow;
  state["tileDesigner"] = showTileDesignerWindow;
  state["designerPaneWidth"] = designerPaneWidth;
  state["designerZoom"] = designerZoom;
  state["designerShowJson"] = designerShowJson;
  state["designerShowThoughts"] = designerShowThoughts;
  state["tileDesignerAutoArt"] = tileDesignerAutoArt;
  state["blueprintPaneWidth"] = blueprintPaneWidth;
  state["blueprintZoom"] = blueprintZoom;
  state["aiMenu"] = showAIMenuWindow;
  state["npcMenu"] = showNPCMenuWindow;
  state["taskBoard"] = showTaskBoardWindow;
  state["systemOne"] = showSystemOneWindow;
  state["visionDebug"] = showVisionDebugWindow;
  state["facingMarkers"] = showFacingMarkers;
  state["visionOverlay"] = showVisionOverlay;
  state["systemOneProfile"] = systemOneProfile;
  state["visionChat"] = showVisionChatWindow;
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
      tileSortMetric = ContentSortMetricFromId(
          state.value("mapEditorTileSort", std::string("name")),
          ContentSortMetric::Name);
      tileSortDescending =
          state.value("mapEditorTileSortDescending", false);
      objectSortMetric = ContentSortMetricFromId(
          state.value("mapEditorObjectSort", std::string("name")),
          ContentSortMetric::Name);
      objectSortDescending =
          state.value("mapEditorObjectSortDescending", false);
      showBlueprintWindow = state.value("blueprint", false);
      showDesignerWindow = state.value("designer", false);
      showTileDesignerWindow = state.value("tileDesigner", false);
      designerPaneWidth = state.value("designerPaneWidth", designerPaneWidth);
      designerZoom = state.value("designerZoom", designerZoom);
      designerShowJson = state.value("designerShowJson", designerShowJson);
      designerShowThoughts =
          state.value("designerShowThoughts", designerShowThoughts);
      tileDesignerAutoArt =
          state.value("tileDesignerAutoArt", tileDesignerAutoArt);
      // Recorded separately from the value: a file written before this key existed
      // must leave the preference *unstated*, so the window can follow
      // art.generation.enabled instead of defaulting to off.
      tileDesignerAutoArtStored = state.contains("tileDesignerAutoArt");
      blueprintPaneWidth = state.value("blueprintPaneWidth", blueprintPaneWidth);
      blueprintZoom = state.value("blueprintZoom", blueprintZoom);
      showAIMenuWindow = state.value("aiMenu", false);
      showNPCMenuWindow = state.value("npcMenu", false);
      showTaskBoardWindow = state.value("taskBoard", false);
      showSystemOneWindow = state.value("systemOne", false);
      showVisionDebugWindow = state.value("visionDebug", false);
      showFacingMarkers = state.value("facingMarkers", false);
      showVisionOverlay = state.value("visionOverlay", false);
      systemOneProfile = state.value("systemOneProfile", std::string());
      showVisionChatWindow = state.value("visionChat", false);
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
