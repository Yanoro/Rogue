#include "Game.h"
#include "rlImGui.h"
#include <fstream>
#include <iostream>

Game::Game() {}

Game::~Game() {
  Shutdown();
}

std::string findFirstJsonFile(const std::string& directoryPath)  {
  try {
    if (!std::filesystem::exists(directoryPath) or !std::filesystem::is_directory(directoryPath)) {
      return ""; // Directory doesn't exist
    }

    for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
      if (entry.is_regular_file() && entry.path().extension() == ".json") {
        return entry.path().string();
      }
    }
  } catch (const std::filesystem::filesystem_error& e) {
    std::cerr << "Filesystem error: " << e.what() << std::endl;
  }
  return ""; // No JSON file found
}

// TODO: Function either works or throws an exception,
// Probably a better way to do this
flecs::entity Game::createEntity(const std::string &texturePath, const Position &pos, 
                        std::string entityName = "") {
  std::string jsonPath = findFirstJsonFile(std::filesystem::path(texturePath).parent_path().string());
  std::ifstream jsonFile;
  jsonFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);

  try {
    jsonFile.open(jsonPath);
  }
  catch (const std::ifstream::failure &e) {
    std::cerr << "Game::createEntity: Exception opening/reading file: " << texturePath << " " << e.what() << std::endl;
    throw;
  }

  try {
    auto json = nlohmann::json::parse(jsonFile);
    unsigned int frameCount = json["frames"].size(); 
    raylib::Texture *characterTexture = resourceManager->GetTexture(texturePath); 
    CharacterAnimation charAnimation(characterTexture, frameCount);
    for (const auto &currFrame :json["frames"]) {
      double duration = currFrame.value("duration", 100.0) / 1000; // From seconds to miliseconds
      auto currRect = currFrame["frame"];
      raylib::Rectangle frameRect = {currRect["x"], currRect["y"], currRect["w"], currRect["h"]};
      charAnimation.frames.emplace_back(Frame{frameRect, duration});
    }
    return ecs.entity(entityName.c_str())
      .set<Position>(pos)
      .set<CharacterAnimation>(charAnimation)
      .add<BlocksTile>();  
  } catch (const std::exception &e) {
    std::cerr << "JSON Parse Error: " << e.what() << " jsonPath: " << jsonPath << std::endl;
    throw; 
  }
}

void Game::ECSInit() {
  ecs.import<flecs::monitor>();
  ecs.set<flecs::Rest>({});

  renderPipeline = ecs.pipeline()
    .with(flecs::System)
    .with<Render>()
    .build();

  ecs.system<Drawable, Position>()
    .kind<Render>()
    .each([this](const Drawable &draw, const Position &pos) {
      ScreenCoords screenCoords = map->MapCoordsToScreenCoords(pos.x, pos.y);
      raylib::Vector2 position(screenCoords.first, screenCoords.second);
      draw.texture->Draw(draw.srcRect, position);
    });

  
  ecs.system<Position, CharacterAnimation>()
    .kind<Render>()
    .each([this](const Position &pos, CharacterAnimation &characterAnimation) {

      raylib::Texture *texture = characterAnimation.texture;
      unsigned int currFrame = characterAnimation.currentFrame;
      ScreenCoords screenCoords = map->MapCoordsToScreenCoords(pos.x, pos.y);
      raylib::Vector2 position(screenCoords.first, screenCoords.second);
      texture->Draw(characterAnimation.frames[currFrame].frameRect, position);
      double currTime = GetTime();
      if (currTime - characterAnimation.lastFrameTime > characterAnimation.frames[currFrame].duration) {
        characterAnimation.currentFrame = (currFrame + 1) % characterAnimation.frameCount;
        characterAnimation.lastFrameTime = currTime;
      }
    });

  playerEntity = createEntity("./data/tilesets/Pixel Crawler - Free Pack/Entities/NPCS/Rogue/Idle/Idle-Sheet.png", {1, 1}, DEFAULT_PLAYER_ENTITY_NAME);
  createEntity("./data/tilesets/Pixel Crawler - Free Pack/Entities/NPCS/Knight/Idle/Idle-Sheet.png", 
               {20, 1});
  createEntity("./data/tilesets/Pixel Crawler - Free Pack/Entities/NPCS/Wizzard/Idle/Idle-Sheet.png", 
               {30, 1});
}

void Game::Init(std::string mapPath) {
  if (window.IsReady()) return;

  // Use FLAG_BORDERLESS_WINDOWED_MODE for borderless fullscreen
  raylib::Window::SetConfigFlags(FLAG_VSYNC_HINT | FLAG_BORDERLESS_WINDOWED_MODE);

  window.Init(0, 0, "AIRogue");

  resourceManager = std::make_unique<ResourceManager>();
  ECSInit();
  map = std::make_unique<Map>(mapPath, *resourceManager, ecs); 

 // Ensure it starts on the primary monitor
  window.SetMonitor(0);

  window.SetTargetFPS(60);

  float screenWidth = 1080;
  float screenHeight = 1920;

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
  ecs.progress();
}

void Game::Draw() {
  camera.BeginMode();
  ecs.run_pipeline(renderPipeline);
  camera.EndMode();
}

// TODO: It slightly bothers me that this logic is not inside inputHandler
// Also executing and then undoing it just sucks
void Game::handleInput() {
  Command *cmd = inputHandler.get()->handleInput();
  if (cmd != nullptr) {
    flecs::entity playerEntity = ecs.lookup(DEFAULT_PLAYER_ENTITY_NAME.c_str());
    
    cmd->execute(playerEntity);
    if (auto moveCmd = dynamic_cast<MoveCommand*>(cmd)) {
      const Position *playerPos = playerEntity.get<Position>();
      bool inBounds = map.get()->IsInBounds(playerPos->x, playerPos->y);
      // TODO: Check if there is a way to make this lambda not receive BlocksTile
      bool blockedTile = ecs.filter<Position, BlocksTile>()
        .find([&playerEntity, playerPos](flecs::entity entity, const Position &pos, BlocksTile bT) {
          if (entity.id() == playerEntity.id()) { return false; }
          return playerPos->x == pos.x and playerPos->y == pos.y;
        });
      if (!inBounds or blockedTile) { moveCmd->undo(playerEntity); }
    } 
  }
}                                                                               ;

void Game::Shutdown() {
  if (!window.IsReady()) return;

  rlImGuiShutdown();
  window.Close();
}

bool Game::shouldClose() const {
  return window.ShouldClose();
}
