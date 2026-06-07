#include "Game.h"
#include "Components.h"
#include "Defaults.h"
#include "PathFinding.h"
#include "DebugWindowState.h"
#include "DebugLog.h"
#include "MapReloader.h"
#include "DrawAsciiDebug.h"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"
#include <algorithm>
#include <iostream>
#include <filesystem>

Game::Game() {}

Game::~Game() { Shutdown(); }

// TODO: Should fail if entity spawns inside a block tile
flecs::entity Game::createEntity(const GamePosition &pos,
                                 std::string entityName) {
  return ecs.entity(entityName.c_str())
      .set<ScreenPosition>(map->GameCoordsToScreenCoords(pos.x, pos.y))
      .set<GamePosition>(pos)
      .add<BlocksTile>();
}

void Game::ECSInitRenderSystems() {
  renderPipeline = ecs.pipeline().with(flecs::System).with<Render>().build();

  ecs.system<DrawAscii, ScreenPosition>().kind<Render>().each(
      [this](const DrawAscii &ascii, const ScreenPosition screenPos) {
        DrawRectangleV(
            {screenPos.x, screenPos.y},
            {static_cast<float>(ascii.width), static_cast<float>(ascii.height)},
            ascii.backgroundColor);

        const char buf[2] = {ascii.ch, '\0'};
        size_t fontSize = std::min(ascii.width, ascii.height);
        Vector2 textSize = MeasureTextEx(gameFont, buf, fontSize, 0);
        Vector2 textPos = {
            std::round(screenPos.x + (ascii.width - textSize.x) / 2.0f),
            std::round(screenPos.y + (ascii.height - textSize.y) / 2.0f)};

        // Debug rectangles - now togglable
        if (DrawAsciiDebug::GetShowOuterRectangles()) {
          raylib::Rectangle outerRect(screenPos.x, screenPos.y, ascii.width,
                                      ascii.height);
          outerRect.DrawLines(RED, 1.0f);
        }

        if (DrawAsciiDebug::GetShowInnerRectangles()) {
          raylib::Rectangle innerRect(
              std::round(screenPos.x + (ascii.width - textSize.x) / 2.0f),
              std::round(screenPos.y + (ascii.height - textSize.y) / 2.0f),
              textSize.x, textSize.y);
          innerRect.DrawLines(BLUE, 1.0f);
        }

        DrawTextCodepoint(gameFont, (int)ascii.ch, textPos, fontSize,
                          ascii.characterColor);
      });
}

float GetDistanceRecs(Rectangle rec1, Rectangle rec2) {
  // Calculate horizontal distance
  float dx =
      std::max(0.0f, std::max(rec1.x, rec2.x) -
                         std::min(rec1.x + rec1.width, rec2.x + rec2.width));

  // Calculate vertical distance
  float dy =
      std::max(0.0f, std::max(rec1.y, rec2.y) -
                         std::min(rec1.y + rec1.height, rec2.y + rec2.height));

  // Shortest distance is the hypotenuse of dx and dy
  return sqrtf(dx * dx + dy * dy);
}

void Game::ECSInitPhysicsSystems() {
  ecs.system<Velocity, Acceleration, Friction>().each(
      [](Velocity &vel, Acceleration &accel, const Friction &friction) {
        if (vel.Length() > 3.0f) {
          float gravity = 9.8f;
          accel += vel.Normalize().Scale(-gravity * friction.value);
        } else {
          vel = {};
        }
      });

  ecs.system<Velocity, Acceleration, GamePosition, ScreenPosition, DrawAscii,
             TargetPath>()
      .each([this](flecs::entity entity, Velocity &vel, Acceleration &accel,
                   const GamePosition &currPos, const ScreenPosition &screenPos,
                   const DrawAscii &ascii, TargetPath &tPath) {
        if (tPath.path.empty()) {
          entity.remove<TargetPath>();
          return;
        }
        GamePosition currWaypoint = tPath.path[0];
        size_t tWidth = map->GetTileWidth();
        size_t tHeight = map->GetTileHeight();

        // If we are getting close to our target we start slowing down
        GamePosition target = tPath.path.back();
        ScreenPosition targetScreenPos =
            map->GameCoordsToScreenCoords(target.x, target.y);
        raylib::Vector2 centerTile = raylib::Vector2(tWidth, tHeight) / 2.0f;
        // Centralize both screen postiions

        targetScreenPos += centerTile;
        ScreenPosition entCenterScreenPos =
            screenPos + (raylib::Vector2(ascii.width, ascii.height) / 2.0f);

        float finalTargetDistance =
            entCenterScreenPos.Distance(targetScreenPos);

        const int slowingRadius = std::min(tWidth, tHeight) * 1.5f;
        float slowFactor = 1;
        if (finalTargetDistance < slowingRadius) {
          slowFactor = finalTargetDistance / slowingRadius;
        }

        ScreenPosition currWaypointScreenPos =
            map->GameCoordsToScreenCoords(currWaypoint.x, currWaypoint.y);
        currWaypointScreenPos += centerTile;

        Velocity desiredVelocity = static_cast<raylib::Vector2>(
            currWaypointScreenPos - entCenterScreenPos);
        if (desiredVelocity.LengthSqr() > 0) {
          desiredVelocity = desiredVelocity.Normalize();
        }

        float targetDistance =
            entCenterScreenPos.Distance(currWaypointScreenPos);

        if (vel.Length() > 0 && vel.Length() < DEFAULT_MINIMUM_WAYPOINT_SPEED &&
            targetDistance > DEFAULT_MINIMUM_WAYPOINT_SPEED) {
          vel = vel.Normalize() * DEFAULT_MINIMUM_WAYPOINT_SPEED;
        } else if (vel.Length() == 0) {
          vel = DEFAULT_MINIMUM_WAYPOINT_SPEED;
        }

        desiredVelocity *= DEFAULT_WAYPOINT_ACCEL * slowFactor;
        accel += desiredVelocity - vel;

        static const int dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
        static const int dy[] = {-1, -1, -1, 0, 0, 1, 1, 10};

        // Entity might get stuck or unnnecesarily hug walls unless
        // we add a force repelling them
        // We only do this if the velocity has a certain magnitude, otherwise
        // the entity would "Bounce" around the wall when slow
        if (vel.Length() > DEFAULT_MINIMUM_VEL_FOR_WALL_REPEL) {
          for (int i = 0; i < 8; i++) {
            int currX = currPos.x + dx[i];
            int currY = currPos.y + dy[i];
            Tile *currNeighbour = map->GetTile(currX, currY);
            if (currNeighbour == nullptr || !currNeighbour->blocksTile) {
              continue;
            }

            ScreenPosition neighbourScreenPos =
                map->GameCoordsToScreenCoords(currX, currY);

            Velocity wallRepelDirection = static_cast<raylib::Vector2>(
                entCenterScreenPos - (neighbourScreenPos + centerTile));

            raylib::Rectangle entRect(screenPos.x, screenPos.y, ascii.width,
                                      ascii.height);
            raylib::Rectangle neighbourRect(
                neighbourScreenPos.x, neighbourScreenPos.y, tWidth, tHeight);

            float distance = GetDistanceRecs(entRect, neighbourRect);
            distance = std::max(distance, DEFAULT_WALL_REPEL_FORCE);

            wallRepelDirection *= 1 / (std::pow(distance, 2));
            accel += wallRepelDirection;
          }
        }

        if (targetDistance < DEFAULT_MINIMUM_WAYPOINT_DISTANCE) {
          tPath.path.erase(tPath.path.begin());
        }
      });

  ecs.system<Velocity, Acceleration>().each(
      [](Velocity &vel, const Acceleration &accel) {
        vel += accel * GetFrameTime();
      });

  ecs.system<Velocity, const MaxSpeed>().each(
      [](Velocity &vel, const MaxSpeed &maxSpeed) {
        vel = vel.Clamp(0.0f, maxSpeed.value);
      });

  ecs.system<Velocity, DrawAscii, ScreenPosition>().without<Intangible>().each(
      [this](Velocity &vel, const DrawAscii &ascii,
             const ScreenPosition &origScreenPos) {
        ScreenPosition newScreenPos = origScreenPos + (vel * GetFrameTime());
        size_t fontSize = std::min(ascii.width, ascii.height);

        bool nullXVector = false;
        bool nullYVector = false;

        if ((newScreenPos.x < 0) ||
            (newScreenPos.x + ascii.width > map->GetMapWidthPx())) {
          nullXVector = true;
        }
        if ((newScreenPos.y < 0) ||
            (newScreenPos.y + ascii.height > map->GetMapHeightPx())) {
          nullYVector = true;
        }

        raylib::Rectangle deltaXRect(newScreenPos.x, origScreenPos.y, fontSize,
                                     fontSize);
        raylib::Rectangle deltaYRect(origScreenPos.x, newScreenPos.y, fontSize,
                                     fontSize);

        GamePosition gamePos =
            map->ScreenCoordsToGameCoords(origScreenPos.x, origScreenPos.y);
        static const int dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
        static const int dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};

        for (int i = 0; i < 8; ++i) {
          if (nullXVector && nullYVector) {
            break;
          }

          int currX = gamePos.x + dx[i];
          int currY = gamePos.y + dy[i];
          Tile *currTile = map->GetTile(currX, currY);

          if (currTile == NULL || !currTile->blocksTile) {
            continue;
          }
          ScreenPosition currTileScreenPos =
              map->GameCoordsToScreenCoords(currX, currY);
          raylib::Rectangle currRect(currTileScreenPos.x, currTileScreenPos.y,
                                     currTile->ascii->width,
                                     currTile->ascii->height);
          if (deltaXRect.CheckCollision(currRect)) {
            nullXVector = true;
          }
          if (deltaYRect.CheckCollision(currRect)) {
            nullYVector = true;
          }
        }

        vel.x = (nullXVector) ? 0 : vel.x;
        vel.y = (nullYVector) ? 0 : vel.y;
      });

  ecs.system<Velocity, ScreenPosition>().each(
      [](const Velocity &vel, ScreenPosition &pos) {
        pos += vel * GetFrameTime();
      });

  // Save and reset variables used in the physics simulation
  ecs.system<Acceleration>().each([this](Acceleration &accel) {
    lastAccel = accel;
    accel = {};
  });

  ecs.system<Velocity>().each([this](const Velocity &vel) { lastVel = vel; });
}

void Game::ECSInitLogicSystems() {
  ecs.system<ScreenPosition, GamePosition>().each(
      [this](const ScreenPosition &screenPos, GamePosition &gamePos) {
        gamePos = map->ScreenCoordsToGameCoords(screenPos.x, screenPos.y);
      });
}

void Game::ECSInit(std::string mapPath) {
  ecs.import <flecs::monitor>();
  ecs.set<flecs::Rest>({});

  map = std::make_unique<Map>(mapPath, ecs);

  RegisterComponents(ecs);

  ECSInitPhysicsSystems();
  ECSInitLogicSystems();
  ECSInitRenderSystems();

  playerEntity = createEntity({5, 15}, DEFAULT_PLAYER_ENTITY_NAME);
  playerEntity.set<MaxSpeed>({DEFAULT_MAXSPEED});
  playerEntity.set<Friction>({DEFAULT_FRICTION});
  playerEntity.set<Velocity>({0, 0});
  playerEntity.set<Acceleration>({0, 0});
  playerEntity.set<DrawAscii>({
      '@',
      {128, 0, 128, 255},
      {128, 128, 128, 0},
      DEFAULT_PLAYER_WIDTH,
      DEFAULT_PLAYER_HEIGHT,
  });

  createEntity({20, 1});
  createEntity({30, 1});
}

void Game::Init(std::string mapPath) {
  if (window.IsReady())
    return;

  // Do not set borderless flag, we will use proper fullscreen with explicit
  // monitor positioning
  raylib::Window::SetConfigFlags(FLAG_VSYNC_HINT);

  // TODO: Make this general
  window.Init(1920, 1080, "AIRogue");

  // Remember that our font MUST be monofont
  gameFont = LoadFontEx(DEFAULT_FONT_PATH, DEFAULT_FONTSIZE, NULL, 0);
  SetTextureFilter(gameFont.texture, TEXTURE_FILTER_POINT);

  gameTexture = raylib::RenderTexture2D(1920, 1080);
  SetTextureFilter(gameTexture.texture, TEXTURE_FILTER_POINT);

  resourceManager = std::make_unique<ResourceManager>();
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

  // Initialize debug systems
  debugWindowState = std::make_unique<DebugWindowState>();
  debugLog = std::make_unique<DebugLog>();
  mapReloader = std::make_unique<MapReloader>("./");

  // Discover available fonts in the ./fonts directory
  const std::string fontsDir = "./fonts";
  if (std::filesystem::exists(fontsDir) && std::filesystem::is_directory(fontsDir)) {
    for (const auto &entry : std::filesystem::recursive_directory_iterator(fontsDir)) {
      if (entry.is_regular_file()) {
        std::string path = entry.path().string();
        std::string extension = path.substr(path.find_last_of(".") + 1);
        
        // Convert extension to lowercase for comparison
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        
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
      debugLog->LogInfo("Total fonts discovered: " + std::to_string(availableFontPaths.size()));
      selectedFontIndex = 0; // Start with the first font
    }
  }

  // Load debug window state if it exists
  debugWindowState->LoadState("./debug_windows_state.json");
  
  // Apply loaded state to the game
  showDebugConsole = debugWindowState->GetShowDebugConsole();
  showPlayerInfoWindow = debugWindowState->GetShowPlayerInfoWindow();
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
    debugWindowState->SetShowPlayerInfoWindow(showPlayerInfoWindow);
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
