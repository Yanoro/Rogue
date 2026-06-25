#include "AI.h"
#include "Components.h"
#include "DebugLog.h"
#include "DebugWindows.h"
#include "Defaults.h"
#include "DrawAsciiDebug.h"
#include "EntityInfoWindow.h"
#include "Game.h"
#include "NPC.h"
#include "PathFinding.h"
#include "raylib-cpp.hpp"
#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <queue>
#include <regex>
#include <vector>

flecs::entity Game::createNPC(const GamePosition &pos, std::string name,
                              std::string prompt) {

  flecs::entity entity =
      ecs.entity()
          .set<ScreenPosition>(map->GameCoordsToScreenCoords(pos.x, pos.y))
          .set<GamePosition>(pos)
          .set<Hitbox>(
              {DEFAULT_ENTITY_HITBOX_WIDTH, DEFAULT_ENTITY_HITBOX_HEIGHT})
          .set<DrawAscii>({
              '@',
              {128, 0, 0, 255},
              {128, 128, 128, 0},
              DEFAULT_ENTITY_VISUAL_WIDTH,
              DEFAULT_ENTITY_VISUAL_HEIGHT,

          })
          .set<WindowOnClick>({WindowType::EntityInfoWindowType})
          .set<Velocity>({0, 0})
          .set<Acceleration>({0, 0})
          .set<Friction>({DEFAULT_ENTITY_FRICTION})
          .add<BlocksTile>()
          .set<NPCNewPrompt>({prompt});

  if (name == "") {
    flecs::entity_t id = entity.id();
    name = "NPC " + std::to_string(id);
  }
  entity.set<DisplayName>({name});
  return entity;
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

  ecs.system<ScreenPosition, DisplayName>().kind<Render>().each(
      [](const ScreenPosition &pos, const DisplayName &displayName) {
        ScreenPosition TextPos = {pos.x, pos.y - 25};
        DrawText(displayName.name.c_str(), TextPos.x, TextPos.y, 12, BLACK);
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

  // Steer system through a path
  ecs.system<Velocity, Acceleration, GamePosition, ScreenPosition, Hitbox,
             TargetPath>()
      .each([this](flecs::entity entity, Velocity &vel, Acceleration &accel,
                   const GamePosition &currPos, const ScreenPosition &screenPos,
                   const Hitbox &hitbox, TargetPath &tPath) {
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
            screenPos + (raylib::Vector2(hitbox.width, hitbox.height) / 2.0f);

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

            raylib::Rectangle entRect(screenPos.x, screenPos.y, hitbox.width,
                                      hitbox.height);
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

  // Collision system
  ecs.system<Velocity, Hitbox, ScreenPosition>().without<Intangible>().each(
      [this](Velocity &vel, const Hitbox &hitbox,
             const ScreenPosition &origScreenPos) {
        ScreenPosition newScreenPos = origScreenPos + (vel * GetFrameTime());
        // TODO: Why are we taking the min here?
        size_t hitboxSize = std::min(hitbox.width, hitbox.height);

        bool nullXVector = false;
        bool nullYVector = false;

        if ((newScreenPos.x < 0) ||
            (newScreenPos.x + hitbox.width > map->GetMapWidthPx())) {
          nullXVector = true;
        }
        if ((newScreenPos.y < 0) ||
            (newScreenPos.y + hitbox.height > map->GetMapHeightPx())) {
          nullYVector = true;
        }

        raylib::Rectangle deltaXRect(newScreenPos.x, origScreenPos.y,
                                     hitboxSize, hitboxSize);
        raylib::Rectangle deltaYRect(origScreenPos.x, newScreenPos.y,
                                     hitboxSize, hitboxSize);

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
                                     currTile->hitbox.width,
                                     currTile->hitbox.height);
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

void Game::LoadMap(std::string mapPath) {
  hasClicked = false;
  validTileSelected = false;

  flecs::entity mapEntity = ecs.entity("CurrentMap");
  ecs.defer([&]() {
    mapEntity.children([](flecs::entity child) { child.destruct(); });
  });
  map = std::make_unique<Map>(mapEntity, mapPath);
  ecs.set<MapResource>({map.get()});

  Map *currentMap = map.get();
  // Teleport entities that get stuck to safety
  // TODO: Maybe have a better tag instead of velocity
  // to tell when an entity should be teleported out
  if (currentMap) {
    ecs.filter<Velocity, Hitbox, GamePosition, ScreenPosition>().each(
        [currentMap](const Velocity &, const Hitbox &hitbox, GamePosition &gPos,
                     ScreenPosition &sPos) {
          auto isCollidingAt = [currentMap, &sPos,
                                &hitbox](const GamePosition &pos) {
            ScreenPosition screenPos =
                currentMap->GameCoordsToScreenCoords(pos.x, pos.y);
            Rectangle rect = {sPos.x, sPos.y, (float)hitbox.width,
                              (float)hitbox.height};

            struct RelativeTile {
              int dx;
              int dy;
            };
            RelativeTile offsets[] = {{0, 0}, {0, -1}, {1, 0}, {1, 1}, {0, 1}};

            for (const auto &offset : offsets) {
              int tx = pos.x + offset.dx;
              int ty = pos.y + offset.dy;
              Tile *t = currentMap->GetTile(tx, ty);
              bool blocks = (!t) || t->blocksTile;
              if (blocks) {

                ScreenPosition tileSPos =
                    currentMap->GameCoordsToScreenCoords(tx, ty);
                Rectangle tileRect = {tileSPos.x, tileSPos.y,
                                      (float)currentMap->GetTileWidth(),
                                      (float)currentMap->GetTileHeight()};
                if (CheckCollisionRecs(rect, tileRect)) {
                  return true;
                }
              }
            }
            return false;
          };

          if (isCollidingAt(gPos)) {
            int mapWidth = currentMap->GetWidth();
            int mapHeight = currentMap->GetHeight();
            std::vector<bool> visited(mapWidth * mapHeight, false);
            std::queue<GamePosition> q;

            int startX = std::max(0, std::min(gPos.x, mapWidth - 1));
            int startY = std::max(0, std::min(gPos.y, mapHeight - 1));
            GamePosition clampedStart = {startX, startY};

            q.push(clampedStart);
            visited[clampedStart.y * mapWidth + clampedStart.x] = true;

            const int dx[] = {0, 0, -1, 1, -1, -1, 1, 1};
            const int dy[] = {-1, 1, 0, 0, -1, 1, -1, 1};

            bool found = false;
            GamePosition safePos = gPos;

            while (!q.empty()) {
              GamePosition curr = q.front();
              Tile *currTile = currentMap->GetTile(curr.x, curr.y);
              q.pop();

              if (currTile && !currTile->blocksTile) {
                safePos = curr;
                found = true;
                break;
              }

              for (int i = 0; i < 8; ++i) {
                GamePosition next = {curr.x + dx[i], curr.y + dy[i]};
                if (currentMap->IsInBounds(next.x, next.y)) {
                  int index = next.y * mapWidth + next.x;
                  if (!visited[index]) {
                    visited[index] = true;
                    q.push(next);
                  }
                }
              }
            }

            if (found) {
              gPos = safePos;
              sPos = currentMap->GameCoordsToScreenCoords(gPos.x, gPos.y);
            }
          }
        });
  }
}

MessageCommand ParseMessageCommand(std::string msg) {
  // Static means that we don't have to recompile every time
  // this function gets run
  static std::regex moveRegex(R"(\[MOVE_TO\s+(.+?)\s*\])",
                              std::regex_constants::icase);
  static std::regex nothingRegex(R"(\[DO_NOTHING\])",
                                 std::regex_constants::icase);
  std::smatch match;

  if (std::regex_search(msg, match, moveRegex)) {
    return {NPCCommandType::MOVE_TO, match[1].str()};
  } else if (std::regex_search(msg, match, nothingRegex)) {
    return {NPCCommandType::DO_NOTHING, ""};
  }

  return {NPCCommandType::NONE, ""};
}

void SendNewPrompt(flecs::entity entity, std::string prompt) {
  entity.set<NPCNewPrompt>({"\n" + prompt});
}

void Game::ECSInitActionSystems() {
  ecs.system<DO_NOTHING_ACTION>().each(
      [](flecs::entity entity, DO_NOTHING_ACTION &action) {
        action.time_remaining -= GetTime();
        if (action.time_remaining > 0) {
          return;
        }
        std::string response =
            "World: You have awaited for a while, what's next?\n";
        entity.remove<DO_NOTHING_ACTION>();
        SendNewPrompt(entity, response);
      });

  ecs.system<MOVE_TO_ACTION, GamePosition>().each(
      [this](flecs::entity entity, MOVE_TO_ACTION &action,
             const GamePosition &gamePos) {
        Location *loc = action.location;
        const GamePosition target = loc->pos;
        if (gamePos == target) {
          entity.remove<MOVE_TO_ACTION>();
          std::string response =
              "WORLD: You have arrived at your destination, what's next?\n";
          SendNewPrompt(entity, response);
        } else if (!entity.has<TargetPath>()) {
          entity.set<TargetPath>({AStar(map.get(), gamePos, target)});
        }
      });
}

void Game::ECSInitAgentSystems() {
  // TODO: Move this observer to a place just for observers
  ecs.observer<NPCContext>()
      .event(flecs::OnSet)
      .each([](flecs::entity e, NPCContext &ctx) {
        ctx.contextID = std::to_string(e.id());
      });

  auto ai = ecs.get<AIBackend>();
  ecs.system<NPCNewPrompt>().each([this, ai](flecs::entity entity,
                                             const NPCNewPrompt &prompt) {
    NPCContext *ctx = entity.get_mut<NPCContext>();
    AIRequest *request = entity.get_mut<AIRequest>();

    if (!entity.has<AIRequest>()) {
      flecs::world async_stage = ecs.async_stage();
      AI *aiPtr = ai->ptr.get();

      auto callBack = [entity, &async_stage](const std::string &token) mutable {
        if (entity.is_alive()) {
          async_stage.defer([entity, token]() {
            AIRequest *newRequest = entity.get_mut<AIRequest>();
            if (!token.empty()) {
              newRequest->finished = true;
            } else {
              newRequest->pendingResponse += token;
            }
          });
        }
      };

      ctx->context += prompt.msg;
      aiPtr->generateStream(ctx->contextID, ctx->context, callBack);
    } else {
      if (!request->finished) {
        return;
      }
      ctx->context += request->pendingResponse;
      MessageCommand msgCmd = ParseMessageCommand(request->pendingResponse);

      entity.set<MessageCommand>(msgCmd);
      entity.remove<AIRequest>();
      entity.remove<NPCNewPrompt>();
    }
  });

  ecs.system<MessageCommand>().each(
      [this](flecs::entity entity, const MessageCommand &cmd) {
        std::string response = "\n";

        switch (cmd.type) {
        case (NPCCommandType::DO_NOTHING): {
          entity.set<DO_NOTHING_ACTION>(
              {DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS});
          break;
        }
        case (NPCCommandType::MOVE_TO): {
          Location *loc = map->GetLocation(cmd.target);
          if (loc != nullptr) {
            entity.set<MOVE_TO_ACTION>({loc});
          } else {
            response += "WORLD: You tried to move to an unknown location.\n";
            SendNewPrompt(entity, response);
          }
          break;
        }
        case (NPCCommandType::NONE): {
          SendNewPrompt(entity, "");
          break;
        }
        default:
          break;
        }
      });
}

void Game::ECSInit(std::string mapPath) {
  ecs.import <flecs::monitor>();
  ecs.set<flecs::Rest>({});

  LoadMap(mapPath);

  RegisterComponents(ecs);

  auto ai = std::make_unique<OllamaAI>("llama3");
  ecs.set<AIBackend>({std::move(ai)});

  ECSInitPhysicsSystems();
  ECSInitLogicSystems();
  ECSInitRenderSystems();
  ECSInitAgentSystems();
  ECSInitActionSystems();

  GamePosition startPlayerPos = {14, 14};
  playerEntity = ecs.entity(DEFAULT_PLAYER_ENTITY_NAME.c_str());
  playerEntity.set<GamePosition>(startPlayerPos);

  playerEntity.set<ScreenPosition>(
      map->GameCoordsToScreenCoords(startPlayerPos.x, startPlayerPos.y));
  playerEntity.set<MaxSpeed>({DEFAULT_MAXSPEED});
  playerEntity.set<Friction>({DEFAULT_PLAYER_FRICTION});
  playerEntity.set<Velocity>({0, 0});
  playerEntity.set<Acceleration>({0, 0});
  playerEntity.set<Hitbox>(
      {DEFAULT_PLAYER_HITBOX_WIDTH, DEFAULT_PLAYER_HITBOX_HEIGHT});
  playerEntity.set<DrawAscii>({
      '@',
      {128, 0, 128, 255},
      {128, 128, 128, 0},
      DEFAULT_PLAYER_VISUAL_WIDTH,
      DEFAULT_PLAYER_VISUAL_HEIGHT,

  });
  playerEntity.set<WindowOnClick>({WindowType::EntityInfoWindowType});

  createNPC({20, 1}, "John",
            "Your name is John, you like staying at thelibrary");
  createNPC({30, 1}, "Carl", "Your name is Carl, you like walking around town");

  // Initialize debug window entities
  debugConsoleWindowEntity = ecs.entity("Debug Console Window");
  tileInfoWindowEntity = ecs.entity("Tile Info Window");
  astarWindowEntity = ecs.entity("A* Window");
  entityOverviewWindowEntity = ecs.entity("Entity Overview Window");
  debugLogWindowEntity = ecs.entity("Debug Log Window");
  mapReloadWindowEntity = ecs.entity("Map Reload Window");
  drawAsciiToggleWindowEntity = ecs.entity("DrawAscii Debug Window");
  fontSelectionWindowEntity = ecs.entity("Font Selection Window");

  // Apply loaded state to the entities
  if (debugWindowState->GetShowDebugConsole()) {
    debugConsoleWindowEntity.set<ActiveWindow>(
        {std::make_shared<DebugConsoleWindow>(this)});
  }
  if (debugWindowState->GetShowTileInfoWindow()) {
    tileInfoWindowEntity.set<ActiveWindow>(
        {std::make_shared<TileInfoWindow>(this)});
  }
  if (debugWindowState->GetShowAStarWindow()) {
    astarWindowEntity.set<ActiveWindow>({std::make_shared<AStarWindow>(this)});
  }
  if (debugWindowState->GetShowEntityOverviewWindow()) {
    entityOverviewWindowEntity.set<ActiveWindow>(
        {std::make_shared<EntityOverviewWindow>(this)});
  }
  if (debugWindowState->GetShowDebugLogWindow()) {
    debugLogWindowEntity.set<ActiveWindow>(
        {std::make_shared<DebugLogWindow>(this)});
  }
  if (debugWindowState->GetShowMapReloadWindow()) {
    mapReloadWindowEntity.set<ActiveWindow>(
        {std::make_shared<MapReloadWindow>(this)});
  }
  if (debugWindowState->GetShowDrawAsciiToggleWindow()) {
    drawAsciiToggleWindowEntity.set<ActiveWindow>(
        {std::make_shared<DrawAsciiDebugWindow>(this)});
  }
  if (debugWindowState->GetShowFontSelectionWindow()) {
    fontSelectionWindowEntity.set<ActiveWindow>(
        {std::make_shared<FontSelectionWindow>(this)});
  }
  if (debugWindowState->GetShowEntityInfoWindow()) {
    playerEntity.set<ActiveWindow>(
        {std::make_shared<EntityInfoWindow>(playerEntity)});
  }
}
