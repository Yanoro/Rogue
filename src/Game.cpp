#include "Game.h"
#include "AIChatWindow.h"
#include "AgentContextWindow.h"
#include "BlueprintExpansion.hpp"
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
#include "TileDesigner.hpp"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

// The tile-art queue's completion callback. Declared here because Game::ECSInit
// hands the queue a pointer to it; defined at the bottom with the rest of the
// queue integration.
void OnTileArtCompleted(void *context, const TileArtQueue::Job &job);

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

  // Opened here rather than where speech is set up: raylib's audio device is
  // process-wide, everything that might play anything needs it to exist first, and
  // this is the first point where there is a debug log to complain to.
  //
  // Silence is the failure mode, deliberately. No device -- a headless box, a busy
  // PipeWire, a locked session -- must not stop the game from starting, so
  // SpeechQueue::Init checks IsAudioDeviceReady() and refuses to enqueue rather
  // than the game dying on the first utterance.
  InitAudioDevice();
  if (!IsAudioDeviceReady()) {
    debugLog->LogWarning(
        "Audio: no audio device could be opened; text to speech will be silent.");
  }

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

  // The blueprint ghost follows the mouse continuously, not only on click, so the
  // preview has to be re-anchored every frame. Deliberately outside the
  // button-pressed branch below: a ghost that only moved when you clicked would
  // be useless for judging a site.
  if (!selectedBlueprintId.empty() && map && !ImGui::GetIO().WantCaptureMouse) {
    const Vector2 ghostWorld = GetScreenToWorld2D(GetMousePosition(), camera);
    blueprintGhostOrigin = map->ScreenCoordsToGameCoords(ghostWorld.x, ghostWorld.y);
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

    // Blueprint placement mode. The ghost is re-anchored to the mouse every
    // frame, and this is the one place the mouse is read, so the preview and the
    // commit always describe the same site. A left click commits when the site is
    // legal; an illegal click is reported rather than silently ignored, because
    // "nothing happened" is the least useful possible reply.
    if (!selectedBlueprintId.empty()) {
      if (auto *registryRes = ecs.get<BlueprintRegistryResource>()) {
        const Blueprint *blueprint = registryRes->registry != nullptr
                                         ? registryRes->registry->Get(selectedBlueprintId)
                                         : nullptr;
        if (blueprint != nullptr) {
          blueprintGhostOrigin = gPos;
          const BlueprintPlacement placement =
              BuildBlueprintPlacement(*blueprint, gPos);
          if (placement.allLegal) {
            lastBlueprintPlacementMessage =
                PlaceBlueprintAt(*blueprint, gPos);
          } else {
            lastBlueprintPlacementMessage =
                "Cannot build " + blueprint->name + " at (" +
                std::to_string(gPos.x) + "," + std::to_string(gPos.y) +
                "): the site is blocked. See the Blueprints window for why.";
          }
          return;
        }
      }
      // The selected id no longer resolves (registry reloaded, say): drop the
      // mode rather than leaving a ghost that cannot be placed.
      selectedBlueprintId.clear();
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
        PaintTile(gPos, editorSelection.name);
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
  // The speech pump lives here, not in the Designer window's Draw(): Draw only
  // runs while that window is visible, so a pane closed to watch the world would
  // silence the speech mid-sentence and it would look like the device failing.
  if (speechQueue) {
    speechQueue->Update();
  }
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
    debugWindowState->SetShowBlueprintWindow(
        blueprintWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowDesignerWindow(
        designerWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowTileDesignerWindow(
        tileDesignerWindowEntity.has<ActiveWindow>());
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

  // Speech goes down before the audio device does. Shutdown() joins the worker,
  // which may be blocked inside a piper child waiting for output, so the child is
  // killed and reaped here rather than left to outlive the game; and the Sound it
  // produced is unloaded while the device is still up.
  //
  // The tile-art worker joins before that, and before the ECS quits: a batch in
  // flight will post a completion whose callback touches the world, so it must be
  // waited on while the world is still valid. Bounded inside Stop() -- a request
  // can take tens of seconds, and a game that will not close is its own bug.
  tileArtQueue.Stop();
  speechQueue.reset();
  speechBackend.reset();
  CloseAudioDevice();

  rlImGuiShutdown();
  ecs.quit();
  window.Close();
}

bool Game::shouldClose() const { return window.ShouldClose(); }

int Game::maxDesignAttempts() const { return Settings().designer.maxDesignAttempts; }

std::string Game::BuildDesignerSystemPrompt() const {
  const GameSettings &settings = Settings();

  std::string prompt;
  if (!settings.designer.promptFile.empty()) {
    std::ifstream file(settings.designer.promptFile);
    if (file.is_open()) {
      std::stringstream buffer;
      buffer << file.rdbuf();
      prompt = buffer.str();
    } else {
      const std::string message =
          "Designer: could not read the prompt file " +
          settings.designer.promptFile +
          "; the designer will run without its instructions.\n";
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message;
    }
  }

  // The palette is generated from the live content registries so it cannot drift
  // from what the validator will accept. The %MATERIALS% placeholder is replaced
  // when present, and appended when not, so a prompt file that forgets it still
  // gets the list rather than silently losing the vocabulary.
  std::string palette = "Tiles, for `paint` -- write the name exactly as shown:\n";
  if (tileRegistry.Count() == 0) {
    palette += "  (none: no tile catalog is loaded, so no design can be built)\n";
  } else {
    for (const TileDef *tile : tileRegistry.GetAll()) {
      palette += "  - " + tile->name +
                 (tile->blocksTile ? "  [blocks movement]" : "") + "\n";
    }
  }

  palette += "\nObjects, for `place` -- each one costs a real item:\n";
  const auto &templates = objectFactory.GetTemplates();
  if (templates.empty()) {
    palette += "  (none)\n";
  } else {
    std::vector<std::string> names;
    names.reserve(templates.size());
    for (const auto &entry : templates) {
      names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    for (const std::string &name : names) {
      palette += "  - " + name + "\n";
    }
  }

  const std::string placeholder = "%MATERIALS%";
  const size_t at = prompt.find(placeholder);
  if (at != std::string::npos) {
    prompt.replace(at, placeholder.size(), palette);
  } else if (!prompt.empty()) {
    prompt +=
        "\n\n## Available materials (generated from this world's content)\n\n" +
        palette;
  }
  return prompt;
}

std::string Game::BuildTileDesignerSystemPrompt() const {
  // The writing of the prompt -- the file read, the %GLYPHS%/%TILES%
  // substitution, the glyph grouping -- lives in TileDesigner so that the
  // headless `--tile-design` mode and this window cannot disagree about what the
  // model was told. This method is the seam that gives it the live registry.
  return TileDesigner::BuildSystemPrompt(
      Settings().content.tileDesignerPromptFile, tileRegistry);
}

// ---------------------------------------------------------------------------
// Blueprint placement
// ---------------------------------------------------------------------------
BlueprintPlacement Game::BuildBlueprintPlacement(const Blueprint &blueprint,
                                                 GamePosition origin) {
  BlueprintPlacement result;
  const BlueprintExpansion::Expansion expansion = BlueprintExpansion::Expand(blueprint);

  if (map == nullptr) {
    result.allLegal = false;
    result.summary = "No map is loaded.";
    return result;
  }

  // Everything already standing on the map, gathered once rather than rescanned
  // per cell. This is the same occupancy test PlaceAtAction uses, kept cheap on
  // purpose: it runs every frame for the ghost, so it is bounds, a blocking tile,
  // and an existing object -- not the structural validator, which is not written
  // yet and which a debug tool must be able to bypass to show a broken design.
  std::unordered_map<int, std::string> occupied;
  ecs.filter<const GamePosition, const DisplayName>().each(
      [&](flecs::entity, const GamePosition &pos, const DisplayName &name) {
        occupied[pos.y * map->GetWidth() + pos.x] = name.name;
      });

  result.cells.reserve(expansion.cells.size());
  for (const BlueprintExpansion::ExpandedCell &cell : expansion.cells) {
    BlueprintGhostCell ghost;
    ghost.position = {origin.x + cell.x, origin.y + cell.y};
    ghost.tile = cell.tile;
    ghost.object = cell.object;
    ghost.carve = cell.carve;

    if (!map->IsInBounds(static_cast<float>(ghost.position.x),
                         static_cast<float>(ghost.position.y))) {
      ghost.legal = false;
      ghost.reason = "outside the map";
    } else {
      const auto occupant =
          occupied.find(ghost.position.y * map->GetWidth() + ghost.position.x);
      if (occupant != occupied.end()) {
        ghost.legal = false;
        ghost.reason = "occupied by " + occupant->second;
      } else if (!cell.carve && !cell.tile.empty()) {
        Tile *existing = map->GetTile(ghost.position.x, ghost.position.y);
        // A cell the design only clears is never refused for its ground, and a
        // cell that places an object still needs walkable ground under it.
        if (existing != nullptr && existing->blocksTile) {
          ghost.legal = false;
          ghost.reason = existing->name + " blocks the site";
        }
      }
    }

    if (!ghost.legal) {
      result.allLegal = false;
    }
    result.cells.push_back(std::move(ghost));
  }

  if (expansion.cells.empty()) {
    result.allLegal = false;
    result.summary = "This design expands to no cells.";
  }
  return result;
}

std::string Game::PlaceBlueprintAt(const Blueprint &blueprint,
                                   GamePosition origin) {
  if (map == nullptr) {
    return "No map is loaded.";
  }

  const BlueprintPlacement placement = BuildBlueprintPlacement(blueprint, origin);
  if (!placement.allLegal) {
    return "Refused: the site is blocked.";
  }

  flecs::entity mapEntity = ecs.lookup("CurrentMap");
  int tilesPainted = 0;
  int objectsPlaced = 0;
  int cellsCleared = 0;

  for (const BlueprintGhostCell &cell : placement.cells) {
    if (cell.carve) {
      // A carve clears the cell's object so an opening really is an opening. The
      // ground stays: "carve" removes what was built, not the earth under it.
      flecs::entity target = flecs::entity::null();
      ecs.filter<const GamePosition>().each([&](flecs::entity e, const GamePosition &pos) {
        if (target.is_alive()) return;
        if (pos.x == cell.position.x && pos.y == cell.position.y) {
          target = e;
        }
      });
      if (target.is_alive()) {
        target.destruct();
      }
      ++cellsCleared;
      continue;
    }

    if (!cell.object.empty()) {
      // Objects are stamped authored so a save writes them back, which is what
      // makes a placed building survive a reload. "authored" is exactly what the
      // map loader and the map editor pass; a runtime spawn (a harvest drop)
      // leaves it false and the writer ignores it.
      if (mapEntity.is_valid()) {
        objectFactory.SpawnObject(ecs, mapEntity, map.get(), cell.object,
                                  cell.position, true);
        ++objectsPlaced;
      }
      continue;
    }

    if (!cell.tile.empty() && PaintTile(cell.position, cell.tile)) {
      ++tilesPainted;
    }
  }

  // The checkbox governs terrain persistence only. Objects are always authored,
  // because leaving them unmarked would strand an object the map cannot write
  // back, which is never what a debug placement wants; the terrain flag exists
  // because painting tiles is the half that mutates the map's own layout rather
  // than adding to it.
  if (blueprintPersistTerrain) {
    map->SaveToFile(mapFilePath);
  }

  std::string message = "Built " + blueprint.name + " at (" +
                        std::to_string(origin.x) + "," + std::to_string(origin.y) +
                        "): " + std::to_string(tilesPainted) + " tiles, " +
                        std::to_string(objectsPlaced) + " objects";
  if (cellsCleared > 0) {
    message += ", " + std::to_string(cellsCleared) + " cells cleared";
  }
  message += blueprintPersistTerrain
                 ? ". Terrain written to " + mapFilePath + "."
                 : ". Terrain is in memory only (persistence is off).";
  return message;
}

flecs::entity Game::FindTileEntity(GamePosition position) {
  flecs::entity mapEntity = ecs.lookup("CurrentMap");
  if (!mapEntity.is_valid()) {
    return flecs::entity::null();
  }

  const ScreenPosition target =
      map->GameCoordsToScreenCoords(position.x, position.y);
  flecs::entity found = flecs::entity::null();
  ecs.filter<const ScreenPosition>().each([&](flecs::entity e, const ScreenPosition &sPos) {
    if (found.is_alive()) return;
    // Background tiles are children of the map and carry no GamePosition, which
    // is what tells them apart from the things standing on them.
    if (e.parent() != mapEntity || e.has<GamePosition>()) return;
    if (sPos.x == target.x && sPos.y == target.y) {
      found = e;
    }
  });
  return found;
}

bool Game::PaintTile(GamePosition position, const std::string &tileName) {
  if (map == nullptr) return false;
  if (!map->IsInBounds(static_cast<float>(position.x),
                       static_cast<float>(position.y))) {
    return false;
  }

  // Prefer a tile the map already has, so a map full of wooden floor does not
  // accumulate one duplicate tile definition per painted cell. Only a tile the
  // map has never used is created from the catalog, and CreateTile refuses when
  // there is no catalog rather than painting an invisible tile.
  Tile *tile = nullptr;
  for (const auto &candidate : map->GetUniqueTiles()) {
    if (candidate->name == tileName) {
      tile = candidate.get();
      break;
    }
  }
  if (tile == nullptr) {
    tile = map->CreateTile(tileName);
  }
  if (tile == nullptr) {
    return false;
  }

  map->addTileToMap(tile, position.x, position.y);

  // Refresh the cell's visual. The sprite is display state like the glyph, so the
  // previous tile's texture is removed before the new one is resolved --
  // otherwise painting wooden floor over grass would keep the grass texture and
  // only swap the glyph underneath it.
  flecs::entity visual = FindTileEntity(position);
  if (visual.is_alive()) {
    visual.set<DrawAscii>(*tile->ascii);
    visual.remove<Sprite>();
    AttachSprite(visual, tile->sprite, TileSpriteId(tile->name), resourceManager,
                 debugLog.get());
  }
  return true;
}

size_t Game::RefreshTileVisuals(const std::string &tileName) {
  if (map == nullptr || tileName.empty()) {
    return 0;
  }

  // The Tile instances this name refers to. Usually one, but a map may hold two
  // palette entries with the same name and different overrides (a deliberate
  // non-goal of the palette work: duplicate names stay distinct tiles), and both
  // should gain the art.
  std::vector<Tile *> targets;
  for (const auto &candidate : map->GetUniqueTiles()) {
    if (candidate->name == tileName) {
      targets.push_back(candidate.get());
    }
  }
  if (targets.empty()) {
    // Not on the map. Normal -- a tile can be designed and never painted -- and the
    // art is still in the catalog and the generated layer, so the next PaintTile
    // picks it up.
    return 0;
  }

  // The cells, found through the map rather than by scanning render entities for a
  // matching glyph. `GetTile` returns the Tile* the cell holds, so identity is
  // exact: the four walls that share '#' are four different Tile instances and only
  // the one being resolved changes.
  const int width = map->GetWidth();
  const int height = map->GetHeight();
  size_t refreshed = 0;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      Tile *cellTile = map->GetTile(x, y);
      if (cellTile == nullptr) continue;
      if (std::find(targets.begin(), targets.end(), cellTile) == targets.end()) {
        continue;
      }

      const GamePosition position{x, y};
      flecs::entity visual = FindTileEntity(position);
      if (!visual.is_alive()) continue;

      // The same refresh PaintTile performs, and for the same reason: a sprite is
      // display state that has to be re-resolved when the tile changes, and the
      // previous one removed first so a failed re-resolve cannot leave the old
      // texture on screen.
      //
      // The targeted forget is what makes the re-resolve find the new file.
      // ResourceManager memoises absence, which is exactly why a sprite added
      // while the game runs "is not noticed until the next map load" -- so without
      // this line AttachSprite would be told the file does not exist, from the
      // cache, forever.
      std::string resolvedPath;
      SpriteResolves(cellTile->sprite, TileSpriteId(cellTile->name), &resolvedPath);
      if (!resolvedPath.empty()) {
        resourceManager.ForgetPath(resolvedPath);
      }

      visual.set<DrawAscii>(*cellTile->ascii);
      visual.remove<Sprite>();
      if (AttachSprite(visual, cellTile->sprite, TileSpriteId(cellTile->name),
                       resourceManager, debugLog.get())) {
        ++refreshed;
      }
      // Counted only when a Sprite was actually attached. A cell that still has no
      // art is not "refreshed" in any sense the caller cares about, and reporting
      // it as such would make the count a lie.
    }
  }

  if (refreshed > 0 && debugLog) {
    debugLog->LogInfo("Tile art: refreshed " + std::to_string(refreshed) +
                      " cell(s) drawing '" + tileName + "'");
  }
  return refreshed;
}

// ---------------------------------------------------------------------------
// Live tile-art generation
// ---------------------------------------------------------------------------

// The queue's completion callback. Runs on the main thread, inside the drain, and
// is the only place the swap happens.
//
// A plain function with a void* rather than a capturing lambda because the queue
// stores the callback as a raw pointer: the worker thread posts *data* and this
// runs later on the main thread, so nothing here crosses a thread boundary.
//
// At file scope, not in an anonymous namespace, because Game::ECSInit takes its
// address and a name with internal linkage in two translation units would be two
// different functions. Declared near the top of this file for the same reason.
void OnTileArtCompleted(void *context, const TileArtQueue::Job &job) {
  Game *game = static_cast<Game *>(context);
  if (game == nullptr) return;
  // A one-line forwarder: all of the world-facing work is inside Game, where the
  // private members it needs already live.
  game->HandleTileArtGenerated(job);
}

void Game::HandleTileArtGenerated(const TileArtQueue::Job &job) {
  // The sprite is already in the generated layer -- the tool put it there, because
  // that is where the generated layer is and the tool writes PNGs. So the swap is
  // the refresh: forget the memoised absence, re-resolve, attach.
  const size_t cells = RefreshTileVisuals(job.tileName);
  if (debugLog) {
    debugLog->LogInfo("Tile art: '" + job.tileName + "' generated (" + job.path +
                      "); " + std::to_string(cells) + " cell(s) updated");
  }
}

void Game::ECSConfigureTileArtQueue() {
  const GameSettings &settings = Settings();

  TileArtQueue::Config queueConfig;
  queueConfig.enabled = settings.art.generation.enabled;
  queueConfig.command = settings.art.generation.command;
  queueConfig.batchDelaySeconds = settings.art.generation.batchDelaySeconds;
  queueConfig.maxRequestsPerSession =
      settings.art.generation.maxRequestsPerSession;

  const std::string workDirectory = settings.art.generation.workDirectory.empty()
                                        ? "data/generated/work"
                                        : settings.art.generation.workDirectory;
  // The generated layer's sprites directory, resolved through the same roots the
  // renderer uses rather than re-derived: the tool must write exactly where the
  // engine looks, and there is already one place that knows where that is.
  const SpriteRootsInfo roots = CurrentSpriteRoots();
  const std::string spritesDirectory = roots.generatedSprites;

  // The derived palette, which is what the cut stage locks to. Empty when none has
  // been derived -- then the tool leaves the sprite unlocked and placement refuses
  // it, which is the honest outcome rather than a sprite that claims to be locked
  // and is not.
  std::string paletteJson = "data/palette.json";
  std::error_code ec;
  if (!std::filesystem::exists(paletteJson, ec)) {
    paletteJson.clear();
  }

  tileArtQueue.Configure(queueConfig, workDirectory, spritesDirectory, paletteJson);
  // A member function pointer is just a function pointer with a bound context, so
  // the callback needs no trampoline and nothing crosses a thread boundary but the
  // job data.
  tileArtQueue.SetCompletion([this](const TileArtQueue::Job &job) {
    // `this` is captured rather than passed as a context pointer: the queue lives
    // exactly as long as the Game that owns it, and Stop() joins the worker before
    // the Game is destroyed.
    HandleTileArtGenerated(job);
  });
  tileArtCompletionWired = true;
}

bool Game::RequestTileArt(const std::string &tileName, std::string *reason) {
  return tileArtQueue.Request(tileName, reason);
}

Game::TileArtPumpResult Game::PumpTileArtQueue(float deltaSeconds) {
  TileArtPumpResult result;

  // Drain first, then update. A completion that arrived during the last worker
  // step is swapped in this frame, before the batch window is even considered --
  // so a swap is never delayed by the window.
  result.completed = tileArtQueue.DrainCompleted();

  // The state changes the drain just made are the window's business; nothing else
  // is notified, because the only consequence of a completed job is the swap that
  // already happened.
  result.startedBatch = tileArtQueue.Update(deltaSeconds);
  return result;
}
