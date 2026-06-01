#include "Game.h"
#include "Components.h"
#include "PathFinding.h"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

void Game::DrawPlayerInfoWindow() {
  ImGui::Begin("Player Entity");
  if (playerEntity.is_alive()) {
    if (const GamePosition *gPos = playerEntity.get<GamePosition>()) {
      ImGui::Text("Game Position: (%d, %d)", gPos->x, gPos->y);
    }
    if (const ScreenPosition *sPos = playerEntity.get<ScreenPosition>()) {
      ImGui::Text("Screen Position: (%.2f, %.2f)", sPos->x, sPos->y);
    }
    if (const TargetPath *targetPath = playerEntity.get<TargetPath>()) {
      if (!targetPath->path.empty()) {
        const GamePosition &lastPos = targetPath->path.back();
        ImGui::Text("Moving to: (%d, %d)", lastPos.x, lastPos.y);
      }
    }
    if (const Velocity *vel = playerEntity.get<Velocity>()) {
      ImGui::Text("Velocity: (%.2f, %.2f)", vel->x, vel->y);
    } else {
      ImGui::Text("Velocity: (0.00, 0.00)");
    }
    if (const Acceleration *accel = playerEntity.get<Acceleration>()) {
      ImGui::Text("Acceleration: (%.2f, %.2f)", accel->x, accel->y);
    } else {
      ImGui::Text("Acceleration: (0.00, 0.00)");
    }
    if (const MaxSpeed *speed = playerEntity.get<MaxSpeed>()) {
      ImGui::Text("Max Speed: %.2f", speed->value);
    }
    if (const Friction *friction = playerEntity.get<Friction>()) {
      ImGui::Text("Friction: %.2f", friction->value);
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

    const Acceleration *accel = playerEntity.get<Acceleration>();

    if (accel && maxSpeed && maxSpeed->value > 0.0f) {
      // Scale relative to max speed (can be tuned later if acceleration exceeds
      // this)
      float scaleX = accelMaxRadius / (maxSpeed->value * 5.0f);
      float scaleY = accelMaxRadius / (maxSpeed->value * 5.0f);
      ImVec2 accelEnd(accelCenter.x + accel->x * scaleX,
                      accelCenter.y + accel->y * scaleY);

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

void Game::DrawGameWindows() {

  DrawPlayerInfoWindow();

  DrawTileInfoWindow();

  DrawAStarWindow();
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
