#include <iostream>
#include "Components.h"
#include "Game.h"
#include "PathFinding.h"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

void Game::DrawPlayerInfoWindow() {
  ImGui::Begin("Player Entity");
  if (playerEntity.is_alive()) {
    if (const GamePosition *gPos = playerEntity.get<GamePosition>()) {
      int pos[2] = {gPos->x, gPos->y};
      if (ImGui::InputInt2("Game Position", pos)) {
        playerEntity.set<GamePosition>({pos[0], pos[1]});
        if (map) {
           playerEntity.set<ScreenPosition>(map->GameCoordsToScreenCoords(pos[0], pos[1]));
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
    if (const Velocity *vel = playerEntity.get<Velocity>()) {
      float v[2] = {vel->x, vel->y};
      if (ImGui::DragFloat2("Velocity", v)) {
        playerEntity.set<Velocity>({v[0], v[1]});
      }
    } else {
      ImGui::Text("Velocity: (0.00, 0.00)");
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
    ImGui::Text("Velocity Vector:");

    ImVec2 canvasSize(120.0f, 120.0f);
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

    // Get current velocity and max speed
    const Velocity *vel = playerEntity.get<Velocity>();
    const MaxSpeed *maxSpeed = playerEntity.get<MaxSpeed>();

    if (vel && maxSpeed && maxSpeed->value > 0.0f) {
      // Calculate scaled endpoint
      float scaleX = maxRadius / maxSpeed->value;
      float scaleY = maxRadius / maxSpeed->value;
      ImVec2 velEnd(center.x + vel->x * scaleX, center.y + vel->y * scaleY);

      // Draw velocity vector line and arrowhead/dot
      drawList->AddLine(center, velEnd, IM_COL32(50, 255, 50, 255), 2.0f);
      drawList->AddCircleFilled(velEnd, 3.0f, IM_COL32(50, 255, 50, 255));
    }

    // Center point
    drawList->AddCircleFilled(center, 2.0f, IM_COL32(255, 255, 255, 255));

    ImGui::Separator();
    ImGui::Text("Acceleration Vector:");

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

  ImGui::Begin("Tile Info");

  if (hasClicked) {

    ImGui::Text("Map Coordinates: (%d, %d)", lastClickedPos.x,
                lastClickedPos.y);

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
  ImGui::Begin("A*");

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

  ImGui::Begin("Entity Overview");

  // Optional: Add a text filter
  static ImGuiTextFilter filter;
  filter.Draw("Filter");

  ImGui::Separator();

  // Begin a table for better formatting
  if (ImGui::BeginTable("EntitiesTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
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
        if (const GamePosition* pos = e.get<GamePosition>()) {
          ImGui::Text("(%d, %d)", pos->x, pos->y);
        } else if (const ScreenPosition* sPos = e.get<ScreenPosition>()) {
          ImGui::Text("Screen(%.1f, %.1f)", sPos->x, sPos->y);
        } else {
          ImGui::Text("-");
        }

        // 4. Components Column
        ImGui::TableNextColumn();
        
        e.each([&](flecs::id id) {
            std::string label = std::string(id.str().c_str()) + "##" + std::to_string(e.id()) + "_" + std::to_string(id.raw_id());
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
  for (auto it = openComponentWindows.begin(); it != openComponentWindows.end(); ) {
      bool open = true;
      std::string winName = "Component: " + std::string(it->id.str().c_str()) + " (" + std::string(it->entity.name().c_str() ? it->entity.name().c_str() : "Unnamed") + ")##" + std::to_string(it->entity.id()) + "_" + std::to_string(it->id.raw_id());
      
      ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_FirstUseEver);
      if (ImGui::Begin(winName.c_str(), &open)) {
          const void* ptr = it->entity.get(it->id.raw_id());
          if (ptr) {
              char* json = ecs_ptr_to_json(ecs.c_ptr(), it->id.raw_id(), ptr);
              if (json) {
                  ImGui::TextWrapped("%s", json);
                  ecs_os_free(json);
              } else {
                  ImGui::TextDisabled("No JSON representation available.");
              }
          }
          else {
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

void Game::DrawGameWindows() {

  DrawPlayerInfoWindow();

  DrawTileInfoWindow();

  DrawAStarWindow();

  DrawEntityOverviewWindow();
}

void Game::Draw() {
  camera.BeginMode();
  ecs.run_pipeline(renderPipeline);

  if (map) {
    int tileW = map->getTileWidth();
    int tileH = map->getTileHeight();

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
}
