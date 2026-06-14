#include "Game.h"
#include "Components.h"
#include "DebugLog.h"
#include "DebugWindowState.h"
#include "Defaults.h"
#include "DrawAsciiDebug.h"
#include "EntityInfoWindow.h"
#include "AgentContextWindow.h"
#include "AIChatWindow.h"
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

  // Ensure it starts on the main monitor
  int currentMonitor = 0;

  // Explicitly move the window to the correct monitor's origin before scaling
  window.SetPosition(GetMonitorPosition(currentMonitor).x,
                     GetMonitorPosition(currentMonitor).y);
  window.SetSize(GetMonitorWidth(currentMonitor),
                 GetMonitorHeight(currentMonitor));

  // Actually go fullscreen (locks correctly on the targeted monitor in most
  // X11 setups)
  ToggleFullscreen();

  window.SetTargetFPS(60);

  // Since X11 can treat multi-monitors as a single massive 3000x1920 screen,
  // GetScreenWidth() might return 3000 even if we only draw to one screen.
  // We constrain the width to a reasonable maximum for zoom calculations.
  float screenWidth = GetScreenWidth();
  float screenHeight = GetScreenHeight();

  // If the reported screen width is unusually large (e.g. dual monitors
  // spanning), clamp it for the sake of the zoom calculation so the game
  // isn't zoomed out/in too far.
  if (screenWidth > 2560) {
    screenWidth = 1080;
  }

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

  // Apply loaded state to the game
  showDebugConsole = debugWindowState->GetShowDebugConsole();
  showEntityInfoWindow = debugWindowState->GetShowEntityInfoWindow();
  showTileInfoWindow = debugWindowState->GetShowTileInfoWindow();
  showAStarWindow = debugWindowState->GetShowAStarWindow();
  showEntityOverviewWindow = debugWindowState->GetShowEntityOverviewWindow();
  showDebugLogWindow = debugWindowState->GetShowDebugLogWindow();
  showMapReloadWindow = debugWindowState->GetShowMapReloadWindow();
  showDrawAsciiToggleWindow = debugWindowState->GetShowDrawAsciiToggleWindow();

  debugLog->LogInfo("Game initialized successfully");

  rlImGuiSetup(true);
}

void Game::UpdateGUI() {
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
      !ImGui::GetIO().WantCaptureMouse) {
    Vector2 mouseWorldPos = GetScreenToWorld2D(GetMousePosition(), camera);
    GamePosition gPos =
        map->ScreenCoordsToGameCoords(mouseWorldPos.x, mouseWorldPos.y);

    ecs.defer_begin();
    ecs.filter<GamePosition, WindowOnClick>().each(
        [&gPos, this](flecs::entity entity, const GamePosition &pos,
                      WindowOnClick &clickWin) {
          if (gPos == pos) {
            switch (clickWin.type) {
            case ::WindowType::AIChatWindowType:
              if (entity.has<ActiveWindow>()) {
                entity.remove<ActiveWindow>();
              } else {
                const AIBackend *backend = entity.get<AIBackend>();
                if (backend != nullptr && backend->ptr != nullptr) {
                  entity.set<ActiveWindow>(
                      {std::make_shared<AIChatWindow>(backend->ptr)});
                }
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
                    {std::make_shared<EntityInfoWindow>(entity, map.get())});
              }
              break;
            case ::WindowType::NPCContextWindowType:
              if (entity.has<ActiveWindow>()) {
                entity.remove<ActiveWindow>();
              } else {
                const NPC *npc = entity.get<NPCComponent>()->ptr.get();
                entity.set<ActiveWindow>(
                    {std::make_shared<NPCContextWindow>(npc->getContextID())});
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
    debugWindowState->SetShowDebugConsole(showDebugConsole);
    debugWindowState->SetShowEntityInfoWindow(showEntityInfoWindow);
    debugWindowState->SetShowTileInfoWindow(showTileInfoWindow);
    debugWindowState->SetShowAStarWindow(showAStarWindow);
    debugWindowState->SetShowEntityOverviewWindow(showEntityOverviewWindow);
    debugWindowState->SetShowDebugLogWindow(showDebugLogWindow);
    debugWindowState->SetShowMapReloadWindow(showMapReloadWindow);
    debugWindowState->SetShowDrawAsciiToggleWindow(showDrawAsciiToggleWindow);

    debugWindowState->SaveState("./debug_windows_state.json");
    if (debugLog) {
      debugLog->LogInfo("Debug window state saved on shutdown");
    }
  }

  rlImGuiShutdown();
  window.Close();
}

bool Game::shouldClose() const { return window.ShouldClose(); }
