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

  // Do not set borderless flag, we will use proper fullscreen with explicit
  // monitor positioning
  raylib::Window::SetConfigFlags(FLAG_VSYNC_HINT);

  // TODO: Make this general
  window.Init(1920, 1080, "AIRogue");

  // Initialize debug systems early to read state
  debugWindowState = std::make_unique<DebugWindowState>();
  debugLog = std::make_unique<DebugLog>();
  mapReloader = std::make_unique<MapReloader>("./");

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

  // Offset the mouse so clicking maps 1:1 on the primary display coordinates
  SetMouseOffset(static_cast<int>(-GetMonitorPosition(currentMonitor).x),
                 static_cast<int>(-GetMonitorPosition(currentMonitor).y));

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
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
      !ImGui::GetIO().WantCaptureMouse) {
    Vector2 mouseWorldPos = GetScreenToWorld2D(GetMousePosition(), camera);
    GamePosition gPos =
        map->ScreenCoordsToGameCoords(mouseWorldPos.x, mouseWorldPos.y);

    auto ai = ecs.get<AIBackend>();
    ecs.defer_begin();
    ecs.filter<GamePosition, WindowOnClick>().each(
        [&gPos, ai, this](flecs::entity entity, const GamePosition &pos,
                          WindowOnClick &clickWin) {
          if (gPos == pos) {
            switch (clickWin.type) {
            case ::WindowType::AIChatWindowType:
              if (entity.has<ActiveWindow>()) {
                entity.remove<ActiveWindow>();
              } else {
                entity.set<ActiveWindow>(
                    {std::make_shared<AIChatWindow>(ai->ptr.get())});
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
                std::shared_ptr<NPC> npc = entity.get<NPCComponent>()->ptr;
                entity.set<ActiveWindow>({std::make_shared<NPCContextWindow>(
                    std::weak_ptr<NPC>(npc))});
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
          playerEntity.set<TargetPath>({path});
        }
      }
      isSettingPlayerTarget =
          false; // Disable the mode after setting the target
    } else {
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

    debugWindowState->SaveState("./debug_windows_state.json");
    if (debugLog) {
      debugLog->LogInfo("Debug window state saved on shutdown");
    }
  }

  // Gracefully stop all NPC threads before destroying the flecs world.
  // This prevents race conditions where NPC threads might query flecs
  // components (like TargetPath) while flecs is in the middle of being torn
  // down.
  ecs.filter<NPCComponent>().each([](flecs::entity, NPCComponent &npc) {
    if (npc.ptr) {
      npc.ptr.reset();
    }
  });

  rlImGuiShutdown();
  ecs.quit();
  window.Close();
}

bool Game::shouldClose() const { return window.ShouldClose(); }
