#include "Game.h"
#include "AIChatWindow.h"
#include "AgentContextWindow.h"
#include "Components.h"
#include "DebugLog.h"
#include "DebugWindowState.h"
#include "Defaults.h"
#include "DrawAsciiDebug.h"
#include "EntityInfoWindow.h"
#include "MapReloader.h"
#include "PathFinding.h"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"
#include <algorithm>
#include <filesystem>
#include <iostream>

Game::Game() {}

Game::~Game() { Shutdown(); }

void Game::Init(std::string mapPath) {
  if (window.IsReady())
    return;

  mapFilePath = mapPath;

  // Do not set borderless flag, we will use proper fullscreen with explicit
  // monitor positioning
  raylib::Window::SetConfigFlags(FLAG_VSYNC_HINT);

  // TODO: Make this general
  window.Init(1920, 1080, "AIRogue");

  // Initialize debug systems early to read state
  debugLog = std::make_unique<DebugLog>();
  debugWindowState = std::make_unique<DebugWindowState>(debugLog.get());
  mapReloader = std::make_unique<MapReloader>("./", debugLog.get());

  // Load debug window state if it exists
  debugWindowState->LoadState("./debug_windows_state.json");

  // Remember that our font MUST be monofont
  std::string fontPathToLoad = DEFAULT_FONT_PATH;
  if (!debugWindowState->GetDefaultFontPath().empty() &&
      std::filesystem::exists(debugWindowState->GetDefaultFontPath())) {
    fontPathToLoad = debugWindowState->GetDefaultFontPath();
  }

  gameFont = LoadFontEx(fontPathToLoad.c_str(), DEFAULT_FONTSIZE, NULL, 0);
  SetTextureFilter(gameFont.texture, TEXTURE_FILTER_POINT);

  gameTexture = raylib::RenderTexture2D(1920, 1080);
  SetTextureFilter(gameTexture.texture, TEXTURE_FILTER_POINT);

  // TODO: Feels weird that map initialization occurs here
  ECSInit(mapPath);
  // virtualWidth = gameFont.baseSize * map->GetWidth();
  // virtualHeight = gameFont.baseSize * map->GetHeight();
  virtualWidth = 640;
  virtualHeight = 512;

  // Target the primary monitor (monitor 1) if there are multiple displays,
  // since monitor 0 is the side monitor
  currentMonitor = 0;
  if (GetMonitorCount() > 1) {
    currentMonitor = 1;
  }

  // Explicitly move the window to the correct monitor's origin before scaling
  window.SetPosition(GetMonitorPosition(currentMonitor).x,
                     GetMonitorPosition(currentMonitor).y);
  window.SetSize(GetMonitorWidth(currentMonitor),
                 GetMonitorHeight(currentMonitor));

  // Mouse offset removed. Raylib already reports window-relative coordinates.

  // Actually go fullscreen (locks correctly on the targeted monitor in most
  // X11 setups)
  ToggleFullscreen();

  window.SetTargetFPS(60);

  // Set the screen size to match our hardcoded monitor resolution (1920x1080)
  float screenWidth = 1920.0f;
  float screenHeight = 1080.0f;

  float mapWidthPx = map->GetMapWidthPx();
  float mapHeightPx = map->GetMapHeightPx();

  float zoomX = screenWidth / mapWidthPx;
  float zoomY = screenHeight / mapHeightPx;

  cameraMode = GameCameraMode::FollowMode;

  camera.target = {0, 0};
  camera.offset = {0, 0};
  camera.rotation = 0.0f;
  camera.zoom = std::max(1.0f, std::floor(std::min(zoomX, zoomY)));

  inputHandler = std::make_unique<InputHandler>(camera, cameraMode, mapWidthPx,
                                                mapHeightPx);

  // Discover available fonts in the ./fonts directory
  const std::string fontsDir = "./fonts";
  if (std::filesystem::exists(fontsDir) &&
      std::filesystem::is_directory(fontsDir)) {
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(fontsDir)) {
      if (entry.is_regular_file()) {
        std::string path = entry.path().string();
        std::string extension = path.substr(path.find_last_of(".") + 1);

        // Convert extension to lowercase for comparison
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       ::tolower);

        // Look for .ttf files
        if (extension == "ttf") {
          availableFontPaths.push_back(path);
          debugLog->LogInfo("Font discovered: " + path);
        }
      }
    }

    // Sort the font paths for consistent ordering
    std::sort(availableFontPaths.begin(), availableFontPaths.end());

    if (!availableFontPaths.empty()) {
      debugLog->LogInfo("Total fonts discovered: " +
                        std::to_string(availableFontPaths.size()));
      selectedFontIndex = 0; // Start with the first font
    }
  }

  debugLog->LogInfo("Game initialized successfully");

  rlImGuiSetup(true);
}

void Game::UpdateGUI() {
  // Finish a location drag even when the button comes up over a window, so the
  // drag can never be left dangling.
  if (isDraggingLocation && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
    isDraggingLocation = false;
    pendingLocation.topLeft = {std::min(locationDragStart.x, locationDragEnd.x),
                               std::min(locationDragStart.y, locationDragEnd.y)};
    pendingLocation.bottomRight = {std::max(locationDragStart.x, locationDragEnd.x),
                                   std::max(locationDragStart.y, locationDragEnd.y)};
    pendingLocation.name = "New Location";
    pendingLocation.description = "";
    pendingLocationWantsFocus = true;
    pendingLocationHasFocus = false;
  }

  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
      !ImGui::GetIO().WantCaptureMouse) {
    Vector2 mouseWorldPos = GetScreenToWorld2D(GetMousePosition(), camera);
    GamePosition gPos =
        map->ScreenCoordsToGameCoords(mouseWorldPos.x, mouseWorldPos.y);

    // Location drawing takes priority over whatever tile or object is selected:
    // while the mode is armed a left drag marks tiles instead of placing them.
    // The naming prompt blocks new drags so two rectangles cannot stack up.
    if (createLocationMode && !pendingLocationWantsFocus &&
        !pendingLocationHasFocus) {
      isDraggingLocation = true;
      locationDragStart = gPos;
      locationDragEnd = gPos;
      // Keep the marked rectangle around so the render pass can preview it.
      pendingLocation.topLeft = gPos;
      pendingLocation.bottomRight = gPos;
      return;
    }

    // Removal mode is checked before the tile/object selection branch, so a
    // click that hits a location deletes it instead of placing anything.
    if (removeLocationMode) {
      std::string removedName = map->RemoveLocationAt(gPos);
      if (!removedName.empty()) {
        lastRemovedLocationName = removedName;
        if (debugLog) {
          debugLog->LogInfo("Map editor: removed location '" + removedName +
                            "' at (" + std::to_string(gPos.x) + "," +
                            std::to_string(gPos.y) +
                            ") (not saved to the map file)");
        }
        return;
      }
    }

    // Trash mode: delete the object sitting on the clicked tile. Only
    // MapAuthored objects (map-file entries and editor placements) are eligible,
    // which excludes NPCs, the player, held items and anything the world spawned.
    if (removeObjectMode) {
      flecs::entity target = flecs::entity::null();
      ecs.filter<const GamePosition>().each([&](flecs::entity e,
                                               const GamePosition &pos) {
        if (target.is_alive() || pos.x != gPos.x || pos.y != gPos.y) {
          return;
        }
        if (e.has<MapAuthored>() && e.has<ItemType>()) {
          target = e;
        }
      });

      if (target.is_alive()) {
        const ItemType *itemType = target.get<ItemType>();
        std::string removedType = itemType ? itemType->id : "object";
        std::string removedLabel = removedType;
        if (const DisplayName *displayName = target.get<DisplayName>()) {
          removedLabel = displayName->name;
        }

        // Destructed after the query, never during it: destroying while iterating
        // would invalidate the filter's range.
        target.destruct();

        lastRemovedObjectName = removedLabel;
        if (debugLog) {
          debugLog->LogInfo("Map editor: removed object '" + removedType +
                            "' at (" + std::to_string(gPos.x) + "," +
                            std::to_string(gPos.y) + ")");
        }
        return;
      }
    }

    bool clickedWindow = false;
    auto ai = ecs.get<AIBackend>();
    ecs.defer_begin();
    ecs.filter<GamePosition, WindowOnClick>().each(
        [&gPos, &mouseWorldPos, ai, this, &clickedWindow](flecs::entity entity, const GamePosition &pos,
                          WindowOnClick &clickWin) {
          bool clickedOnEntity = false;
          if (const ScreenPosition *sPos = entity.get<ScreenPosition>()) {
            if (const Hitbox *hb = entity.get<Hitbox>()) {
              raylib::Rectangle rect(sPos->x, sPos->y, hb->width, hb->height);
              if (rect.CheckCollision(mouseWorldPos)) {
                clickedOnEntity = true;
              }
            }
          }
          if (!clickedOnEntity && gPos == pos) {
            clickedOnEntity = true;
          }

          if (clickedOnEntity) {
            clickedWindow = true;
            switch (clickWin.type) {
            case ::WindowType::AIChatWindowType:
              if (entity.has<ActiveWindow>()) {
                entity.remove<ActiveWindow>();
              } else {
                entity.set<ActiveWindow>(
                    {std::make_shared<AIChatWindow>(ai->ptr.get(), entity)});
              }
              break;
            case ::WindowType::EntityInfoWindowType:
              if (entity.has<ActiveWindow>()) {
                entity.remove<ActiveWindow>();
              } else {
                this->ecs.filter<WindowOnClick, ActiveWindow>().each(
                    [entity](flecs::entity other, WindowOnClick &otherWin,
                             ActiveWindow &) {
                      if (otherWin.type == ::WindowType::EntityInfoWindowType &&
                          other != entity) {
                        other.remove<ActiveWindow>();
                      }
                    });

                entity.set<ActiveWindow>(
                    {std::make_shared<EntityInfoWindow>(entity)});
              }
              break;
            case ::WindowType::NPCContextWindowType:
              if (entity.has<ActiveWindow>()) {
                entity.remove<ActiveWindow>();
              } else {
                entity.set<ActiveWindow>(
                    {std::make_shared<NPCContextWindow>(entity)});
              }
              break;
            default:
              break;
            }
          }
        });
    ecs.defer_end();

    if (isSelectingAStarPath) {
      if (astarClickCount == 0) {
        astarStartPos = gPos;
        astarClickCount++;
      } else if (astarClickCount == 1) {
        astarEndPos = gPos;
        astarClickCount = 0;
        isSelectingAStarPath = false;
        astarPath = AStar(map.get(), astarStartPos, astarEndPos);
      }
    } else if (isSettingPlayerTarget) {
      if (playerEntity.is_alive()) {
        if (const GamePosition *pPos = playerEntity.get<GamePosition>()) {
          std::vector<GamePosition> path = AStar(map.get(), *pPos, gPos);
          playerEntity.set<MOVE_THROUGH_PATH_ACTION>({path});
          if (playerEntity.has<PendingPlayerInteraction>()) {
            playerEntity.remove<PendingPlayerInteraction>();
          }
        }
      }
      isSettingPlayerTarget =
          false; // Disable the mode after setting the target
    } else if (editorSelection.type != EditorSelectionType::None && !clickedWindow) {
      if (editorSelection.type == EditorSelectionType::Tile) {
        Tile* newTile = nullptr;
        for (const auto& uTile : map->GetUniqueTiles()) {
          if (uTile->name == editorSelection.name) {
            newTile = uTile.get();
            break;
          }
        }
        if (newTile) {
          map->addTileToMap(newTile, gPos.x, gPos.y);
          
          // Update the visual representation
          ScreenPosition targetScreenPos = map->GameCoordsToScreenCoords(gPos.x, gPos.y);
          flecs::entity mapEntity = ecs.lookup("CurrentMap");
          if (mapEntity.is_valid()) {
            flecs::entity targetEntity = flecs::entity::null();
            ecs.filter<const ScreenPosition>().each(
                [&](flecs::entity e, const ScreenPosition& sPos) {
                  // Background tiles are children of Map and don't have GamePosition
                  if (e.parent() == mapEntity && !e.has<GamePosition>()) {
                    if (sPos.x == targetScreenPos.x && sPos.y == targetScreenPos.y) {
                      targetEntity = e;
                    }
                  }
                });
            
            if (targetEntity.is_alive()) {
              targetEntity.set<DrawAscii>(*newTile->ascii);
            }
          }
        }
      } else if (editorSelection.type == EditorSelectionType::ObjectTemplate) {
        flecs::entity mapEntity = ecs.lookup("CurrentMap");
        if (mapEntity.is_valid()) {
          flecs::entity existingObj = flecs::entity::null();
          ecs.filter<const GamePosition>().each([&](flecs::entity e, const GamePosition& pos) {
            if (pos.x == gPos.x && pos.y == gPos.y && e != playerEntity && !e.has<CharacterTag>()) {
               if (!existingObj.is_alive()) {
                 existingObj = e;
               }
            }
          });
          
          if (existingObj.is_alive()) {
            // Overwriting an entity with an editor template makes it authored
            // content, keyed to whatever the user just chose.
            existingObj.set<MapAuthored>({editorSelection.name});
            objectFactory.ApplyTemplate(existingObj, editorSelection.name, map.get());
          } else {
            // authored = true: editor placement must be written back by the map writer.
            objectFactory.SpawnObject(ecs, mapEntity, map.get(),
                                      editorSelection.name, gPos, true);
          }
        }
      }
    } else if (!clickedWindow) {
      double currentTime = GetTime();
      bool isDoubleClick = (currentTime - lastLeftClickTime < DEFAULT_DOUBLE_CLICK_TIME) && (lastClickedPos == gPos);
      lastLeftClickTime = currentTime;

      if (isDoubleClick) {
        if (playerEntity.is_alive()) {
          if (const GamePosition *pPos = playerEntity.get<GamePosition>()) {
            std::vector<GamePosition> path = AStar(map.get(), *pPos, gPos);
            if (!path.empty()) {
              playerEntity.set<MOVE_THROUGH_PATH_ACTION>({path});
              if (playerEntity.has<PendingPlayerInteraction>()) {
                playerEntity.remove<PendingPlayerInteraction>();
              }
            }
          }
        }
      }

      lastClickedPos = gPos;
      hasClicked = true;
      validTileSelected = false;

      ecs.filter<Tile, const GamePosition>().each(
          [this, gPos](flecs::entity e, const Tile &, const GamePosition &p) {
            if (p.x == gPos.x && p.y == gPos.y) {
              selectedTile = e;
              validTileSelected = true;
            }
          });
    }
  } else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !ImGui::GetIO().WantCaptureMouse) {
    Vector2 mouseWorldPos = GetScreenToWorld2D(GetMousePosition(), camera);
    GamePosition gPos = map->ScreenCoordsToGameCoords(mouseWorldPos.x, mouseWorldPos.y);

    bool found = false;
    ecs.filter<Interactable, const GamePosition>().each(
        [this, &gPos, &found](flecs::entity entity, const Interactable &, const GamePosition &pos) {
          if (!found && gPos.x == pos.x && gPos.y == pos.y) {
            contextMenuTarget = entity;
            openContextMenu = true;
            found = true;
          }
        });
  }

  // Track the drag past the press so the marked grid follows the mouse. The
  // pending rectangle is kept normalised (top-left / bottom-right) so the
  // preview and the naming prompt can read it directly.
  if (isDraggingLocation) {
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
      Vector2 dragWorldPos = GetScreenToWorld2D(GetMousePosition(), camera);
      locationDragEnd =
          map->ScreenCoordsToGameCoords(dragWorldPos.x, dragWorldPos.y);
      pendingLocation.topLeft = {std::min(locationDragStart.x, locationDragEnd.x),
                                 std::min(locationDragStart.y, locationDragEnd.y)};
      pendingLocation.bottomRight = {std::max(locationDragStart.x, locationDragEnd.x),
                                     std::max(locationDragStart.y, locationDragEnd.y)};
    } else {
      // The button is already up but the release branch above did not see it
      // — drop the drag rather than leaving it dangling.
      isDraggingLocation = false;
    }
  }
}

void Game::Update() {
  UpdateGUI();
  ecs.progress();
}

void Game::handleInput() {
  std::vector<Command *> commands = inputHandler->handleInput();
  for (Command *cmd : commands) {
    if (cmd) {
      cmd->execute(playerEntity);
    }
  }
}

bool Game::SaveMapToFile() {
  if (!map) {
    if (debugLog) {
      debugLog->LogError("Cannot save map: no map is loaded.");
    }
    return false;
  }

  if (mapFilePath.empty()) {
    if (debugLog) {
      debugLog->LogError("Cannot save map: the map file path is unknown.");
    }
    return false;
  }

  return map->SaveToFile(mapFilePath);
}

void Game::Shutdown() {
  if (!window.IsReady())
    return;

  // Unload the game font before closing
  if (gameFont.glyphCount > 0) {
    UnloadFont(gameFont);
  }

  if (debugWindowState) {
    debugWindowState->SetShowDebugConsole(
        debugConsoleWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowEntityInfoWindow(playerEntity.has<ActiveWindow>());
    debugWindowState->SetShowTileInfoWindow(
        tileInfoWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowAStarWindow(astarWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowEntityOverviewWindow(
        entityOverviewWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowDebugLogWindow(
        debugLogWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowMapReloadWindow(
        mapReloadWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowDrawAsciiToggleWindow(
        drawAsciiToggleWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowFontSelectionWindow(
        fontSelectionWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowMapEditorWindow(
        mapEditorWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowNPCMenuWindow(
        npcMenuWindowEntity.has<ActiveWindow>());

    debugWindowState->SaveState("./debug_windows_state.json");
    if (debugLog) {
      debugLog->LogInfo("Debug window state saved on shutdown");
    }
  }

  // Gracefully stop all NPC threads before destroying the flecs world.
  // This prevents race conditions where NPC threads might query flecs
  // components (like TargetPath) while flecs is in the middle of being torn
  // down.
  // TODO: Probably unnecessary , try removing later
  // ecs.filter<NPCComponent>().each([](flecs::entity, NPCComponent &npc) {
  //   if (npc.ptr) {
  //     npc.ptr.reset();
  //   }
  // });

  rlImGuiShutdown();
  ecs.quit();
  window.Close();
}

bool Game::shouldClose() const { return window.ShouldClose(); }
