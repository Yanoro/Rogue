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
#include "StorageWindow.h"
#include "Game.h"

#include "AgentBrain.h"
#include "AgentActions.h"
#include "CharacterStatusWindow.h"
#include "GlobalCommandRegistry.h"
#include "InteractionRegistry.h"
#include "PathFinding.h"
#include "EffectResolver.hpp"
#include "Equipment.hpp"
#include "EquipmentRuntime.hpp"
#include "FloatingText.hpp"
#include "ItemRegistry.h"
#include "RaceRegistry.h"
#include "Rng.hpp"
#include "SkillTraining.hpp"
#include "StartingSkills.hpp"
#include "StringUtils.hpp"
#include "flecs.h"
#include "imgui.h"
#include "raylib-cpp.hpp"
#include "raylib.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <fstream>
#include <mutex>
#include <queue>
#include <regex>
#include <set>
#include <vector>

// Gives a character the full standard stat set at its neutral values.
//
// Every value is the registry baseline, so a freshly spawned character is
// mechanically neutral: nothing is faster or slower until something raises a
// stat. A future race that lacks a stat will simply omit it from this list, and
// the baseline fallback in StatView makes that omission harmless -- which is why
// there is deliberately no "default block" to keep in sync anywhere.
static void GiveDefaultStats(flecs::entity character,
                             const StatRegistry *registry) {
  if (!registry) {
    return;
  }
  StatBlock block;
  for (const StatDef *def : registry->GetAll()) {
    block.values.push_back({def->id, def->baseline});
  }
  character.set<StatBlock>(block);
}

// Gives a character every registered skill, untrained.
//
// Every character carries the full skill list at level 0, the way it carries the
// full stat list at baseline. An absent skill means the same thing as level 0 --
// untrained, and therefore neutral -- so this exists for visibility and to keep
// the block's shape uniform, not because the effect system requires it.
static void GiveStartingSkills(flecs::entity character,
                               const SkillRegistry *registry) {
  if (!registry) {
    return;
  }
  SkillBlock block;
  for (const SkillDef *def : registry->GetAll()) {
    SkillValue value;
    value.id = def->id;
    value.level = 0;
    value.xp = 0;
    block.values.push_back(value);
  }
  character.set<SkillBlock>(block);
}

// Stamps authored starting proficiency (the map's "skills" object) onto the
// untrained block every character is given above.
//
// Content errors are reported and skipped rather than fatal, exactly like an
// unknown object type in a map: a typo in one NPC's skills must not abort the
// load. A skill the registry does not know and a stage name the skill does not
// declare both land here, because both are only decidable against the registry
// and the map deliberately does not hold one.
static void ApplyStartingSkills(flecs::entity character,
                                const SkillRegistry *registry,
                                const std::vector<StartingSkill> &startingSkills,
                                DebugLog *debugLog) {
  if (!registry || startingSkills.empty() || !character.has<SkillBlock>()) {
    return;
  }
  SkillBlock *block = character.get_mut<SkillBlock>();
  if (!block) {
    return;
  }

  auto warn = [debugLog](const std::string &message) {
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };

  for (const StartingSkill &spec : startingSkills) {
    const SkillDef *def = registry->Get(spec.id);
    if (!def) {
      warn("Warning: Starting skill '" + spec.id +
           "' is not a known skill; skipping it.");
      continue;
    }
    std::string reason;
    if (!ApplyStartingSkill(*block, *def, spec, &reason)) {
      warn("Warning: Starting skill '" + spec.id +
           "' could not be applied: " + reason + ".");
    }
  }
}

// Decides the body slots a character is born with.
//
// Precedence, and every fallback exists so a map written before races existed
// still spawns a wearable character rather than a naked one: an authored list on
// the NPC wins, otherwise the NPC's race, otherwise the "human" race, otherwise
// the code-level humanoid body. A race is content like a skill, so an unknown id
// is a warning and a fallback, never a load failure.
static void GiveSlots(flecs::entity character, const RaceRegistry *races,
                      const std::string &raceId,
                      const std::vector<SlotSpec> &authored,
                      DebugLog *debugLog) {
  std::vector<SlotSpec> slots = authored;

  if (slots.empty() && races) {
    const std::string wanted = raceId.empty() ? std::string("human") : raceId;
    if (const RaceDef *def = races->Get(wanted)) {
      slots = def->slots;
    } else if (!raceId.empty()) {
      const std::string message =
          "Warning: NPC race '" + raceId +
          "' is not a known race; using the default humanoid body instead.";
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    }
  }

  if (slots.empty()) {
    slots = DefaultHumanoidSlots();
  }

  EquipmentSlots block;
  block.slots = std::move(slots);
  character.set<EquipmentSlots>(block);
}

// TODO: Adding checks to see if we are not repeating values seems like a good
// idea for instance we can't have two npcs with the same name

flecs::entity Game::createNPC(const NPCData &data) {
  const GamePosition &pos = data.position;
  std::string name = data.name;
  const std::string &characterBackground = data.background;

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

  // Stamped before the brain is built, so the agent's prompt and every later
  // resolution see a complete and neutral sheet.
  if (auto registryRes = ecs.get<StatRegistryResource>()) {
    GiveDefaultStats(entity, registryRes->registry);
  }
  if (auto skillRes = ecs.get<SkillRegistryResource>()) {
    GiveStartingSkills(entity, skillRes->registry);
    // Authored proficiency goes on top of the untrained block, before the brain
    // exists, so the first action the agent takes already resolves against it.
    ApplyStartingSkills(entity, skillRes->registry, data.startingSkills,
                        debugLog.get());
  }

  // The body comes before any gear, because equipping needs to know which slots
  // the character actually has.
  GiveSlots(entity, &raceRegistry, data.race, data.slots, debugLog.get());

  // Authored starting equipment. Each id is spawned as a real held item and put
  // on through the same planner [EQUIP] uses, so a too-small body, an unknown
  // template or a missing item definition reports instead of silently doing
  // nothing.
  if (!data.equipped.empty()) {
    auto factoryRes = ecs.get<ObjectFactoryResource>();
    if (factoryRes && factoryRes->factory) {
      for (const std::string &typeId : data.equipped) {
        flecs::entity item =
            factoryRes->factory->SpawnObject(ecs, entity, nullptr, typeId);
        if (!item.is_alive()) {
          continue;
        }
        entity.add<Holds>(item);
        const EquipOutcome outcome = EquipItem(entity, item, &itemRegistry);
        if (!outcome.ok) {
          const std::string message = "Warning: NPC '" + name +
                                      "' could not equip '" + typeId +
                                      "': " + outcome.message;
          if (debugLog) debugLog->LogWarning(message);
          else std::cerr << message;
        }
      }
    }
  }

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

  // Each active command is listed with its own description directly beneath it,
  // in the same description-then-examples style the interaction registry uses,
  // so the model sees what a command does next to the command itself.
  std::string commandsList = "";
  for (const auto &cmd : GlobalCommandRegistry::GetCommands()) {
    if (cmd.hidden || cmd.format.empty()) continue;
    commandsList += cmd.format + "\n";
    if (!cmd.rule.empty()) {
      commandsList += "  - " + cmd.rule + "\n";
    }
  }
  // The template already supplies the blank line that follows the list.
  if (!commandsList.empty() && commandsList.back() == '\n') {
    commandsList.pop_back();
  }

  std::regex re1("%LOCATIONS%");
  std::regex re2("%BACKGROUND%");
  std::regex re3("%CHARACTERS%");
  std::regex re4("%COMMANDS_LIST%");

  std::string startingPrompt =
      std::regex_replace(DEFAULT_NPC_PROMPT, re1, locations);
  startingPrompt = std::regex_replace(startingPrompt, re2, characterBackground);
  startingPrompt = std::regex_replace(startingPrompt, re3, characterNames);
  startingPrompt = std::regex_replace(startingPrompt, re4, commandsList);
  startingPrompt += "\n";
  entity.set<NPCContext>({ {{"system", startingPrompt}}, ""});

  if (name == "") {
    flecs::entity_t id = entity.id();
    name = "NPC " + std::to_string(id);
  }
  entity.set<DisplayName>({name});
  entity.set<NameTagColor>({BLUE});
  entity.set_name(name.c_str());

  auto brain = std::make_unique<AgentBrain>(entity, name, debugLog.get());
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

  // Progress bar for timed actions (e.g. harvesting). The action entity carries
  // the ActionTimer/ActionActor components, so the bar is positioned relative to
  // the acting entity and is empty when the action starts, filling as it runs out.
  ecs.system<ActionTimer, ActionActor>("ActionProgressBarSystem")
      .kind<Render>()
      .each([](const ActionTimer &timer, const ActionActor &action) {
        flecs::entity actor = action.actor;
        if (!actor.is_alive() || !actor.has<ScreenPosition>()) {
          return;
        }

        const ScreenPosition &pos = *actor.get<ScreenPosition>();

        float entityWidth = static_cast<float>(DEFAULT_ENTITY_VISUAL_WIDTH);
        float entityHeight = static_cast<float>(DEFAULT_ENTITY_VISUAL_HEIGHT);
        if (actor.has<DrawAscii>()) {
          entityWidth = static_cast<float>(actor.get<DrawAscii>()->width);
          entityHeight = static_cast<float>(actor.get<DrawAscii>()->height);
        } else if (actor.has<Hitbox>()) {
          entityWidth = static_cast<float>(actor.get<Hitbox>()->width);
          entityHeight = static_cast<float>(actor.get<Hitbox>()->height);
        }

        float barWidth = std::max(entityWidth, DEFAULT_ACTION_BAR_MIN_WIDTH);
        float barX = std::round(pos.x + entityWidth / 2.0f - barWidth / 2.0f);
        float barY = std::round(pos.y + entityHeight + DEFAULT_ACTION_BAR_OFFSET_Y);

        DrawRectangleV({barX, barY}, {barWidth, DEFAULT_ACTION_BAR_HEIGHT},
                       DEFAULT_ACTION_BAR_BACKGROUND);
        DrawRectangleV({barX, barY},
                       {barWidth * timer.Progress(), DEFAULT_ACTION_BAR_HEIGHT},
                       DEFAULT_ACTION_BAR_FILL);
      });

  // Floating change numbers (XP today, damage later). Registered last so they
  // draw above sprites and name tags, and inside the camera pass so they scale
  // and scroll with the world instead of floating over the screen.
  ecs.system<FloatingText>("FloatingTextRenderSystem")
      .kind<Render>()
      .each([this](const FloatingText &floating) {
        // A live target wins; a dead one keeps the number at its last known
        // spot, so the killing blow is still visible.
        Vector2 anchor = floating.anchor;
        bool haveAnchor = floating.anchored;
        float entityWidth = static_cast<float>(DEFAULT_ENTITY_VISUAL_WIDTH);
        if (floating.target.is_alive()) {
          if (const ScreenPosition *pos = floating.target.get<ScreenPosition>()) {
            anchor = {pos->x, pos->y};
            haveAnchor = true;
          }
          if (const DrawAscii *ascii = floating.target.get<DrawAscii>()) {
            entityWidth = static_cast<float>(ascii->width);
          } else if (const Hitbox *hitbox = floating.target.get<Hitbox>()) {
            entityWidth = static_cast<float>(hitbox->width);
          }
        }
        // Nothing to hang the number on: an entity that never had a screen
        // position, and is no longer alive to gain one.
        if (!haveAnchor) {
          return;
        }

        const FloatingTextMotion motion = EvaluateFloatingTextMotion(
            floating.elapsed, floating.lifetime, floating.rise,
            floating.stackIndex, DEFAULT_FLOATING_TEXT_FAN_SPACING,
            DEFAULT_FLOATING_TEXT_FADE_START);

        const Vector2 textSize =
            MeasureTextEx(gameFont, floating.text.c_str(),
                          DEFAULT_FLOATING_TEXT_FONT_SIZE, 0);

        const float centerX = anchor.x + entityWidth / 2.0f + motion.fan;
        const Vector2 textPos = {
            std::round(centerX - textSize.x / 2.0f),
            std::round(anchor.y - DEFAULT_FLOATING_TEXT_OFFSET_Y - textSize.y -
                       motion.rise)};

        Color color = floating.color;
        color.a = static_cast<unsigned char>(std::clamp(
            static_cast<float>(color.a) * motion.alpha, 0.0f, 255.0f));

        // A one-pixel drop shadow keeps a small number legible over a bright
        // tile without needing a full outline pass.
        const Color shadow = {0, 0, 0, color.a};
        DrawTextEx(gameFont, floating.text.c_str(),
                   {textPos.x + 1.0f, textPos.y + 1.0f},
                   DEFAULT_FLOATING_TEXT_FONT_SIZE, 0, shadow);
        DrawTextEx(gameFont, floating.text.c_str(), textPos,
                   DEFAULT_FLOATING_TEXT_FONT_SIZE, 0, color);
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

  // Floating change numbers. Ages them out and keeps their anchor pinned to the
  // target, so a living character drags its number along and a dead one leaves
  // it where it fell. Advance-then-test means a number is always drawn at least
  // once, even at a lifetime shorter than a frame.
  ecs.system<FloatingText>("FloatingTextLifetimeSystem")
      .each([](flecs::entity e, FloatingText &floating) {
        if (floating.target.is_alive()) {
          if (const ScreenPosition *pos = floating.target.get<ScreenPosition>()) {
            floating.anchor = {pos->x, pos->y};
            floating.anchored = true;
          }
        }

        floating.elapsed += GetFrameTime();
        if (floating.elapsed >= floating.lifetime) {
          e.destruct();
        }
      });
}

void Game::LoadMap(std::string mapPath, bool spawnNPCs) {
  hasClicked = false;
  validTileSelected = false;
  mapFilePath = mapPath;

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
    if (spawnNPCs) {
      for (const auto& npc : currentMap->GetNPCs()) {
        flecs::entity npcEntity = createNPC(npc);
        // Starting inventory becomes real Holds children, the same shape
        // crafting and storage use (see ObjectFactory::SpawnInventory).
        auto factoryRes = ecs.get<ObjectFactoryResource>();
        if (factoryRes && factoryRes->factory) {
          factoryRes->factory->SpawnInventory(ecs, npcEntity, npc.inventory);
        }
      }
    }

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
              if (pendingInt->interactionName == DEFAULT_SEE_INVENTORY_OPTION) {
                // Player-only QOL option, not an InteractionRegistry entry, so
                // it is handled here instead of the lookup below. Unlike the
                // [EXAMINE] interaction it only opens the read-only window.
                OpenStorageWindow(pendingInt->targetEntity);
              } else {
                auto interactions = InteractionRegistry::GetAvailableInteractions(pendingInt->targetEntity);
                for (const auto& interaction : interactions) {
                  if (interaction.name == pendingInt->interactionName) {
                    std::string msg = interaction.execute(entity, pendingInt->targetEntity, "");
                    if (debugLog && !msg.empty()) debugLog->Log(msg);
                    break;
                  }
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

namespace {

// Every input entity a craft is about to spend, plus the human-readable shortage
// when the actor cannot pay the full cost. `items` is empty exactly when
// `missing` is non-empty.
struct RecipeInputClaim {
  std::vector<flecs::entity> items;
  std::string missing;
};

// Walks the actor's inventory once and claims up to `count` units of every
// input. Each entity is claimed at most once, so a recipe that lists the same
// item twice cannot spend one unit twice. Used twice for a timed craft: once at
// start to refuse unsatisfiable work before the wait, and once when the timer
// ends to spend exactly what is still there.
//
// `batches` is how many times the recipe runs in one craft (the trailing number
// in [CRAFT bread 3]), so every listed input is multiplied by it: 3 breads at
// 1 flour each claim 3 flour, and the shortage message reports what the whole
// batch is still missing.
RecipeInputClaim ClaimRecipeInputs(flecs::entity actor,
                                   const CraftRecipe &recipe, int batches = 1) {
  if (batches < 1) batches = 1;

  RecipeInputClaim claim;
  for (const ItemStack &input : recipe.inputs) {
    const int needed = input.count * batches;
    std::vector<flecs::entity> found;
    actor.each<Holds>([&](flecs::entity child) {
      if (static_cast<int>(found.size()) >= needed) return;
      if (!child.is_alive()) return;
      if (std::find(claim.items.begin(), claim.items.end(), child) !=
          claim.items.end()) {
        return;
      }
      const ItemType *itemType = child.get<ItemType>();
      if (itemType && StringUtils::EqualsIgnoreCase(itemType->id, input.item)) {
        found.push_back(child);
      }
    });

    if (static_cast<int>(found.size()) < needed) {
      if (!claim.missing.empty()) claim.missing += ", ";
      claim.missing +=
          std::to_string(needed - static_cast<int>(found.size())) + " " +
          input.item;
      // Keep checking the other inputs so the shortage message is complete.
      continue;
    }
    claim.items.insert(claim.items.end(), found.begin(), found.end());
  }
  return claim;
}

// Takes the next engine-owned roll group. Each group is derived from its own
// counter value, so groups cannot share or shift each other's streams: adding a
// new roll site does not change the outcome of any existing one.
//
// Falls back to a fixed seed when the world has not been seeded yet, which keeps
// the result deterministic rather than merely accidental.
Rng NextRollStream(flecs::world world) {
  WorldRandomness *randomness = world.get_mut<WorldRandomness>();
  if (!randomness) {
    return Rng(0);
  }
  return Rng::Derived(randomness->seed, ++randomness->groups, 0);
}

// Spawns and hands over every entry of a loot table that passes its chance roll,
// returning the item type ids that dropped, in table order.
//
// The ids come from the table, NOT from the spawned entities, and that is
// deliberate: all of these call sites run inside a system iteration, which is a
// deferred context. A freshly spawned entity there reports is_alive() == true
// while none of its components are readable until the sync point, so reading
// ItemType or DisplayName off it yields nothing for every drop -- which is
// exactly how harvesting came to report "nothing" while still handing over the
// items. The table's own id string is always available.
//
// This is the ONLY place a loot chance is rolled. The timed and instant harvest
// paths used to carry near-identical copies of this loop, which is exactly how
// an effect applied to one path and not the other would have gone unnoticed.
std::vector<std::string> RollLootTable(flecs::entity actor,
                                       const std::vector<LootEntry> &entries,
                                       ObjectFactory *factory, Rng &rng) {
  std::vector<std::string> dropped;
  if (!factory) {
    return dropped;
  }
  // flecs::world is a handle returned by value, so it cannot bind directly to
  // SpawnObject's flecs::world& parameter.
  flecs::world world = actor.world();
  for (const LootEntry &entry : entries) {
    // One roll per entry, in table order, engine-owned: an effect hook may
    // adjust a chance but must never roll the same entry itself.
    if (!rng.Chance(entry.chance)) {
      continue;
    }
    flecs::entity item =
        factory->SpawnObject(world, actor, nullptr, entry.itemType);
    if (!item.is_alive()) {
      continue;
    }
    actor.add<Holds>(item);
    dropped.push_back(entry.itemType);
  }
  return dropped;
}

// The registries every resolution needs, gathered in one place so a call site
// cannot forget one and silently resolve as if nothing had any effect. Any of
// them may be absent in a half-initialised world; the fallbacks keep the failure
// mode "nothing changes" rather than "nothing works".
struct ResolutionInputs {
  SourceDefinitions definitions;
  const HookRegistry *hooks = nullptr;
};

ResolutionInputs MakeResolutionInputs(flecs::world world) {
  auto statsRes = world.get<StatRegistryResource>();
  auto skillRes = world.get<SkillRegistryResource>();
  auto itemRes = world.get<ItemRegistryResource>();
  auto hooksRes = world.get<HookRegistryResource>();

  static const HookRegistry noHooks;
  ResolutionInputs inputs;
  inputs.definitions = SourceDefinitions(
      statsRes ? statsRes->registry : nullptr,
      skillRes ? skillRes->registry : nullptr,
      itemRes ? itemRes->registry : nullptr);
  inputs.hooks = (hooksRes && hooksRes->hooks) ? hooksRes->hooks : &noHooks;
  return inputs;
}

// Comma-joined list of item type ids. Empty when there are none, so each caller
// can decide how to phrase "nothing" (harvest says "nothing", crafting says
// "nothing came out of it").
std::string JoinItemTypes(const std::vector<std::string> &itemTypes) {
  std::string joined;
  for (const std::string &itemType : itemTypes) {
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += itemType;
  }
  return joined;
}

// What one harvest produced: the items, and any skill progress it earned.
//
// Kept together so a caller can report both in one message, in the right order.
// Granting inside the helper is what keeps the two harvest paths from drifting;
// returning the progress rather than appending it is what lets the caller put the
// skill line AFTER the harvest line instead of before it.
struct HarvestOutcome {
  std::string droppedItems;
  std::vector<SkillGain> training;
  // Tools that reached zero durability on this harvest, by display name. Kept
  // alongside the drops so the caller can report the break in the same message,
  // the way training gains already are.
  std::vector<std::string> brokenTools;
};

// Applies a subject's training grants to the actor, if it teaches anything.
//
// Called from the commit points themselves rather than by their callers, so an
// action that never commits earns nothing: a cancelled craft and the [CRAFT]
// dry run both never reach here.
std::vector<SkillGain> ApplySubjectTraining(flecs::entity actor,
                                            const std::vector<SkillXp> &grants,
                                            const std::string &activity) {
  if (grants.empty()) {
    return {};
  }
  auto skillRes = actor.world().get<SkillRegistryResource>();
  if (!skillRes || !skillRes->registry) {
    return {};
  }
  SkillBlock *block = actor.get_mut<SkillBlock>();
  if (!block) {
    return {};
  }

  // Report the raw XP of every grant this actor will actually receive, not just
  // the ones that cross a level. The whole point of the floating number is to
  // make the small, frequent gains visible, and a grant that only banks progress
  // toward the next level still earned something. GrantApplies is the same rule
  // ApplyTraining uses below, so a number can never appear for XP that was
  // silently dropped (unknown skill, wrong activity, skill not held).
  //
  // `spawned` keeps a single award that trains several skills from collapsing
  // into one number: entities created earlier in this deferred frame are not yet
  // visible to the stacking count, so the caller numbers the batch itself.
  int spawned = 0;
  for (const SkillXp &grant : grants) {
    if (grant.xp <= 0 ||
        !GrantApplies(*block, skillRes->registry->Get(grant.skillId),
                      activity)) {
      continue;
    }
    if (SpawnFloatingXp(actor, grant.xp, spawned).is_alive()) {
      ++spawned;
    }
  }

  return ApplyTraining(*block, grants, *skillRes->registry, activity);
}

// Resolves the loot table for one harvest and rolls it, returning the item type
// ids that dropped. Internal to HarvestResult below.
std::vector<std::string> HarvestDrops(flecs::entity actor, flecs::entity target,
                                      const std::vector<LootDrop> &base) {
  ObjectFactory *factory = nullptr;
  if (auto factoryRes = actor.world().get<ObjectFactoryResource>()) {
    factory = factoryRes->factory;
  }

  std::vector<LootEntry> table = ToLootEntries(base);

  const ResolutionInputs inputs = MakeResolutionInputs(actor.world());

  LootRequest request;
  request.activity = "harvest";
  request.subjectId = target.has<ItemType>() ? target.get<ItemType>()->id : "";
  request.subject = target;

  table = ResolveLoot(actor, request, base, inputs.definitions, *inputs.hooks)
              .entries;

  Rng rng = NextRollStream(actor.world());
  return RollLootTable(actor, table, factory, rng);
}

// Resolves and rolls one harvest, and applies whatever the object teaches.
//
// BOTH harvest commit sites go through here -- the timed resolution and the
// instant fallback. They used to carry separate copies of this sequence, and that
// is precisely how an effect, or a training grant, ends up applying to a timed
// harvest and silently doing nothing on an instant one.
HarvestOutcome HarvestResult(flecs::entity actor, flecs::entity target,
                             const std::vector<LootDrop> &base) {
  HarvestOutcome outcome;
  outcome.droppedItems = JoinItemTypes(HarvestDrops(actor, target, base));
  if (outcome.droppedItems.empty()) {
    outcome.droppedItems = "nothing";
  }

  if (const Trains *trains = target.get<Trains>()) {
    outcome.training = ApplySubjectTraining(actor, trains->grants, "harvest");
  }

  // Wear is charged LAST, so the harvest that breaks a tool still yields: the
  // work was done before the tool gave out. This is the one door both commit
  // paths go through, so a tool cannot wear on a timed harvest and not on an
  // instant one.
  if (const Harvestable *harvestable = target.get<Harvestable>()) {
    outcome.brokenTools =
        WearEquipment(actor, "harvest", harvestable->durabilityCost,
                      ItemsOf(actor.world()))
            .broken;
  }
  return outcome;
}

// "Your Iron Scythe broke and is destroyed." for each tool that gave out.
std::string FormatBrokenTools(const std::vector<std::string> &broken) {
  std::string message;
  for (const std::string &name : broken) {
    message += "Your " + name + " broke and is destroyed.\n";
  }
  return message;
}

// What one execution of a recipe produced, plus any skill progress it earned.
struct RecipeOutcome {
  std::string produced; // empty when every chance failed
  std::vector<SkillGain> training;
};

// Rolls a recipe's outputs once and hands each success to the actor. Shared by
// the instant path in the Craft interaction and by CraftResolutionSystem below,
// so the two cannot drift. Returns the list actually produced ("Flour, Flour"),
// or empty when every chance failed.
//
// One call is one run of the recipe. A batch craft calls this `batches` times
// rather than multiplying the outputs in one pass, because each output carries
// its own chance and three independent rolls are not the same as one roll times
// three. The caller passes one stream for the whole batch, so the runs draw
// successive values from it.
//
// Training is earned here, before the rolls, and deliberately does not depend on
// anything dropping: the materials are already spent and the work was still done,
// so a recipe whose outputs all failed to appear still teaches what it teaches.
// Called once per execution, which is what makes `[CRAFT bread 3]` worth three
// times the XP.
RecipeOutcome ProduceRecipeOutputs(flecs::entity actor, const CraftRecipe &recipe,
                                   ObjectFactory *factory, Rng &rng) {
  RecipeOutcome outcome;
  outcome.produced = JoinItemTypes(
      RollLootTable(actor, ToLootEntries(recipe.outputs), factory, rng));
  outcome.training = ApplySubjectTraining(actor, recipe.trains, "craft");
  return outcome;
}

// Name for a batch in a message: "3x Bread" for a batch, plain "Bread" for one,
// so a single-item craft reads exactly as it did before batching existed.
std::string FormatBatchedLabel(int batches, const std::string &label) {
  if (batches > 1) {
    return std::to_string(batches) + "x " + label;
  }
  return label;
}

} // namespace

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
          if (token.empty()) {
            newRequest->finished = true;
          } else {
            newRequest->pendingResponse += token;
          }

          // Record the reply through the brain so it lands in NPCContext AND the
          // per-NPC transcript. Writing history directly here used to bypass the
          // transcript, so the log was missing every AI reply.
          AgentBrainWrapper *wrapper = entity.get_mut<AgentBrainWrapper>();
          if (wrapper && wrapper->agBrain) {
            wrapper->agBrain->recordAssistantStream(token);
          }
        }
      });
    };

    if (!request.prompt.empty()) {
      AgentBrainWrapper *wrapper = entity.get_mut<AgentBrainWrapper>();
      if (wrapper && wrapper->agBrain) {
        wrapper->agBrain->pushContext("user", request.prompt);
      }
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
      if (!evolvables[i].isActive) continue;

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

  ecs.system<ActionTimer>("ActionTimerSystem").iter([](flecs::iter& it, ActionTimer* timers) {
      float dt = it.delta_time();
      for (auto i : it) {
          timers[i].timeRemaining -= dt;
          if (timers[i].timeRemaining <= 0.0f) {
              it.entity(i).add<ActionCompleted>();
              it.entity(i).remove<ActionTimer>();
          }
      }
  });

  ecs.system<ActionActor, ActionTarget>("HarvestResolutionSystem")
      .with<HarvestAction>()
      .with<ActionCompleted>()
      .each([this](flecs::entity actionEntity, ActionActor& a, ActionTarget& t) {
          flecs::entity actor = a.actor;
          flecs::entity target = t.target;

          if (actor.is_alive() && target.is_alive()) {
              Harvestable* h = target.get_mut<Harvestable>();
              if (h) {
                  h->amountRemaining--;

                  // Resolve, roll and train, through the same helper the instant
                  // path uses.
                  const HarvestOutcome outcome =
                      HarvestResult(actor, target, h->lootTable.drops);

                  std::string objName = target.has<DisplayName>() ? target.get<DisplayName>()->name : "Object";
                  std::string msg = "System: You harvested " + outcome.droppedItems + " from " + objName + ".\n";
                  msg += FormatTrainingGains(outcome.training);
                  msg += FormatBrokenTools(outcome.brokenTools);

                  if (h->amountRemaining <= 0) {
                      msg += "The " + objName + " is depleted and destroyed.\n";
                      actor.world().defer([target]() { target.destruct(); });
                  }

                  if (actor.has<AgentBrainWrapper>()) {
                      actor.get_mut<AgentBrainWrapper>()->agBrain->appendContext("user", msg);
                  }
                  if (!actor.has<AgentBrainWrapper>() && this->debugLog) {
                      this->debugLog->Log(msg);
                  }
              }
          }

          if (actor.is_alive()) {
              actor.remove<Busy>();
          }
          actionEntity.destruct();
      });

  // Completes a timed craft started by the Craft interaction. The ingredients
  // are spent here, immediately before the outputs are created, so a craft that
  // cannot resolve never destroys them. Mirrors HarvestResolutionSystem,
  // including the contract that the interaction itself returned an empty success
  // message: the model is woken exactly once, here, with the result.
  ecs.system<ActionActor, CraftAction>("CraftResolutionSystem")
      .with<ActionCompleted>()
      .each([this](flecs::entity actionEntity, ActionActor& a, CraftAction& craft) {
          flecs::entity actor = a.actor;

          if (actor.is_alive()) {
              auto regRes = actor.world().get<RecipeRegistryResource>();
              const RecipeRegistry* registry = regRes ? regRes->registry : nullptr;
              // The recipe is looked up by id rather than captured as a pointer,
              // so a registry reload between start and finish cannot dangle.
              const CraftRecipe* recipe =
                  registry ? registry->Get(craft.recipeId) : nullptr;
              auto factoryRes = actor.world().get<ObjectFactoryResource>();
              ObjectFactory* factory = factoryRes ? factoryRes->factory : nullptr;

              if (!recipe || !factory) {
                  // Engine-integrity failure, not a gameplay outcome: content or
                  // the world was torn down under a running craft. It belongs in
                  // the debug log, not in the NPC's conversation -- and since
                  // nothing has been consumed at this point, the actor's
                  // materials survive.
                  if (this->debugLog) {
                      std::string actorName = actor.has<DisplayName>()
                                                  ? actor.get<DisplayName>()->name
                                                  : "an unnamed actor";
                      this->debugLog->LogError(
                          "CraftResolutionSystem: craft of '" + craft.recipeId +
                          "' by " + actorName + " produced nothing (" +
                          (!recipe ? "recipe no longer exists"
                                   : "no object factory is loaded") +
                          ").");
                  }
              } else {
                  // A batch craft is `count` copies of the same recipe executed
                  // under one timer. The count is clamped here as well as at the
                  // interaction, because it decides how much the loop below
                  // spends: a zero or negative value from anywhere would
                  // otherwise silently produce nothing.
                  const int batches = craft.count > 0 ? craft.count : 1;

                  // Claim again instead of trusting the interaction's dry run:
                  // this is the copy that gets spent, so it is the only one that
                  // has to still be valid when the timer ends. The claim covers
                  // the whole batch, so a batch that can no longer be paid for in
                  // full is canceled rather than partially executed.
                  RecipeInputClaim claim = ClaimRecipeInputs(actor, *recipe, batches);
                  std::string msg;
                  if (!claim.missing.empty()) {
                      // A real gameplay outcome: the actor spent the materials on
                      // something else during the wait. Nothing is destroyed and
                      // it is told why the craft came to nothing.
                      msg = "System: You no longer have the materials to craft " +
                            FormatBatchedLabel(batches, recipe->id) +
                            ". Still needed: " + claim.missing +
                            ". The craft was canceled.\n";
                  } else {
                      // Spend the materials only now, on the path that is about to
                      // produce something.
                      for (flecs::entity item : claim.items) {
                          actor.remove<Holds>(item);
                          item.destruct();
                      }

                      // One roll per batch item, not one roll scaled by the
                      // batch: probability lives on each output. One stream is
                      // shared across the batch so each run draws the next value.
                      Rng rng = NextRollStream(actor.world());
                      std::string produced;
                      std::vector<SkillGain> training;
                      for (int i = 0; i < batches; ++i) {
                          RecipeOutcome run = ProduceRecipeOutputs(actor, *recipe, factory, rng);
                          if (!run.produced.empty()) {
                              if (!produced.empty()) produced += ", ";
                              produced += run.produced;
                          }
                          for (const SkillGain &gain : run.training) {
                              training.push_back(gain);
                          }
                      }
                      if (produced.empty()) {
                          msg = "System: You crafted " +
                                FormatBatchedLabel(batches, recipe->id) +
                                " but nothing came out of it.\n";
                      } else {
                          msg = "System: You finished crafting " + produced +
                                " and put it in your inventory.\n";
                      }
                      msg += FormatTrainingGains(training);
                  }

                  if (actor.has<AgentBrainWrapper>()) {
                      actor.get_mut<AgentBrainWrapper>()->agBrain->appendContext("user", msg);
                  }
                  if (!actor.has<AgentBrainWrapper>() && this->debugLog) {
                      this->debugLog->Log(msg);
                  }
              }
          }

          if (actor.is_alive()) {
              actor.remove<Busy>();
          }
          actionEntity.destruct();
      });
};

namespace {

std::string TrimCopy(const std::string &text) {
  size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

// Durations are written into messages a player and a model read, where
// "3.000000 seconds" is noise. Whole seconds print bare, anything else keeps one
// decimal: 3 -> "3", 2.5 -> "2.5".
std::string FormatSeconds(float seconds) {
  std::string text = std::to_string(seconds);
  text.erase(text.find_last_not_of('0') + 1);
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  return text;
}

// Named C++ effects (docs/stat-effects-design.md section 4.4).
//
// Data references these by bare string in a stat's "effects" array, e.g.
// `"effects": ["dexterous_grip"]`, and every stat carrying that id activates
// them. This is the escape hatch for effects a declarative link cannot express:
// conditional bonuses, gates, anything that has to read the world.
//
// Deliberately empty for now, and that is a real state rather than an oversight:
// every effect the game currently ships is a declarative link in data/stats.
// This is the place to add the ones that are not --
//
//   hookRegistry.OnDuration("dexterous_grip",
//       [](DurationContext &ctx) {
//         if (ctx.subjectId == "wheat_mature") ctx.out.Pct(-0.05f, "dexterous_grip");
//       });
//
// Loot is structural rather than numeric, so its hooks edit the table:
//
//   hookRegistry.OnLoot("seed_finder",
//       [](LootContext &ctx) {
//         if (ctx.subjectId == "wheat_mature")
//           ctx.out.Append("wheat_seed", 0.25f, ctx.sourceId);
//       });
//
// Note there is no RNG in either context yet: a proc needs a stream identity, and
// duration is resolved before the action entity exists. See the open question in
// section 5.3 before adding a probabilistic hook here.
void RegisterGameHooks(HookRegistry &hookRegistry) { (void)hookRegistry; }

// Reports every training grant that can never be applied, once, at startup.
//
// A subject may name any skill, so without this a typo ("blacksmith" for
// "blacksmithing") or a wrong activity -- a recipe claiming to train a
// harvest-only skill -- would be a silent no-op on every single craft. That is
// the same failure mode the named-hook check exists to prevent.
//
// This is the job the skill's `trainedBy` list does: a guard, not a selector. It
// never decides who trains what, only whether the subject's claim is one the
// skill accepts.
void ValidateTrainingGrants(
    const ObjectFactory &factory, const RecipeRegistry &recipes,
    const SkillRegistry &skills,
    const std::function<void(const std::string &)> &warn) {
  auto checkGrant = [&](const std::string &owner, const std::string &activity,
                        const std::string &skillId) {
    const SkillDef *def = skills.Get(skillId);
    if (!def) {
      warn("Warning: " + owner + " trains unknown skill '" + skillId +
           "', so the grant does nothing.");
      return;
    }
    if (!AcceptsActivity(*def, activity)) {
      warn("Warning: " + owner + " trains '" + skillId + "', which is not " +
           "trained by " + activity + ". Add \"" + activity +
           "\" to its trainedBy list if that is intended; otherwise the grant "
           "does nothing.");
    }
  };

  // Object templates are not entities until they are spawned, and an object that
  // teaches nothing need not be spawned at all, so they are checked from the raw
  // JSON the factory holds rather than from the world.
  for (const auto &pair : factory.GetTemplates()) {
    if (!pair.second.contains("trains")) {
      continue;
    }
    const nlohmann::json &trains = pair.second["trains"];
    if (!trains.is_object()) {
      continue; // malformed; reported when the object is spawned
    }
    for (auto it = trains.begin(); it != trains.end(); ++it) {
      checkGrant("Object template '" + pair.first + "'", "harvest", it.key());
    }
  }

  for (const std::string &id : recipes.GetIds()) {
    const CraftRecipe *recipe = recipes.Get(id);
    if (!recipe) {
      continue;
    }
    for (const SkillXp &grant : recipe->trains) {
      checkGrant("Recipe '" + id + "'", "craft", grant.skillId);
    }
  }
}

// Reports every item whose required slot kind no body can accept.
//
// Slot kinds are an open vocabulary, so nothing stops an author writing
// "main_hand" where the vocabulary says "hand". The item would then be
// permanently unequippable, and -- because a body simply has no such slot -- the
// failure would look like the item doing nothing. This turns that typo into a
// startup report, the same job ValidateTrainingGrants does for skill grants.
//
// The bodies considered are every race plus the default humanoid set, which is
// what a character with neither a race nor authored slots gets.
void ValidateItemSlots(const ItemRegistry &items, const RaceRegistry &races,
                       const std::function<void(const std::string &)> &warn) {
  std::set<std::string> acceptedKinds;
  for (const SlotSpec &slot : DefaultHumanoidSlots()) {
    acceptedKinds.insert(slot.accepts);
  }
  for (const RaceDef *race : races.GetAll()) {
    for (const SlotSpec &slot : race->slots) {
      acceptedKinds.insert(slot.accepts);
    }
  }

  for (const ItemDef *item : items.GetAll()) {
    if (item->slot.empty()) {
      continue; // already reported by the loader
    }
    if (acceptedKinds.find(item->slot) == acceptedKinds.end()) {
      warn("Warning: Item '" + item->id + "' needs a '" + item->slot +
           "' slot, which no race or the default humanoid body provides, so it "
           "can never be equipped.");
    }
  }
}

// Reports every equipment requirement no item can satisfy.
//
// Requirements name item tags, so a typo ("syth" for "scythe") would make an
// object permanently unharvestable in a way that looks exactly like the verb
// being broken. Checking the shipped tags once at startup turns that into a
// report, the same job ValidateTrainingGrants and ValidateItemSlots do.
void ValidateObjectRequirements(
    const ObjectFactory &factory, const ItemRegistry &items,
    const std::function<void(const std::string &)> &warn) {
  std::set<std::string> providedTags;
  for (const ItemDef *item : items.GetAll()) {
    for (const std::string &tag : item->tags) {
      providedTags.insert(tag);
    }
  }

  for (const auto &pair : factory.GetTemplates()) {
    if (!pair.second.contains("requires")) {
      continue;
    }
    const nlohmann::json &requiresJson = pair.second["requires"];
    if (!requiresJson.is_object() || !requiresJson.contains("equipped")) {
      continue; // malformed; reported when the object is spawned
    }
    const nlohmann::json &equippedJson = requiresJson["equipped"];

    auto checkTag = [&](const std::string &tag) {
      if (providedTags.find(tag) == providedTags.end()) {
        warn("Warning: Object template '" + pair.first + "' requires a '" + tag +
             "' to be equipped, but no item carries that tag, so the "
             "requirement can never be met.");
      }
    };

    if (equippedJson.is_string()) {
      checkTag(equippedJson.get<std::string>());
    } else if (equippedJson.is_array()) {
      for (const auto &tagJson : equippedJson) {
        if (tagJson.is_string()) {
          checkTag(tagJson.get<std::string>());
        }
      }
    }
  }
}

// One line per recipe, written with content ids rather than display names,
// because the id is the exact token [CRAFT ...] and the recipe files use.
std::string FormatRecipe(const CraftRecipe &recipe) {
  std::string line = recipe.id + " (";
  for (size_t i = 0; i < recipe.inputs.size(); ++i) {
    if (i > 0) line += " + ";
    line += std::to_string(recipe.inputs[i].count) + " " + recipe.inputs[i].item;
  }
  line += " -> ";
  for (size_t i = 0; i < recipe.outputs.size(); ++i) {
    if (i > 0) line += " + ";
    const LootDrop &output = recipe.outputs[i];
    line += output.itemType;
    if (output.chance < 1.0f) {
      line += " (" + std::to_string(std::lround(output.chance * 100.0f)) +
              "% chance)";
    }
  }
  line += ")";
  // Surface the wait, because it differs per recipe and the model needs it to
  // plan around being busy.
  if (recipe.craftTimeSeconds > 0.0f) {
    line += " takes " + FormatSeconds(recipe.craftTimeSeconds) + "s";
  }
  return line;
}

// Bullet list of everything a station advertises. Recipe ids missing from the
// registry are reported instead of skipped: a silent gap looks exactly like
// "the station cannot do that" and hides a content bug.
std::string FormatStationRecipes(const Workstation &station,
                                 const RecipeRegistry *registry) {
  std::string msg;
  for (const std::string &id : station.recipes) {
    const CraftRecipe *recipe = registry ? registry->Get(id) : nullptr;
    if (recipe) {
      msg += "- " + FormatRecipe(*recipe) + "\n";
    } else {
      msg += "- " + id + " (unknown: no data/recipes/" + id + ".json)\n";
    }
  }
  return msg;
}

// Resolves what the station advertises against the registry, case-insensitively
// so a model that emits "Flour" still resolves. Returns nullptr when the station
// does not offer the recipe (which is different from the recipe not existing).
const CraftRecipe *ResolveStationRecipe(const RecipeRegistry *registry,
                                        const Workstation &station,
                                        const std::string &requestedId) {
  if (!registry || requestedId.empty()) {
    return nullptr;
  }
  for (const std::string &id : station.recipes) {
    if (!StringUtils::EqualsIgnoreCase(id, requestedId)) {
      continue;
    }
    return registry->Get(id);
  }
  return nullptr;
}

} // namespace

void Game::ECSInit(std::string mapPath) {
  ecs.import <flecs::monitor>();
  ecs.set<flecs::Rest>({});

  objectFactory.SetDebugLog(debugLog.get());
  objectFactory.LoadTemplates("data/objects");
  ecs.set<ObjectFactoryResource>({&objectFactory});

  recipeRegistry.SetDebugLog(debugLog.get());
  recipeRegistry.LoadRecipes("data/recipes");
  ecs.set<RecipeRegistryResource>({&recipeRegistry});

  statRegistry.SetDebugLog(debugLog.get());
  statRegistry.LoadStats("data/stats");
  ecs.set<StatRegistryResource>({&statRegistry});

  skillRegistry.SetDebugLog(debugLog.get());
  skillRegistry.LoadSkills("data/skills");
  ecs.set<SkillRegistryResource>({&skillRegistry});

  itemRegistry.SetDebugLog(debugLog.get());
  itemRegistry.LoadItems("data/items");
  ecs.set<ItemRegistryResource>({&itemRegistry});

  // The factory seeds per-instance durability from the item definitions, so it
  // needs the registry. Handed over here rather than pulled in at spawn time so
  // the factory keeps no dependency on the item system.
  objectFactory.SetItemRegistry(&itemRegistry);

  raceRegistry.SetDebugLog(debugLog.get());
  raceRegistry.LoadRaces("data/races");
  ecs.set<RaceRegistryResource>({&raceRegistry});

  // Named C++ effects, then a check that every hook id the data references
  // actually has a handler. An unregistered id would otherwise be a silent
  // no-op -- the source would simply look like it does nothing -- which is the
  // exact failure mode this report exists to prevent. Stats, skills and items
  // are checked together, because any of them can declare a hook.
  RegisterGameHooks(hookRegistry);
  ecs.set<HookRegistryResource>({&hookRegistry});
  std::vector<std::string> declaredHooks = statRegistry.GetAllHookIds();
  for (const std::string &id : skillRegistry.GetAllHookIds()) {
    declaredHooks.push_back(id);
  }
  for (const std::string &id : itemRegistry.GetAllHookIds()) {
    declaredHooks.push_back(id);
  }
  std::sort(declaredHooks.begin(), declaredHooks.end());
  declaredHooks.erase(std::unique(declaredHooks.begin(), declaredHooks.end()),
                      declaredHooks.end());
  for (const std::string &hookId : declaredHooks) {
    if (!hookRegistry.HasAnyHook(hookId)) {
      const std::string msg =
          "Warning: stat, skill or item data references named effect '" +
          hookId +
          "' but no hook is registered for it on any point, so it does nothing.";
      if (debugLog) debugLog->LogWarning(msg);
      else std::cerr << msg << std::endl;
    }
  }

  // A skill's trainedBy list only guards; this is where a subject that names the
  // wrong skill, or the right skill for the wrong activity, is reported.
  ValidateTrainingGrants(objectFactory, recipeRegistry, skillRegistry,
                         [this](const std::string &message) {
                           if (debugLog) debugLog->LogWarning(message);
                           else std::cerr << message << std::endl;
                         });

  // An item that needs a slot kind no body provides can never be equipped. That
  // is a content typo -- "main_hand" as a kind rather than "hand" -- and is far
  // cheaper to report once here than to debug from a command that silently
  // refuses.
  ValidateItemSlots(itemRegistry, raceRegistry,
                    [this](const std::string &message) {
                      if (debugLog) debugLog->LogWarning(message);
                      else std::cerr << message << std::endl;
                    });

  // The subject-side counterpart: a requirement naming a tag no item carries can
  // never be met, which would look like the verb itself being broken.
  ValidateObjectRequirements(objectFactory, itemRegistry,
                             [this](const std::string &message) {
                               if (debugLog) debugLog->LogWarning(message);
                               else std::cerr << message << std::endl;
                             });

  GlobalCommandRegistry::Clear();

  GlobalCommandRegistry::Register({
    "[WAIT $SECONDS]",
    "Wait before doing anything else. Examples: [WAIT 5], [WAIT 20].",
    std::regex(R"(\[WAIT\s+(\d+(?:\.\d+)?)\s*\])", std::regex_constants::icase),
    [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      float time = std::stof(match[1].str());
      return [time](flecs::entity) { return std::make_unique<WaitAction>(time); };
    }
  });

  GlobalCommandRegistry::Register({
    "[MOVE_TO $TARGET]",
    "Pathfind automatically to a character, location or an object. When you arrive at an object you are shown the interactions it offers. Examples: [MOVE_TO Pietro], [MOVE_TO Mill].",
    std::regex(R"(\[MOVE_TO\s+(.+?)\s*\])", std::regex_constants::icase),
    [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string targetName = match[1].str();
      return [targetName](flecs::entity) { return std::make_unique<MoveToEntityAction>(targetName); };
    }
  });

  GlobalCommandRegistry::Register({
    "[TALK_TO $TARGET]",
    "Start or continue a conversation with a character. Example: [TALK_TO Pietro].",
    std::regex(R"(\[TALK_TO\s+(.+?)\s*\])", std::regex_constants::icase),
    [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string targetName = match[1].str();
      return [targetName](flecs::entity entity) { return std::make_unique<TalkAction>(entity, targetName); };
    }
  });

  GlobalCommandRegistry::Register({
    "[CHARACTERS]",
    "List the characters nearby. Example: [CHARACTERS].",
    std::regex(R"(\[CHARACTERS\])", std::regex_constants::icase),
    [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<CharactersAction>(); };
    }
  });

  GlobalCommandRegistry::Register({
    "[LOCATIONS]",
    "List all known mapped locations. Example: [LOCATIONS].",
    std::regex(R"(\[LOCATIONS\])", std::regex_constants::icase),
    [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<LocationsAction>(); };
    }
  });

  GlobalCommandRegistry::Register({
    "[SURROUNDINGS]",
    "List what is close enough to interact with: nearby objects, items on the ground and characters, each with the exact name to use with [MOVE_TO] and its offset from you. Example: [SURROUNDINGS].",
    std::regex(R"(\[SURROUNDINGS\])", std::regex_constants::icase),
    [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<SurroundingsAction>(); };
    }
  });

  GlobalCommandRegistry::Register({
    "[INVENTORY]",
    "List the items you are currently carrying. Example: [INVENTORY].",
    std::regex(R"(\[INVENTORY\])", std::regex_constants::icase),
    [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<InventoryAction>(); };
    }
  });

  GlobalCommandRegistry::Register({
    "[EXAMINE_ITEM $ITEM_NAME]",
    "See what actions you can perform with an item you are carrying. Example: [EXAMINE_ITEM Wheat].",
    std::regex(R"(\[EXAMINE_ITEM\s+(.+?)\s*\])", std::regex_constants::icase),
    [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string itemName = match[1].str();
      return [itemName](flecs::entity) { return std::make_unique<ExamineItemAction>(itemName); };
    }
  });

  GlobalCommandRegistry::Register({
    "[EQUIP $ITEM_NAME]",
    "Wear or wield an item you are carrying, so its effects apply to you. Replaces whatever is already in the slot it needs. Example: [EQUIP Iron Scythe].",
    std::regex(R"(\[EQUIP\s+(.+?)\s*\])", std::regex_constants::icase),
    [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string itemName = match[1].str();
      return [itemName](flecs::entity) { return std::make_unique<EquipAction>(itemName); };
    }
  });

  GlobalCommandRegistry::Register({
    "[UNEQUIP $ITEM_NAME]",
    "Take off an item you are wearing. Example: [UNEQUIP Iron Scythe].",
    std::regex(R"(\[UNEQUIP\s+(.+?)\s*\])", std::regex_constants::icase),
    [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string itemName = match[1].str();
      return [itemName](flecs::entity) { return std::make_unique<UnequipAction>(itemName); };
    }
  });

  GlobalCommandRegistry::Register({
    "[EQUIPMENT]",
    "List what you are wearing and which slot each item occupies. Example: [EQUIPMENT].",
    std::regex(R"(\[EQUIPMENT\])", std::regex_constants::icase),
    [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<EquipmentAction>(); };
    }
  });

  GlobalCommandRegistry::Register({
    "[PLACE $ITEM AT X,Y ...]",
    "- Sets an item you are carrying down in the world at one or more relative coordinates. What can be placed and where is up to the item.",
    std::regex(R"(\[PLACE\s+(.+?)\s+AT\s+((?:-?\d+,-?\d+\s*)+)\])", std::regex_constants::icase),
    [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string itemName = match[1].str();
      std::string coordsStr = match[2].str();
      return [itemName, coordsStr](flecs::entity) { return std::make_unique<PlaceAtAction>(itemName, coordsStr); };
    },
    true
  });

  // Trade verbs. Registered hidden so they are not dumped into every NPC's
  // spawn prompt; the protocol is taught in the conversation-start message and
  // each offer carries its own reply options. They sit before
  // [GENERIC_INTERACT] so an [OFFER ...] is never swallowed by the catch-all,
  // and they exist mainly to give a useful reply when a model uses one outside
  // a conversation.
  GlobalCommandRegistry::Register({
    "[OFFER $GIVE FOR $RECEIVE]",
    "- Propose a trade while talking to someone; separate multiple items with commas. Examples: [OFFER 3 Iron Ingot FOR 8 Flour], [OFFER 2 Flour, 5 Bronze Coins FOR 3 Iron Ingot].",
    std::regex(R"(\[OFFER\b[^\]]*\])", std::regex_constants::icase),
    [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<TradeOutOfContextAction>("OFFER"); };
    },
    true
  });

  GlobalCommandRegistry::Register({
    "[COUNTER_OFFER $GIVE FOR $RECEIVE]",
    "- Propose different terms while your partner's offer is pending; separate multiple items with commas. Examples: [COUNTER_OFFER 2 Iron Ingot FOR 10 Flour], [COUNTER_OFFER 2 Flour, 5 Bronze Coins FOR 3 Iron Ingot].",
    std::regex(R"(\[COUNTER_OFFER\b[^\]]*\])", std::regex_constants::icase),
    [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<TradeOutOfContextAction>("COUNTER_OFFER"); };
    },
    true
  });

  GlobalCommandRegistry::Register({
    "[ACCEPT]",
    "- Agree to the trade offer currently on the table.",
    std::regex(R"(\[ACCEPT\])", std::regex_constants::icase),
    [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<TradeOutOfContextAction>("ACCEPT"); };
    },
    true
  });

  GlobalCommandRegistry::Register({
    "[DECLINE]",
    "- Refuse the trade offer currently on the table.",
    std::regex(R"(\[DECLINE\])", std::regex_constants::icase),
    [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<TradeOutOfContextAction>("DECLINE"); };
    },
    true
  });

  GlobalCommandRegistry::Register({
    "[GENERIC_INTERACT]",
    "",
    std::regex(R"(\[([A-Z_]+)(?:\s+(.+?))?\s*\])", std::regex_constants::icase),
    [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string actionName = match[1].str();
      std::string args = match.size() > 2 ? match[2].str() : "";
      return [actionName, args](flecs::entity) { return std::make_unique<GenericInteractAction>(actionName, args); };
    },
    true
  });

  // Component reflection has to exist before anything is spawned. LoadMap
  // creates the NPCs, and a component added to an entity before its registration
  // is auto-registered without metadata that a later registration cannot repair.
  // StatBlock is stamped on every character at spawn, so this can no longer wait
  // until after the map is loaded.
  RegisterComponents(ecs);

  // Seed every gameplay roll from one place, and log it. The seed is what makes
  // a run reproducible, so a surprising loot outcome can be replayed rather than
  // argued about.
  {
    const uint64_t seed = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    ecs.set<WorldRandomness>({seed, 0});
    if (debugLog) {
      debugLog->Log("World RNG seed: " + std::to_string(seed));
    }
  }

  LoadMap(mapPath, true);

  InteractionRegistry::Clear();
  
  ItemInteractionRegistry::Clear();

  // An item is equippable exactly when a definition claims it, so this is
  // registered by predicate rather than by component: the same object without a
  // data/items/<id>.json is just a carried thing.
  ItemInteractionRegistry::Register({
      "Equip",
      [](flecs::entity item) {
        auto res = item.world().get<ItemRegistryResource>();
        if (!res || !res->registry) return false;
        const ItemType* type = item.get<ItemType>();
        return type != nullptr && res->registry->Get(type->id) != nullptr;
      },
      [](flecs::entity, flecs::entity item) -> std::string {
        const std::string name =
            item.has<DisplayName>() ? item.get<DisplayName>()->name : "that item";
        return "System: To wear " + name + ", use the command [EQUIP " + name +
               "].\n";
      },
      "Wear or wield this item so its effects apply to you while it is on. Example: [EQUIP Iron Scythe]",
  });

  ItemInteractionRegistry::RegisterComponentInteraction<Placeable>(
      "Place",
      "Ask how to set this item down in the world; it replies with the exact [PLACE ...] command and the relative coordinates to use. Example: [PLACE Wheat Seeds AT 1,0]",
      [](flecs::entity actor, flecs::entity item) -> std::string {
      std::string itemName = item.has<DisplayName>() ? item.get<DisplayName>()->name : "Item";
      std::string msg = GetSurroundingsRadar(actor, 2);
      msg += "\nSystem: To place " + itemName + ", use the command [PLACE " + itemName + " AT X,Y ...]\n";
      msg += "where X and Y are relative coordinates from your position (e.g. -1,0 for West, 1,1 for South-East). You can give several coordinates to place more than one at a time.\n";

      // The rules come from the template, so the hint repeats them rather than
      // assuming the agent can infer them from the grid.
      const Placeable* policy = item.get<Placeable>();
      if (policy && policy->maxDistance > 0) {
        msg += "It has to go within " + std::to_string(policy->maxDistance) +
               (policy->maxDistance == 1 ? " tile of you.\n" : " tiles of you.\n");
      }
      if (policy && !policy->surface.empty()) {
        msg += "It can only be placed on: ";
        for (size_t i = 0; i < policy->surface.size(); ++i) {
          if (i > 0) msg += (i + 1 == policy->surface.size()) ? " or " : ", ";
          msg += policy->surface[i];
        }
        msg += ".\n";
      }
      return msg;
  });

  InteractionRegistry::RegisterComponentInteraction<Harvestable>(
      "Harvest",
      "Gather the resources this object holds. Harvesting can take a few seconds, and it yields nothing once the object is depleted. Example: [HARVEST]",
      [](flecs::entity actor, flecs::entity target, std::string args) -> std::string {
      std::string objName = target.has<DisplayName>() ? target.get<DisplayName>()->name : "Object";

      // A declared capability gate is checked before anything else, so an
      // unqualified actor is told what it is missing instead of watching a timer
      // and ending with nothing. Requirements are content (an object's "requires"
      // block plus item tags), so nothing here knows what a scythe is.
      if (const Requires* required = target.get<Requires>()) {
        const std::string missing = FirstMissingEquippedTag(
            actor, required->equippedTags, ItemsOf(actor.world()));
        if (!missing.empty()) {
          return "System: You cannot harvest the " + objName + ": you need a " +
                 missing + " equipped.\n";
        }
      }

      Harvestable* h = target.get_mut<Harvestable>();
      if (h->lootTable.drops.empty()) {
          return "System: The " + objName + " cannot be harvested because it has no loot table.\n";
      }

      if (h->amountRemaining > 0) {
        // Resolve the wait once, here, and use that single number for the
        // branch, the timer and the message. Resolving in more than one place is
        // exactly how the advertised time and the real one drift apart.
        float seconds = h->timer;
        std::string breakdown;

        const ResolutionInputs inputs = MakeResolutionInputs(actor.world());

        DurationRequest request;
        request.activity = "harvest";
        request.subjectId =
            target.has<ItemType>() ? target.get<ItemType>()->id : "";
        request.subject = target;
        request.baseSeconds = h->timer;

        const DurationResolution resolved =
            ResolveDuration(actor, request, inputs.definitions, *inputs.hooks);

        // A gate refuses the action outright rather than adjusting it.
        if (resolved.fold.blocked) {
          return "System: You cannot harvest the " + objName + ": " +
                 resolved.fold.blockReason + "\n";
        }

        seconds = resolved.fold.value;
        breakdown = FormatDurationBreakdown(resolved.fold, h->timer);

        // A resolved wait of zero means the work is instant: either it was
        // authored that way, or bonuses brought it down to the floor.
        if (seconds > 0.0f) {
            if (!actor.has<Busy>()) {
                flecs::entity actionEntity = actor.world().entity()
                    .set<ActionActor>({actor})
                    .set<ActionTarget>({target})
                    .set<ActionTimer>({seconds})
                    .add<HarvestAction>();
                
                actor.set<Busy>({actionEntity});

                std::string msg = "System: Started harvesting " + objName +
                                  ". It will take " + FormatSeconds(seconds) +
                                  " seconds." + breakdown + "\n";
                if (actor.has<AgentBrainWrapper>()) {
                    // Log it but do NOT append to the agent's context: the harvest
                    // completes on its own and HarvestResolutionSystem reports the
                    // outcome, so waking the model here only costs a request and
                    // lets the agent act while it is meant to be busy.
                    actor.get_mut<AgentBrainWrapper>()->agBrain->logNote(msg);
                    return ""; // Return empty so AI Action queue doesn't hold it
                } else {
                    return msg; // Return msg so player queue logs it immediately
                }
            } else {
                return "System: You are already busy doing something else.\n";
            }
        }

        // Instant harvest fallback: the resolved wait came out at zero.
        h->amountRemaining--;

        // The same shared helper the timed path uses: the two must resolve, roll
        // and train identically or something would silently apply to one and not
        // the other.
        const HarvestOutcome outcome =
            HarvestResult(actor, target, h->lootTable.drops);

        std::string msg = "System: You harvested " + outcome.droppedItems + " from " + objName + ".\n";
        msg += FormatTrainingGains(outcome.training);
        msg += FormatBrokenTools(outcome.brokenTools);
        if (h->amountRemaining <= 0) {
          msg += "The " + objName + " is depleted and destroyed.\n";
          target.destruct();
        }
        return msg;
      }
      return "System: The " + objName + " has no more resources to harvest.\n";
  });

  InteractionRegistry::RegisterComponentInteraction<Workstation>(
      "Craft",
      "Turn materials you are carrying into a finished good at this station. Crafting takes time and the wait depends on the recipe, during which you are busy and cannot start anything else; you are told the result when it finishes. Name the recipe you want, and optionally how many to make as a trailing number: the same materials are consumed per item and the wait multiplies, so 3 breads cost 3 times the materials and 3 times the time. Examples: [CRAFT flour], [CRAFT bread 3]. Use [CRAFT] on its own to list what this station can make, what each recipe consumes and how long it takes.",
      [](flecs::entity actor, flecs::entity target, std::string args) -> std::string {
      const Workstation* station = target.get<Workstation>();
      if (!station) {
        return "System: This is not a crafting station.\n";
      }

      auto regRes = actor.world().get<RecipeRegistryResource>();
      const RecipeRegistry* registry = regRes ? regRes->registry : nullptr;
      if (!registry) {
        return "System: No recipes are loaded, so nothing can be crafted.\n";
      }

      std::string stationName =
          target.has<DisplayName>() ? target.get<DisplayName>()->name : "station";

      // No recipe named: this is the "what can I make here?" query. The player
      // context menu passes no args, and an agent gets here with [CRAFT].
      std::string requested = TrimCopy(args);
      if (requested.empty()) {
        if (station->recipes.empty()) {
          return "System: The " + stationName + " cannot craft anything.\n";
        }
        std::string msg = "System: The " + stationName + " can craft:\n" +
                          FormatStationRecipes(*station, registry);
        msg += "Use [CRAFT $RECIPE] to make one, for example [CRAFT " +
               station->recipes.front() +
               "]. Add a trailing number to make several in one go, e.g. [CRAFT " +
               station->recipes.front() +
               " 3], which consumes and takes that many times as much.\n";
        return msg;
      }

      // "bread 3" -> {"bread", 3}; a bare "[CRAFT bread]" means one, and a
      // trailing number that is not a count simply stays part of the recipe name.
      std::pair<std::string, int> request = StringUtils::SplitTrailingCount(requested);
      const std::string recipeName = request.first;
      const int batches = request.second > 0 ? request.second : 1;

      const CraftRecipe* recipe =
          ResolveStationRecipe(registry, *station, recipeName);
      if (!recipe) {
        return "System: The " + stationName + " has no recipe called " + recipeName +
               ". Use [CRAFT] to see what it can make.\n";
      }

      // Refuse unsatisfiable work before the wait: a recipe that is only half
      // satisfiable must fail now with a complete shortage list, not after the
      // actor has stood at the station for the whole timer. This claim is a dry
      // run for a timed craft -- CraftResolutionSystem claims and spends again
      // when the timer ends. It is asked for the whole batch, so [CRAFT bread 3]
      // is refused up front when only two loaves' worth of flour is carried.
      RecipeInputClaim claim = ClaimRecipeInputs(actor, *recipe, batches);
      if (!claim.missing.empty()) {
        return "System: You do not have enough materials to craft " +
               FormatBatchedLabel(batches, recipe->id) +
               ". Still needed: " + claim.missing + ".\n";
      }

      auto factoryRes = actor.world().get<ObjectFactoryResource>();
      if (!factoryRes || !factoryRes->factory) {
        return "System: Crafting is unavailable: no object factory is loaded.\n";
      }

      // One timed action per actor. A craft takes real time, so it must not start
      // on top of a harvest (or another craft) and leave two action entities
      // fighting over Busy.
      if (actor.has<Busy>()) {
        return "System: You are already busy doing something else.\n";
      }

      if (recipe->craftTimeSeconds > 0.0f) {
        // Nothing is consumed here. The materials have to survive a craft that
        // fails to resolve, so CraftResolutionSystem spends them right before it
        // creates the outputs; Busy is what keeps them from being spent twice in
        // the meantime. The waiting time is the recipe's own, not a global
        // constant: smelting one ore and forging a scythe should not take the
        // same number of seconds. The id travels on the action entity because
        // the definition lives in the registry, which is allowed to reload.
        // A batch is the per-item wait times the item count, so [CRAFT bread 3]
        // on a 5 second recipe waits 15 seconds.
        const float totalSeconds = recipe->craftTimeSeconds * static_cast<float>(batches);
        flecs::entity actionEntity = actor.world().entity()
            .set<ActionActor>({actor})
            .set<ActionTarget>({target})
            .set<ActionTimer>({totalSeconds})
            .set<CraftAction>({recipe->id, batches});

        actor.set<Busy>({actionEntity});

        std::string msg = "System: Started crafting " +
                          FormatBatchedLabel(batches, recipe->name) +
                          ". It will take " + FormatSeconds(totalSeconds) +
                          " seconds.\n";
        if (actor.has<AgentBrainWrapper>()) {
          // Log it but do NOT append to the agent's context: the craft completes
          // on its own and CraftResolutionSystem reports the outcome, so waking
          // the model here only costs a request and lets the agent act while it
          // is meant to be busy. Same contract as [HARVEST].
          actor.get_mut<AgentBrainWrapper>()->agBrain->logNote(msg);
          return ""; // Return empty so AI Action queue doesn't hold it
        }
        return msg; // Return msg so player queue logs it immediately
      }

      // Instant fallback for a recipe authored with craftTime 0: there is no
      // window between claiming and producing, so the dry-run claim above is the
      // one that gets spent. Destroying it before ProduceRecipeOutputs walks the
      // same relationship is why the entities were collected first. The batch is
      // executed one run at a time so each output rolls its own chance.
      for (flecs::entity item : claim.items) {
        actor.remove<Holds>(item);
        item.destruct();
      }
      Rng rng = NextRollStream(actor.world());
      std::string produced;
      std::vector<SkillGain> training;
      for (int i = 0; i < batches; ++i) {
        RecipeOutcome run = ProduceRecipeOutputs(actor, *recipe, factoryRes->factory, rng);
        if (!run.produced.empty()) {
          if (!produced.empty()) produced += ", ";
          produced += run.produced;
        }
        for (const SkillGain &gain : run.training) {
          training.push_back(gain);
        }
      }

      std::string msg;
      if (produced.empty()) {
        msg = "System: You crafted " + FormatBatchedLabel(batches, recipe->id) +
              " but nothing came out of it.\n";
      } else {
        msg = "System: You crafted " + produced + " and put it in your inventory.\n";
      }
      return msg + FormatTrainingGains(training);
  });

  InteractionRegistry::RegisterComponentInteraction<DisplayName>(
      "Examine",
      "Look closely at this object to learn what it is, what it currently holds and what you can do with it. Example: [EXAMINE]",
      [](flecs::entity actor, flecs::entity target, std::string args) -> std::string {
      std::string objName = target.has<DisplayName>() ? target.get<DisplayName>()->name : "Object";
      std::string description = target.has<ObjectDescription>() ? target.get<ObjectDescription>()->text : "It looks unremarkable.";
      std::string msg = "System: You examined the " + objName + ". " + description + "\n";
      if (target.has<Harvestable>()) {
          const Harvestable* h = target.get<Harvestable>();
          if (h->amountRemaining > 0) {
              msg += "You can harvest it " + std::to_string(h->amountRemaining) + " more time(s).\n";
          } else {
              msg += "It has nothing left to harvest.\n";
          }
      }
      if (target.has<Storage>()) {
          msg += "It can be used to [STORE $ITEM_NAME $COUNT] or [TAKE $ITEM_NAME $COUNT].\n";
          std::vector<std::string> itemNames;
          target.each<Holds>([&](flecs::entity child) {
              if (child.is_alive() && child.has<DisplayName>()) {
                  itemNames.push_back(child.get<DisplayName>()->name);
              }
          });
          if (!itemNames.empty()) {
              msg += "It currently holds: " +
                     StringUtils::FormatStackedNames(StringUtils::StackNames(itemNames)) + ".\n";
          } else {
              msg += "It is currently empty.\n";
          }
      }
      if (const Evolvable* e = target.get<Evolvable>()) {
          if (e->isActive) {
              msg += "It is still growing.\n";
          }
      }
      if (target.has<Workstation>()) {
          auto regRes = target.world().get<RecipeRegistryResource>();
          const RecipeRegistry* registry = regRes ? regRes->registry : nullptr;
          msg += "It can craft:\n";
          msg += FormatStationRecipes(*target.get<Workstation>(), registry);
      }
      return msg;
  });

  InteractionRegistry::RegisterComponentInteraction<Storage>(
      "Store",
      "Put items you are carrying into this container (it has limited space). The last word is how many to store. Examples: [STORE Wheat 3], [STORE Iron Ore 5]",
      [](flecs::entity actor, flecs::entity target, std::string args) -> std::string {
      auto [itemName, count] = StringUtils::SplitTrailingCount(args);
      if (itemName.empty()) {
          return "System: You must specify an item to store. Use [STORE $ITEM_NAME $COUNT].\n";
      }

      // Collect first, then move: transferring Holds mutates the relationship
      // this scan walks, and we only want up to $COUNT items.
      std::vector<flecs::entity> itemsToStore;
      actor.each<Holds>([&](flecs::entity child) {
          if (static_cast<int>(itemsToStore.size()) >= count) return;
          if (child.is_alive() && child.has<DisplayName>()) {
              if (StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, itemName)) {
                  itemsToStore.push_back(child);
              }
          }
      });

      if (itemsToStore.empty()) {
          return "System: You do not have an item named " + itemName + " to store.\n";
      }

      int currentCount = 0;
      target.each<Holds>([&](flecs::entity) { currentCount++; });
      int spaceLeft = target.get<Storage>()->capacity - currentCount;
      if (spaceLeft <= 0) {
          return "System: The storage is full.\n";
      }

      int stored = 0;
      for (flecs::entity item : itemsToStore) {
          if (stored >= spaceLeft) break;

          // An item that leaves its holder stops being worn; otherwise it would
          // keep contributing from inside the chest.
          DropEquipped(item);
          actor.remove<Holds>(item);
          target.add<Holds>(item);
          item.child_of(target);
          stored++;
      }

      std::string msg = "System: You stored " + std::to_string(stored) + " " + itemName + ".\n";
      if (stored < static_cast<int>(itemsToStore.size())) {
          msg += "System: The storage is full, " + std::to_string(static_cast<int>(itemsToStore.size()) - stored) + " " + itemName + " stayed with you.\n";
      }
      return msg;
  });

  InteractionRegistry::RegisterComponentInteraction<Storage>(
      "Take",
      "Take items out of this container and carry them yourself. The last word is how many to take. Examples: [TAKE Wheat 3], [TAKE Iron Ore 5]",
      [](flecs::entity actor, flecs::entity target, std::string args) -> std::string {
      auto [itemName, count] = StringUtils::SplitTrailingCount(args);
      if (itemName.empty()) {
          return "System: You must specify an item to take. Use [TAKE $ITEM_NAME $COUNT].\n";
      }

      // Collect first, then move: transferring Holds mutates the relationship
      // this scan walks, and we only want up to $COUNT items.
      std::vector<flecs::entity> itemsToTake;
      target.each<Holds>([&](flecs::entity child) {
          if (static_cast<int>(itemsToTake.size()) >= count) return;
          if (child.is_alive() && child.has<DisplayName>()) {
              if (StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, itemName)) {
                  itemsToTake.push_back(child);
              }
          }
      });

      if (itemsToTake.empty()) {
          return "System: The storage does not have an item named " + itemName + ".\n";
      }

      for (flecs::entity item : itemsToTake) {
          DropEquipped(item);
          target.remove<Holds>(item);
          actor.add<Holds>(item);
          item.child_of(actor);
      }

      return "System: You took " + std::to_string(static_cast<int>(itemsToTake.size())) + " " + itemName + ".\n";
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
        if (j.contains("provider")) {
          openrouter->setOption("provider", j["provider"]);
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

  GamePosition startPlayerPos = {48, 20};
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
  if (auto registryRes = ecs.get<StatRegistryResource>()) {
    GiveDefaultStats(playerEntity, registryRes->registry);
  }
  if (auto skillRes = ecs.get<SkillRegistryResource>()) {
    GiveStartingSkills(playerEntity, skillRes->registry);
  }
  // The player gets the same default body every other character does, so
  // [EQUIPMENT] and [EQUIP] work from the first frame.
  GiveSlots(playerEntity, &raceRegistry, "", {}, debugLog.get());


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
  npcMenuWindowEntity = ecs.entity("NPC Menu Window");

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
  if (debugWindowState->GetShowNPCMenuWindow()) {
    npcMenuWindowEntity.set<ActiveWindow>(
        {std::make_shared<NPCMenuWindow>(this)});
  }
  if (debugWindowState->GetShowEntityInfoWindow()) {
    playerEntity.set<ActiveWindow>(
        {std::make_shared<EntityInfoWindow>(playerEntity)});
  }
}

void Game::OpenStorageWindow(flecs::entity container) {
  if (!container.is_alive()) {
    return;
  }
  // This can be called from inside a system's iteration (the pending
  // interaction resolver) or from a window's draw pass, so the structural
  // changes are deferred to the end of the stage.
  ecs.defer([this, container]() {
    // Reuse the carrier entity if this container already has a window, so
    // asking to see the same inventory again does not stack a duplicate. A
    // closed window keeps its carrier (with StorageWindowTarget, without
    // ActiveWindow) and is woken back up here.
    flecs::entity carrier = flecs::entity::null();
    ecs.filter<const StorageWindowTarget>().each(
        [&](flecs::entity owner, const StorageWindowTarget &target) {
          if (carrier.is_alive()) {
            return;
          }
          if (target.container == container) {
            carrier = owner;
          }
        });

    if (!carrier.is_alive()) {
      // One carrier per inventory, so several windows can be open at once.
      carrier = ecs.entity();
      carrier.set<StorageWindowTarget>({container});
    }

    if (!carrier.has<ActiveWindow>()) {
      carrier.set<ActiveWindow>(
          {std::make_shared<StorageWindow>(container, carrier)});
    }
  });
}

void Game::OpenCharacterStatusWindow(flecs::entity character) {
  if (!character.is_alive()) {
    return;
  }
  // Called from the context menu's draw pass, so the structural changes are
  // deferred to the end of the stage, the same as OpenStorageWindow.
  ecs.defer([this, character]() {
    // One carrier per character, reused so asking twice does not stack a second
    // window. A character's own ActiveWindow slot cannot be used for this: it
    // already belongs to whichever chat or entity-info window is open on it.
    flecs::entity carrier = flecs::entity::null();
    ecs.filter<const CharacterStatusWindowTarget>().each(
        [&](flecs::entity owner, const CharacterStatusWindowTarget &target) {
          if (carrier.is_alive()) {
            return;
          }
          if (target.character == character) {
            carrier = owner;
          }
        });

    if (!carrier.is_alive()) {
      carrier = ecs.entity();
      carrier.set<CharacterStatusWindowTarget>({character});
    }

    if (!carrier.has<ActiveWindow>()) {
      carrier.set<ActiveWindow>(
          {std::make_shared<CharacterStatusWindow>(character, carrier)});
    }
  });
}
