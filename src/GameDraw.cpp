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

void Game::DrawGameWindows() {
  // Draw any active component-based UI windows
  ecs.filter<ActiveWindow>().each([](ActiveWindow &win) {
    if (win.ptr) {
      win.ptr->Draw();
    }
  });
}


void Game::Draw() {
  if (cameraMode == GameCameraMode::FollowMode) {
    const ScreenPosition *playerPos = playerEntity.get<ScreenPosition>();
    camera.target = {playerPos->x, playerPos->y};
  }

  float targetWidth = static_cast<float>(gameTexture.texture.width);
  float targetHeight = static_cast<float>(gameTexture.texture.height);

  if (cameraMode == GameCameraMode::FollowMode) {
    camera.offset = {targetWidth / 2.0f, targetHeight / 2.0f};
  } else {
    camera.offset = {0.0f, 0.0f};
  }

  int mapWidthPx = map->GetMapWidthPx();
  int mapHeightPx = map->GetMapHeightPx();

  float halfVisibleWidth = camera.offset.x / camera.zoom;
  float maxVisibleWidth = (targetWidth - camera.offset.x) / camera.zoom;
  if (mapWidthPx * camera.zoom > targetWidth) {
    camera.target.x = std::clamp(camera.target.x, halfVisibleWidth,
                                 mapWidthPx - maxVisibleWidth);
  } else {
    camera.target.x = mapWidthPx / 2.0f - (targetWidth / 2.0f - camera.offset.x) / camera.zoom;
  }

  float halfVisibleHeight = camera.offset.y / camera.zoom;
  float maxVisibleHeight = (targetHeight - camera.offset.y) / camera.zoom;
  if (mapHeightPx * camera.zoom > targetHeight) {
    camera.target.y = std::clamp(camera.target.y, halfVisibleHeight,
                                 mapHeightPx - maxVisibleHeight);
  } else {
    camera.target.y = mapHeightPx / 2.0f - (targetHeight / 2.0f - camera.offset.y) / camera.zoom;
  }

  BeginDrawingGame();
  ClearBackground(GRAY); // Clear the render texture

  camera.BeginMode();

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

  Rectangle destRec = {GetMonitorPosition(currentMonitor).x,
                       GetMonitorPosition(currentMonitor).y,
                       1920.0f,
                       1080.0f};
  DrawTexturePro(gameTexture.texture, sourceRec, destRec, {0, 0}, 0.0f, WHITE);

  DrawFPS(10, 10);
}



void Game::BeginDrawingGame() { BeginTextureMode(gameTexture); }

void Game::EndDrawingGame() { EndTextureMode(); }
