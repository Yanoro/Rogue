#include "Game.h"
#include "Components.h"
#include "PathFinding.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "raylib.h"
#include "rlImGui.h"
#include <algorithm>
#include <fstream>
#include <iostream>

Game::Game() {}

Game::~Game() { Shutdown(); }

std::string findFirstJsonFile(const std::string &directoryPath) {
  try {
    if (!std::filesystem::exists(directoryPath) or
        !std::filesystem::is_directory(directoryPath)) {
      return ""; // Directory doesn't exist
    }

    for (const auto &entry :
         std::filesystem::directory_iterator(directoryPath)) {
      if (entry.is_regular_file() && entry.path().extension() == ".json") {
        return entry.path().string();
      }
    }
  } catch (const std::filesystem::filesystem_error &e) {
    std::cerr << "Filesystem error: " << e.what() << std::endl;
  }
  return ""; // No JSON file found
}

// TODO: Function either works or throws an exception,
// Probably a better way to do this
flecs::entity Game::createEntity(const std::string &texturePath,
                                 const GamePosition &pos,
                                 std::string entityName) {
  std::string jsonPath = findFirstJsonFile(
      std::filesystem::path(texturePath).parent_path().string());
  std::ifstream jsonFile;
  jsonFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);

  try {
    jsonFile.open(jsonPath);
  } catch (const std::ifstream::failure &e) {
    std::cerr << "Game::createEntity: Exception opening/reading file: "
              << texturePath << " " << e.what() << std::endl;
    throw;
  }

  try {
    auto json = nlohmann::json::parse(jsonFile);
    unsigned int frameCount = json["frames"].size();
    raylib::Texture *characterTexture =
        resourceManager->GetTexture(texturePath);
    CharacterAnimation charAnimation(characterTexture, frameCount);
    for (const auto &currFrame : json["frames"]) {
      double duration = currFrame.value("duration", 100.0) /
                        1000; // From seconds to miliseconds
      auto currRect = currFrame["frame"];
      raylib::Rectangle frameRect = {currRect["x"], currRect["y"],
                                     currRect["w"], currRect["h"]};
      charAnimation.frames.emplace_back(Frame{frameRect, duration});
    }
    return ecs.entity(entityName.c_str())
        .set<ScreenPosition>(map->GameCoordsToScreenCoords(pos.x, pos.y))
        .set<GamePosition>(pos)
        .set<CharacterAnimation>(charAnimation)
        .add<BlocksTile>();
  } catch (const std::exception &e) {
    std::cerr << "JSON Parse Error: " << e.what() << " jsonPath: " << jsonPath
              << std::endl;
    throw;
  }
}

void Game::ECSInitRenderSystems() {
  renderPipeline = ecs.pipeline().with(flecs::System).with<Render>().build();

  ecs.system<Drawable, ScreenPosition>().kind<Render>().each(
      [](const Drawable &draw, const ScreenPosition &pos) {
        draw.texture->Draw(draw.srcRect, pos);
      });

  ecs.system<Tile, ScreenPosition>().kind<Render>().each(
      [this](const Tile &tile, const ScreenPosition screenPos) {
        float width = map->GetTileWidth();
        float height = map->GetTileHeight();
        DrawRectangleV({screenPos.x, screenPos.y}, {width, height},
                       tile.backgroundColor);

        float fontSize = std::min(width, height);
        const char buf[2] = {tile.ch, '\0'};
        Vector2 textSize = MeasureTextEx(gameFont, buf, fontSize, 0);
        Vector2 textPos = {std::round(screenPos.x + (width - textSize.x) / 2.0f),
                           std::round(screenPos.y + (height - textSize.y) / 2.0f - 6.0f)};

        DrawTextCodepoint(gameFont, (int)tile.ch, textPos, fontSize,
                          tile.characterColor);
      });

  // TODO: Probably can remove this
  ecs.system<ScreenPosition, CharacterAnimation>().kind<Render>().each(
      [](const ScreenPosition &pos, CharacterAnimation &characterAnimation) {
        unsigned int currFrame = characterAnimation.currentFrame;
        DrawCircleV(pos, 7.5f, BLUE);
        double currTime = GetTime();
        if (currTime - characterAnimation.lastFrameTime >
            characterAnimation.frames[currFrame].duration) {
          characterAnimation.currentFrame =
              (currFrame + 1) % characterAnimation.frameCount;
          characterAnimation.lastFrameTime = currTime;
        }
      });
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

  ecs.system<Velocity, Acceleration, GamePosition, TargetPath>().each(
      [](flecs::entity entity, const Velocity &vel, Acceleration &accel,
         const GamePosition &currPos, TargetPath &tPath) {
        if (tPath.path.empty()) {
          entity.remove<TargetPath>();
          return;
        }
        GamePosition currWaypoint = tPath.path[0];
        Velocity desiredVelocity =
            static_cast<raylib::Vector2>(currWaypoint - currPos).Normalize();

        desiredVelocity *= DEFAULT_MAXSPEED;
        accel += desiredVelocity - vel;
        if (currWaypoint.x == currPos.x && currWaypoint.y == currPos.y) {
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

  // Save and reset variables used in the physics simulation
  ecs.system<Acceleration>().each([this](Acceleration &accel) {
    lastAccel = accel;
    accel = {};
  });

  // TODO: This seems to be really inneficient, probably easy optimization gains
  // Bounds Checking
  auto collisionFilter = ecs.filter<GamePosition, BlocksTile>();
  ecs.system<Velocity, ScreenPosition>().without<Intangible>().each(
      [collisionFilter, this](const flecs::entity currentEntity,
                              const Velocity &vel,
                              ScreenPosition &origScreenPos) {
        ScreenPosition newScreenPos = origScreenPos + (vel * GetFrameTime());
        GamePosition newGamePos =
            map->ScreenCoordsToGameCoords(newScreenPos.x, newScreenPos.y);

        if (!map.get()->IsInBounds(newGamePos.x, newGamePos.y)) {
          return;
        }

        bool isBlocked = false;
        collisionFilter.find([&](flecs::entity blockEntity,
                                 const GamePosition &pos, const BlocksTile) {
          if (blockEntity == currentEntity) {
            return false;
          }
          if (pos.x == newGamePos.x && pos.y == newGamePos.y) {
            isBlocked = true;
            return true;
          }
          return false;
        });

        if (!isBlocked) {
          origScreenPos = newScreenPos;
        }
      });
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

  playerEntity = createEntity("./data/tilesets/Pixel Crawler - Free "
                              "Pack/Entities/NPCS/Rogue/Idle/Idle-Sheet.png",
                              {1, 1}, DEFAULT_PLAYER_ENTITY_NAME);
  playerEntity.set<MaxSpeed>({DEFAULT_MAXSPEED});
  playerEntity.set<Friction>({DEFAULT_FRICTION});
  playerEntity.set<Velocity>({0, 0});
  playerEntity.set<Acceleration>({0, 0});

  createEntity("./data/tilesets/Pixel Crawler - Free "
               "Pack/Entities/NPCS/Knight/Idle/Idle-Sheet.png",
               {20, 1});
  createEntity("./data/tilesets/Pixel Crawler - Free "
               "Pack/Entities/NPCS/Wizzard/Idle/Idle-Sheet.png",
               {30, 1});
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
  gameFont = GetFontDefault();

  gameTexture = raylib::RenderTexture2D(1920, 1080);
  SetTextureFilter(gameTexture.texture, TEXTURE_FILTER_POINT);

  resourceManager = std::make_unique<ResourceManager>();
  //TODO: Feels weird that map initialization occurs here
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

  // Actually go fullscreen (locks correctly on the targeted monitor in most X11
  // setups)
  ToggleFullscreen();

  window.SetTargetFPS(60);

  // Since X11 can treat multi-monitors as a single massive 3000x1920 screen,
  // GetScreenWidth() might return 3000 even if we only draw to one screen.
  // We constrain the width to a reasonable maximum for zoom calculations.
  float screenWidth = GetScreenWidth();
  float screenHeight = GetScreenHeight();

  // If the reported screen width is unusually large (e.g. dual monitors
  // spanning), clamp it for the sake of the zoom calculation so the game isn't
  // zoomed out/in too far.
  if (screenWidth > 2560) {
    screenWidth = 1080;
  }

  float mapWidthPx = map->GetMapWidthPx();
  float mapHeightPx = map->GetMapHeightPx();

  float zoomX = screenWidth / mapWidthPx;
  float zoomY = screenHeight / mapHeightPx;

  // camera.target = { mapWidthPx / 2.0f, mapHeightPx / 2.0f };
  // camera.offset = { screenWidth / 2.0f, screenHeight / 2.0f} ;
  camera.target = {0, 0};
  camera.offset = {0, 0};
  camera.rotation = 0.0f;
  camera.zoom = std::max(1.0f, std::floor(std::min(zoomX, zoomY)));

  inputHandler = std::make_unique<InputHandler>(camera, mapWidthPx, mapHeightPx);

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
        astarPath = AStar(ecs, astarStartPos, astarEndPos);
      }
    } else if (isSettingPlayerTarget) {
      if (playerEntity.is_alive()) {
        if (const GamePosition *pPos = playerEntity.get<GamePosition>()) {
          std::vector<GamePosition> path = AStar(ecs, *pPos, gPos);
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

  rlImGuiShutdown();
  window.Close();
}

bool Game::shouldClose() const { return window.ShouldClose(); }
