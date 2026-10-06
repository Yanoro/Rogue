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
#include "Settings.h"
#include "SpritePaths.hpp"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"
#include <algorithm>
#include <filesystem>
#include <iostream>

Game::Game() {}

Game::~Game() { Shutdown(); }

void Game::Init() {
  if (window.IsReady())
    return;

  // Everything below reads from the one settings object main() filled in. Taken
  // by reference rather than copied so there is exactly one place a setting can
  // be read from.
  const GameSettings &settings = Settings();

  mapFilePath = settings.map.file;

  // Do not set borderless flag, we will use proper fullscreen with explicit
  // monitor positioning
  raylib::Window::SetConfigFlags(settings.window.vsync ? FLAG_VSYNC_HINT : 0);

  window.Init(settings.window.width, settings.window.height,
              settings.window.title.c_str());

  // Initialize debug systems early to read state
  debugLog = std::make_unique<DebugLog>(
      static_cast<size_t>(settings.debug.logMaxEntries));
  debugWindowState = std::make_unique<DebugWindowState>(debugLog.get());
  mapReloader = std::make_unique<MapReloader>(settings.map.mapsDirectory,
                                              debugLog.get());

  // Load debug window state if it exists
  debugWindowState->LoadState(settings.debug.windowStateFile);

  // Remember that our font MUST be monofont
  std::string fontPathToLoad = settings.render.fontPath;
  if (!debugWindowState->GetDefaultFontPath().empty() &&
      std::filesystem::exists(debugWindowState->GetDefaultFontPath())) {
    fontPathToLoad = debugWindowState->GetDefaultFontPath();
  }

  gameFont =
      LoadFontEx(fontPathToLoad.c_str(), settings.render.fontSize, NULL, 0);
  SetTextureFilter(gameFont.texture, TEXTURE_FILTER_POINT);

  // Art cells are drawn at their own size from their own rasterization, and with
  // the CP437 block, shade and box-drawing codepoints that raylib's default
  // ASCII-only load leaves out. Both are what let a cell carry sub-cell detail;
  // AsciiArtFonts.hpp explains why that matters at 8-10px.
  artFonts.Load(fontPathToLoad);
  {
    const std::string coverage = artFonts.CoverageWarning();
    if (coverage.empty()) {
      debugLog->LogInfo("Art fonts: " +
                        std::to_string(ArtGlyphCodepoints().size()) +
                        " glyph codepoints rasterized at " +
                        std::to_string(AsciiArtFonts::kSizeCount) + " sizes");
    } else {
      debugLog->LogWarning("Art fonts: " + coverage);
    }
  }

  gameTexture = raylib::RenderTexture2D(1920, 1080);
  SetTextureFilter(gameTexture.texture, TEXTURE_FILTER_POINT);

  // TODO: Feels weird that map initialization occurs here
  ECSInit();
  // virtualWidth = gameFont.baseSize * map->GetWidth();
  // virtualHeight = gameFont.baseSize * map->GetHeight();
  virtualWidth = 640;
  virtualHeight = 512;

  // window.monitor >= 0 forces that display. -1 keeps the original rule: the
  // primary (monitor 1) when several are connected, because monitor 0 is the
  // side monitor on the author's desk.
  int monitor = settings.window.monitor;
  if (monitor < 0) {
    monitor = GetMonitorCount() > 1 ? 1 : 0;
  }
  if (monitor >= GetMonitorCount()) {
    debugLog->LogWarning("Settings: window.monitor " + std::to_string(monitor) +
                         " does not exist (" +
                         std::to_string(GetMonitorCount()) +
                         " connected); using monitor 0.");
    monitor = 0;
  }
  currentMonitor = monitor;

  if (settings.window.fullscreen) {
    // Explicitly move the window to the correct monitor's origin before scaling
    window.SetPosition(GetMonitorPosition(currentMonitor).x,
                       GetMonitorPosition(currentMonitor).y);
    window.SetSize(GetMonitorWidth(currentMonitor),
                   GetMonitorHeight(currentMonitor));

    // Mouse offset removed. Raylib already reports window-relative coordinates.

    // Actually go fullscreen (locks correctly on the targeted monitor in most
    // X11 setups)
    ToggleFullscreen();
  }

  window.SetTargetFPS(settings.window.targetFPS);

  // The camera fits the map to this reference resolution. It is the configured
  // window size rather than the live screen size, which is what the original
  // hardcoded 1920x1080 pair amounted to.
  float screenWidth = static_cast<float>(settings.window.width);
  float screenHeight = static_cast<float>(settings.window.height);

  float mapWidthPx = map->GetMapWidthPx();
  float mapHeightPx = map->GetMapHeightPx();

  float zoomX = screenWidth / mapWidthPx;
  float zoomY = screenHeight / mapHeightPx;

  cameraMode = settings.render.cameraMode == "free" ? GameCameraMode::FreeRoamMode
                                                    : GameCameraMode::FollowMode;

  camera.target = {0, 0};
  camera.offset = {0, 0};
  camera.rotation = 0.0f;
  // 0 means "fit the map to the window". Any other value is taken literally, so
  // a fixed zoom can be authored without the fit maths fighting it.
  camera.zoom = settings.render.cameraZoom > 0.0f
                    ? settings.render.cameraZoom
                    : std::max(1.0f, std::floor(std::min(zoomX, zoomY)));

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

  // A one-line record of what the run actually started from, so a surprising
  // session can be explained from the Debug Log without opening the JSON. The
  // API key is deliberately reduced to present/absent.
  debugLog->LogInfo("Settings: map=" + settings.map.file + " spawnNPCs=" +
                    (settings.map.spawnNPCs ? "true" : "false") + " model=" +
                    settings.model.type + "/" + settings.model.model +
                    (settings.model.apiKey.empty() ? " (no api key)"
                                                   : " (api key present)") +
                    " npcPrompt=" +
                    (settings.ai.npcPromptTemplate.empty()
                         ? std::string("built-in")
                         : settings.ai.npcPromptFile));

  debugLog->LogInfo("Game initialized successfully");

  rlImGuiSetup(true);

  // ImGui renders with its own font, which is ASCII-only, so without this the
  // windows that preview art (the map editor's object palette, inventory rows)
  // would show blanks wherever the art uses a block character. ImGui's built-in
  // font cannot be extended, so this is a second font added from the same file.
  ArtImGuiFont() = AsciiArtFonts::AddImGuiArtFont(fontPathToLoad, 16.0f);
  if (ArtImGuiFont() == nullptr) {
    debugLog->LogWarning(
        "Art fonts: no ImGui art font could be added, so art that uses block "
        "characters will show blanks in the map editor palette.");
  }
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
              // The sprite is display state like the glyph, so the previous
              // tile's texture is cleared before the new one is resolved --
              // otherwise painting wooden floor over grass would keep the grass
              // texture and only swap the glyph underneath it.
              targetEntity.remove<Sprite>();
              AttachSprite(targetEntity, newTile->sprite,
                           TileSpriteId(newTile->name), resourceManager,
                           debugLog.get());
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

      // Map tiles are the Map's own children, tagged MapTileVisual and given a
      // ScreenPosition when the map loads; they carry no Tile component and no
      // GamePosition. Matching on those two -- as this used to -- therefore never
      // selected anything, so every consumer saw "no valid tile". Resolve the
      // entity by the screen position it was created at instead.
      const ScreenPosition clickedScreenPos =
          map->GameCoordsToScreenCoords(gPos.x, gPos.y);
      ecs.filter<MapTileVisual, const ScreenPosition>().each(
          [this, &clickedScreenPos](flecs::entity e, MapTileVisual,
                                    const ScreenPosition &sPos) {
            if (sPos == clickedScreenPos) {
              selectedTile = e;
              validTileSelected = true;
            }
          });
    }
  } else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !ImGui::GetIO().WantCaptureMouse) {
    Vector2 mouseWorldPos = GetScreenToWorld2D(GetMousePosition(), camera);
    GamePosition gPos = map->ScreenCoordsToGameCoords(mouseWorldPos.x, mouseWorldPos.y);

    bool found = false;
    // Both interactable objects and characters open the player context menu.
    // The player itself is excluded: its own inventory is not exposed this way.
    ecs.filter<const GamePosition>().each(
        [this, &gPos, &found](flecs::entity entity, const GamePosition &pos) {
          if (found || entity == playerEntity) {
            return;
          }
          if (gPos.x != pos.x || gPos.y != pos.y) {
            return;
          }
          if (!entity.has<Interactable>() && !entity.has<CharacterTag>()) {
            return;
          }
          contextMenuTarget = entity;
          openContextMenu = true;
          found = true;
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

  // And every art-cell rasterization. UnloadFont is a no-op for raylib's default
  // font, which is what a failed ladder load leaves behind.
  artFonts.Unload();

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

    debugWindowState->SaveState(Settings().debug.windowStateFile);
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
