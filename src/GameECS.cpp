#include "AI.h"
#include "OllamaAI.h"
#include "GeminiAI.h"
#include "OpenRouterAI.h"
#include "Components.h"
#include "DebugLog.h"
#include "DebugWindows.h"
#include "Defaults.h"
#include "DrawAsciiDebug.h"
#include "EntityInfoWindow.h"
#include "Game.h"

#include "AgentBrain.h"
#include "InteractionRegistry.h"
#include "PathFinding.h"
#include "StringUtils.hpp"
#include "flecs.h"
#include "imgui.h"
#include "raylib-cpp.hpp"
#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <fstream>
#include <mutex>
#include <queue>
#include <regex>
#include <vector>

// TODO: Adding checks to see if we are not repeating values seems like a good
// idea for instance we can't have two npcs with the same name

flecs::entity Game::createNPC(const GamePosition &pos, std::string name,
                              std::string characterBackground) {

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
          .set<WindowOnClick>({WindowType::NPCContextWindowType})
          .add<BlocksTile>();

  entity.add<CharacterTag>();

  std::string locations = "";
  std::string characterNames = "";
  Map *currMap = entity.world().get<MapResource>()->map;

  for (const auto &currName : currMap->GetAllLocationNames()) {
    locations += currName + ", ";
  }

  if (locations.length() >= 2) {
    locations.erase(locations.length() - 2);
  }

  if (characterNames.length() >= 2) {
    characterNames.erase(characterNames.length() - 2);
  }

  std::regex re1("%LOCATIONS%");
  std::regex re2("%BACKGROUND%");
  std::regex re3("%CHARACTERS%");

  std::string startingPrompt =
      std::regex_replace(DEFAULT_NPC_PROMPT, re1, locations);
  startingPrompt = std::regex_replace(startingPrompt, re2, characterBackground);
  startingPrompt = std::regex_replace(startingPrompt, re3, characterNames);
  startingPrompt += "\n";
  entity.set<NPCContext>({ {{"system", startingPrompt}}, ""});

  if (name == "") {
    flecs::entity_t id = entity.id();
    name = "NPC " + std::to_string(id);
  }
  entity.set<DisplayName>({name});
  entity.set<NameTagColor>({BLUE});
  entity.set_name(name.c_str());

  auto brain = std::make_unique<AgentBrain>(entity, name);
  brain->isStopped = debugWindowState ? debugWindowState->GetStopAllAI() : false;
  entity.set<AgentBrainWrapper>({std::move(brain)});
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
      [](flecs::entity e, const ScreenPosition &pos, const DisplayName &displayName) {
        Color tagColor = BLACK;
        if (e.has<NameTagColor>()) {
          tagColor = e.get<NameTagColor>()->color;
        }

        int fontSize = 12;
        int textWidth = MeasureText(displayName.name.c_str(), fontSize);
        
        float entityWidth = 32.0f;
        if (e.has<DrawAscii>()) {
          entityWidth = e.get<DrawAscii>()->width;
        } else if (e.has<Hitbox>()) {
          entityWidth = e.get<Hitbox>()->width;
        }
        
        ScreenPosition TextPos = {pos.x + entityWidth / 2.0f - textWidth / 2.0f, pos.y - 20.0f};
        DrawText(displayName.name.c_str(), TextPos.x, TextPos.y, fontSize, tagColor);
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
  ecs.system<GamePosition, ScreenPosition>().each(
      [this](flecs::entity e, const GamePosition oldGamePos,
             const ScreenPosition &screenPos) {
        const GamePosition newGamePos =
            map->ScreenCoordsToGameCoords(screenPos.x, screenPos.y);
        if (oldGamePos != newGamePos) {
          e.set<GamePosition>(newGamePos);
        }
      });
}

void Game::LoadMap(std::string mapPath) {
  hasClicked = false;
  validTileSelected = false;

  flecs::entity mapEntity = ecs.entity("CurrentMap");
  ecs.defer([&]() {
    mapEntity.children([](flecs::entity child) { child.destruct(); });
  });
  map = std::make_unique<Map>(mapEntity, mapPath, debugLog.get());
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
  // Static means that we don't have to recompile the regex every time
  // this function gets run
  static std::regex moveRegex(R"(\[MOVE_TO\s+(.+?)\s*\])",
                              std::regex_constants::icase);
  static std::regex talkToRegex(R"(\[TALK_TO\s+(.+?)\s*\])",
                                std::regex_constants::icase);
  static std::regex nothingRegex(R"(\[DO_NOTHING\])",
                                 std::regex_constants::icase);
  static std::regex charactersRegex(R"(\[CHARACTERS\])",
                                    std::regex_constants::icase);
  std::smatch match;

  if (std::regex_search(msg, match, moveRegex)) {
    return {NPCCommandType::MOVE_TO_LOCATION, match[1].str()};
  } else if (std::regex_search(msg, match, talkToRegex)) {
    return {NPCCommandType::TALK_TO, match[1].str()};
  } else if (std::regex_search(msg, match, nothingRegex)) {
    return {NPCCommandType::DO_NOTHING, ""};
  } else if (std::regex_search(msg, match, charactersRegex)) {
    return {NPCCommandType::CHARACTERS_QUERY, ""};
  }

  return {NPCCommandType::INVALID_COMMAND, ""};
}

void SendNewPrompt(flecs::entity entity, std::string prompt) {

}

void Game::ECSInitActionSystems() {
  ecs.observer<MOVE_TO_LOCATION_ACTION>()
      .event(flecs::OnSet)
      .each([this](flecs::entity entity, MOVE_TO_LOCATION_ACTION &action) {
        const GamePosition *pGamePos = entity.get<GamePosition>();
        if (!pGamePos)
          return;
        const GamePosition gamePos = *pGamePos;

        std::string response;
        Location *loc = action.location;
        if (loc == nullptr) {
          entity.remove<MOVE_TO_LOCATION_ACTION>();
          response = "System: You tried to move to an unknown location.";
          SendNewPrompt(entity, response);
          return;
        }
        const GamePosition target = loc->pos;

        entity.set<MOVE_THROUGH_PATH_ACTION>(
            {AStar(map.get(), gamePos, target)});

        ecs.observer<GamePosition>()
            .with(entity)
            .event(flecs::OnUpdate)
            .each([entity, target](flecs::iter &it, size_t,
                                   const GamePosition &pos) {
              if (!(target == pos))
                return;
              std::string response =
                  "System: You have arrived at your destination, what's next?";
              SendNewPrompt(entity, response);
              it.system().destruct();
            });
        entity.remove<MOVE_TO_LOCATION_ACTION>();
      });

  // Steer system through a path
  ecs.system<Velocity, Acceleration, GamePosition, ScreenPosition, Hitbox,
             MOVE_THROUGH_PATH_ACTION>()
      .each([this](flecs::entity entity, Velocity &vel, Acceleration &accel,
                   const GamePosition &currPos, const ScreenPosition &screenPos,
                   const Hitbox &hitbox, MOVE_THROUGH_PATH_ACTION &tPath) {
        if (tPath.path.empty()) {
          std::string name = entity.name().c_str();
          debugLog->LogInfo("Entity " + name + " has finished it's path");
          entity.remove<MOVE_THROUGH_PATH_ACTION>();
          
          if (auto pendingInt = entity.get<PendingPlayerInteraction>()) {
            if (pendingInt->targetEntity.is_alive()) {
              auto interactions = InteractionRegistry::GetAvailableInteractions(pendingInt->targetEntity);
              for (const auto& interaction : interactions) {
                if (interaction.name == pendingInt->interactionName) {
                  std::string msg = interaction.execute(entity, pendingInt->targetEntity, "");
                  if (debugLog) debugLog->Log(msg);
                  break;
                }
              }
            }
            entity.remove<PendingPlayerInteraction>();
          }
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

        ScreenPosition currPosScreen = map->GameCoordsToScreenCoords(currPos.x, currPos.y);
        currPosScreen += centerTile;
        if (entCenterScreenPos.Distance(currPosScreen) < 8.0f && currPos != currWaypoint) {
            tPath.path = AStar(map.get(), currPos, target);
            if (!tPath.path.empty()) {
                currWaypoint = tPath.path[0];
                currWaypointScreenPos = map->GameCoordsToScreenCoords(currWaypoint.x, currWaypoint.y);
                currWaypointScreenPos += centerTile;
            }
        }

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
        Acceleration pathAccel = desiredVelocity - vel;
        if (pathAccel.Length() > DEFAULT_WAYPOINT_ACCEL) {
            pathAccel = pathAccel.Normalize() * DEFAULT_WAYPOINT_ACCEL;
        }
        
        if (const Friction *f = entity.get<Friction>()) {
            if (vel.Length() > 3.0f) {
                float gravity = 9.8f;
                pathAccel += vel.Normalize().Scale(gravity * f->value);
            }
        }
        
        accel += pathAccel;

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

  // In case someone is moving towards an moving target
  // update their path whenever the target changes positions
  ecs.observer<GamePosition>()
      .event(flecs::OnSet)
      .each([this](flecs::iter &it, size_t i, GamePosition &target_pos) {
        flecs::entity target_entity = it.entity(i);

        auto chasers = it.world()
                           .filter_builder<>()
                           .with<MovingTowards>(target_entity)
                           .build();

        chasers.each([&it, target_pos, this](flecs::entity chaser) {
          const GamePosition *chaser_pos = chaser.get<GamePosition>();
          if (!chaser_pos)
            return;

          it.world().defer([&] {
            chaser.set<MOVE_THROUGH_PATH_ACTION>(
                {AStar(map.get(), *chaser_pos, target_pos)});
          });
        });
      });

  ecs.system<GamePosition>()
      .with<MovingTowards>(flecs::Wildcard)
      .each([this](flecs::iter &it, size_t i, GamePosition &pos) {
        // Get the target from the 2nd term
        flecs::entity target = it.pair(2).second();
        flecs::entity entity = it.entity(i);

        const GamePosition *targetPos = target.get<GamePosition>();

        if (!entity.has<MOVE_THROUGH_PATH_ACTION>()) {
          entity.set<MOVE_THROUGH_PATH_ACTION>(
              {AStar(map.get(), pos, *targetPos)});
        }

        if (Map::AreNeighbours(pos, *targetPos)) {
          entity.remove<MovingTowards>(target);
        }
      });



}

static std::mutex g_AIResponseMutex;
static std::vector<std::function<void()>> g_AIResponseQueue;

void Game::ECSInitAgentSystems() {
  // System to flush background AI thread callbacks on the main thread safely
  ecs.system("AIResponseQueueFlusher").iter([](flecs::iter &) {
    std::vector<std::function<void()>> queue_copy;
    {
      std::lock_guard<std::mutex> lock(g_AIResponseMutex);
      queue_copy = std::move(g_AIResponseQueue);
    }
    for (auto &func : queue_copy) {
      func();
    }
  });

  // TODO: Move this observer to a place just for observers
  ecs.observer<NPCContext>()
      .event(flecs::OnSet)
      .each([](flecs::entity e, NPCContext &ctx) {
        ctx.contextID = std::to_string(e.id());
      });

  ecs.observer<AIRequest>()
      .event(flecs::OnRemove)
      .each([](flecs::entity e, AIRequest &req) {
        req.stopSource.request_stop();
      });
  auto ai = ecs.get<AIBackend>();
  ecs.system<AIRequest, NPCContext>().each([this, ai](flecs::entity entity,
                                                      AIRequest &request,
                                                      NPCContext &ctx) {
    if (request.dispatched) return;
    request.dispatched = true;

    AI *aiPtr = ai->ptr.get();
    auto callBack = [entity, this](const std::string &token) mutable {
      // Run safely on background thread, queueing for main thread
      std::lock_guard<std::mutex> lock(g_AIResponseMutex);
      g_AIResponseQueue.push_back([entity, token, this]() {
        if (entity.is_alive()) {
          if (!entity.has<AIRequest>()) {
            debugLog->LogError("Callback ran without corresponding AIRequest!");
            return;
          }

          AIRequest *newRequest = entity.get_mut<AIRequest>();
          NPCContext *ctx = entity.get_mut<NPCContext>();
          if (token.empty()) {
            newRequest->finished = true;
            if (this->debugLog) {
                this->debugLog->LogInfo(std::string("AI Resp (") + entity.name().c_str() + "): " + newRequest->pendingResponse);
            }
            if (!ctx->history.empty() && !ctx->history.back().content.empty() && ctx->history.back().content.back() != '\n') {
              ctx->history.back().content += "\n";
            }
          } else {
            newRequest->pendingResponse += token;
            if (!ctx->history.empty() && ctx->history.back().role == "assistant") {
              ctx->history.back().content += token;
            } else {
              ctx->history.push_back({"assistant", token});
            }
          }
        }
      });
    };

    if (!request.prompt.empty()) {
      ctx.history.push_back({"user", request.prompt});
    }
    aiPtr->generateStream(ctx.contextID, ctx.history, callBack, request.stopSource.get_token());
  });

  ecs.system<AgentBrainWrapper>().iter([](flecs::iter &it, AgentBrainWrapper *brains) {
    float dt = it.delta_time() * 1000.0f;

    for (auto i : it) {
      brains[i].agBrain.get()->update(dt);
    }
  });
  ecs.system<Evolvable>("GrowthSystem").iter([](flecs::iter &it, Evolvable *evolvables) {
    float dt = it.delta_time();
    bool resourcesLoaded = false;
    ObjectFactory* factory = nullptr;
    Map* map = nullptr;

    for (auto i : it) {
      evolvables[i].timeRemaining -= dt;
      if (evolvables[i].timeRemaining <= 0.0f) {
        if (!resourcesLoaded) {
          auto factoryRes = it.world().get<ObjectFactoryResource>();
          factory = factoryRes ? factoryRes->factory : nullptr;
          auto mapRes = it.world().get<MapResource>();
          map = mapRes ? mapRes->map : nullptr;
          resourcesLoaded = true;
        }

        if (factory) {
          std::string nextStage = evolvables[i].nextStageTemplate;
          factory->ApplyTemplate(it.entity(i), nextStage, map);
        }
      }
    }
  });
};

void Game::ECSInit(std::string mapPath) {
  ecs.import <flecs::monitor>();
  ecs.set<flecs::Rest>({});

  objectFactory.SetDebugLog(debugLog.get());
  objectFactory.LoadTemplates("data/objects");
  ecs.set<ObjectFactoryResource>({&objectFactory});

  LoadMap(mapPath);

  RegisterComponents(ecs);

  InteractionRegistry::Clear();
  
  InteractionRegistry::RegisterComponentInteraction<Harvestable>("Harvest", [](flecs::entity actor, flecs::entity target, std::string args) -> std::string {
      std::string objName = target.has<DisplayName>() ? target.get<DisplayName>()->name : "Object";
      
      Harvestable* h = target.get_mut<Harvestable>();
      if (h->lootTable.drops.empty()) {
          return "System: The " + objName + " cannot be harvested because it has no loot table.\n";
      }

      if (h->amountRemaining > 0) {
        h->amountRemaining--;
        auto factoryRes = actor.world().get<ObjectFactoryResource>();
        std::string droppedItemsStr = "";

        if (factoryRes && factoryRes->factory) {
          flecs::world ecs = actor.world();
          
          for (const auto& drop : h->lootTable.drops) {
              float roll = (float)rand() / RAND_MAX;
              if (roll <= drop.chance) {
                  flecs::entity spawned = factoryRes->factory->SpawnObject(ecs, actor, nullptr, drop.itemType);
                  if (spawned.is_alive()) {
                      actor.add<Holds>(spawned);
                      droppedItemsStr += drop.itemType + ", ";
                  }
              }
          }
        }

        if (!droppedItemsStr.empty()) {
            droppedItemsStr.pop_back();
            droppedItemsStr.pop_back();
        } else {
            droppedItemsStr = "nothing";
        }

        std::string msg = "System: You harvested " + droppedItemsStr + " from " + objName + ".\n";
        if (h->amountRemaining <= 0) {
          msg += "The " + objName + " is depleted and destroyed.\n";
          target.destruct();
        }
        return msg;
      }
      return "System: The " + objName + " has no more resources to harvest.\n";
  });

  InteractionRegistry::RegisterComponentInteraction<Workstation>("Craft", [](flecs::entity actor, flecs::entity target, std::string args) -> std::string {
      return "System: Crafting menu opened (Not yet implemented).\n";
  });

  InteractionRegistry::RegisterComponentInteraction<DisplayName>("Examine", [](flecs::entity actor, flecs::entity target, std::string args) -> std::string {
      std::string objName = target.has<DisplayName>() ? target.get<DisplayName>()->name : "Object";
      std::string msg = "System: You examined the " + objName + ". It looks normal.\n";
      if (target.has<Harvestable>()) {
          const Harvestable* h = target.get<Harvestable>();
          msg += "You can harvest it " + std::to_string(h->amountRemaining) + " more time(s).\n";
      }
      if (target.has<Storage>()) {
          msg += "It can be used to [STORE $ITEM_NAME] or [TAKE $ITEM_NAME].\n";
          std::string items = "";
          target.each<Holds>([&](flecs::entity child) {
              if (child.is_alive() && child.has<DisplayName>()) {
                  items += child.get<DisplayName>()->name + ", ";
              }
          });
          if (!items.empty()) {
              items.pop_back();
              items.pop_back();
              msg += "It currently holds: " + items + ".\n";
          } else {
              msg += "It is currently empty.\n";
          }
      }
      return msg;
  });

  InteractionRegistry::RegisterComponentInteraction<Storage>("Store", [](flecs::entity actor, flecs::entity target, std::string args) -> std::string {
      if (args.empty()) {
          return "System: You must specify an item to store. Use [STORE $ITEM_NAME].\n";
      }

      flecs::entity itemToStore = flecs::entity::null();
      actor.each<Holds>([&](flecs::entity child) {
          if (child.is_alive() && child.has<DisplayName>()) {
              if (StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, args)) {
                  itemToStore = child;
              }
          }
      });

      if (!itemToStore.is_alive()) {
          return "System: You do not have an item named " + args + " to store.\n";
      }

      int currentCount = 0;
      target.each<Holds>([&](flecs::entity) { currentCount++; });
      if (currentCount >= target.get<Storage>()->capacity) {
          return "System: The storage is full.\n";
      }

      actor.remove<Holds>(itemToStore);
      target.add<Holds>(itemToStore);
      itemToStore.child_of(target);

      return "System: You stored the " + itemToStore.get<DisplayName>()->name + ".\n";
  });

  InteractionRegistry::RegisterComponentInteraction<Storage>("Take", [](flecs::entity actor, flecs::entity target, std::string args) -> std::string {
      if (args.empty()) {
          return "System: You must specify an item to take. Use [TAKE $ITEM_NAME].\n";
      }

      flecs::entity itemToTake = flecs::entity::null();
      target.each<Holds>([&](flecs::entity child) {
          if (child.is_alive() && child.has<DisplayName>()) {
              if (StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, args)) {
                  itemToTake = child;
              }
          }
      });

      if (!itemToTake.is_alive()) {
          return "System: The storage does not have an item named " + args + ".\n";
      }

      target.remove<Holds>(itemToTake);
      actor.add<Holds>(itemToTake);
      itemToTake.child_of(actor);

      return "System: You took the " + itemToTake.get<DisplayName>()->name + ".\n";
  });

  std::unique_ptr<AI> ai;
  std::ifstream f("model.json");
  // TODO: Add more options
  if (!f.is_open()) {
    std::cerr << "Error: model.json not found! Falling back to default Ollama model." << std::endl;
    ai = std::make_unique<OllamaAI>("llama3");
    ai->setErrorLogger([this](const std::string& err) { if (debugLog) debugLog->LogError(err); });
  } else {
    try {
      nlohmann::json j;
      f >> j;

      std::string type = j.value("type", "ollama");
      std::string model = j.value("model", "llama3");

      if (type == "gemini") {
        std::string apiKey = j.value("apiKey", "");
        auto gemini = std::make_unique<GeminiAI>(apiKey, model);
        gemini->setErrorLogger([this](const std::string& err) { if (debugLog) debugLog->LogError(err); });
        ai = std::move(gemini);
      } else if (type == "openrouter") {
        std::string apiKey = j.value("apiKey", "");
        auto openrouter = std::make_unique<OpenRouterAI>(apiKey, model);
        openrouter->setErrorLogger([this](const std::string& err) { if (debugLog) debugLog->LogError(err); });
        if (j.contains("temperature")) {
          openrouter->setOption("temperature", j["temperature"]);
        }
        if (j.contains("max_tokens")) {
          openrouter->setOption("max_tokens", j["max_tokens"]);
        }
        ai = std::move(openrouter);
      } else {
        std::string endpoint = j.value("endpoint", "http://localhost:11434/api/generate");
        auto ollama = std::make_unique<OllamaAI>(model, endpoint);
        ollama->setErrorLogger([this](const std::string& err) { if (debugLog) debugLog->LogError(err); });
        
        if (j.contains("temperature")) {
          ollama->setOption("temperature", j["temperature"]);
        }
        ai = std::move(ollama);
      }
      
      if (ai) {
        ai->setErrorLogger([this](const std::string& err) {
          if (this->debugLog) {
            this->debugLog->LogError(err);
          }
        });
      }
    } catch (const std::exception& e) {
      std::cerr << "Error parsing model.json: " << e.what() << std::endl;
      ai = std::make_unique<OllamaAI>("llama3");
      ai->setErrorLogger([this](const std::string& err) { if (debugLog) debugLog->LogError(err); });
    }
  }

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
  playerEntity.add<CharacterTag>();

  createNPC({20, 1}, "Pietro",
            "Your name is Pietro, you are a young man that loves apple trees.");

  // Initialize debug window entities
  debugConsoleWindowEntity = ecs.entity("Debug Console Window");
  tileInfoWindowEntity = ecs.entity("Tile Info Window");
  astarWindowEntity = ecs.entity("A* Window");
  entityOverviewWindowEntity = ecs.entity("Entity Overview Window");
  debugLogWindowEntity = ecs.entity("Debug Log Window");
  mapReloadWindowEntity = ecs.entity("Map Reload Window");
  drawAsciiToggleWindowEntity = ecs.entity("DrawAscii Debug Window");
  fontSelectionWindowEntity = ecs.entity("Font Selection Window");
  mapEditorWindowEntity = ecs.entity("Map Editor Window");
  aiMenuWindowEntity = ecs.entity("AI Menu Window");

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
  if (debugWindowState->GetShowMapEditorWindow()) {
    mapEditorWindowEntity.set<ActiveWindow>(
        {std::make_shared<MapEditorWindow>(this)});
  }
  if (debugWindowState->GetShowAIMenuWindow()) {
    aiMenuWindowEntity.set<ActiveWindow>(
        {std::make_shared<AIMenuWindow>(this)});
  }
  if (debugWindowState->GetShowEntityInfoWindow()) {
    playerEntity.set<ActiveWindow>(
        {std::make_shared<EntityInfoWindow>(playerEntity)});
  }
}
