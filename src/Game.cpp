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

  ecs.system<Drawable, ScreenPosition>().kind<Render>().each(
      [](const Drawable &draw, const ScreenPosition &pos) {
        draw.texture->Draw(draw.srcRect, pos);
      });

  ecs.system<ScreenPosition, const Velocity>().each(
      [](ScreenPosition &pos, const Velocity &vel) {
        pos.x += vel.x * GetFrameTime();
        pos.y += vel.y * GetFrameTime();
      });

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
      [&tilesBlocked](const GamePosition &pos, const BlocksTile bt) {
        tilesBlocked.push_back(pos);
      });

  // Add tangible tag
  // ecs.system<Velocity, ScreenPosition>().each(
  //     [&tilesBlocked](Velocity &vel, const ScreenPosition &pos) {
  //       Position newPos{pos.x + vel.x, pos.y + vel.y};
  //       if (std::find(tilesBlocked.begin(), tilesBlocked.end(), newPos) !=
  //           tilesBlocked.end()) {
  //         if (pos.x != newPos.x) {
  //           vel.x = 0;
  //         }
  //         if (pos.y != newPos.y) {
  //           vel.y = 0;
  //         }
  //       }
  //     });

  playerEntity = createEntity("./data/tilesets/Pixel Crawler - Free "
                              "Pack/Entities/NPCS/Rogue/Idle/Idle-Sheet.png",
                              {1, 1}, DEFAULT_PLAYER_ENTITY_NAME);
  playerEntity.set<MaxSpeed>({DEFAULT_MAXSPEED_X, DEFAULT_MAXSPEED_Y});
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

  // Ensure it starts on the current monitor
  int currentMonitor = GetCurrentMonitor();

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

void Game::Update() { ecs.progress(); }

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
  } else {
    ImGui::Text("Player entity not alive.");
  }
  ImGui::End();
}

void Game::DrawGameWindows() {
  DrawPlayerInfoWindow();
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
