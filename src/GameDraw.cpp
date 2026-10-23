#include "Components.h"
#include "Defaults.h"
#include "SpatialMemory.hpp"
#include "VisionQuery.hpp"
#include "Game.h"
#include "PathFinding.h"
#include "DebugLog.h"
#include "DrawAsciiDebug.h"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"
#include <iostream>
#include <sstream>
#include <cmath>
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

    // Stage 1 of docs/spatial-memory-design.md: what the character can see.
    //
    // The tint is the cached sweep (`Vision::lastVisible`) and never a fresh one:
    // an LOS pass over ~390 cells per frame per character is not a cost the render
    // path may have. The arc is drawn from the *cached* facing for the same reason
    // -- arc and tint must agree, or tuning the cone becomes guesswork.
    //
    // This draws over the world and hides nothing. Sight is an acquisition rule in
    // this phase, not fog of war (section 9), so an unlit barrel is still visible
    // to the player even though the character has not seen it.
    if (debugWindowState->GetShowVisionOverlay()) {
      ecs.filter<const Vision, const ScreenPosition, const Hitbox>().each(
          [this, tileW, tileH](const Vision &vision,
                               const ScreenPosition &screenPos,
                               const Hitbox &hitbox) {
            for (const GamePosition &cell : vision.lastVisible) {
              const ScreenPosition at =
                  map->GameCoordsToScreenCoords(cell.x, cell.y);
              DrawRectangle(static_cast<int>(at.x), static_cast<int>(at.y),
                            tileW, tileH, ColorAlpha(SKYBLUE, 0.18f));
            }

            const float centerX = screenPos.x + hitbox.width / 2.0f;
            const float centerY = screenPos.y + hitbox.height / 2.0f;
            const float rangePx = vision.range * static_cast<float>(tileW);
            const float centerAngle =
                std::atan2(vision.cachedFacing.y, vision.cachedFacing.x) * RAD2DEG;

            // The cached angle, not the component's standing value: a full-circle
            // survey samples 360 degrees once and restores the cone on the next
            // frame, and this arc must describe the sweep that tinted the cells.
            if (vision.cachedAngleDegrees >= 359.5f) {
              DrawCircleLines(static_cast<int>(centerX),
                              static_cast<int>(centerY), rangePx,
                              ColorAlpha(SKYBLUE, 0.45f));
            } else {
              const float half = vision.cachedAngleDegrees * 0.5f;
              DrawCircleSectorLines({centerX, centerY}, rangePx,
                                    centerAngle - half, centerAngle + half, 32,
                                    ColorAlpha(SKYBLUE, 0.45f));
            }
            if (vision.radarRadius > 0) {
              DrawCircleLines(static_cast<int>(centerX),
                              static_cast<int>(centerY),
                              vision.radarRadius * static_cast<float>(tileW),
                              ColorAlpha(SKYBLUE, 0.35f));
            }
          });
    }

    // Places, as the character's memory claims them (docs/spatial-memory-design.md
    // section 7.2): one outline for the extent, the member cells tinted inside it,
    // and a label above the box.
    //
    // The outline is a bounding box, so for an L-shaped cluster it can cover cells
    // the character never observed. The tinted members are drawn precisely for
    // that reason: the box says "this is the area", the tint says "this is what was
    // actually seen", and the gap between them is visible rather than implied.
    //
    // The character is the one selected in the Vision window's Memory tab -- the
    // same selection whose record and dump that tab shows. Nothing selected, or a
    // selected entity destroyed by a map reload, falls back to the player.
    //
    // The selection alone does not draw: the box is ticked per character
    // (Game::placeOverlayCharacters), and only a character whose own box is ticked
    // is drawn. Without that gate the single flag used to follow the selector to
    // every character, which made a per-character checkbox behave like a global.
    flecs::entity overlayEntity =
        placeOverlayEntity != 0 ? ecs.entity(placeOverlayEntity)
                                : flecs::entity::null();
    if (!overlayEntity.is_alive() || !overlayEntity.has<SpatialMemory>()) {
      overlayEntity = playerEntity;
    }
    if (overlayEntity.is_alive() &&
        placeOverlayCharacters.count(overlayEntity.id()) != 0) {
      if (const SpatialMemory *memory = overlayEntity.get<SpatialMemory>()) {
        for (const KnownPlace &place : memory->places) {
          const Color placeColor = Memory::PlaceColor(place.id);

          for (const std::uint64_t key : place.memberCells) {
            const auto cell = memory->cells.find(key);
            const GamePosition pos =
                cell == memory->cells.end() ? VisionQuery::CellFromKey(key)
                                            : cell->second.pos;
            const ScreenPosition at =
                map->GameCoordsToScreenCoords(pos.x, pos.y);

            // Tinted by the dominant aspect *at that cell*, not by place
            // identity: the point of the field is that it varies, and a flat
            // tint would hide exactly that. Fall back to the place colour for a
            // place with no sources (a named area).
            std::string aspect;
            const float value = Memory::DominantAspectAt(place, pos, &aspect);
            const Color tint = (value > 0.0f && !aspect.empty())
                                   ? Memory::CategoryColor(aspect)
                                   : placeColor;
            DrawRectangle(static_cast<int>(at.x), static_cast<int>(at.y), tileW,
                          tileH, ColorAlpha(tint, 0.18f + 0.22f * value));
          }

          const ScreenPosition origin =
              map->GameCoordsToScreenCoords(place.anchor.x, place.anchor.y);
          const Rectangle box = {origin.x, origin.y,
                                 place.width * static_cast<float>(tileW),
                                 place.height * static_cast<float>(tileH)};
          DrawRectangleLinesEx(box, 2.0f, placeColor);

          // Unnamed places show their id and surface, which is what the JSON
          // record calls them; a named one shows the name it was given.
          // Named places show their name; unnamed ones show the engine's own
          // vocabulary -- kind, and the strongest quality it can see -- so a box
          // can be matched to the JSON without hovering it.
          std::string label;
          if (!place.labels.empty()) {
            label = place.labels.front();
            if (place.labels.size() > 1) {
              label += " (+" + std::to_string(place.labels.size() - 1) + ")";
            }
          } else {
            label = "#" + std::to_string(place.id) + " " + place.primaryAspect;
            if (!place.aspects.empty()) {
              const PlaceAspect &strongest = place.aspects.front();
              label += ", " + strongest.category + " " +
                       std::to_string(static_cast<int>(strongest.peak * 100)) +
                       "%";
            }
          }
          const int fontSize = 12;
          const int textWidth = MeasureText(label.c_str(), fontSize);
          float textX = origin.x + (box.width - textWidth) / 2.0f;
          float textY = origin.y - fontSize - 4.0f;
          if (textY < 0.0f) {
            textY = origin.y + 2.0f;  // keep the label on screen at the top edge
          }
          if (textX < 0.0f) {
            textX = 0.0f;
          }
          DrawRectangle(static_cast<int>(textX) - 2, static_cast<int>(textY) - 1,
                        textWidth + 4, fontSize + 2,
                        ColorAlpha(BLACK, 0.55f));
          DrawText(label.c_str(), static_cast<int>(textX),
                   static_cast<int>(textY), fontSize, placeColor);
        }
      }
    }

    // Stage 0 of docs/spatial-memory-design.md: one short line per character
    // showing the direction `Facing` holds, so the direction the cone will later
    // be built on can be judged before anything is sampled from it.
    //
    // Drawn here, directly, for the same reason the two overlays above are: an
    // overlay that lives inside the render pipeline can perturb it, and this one
    // has to be switchable without attaching a component to anything.
    //
    // A marker is drawn for every entity that has all three components, not only
    // for characters -- the component is the opt-in, which keeps this consistent
    // with the system that writes it.
    if (debugWindowState->GetShowFacingMarkers()) {
      const float markerLength = tileW * DEFAULT_FACING_MARKER_TILE_FRACTION;
      ecs.filter<const Facing, const ScreenPosition, const Hitbox>().each(
          [markerLength](const Facing &facing, const ScreenPosition &screenPos,
                         const Hitbox &hitbox) {
            const ::Vector2 center = {
                screenPos.x + hitbox.width / 2.0f,
                screenPos.y + hitbox.height / 2.0f};
            const ::Vector2 tip = {center.x + facing.x * markerLength,
                                   center.y + facing.y * markerLength};
            DrawLineV(center, tip, YELLOW);
            // A dot at the tip, so a short marker reads as a direction rather
            // than a stray artefact at a glance.
            DrawCircleV(tip, 2.0f, YELLOW);
          });
    }

    // Place names, as stamps: one cell each, drawn while the naming prompt is
    // open so the user can see which cell the name will point at. The radius is
    // shown as a ring because that is what it is -- how far the name may spread
    // if it has to make a place -- not a rectangle it claims.
    if (pendingPlaceSeedWantsFocus || pendingPlaceSeedHasFocus) {
      const ScreenPosition at = map->GameCoordsToScreenCoords(
          pendingPlaceSeed.position.x, pendingPlaceSeed.position.y);
      DrawRectangle(static_cast<int>(at.x), static_cast<int>(at.y), tileW, tileH,
                    Color{255, 255, 0, 90});
      DrawRectangleLines(static_cast<int>(at.x), static_cast<int>(at.y), tileW,
                         tileH, YELLOW);
      const float centreX = at.x + tileW * 0.5f;
      const float centreY = at.y + tileH * 0.5f;
      DrawCircleLines(static_cast<int>(centreX), static_cast<int>(centreY),
                      static_cast<float>(pendingPlaceSeed.radius) * tileW,
                      ORANGE);
      DrawText(pendingPlaceSeed.label.empty() ? "(name this cell)"
                                              : pendingPlaceSeed.label.c_str(),
               static_cast<int>(at.x), static_cast<int>(at.y) - 16, 16, ORANGE);
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

    if (debugWindowState && debugWindowState->GetShowPlaceNames()) {
      // Place names are seeds, so there is no rectangle to draw: a cross marks
      // the cell they point at and a ring shows the radius they may spread to.
      Color nameColors[] = {RED,  GREEN, BLUE,   YELLOW,
                            MAGENTA, ORANGE, PURPLE, PINK};
      int colorIdx = 0;
      for (const PlaceSeed &seed : map->GetPlaceSeeds()) {
        const ScreenPosition sPos =
            map->GameCoordsToScreenCoords(seed.position.x, seed.position.y);
        const Color col = nameColors[colorIdx % 8];
        const float centreX = sPos.x + tileW * 0.5f;
        const float centreY = sPos.y + tileH * 0.5f;
        DrawCircleLines(static_cast<int>(centreX), static_cast<int>(centreY),
                        static_cast<float>(seed.radius) * tileW, col);
        DrawLineV({centreX - 6, centreY}, {centreX + 6, centreY}, col);
        DrawLineV({centreX, centreY - 6}, {centreX, centreY + 6}, col);
        DrawText(seed.label.c_str(), static_cast<int>(centreX) + 6,
                 static_cast<int>(centreY) - 18, 18, col);
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

  // raylib's DrawFPS hard-codes its colour (LIME, switching to ORANGE/RED below
  // 30/10 FPS), so draw the same text ourselves to keep the counter red.
  DrawText(TextFormat("%2i FPS", GetFPS()), 10, 10, 20, RED);
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
