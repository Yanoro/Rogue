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

  bool GetShowBlueprintWindow() const { return showBlueprintWindow; }
  void SetShowBlueprintWindow(bool value) { showBlueprintWindow = value; }

  bool GetShowDesignerWindow() const { return showDesignerWindow; }
  void SetShowDesignerWindow(bool value) { showDesignerWindow = value; }

  bool GetShowTileDesignerWindow() const { return showTileDesignerWindow; }
  void SetShowTileDesignerWindow(bool value) { showTileDesignerWindow = value; }

  // Designer window layout: the width of the blueprint pane and the size of one
  // cell in it. Layout state lives here rather than in the window because the
  // other windows are rebuilt on every toggle while the designer is kept alive,
  // and because a layout you have to redo every launch is worse than no layout
  // control. Clamped on read, so a hand-edited file cannot produce a pane that
  // cannot be grabbed or a zoom of zero.
  float GetDesignerPaneWidth() const { return designerPaneWidth; }
  void SetDesignerPaneWidth(float value) { designerPaneWidth = value; }
  float GetDesignerZoom() const { return designerZoom; }
  void SetDesignerZoom(float value) { designerZoom = value; }
  bool GetDesignerShowJson() const { return designerShowJson; }
  void SetDesignerShowJson(bool value) { designerShowJson = value; }
  bool GetDesignerShowThoughts() const { return designerShowThoughts; }
  void SetDesignerShowThoughts(bool value) { designerShowThoughts = value; }

  // The same two values for the Blueprints window, kept separate rather than
  // shared: the two windows hold different content, so a width that suits one
  // does not suit the other. Clamped on use, not here.
  float GetBlueprintPaneWidth() const { return blueprintPaneWidth; }
  void SetBlueprintPaneWidth(float value) { blueprintPaneWidth = value; }
  float GetBlueprintZoom() const { return blueprintZoom; }
  void SetBlueprintZoom(float value) { blueprintZoom = value; }

  bool GetShowAIMenuWindow() const { return showAIMenuWindow; }
  void SetShowAIMenuWindow(bool value) { showAIMenuWindow = value; }

  bool GetShowNPCMenuWindow() const { return showNPCMenuWindow; }
  void SetShowNPCMenuWindow(bool value) { showNPCMenuWindow = value; }

  bool GetShowVisionChatWindow() const { return showVisionChatWindow; }
  void SetShowVisionChatWindow(bool value) { showVisionChatWindow = value; }

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
  bool showBlueprintWindow;
  bool showDesignerWindow;
  bool showTileDesignerWindow;

  // Clamped to the ranges in DesignerChatWindow when read; the stored value is
  // whatever the user last dragged to.
  float designerPaneWidth = 380.0f;
  float designerZoom = 64.0f;
  bool designerShowJson = true;
  // Off by default: the reasoning is noise while watching a design being produced,
  // and it is always in the log when it is wanted.
  bool designerShowThoughts = false;
  float blueprintPaneWidth = 420.0f;
  float blueprintZoom = 64.0f;
  bool showAIMenuWindow;
  bool showNPCMenuWindow;
  bool showVisionChatWindow;
  bool showLocations;
  bool stopAllAI;
  std::string defaultFontPath;
  
    static constexpr const char *STATE_FILE_PATH = "./debug_windows_state.json";};
