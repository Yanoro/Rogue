#include "Game.h"
#include "rlImGui.h"
#include "imgui.h"
#include "ResourceManager.h"

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
    std::cerr << "Filesystem error: " << e.what() << 
      std::endl;
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
    std::cerr << "Game::createEntity: Exception opening/reading file: " << e.what() << std::endl;
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
      .set<CharacterAnimation>(charAnimation);  
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
}

//TODO: Add checks for cases where there are multiple entities in the same position

void Game::Init(std::string mapPath) {
  if (window.IsReady()) return;

  // Use FLAG_BORDERLESS_WINDOWED_MODE for borderless fullscreen
  raylib::Window::SetConfigFlags(FLAG_VSYNC_HINT | FLAG_BORDERLESS_WINDOWED_MODE);

  window.Init(0, 0, "AIRogue");

  resourceManager = std::make_unique<ResourceManager>();
  map = std::make_unique<Map>(mapPath, *resourceManager);

  ECSInit();

  camera.target = {0, 0};
  camera.offset = {300, 50};
  camera.rotation = 0.0f;
  camera.zoom = 1.0f; 
 
  inputHandler = std::make_unique<InputHandler>(camera);

  // Ensure it starts on the primary monitor
  window.SetMonitor(0);

  window.SetTargetFPS(60);

  rlImGuiSetup(true);

}

void Game::Update() {
  ecs.progress();
}

void Game::Draw() {
  camera.BeginMode();
  map->Draw();
  ecs.run_pipeline(renderPipeline);
  camera.EndMode();
}

void Game::handleInput() {
  Command *cmd = inputHandler.get()->handleInput();
  if (cmd != nullptr) {
    flecs::entity playerEntity = ecs.lookup(DEFAULT_PLAYER_ENTITY_NAME.c_str());
    cmd->execute(playerEntity);
    if (auto moveCmd = dynamic_cast<MoveCommand*>(cmd)) {
      bool invalidMovement = ecs.query<Position>()
        .find([this](const Position &pos) {
          return not map.get()->IsInBounds(pos.x, pos.y);
        });
      if (invalidMovement) { moveCmd->undo(playerEntity); }
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
