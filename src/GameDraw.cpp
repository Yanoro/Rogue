#include "Components.h"
#include "Defaults.h"
#include "Game.h"
#include "PathFinding.h"
#include "DebugLog.h"
#include "DrawAsciiDebug.h"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <algorithm>

// Helper function to format JSON with indentation for pretty printing
static std::string PrettyPrintJson(const char *json) {
  if (!json) return "";

  std::string result;
  int indent = 0;
  bool inString = false;
  bool escapeNext = false;

  for (size_t i = 0; i < strlen(json); ++i) {
    char c = json[i];

    if (escapeNext) {
      result += c;
      escapeNext = false;
      continue;
    }

    if (c == '\\' && inString) {
      result += c;
      escapeNext = true;
      continue;
    }

    if (c == '"') {
      inString = !inString;
      result += c;
      continue;
    }

    if (inString) {
      result += c;
      continue;
    }

    switch (c) {
    case '{':
    case '[':
      result += c;
      result += '\n';
      indent++;
      for (int j = 0; j < indent * 2; ++j) result += ' ';
      break;
    case '}':
    case ']':
      result += '\n';
      indent--;
      for (int j = 0; j < indent * 2; ++j) result += ' ';
      result += c;
      break;
    case ',':
      result += c;
      result += '\n';
      for (int j = 0; j < indent * 2; ++j) result += ' ';
      break;
    case ':':
      result += c;
      result += ' ';
      break;
    case ' ':
    case '\n':
    case '\t':
      // Skip whitespace (we're adding our own)
      break;
    default:
      result += c;
    }
  }

  return result;
}

void Game::DrawDebugConsoleWindow() {
  ImGui::Begin("Debug Console", &showDebugConsole,
               ImGuiWindowFlags_AlwaysAutoResize);

  ImGui::Text("Debug Windows");
  ImGui::Separator();

  if (ImGui::Checkbox("Player Entity Window", &showPlayerInfoWindow)) {
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Tile Info Window", &showTileInfoWindow)) {
  }

  if (ImGui::Checkbox("A* Window", &showAStarWindow)) {
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Entity Overview", &showEntityOverviewWindow)) {
  }

  if (ImGui::Checkbox("Debug Log Window", &showDebugLogWindow)) {
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Map Reload Window", &showMapReloadWindow)) {
  }

  if (ImGui::Checkbox("DrawAscii Toggle", &showDrawAsciiToggleWindow)) {
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Font Selection", &showFontSelectionWindow)) {
  }

  ImGui::Separator();
  ImGui::Text("All debug windows can be closed by clicking the X button.");

  ImGui::End();
}


void Game::DrawPlayerInfoWindow() {
  ImGui::Begin("Player Entity", &showPlayerInfoWindow);
  if (playerEntity.is_alive()) {
    if (const GamePosition *gPos = playerEntity.get<GamePosition>()) {
      int pos[2] = {gPos->x, gPos->y};
      if (ImGui::InputInt2("Game Position", pos)) {
        playerEntity.set<GamePosition>({pos[0], pos[1]});
        if (map) {
          playerEntity.set<ScreenPosition>(
              map->GameCoordsToScreenCoords(pos[0], pos[1]));
        }
      }
    }
    if (const ScreenPosition *sPos = playerEntity.get<ScreenPosition>()) {
      float pos[2] = {sPos->x, sPos->y};
      if (ImGui::DragFloat2("Screen Position", pos)) {
        playerEntity.set<ScreenPosition>({pos[0], pos[1]});
      }
    }
    if (const TargetPath *targetPath = playerEntity.get<TargetPath>()) {
      if (!targetPath->path.empty()) {
        const GamePosition &lastPos = targetPath->path.back();
        ImGui::Text("Moving to: (%d, %d)", lastPos.x, lastPos.y);
      }
    }

    float v[2] = {lastVel.x, lastVel.y};
    if (ImGui::DragFloat2("Velocity", v)) {
      playerEntity.set<Velocity>({v[0], v[1]});
    }
    float a[2] = {lastAccel.x, lastAccel.y};
    if (ImGui::DragFloat2("Acceleration", a)) {
      playerEntity.set<Acceleration>({a[0], a[1]});
    }

    if (const MaxSpeed *speed = playerEntity.get<MaxSpeed>()) {
      float s = speed->value;
      if (ImGui::DragFloat("Max Speed", &s)) {
        playerEntity.set<MaxSpeed>({s});
      }
    }
    if (const Friction *friction = playerEntity.get<Friction>()) {
      float f = friction->value;
      if (ImGui::DragFloat("Friction", &f)) {
        playerEntity.set<Friction>({f});
      }
    }

    ImGui::Separator();
    bool isIntangible = playerEntity.has<Intangible>();
    if (ImGui::Button(isIntangible ? "Remove Intangible" : "Make Intangible")) {
      if (isIntangible) {
        playerEntity.remove<Intangible>();
      } else {
        playerEntity.add<Intangible>();
      }
    }

    ImGui::Separator();
    ImGui::Text("Velocity & Acceleration Vectors:");

    // Layout the velocity and acceleration vectors side by side
    ImVec2 canvasSize(120.0f, 120.0f);
    
    // Velocity Vector (left side)
    ImGui::Text("Velocity:");
    ImGui::SameLine(150.0f);
    ImGui::Text("Acceleration:");
    
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##vel_canvas", canvasSize);

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    ImVec2 center(canvasPos.x + canvasSize.x * 0.5f,
                  canvasPos.y + canvasSize.y * 0.5f);
    float maxRadius = canvasSize.x * 0.45f;

    // Draw background circle representing max speed
    drawList->AddCircleFilled(center, maxRadius, IM_COL32(40, 40, 40, 255));
    drawList->AddCircle(center, maxRadius, IM_COL32(100, 100, 100, 255));

    // Draw crosshair axes
    drawList->AddLine(ImVec2(center.x, canvasPos.y + 5),
                      ImVec2(center.x, canvasPos.y + canvasSize.y - 5),
                      IM_COL32(80, 80, 80, 255));
    drawList->AddLine(ImVec2(canvasPos.x + 5, center.y),
                      ImVec2(canvasPos.x + canvasSize.x - 5, center.y),
                      IM_COL32(80, 80, 80, 255));

    const MaxSpeed *maxSpeed = playerEntity.get<MaxSpeed>();

    if (maxSpeed && maxSpeed->value > 0.0f) {
      // Calculate scaled endpoint
      float scaleX = maxRadius / maxSpeed->value;
      float scaleY = maxRadius / maxSpeed->value;
      ImVec2 velEnd(center.x + lastVel.x * scaleX, center.y + lastVel.y * scaleY);

      // Draw velocity vector line and arrowhead/dot
      drawList->AddLine(center, velEnd, IM_COL32(50, 255, 50, 255), 2.0f);
      drawList->AddCircleFilled(velEnd, 3.0f, IM_COL32(50, 255, 50, 255));
    }

    // Center point
    drawList->AddCircleFilled(center, 2.0f, IM_COL32(255, 255, 255, 255));

    // Draw acceleration vector on the same line, to the right
    ImGui::SameLine();

    ImVec2 accelCanvasSize(120.0f, 120.0f);
    ImVec2 accelCanvasPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##accel_canvas", accelCanvasSize);

    ImVec2 accelCenter(accelCanvasPos.x + accelCanvasSize.x * 0.5f,
                       accelCanvasPos.y + accelCanvasSize.y * 0.5f);
    float accelMaxRadius = accelCanvasSize.x * 0.45f;

    // Draw background circle
    drawList->AddCircleFilled(accelCenter, accelMaxRadius,
                              IM_COL32(40, 40, 40, 255));
    drawList->AddCircle(accelCenter, accelMaxRadius,
                        IM_COL32(100, 100, 100, 255));

    // Draw crosshair axes
    drawList->AddLine(
        ImVec2(accelCenter.x, accelCanvasPos.y + 5),
        ImVec2(accelCenter.x, accelCanvasPos.y + accelCanvasSize.y - 5),
        IM_COL32(80, 80, 80, 255));
    drawList->AddLine(
        ImVec2(accelCanvasPos.x + 5, accelCenter.y),
        ImVec2(accelCanvasPos.x + accelCanvasSize.x - 5, accelCenter.y),
        IM_COL32(80, 80, 80, 255));

    if (maxSpeed && maxSpeed->value > 0.0f) {
      // Scale relative to max speed (can be tuned later if acceleration exceeds
      // this)
      float scaleX = accelMaxRadius / (maxSpeed->value * 5.0f);
      float scaleY = accelMaxRadius / (maxSpeed->value * 5.0f);
      ImVec2 accelEnd(accelCenter.x + lastAccel.x * scaleX,
                      accelCenter.y + lastAccel.y * scaleY);

      // Draw acceleration vector line and arrowhead/dot (Red instead of Green)
      drawList->AddLine(accelCenter, accelEnd, IM_COL32(255, 50, 50, 255),
                        2.0f);
      drawList->AddCircleFilled(accelEnd, 3.0f, IM_COL32(255, 50, 50, 255));
    }

    // Center point
    drawList->AddCircleFilled(accelCenter, 2.0f, IM_COL32(255, 255, 255, 255));
  } else {
    ImGui::Text("Player entity not alive.");
  }
  ImGui::End();
}

void Game::DrawTileInfoWindow() {

  ImGui::Begin("Tile Info", &showTileInfoWindow);

  if (hasClicked) {

    ImGui::Text("Map Coordinates: (%d, %d)", lastClickedPos.x,
                lastClickedPos.y);

    if (Location *loc = map->GetLocation(lastClickedPos)) {
      ImGui::Separator();
      ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Location: %s", loc->name.c_str());
      ImGui::TextWrapped("%s", loc->description.c_str());
    }

    ImGui::Separator();

    if (validTileSelected && selectedTile.is_alive()) {

      ImGui::Text("Tile Entity ID: %lu", selectedTile.id());

      if (const ScreenPosition *sPos = selectedTile.get<ScreenPosition>()) {

        ImGui::Text("Screen Position: (%.2f, %.2f)", sPos->x, sPos->y);
      }

      if (selectedTile.has<BlocksTile>()) {

        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Blocks Path: YES");

      } else {

        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Blocks Path: NO");
      }

    } else {

      ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                         "No valid tile entity at this position.");
    }

  } else {

    ImGui::Text("Click anywhere on the map to inspect.");
  }

  ImGui::End();
}

void Game::DrawAStarWindow() {
  ImGui::Begin("A*", &showAStarWindow);

  if (isSelectingAStarPath) {
    if (ImGui::Button("Cancel Selection")) {
      isSelectingAStarPath = false;
      astarClickCount = 0;
    }
    ImGui::Text("Click %d/2 on map", astarClickCount + 1);
  } else if (isSettingPlayerTarget) {
    if (ImGui::Button("Cancel Player Target")) {
      isSettingPlayerTarget = false;
    }
    ImGui::Text("Click on map to set target");
  } else {
    if (ImGui::Button("Select Path")) {
      isSelectingAStarPath = true;
      astarClickCount = 0;
      astarStartPos = {0, 0};
      astarEndPos = {0, 0};
      astarPath.clear();
    }
    if (ImGui::Button("Set Player Target")) {
      isSettingPlayerTarget = true;
    }
  }

  ImGui::End();
}

void Game::DrawEntityOverviewWindow() {
  struct OpenComponentWindow {
    flecs::entity entity;
    flecs::id id;
  };
  static std::vector<OpenComponentWindow> openComponentWindows;

  ImGui::Begin("Entity Overview", &showEntityOverviewWindow);

  // Optional: Add a text filter
  static ImGuiTextFilter filter;
  filter.Draw("Filter");

  ImGui::Separator();

  // Begin a table for better formatting
  if (ImGui::BeginTable("EntitiesTable", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Position");
    ImGui::TableSetupColumn("Components");
    ImGui::TableHeadersRow();

    ecs.filter_builder().term(flecs::Any).build().each([&](flecs::entity e) {
      // Skip internal Flecs entities
      std::string path = e.path().c_str() ? e.path().c_str() : "";
      if (path.find("::flecs") == 0) {
        return;
      }

      // Get the name or ID string for filtering and display
      std::string name = e.name().c_str() ? e.name().c_str() : "Unnamed";
      std::string idStr = std::to_string(e.id());

      // Combine name and ID for the filter
      std::string searchString = name + " " + idStr;

      if (filter.PassFilter(searchString.c_str())) {
        ImGui::TableNextRow();

        // 1. ID Column
        ImGui::TableNextColumn();
        ImGui::Text("%lu", e.id());

        // 2. Name Column
        ImGui::TableNextColumn();
        ImGui::Text("%s", name.c_str());

        // 3. Position Column
        ImGui::TableNextColumn();
        if (const GamePosition *pos = e.get<GamePosition>()) {
          ImGui::Text("(%d, %d)", pos->x, pos->y);
        } else if (const ScreenPosition *sPos = e.get<ScreenPosition>()) {
          ImGui::Text("Screen(%.1f, %.1f)", sPos->x, sPos->y);
        } else {
          ImGui::Text("-");
        }

        // 4. Components Column
        ImGui::TableNextColumn();

        e.each([&](flecs::id id) {
          std::string label = std::string(id.str().c_str()) + "##" +
                              std::to_string(e.id()) + "_" +
                              std::to_string(id.raw_id());
          if (ImGui::SmallButton(label.c_str())) {
            openComponentWindows.push_back({e, id});
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("ID: %lu", id.raw_id());
          }
          ImGui::SameLine();
        });
        ImGui::Text(" "); // End of line for the table cell
      }
    });

    ImGui::EndTable();
  }

  ImGui::End();

  // Draw open component windows
  for (auto it = openComponentWindows.begin();
       it != openComponentWindows.end();) {
    bool open = true;
    std::string winName =
        "Component: " + std::string(it->id.str().c_str()) + " (" +
        std::string(it->entity.name().c_str() ? it->entity.name().c_str()
                                              : "Unnamed") +
        ")##" + std::to_string(it->entity.id()) + "_" +
        std::to_string(it->id.raw_id());

    ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(winName.c_str(), &open)) {
      const void *ptr = it->entity.get(it->id.raw_id());
      if (ptr) {
        char *json = ecs_ptr_to_json(ecs.c_ptr(), it->id.raw_id(), ptr);
        if (json) {
          std::string prettyJson = PrettyPrintJson(json);
          ImGui::TextWrapped("%s", prettyJson.c_str());
          ecs_os_free(json);
        } else {
          ImGui::TextDisabled("No JSON representation available.");
        }
      } else {
        ImGui::TextDisabled("No data for this component (might be a tag).");
      }
    }
    ImGui::End();

    if (!open) {
      it = openComponentWindows.erase(it);
    } else {
      ++it;
    }
  }
}

void Game::DrawDebugLogWindow() {
  ImGui::Begin("Debug Log", &showDebugLogWindow,
               ImGuiWindowFlags_AlwaysAutoResize);

  ImGui::Text("Debug Log (Latest %zu entries)", debugLog->GetMaxEntries());
  ImGui::Separator();

  if (ImGui::Button("Clear Log")) {
    debugLog->Clear();
  }

  ImGui::BeginChild("##LogContent", ImVec2(500, 300),
                    ImGuiChildFlags_Borders);

  const auto &entries = debugLog->GetEntries();
  for (const auto &entry : entries) {
    ImVec4 color;
    switch (entry.level) {
    case DebugLog::LogLevel::INFO:
      color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
      break;
    case DebugLog::LogLevel::WARNING:
      color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
      break;
    case DebugLog::LogLevel::ERROR:
      color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
      break;
    case DebugLog::LogLevel::DEBUG:
      color = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
      break;
    }
    ImGui::TextColored(color, "[%s] %s", entry.timestamp.c_str(),
                      entry.message.c_str());
  }

  // Auto-scroll to bottom when new entries are added
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetItemRectSize().y) {
    ImGui::SetScrollHereY(1.0f);
  }

  ImGui::EndChild();
  ImGui::End();
}

void Game::DrawMapReloadWindow() {
  ImGui::Begin("Map Reload", &showMapReloadWindow,
               ImGuiWindowFlags_AlwaysAutoResize);

  ImGui::Text("Available Maps");
  ImGui::Separator();

  if (ImGui::Button("Refresh Map List")) {
    mapReloader->RefreshMapList();
    debugLog->LogInfo("Map list refreshed");
  }

  ImGui::Separator();

  const auto &mapList = mapReloader->GetMapList();
  if (mapList.empty()) {
    ImGui::TextDisabled("No maps found in: %s",
                       mapReloader->GetDirectory().c_str());
  } else {
    static int selectedMapIndex = 0;

    if (ImGui::BeginListBox("##MapList", ImVec2(-1, 200))) {
      for (size_t i = 0; i < mapList.size(); i++) {
        bool isSelected = (selectedMapIndex == (int)i);
        if (ImGui::Selectable(mapList[i].c_str(), isSelected)) {
          selectedMapIndex = i;
        }
        if (isSelected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndListBox();
    }

    if (selectedMapIndex >= 0 && selectedMapIndex < (int)mapList.size()) {
      std::string mapPath = mapReloader->GetMapPath(selectedMapIndex);
      ImGui::Text("Selected: %s", mapList[selectedMapIndex].c_str());
      ImGui::Text("Path: %s", mapPath.c_str());
    }
  }

  ImGui::End();
}

void Game::DrawDrawAsciiToggleWindow() {
  ImGui::Begin("DrawAscii Debug", &showDrawAsciiToggleWindow,
               ImGuiWindowFlags_AlwaysAutoResize);

  ImGui::Text("DrawAscii Rectangle Toggles");
  ImGui::Separator();

  bool showOuter = DrawAsciiDebug::GetShowOuterRectangles();
  if (ImGui::Checkbox("Show Outer Rectangles (RED)", &showOuter)) {
    DrawAsciiDebug::SetShowOuterRectangles(showOuter);
    debugLog->LogInfo(showOuter ? "Outer rectangles enabled" : "Outer rectangles disabled");
  }

  bool showInner = DrawAsciiDebug::GetShowInnerRectangles();
  if (ImGui::Checkbox("Show Inner Rectangles (BLUE)", &showInner)) {
    DrawAsciiDebug::SetShowInnerRectangles(showInner);
    debugLog->LogInfo(showInner ? "Inner rectangles enabled" : "Inner rectangles disabled");
  }

  ImGui::Separator();
  ImGui::Text("Red: Tile boundaries");
  ImGui::Text("Blue: Text boundaries");

  ImGui::End();
}

void Game::DrawGameWindows() {
  // Always draw the debug console to control other windows
  DrawDebugConsoleWindow();

  // Draw individual debug windows only if enabled
  if (showPlayerInfoWindow) {
    DrawPlayerInfoWindow();
  }

  if (showTileInfoWindow) {
    DrawTileInfoWindow();
  }

  if (showAStarWindow) {
    DrawAStarWindow();
  }

  if (showEntityOverviewWindow) {
    DrawEntityOverviewWindow();
  }

  if (showDebugLogWindow) {
    DrawDebugLogWindow();
  }

  if (showMapReloadWindow) {
    DrawMapReloadWindow();
  }

  if (showDrawAsciiToggleWindow) {
    DrawDrawAsciiToggleWindow();
  }

  if (showFontSelectionWindow) {
    DrawFontSelectionWindow();
  }
}

void Game::Draw() {
  BeginDrawingGame();
  ClearBackground(GRAY); // Clear the render texture

  camera.BeginMode();

  if (cameraMode == GameCameraMode::FollowMode) {
    const ScreenPosition *playerPos = playerEntity.get<ScreenPosition>();
    int screenWidth = GetScreenWidth();
    int screenHeight = GetScreenHeight();
    int mapWidthPx = map->GetMapWidthPx();
    int mapHeightPx = map->GetMapHeightPx();

    camera.offset = {screenWidth / 2.0f, screenHeight / 2.0f};
    camera.target = {playerPos->x, playerPos->y};

    camera.target.x = std::clamp(camera.target.x, camera.offset.x / camera.zoom,
                                mapWidthPx - (camera.offset.x / camera.zoom));

    camera.target.y = std::clamp(camera.target.y, camera.offset.y / camera.zoom,
                                mapHeightPx - (camera.offset.y / camera.zoom));
  }

  ecs.run_pipeline(renderPipeline);

  if (map) {
    int tileW = map->GetTileWidth();
    int tileH = map->GetTileHeight();

    if (!astarPath.empty()) {
      for (const auto &pos : astarPath) {
        ScreenPosition sPos = map->GameCoordsToScreenCoords(pos.x, pos.y);
        DrawRectangleLines(sPos.x, sPos.y, tileW, tileH, RED);
      }
    }

    ecs.filter<const TargetPath>().each(
        [this, tileW, tileH](const TargetPath &targetPath) {
          for (const auto &pos : targetPath.path) {
            ScreenPosition sPos = map->GameCoordsToScreenCoords(pos.x, pos.y);
            DrawRectangleLines(sPos.x, sPos.y, tileW, tileH, BLUE);
          }
        });
  }

  camera.EndMode();
  EndDrawingGame();

  float scale = floorf(fminf((float)GetScreenWidth() / virtualWidth,
                             (float)GetScreenHeight() / virtualHeight));

  if (scale < 1.0f) {
    scale = 1.0f;
  }

  // Source rectangle (Note the negative height to flip it right-side up!)
  Rectangle sourceRec = {0.0f, 0.0f, (float)gameTexture.texture.width,
                         -(float)gameTexture.texture.height};

  Rectangle destRec = {0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()};
  DrawTexturePro(gameTexture.texture, sourceRec, destRec, {0, 0}, 0.0f, WHITE);

  DrawFPS(10, 10);
}

void Game::DrawFontSelectionWindow() {
  ImGui::Begin("Font Selection", &showFontSelectionWindow,
               ImGuiWindowFlags_AlwaysAutoResize);

  ImGui::Text("Available Fonts");
  ImGui::Separator();

  // Display combo box with available fonts
  if (!availableFontPaths.empty()) {
    // Create a display names vector for the combo box
    static std::vector<std::string> fontDisplayNames;
    
    if (fontDisplayNames.empty()) {
      for (const auto &path : availableFontPaths) {
        // Extract just the filename from the full path
        size_t lastSlash = path.find_last_of("/\\");
        std::string filename = (lastSlash == std::string::npos) 
                               ? path 
                               : path.substr(lastSlash + 1);
        fontDisplayNames.push_back(filename);
      }
    }

    // Create array of C-strings for ImGui::Combo
    std::vector<const char *> items;
    for (const auto &name : fontDisplayNames) {
      items.push_back(name.c_str());
    }

    if (ImGui::Combo("##FontList", &selectedFontIndex, items.data(),
                     items.size())) {
      // Font selection changed
      if (selectedFontIndex >= 0 && selectedFontIndex < (int)availableFontPaths.size()) {
        // Unload the old font if it was loaded
        if (gameFont.glyphCount > 0) {
          UnloadFont(gameFont);
        }
        
        // Load the new font
        const std::string &fontPath = availableFontPaths[selectedFontIndex];
        gameFont = LoadFontEx(fontPath.c_str(), DEFAULT_FONTSIZE, NULL, 0);
        SetTextureFilter(gameFont.texture, TEXTURE_FILTER_POINT);
        
        debugLog->LogInfo("Font loaded: " + fontPath);
      }
    }

    ImGui::Separator();

    // Display font preview
    if (selectedFontIndex >= 0 && selectedFontIndex < (int)fontDisplayNames.size()) {
      ImGui::Text("Preview:");
      ImGui::Text("%s", "The quick brown fox jumps over the lazy dog.");
      ImGui::Text("%s", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
      ImGui::Text("%s", "abcdefghijklmnopqrstuvwxyz");
      ImGui::Text("%s", "0123456789 !@#$^&*()");

      ImGui::Separator();
      if (ImGui::Button("Set to default")) {
        debugWindowState->SetDefaultFontPath(availableFontPaths[selectedFontIndex]);
        debugWindowState->SaveState("./debug_windows_state.json");
        if (debugLog) {
          debugLog->LogInfo("Default font set to: " + availableFontPaths[selectedFontIndex]);
        }
      }
    }
  } else {
    ImGui::TextDisabled("No fonts found in ./fonts directory");
  }

  ImGui::End();
}

void Game::BeginDrawingGame() { BeginTextureMode(gameTexture); }

void Game::EndDrawingGame() { EndTextureMode(); }
