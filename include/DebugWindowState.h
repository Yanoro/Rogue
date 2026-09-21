#pragma once

#include <string>
#include <filesystem>
#include <nlohmann/json.hpp>

class DebugLog;

class DebugWindowState {
public:
  DebugWindowState(DebugLog* debugLog = nullptr);
  
  // Save debug window visibility states to JSON
  void SaveState(const std::string &filePath) const;
  
  // Load debug window visibility states from JSON
  void LoadState(const std::string &filePath);
  
  // Getters and setters
  bool GetShowDebugConsole() const { return showDebugConsole; }
  void SetShowDebugConsole(bool value) { showDebugConsole = value; }
  
  bool GetShowEntityInfoWindow() const { return showEntityInfoWindow; }
  void SetShowEntityInfoWindow(bool value) { showEntityInfoWindow = value; }
  
  bool GetShowTileInfoWindow() const { return showTileInfoWindow; }
  void SetShowTileInfoWindow(bool value) { showTileInfoWindow = value; }
  
  bool GetShowAStarWindow() const { return showAStarWindow; }
  void SetShowAStarWindow(bool value) { showAStarWindow = value; }
  
  bool GetShowEntityOverviewWindow() const { return showEntityOverviewWindow; }
  void SetShowEntityOverviewWindow(bool value) { showEntityOverviewWindow = value; }
  
  bool GetShowDebugLogWindow() const { return showDebugLogWindow; }
  void SetShowDebugLogWindow(bool value) { showDebugLogWindow = value; }
  
  bool GetShowMapReloadWindow() const { return showMapReloadWindow; }
  void SetShowMapReloadWindow(bool value) { showMapReloadWindow = value; }
  
  bool GetShowDrawAsciiToggleWindow() const { return showDrawAsciiToggleWindow; }
  void SetShowDrawAsciiToggleWindow(bool value) { showDrawAsciiToggleWindow = value; }

  bool GetShowFontSelectionWindow() const { return showFontSelectionWindow; }
  void SetShowFontSelectionWindow(bool value) { showFontSelectionWindow = value; }

  bool GetShowMapEditorWindow() const { return showMapEditorWindow; }
  void SetShowMapEditorWindow(bool value) { showMapEditorWindow = value; }

  bool GetShowAIMenuWindow() const { return showAIMenuWindow; }
  void SetShowAIMenuWindow(bool value) { showAIMenuWindow = value; }

  bool GetShowNPCMenuWindow() const { return showNPCMenuWindow; }
  void SetShowNPCMenuWindow(bool value) { showNPCMenuWindow = value; }

  bool GetShowLocations() const { return showLocations; }
  void SetShowLocations(bool value) { showLocations = value; }

  std::string GetDefaultFontPath() const { return defaultFontPath; }
  void SetDefaultFontPath(const std::string& path) { defaultFontPath = path; }

  bool GetStopAllAI() const { return stopAllAI; }
  void SetStopAllAI(bool value) { stopAllAI = value; }

private:
  DebugLog* debugLog;
  bool showDebugConsole;
  bool showEntityInfoWindow;
  bool showTileInfoWindow;
  bool showAStarWindow;
  bool showEntityOverviewWindow;
  bool showDebugLogWindow;
  bool showMapReloadWindow;
  bool showDrawAsciiToggleWindow;
  bool showFontSelectionWindow;
  bool showMapEditorWindow;
  bool showAIMenuWindow;
  bool showNPCMenuWindow;
  bool showLocations;
  bool stopAllAI;
  std::string defaultFontPath;
  
    static constexpr const char *STATE_FILE_PATH = "./debug_windows_state.json";};
