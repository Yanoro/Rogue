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
#include "BlueprintExpansion.hpp"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
#include "TileRegistry.h"
#include "VisionChatWindow.h"

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

      // "Show Status" is the same kind of player-side shortcut, offered for any
      // character. It opens straight away rather than walking the player over
      // first, because reading a character sheet is inspection rather than an
      // action on the world.
      const bool hasShowStatus = contextMenuTarget.has<CharacterTag>();

      if (interactions.empty() && !hasSeeInventory && !hasShowStatus) {
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
        if (hasShowStatus) {
          if (!interactions.empty() || hasSeeInventory) {
            ImGui::Separator();
          }
          if (ImGui::Selectable(DEFAULT_SHOW_STATUS_OPTION)) {
            OpenCharacterStatusWindow(contextMenuTarget);
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

    // Blueprint placement preview: the design's real tiles and objects, drawn at
    // partial alpha and tinted by legality. It uses the real art rather than a
    // flat rectangle because the point of the debug window is to see what the
    // design *is* before committing it, and green/red still answers "can it go
    // here" at a glance.
    //
    // Drawn here, in the world pass, so it lands on top of the tile and entity
    // layers and under the ImGui windows -- the same reason the location preview
    // is here rather than in a window's Draw().
    if (!selectedBlueprintId.empty()) {
      if (auto* registryRes = ecs.get<BlueprintRegistryResource>()) {
        const Blueprint* blueprint =
            registryRes->registry != nullptr
                ? registryRes->registry->Get(selectedBlueprintId)
                : nullptr;
        if (blueprint != nullptr) {
          const BlueprintPlacement placement =
              BuildBlueprintPlacement(*blueprint, blueprintGhostOrigin);
          for (const BlueprintGhostCell& cell : placement.cells) {
            const ScreenPosition sPos =
                map->GameCoordsToScreenCoords(cell.position.x, cell.position.y);
            const Color tint =
                cell.legal ? Color{110, 255, 110, 130} : Color{255, 90, 90, 130};

            // The plate. Always drawn, so a carved cell and a cell with no art
            // both still read as part of the footprint.
            DrawRectangle(sPos.x, sPos.y, tileW, tileH, tint);

            if (cell.carve) {
              // An opening is shown as one, rather than as a tile.
              DrawRectangleLines(sPos.x, sPos.y, tileW, tileH,
                                 cell.legal ? GREEN : RED);
              continue;
            }

            // Resolve the art exactly the way the renderer does, by reusing the
            // real path helper on a throwaway entity: catalog "sprite", then the
            // data/sprites/tile_<name>.png convention, and nothing when neither
            // loads -- in which case the glyph below is what draws. Duplicating
            // that resolution here would be the surest way for the ghost to
            // disagree with what actually gets built.
            const char* glyph = nullptr;
            char glyphStorage = '?';
            const TileDef* definition = nullptr;
            if (!cell.tile.empty()) {
              definition = tileRegistry.Get(cell.tile);
            }
            if (definition != nullptr) {
              glyphStorage = definition->hasCharacter ? definition->character : '?';
              glyph = &glyphStorage;
            }
            // An object's art comes from its template, which the ghost does not
            // resolve; a placed fixture falls through to the plate and border
            // only, which still shows where it lands.

            std::string spritePath;
            if (!cell.tile.empty()) {
              flecs::entity probe = ecs.entity();
              AttachSprite(probe, definition != nullptr ? definition->sprite : "",
                           TileSpriteId(cell.tile), resourceManager, nullptr);
              if (const Sprite* sprite = probe.get<Sprite>()) {
                spritePath = sprite->path;
              }
              probe.destruct();
            }

            bool drewSprite = false;
            if (!spritePath.empty()) {
              if (raylib::Texture* texture = resourceManager.GetTexture(spritePath)) {
                // DrawSpriteFitted cannot tint, and the whole point of this
                // overlay is the tint, so the same fit is applied here with the
                // ghost's alpha. The layout maths comes from SpriteDraw.hpp.
                const SpriteRect fit = SpriteFitRect(*texture, sPos.x, sPos.y,
                                                     static_cast<float>(tileW),
                                                     static_cast<float>(tileH));
                const Rectangle source = {
                    0.0f, 0.0f, static_cast<float>(texture->width),
                    static_cast<float>(texture->height)};
                const Rectangle destination = {fit.x, fit.y, fit.width, fit.height};
                DrawTexturePro(*texture, source, destination, Vector2{0.0f, 0.0f},
                               0.0f, tint);
                drewSprite = fit.width > 0.0f;
              }
            }

            if (!drewSprite) {
              const char buffer[2] = {glyph != nullptr ? *glyph : '?', '\0'};
              const float fontSize = static_cast<float>(std::min(tileW, tileH));
              const Vector2 size = MeasureTextEx(gameFont, buffer, fontSize, 0);
              DrawTextEx(gameFont, buffer,
                         Vector2{std::round(sPos.x + (tileW - size.x) / 2.0f),
                                 std::round(sPos.y + (tileH - size.y) / 2.0f)},
                         fontSize, 0, tint);
            }
          }

          // The footprint border, so the extent of the design is readable even
          // when every cell is the same colour.
          const ScreenPosition originPos = map->GameCoordsToScreenCoords(
              blueprintGhostOrigin.x, blueprintGhostOrigin.y);
          DrawRectangleLinesEx(
              Rectangle{static_cast<float>(originPos.x),
                        static_cast<float>(originPos.y),
                        static_cast<float>(blueprint->Width() * tileW),
                        static_cast<float>(blueprint->Height() * tileH)},
              2.0f, placement.allLegal ? Color{80, 255, 80, 220}
                                       : Color{255, 80, 80, 220});
        }
      }
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

  // The one moment in the frame where the world is on screen and none of this
  // frame's ImGui exists in the framebuffer yet (ImGui rasterises in rlImGuiEnd).
  // That is what the Vision Chat window's "world only" screenshot needs. Placed
  // before DrawFPS so the counter is not part of the image either.
  if (visionChatWindow) {
    visionChatWindow->OnWorldFrameRendered();
  }

  DrawFPS(10, 10);
}



void Game::BeginDrawingGame() { BeginTextureMode(gameTexture); }

void Game::EndDrawingGame() { EndTextureMode(); }

Rectangle Game::GetWorldScreenRect() const {
  // The world is blitted at a fixed 1920x1080 destination (see Draw), so on a
  // smaller window it is clipped and on a larger one it leaves margins. Reporting
  // the intersection keeps a screenshot free of that empty border.
  const float worldWidth = static_cast<float>(gameTexture.texture.width);
  const float worldHeight = static_cast<float>(gameTexture.texture.height);
  const float screenWidth = static_cast<float>(GetScreenWidth());
  const float screenHeight = static_cast<float>(GetScreenHeight());
  return {0.0f, 0.0f, std::min(worldWidth, screenWidth),
          std::min(worldHeight, screenHeight)};
}
