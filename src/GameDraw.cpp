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
#include "InteractionRegistry.h"

void Game::DrawGameWindows() {
  // Draw any active component-based UI windows
  ecs.defer([&]() {
    ecs.filter<ActiveWindow>().each([](ActiveWindow &win) {
      if (win.ptr) {
        win.ptr->Draw();
      }
    });
  });

  if (openContextMenu) {
    ImGui::OpenPopup("ObjectContextMenu");
    openContextMenu = false; // Reset so it doesn't keep opening
  }

  if (ImGui::BeginPopup("ObjectContextMenu")) {
    if (contextMenuTarget.is_alive()) {
      auto interactions = InteractionRegistry::GetAvailableInteractions(contextMenuTarget);
      
      std::string objName = "Object";
      if (contextMenuTarget.has<DisplayName>()) {
        objName = contextMenuTarget.get<DisplayName>()->name;
      }
      ImGui::TextDisabled("%s", objName.c_str());
      ImGui::Separator();

      // Any option first walks the player to the object, then runs on arrival
      // (see the MOVE_THROUGH_PATH_ACTION system in GameECS.cpp).
      auto selectInteraction = [this](const std::string &interactionName) {
        // Cancel any old interaction first
        playerEntity.remove<PendingPlayerInteraction>();

        // Pathfind to the object
        if (const GamePosition *pPos = playerEntity.get<GamePosition>()) {
          if (const GamePosition *tPos = contextMenuTarget.get<GamePosition>()) {
            std::vector<GamePosition> path = AStar(map.get(), *pPos, *tPos);
            playerEntity.set<MOVE_THROUGH_PATH_ACTION>({path});
            playerEntity.set<PendingPlayerInteraction>(
                {contextMenuTarget, interactionName});
          }
        }
      };

      // "See Inventory" is a player-side QOL shortcut for anything that holds
      // items: Storage containers and characters. It is not an
      // InteractionRegistry entry on purpose, so it never shows up in the
      // commands the AI is offered or can issue.
      const bool hasSeeInventory = contextMenuTarget.has<Storage>() ||
                                   contextMenuTarget.has<CharacterTag>();

      if (interactions.empty() && !hasSeeInventory) {
        ImGui::Text("No actions available");
      } else {
        for (const auto& interaction : interactions) {
          if (ImGui::Selectable(interaction.name.c_str())) {
            selectInteraction(interaction.name);
          }
        }
        if (hasSeeInventory) {
          if (!interactions.empty()) {
            ImGui::Separator();
          }
          if (ImGui::Selectable(DEFAULT_SEE_INVENTORY_OPTION)) {
            selectInteraction(DEFAULT_SEE_INVENTORY_OPTION);
          }
        }
      }
    }
    ImGui::EndPopup();
  }
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

    ecs.filter<const MOVE_THROUGH_PATH_ACTION>().each(
        [this, tileW, tileH](const MOVE_THROUGH_PATH_ACTION &targetPath) {
          for (const auto &pos : targetPath.path) {
            ScreenPosition sPos = map->GameCoordsToScreenCoords(pos.x, pos.y);
            DrawRectangleLines(sPos.x, sPos.y, tileW, tileH, BLUE);
          }
        });

    // Live preview of the location rectangle currently being dragged out (and
    // of the one waiting to be named, so the user can see what they marked).
    if (isDraggingLocation || pendingLocationWantsFocus || pendingLocationHasFocus) {
      auto markedTiles = map->GetTilePositionsInRect(
          pendingLocation.topLeft,
          pendingLocation.bottomRight.x - pendingLocation.topLeft.x + 1,
          pendingLocation.bottomRight.y - pendingLocation.topLeft.y + 1);

      for (const auto &pos : markedTiles) {
        ScreenPosition sPos = map->GameCoordsToScreenCoords(pos.x, pos.y);
        DrawRectangle(sPos.x, sPos.y, tileW, tileH, Color{255, 255, 0, 90});
        DrawRectangleLines(sPos.x, sPos.y, tileW, tileH, YELLOW);
      }

      ScreenPosition boxPos =
          map->GameCoordsToScreenCoords(pendingLocation.topLeft.x,
                                        pendingLocation.topLeft.y);
      int boxWidth =
          (pendingLocation.bottomRight.x - pendingLocation.topLeft.x + 1) * tileW;
      int boxHeight =
          (pendingLocation.bottomRight.y - pendingLocation.topLeft.y + 1) * tileH;
      DrawRectangleLinesEx(Rectangle{static_cast<float>(boxPos.x),
                                     static_cast<float>(boxPos.y),
                                     static_cast<float>(boxWidth),
                                     static_cast<float>(boxHeight)},
                           3.0f, ORANGE);
    }

    if (debugWindowState && debugWindowState->GetShowLocations()) {
      const auto& locations = map->GetLocations();
      Color locationColors[] = { RED, GREEN, BLUE, YELLOW, MAGENTA, ORANGE, PURPLE, PINK };
      int colorIdx = 0;
      for (const auto& loc : locations) {
        ScreenPosition sPos = map->GameCoordsToScreenCoords(loc->pos.x, loc->pos.y);
        int rectWidth = loc->width * tileW;
        int rectHeight = loc->height * tileH;
        Color col = locationColors[colorIdx % 8];
        
        // Draw the bounding box
        DrawRectangleLines(sPos.x, sPos.y, rectWidth, rectHeight, col);
        
        // Draw the location name in the center
        int fontSize = 20;
        int textWidth = MeasureText(loc->name.c_str(), fontSize);
        int textX = sPos.x + (rectWidth - textWidth) / 2;
        int textY = sPos.y + (rectHeight - fontSize) / 2;
        DrawText(loc->name.c_str(), textX, textY, fontSize, col);
        
        colorIdx++;
      }
    }
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

  Rectangle destRec = {0.0f,
                       0.0f,
                       1920.0f,
                       1080.0f};
  DrawTexturePro(gameTexture.texture, sourceRec, destRec, {0, 0}, 0.0f, WHITE);

  DrawFPS(10, 10);
}



void Game::BeginDrawingGame() { BeginTextureMode(gameTexture); }

void Game::EndDrawingGame() { EndTextureMode(); }
