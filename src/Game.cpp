#include "Game.h"
#include "imgui.h"
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

void Game::ECSInit(std::string mapPath) {
  ecs.import <flecs::monitor>();
  ecs.set<flecs::Rest>({});

  map = std::make_unique<Map>(mapPath, *resourceManager, ecs);

  renderPipeline = ecs.pipeline().with(flecs::System).with<Render>().build();

  // TODO: Really got to separate this into multiple functions
  ecs.system<Drawable, ScreenPosition>().kind<Render>().each(
      [](const Drawable &draw, const ScreenPosition &pos) {
        draw.texture->Draw(draw.srcRect, pos);
      });

  // ecs.system<ScreenPosition, const Velocity>().each(
  //     [](ScreenPosition &pos, const Velocity &vel) {
  //       pos.x += vel.x * GetFrameTime();
  //       pos.y += vel.y * GetFrameTime();
  //     });

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

  ecs.system<ScreenPosition, GamePosition>().each(
      [this](const ScreenPosition &screenPos, GamePosition &gamePos) {
        gamePos = map->ScreenCoordsToGameCoords(screenPos.x, screenPos.y);
      });

  ecs.system<Velocity, const MaxSpeed>().each(
      [](Velocity &vel, const MaxSpeed &maxSpeed) {
        vel.x = std::clamp(vel.x, -maxSpeed.maxX, maxSpeed.maxX);
        vel.y = std::clamp(vel.y, -maxSpeed.maxY, maxSpeed.maxY);
      });

  ecs.system<Velocity, Friction>().each(
      [](Velocity &vel, const Friction &friction) {
        if (vel.x > 0) {
          vel.x = std::max(0.0f, vel.x - friction.value);
        } else if (vel.x < 0) {
          vel.x = std::min(0.0f, vel.x + friction.value);
        }
        if (vel.y >= 0) {
          vel.y = std::max(0.0f, vel.y - friction.value);
        } else if (vel.y < 0) {
          vel.y = std::min(0.0f, vel.y + friction.value);
        }
      });

  std::vector<GamePosition> tilesBlocked;
  ecs.filter<GamePosition, BlocksTile>().each(
      [&tilesBlocked](const GamePosition &pos, const BlocksTile) {
        tilesBlocked.push_back(pos);
      });

  auto collisionFilter = ecs.filter<GamePosition, BlocksTile>();

  //TODO: This wall seems to be really inneficient, probably easy optimization gains
  ecs.system<Velocity, ScreenPosition>().without<Intangible>().each(
      [collisionFilter, this](flecs::entity currentEntity, Velocity &vel,
                              ScreenPosition &origScreenPos) {
        ScreenPosition newScreenPos{origScreenPos.x + (vel.x * GetFrameTime()),
                                    origScreenPos.y + (vel.y * GetFrameTime())};
        GamePosition newGamePos =
            map->ScreenCoordsToGameCoords(newScreenPos.x, newScreenPos.y);

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

  ecs.system<Velocity, ScreenPosition, Intangible>().each(
      [](const Velocity &vel, ScreenPosition &pos, const Intangible) {
        pos.x += vel.x * GetFrameTime();
        pos.y += vel.y * GetFrameTime();
      });

  playerEntity = createEntity("./data/tilesets/Pixel Crawler - Free "
                              "Pack/Entities/NPCS/Rogue/Idle/Idle-Sheet.png",
                              {1, 1}, DEFAULT_PLAYER_ENTITY_NAME);
  playerEntity.set<MaxSpeed>({DEFAULT_MAXSPEEDX, DEFAULT_MAXSPEEDY});
  playerEntity.set<Friction>({DEFAULT_FRICTION});

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

  // Initialize with a default small size, it will be resized immediately
  window.Init(100, 100, "AIRogue");

  resourceManager = std::make_unique<ResourceManager>();
  ECSInit(mapPath);

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

  std::cout << screenWidth << " " << screenHeight << std::endl;

  float mapWidthPx = map->GetWidth() * map->getTileWidth();
  float mapHeightPx = map->GetHeight() * map->getTileHeight();

  float zoomX = screenWidth / mapWidthPx;
  float zoomY = screenHeight / mapHeightPx;

  // camera.target = { mapWidthPx / 2.0f, mapHeightPx / 2.0f };
  // camera.offset = { screenWidth / 2.0f, screenHeight / 2.0f} ;
  camera.target = {0, 0};
  camera.offset = {0, 0};
  camera.rotation = 0.0f;
  camera.zoom = std::min(zoomX, zoomY);

  inputHandler = std::make_unique<InputHandler>(camera);

  rlImGuiSetup(true);
}

void Game::Update() {

  // TODO: This really shouldn't be here, move this later
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
      !ImGui::GetIO().WantCaptureMouse) {
    Vector2 mouseWorldPos = GetScreenToWorld2D(GetMousePosition(), camera);
    GamePosition gPos =
        map->ScreenCoordsToGameCoords(mouseWorldPos.x, mouseWorldPos.y);
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

  ecs.progress();
}

void Game::DrawPlayerInfoWindow() {
  ImGui::Begin("Player Entity");
  if (playerEntity.is_alive()) {
    if (const GamePosition *gPos = playerEntity.get<GamePosition>()) {
      ImGui::Text("Game Position: (%d, %d)", gPos->x, gPos->y);
    }
    if (const ScreenPosition *sPos = playerEntity.get<ScreenPosition>()) {
      ImGui::Text("Screen Position: (%.2f, %.2f)", sPos->x, sPos->y);
    }
    if (const Velocity *vel = playerEntity.get<Velocity>()) {
      ImGui::Text("Velocity: (%.2f, %.2f)", vel->x, vel->y);
    } else {
      ImGui::Text("Velocity: (0.00, 0.00)");
    }
    if (const MaxSpeed *speed = playerEntity.get<MaxSpeed>()) {
      ImGui::Text("Max Speed: (%.2f, %.2f)", speed->maxX, speed->maxY);
    }
    if (const Friction *friction = playerEntity.get<Friction>()) {
      ImGui::Text("Friction: %.2f", friction->value);
    }

    ImGui::Separator();
    bool isIntangible = playerEntity.has<Intangible>();
    if (ImGui::Button(isIntangible ? "Remove Intangible" : "Make Intangible")) {
      if (isIntangible) {
        playerEntity.remove<Intangible>();
      } else {
        playerEntity.add<Intangible>();
      }
    }

    ImGui::Separator();
    ImGui::Text("Velocity Vector:");

    ImVec2 canvasSize(120.0f, 120.0f);
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##vel_canvas", canvasSize);

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    ImVec2 center(canvasPos.x + canvasSize.x * 0.5f,
                  canvasPos.y + canvasSize.y * 0.5f);
    float maxRadius = canvasSize.x * 0.45f;

    // Draw background circle representing max speed
    drawList->AddCircleFilled(center, maxRadius, IM_COL32(40, 40, 40, 255));
    drawList->AddCircle(center, maxRadius, IM_COL32(100, 100, 100, 255));

    // Draw crosshair axes
    drawList->AddLine(ImVec2(center.x, canvasPos.y + 5),
                      ImVec2(center.x, canvasPos.y + canvasSize.y - 5),
                      IM_COL32(80, 80, 80, 255));
    drawList->AddLine(ImVec2(canvasPos.x + 5, center.y),
                      ImVec2(canvasPos.x + canvasSize.x - 5, center.y),
                      IM_COL32(80, 80, 80, 255));

    // Get current velocity and max speed
    const Velocity *vel = playerEntity.get<Velocity>();
    const MaxSpeed *maxSpeed = playerEntity.get<MaxSpeed>();

    if (vel && maxSpeed && maxSpeed->maxX > 0.0f && maxSpeed->maxY > 0.0f) {
      // Calculate scaled endpoint
      float scaleX = maxRadius / maxSpeed->maxX;
      float scaleY = maxRadius / maxSpeed->maxY;
      ImVec2 velEnd(center.x + vel->x * scaleX, center.y + vel->y * scaleY);

      // Draw velocity vector line and arrowhead/dot
      drawList->AddLine(center, velEnd, IM_COL32(50, 255, 50, 255), 2.0f);
      drawList->AddCircleFilled(velEnd, 3.0f, IM_COL32(50, 255, 50, 255));
    }

    // Center point
    drawList->AddCircleFilled(center, 2.0f, IM_COL32(255, 255, 255, 255));
  } else {
    ImGui::Text("Player entity not alive.");
  }
  ImGui::End();
}

void Game::DrawTileInfoWindow() {

  ImGui::Begin("Tile Info");

  if (hasClicked) {

    ImGui::Text("Map Coordinates: (%d, %d)", lastClickedPos.x,
                lastClickedPos.y);

    ImGui::Separator();

    if (validTileSelected && selectedTile.is_alive()) {

      ImGui::Text("Tile Entity ID: %lu", selectedTile.id());

      if (const ScreenPosition *sPos = selectedTile.get<ScreenPosition>()) {

        ImGui::Text("Screen Position: (%.2f, %.2f)", sPos->x, sPos->y);
      }

      if (selectedTile.has<BlocksTile>()) {

        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Blocks Path: YES");

      } else {

        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Blocks Path: NO");
      }

    } else {

      ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                         "No valid tile entity at this position.");
    }

  } else {

    ImGui::Text("Click anywhere on the map to inspect.");
  }

  ImGui::End();
}

void Game::DrawGameWindows() {

  DrawPlayerInfoWindow();

  DrawTileInfoWindow();
}

void Game::Draw() {
  camera.BeginMode();
  ecs.run_pipeline(renderPipeline);
  camera.EndMode();
}

void Game::handleInput() {
  std::vector<Command *> commands = inputHandler->handleInput();
  for (Command *cmd : commands) {
    if (cmd) {
      cmd->execute(playerEntity);
    }
  }
}

// =========== Old Input Handling, will probably use this later ===========
// TODO: It slightly bothers me that this logic is not inside inputHandler
// Also executing and then undoing it just sucks
// void Game::handleInput() {
//   Command *cmd = inputHandler.get()->handleInput();
//   if (cmd != nullptr) {
//     flecs::entity playerEntity =
//     ecs.lookup(DEFAULT_PLAYER_ENTITY_NAME.c_str());
//
//     cmd->execute(playerEntity);
//     if (auto moveCmd = dynamic_cast<MoveCommand*>(cmd)) {
//       const Position *playerPos = playerEntity.get<Position>();
//       bool inBounds = map.get()->IsInBounds(playerPos->x, playerPos->y);
//       // TODO: Check if there is a way to make this lambda not receive
//       BlocksTile bool blockedTile = ecs.filter<Position, BlocksTile>()
//         .find([&playerEntity, playerPos](flecs::entity entity, const Position
//         &pos, BlocksTile bT) {
//           if (entity.id() == playerEntity.id()) { return false; }
//           return playerPos->x == pos.x and playerPos->y == pos.y;
//         });
//       if (!inBounds or blockedTile) { moveCmd->undo(playerEntity); }
//     }
//   }
// } ;

void Game::Shutdown() {
  if (!window.IsReady())
    return;

  rlImGuiShutdown();
  window.Close();
}

bool Game::shouldClose() const { return window.ShouldClose(); }
