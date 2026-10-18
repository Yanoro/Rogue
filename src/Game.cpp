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
#include "ObjectDesigner.hpp"
#include "PathFinding.h"
#include "Settings.h"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
#include "SpriteRouter.hpp"
#include "TileArt.hpp"
#include "TileDesigner.hpp"
#include "TileDesignerWindow.h"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"
// For rlDrawRenderBatchActive, which the draw probe needs before reading the
// framebuffer back. raylib.h does not re-export rlgl.h; ImageEncode.cpp already
// includes it directly for the same reason.
#include "rlgl.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>

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

  // The System 1 record, created before anything can call System 1 rather than on
  // the first call: the overview window reads it every frame and must not have to
  // cope with it appearing later. The log file itself is still created lazily, on
  // the first record, so a run that never calls System 1 leaves nothing behind.
  systemOneRecorder = std::make_unique<SystemOne::SystemOneRecorder>(
      static_cast<std::size_t>(settings.system1.maxRecords));
  systemOneRecorder->SetWarningSink([this](const std::string &message) {
    if (debugLog) debugLog->LogWarning(message);
  });

  // The live System 1 provider. Built here, before any window can be shown, so a window
  // never has to cope with it appearing later. `Available()` is false without a key, which
  // is a supported state: the sprite designer's router then reports that it cannot ask
  // rather than offering a control that cannot work.
  //
  // The recorder is attached so every decision made in a real run is recorded -- the whole
  // reason the overview window exists -- and a harness (`--sprite-route`) builds its own
  // provider and leaves `harness` attribution on its calls.
  {
    // Resolved once, here, so the picker and the game share one idea of what a
    // profile is. The flat `system1` block always contributes profile "default", so a
    // config with no `profiles` array behaves exactly as it did before profiles
    // existed.
    systemOneProfiles = SystemOne::ResolveProfiles(settings);

    const std::string persisted =
        debugWindowState ? debugWindowState->GetSystemOneProfile() : std::string();
    const std::string wanted =
        SystemOne::ResolveActiveId(systemOneProfiles, settings, persisted,
                                   settings.system1.cliProfile);

    auto classifierFor = [](const SystemOne::ModelProfile &profile) {
      return std::make_shared<SystemOne::JevClassifier>(profile.config);
    };

    // Startup does not probe: a probe at launch would add seconds to every run and
    // cannot tell "not running yet" from "not running", which is a distinction only a
    // person can act on. The picker probes on demand; startup just reports.
    if (const SystemOne::ModelProfile *profile =
            SystemOne::FindProfile(systemOneProfiles, wanted)) {
      auto classifier = classifierFor(*profile);
      if (classifier->Available()) {
        classifier->SetRecorder(systemOneRecorder.get());
        systemOneClassifier = classifier;
        activeSystemOneProfile = profile->id;
        systemOneStatus[profile->id] = {SystemOne::ProfileHealth::InUse,
                                        "serving calls since startup"};
        if (debugLog) {
          debugLog->LogInfo("System 1 router: profile '" + profile->id + "' (" +
                            classifier->ModelLabel() + ") via " +
                            classifier->Endpoint());
        }
      } else {
        activeSystemOneProfile = profile->id;
        systemOneStatus[profile->id] = {SystemOne::ProfileHealth::Unreachable,
                                        "no API key configured"};
        if (debugLog) {
          debugLog->LogInfo("System 1 router: profile '" + profile->id +
                            "' has no API key, so nothing can be asked");
        }
      }
    } else if (debugLog) {
      debugLog->LogInfo("System 1 router: no profile resolved; nothing can be asked");
    }
  }

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

    // A place name is a stamp on one cell, not a rectangle: the name says
    // something about the ground, and the ground's extent is the derivation's
    // business. The naming prompt blocks new stamps so two cannot stack up.
    if (namePlaceSeedMode && !pendingPlaceSeedWantsFocus &&
        !pendingPlaceSeedHasFocus) {
      pendingPlaceSeed.position = gPos;
      pendingPlaceSeed.radius = DEFAULT_PLACE_NAME_RADIUS;
      pendingPlaceSeed.label = "";
      pendingPlaceSeedWantsFocus = true;
      pendingPlaceSeedHasFocus = false;
      return;
    }

    // Removal is checked before the tile/object selection branch, so a click that
    // hits a name removes it instead of placing anything.
    if (removePlaceSeedMode) {
      std::string removedLabel = map->RemovePlaceSeedAt(gPos);
      if (!removedLabel.empty()) {
        lastRemovedPlaceSeedLabel = removedLabel;
        if (debugLog) {
          debugLog->LogInfo("Map editor: removed place name '" + removedLabel +
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

    // Trash mode: delete the object sitting on the clicked tile, or the tile
    // itself when nothing is placed there. Only MapAuthored objects (map-file
    // entries and editor placements) are eligible, which excludes NPCs, the
    // player, held items and anything the world spawned. The object is taken
    // first and the tile only when the cell holds no object, so clearing a tree
    // never also erases the ground under it -- and an erased tile is a real
    // edit: it is written to the map file as an empty cell.
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

      // Nothing authored on the cell, so the ground is what the trash takes.
      // Returns true only when a tile was actually there, so a click on an
      // already-empty or out-of-bounds cell falls through to normal handling.
      if (RemoveTileAt(gPos)) {
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

}

void Game::Update() {
  // Rebuild the blocking-object cache before anything can path, move or place
  // this frame. One frame of staleness is invisible; reading ECS from AStar
  // would not be.
  if (map) {
    map->RefreshBlockingObjects();
  }
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
    debugWindowState->SetShowTaskBoardWindow(
        taskBoardWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowSystemOneWindow(
        systemOneWindowEntity.has<ActiveWindow>());
    debugWindowState->SetShowVisionDebugWindow(
        visionDebugWindowEntity.has<ActiveWindow>());

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
  // A wrapper over the kind-aware builder, kept because the tile window and the
  // existing tests call it by name. New callers should use BuildSpriteSystemPrompt.
  return BuildSpriteSystemPrompt(SpriteKinds::Kind::Tile);
}

std::string Game::BuildSpriteSystemPrompt(SpriteKinds::Kind kind,
                                          bool set) const {
  // The writing of the prompt -- the file read, the %CONTRACT%/%GLYPHS%/%VOCAB%
  // substitution, the glyph grouping -- lives in TileDesigner so that the headless
  // `--sprite-design` mode and the window cannot disagree about what the model was
  // told. This method is the seam that gives it the live world: the tile catalog for
  // the tile kind, and (from phase 2) the object factory's components for the object
  // kind.
  //
  // The vocabulary is assembled here rather than inside the prompt builder because
  // the two kinds read it from different places, and a builder that knew about both
  // would have to know about every future kind as well.
  const GameSettings &gameSettings = Settings();
  const SpriteKinds::Descriptor &descriptor = SpriteKinds::For(kind);

  TileDesigner::PromptVocabulary vocabulary;
  std::string promptFile = descriptor.promptFile;

  if (kind == SpriteKinds::Kind::Tile) {
    promptFile = gameSettings.content.tileDesignerPromptFile;

    vocabulary.body = TileDesigner::TileVocabulary(tileRegistry);
    const std::vector<std::string> legend = TileDesigner::GlyphLegend(tileRegistry);
    if (legend.empty()) {
      vocabulary.glyphs =
          "  (the catalog is empty, so every printable character is free)\n";
    } else {
      for (const std::string &line : legend) {
        vocabulary.glyphs += line + "\n";
      }
    }
  } else {
    promptFile = gameSettings.content.objectDesignerPromptFile;
    // Generated from the factory's registered components, not written into the
    // document: a hand-written list drifts the moment a component is added, and the
    // failure is quiet -- the designer writes a key the engine ignores. The first draft
    // of the design doc hand-wrote this list and got the component *count* wrong.
    vocabulary.body = ObjectDesigner::ComponentVocabulary(objectFactory);
    // No glyph legend for objects: a glyph has to be free to be worth choosing, and
    // object glyphs are not what agents read the map by (they see the ASCII art). The
    // document has no %GLYPHS% placeholder and nothing is appended for it.
  }

  return TileDesigner::BuildPromptForKind(
      descriptor, promptFile, gameSettings.content.spriteContractFile, vocabulary,
      set ? TileDesigner::PromptShape::Set : TileDesigner::PromptShape::One);
}

Game::ObjectLookup Game::LookupObject(const std::string &key) const {
  ObjectLookup lookup;
  const auto &templates = objectFactory.GetTemplates();
  const auto found = templates.find(key);
  if (found == templates.end()) {
    return lookup;
  }
  lookup.found = true;
  // The distinction that matters: an object the same key as a hand-authored template is
  // the catalog's, not the overlay's, and reporting them the same way would make a broken
  // overlay load look like a working one whenever the name happens to exist in both.
  lookup.fromGeneratedOverlay = objectFactory.IsRuntimeAdded(key);
  lookup.name = found->second.value("name", key);
  return lookup;
}

std::vector<std::string> Game::ObjectKeys() const {
  std::vector<std::string> keys;
  for (const auto &pair : objectFactory.GetTemplates()) {
    keys.push_back(pair.first);
  }
  std::sort(keys.begin(), keys.end());
  return keys;
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

  // Walls and doors placed here are objects now, so the blocking cache must see
  // them before the next path is requested.
  map->RefreshBlockingObjects();

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

    // Forget what is memoised about this tile's sprite before resolving it, for the
    // same reason RefreshTileVisuals does: ResourceManager remembers an absent path
    // *and* a failed load, and this is the one moment we know the file may have
    // appeared since the last look.
    //
    // Without it, painting a tile whose art did not exist yet poisons the cell
    // permanently -- the absence is remembered at paint time, so generating the art
    // afterwards changes nothing and the cell keeps drawing its glyph until the map
    // is reloaded. That is the reported "the tile's image is still the ASCII".
    //
    // The explicit path is forgotten even when empty: an empty path plus a convention
    // id resolves to the convention path, and forgetting "" is harmless, so this
    // stays a single unconditional forget rather than a copy of the resolution order.
    std::string resolvedPath;
    SpriteResolves(tile->sprite, TileSpriteId(tile->name), &resolvedPath);
    if (!resolvedPath.empty()) {
      resourceManager.ForgetPath(resolvedPath);
    }

    AttachSprite(visual, tile->sprite, TileSpriteId(tile->name), resourceManager,
                 debugLog.get());
  }
  return true;
}

bool Game::RemoveTileAt(GamePosition position) {
  if (map == nullptr) return false;

  // The map owns the cell. Asking it what is there first, rather than inferring
  // it from the name RemoveTileAt hands back, keeps "nothing to erase" an
  // explicit answer instead of a sentinel a nameless tile could collide with.
  // GetTile bounds-checks, so an off-map click is refused here too.
  if (map->GetTile(position.x, position.y) == nullptr) {
    return false;
  }

  const std::string removedName = map->RemoveTileAt(position);

  // The cell's render entity goes with the tile. It is a child of the map entity
  // holding the ground's DrawAscii and Sprite, and there is no "empty" art for
  // it to fall back to -- leaving it alive would keep drawing the tile the map
  // no longer holds. Destroyed here, not inside the query that found it, and
  // this is called from the input path rather than a filter, so no iteration is
  // invalidated.
  flecs::entity visual = FindTileEntity(position);
  if (visual.is_alive()) {
    visual.destruct();
  }

  lastRemovedTileName = removedName;
  if (debugLog) {
    debugLog->LogInfo("Map editor: removed tile '" + removedName + "' at (" +
                      std::to_string(position.x) + "," +
                      std::to_string(position.y) + ")");
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

Game::CellSpriteState Game::InspectCellSprite(GamePosition position) {
  CellSpriteState state;
  flecs::entity visual = FindTileEntity(position);
  if (!visual.is_alive()) {
    return state;
  }
  state.hasVisual = true;
  if (map != nullptr) {
    if (const Tile *cell = map->GetTile(position.x, position.y)) {
      state.tileName = cell->name;
    }
  }
  const Sprite *sprite = visual.get<Sprite>();
  if (sprite != nullptr) {
    state.hasSprite = true;
    state.spriteId = sprite->path;
  }
  return state;
}

bool Game::TileCellSpriteCount(const std::string &tileName, int *withArt,
                               int *withGlyph) {
  if (withArt != nullptr) *withArt = 0;
  if (withGlyph != nullptr) *withGlyph = 0;
  if (map == nullptr || tileName.empty()) {
    return false;
  }

  // Over the map's own extent rather than a guessed rectangle: the caller cannot see
  // the map to know how big it is, and a fixed bound would silently miss cells.
  const int width = map->GetWidth();
  const int height = map->GetHeight();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const CellSpriteState state = InspectCellSprite(GamePosition{x, y});
      if (!state.hasVisual) continue;
      if (state.tileName != tileName) continue;
      if (state.hasSprite) {
        if (withArt != nullptr) ++(*withArt);
      } else if (withGlyph != nullptr) {
        ++(*withGlyph);
      }
    }
  }
  return true;
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

size_t Game::RefreshObjectVisuals(const std::string &templateKey) {
  if (templateKey.empty()) {
    return 0;
  }

  // Collected first, then modified. Adding and removing a component while iterating would
  // be safe here because `Sprite` is not the component being matched on, but relying on
  // that is the kind of quiet assumption that breaks when the query grows.
  std::vector<flecs::entity> targets;
  ecs.each([&](flecs::entity entity, const ItemType &type) {
    if (type.id == templateKey) {
      targets.push_back(entity);
    }
  });
  if (targets.empty()) {
    // Not on the map, and not spawned anywhere -- the common case for an object that was
    // just designed. Normal: the next spawn resolves the art at spawn time.
    return 0;
  }

  // An explicit "sprite" key on the template wins over the convention, exactly as it does in
  // ApplyTemplate. Read here rather than passed in, because the template is the authority and
  // the job only knows the key.
  std::string explicitSprite;
  const auto &templates = objectFactory.GetTemplates();
  const auto found = templates.find(templateKey);
  if (found != templates.end() && found->second.contains("sprite") &&
      found->second["sprite"].is_string()) {
    explicitSprite = found->second["sprite"].get<std::string>();
  }

  // The same targeted forget the tile path does, and for the same reason: ResourceManager
  // memoises an absent path, so without this the re-resolve is told the file does not exist
  // -- from the cache -- forever.
  std::string resolvedPath;
  SpriteResolves(explicitSprite, templateKey, &resolvedPath);
  if (!resolvedPath.empty()) {
    resourceManager.ForgetPath(resolvedPath);
  }

  size_t updated = 0;
  for (flecs::entity entity : targets) {
    if (!entity.is_alive()) continue;
    // Removing first so a failed re-resolve cannot leave the previous art attached and make
    // the swap look like it worked.
    entity.remove<Sprite>();
    if (AttachSprite(entity, explicitSprite, templateKey, resourceManager,
                     debugLog.get())) {
      ++updated;
    }
  }

  if (updated > 0 && debugLog) {
    debugLog->LogInfo("Object art: refreshed " + std::to_string(updated) +
                      " entity(ies) of '" + templateKey + "'");
  }
  return updated;
}

// Declared here rather than beside its definition (further down, with the other headless
// test hooks): this function needs it, and the definition carries the long comment about why
// it exists.
static bool WriteTestPng(const std::string &path, bool twoColours);

int Game::RunObjectArtSelfTest(const std::string &templateKey) {
  const auto &templates = objectFactory.GetTemplates();
  if (templates.find(templateKey) == templates.end()) {
    std::cerr << "object art swap: no template '" << templateKey << "' is loaded\n";
    return 1;
  }

  const std::string sprite = TileArt::PlanFor(
                                 TileArt::Request{SpriteKinds::Kind::Object,
                                                  templateKey})
                                 .generatedPath;
  if (sprite.empty()) {
    std::cerr << "object art swap: no generated layer, so there is nowhere to put the "
                 "sprite\n";
    return 1;
  }

  // The sprite has to be *absent* when the entity spawns, or the spawn resolves it and there
  // is no arrival to observe. Moved aside rather than assumed missing: a previous run of
  // another mode leaves one behind, and the first version of this test failed exactly that
  // way -- "the entity already had a sprite" -- which is a fact about the last run, not about
  // the code.
  std::error_code ec;
  const std::filesystem::path spritePath(sprite);
  const std::filesystem::path hidden = spritePath.string() + ".selftest-hidden";
  std::filesystem::create_directories(spritePath.parent_path(), ec);
  const bool existed = std::filesystem::exists(spritePath, ec);
  if (existed) {
    std::filesystem::remove(hidden, ec);
    std::filesystem::rename(spritePath, hidden, ec);
    if (ec) {
      std::cerr << "object art swap: could not move " << sprite
                << " aside: " << ec.message() << "\n";
      return 1;
    }
    // Forget it, so the spawn's resolve is not answered from the memo.
    resourceManager.ForgetPath(sprite);
  }

  struct Restore {
    std::filesystem::path sprite;
    std::filesystem::path hidden;
    bool existed;
    ~Restore() {
      std::error_code cleanup;
      if (existed) {
        std::filesystem::remove(sprite, cleanup);
        std::filesystem::rename(hidden, sprite, cleanup);
      } else {
        std::filesystem::remove(sprite, cleanup);
      }
    }
  } restore{spritePath, hidden, existed};

  // Spawned with no map and no position: this is about the *visual* swap, and a map would
  // add ground work to an art question. ApplyTemplate resolves the sprite at spawn time, and
  // the file is absent, so the entity starts on its ASCII art -- which is exactly the state
  // the bug left it in forever.
  const flecs::entity probe =
      objectFactory.SpawnObject(ecs, ecs.entity(), nullptr, templateKey);
  if (!probe.is_alive()) {
    std::cerr << "object art swap: could not spawn '" << templateKey << "'\n";
    return 1;
  }

  const bool spriteBefore = probe.get<Sprite>() != nullptr;
  std::cout << "object art:  spawned '" << templateKey << "', sprite before="
            << (spriteBefore ? "yes" : "no (on its ascii art)") << "\n";
  if (spriteBefore) {
    std::cout << "FAILED:     the entity already had a sprite, so the test cannot show "
                 "one arriving\n";
    probe.destruct();
    return 1;
  }

  // The sprite the pipeline would have written. Created directly rather than generated:
  // this is about the refresh, and spending money to test a cache invalidation would be a
  // worse trade than drawing four pixels.
  if (!WriteTestPng(sprite, /*twoColours=*/true)) {
    std::cerr << "object art swap: could not write " << sprite << "\n";
    probe.destruct();
    return 1;
  }

  const size_t updated = RefreshObjectVisuals(templateKey);
  const bool spriteAfter = probe.get<Sprite>() != nullptr;
  std::cout << "object art:  refresh updated " << updated << " entity(ies), sprite after="
            << (spriteAfter ? "yes" : "no") << "\n";
  if (spriteAfter) {
    std::cout << "object art:  sprite id " << probe.get<Sprite>()->path << "\n";
  }

  // Clean up the probe entity; the `Restore` guard puts the sprite layer back exactly as it
  // was. Leaving a placeholder sprite behind would be a junk asset the next real run
  // overwrites, and deleting one that was there before would lose the owner's art.
  probe.destruct();

  if (!spriteAfter) {
    std::cout << "FAILED:     the entity kept its ascii art after the sprite landed, which "
                 "is the reported bug\n";
    return 1;
  }
  if (updated == 0) {
    std::cout << "FAILED:     the refresh reported no entities, but the probe is one\n";
    return 1;
  }
  std::cout << "object art:  ok (a spawned object picks up its sprite without a reload)\n";
  return 0;
}

size_t Game::HandleTileArtGenerated(const TileArtQueue::Job &job) {
  // The sprite is already in the generated layer -- the tool put it there, because
  // that is where the generated layer is and the tool writes PNGs. So the swap is
  // the refresh: forget the memoised absence, re-resolve, attach.
  //
  // Which refresh depends on the kind, and getting that wrong is why object art appeared
  // generated but never showed: a tile lives in map cells and an object is a spawned
  // entity, so the cell-based refresh finds nothing to do for an object and reports the
  // same "not on the map yet" a genuinely unplaced tile does. Two different questions.
  const size_t updated = job.kind == SpriteKinds::Kind::Object
                             ? RefreshObjectVisuals(job.conventionId)
                             : RefreshTileVisuals(job.tileName);

  // Zero is the interesting case, not a no-op: it means nothing is currently drawing this
  // sprite, which is normal for something designed and not placed, and is the difference
  // between "the swap failed" and "there was nothing to swap yet". Said in the log because
  // otherwise both look identical from outside.
  const char *unit = job.kind == SpriteKinds::Kind::Object ? " entity(ies)" : " cell(s)";
  const char *unplaced = job.kind == SpriteKinds::Kind::Object
                             ? " (nothing spawned from it yet)"
                             : " (the tile is not on the map yet)";
  if (debugLog) {
    debugLog->LogInfo(std::string("Tile art: '") + job.tileName + "' generated into " +
                      job.path + "; " + std::to_string(updated) + unit + " updated" +
                      (updated == 0 ? unplaced : ""));
  }
  return updated;
}

void Game::ECSConfigureTileArtQueue() {
  const GameSettings &settings = Settings();

  TileArtQueue::Config queueConfig;
  queueConfig.enabled = settings.art.generation.enabled;
  queueConfig.command = settings.art.generation.command;
  queueConfig.batchDelaySeconds = settings.art.generation.batchDelaySeconds;
  queueConfig.maxRequestsPerSession =
      settings.art.generation.maxRequestsPerSession;
  queueConfig.maxCellsPerRequest = settings.art.generation.maxCellsPerRequest;

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
  tileArtQueue.SetCompletion([this](const TileArtQueue::Job &job) -> size_t {
    // `this` is captured rather than passed as a context pointer: the queue lives
    // exactly as long as the Game that owns it, and Stop() joins the worker before
    // the Game is destroyed.
    return HandleTileArtGenerated(job);
  });
  tileArtCompletionWired = true;
}

bool Game::RequestTileArt(const std::string &tileName, std::string *reason) {
  return tileArtQueue.Request(KindForSpriteName(tileName), tileName, reason);
}

SpriteKinds::Kind Game::KindForSpriteName(const std::string &name) const {
  // Asked at request time, on the main thread, against the live registries -- which is the
  // only moment the answer is knowable and cheap. The two are separate namespaces that do
  // overlap ('anvil' is both a tile and an object), so neither is privileged: this checks
  // the *tile* catalog first because art for a tile name is the older and more common
  // request, and a name in both would otherwise generate only one of the two sprites.
  //
  // Both registries hold runtime-added definitions, so a design created a moment ago
  // answers correctly without any extra bookkeeping.
  if (tileRegistry.Get(name) != nullptr) return SpriteKinds::Kind::Tile;

  // The factory is keyed by the template *key* (`apple_sapling`), while every caller of
  // this function passes the definition's *display name* ("apple sapling") -- the name the
  // designer accepted and the window shows. Comparing the one against the other found
  // nothing and answered "tile" for every object, which is exactly the bug this function
  // exists to prevent. So both are tried: the key first, because it costs one lookup, then
  // a scan over display names.
  const auto &templates = objectFactory.GetTemplates();
  if (templates.find(name) != templates.end()) return SpriteKinds::Kind::Object;
  for (const auto &pair : templates) {
    if (pair.second.value("name", pair.first) == name) {
      return SpriteKinds::Kind::Object;
    }
  }
  // Neither: a name nobody has defined yet, which is the common case for a request made
  // before the definition lands. A tile is the safer default because that is what the
  // feature was built around, and the sprite is named accordingly -- an object that asks
  // later gets its own, correctly named, because the queue is keyed on kind and name.
  return SpriteKinds::Kind::Tile;
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

size_t Game::RunTileDesignerLogSelfTest() {
  if (!tileDesignerWindow) {
    std::cerr << "log test: the sprite designer window was not constructed\n";
    return 0;
  }
  const size_t inspected = tileDesignerWindow->RunSelfTestConversation(
      tileDesignerArtSelfTest, tileDesignerSelfTestKind, tileDesignerSelfTestSet);

  // The automatic art request is the one thing here whose *effect* is not in the
  // window: it lands in the queue. Reported so the two outcomes are distinguishable
  // from outside -- generation off means "refused, and the transcript says why",
  // generation on with a stub command means "queued", and a silent neither would
  // look the same as a path that never ran.
  if (tileDesignerArtSelfTest) {
    const TileArtQueue &queue = tileArtQueue;
    const bool queued = queue.CountInState(TileArtQueue::State::Queued) > 0;
    std::cout << "art test:   automatic request "
              << (queued ? "QUEUED" : "not queued");
    if (!queued) {
      std::cout << " (generation is "
                << (queue.Enabled() ? "on, so the tile was probably refused"
                                    : "off -- set art.generation.enabled and a "
                                      "stub command to exercise this")
                << ")";
    }
    std::cout << "\n";
  }

  // The window's normal behaviour is to write an accepted tile into the generated
  // overlay, and that is deliberately not special-cased inside it -- a test hook
  // that changed what the code under test does would be testing something else. So
  // the file is removed here, by the thing that asked for the test. Left behind, it
  // would add a junk tile to the world on every run of this mode.
  if (inspected > 0) {
    const bool objects = tileDesignerSelfTestKind == SpriteKinds::Kind::Object;
    const std::string overlay = objects ? Settings().content.generatedObjects
                                        : Settings().content.generatedTiles;
    // Keyed by the *key*, not the display name: the object overlay is written under the
    // slug of the name, and a mismatch here would leave a junk object in the world on
    // every run of this mode -- which is the bug this whole branch exists to prevent.
    // Every file the canned conversation can have written, by its key. A set writes one per
    // accepted entry, and leaving any of them behind would add junk to the world on every run
    // of this mode -- which is the bug this whole branch exists to prevent.
    const std::vector<std::string> written = {
        objects ? "self_test_post.json" : "self_test_puddle.json",
        "self_test_grate.json",
        "self_test_bucket.json",
        // The broken entry in the set fixture must NOT be written (it has no notes), so it is
        // deliberately absent from this list: if it appears, the test would fail to clean up,
        // which is a louder signal than silently tidying a file that should never exist.
    };
    if (!overlay.empty()) {
      std::error_code ec;
      for (const std::string &name : written) {
        const std::filesystem::path path = std::filesystem::path(overlay) / name;
        if (std::filesystem::exists(path, ec)) {
          std::filesystem::remove(path, ec);
          std::cout << "log test: removed " << path.string() << "\n";
        }
      }
    }
  }
  return inspected;
}

// Writes a tiny PNG holding one or two flat colours. The test needs two files that
// differ in something the pixel view *reports* -- its distinct-colour count -- so the
// first file carries two colours and the second one, which is written over the first's
// path. A hand-rolled encoder is enough here and keeps this mode free of a dependency
// on the generator's Python side.
static bool WriteTestPng(const std::string &path, bool twoColours) {
  const int kSize = 8;
  std::vector<unsigned char> raw;
  raw.reserve(static_cast<size_t>(kSize) * (1 + kSize * 4));
  // The green channel is the variant marker, so the two files are byte-different in a
  // way that changes the pixels without changing the *count* of distinct colours --
  // which is what makes the pixel-view assertion about the reload and not about the
  // arithmetic.
  const unsigned char rightGreen = static_cast<unsigned char>(twoColours ? 30 : 180);
  for (int y = 0; y < kSize; ++y) {
    raw.push_back(0);  // filter: none
    for (int x = 0; x < kSize; ++x) {
      const bool right = x >= kSize / 2;
      raw.push_back(static_cast<unsigned char>(right ? 10 : 200));
      raw.push_back(right ? rightGreen : static_cast<unsigned char>(120));
      raw.push_back(static_cast<unsigned char>(right ? 220 : 40));
      raw.push_back(255);
    }
  }

  auto crc32 = [](const unsigned char *data, size_t length) {
    unsigned int crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
      crc ^= data[i];
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
      }
    }
    return ~crc;
  };
  auto chunk = [&crc32](std::ostream &out, const char type[4],
                        const std::vector<unsigned char> &payload) {
    auto be32 = [](std::ostream &o, unsigned int value) {
      const unsigned char bytes[4] = {
          static_cast<unsigned char>((value >> 24) & 0xFF),
          static_cast<unsigned char>((value >> 16) & 0xFF),
          static_cast<unsigned char>((value >> 8) & 0xFF),
          static_cast<unsigned char>(value & 0xFF)};
      o.write(reinterpret_cast<const char *>(bytes), 4);
    };
    be32(out, static_cast<unsigned int>(payload.size()));
    std::vector<unsigned char> crcInput;
    crcInput.insert(crcInput.end(), type, type + 4);
    crcInput.insert(crcInput.end(), payload.begin(), payload.end());
    out.write(type, 4);
    if (!payload.empty()) {
      out.write(reinterpret_cast<const char *>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
    }
    be32(out, crc32(crcInput.data(), crcInput.size()));
  };

  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }
  const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  out.write(reinterpret_cast<const char *>(signature), 8);

  std::vector<unsigned char> header;
  auto be = [&header](unsigned int value) {
    header.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
    header.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    header.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    header.push_back(static_cast<unsigned char>(value & 0xFF));
  };
  be(kSize);
  be(kSize);
  header.push_back(8);  // bit depth
  header.push_back(6);  // colour type: RGBA
  header.push_back(0);  // compression
  header.push_back(0);  // filter
  header.push_back(0);  // interlace
  chunk(out, "IHDR", header);

  // zlib stream: header, stored (uncompressed) deflate blocks, adler32.
  std::vector<unsigned char> zlib;
  zlib.push_back(0x78);
  zlib.push_back(0x01);
  size_t offset = 0;
  while (offset < raw.size()) {
    const size_t block = std::min<size_t>(65535, raw.size() - offset);
    const bool last = offset + block >= raw.size();
    zlib.push_back(static_cast<unsigned char>(last ? 1 : 0));
    zlib.push_back(static_cast<unsigned char>(block & 0xFF));
    zlib.push_back(static_cast<unsigned char>((block >> 8) & 0xFF));
    zlib.push_back(static_cast<unsigned char>(~block & 0xFF));
    zlib.push_back(static_cast<unsigned char>((~block >> 8) & 0xFF));
    zlib.insert(zlib.end(), raw.begin() + static_cast<long>(offset),
                raw.begin() + static_cast<long>(offset + block));
    offset += block;
  }
  unsigned int a = 1;
  unsigned int bb = 0;
  for (const unsigned char byte : raw) {
    a = (a + byte) % 65521;
    bb = (bb + a) % 65521;
  }
  zlib.push_back(static_cast<unsigned char>((bb >> 8) & 0xFF));
  zlib.push_back(static_cast<unsigned char>(bb & 0xFF));
  zlib.push_back(static_cast<unsigned char>((a >> 8) & 0xFF));
  zlib.push_back(static_cast<unsigned char>(a & 0xFF));
  chunk(out, "IDAT", zlib);
  chunk(out, "IEND", {});
  return out.good();
}

bool Game::PaintTileForTest(GamePosition position, const std::string &tileName,
                            bool *hasSprite) {
  const bool painted = PaintTile(position, tileName);
  if (hasSprite != nullptr) {
    *hasSprite = InspectCellSprite(position).hasSprite;
  }
  return painted;
}

int Game::RunPaintMemoSelfTest(const std::string &tileName) {
  const std::string sprite = TileArt::PlanForTile(tileName).generatedPath;
  if (sprite.empty()) {
    std::cerr << "paint memo test: no generated sprite path for '" << tileName
              << "'\n";
    return 1;
  }
  // The sprite has to exist for the test to have something to hide. Created when it is
  // missing rather than refused, because this harness is about the *cache* and not about
  // whether some earlier run happened to leave a file behind -- and a test that depends on
  // a previous mode's leftovers fails for a reason that has nothing to do with the code
  // under test. A run of another art mode that clears the generated layer is enough to
  // trigger that, which is exactly what happened once.
  std::error_code ec;
  if (!std::filesystem::exists(sprite, ec)) {
    std::filesystem::create_directories(
        std::filesystem::path(sprite).parent_path(), ec);
    if (!WriteTestPng(sprite, /*twoColours=*/true)) {
      std::cerr << "paint memo test: " << sprite
                << " does not exist and could not be created\n";
      return 1;
    }
    std::cout << "paint memo:  created a placeholder sprite at " << sprite << "\n";
  }

  int withArt = 0;
  int withGlyph = 0;
  // The map must have cells drawing this tile; it must *not* have them drawing art.
  //
  // Requiring `withArt > 0` was wrong, and wrong in a way that only showed up once the
  // sprite had been created a moment earlier: cells only carry a `Sprite` if the file
  // resolved when the map loaded, so a sprite created after the boot leaves every cell on
  // its glyph -- which is precisely the situation this test exists to fix, made stronger.
  // A zero-art baseline is therefore accepted, and reported, as long as the map has
  // somewhere to paint.
  if (!TileCellSpriteCount(tileName, &withArt, &withGlyph) ||
      withArt + withGlyph == 0) {
    std::cerr << "paint memo test: no cell on the loaded map draws '" << tileName
              << "', so there is nothing to refresh\n";
    return 1;
  }
  std::cout << "paint memo:  '" << tileName << "' starts with " << withArt
            << " cell(s) drawing art\n";

  // The file disappears for a moment -- what a first generation looks like from the
  // engine's side. The refresh that follows is where the absence is memoised.
  const std::filesystem::path hidden = sprite + ".memo-test-hidden";
  std::filesystem::remove(hidden, ec);
  std::filesystem::rename(sprite, hidden, ec);
  if (ec) {
    std::cerr << "paint memo test: could not hide the sprite: " << ec.message()
              << "\n";
    return 1;
  }

  const size_t hiddenRefresh = RefreshTileVisuals(tileName);
  int hiddenArt = 0;
  int hiddenGlyph = 0;
  TileCellSpriteCount(tileName, &hiddenArt, &hiddenGlyph);

  // Put it back, exactly as generation would have produced it.
  std::filesystem::rename(hidden, sprite, ec);
  if (ec) {
    std::cerr << "paint memo test: could not restore the sprite: " << ec.message()
              << "\n";
    return 1;
  }

  const size_t restoredRefresh = RefreshTileVisuals(tileName);
  int restoredArt = 0;
  int restoredGlyph = 0;
  TileCellSpriteCount(tileName, &restoredArt, &restoredGlyph);

  std::cout << "paint memo:  refresh-without-art -> " << hiddenRefresh
            << " changed (" << hiddenGlyph << " still glyph), after restore -> "
            << restoredRefresh << " changed (" << restoredArt << " drawing art)\n";
  if (restoredArt == 0) {
    std::cerr << "paint memo test: the art came back but the cells did not pick it "
                 "up -- the ResourceManager's memoised absence is still poisoning "
                 "them\n";
    return 1;
  }
  std::cout << "paint memo: ok (art that arrives after a paint is noticed)\n";
  return 0;
}

// ---------------------------------------------------------------------------
// --draw-probe: does the map actually show through an object?
// ---------------------------------------------------------------------------
//
// The bug this exists for had two independent causes and was reported from the
// world, not from a test: an object generated in the sprite designer covered the
// tile it stood on with a solid rectangle. Both fixes are in the pixels and only
// the pixels, so this harness is the only thing in the repo that can tell whether
// they are actually fixed -- a log line cannot say whether a pixel was transparent.
//
// It runs the real frame path (Game::Draw -> render pipeline -> the 1920x1080 game
// texture -> blit) and reads the result back with LoadImageFromScreen, the same way
// raylib's own TakeScreenshot does. Nothing here is synthetic except the choice of
// which cell to use.

namespace {

// An Image that releases itself. Four captures and a dozen early returns is exactly
// the shape that leaks a raylib Image, which is a plain struct with no destructor.
struct ProbeImage {
  Image image{};

  ProbeImage() = default;
  ProbeImage(const ProbeImage &) = delete;
  ProbeImage &operator=(const ProbeImage &) = delete;
  ProbeImage(ProbeImage &&other) noexcept : image(other.image) {
    other.image = Image{};
  }
  ProbeImage &operator=(ProbeImage &&other) noexcept {
    if (this != &other) {
      if (image.data != nullptr) UnloadImage(image);
      image = other.image;
      other.image = Image{};
    }
    return *this;
  }
  ~ProbeImage() {
    if (image.data != nullptr) UnloadImage(image);
  }

  bool valid() const { return image.data != nullptr; }
  int width() const { return image.data != nullptr ? image.width : 0; }
  int height() const { return image.data != nullptr ? image.height : 0; }
  Color At(int x, int y) const { return GetImageColor(image, x, y); }
};

bool ProbeSameColour(Color a, Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Whether two captures are the same frame, ignoring one rectangle.
//
// "The same frame" is the settling test: the first frames after Init are not the
// settled world -- measured here, the tile layer was still drawn where the previous
// window size put it -- and the number of frames that takes is not fixed. Rather than
// guess, draw until two consecutive captures agree. The ignored rectangle is raylib's
// FPS counter, whose digits change on their own and would make every pair differ.
bool ProbeFramesEqualExcept(const ProbeImage &a, const ProbeImage &b,
                            const Rectangle &ignore) {
  if (!a.valid() || !b.valid()) return false;
  if (a.width() != b.width() || a.height() != b.height()) return false;
  if (a.image.format != b.image.format) return false;

  const int ignoreWidth = static_cast<int>(ignore.x + ignore.width);
  const int ignoreHeight = static_cast<int>(ignore.y + ignore.height);
  const int stride = a.width() * 4;
  const auto *left = static_cast<const unsigned char *>(a.image.data);
  const auto *right = static_cast<const unsigned char *>(b.image.data);
  for (int y = 0; y < a.height(); ++y) {
    int skip = 0;
    if (y < ignoreHeight) {
      skip = std::min(stride, ignoreWidth * 4);
    }
    if (skip >= stride) continue;
    if (std::memcmp(left + y * stride + skip, right + y * stride + skip,
                    static_cast<size_t>(stride - skip)) != 0) {
      return false;
    }
  }
  return true;
}

std::string ProbeColourText(Color c) {
  return "(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
         std::to_string(c.b) + "," + std::to_string(c.a) + ")";
}

// How much of a cell is `plate` and how much is something drawn over it. "The plate
// is present" is the whole assertion for a sprite-less fallback, and the ink count is
// what makes it not vacuous: a cell that is entirely plate has no glyph.
struct ProbeCellCounts {
  int plate = 0;
  int ink = 0;
  int total = 0;
};

// How many pixels inside `box` differ between two frames. The legibility question for a
// fallback whose plate is *transparent* -- the hand-authored style, where the world shows
// the tile through it -- is not "is the plate there" but "was anything drawn at all".
int ProbeChangedPixels(const ProbeImage &a, const ProbeImage &b, const Rectangle &box) {
  const int x0 = static_cast<int>(std::round(box.x));
  const int y0 = static_cast<int>(std::round(box.y));
  const int x1 = x0 + static_cast<int>(std::round(box.width));
  const int y1 = y0 + static_cast<int>(std::round(box.height));
  int changed = 0;
  for (int y = std::max(0, y0); y < std::min(a.height(), y1); ++y) {
    for (int x = std::max(0, x0); x < std::min(a.width(), x1); ++x) {
      if (!ProbeSameColour(a.At(x, y), b.At(x, y))) {
        ++changed;
      }
    }
  }
  return changed;
}

ProbeCellCounts ProbeCountCell(const ProbeImage &frame, const Rectangle &box,
                               Color plate) {
  ProbeCellCounts counts;
  const int x0 = static_cast<int>(std::round(box.x));
  const int y0 = static_cast<int>(std::round(box.y));
  const int x1 = x0 + static_cast<int>(std::round(box.width));
  const int y1 = y0 + static_cast<int>(std::round(box.height));
  for (int y = std::max(0, y0); y < std::min(frame.height(), y1); ++y) {
    for (int x = std::max(0, x0); x < std::min(frame.width(), x1); ++x) {
      ++counts.total;
      if (ProbeSameColour(frame.At(x, y), plate)) {
        ++counts.plate;
      } else {
        ++counts.ink;
      }
    }
  }
  return counts;
}

}  // namespace

int Game::RunDrawProbe(const std::string &templateKey) {
  if (map == nullptr) {
    std::cerr << "draw probe: no map is loaded, so there is no tile to see through\n";
    return 1;
  }
  if (objectFactory.GetTemplates().find(templateKey) ==
      objectFactory.GetTemplates().end()) {
    std::cerr << "draw probe: no object template '" << templateKey << "' is loaded\n";
    return 1;
  }

  std::vector<std::string> failures;

  // One frame through the real render path, read back before EndDrawing -- the
  // moment the frame exists in the draw buffer. raylib's own TakeScreenshot reads
  // here, and LoadImageFromScreen flips the rows into screen order itself.
  //
  // rlDrawRenderBatchActive() is not optional: raylib queues geometry on the CPU and
  // only hands it to the GPU when a batch fills, a state change forces it, or
  // EndDrawing runs. A read-back mid-frame therefore sees the cleared buffer -- the
  // first version of this probe compared five black frames and every differential
  // passed vacuously. ImageEncode.cpp carries the same line and the same note.
  auto drawAndCapture = [this]() {
    window.BeginDrawing();
    window.ClearBackground(BLACK);
    Draw();
    rlDrawRenderBatchActive();
    ProbeImage frame;
    frame.image = LoadImageFromScreen();
    window.EndDrawing();
    return frame;
  };

  // A deterministic view. The probe has to know which screen pixel a cell renders to,
  // and the follow camera's answer depends on where the player happens to stand. Free
  // mode at zoom 1 with the origin at the top-left keeps the projection on whole
  // pixels, which is what makes NEAREST-sampled sprites comparable pixel for pixel.
  cameraMode = GameCameraMode::FreeRoamMode;
  camera.offset = {0.0f, 0.0f};
  camera.zoom = 1.0f;
  camera.target = {0.0f, 0.0f};

  // Settle the frame before taking the baseline. The first frames after Init are not the
  // settled world -- measured here, the tile layer was drawn 280 pixels from where the
  // entity layer was, which is exactly the difference between the 1920x1080 render
  // target and the 1280x800 window -- and how many frames that takes is not fixed. A
  // two-frame warmup passed on one run and not on the next, which is the worst kind of
  // harness. So: draw until two consecutive captures are identical, bounded, and refuse
  // to judge anything if they never are.
  const Rectangle fpsCounter{0.0f, 0.0f, 160.0f, 48.0f};
  ProbeImage before = drawAndCapture();
  bool settled = false;
  for (int attempt = 0; attempt < 12 && before.valid(); ++attempt) {
    ProbeImage next = drawAndCapture();
    if (ProbeFramesEqualExcept(before, next, fpsCounter)) {
      before = std::move(next);
      settled = true;
      break;
    }
    before = std::move(next);
  }
  if (!before.valid()) {
    std::cerr << "draw probe: LoadImageFromScreen returned nothing, so no pixel can "
                 "be sampled\n";
    return 1;
  }
  if (!settled) {
    std::cerr << "draw probe: the framebuffer never stopped changing across 12 frames, "
                 "so no frame is trustworthy enough to compare\n";
    return 1;
  }
  std::cout << "draw probe:  framebuffer " << before.width() << "x" << before.height()
            << ", camera target (" << camera.target.x << "," << camera.target.y
            << ") offset (" << camera.offset.x << "," << camera.offset.y << ") zoom "
            << camera.zoom << ", mode "
            << (cameraMode == GameCameraMode::FreeRoamMode ? "free" : "follow") << "\n";

  const float tileWidth = static_cast<float>(map->GetTileWidth());
  const float tileHeight = static_cast<float>(map->GetTileHeight());

  auto screenRectFor = [&](GamePosition p) {
    const ScreenPosition world = map->GameCoordsToScreenCoords(
        static_cast<float>(p.x), static_cast<float>(p.y));
    const Vector2 screen = GetWorldToScreen2D({world.x, world.y}, camera);
    return Rectangle{screen.x, screen.y, tileWidth, tileHeight};
  };

  // Usable = the whole cell is inside the framebuffer and clear of the FPS counter,
  // which DrawFPS paints at (10,10) and which would otherwise be compared as if it
  // were part of the tile.
  auto usable = [&](GamePosition p) {
    const Rectangle r = screenRectFor(p);
    if (r.x < 4.0f || r.y < 4.0f) return false;
    if (r.x + r.width > static_cast<float>(before.width()) - 4.0f) return false;
    if (r.y + r.height > static_cast<float>(before.height()) - 4.0f) return false;
    return !CheckCollisionRecs(r, fpsCounter);
  };

  // Cells with something already standing on them are refused: the differential
  // compares two frames of the same view, and another entity redrawing over the cell
  // would be read as the object's doing.
  auto occupied = [&](GamePosition p) {
    bool taken = false;
    ecs.filter<GamePosition>().each([&](flecs::entity, GamePosition &pos) {
      if (pos.x == p.x && pos.y == p.y) taken = true;
    });
    return taken;
  };

  auto findFreeCell = [&](GamePosition skip) {
    for (int y = 0; y < map->GetHeight(); ++y) {
      for (int x = 0; x < map->GetWidth(); ++x) {
        const GamePosition p{x, y};
        if (p.x == skip.x && p.y == skip.y) continue;
        if (!usable(p) || occupied(p)) continue;
        return p;
      }
    }
    return GamePosition{-1, -1};
  };

  // The ground: a cell whose tile draws a *sprite*. "The tile shows through" has to be
  // a tile that is actually drawn, and a sprite is what this bug was about.
  GamePosition ground{-1, -1};
  std::string groundSprite;
  for (int y = 0; y < map->GetHeight() && ground.x < 0; ++y) {
    for (int x = 0; x < map->GetWidth(); ++x) {
      const GamePosition p{x, y};
      if (!usable(p) || occupied(p)) continue;
      const CellSpriteState cell = InspectCellSprite(p);
      if (!cell.hasVisual || !cell.hasSprite) continue;
      ground = p;
      groundSprite = cell.spriteId;
      break;
    }
  }
  if (ground.x < 0) {
    std::cerr << "draw probe: no visible, unoccupied cell of the loaded map draws a tile "
                 "sprite, so there is no ground to check\n";
    return 1;
  }

  const Rectangle groundRect = screenRectFor(ground);
  std::cout << "draw probe:  ground cell (" << ground.x << "," << ground.y
            << ") at screen (" << groundRect.x << "," << groundRect.y << "), tile sprite "
            << groundSprite << "\n";

  // Check 3a: a tile with a full-bleed sprite still draws that sprite. Tiles share
  // DrawEntityVisual with entities, so the plate change is on their path too; an opaque
  // sprite makes it invisible, and this is the assertion that it stayed that way (a
  // tile that stopped drawing its art would fall back to a glyph and fail here).
  {
    raylib::Texture *texture = resourceManager.GetTexture(groundSprite);
    ProbeImage pixels;
    pixels.image = LoadImage(groundSprite.c_str());
    if (texture == nullptr || !pixels.valid()) {
      failures.push_back("the ground cell's tile sprite '" + groundSprite +
                         "' could not be read back, so the tile layer is unverified");
    } else {
      const SpriteRect fit = SpriteFitRect(*texture, groundRect.x, groundRect.y,
                                           tileWidth, tileHeight);
      const float scaleX = fit.width / static_cast<float>(texture->width);
      const float scaleY = fit.height / static_cast<float>(texture->height);
      int compared = 0;
      int matched = 0;
      for (int dy = 0; dy < static_cast<int>(fit.height); ++dy) {
        for (int dx = 0; dx < static_cast<int>(fit.width); ++dx) {
          const int sx = std::min(
              pixels.image.width - 1, static_cast<int>(static_cast<float>(dx) / scaleX));
          const int sy = std::min(
              pixels.image.height - 1, static_cast<int>(static_cast<float>(dy) / scaleY));
          const Color source = pixels.At(sx, sy);
          // Only fully opaque source pixels: anything else is a blend with whatever is
          // under the tile, and exact equality would be asserting the wrong thing.
          if (source.a != 255) continue;
          const int x = static_cast<int>(std::round(fit.x)) + dx;
          const int y = static_cast<int>(std::round(fit.y)) + dy;
          if (x < 0 || y < 0 || x >= before.width() || y >= before.height()) continue;
          ++compared;
          if (ProbeSameColour(before.At(x, y), source)) {
            ++matched;
          }
        }
      }
      std::cout << "draw probe:  tile art: " << matched << "/" << compared
                << " opaque sprite pixel(s) match the rendered cell\n";
      if (compared == 0) {
        failures.push_back("the ground tile's sprite has no opaque pixel to compare");
      } else if (matched != compared) {
        failures.push_back("only " + std::to_string(matched) + " of " +
                           std::to_string(compared) +
                           " tile-sprite pixel(s) were drawn; a tile with art must draw "
                           "its art, not a glyph");
      }
    }
  }

  // Spawn the object on the ground cell, through the real factory, so this exercises
  // the same template and sprite resolution a placed object does.
  flecs::entity parent = ecs.lookup("CurrentMap");
  if (!parent.is_alive()) parent = ecs.entity();
  flecs::entity probe =
      objectFactory.SpawnObject(ecs, parent, map.get(), templateKey, ground);
  if (!probe.is_alive()) {
    std::cerr << "draw probe: could not spawn '" << templateKey << "'\n";
    return 1;
  }

  const DrawAscii *ascii = probe.get<DrawAscii>();
  if (ascii == nullptr) {
    std::cerr << "draw probe: '" << templateKey
              << "' declares no visual, so there is nothing to sample\n";
    probe.destruct();
    return 1;
  }
  const Sprite *sprite = probe.get<Sprite>();
  if (sprite == nullptr) {
    std::cerr << "draw probe: '" << templateKey
              << "' has no sprite, so there is no cut-out to see the tile through. "
                 "Check the sprite path or generate one first\n";
    probe.destruct();
    return 1;
  }

  // Check 1: the sprite's fully transparent pixels leave the frame under them
  // *identical*. That is "the tile shows through" stated as a pixel assertion: before
  // the fix the plate was painted into them, so every one of them differed.
  //
  // Frames are kept in function scope so a failure can write them out as evidence; a
  // pixel assertion that cannot show the pixels it disagreed about is a hard bug to
  // take anyone's word for.
  ProbeImage withObject;
  ProbeImage withoutSprite;
  ProbeImage withGlyph;
  {
    withObject = drawAndCapture();
    raylib::Texture *texture = resourceManager.GetTexture(sprite->path);
    ProbeImage pixels;
    pixels.image = LoadImage(sprite->path.c_str());
    if (texture == nullptr || !pixels.valid()) {
      failures.push_back("the object's sprite '" + sprite->path +
                         "' could not be read back, so its transparency is unverified");
    } else {
      const SpriteRect fit = SpriteFitRect(*texture, groundRect.x, groundRect.y,
                                           tileWidth, tileHeight);
      std::cout << "draw probe:  object '" << templateKey << "' sprite " << texture->width
                << "x" << texture->height << " drawn at (" << fit.x << "," << fit.y
                << ") " << fit.width << "x" << fit.height << "\n";
      const float scaleX = fit.width / static_cast<float>(texture->width);
      const float scaleY = fit.height / static_cast<float>(texture->height);
      int transparent = 0;
      int interiorTransparent = 0;
      int mismatched = 0;
      int changed = 0;
      bool reportedFirst = false;
      for (int dy = 0; dy < static_cast<int>(fit.height); ++dy) {
        for (int dx = 0; dx < static_cast<int>(fit.width); ++dx) {
          const int sx = std::min(
              pixels.image.width - 1, static_cast<int>(static_cast<float>(dx) / scaleX));
          const int sy = std::min(
              pixels.image.height - 1, static_cast<int>(static_cast<float>(dy) / scaleY));
          const Color source = pixels.At(sx, sy);
          const int x = static_cast<int>(std::round(fit.x)) + dx;
          const int y = static_cast<int>(std::round(fit.y)) + dy;
          if (x < 0 || y < 0 || x >= before.width() || y >= before.height()) continue;
          const Color plain = before.At(x, y);
          const Color drawn = withObject.At(x, y);
          if (source.a == 0) {
            ++transparent;
            // Deeper than the sprite's own outer rim. A sprite whose only transparent
            // pixels are a one-pixel border around a solid rectangle was not cut out at
            // all -- that is what an un-keyed cell looks like after the trim/pad/resize
            // -- and a rim is enough to make the check above pass while the object still
            // covers its tile. See the failure message below.
            if (sx >= 3 && sy >= 3 && sx < pixels.image.width - 3 &&
                sy < pixels.image.height - 3) {
              ++interiorTransparent;
            }
            if (!ProbeSameColour(plain, drawn)) {
              ++mismatched;
              if (!reportedFirst) {
                failures.push_back(
                    "at (" + std::to_string(x) + "," + std::to_string(y) +
                    ") the sprite is transparent but the frame changed from " +
                    ProbeColourText(plain) + " to " + ProbeColourText(drawn) +
                    " -- something is painted behind the sprite");
                reportedFirst = true;
              }
            }
          } else if (!ProbeSameColour(plain, drawn)) {
            ++changed;
          }
        }
      }
      std::cout << "draw probe:  transparency: " << transparent << " transparent sprite "
                << "pixel(s) (" << interiorTransparent << " inside the rim), " << mismatched
                << " of them changed; " << changed << " opaque pixel(s) changed\n";
      if (transparent == 0) {
        // Not a renderer failure: this sprite's background was never keyed out, which
        // is the other half of the bug and lives in tools/gen_tile_art.py.
        failures.push_back(
            "'" + sprite->path +
            "' has no fully transparent pixel, so it still covers the tile. Its "
            "background was not keyed out -- re-cut it with tools/gen_tile_art.py "
            "--cut-check / --sheet, or regenerate it");
      } else if (interiorTransparent == 0) {
        failures.push_back(
            "'" + sprite->path +
            "' is transparent only around its outer rim, which is an un-keyed cell's "
            "padding rather than a cut-out: the object still covers its tile. Re-cut it "
            "with tools/gen_tile_art.py --sheet, or regenerate it");
      }
      if (changed == 0) {
        failures.push_back("the object drew nothing over the tile, so this test is "
                           "vacuous");
      }
    }
  }

  // Check 2a: with the sprite gone, the entity falls back to its ASCII art over the
  // plate. The plate is the template's own background colour, opaque here, so "the
  // plate is present" is an exact colour match on more than half the cell, and "the
  // art is legible" is the ink drawn over it.
  probe.remove<Sprite>();
  {
    withoutSprite = drawAndCapture();
    // Re-read after a component change: flecs moves the entity between archetypes, so
    // a pointer taken before the removal can be stale.
    const DrawAscii *fallback = probe.get<DrawAscii>();
    if (fallback == nullptr) {
      failures.push_back("the entity lost its DrawAscii when the Sprite was removed");
    } else {
      const ProbeCellCounts counts =
          ProbeCountCell(withoutSprite, groundRect, fallback->backgroundColor);
      if (fallback->backgroundColor.a == 255) {
        std::cout << "draw probe:  no-sprite fallback: " << counts.plate << "/"
                  << counts.total << " plate pixel(s), " << counts.ink << " ink\n";
        if (counts.plate * 2 <= counts.total) {
          failures.push_back("a sprite-less object lost its plate: only " +
                             std::to_string(counts.plate) + " of " +
                             std::to_string(counts.total) +
                             " pixel(s) are the template's background colour");
        }
        if (counts.ink < 12) {
          failures.push_back("a sprite-less object drew " + std::to_string(counts.ink) +
                             " ink pixel(s) over its plate, which is not a legible art "
                             "block");
        }
      } else {
        // A transparent background is the hand-authored convention: the fallback is
        // meant to sit on the tile, so there is no plate to look for. What must still
        // hold is that the art drew something.
        const int drawn = ProbeChangedPixels(before, withoutSprite, groundRect);
        std::cout << "draw probe:  no-sprite fallback: no plate (transparent "
                     "background), "
                  << drawn << " pixel(s) drawn over the tile\n";
        if (drawn < 12) {
          failures.push_back("a sprite-less object with a transparent background drew " +
                             std::to_string(drawn) + " pixel(s) over the tile, which is "
                             "not a legible art block");
        }
      }
    }
  }

  // Check 2b: the single-glyph fallback, same plate. An object with neither art nor a
  // sprite must still be readable.
  probe.remove<AsciiArt>();
  {
    withGlyph = drawAndCapture();
    const DrawAscii *glyph = probe.get<DrawAscii>();
    if (glyph == nullptr) {
      failures.push_back("the entity lost its DrawAscii when the AsciiArt was removed");
    } else {
      const ProbeCellCounts counts =
          ProbeCountCell(withGlyph, groundRect, glyph->backgroundColor);
      if (glyph->backgroundColor.a == 255) {
        std::cout << "draw probe:  glyph fallback: " << counts.plate << "/"
                  << counts.total << " plate pixel(s), " << counts.ink << " ink\n";
        if (counts.plate * 2 <= counts.total) {
          failures.push_back("a glyph-only object lost its plate: only " +
                             std::to_string(counts.plate) + " of " +
                             std::to_string(counts.total) +
                             " pixel(s) are the template's background colour");
        }
        if (counts.ink < 4) {
          failures.push_back("a glyph-only object drew " + std::to_string(counts.ink) +
                             " ink pixel(s), so its glyph is not legible");
        }
      } else {
        const int drawn = ProbeChangedPixels(before, withGlyph, groundRect);
        std::cout << "draw probe:  glyph fallback: no plate (transparent background), "
                  << drawn << " pixel(s) drawn over the tile\n";
        if (drawn < 4) {
          failures.push_back("a glyph-only object with a transparent background drew " +
                             std::to_string(drawn) + " pixel(s), so its glyph is not "
                             "legible");
        }
      }
    }
  }

  // Check 3b: a tile *without* art still draws a legible glyph over its plate. Painted
  // rather than found, because the loaded map is uniform -- one tile with a sprite --
  // and the fallback has to be exercised on purpose. The tile is chosen from the live
  // catalog as one with no sprite at all, an opaque plate and a plate colour that is
  // not the clear colour, which is what makes "the plate is present" mean something.
  {
    const TileDef *glyphTile = nullptr;
    for (const TileDef *candidate : tileRegistry.GetAll()) {
      if (!candidate->hasCharacter) continue;
      if (!candidate->hasBackgroundColor || candidate->backgroundColor.a != 255) continue;
      if (candidate->backgroundColor.r == 128 && candidate->backgroundColor.g == 128 &&
          candidate->backgroundColor.b == 128) {
        continue;
      }
      std::string resolved;
      if (SpriteResolves(candidate->sprite, TileSpriteId(candidate->name), &resolved)) {
        continue;
      }
      glyphTile = candidate;
      break;
    }
    const GamePosition glyphCell = findFreeCell(ground);
    if (glyphTile == nullptr) {
      failures.push_back("no catalog tile has an opaque plate and no sprite, so the "
                         "tile-glyph fallback could not be exercised");
    } else if (glyphCell.x < 0) {
      failures.push_back("no free visible cell to paint the glyph-only tile on");
    } else {
      const CellSpriteState original = InspectCellSprite(glyphCell);
      if (!PaintTile(glyphCell, glyphTile->name)) {
        failures.push_back("could not paint '" + glyphTile->name + "' at (" +
                           std::to_string(glyphCell.x) + "," +
                           std::to_string(glyphCell.y) + ")");
      } else {
        ProbeImage withTileGlyph = drawAndCapture();
        const ProbeCellCounts counts = ProbeCountCell(
            withTileGlyph, screenRectFor(glyphCell), glyphTile->backgroundColor);
        std::cout << "draw probe:  tile glyph ('" << glyphTile->name << "' at "
                  << glyphCell.x << "," << glyphCell.y << "): " << counts.plate << "/"
                  << counts.total << " plate pixel(s), " << counts.ink << " ink\n";
        if (counts.plate * 2 <= counts.total) {
          failures.push_back("a tile with no art lost its plate: only " +
                             std::to_string(counts.plate) + " of " +
                             std::to_string(counts.total) +
                             " pixel(s) are its background colour");
        }
        if (counts.ink < 4) {
          failures.push_back("a tile with no art drew " + std::to_string(counts.ink) +
                             " ink pixel(s), so its glyph is not legible");
        }
        // Put the map back as it was found. Nothing saves it, but a probe that leaves
        // the world different from how it started is a trap for the next mode.
        if (!original.tileName.empty()) {
          PaintTile(glyphCell, original.tileName);
        }
      }
    }
  }

  probe.destruct();

  if (!failures.empty()) {
    for (const std::string &failure : failures) {
      std::cout << "draw probe FAILED: " << failure << "\n";
    }
    // The frames the verdict was reached from. A pixel assertion that cannot show the
    // pixels it disagreed about is not one anybody should have to take on trust.
    const std::string base = "/tmp/airogue_draw_probe";
    int written = 0;
    if (before.valid() && ExportImage(before.image, (base + "_before.png").c_str())) {
      ++written;
    }
    if (withObject.valid() &&
        ExportImage(withObject.image, (base + "_object.png").c_str())) {
      ++written;
    }
    if (withoutSprite.valid() &&
        ExportImage(withoutSprite.image, (base + "_no_sprite.png").c_str())) {
      ++written;
    }
    if (withGlyph.valid() && ExportImage(withGlyph.image, (base + "_glyph.png").c_str())) {
      ++written;
    }
    if (written > 0) {
      std::cout << "draw probe:  " << written << " frame(s) written to " << base
                << "_*.png\n";
    }
    return 1;
  }
  std::cout << "draw probe:  ok (the tile shows through a cut-out object; the glyph "
               "and art fallbacks keep a legible plate; a tile with art still draws it)\n";
  return 0;
}

int Game::RunArtCacheSelfTest() {
  if (!tileDesignerWindow) {
    std::cerr << "art cache test: the tile designer window was not constructed\n";
    return 1;
  }

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "airogue_art_cache_test";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  const std::string first = (directory / "stage.png").string();
  const std::string second = (directory / "stage_changed.png").string();

  // Two distinct single-colour files, plus one written over the *same* path as the
  // first. The overwrite is the whole point: a regenerated stage reuses its filename.
  if (!WriteTestPng(first, /*twoColours=*/true) ||
      !WriteTestPng(second, /*twoColours=*/false)) {
    std::cerr << "art cache test: could not write the test images\n";
    return 1;
  }

  const TileDesignerWindow::ThumbnailLookup firstLook =
      tileDesignerWindow->InspectThumbnailForTest(first);
  if (firstLook.textureId == 0) {
    std::cerr << "art cache test: the first image did not load as a thumbnail\n";
    return 1;
  }
  const size_t firstColours = tileDesignerWindow->InspectPixelViewForTest(first)
                                  .distinctColours;
  const size_t firstHash =
      tileDesignerWindow->InspectPixelViewForTest(first).hash;
  if (firstColours == 0 || firstHash == 0) {
    std::cerr << "art cache test: the first image did not load into the pixel view\n";
    return 1;
  }

  // Overwrite the same path with what the second file holds. The stamp is pushed
  // forward explicitly so the test does not depend on the filesystem's timestamp
  // resolution catching two writes made in the same tick.
  std::filesystem::copy_file(second, first,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec) {
    std::cerr << "art cache test: could not rewrite the thumbnail file: "
              << ec.message() << "\n";
    return 1;
  }
  const auto future =
      std::filesystem::last_write_time(first, ec) + std::chrono::seconds(2);
  std::filesystem::last_write_time(first, future, ec);

  const TileDesignerWindow::ThumbnailLookup secondLook =
      tileDesignerWindow->InspectThumbnailForTest(first);
  const TileDesignerWindow::PixelViewLookup secondPixels =
      tileDesignerWindow->InspectPixelViewForTest(first);
  std::cout << "art cache test: thumbnail id " << firstLook.textureId << " -> "
            << secondLook.textureId << ", pixel hash " << firstHash << " -> "
            << secondPixels.hash << "\n";

  // The assertion is on the stamp, not the texture id: raylib reuses ids, so a
  // *successful* reload can land on the same number and an id comparison would
  // report a false failure. What proves the cache noticed is that the identity it
  // resolved changed.
  if (secondLook.textureId == 0) {
    std::cerr << "art cache test: the rewritten image did not load\n";
    return 1;
  }
  if (secondLook.hasStamp && firstLook.hasStamp &&
      secondLook.stamp == firstLook.stamp) {
    std::cerr << "art cache test: the cache kept the old file identity after the "
                 "file changed (stages would show the previous generation)\n";
    return 1;
  }
  if (secondPixels.hash == firstHash) {
    std::cerr << "art cache test: the pixel view kept the previous generation's "
                 "pixels after the sprite changed\n";
    return 1;
  }
  std::cout << "art cache:  ok (a rewritten stage reloads instead of showing the "
               "previous batch)\n";
  return 0;
}

std::string Game::SwitchSystemOneProfile(const std::string &id) {
  const SystemOne::ModelProfile *profile =
      SystemOne::FindProfile(systemOneProfiles, id);
  if (profile == nullptr) {
    return "no profile named '" + id + "'";
  }

  // Built first, so nothing is torn down unless the replacement exists and can work.
  // This ordering is what makes a failed switch a no-op rather than a half-state.
  auto classifier = std::make_shared<SystemOne::JevClassifier>(profile->config);
  if (!classifier->Available()) {
    systemOneStatus[profile->id] = {SystemOne::ProfileHealth::Unreachable,
                                    "no API key configured"};
    return "profile '" + profile->id + "' has no API key configured";
  }

  std::string probeError;
  if (!SystemOne::JevClassifier::ProbeReachable(profile->config, &probeError)) {
    systemOneStatus[profile->id] = {SystemOne::ProfileHealth::Unreachable, probeError};
    return probeError;
  }

  // Recorder attached before the swap, so a call that lands in the same frame is still
  // recorded.
  classifier->SetRecorder(systemOneRecorder.get());

  if (systemOneClassifier != nullptr) {
    systemOneStatus[activeSystemOneProfile] = {SystemOne::ProfileHealth::Reachable,
                                               "not in use"};
  }
  systemOneStatus[profile->id] = {
      SystemOne::ProfileHealth::InUse,
      profile->config.IsLocalEndpoint() ? "local; first call may wait for the model"
                                        : "serving calls"};

  // The swap. Assigning drops the last reference to the previous provider, whose
  // destructor cancels anything it had in flight -- deliberate, see Game.h.
  systemOneClassifier = classifier;
  activeSystemOneProfile = profile->id;
  if (debugWindowState) {
    debugWindowState->SetSystemOneProfile(activeSystemOneProfile);
  }

  const std::string event = "profile switched to '" + profile->id + "' (" +
                            classifier->ModelLabel() + ") via " +
                            classifier->Endpoint();
  if (debugLog) debugLog->LogInfo("System 1: " + event);
  // Into the System 1 log too, so a run whose backend changed mid-way is explained
  // rather than looking like inconsistent model labels in the records.
  if (systemOneRecorder) systemOneRecorder->Note(event);

  return {};
}

const SystemOne::ModelProfile *Game::ActiveSystemOneProfile() const {
  if (activeSystemOneProfile.empty()) return nullptr;
  return SystemOne::FindProfile(systemOneProfiles, activeSystemOneProfile);
}

SystemOne::SystemOneProfileStatus Game::SystemOneStatusFor(const std::string &id) const {
  const auto found = systemOneStatus.find(id);
  if (found != systemOneStatus.end()) return found->second;
  return SystemOne::SystemOneProfileStatus{};
}
