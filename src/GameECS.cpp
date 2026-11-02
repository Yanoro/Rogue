// GameECS.cpp — character setup, NPC spawn, and the core ECS systems.
//
// Part of the GameECS group, split out of one 5.7k-line file so the build can
// compile it in parallel with the others: GameECSCombat.cpp (combat/crafting),
// GameECSInitSystems.cpp (agent/task/tile-art systems) and GameECSInit.cpp
// (ECSInit orchestration). Symbols shared between them are declared in
// src/GameECSInternals.h.

#include "AI.h"
#include "OllamaAI.h"
#include "GeminiAI.h"
#include "OpenRouterAI.h"
#include "ActorArt.hpp"
#include "AsciiArtDraw.hpp"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
#include "IconPaths.hpp"
#include "SkillshotPath.hpp"
#include "GapLog.h"
#include "LogPaths.h"
#include "Components.h"
#include "DebugLog.h"
#include "DebugWindows.h"
#include "Defaults.h"
#include "DrawAsciiDebug.h"
#include "EntityInfoWindow.h"
#include "StorageWindow.h"
#include "Game.h"
#include "ObjectDesigner.hpp"
#include "TileDesigner.hpp"
#include "ContentEditorWindow.h"
#include "DesignerWindow.h"
#include "TileArtQueue.hpp"
#include "PiperSpeech.h"
#include "SpeechQueue.h"
#include "VisionChatWindow.h"
#include "SystemOneWindow.h"

#include "AgentBrain.h"
#include "AgentActions.h"
#include "TaskVerification.h"
#include "CharacterStatusWindow.h"
#include "NavigationDebugWindow.h"
#include "GlobalCommandRegistry.h"
#include "InteractionRegistry.h"
#include "PathFinding.h"
#include "EffectResolver.hpp"
#include "CombatResolver.hpp"
#include "Equipment.hpp"
#include "EquipmentRuntime.hpp"
#include "FloatingText.hpp"
#include "ItemRegistry.h"
#include "RaceRegistry.h"
#include "RecipeFormatting.h"
#include "Rng.hpp"
#include "Settings.h"
#include "SkillTraining.hpp"
#include "StartingSkills.hpp"
#include "StringUtils.hpp"
#include "flecs.h"
#include "imgui.h"
#include "raylib-cpp.hpp"
#include "raylib.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <cmath>
#include <functional>
#include <iostream>
#include <fstream>
#include <mutex>
#include <queue>
#include <regex>
#include <set>
#include <unordered_set>
#include <vector>

#include "SpatialMemory.hpp"
#include "TimeUtils.hpp"
#include "VisionQuery.hpp"

#include "GameECSInternals.h"


// Gives a character the full standard stat set at its neutral values.
//
// Every value is the registry baseline, so a freshly spawned character is
// mechanically neutral: nothing is faster or slower until something raises a
// stat. A future race that lacks a stat will simply omit it from this list, and
// the baseline fallback in StatView makes that omission harmless -- which is why
// there is deliberately no "default block" to keep in sync anywhere.
void GiveDefaultStats(flecs::entity character,
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
void GiveStartingSkills(flecs::entity character,
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

// Stamps authored starting stat values (a map's or a template's "stats" object)
// over the baseline block GiveDefaultStats stamps. Unknown ids are reported and
// skipped, the same policy authored skills follow at spawn.
void ApplyStartingStats(flecs::entity character,
                               const std::vector<StatValue>& starting,
                               const StatRegistry* registry,
                               const std::string& name, DebugLog* debugLog) {
  if (registry == nullptr || starting.empty()) {
    return;
  }
  StatBlock* block = character.get_mut<StatBlock>();
  if (block == nullptr) {
    return;
  }
  for (const StatValue& authored : starting) {
    bool found = false;
    for (StatValue& value : block->values) {
      if (value.id == authored.id) {
        value.value = authored.value;
        found = true;
        break;
      }
    }
    if (found) {
      continue;
    }
    const std::string message = "Warning: NPC '" + name +
                                "' has a starting value for unknown stat '" +
                                authored.id + "'; skipping it.";
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  }
}

// Stamps authored starting proficiency (the map's "skills" object) onto the
// untrained block every character is given above.
//
// Content errors are reported and skipped rather than fatal, exactly like an
// unknown object type in a map: a typo in one NPC's skills must not abort the
// load. A skill the registry does not know and a stage name the skill does not
// declare both land here, because both are only decidable against the registry
// and the map deliberately does not hold one.
void ApplyStartingSkills(flecs::entity character,
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

// Decides the body a character is born with: the slots it can wear, the look it
// is drawn with, and whether it has a raster sprite instead of that ASCII look.
//
// Precedence, and every fallback exists so a map written before races existed
// still spawns a wearable, visible character rather than a naked or invisible
// one: an authored list on the NPC wins, otherwise the NPC's race, otherwise the
// "human" race, otherwise the code-level humanoid body. A race is content like a
// skill, so an unknown id is a warning and a fallback, never a load failure.
//
// Slots, art and the sprite follow that same precedence but independently: the
// race is looked up even when the NPC authored its own slots, because a one-off
// body should still look like its people. A race that declares no art leaves the
// character on DefaultHumanoidArt(), which is also what the player gets.
void GiveBody(flecs::entity character, const RaceRegistry *races,
                     const std::string &raceId,
                     const std::vector<SlotSpec> &authored,
                     const std::string &authorizedSprite,
                     ResourceManager &resources, DebugLog *debugLog) {
  const std::string wanted = raceId.empty() ? std::string("human") : raceId;
  const RaceDef *race = races ? races->Get(wanted) : nullptr;

  std::vector<SlotSpec> slots = authored;
  if (slots.empty()) {
    if (race != nullptr) {
      slots = race->slots;
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

  character.set<AsciiArt>(race != nullptr && !race->art.lines.empty()
                              ? race->art
                              : DefaultHumanoidArt());

  // A sprite replaces that art when one resolves; when none does, the
  // AsciiArt above remains and the character keeps the ASCII look.
  //
  // `sprite` is an ActorSpriteRegistry id or an explicit image path, not only a
  // path. AttachSprite's first argument is an explicit path, which is why passing
  // an actor id such as "wolf" used to fail and fall through to the race's human
  // look. Resolve the reference through the same path-or-convention rule the
  // Content Editor and portraits use.
  const auto attachReference = [&](const std::string &reference) {
    if (reference.empty()) return false;
    const bool explicitPath =
        reference.find('/') != std::string::npos ||
        reference.find('\\') != std::string::npos ||
        (reference.size() > 4 &&
         reference.compare(reference.size() - 4, 4, ".png") == 0);
    std::string resolved;
    const Texture2D *texture =
        explicitPath
            ? ResolveSpriteTexture(reference, std::string(), resources, debugLog,
                                   &resolved)
            : ResolveSpriteTexture(std::string(), reference, resources, debugLog,
                                   &resolved);
    if (texture == nullptr) return false;
    character.set<Sprite>(Sprite{resolved});
    return true;
  };

  const std::string raceSprite = race != nullptr ? race->sprite : std::string();
  const std::string conventionId = race != nullptr ? race->id : std::string();
  const std::string ownSprite =
      authorizedSprite.empty() ? raceSprite : authorizedSprite;
  if (!attachReference(ownSprite)) {
    attachReference(conventionId);
  }
}

// TODO: Adding checks to see if we are not repeating values seems like a good
// idea for instance we can't have two npcs with the same name

void Game::ResolveNpcTemplate(NPCData &data) const {
  auto warn = [this](const std::string &message) {
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };

  // Whether the map authored the background, captured before the template can
  // fill it: context outranks a template, but never overrides the map, which is
  // the one place an instance can say something different from its definition.
  const bool mapAuthoredBackground = !data.background.empty();

  if (data.suppressBrain) {
    data.brain.clear();
  }
  if (data.suppressCombatStrategy) {
    data.combatStrategy.clear();
  }

  if (!data.npcTemplate.empty()) {
    const NpcTemplate *definition = npcTemplateRegistry.Get(data.npcTemplate);
    if (definition == nullptr) {
      warn("Warning: NPC '" + data.name + "' names unknown template '" +
           data.npcTemplate + "'; spawning from the map entry alone");
    } else {
      // The map wins wherever it speaks; the template fills the rest. Field by
      // field rather than wholesale, so a map can set a name and take a
      // template's stats and gear without restating either.
      if (data.name.empty()) data.name = definition->name;
      if (data.background.empty()) data.background = definition->background;
      if (data.race.empty()) data.race = definition->race;
      if (data.sprite.empty()) data.sprite = definition->sprite;
      if (data.portrait.empty()) data.portrait = definition->portrait;
      if (data.slots.empty()) data.slots = definition->slots;
      if (data.inventory.empty()) data.inventory = definition->inventory;
      if (data.startingSkills.empty()) {
        data.startingSkills = definition->startingSkills;
      }
      if (data.startingStats.empty()) data.startingStats = definition->startingStats;
      if (data.equipped.empty()) data.equipped = definition->equipped;
      if (!data.suppressBrain && data.brain.empty()) {
        data.brain = definition->brain;
      }
      data.disposition = definition->disposition;
      data.retaliates = definition->retaliates;
      data.attackDamage = definition->attackDamage;
      data.attackInterval = definition->attackInterval;
      if (!data.suppressCombatStrategy && data.combatStrategy.empty()) {
        data.combatStrategy = definition->combatStrategy;
      }
    }
  }

  if (data.suppressBrain) {
    data.brain.clear();
  }
  if (data.suppressCombatStrategy) {
    data.combatStrategy.clear();
  }
  if (data.brain.empty()) {
    return;
  }
  const BrainTemplate *brain = brainTemplateRegistry.Get(data.brain);
  if (brain == nullptr) {
    warn("Warning: NPC '" + data.name + "' names unknown brain '" + data.brain +
         "'; spawning with no agent context");
    return;
  }
  // A brain's background is what motivates the agent, so it outranks the
  // template's -- but not the map's, which is the instance's own word.
  if (!mapAuthoredBackground && !brain->background.empty()) {
    data.background = brain->background;
  }
  if (data.terminalGoals.empty()) {
    data.terminalGoals = brain->terminalGoals;
  }
}

bool Game::ApplyPlayerMaxSpeed(float maxSpeed, std::string &message) {
  message.clear();
  if (!playerEntity.is_alive()) {
    message = "The player has not been created yet.";
    return false;
  }
  playerEntity.set<MaxSpeed>({maxSpeed});
  std::ostringstream text;
  text << "Player top speed set to " << maxSpeed << ".";
  message = text.str();
  return true;
}

bool Game::ApplyPlayerActorSprite(const std::string &id, std::string &message) {
  message.clear();
  if (!playerEntity.is_alive()) {
    message = "The player has not been created yet.";
    return false;
  }
  if (id.empty()) {
    if (playerEntity.has<Sprite>()) {
      playerEntity.remove<Sprite>();
    }
    message = "Player uses the race's look.";
    return true;
  }
  if (actorSpriteRegistry.Get(id) == nullptr) {
    message = "No actor sprite is named '" + id + "'.";
    return false;
  }
  if (!AttachSprite(playerEntity, std::string(), id, resourceManager,
                    debugLog.get())) {
    message = "Actor sprite '" + id + "' has no art (data/sprites/" + id +
              ".png); the player's look is unchanged.";
    return false;
  }
  message = "Player now wears the '" + id + "' sprite.";
  return true;
}


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
          // Tile-sized, because an actor occupies a tile like any object and
          // because the whole figure has to be drawn inside it. The glyph is the
          // fallback for a race that supplies no art, and this colour is what
          // DefaultHumanoidArt() deliberately leaves out of its palette: the
          // tunic.
          .set<DrawAscii>({
              '@',
              {128, 0, 0, 255},
              {128, 128, 128, 0},
              static_cast<size_t>(map->GetTileWidth()),
              static_cast<size_t>(map->GetTileHeight()),
          })
          .set<WindowOnClick>({WindowType::EntityInfoWindowType})
          .set<Velocity>({0, 0})
          // Authored here rather than attached lazily by the facing system, for
          // the same reason Velocity is: a character without it would be a
          // character the system silently skips, and a component that appears a
          // frame late is harder to explain than one set at birth.
          .set<Facing>({})
          .set<Acceleration>({0, 0})
          .set<Friction>({DEFAULT_ENTITY_FRICTION})
          // The EntityInfo fallback is replaced with NPCContext only when a
          // brain is actually attached. A brainless wolf has no agent context
          // window to show.
          .add<BlocksTile>();

  entity.add<Actor>();

  // The agent phase of docs/spatial-memory-design.md: an NPC perceives through
  // the same `Vision` + `SpatialMemory` pair the player does. It is not told
  // where anything is -- the map's authored place names arrive as seeds and stay
  // inert until that ground is observed.
  AttachPerception(entity);

  // Stamped before the brain is built, so the agent's prompt and every later
  // resolution see a complete and neutral sheet.
  if (auto registryRes = ecs.get<StatRegistryResource>()) {
    GiveDefaultStats(entity, registryRes->registry);
    ApplyStartingStats(entity, data.startingStats, registryRes->registry, name,
                       debugLog.get());
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
  GiveBody(entity, &raceRegistry, data.race, data.slots, data.sprite,
           resourceManager, debugLog.get());

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

  // Combat state. NPCs used to be bodies with stats but no health; the debug
  // spawner and real hostility need the same Vitals the player resolves.
  {
    const ResolutionInputs inputs = MakeResolutionInputs(entity.world());
    const FoldResult maxHealth = ResolveMaxHealth(entity, inputs.definitions);
    const float ceiling = std::max(1.0f, maxHealth.value);
    entity.set<Vitals>({ceiling, ceiling});
  }
  entity.set<MaxSpeed>({DEFAULT_MAXSPEED});

  Disposition stance = Disposition::Neutral;
  if (data.disposition == "aggressive") {
    stance = Disposition::Aggressive;
  }
  entity.set<NpcDisposition>({stance});
  if (data.retaliates) {
    entity.add<Retaliates>();
  }

  // Every NPC carries the melee profile even while Neutral. The controller skips
  // Neutral entities, so flipping the stance (or retaliating) starts it without
  // rebuilding the entity or losing the authored numbers.
  Aggressive attack;
  attack.damage = data.attackDamage > 0.0f ? data.attackDamage : 8.0f;
  attack.interval = data.attackInterval > 0.0f ? data.attackInterval : 6.0f;
  entity.set<Aggressive>(attack);

  // The tactic layer is for brainless bodies only: a brain-driven character's
  // movement belongs to its agent. An unknown id is left on the component so the
  // system can report/ignore it rather than silently substituting another tactic.
  if (data.brain.empty() && !data.combatStrategy.empty()) {
    entity.set<CombatStrategy>({data.combatStrategy, 0.0f});
  }

  // Portrait identity: authored on this NPC, else the people's default. An id the
  // registry does not know is not an error here -- the HUD draws the unknown
  // sprite -- so a typo costs a face, not a spawn.
  {
    std::string portraitId = data.portrait;
    if (portraitId.empty()) {
      const std::string raceId = data.race.empty() ? "human" : data.race;
      if (const RaceDef *race = raceRegistry.Get(raceId)) {
        portraitId = race->portrait;
      }
    }
    if (!portraitId.empty()) {
      entity.set<Portrait>({portraitId});
    }
  }

  // Life goals are authored data, seeded once here and read-only afterwards
  // (docs/task-system-design.md section 5): a model that re-derives its life
  // goals each session makes a character inconsistent across runs, which is the
  // specific thing the task system exists to remove. Stamped before the brain is
  // built, like the other spawn-time state, so the agent's first digest already
  // has its frame. An NPC with no authored goals gets no TaskList at all, which
  // is the "no list behaves exactly as today" case.
  if (!data.terminalGoals.empty()) {
    TaskList taskList;
    SeedTerminalGoals(data.terminalGoals, CurrentTaskTick(entity.world()),
                      taskList);
    if (!taskList.goals.empty()) {
      entity.set<TaskList>(std::move(taskList));
    }
  }

  if (name.empty()) {
    name = "NPC " + std::to_string(entity.id());
  }
  entity.set<DisplayName>({name});
  entity.set<NameTagColor>({BLUE});
  // Flecs names are unique. The display name is intentionally shared -- two
  // wolves are both "Wolf" -- so the entity identity gets the id suffix while
  // every human-facing label keeps using DisplayName.
  const std::string entityName = name + " #" + std::to_string(entity.id());
  entity.set_name(entityName.c_str());

  // A brain is what makes the character LLM-driven. No brain id means a body
  // with no agent: it can still fight (the disposition controller is a C++
  // component), but it has no NPCContext and no AgentBrainWrapper.
  if (!data.brain.empty()) {
    std::string characterNames = "";
    if (characterNames.length() >= 2) {
      characterNames.erase(characterNames.length() - 2);
    }

    // Format-then-short-description, one entry per visible command. The same
    // builder produces the bare [HELP] reply, so the list the model is spawned
    // with and the list [HELP] prints cannot drift apart. The template already
    // supplies the blank line that follows the list.
    std::string commandsList = GlobalCommandRegistry::BuildVisibleCommandList();

    std::regex re2("%BACKGROUND%");
    std::regex re3("%CHARACTERS%");
    std::regex re4("%COMMANDS_LIST%");

    // The template is the compiled-in DEFAULT_NPC_PROMPT unless settings.ai
    // supplied a file. Either way the same placeholders are substituted, so an
    // authored prompt cannot silently lose the live command list.
    //
    // There is no place list here any more: a character must ask [PLACES] and
    // can be wrong. Handing every agent the map's names at spawn was the known
    // gap in docs/place-derivation-design.md section 10.
    const std::string &promptTemplate = Settings().ai.npcPromptTemplate.empty()
                                            ? DEFAULT_NPC_PROMPT
                                            : Settings().ai.npcPromptTemplate;

    std::string startingPrompt = promptTemplate;
    startingPrompt = std::regex_replace(startingPrompt, re2, characterBackground);
    startingPrompt = std::regex_replace(startingPrompt, re3, characterNames);
    startingPrompt = std::regex_replace(startingPrompt, re4, commandsList);
    startingPrompt += "\n";
    entity.set<NPCContext>({ {{"system", startingPrompt}}, ""});

    auto brain = std::make_unique<AgentBrain>(entity, name, debugLog.get());
    brain->SetMemorySalience(systemOneClassifier.get(),
                             Settings().ai.memorySalience);
    brain->isStopped =
        debugWindowState ? debugWindowState->GetStopAllAI() : false;
    entity.set<AgentBrainWrapper>({std::move(brain)});
  }
  return entity;
}

flecs::entity Game::SpawnNpc(NPCData data) {
  // The caller resolves the template and applies instance overrides first;
  // resolving again here would overwrite a per-spawn stance or attack profile.
  flecs::entity npc = createNPC(data);
  if (!npc.is_alive()) {
    return npc;
  }
  if (auto factoryRes = ecs.get<ObjectFactoryResource>()) {
    if (factoryRes->factory != nullptr) {
      factoryRes->factory->SpawnInventory(ecs, npc, data.inventory);
    }
  }
  return npc;
}

void Game::SpawnPendingNpcAt(GamePosition position) {
  const PendingNpcSpawn request = pendingNpcSpawn;
  npcSpawnerStatus.clear();

  if (!map) {
    npcSpawnerStatus = "No map is loaded.";
    return;
  }
  if (!map->IsInBounds(static_cast<float>(position.x),
                       static_cast<float>(position.y)) ||
      map->IsBlocked(position.x, position.y)) {
    npcSpawnerStatus = "Tile (" + std::to_string(position.x) + "," +
                       std::to_string(position.y) +
                       ") is blocked; click a free tile.";
    return;
  }

  NPCData data;
  data.position = position;
  data.npcTemplate = request.npcTemplate;
  data.brain = request.brain;
  data.suppressBrain = request.suppressBrain;
  ResolveNpcTemplate(data);

  // The window's stance/attack choices are per-instance overrides, applied after
  // the template has filled its own defaults.
  data.disposition = request.aggressive ? "aggressive" : "neutral";
  data.retaliates = request.retaliates;
  if (request.suppressCombatStrategy) {
    data.combatStrategy.clear();
  } else if (!request.combatStrategy.empty()) {
    data.combatStrategy = request.combatStrategy;
  }
  if (request.attackDamage > 0.0f) {
    data.attackDamage = request.attackDamage;
  }
  if (request.attackInterval > 0.0f) {
    data.attackInterval = request.attackInterval;
  }

  flecs::entity npc = SpawnNpc(data);
  if (!npc.is_alive()) {
    npcSpawnerStatus = "The NPC could not be created.";
    pendingNpcSpawn.active = false;
    return;
  }
  pendingNpcSpawn.active = false;

  if (request.health > 0.0f) {
    if (Vitals *vitals = npc.get_mut<Vitals>()) {
      vitals->maxHealth = request.health;
      vitals->health = request.health;
    }
  }
  if (request.speed > 0.0f) {
    npc.set<MaxSpeed>({request.speed});
  }

  std::string npcName = "NPC";
  if (const DisplayName *displayName = npc.get<DisplayName>()) {
    npcName = displayName->name;
  }
  npcSpawnerStatus = "Spawned " + npcName + " at (" +
                     std::to_string(position.x) + "," +
                     std::to_string(position.y) + ").";
  if (debugLog) {
    debugLog->LogInfo("NPC spawner: " + npcSpawnerStatus);
  }
}

void Game::KillEntity(flecs::entity entity) {
  if (!entity.is_alive() || entity.has<PlayerControlled>() ||
      entity.has<TrainingDummy>()) {
    return;
  }

  entity.add<Dead>();
  const GamePosition *position = entity.get<GamePosition>();
  std::string name = "NPC";
  if (const DisplayName *displayName = entity.get<DisplayName>()) {
    name = displayName->name;
  }

  // Spawn the corpse before destroying the body: it needs the death tile and
  // the held items still attached. The corpse template deliberately has no
  // Interactable and no Obstacle, so it is scenery the player can walk over and
  // cannot interact with yet.
  flecs::entity corpse = flecs::entity::null();
  auto factoryRes = ecs.get<ObjectFactoryResource>();
  flecs::entity mapEntity = ecs.lookup("CurrentMap");
  if (factoryRes && factoryRes->factory != nullptr && map != nullptr &&
      position != nullptr && mapEntity.is_alive()) {
    corpse = factoryRes->factory->SpawnObject(
        ecs, mapEntity, map.get(), "corpse", *position, false);
  }

  if (corpse.is_alive()) {
    corpse.add<Corpse>();
    std::vector<flecs::entity> held;
    entity.each<Holds>([&](flecs::entity child) { held.push_back(child); });
    for (flecs::entity child : held) {
      if (!child.is_alive()) {
        continue;
      }
      entity.remove<Holds>(child);
      child.child_of(corpse);
      corpse.add<Holds>(child);
    }
  } else if (debugLog) {
    debugLog->LogError("Death: could not spawn a corpse for " + name + ".");
  }

  // A dead caster's committed action must not survive the body.
  if (const Casting *casting = entity.get<Casting>()) {
    const flecs::entity action = casting->actionEntity;
    if (action.is_alive()) {
      action.destruct();
    }
  }
  entity.remove<Casting>();
  entity.remove<Busy>();

  if (debugLog) {
    debugLog->LogCombat(name + " dies.");
  }
  entity.destruct();
}

void Game::ECSInitRenderSystems() {
  renderPipeline = ecs.pipeline().with(flecs::System).with<Render>().build();

  // Two systems, registered in this order, because a render pipeline runs its
  // systems in registration order: the map's own tiles first, everything standing
  // on them after.
  //
  // The split is not cosmetic. Tiles used to be glyphs on a half-transparent
  // plate, so a tile drawn over a character only dimmed it. A tile with a sprite
  // is a full-bleed opaque texture, and the same mistake would erase the
  // character instead. Drawing order was previously incidental -- it happened to
  // work because Map creates its tile entities before anything else -- so it is
  // now stated rather than inherited.
  //
  // Both systems call the one method, so there is still a single implementation
  // of "draw what this entity looks like"; only the order is separated.
  ecs.system<DrawAscii, ScreenPosition>("DrawTileLayer")
      .with<MapTileVisual>()
      .kind<Render>()
      .each([this](flecs::entity e, const DrawAscii &ascii,
                   const ScreenPosition screenPos) {
        DrawEntityVisual(e, ascii, screenPos);
      });

  ecs.system<DrawAscii, ScreenPosition>("DrawEntityLayer")
      .without<MapTileVisual>()
      .kind<Render>()
      .each([this](flecs::entity e, const DrawAscii &ascii,
                   const ScreenPosition screenPos) {
        DrawEntityVisual(e, ascii, screenPos);
      });

  // Name tags and the rest of the render work, registered last so they land on
  // top of both layers. Kept in its own function only because DrawEntityVisual is
  // defined between this point and there; the registration order, which is what
  // the pipeline actually honours, is unchanged.
  ECSInitOverlaySystems();
}

void Game::DrawEntityVisual(flecs::entity e, const DrawAscii &ascii,
                            const ScreenPosition &screenPos) {
  // A raster sprite wins over the ASCII look when the entity has one.
  // Sprite is only ever attached once its image loaded (SpritePaths.hpp),
  // and the lookup is a cache hit, so a null here means the texture was
  // evicted rather than that the path was bad -- and falling through to the
  // art below is the right answer for that, not a blank tile.
  //
  // Resolved *before* the plate because whether a sprite draws is what decides
  // whether the plate is drawn at all -- see the comment on it below.
  const Sprite *sprite = e.get<Sprite>();
  Rectangle spriteRect = {0.0f, 0.0f, 0.0f, 0.0f};
  if (sprite != nullptr) {
    if (raylib::Texture *texture = resourceManager.GetTexture(sprite->path)) {
      spriteRect = DrawSpriteFitted(*texture, screenPos.x, screenPos.y,
                                    static_cast<float>(ascii.width),
                                    static_cast<float>(ascii.height));
    }
  }
  // A sprite or the Unknown placeholder -- nothing else. The glyph and ASCII-art
  // fallbacks are gone: an entity that has no sprite is now *visible* as missing
  // rather than silently substituted, which is what the placeholder is for.
  //
  // The condition is "a texture actually drew", not "the entity has a Sprite
  // component": a Sprite whose texture failed to load must fall back to the
  // placeholder too, or the cell would be blank.
  if (spriteRect.width <= 0.0f) {
    if (raylib::Texture *unknown = resourceManager.GetTexture(UnknownSpritePath())) {
      spriteRect = DrawSpriteFitted(*unknown, screenPos.x, screenPos.y,
                                    static_cast<float>(ascii.width),
                                    static_cast<float>(ascii.height));
    }
  }

  // Debug rectangles - now togglable
  if (DrawAsciiDebug::GetShowOuterRectangles()) {
    raylib::Rectangle outerRect(screenPos.x, screenPos.y, ascii.width,
                                ascii.height);
    outerRect.DrawLines(RED, 1.0f);
  }

  if (DrawAsciiDebug::GetShowInnerRectangles()) {
    // The drawn sprite's own rect, which is what says whether it sits centred in
    // the tile. There is no second box any more: the look is always an image.
    if (spriteRect.width > 0.0f) {
      DrawRectangleLinesEx(spriteRect, 1.0f, BLUE);
    }
  }
}

// The render work that draws over the tile and entity layers: name tags and the
// other overlays. Registered by ECSInitRenderSystems, after both layers.
void Game::ECSInitOverlaySystems() {
  ecs.system<ScreenPosition, DisplayName>().kind<Render>().each(
      [](flecs::entity e, const ScreenPosition &pos, const DisplayName &displayName) {
        // Name tags are opt-in for objects. Characters always show theirs; a
        // field of Structural wall objects must not turn into a wall of text.
        if (e.has<Structural>()) {
          return;
        }
        if (!e.has<ShowNameTag>() && !e.has<Actor>()) {
          return;
        }

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
      .each([this](flecs::entity actionEntity, const ActionTimer &timer,
                   const ActionActor &action) {
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

        // A cast carries its ability name on a small dark strip under the bar.
        // Every caster gets this, not just the player: the bar belongs to the
        // action entity, so an NPC's telegraph reads the same as the player's.
        if (const CastAction *cast = actionEntity.get<CastAction>()) {
          if (cast->label.empty()) {
            return;
          }
          const Vector2 textSize =
              MeasureTextEx(gameFont, cast->label.c_str(),
                            DEFAULT_CAST_LABEL_FONT_SIZE, 0);
          const float labelWidth =
              textSize.x + DEFAULT_CAST_LABEL_PAD_X * 2.0f;
          const float labelHeight =
              textSize.y + DEFAULT_CAST_LABEL_PAD_Y * 2.0f;
          const float labelX =
              std::round(pos.x + entityWidth / 2.0f - labelWidth / 2.0f);
          const float labelY =
              std::round(barY + DEFAULT_ACTION_BAR_HEIGHT +
                         DEFAULT_CAST_LABEL_GAP_Y);

          DrawRectangleRounded(
              Rectangle{labelX, labelY, labelWidth, labelHeight}, 0.35f, 4,
              DEFAULT_CAST_LABEL_BACKGROUND);
          DrawTextEx(gameFont, cast->label.c_str(),
                     {labelX + DEFAULT_CAST_LABEL_PAD_X,
                      labelY + DEFAULT_CAST_LABEL_PAD_Y},
                     DEFAULT_CAST_LABEL_FONT_SIZE, 0, DEFAULT_CAST_LABEL_TEXT);
        }
      });

  // Projectiles draw above the world and under the floating numbers, so a hit
  // report is never hidden by the object that caused it.
  ecs.system<Projectile, ScreenPosition>("ProjectileRenderSystem")
      .kind<Render>()
      .each([this](const Projectile& projectile,
                   const ScreenPosition& position) {
        const Texture2D* texture = ResolveSpriteReference(
            projectile.sprite, resourceManager, debugLog.get());
        if (texture == nullptr) {
          return;
        }
        DrawSpriteFitted(*texture, position.x - projectile.size / 2.0f,
                         position.y - projectile.size / 2.0f,
                         projectile.size, projectile.size);
      });

  // The aimed lance itself: a rotated sprite following its path. The sprite art
  // points right; the rotation is the segment heading at the current distance.
  ecs.system<Skillshot>("SkillshotRenderSystem")
      .kind<Render>()
      .each([this](const Skillshot& skillshot) {
        if (skillshot.path.size() < 2) {
          return;
        }
        const Texture2D* texture = ResolveSpriteReference(
            skillshot.sprite, resourceManager, debugLog.get());
        if (texture == nullptr) {
          return;
        }
        const Vector2 position = SkillshotPointAt(
            skillshot.path, skillshot.distanceTraveled);
        const Vector2 direction = SkillshotDirectionAt(
            skillshot.path, skillshot.distanceTraveled);
        const Rectangle source = {0.0f, 0.0f,
                                  static_cast<float>(texture->width),
                                  static_cast<float>(texture->height)};
        const Rectangle destination = {
            position.x, position.y, skillshot.length, skillshot.width};
        const Vector2 origin = {skillshot.length * 0.5f,
                                skillshot.width * 0.5f};
        const float rotation =
            atan2f(direction.y, direction.x) * RAD2DEG;
        DrawTexturePro(*texture, source, destination, origin, rotation, WHITE);
      });

  // The aim preview. It draws the same polyline the launch will sample, so the
  // player is looking at the real path, not an approximation of it.
  ecs.system<SkillshotAim>("SkillshotAimRenderSystem")
      .kind<Render>()
      .each([](const SkillshotAim& aim) {
        if (aim.path.size() < 2) {
          return;
        }
        for (size_t i = 1; i < aim.path.size(); ++i) {
          const Vector2 a = aim.path[i - 1];
          const Vector2 b = aim.path[i];
          DrawLineEx(a, b, aim.width, DEFAULT_SKILLSHOT_AIM_FILL);
          DrawLineEx(a, b, 2.0f, DEFAULT_SKILLSHOT_AIM_EDGE);
        }
        DrawCircleV(aim.path.back(), 5.0f, DEFAULT_SKILLSHOT_AIM_TIP);
      });

  // Cast range and the positions from which a movement-enabled cast keeps its
  // locked target in range. Chebyshev tiles, matching WithinActivationRange.
  ecs.system<SkillRangeIndicator, GamePosition>("SkillRangeIndicatorRender")
      .kind<Render>()
      .each([this](flecs::entity entity,
                   const SkillRangeIndicator &indicator,
                   const GamePosition &casterPosition) {
        if (!Settings().combat.showSkillRanges || map == nullptr ||
            indicator.rangeTiles <= 0.0f) {
          return;
        }
        const int range =
            std::max(1, static_cast<int>(std::ceil(indicator.rangeTiles)));
        const float tileWidth = static_cast<float>(map->GetTileWidth());
        const float tileHeight = static_cast<float>(map->GetTileHeight());
        const Color castColour = {90, 170, 255, 165};
        const Color movementColour = {120, 230, 140, 125};
        const Color movementFill = {120, 230, 140, 20};

        const auto drawTile = [&](int x, int y, Color colour, bool filled) {
          if (!map->IsInBounds(static_cast<float>(x),
                               static_cast<float>(y))) {
            return;
          }
          const ScreenPosition tile =
              map->GameCoordsToScreenCoords(x, y);
          const Rectangle rect = {tile.x, tile.y, tileWidth, tileHeight};
          if (filled) {
            DrawRectangleRec(rect, colour);
          } else {
            DrawRectangleLinesEx(rect, 1.5f, colour);
          }
        };

        // Cast range: the boundary of the caster's square.
        for (int dy = -range; dy <= range; ++dy) {
          for (int dx = -range; dx <= range; ++dx) {
            if (std::max(std::abs(dx), std::abs(dy)) != range) continue;
            drawTile(casterPosition.x + dx, casterPosition.y + dy, castColour,
                     false);
          }
        }

        // Valid caster positions: tiles from which the target is still in the
        // skill's range. A disabled-movement skill cancels on any step, so it
        // deliberately shows only the cast ring, not a misleading safe area.
        if (!indicator.movementArea || !indicator.target.is_alive()) {
          return;
        }
        const GamePosition *targetPosition =
            indicator.target.get<GamePosition>();
        if (targetPosition == nullptr) {
          return;
        }
        if (range <= 2) {
          for (int dy = -range; dy <= range; ++dy) {
            for (int dx = -range; dx <= range; ++dx) {
              drawTile(targetPosition->x + dx, targetPosition->y + dy,
                       movementFill, true);
            }
          }
        }
        for (int dy = -range; dy <= range; ++dy) {
          for (int dx = -range; dx <= range; ++dx) {
            if (std::max(std::abs(dx), std::abs(dy)) != range) continue;
            drawTile(targetPosition->x + dx, targetPosition->y + dy,
                     movementColour, false);
          }
        }
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

        // An ability call-out carries its own strip. It fades with the word, so
        // the bar cannot outlive the text it explains.
        if (floating.background) {
          const Color background = {
              DEFAULT_CAST_LABEL_BACKGROUND.r,
              DEFAULT_CAST_LABEL_BACKGROUND.g,
              DEFAULT_CAST_LABEL_BACKGROUND.b,
              static_cast<unsigned char>(
                  std::clamp(static_cast<float>(
                                 DEFAULT_CAST_LABEL_BACKGROUND.a) *
                                 motion.alpha,
                             0.0f, 255.0f))};
          DrawRectangleRounded(
              Rectangle{textPos.x - DEFAULT_CAST_LABEL_PAD_X,
                        textPos.y - DEFAULT_CAST_LABEL_PAD_Y,
                        textSize.x + DEFAULT_CAST_LABEL_PAD_X * 2.0f,
                        textSize.y + DEFAULT_CAST_LABEL_PAD_Y * 2.0f},
              0.35f, 4, background);
        }

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

          if (!map->IsBlocked(currX, currY)) {
            continue;
          }
          ScreenPosition currTileScreenPos =
              map->GameCoordsToScreenCoords(currX, currY);
          raylib::Rectangle currRect(currTileScreenPos.x, currTileScreenPos.y,
                                     (float)map->GetTileWidth(),
                                     (float)map->GetTileHeight());
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
  map = std::make_unique<Map>(mapEntity, mapPath, debugLog.get(),
                              &resourceManager);
  ecs.set<MapResource>({map.get()});

  Map *currentMap = map.get();
  // The Map constructor has already spawned the map's authored objects, so the
  // blocking cache can be correct before the relocation pass below.
  currentMap->RefreshBlockingObjects();
  // Teleport entities that get stuck to safety
  // TODO: Maybe have a better tag instead of velocity
  // to tell when an entity should be teleported out
  if (currentMap) {
    if (spawnNPCs) {
      for (const auto& authored : currentMap->GetNPCs()) {
        // The map is the instance; the template is its starting point. Resolving
        // here rather than in createNPC keeps createNPC a single spawn path for
        // both template-built and authored characters.
        NPCData npc = authored;
        ResolveNpcTemplate(npc);
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
              bool blocks = currentMap->IsBlocked(tx, ty);
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
              q.pop();

              if (!currentMap->IsBlocked(curr.x, curr.y)) {
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
            if (!map->IsBlocked(currX, currY)) {
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

  // Facing, derived and never authored (docs/spatial-memory-design.md 4.1).
  //
  // Registered after the steer system in the same function so it reads the
  // velocity that was just written rather than last frame's. It has no
  // Actor term on purpose: what makes an entity faceable is having the
  // component, and that is exactly what stage 1 will sample from -- so this stays
  // correct if a future non-character ever needs a direction.
  ecs.system<const Velocity, const ScreenPosition, const Hitbox, Facing>()
      .each([this](flecs::entity entity, const Velocity &vel,
                   const ScreenPosition &screenPos, const Hitbox &hitbox,
                   Facing &facing) {
        // Moving: face the way the body is going. The steer system decelerates
        // through small non-zero velocities, hence a threshold rather than a
        // zero test -- otherwise a stopping character's facing spins on residual
        // values.
        if (vel.Length() > DEFAULT_FACING_MIN_SPEED) {
          facing = Facing(vel.Normalize());
          return;
        }

        // Standing still: the player aims, everyone else keeps whatever
        // direction it last had. Aiming at the cursor is what makes the cone
        // testable without walking, and it is the only player-specific rule in
        // the system.
        if (entity != playerEntity) {
          return;
        }

        // The Vision Debug window can switch the mouse rule off entirely, which
        // is what lets you walk a cone past something without the cursor dragging
        // it around. A debug switch, not a gameplay setting: the design says the
        // stationary player aims (section 4.1), and this only makes that rule
        // observable from the outside.
        if (debugWindowState && !debugWindowState->GetCursorFacingEnabled()) {
          return;
        }

        // Not while a window owns the mouse: dragging a debug window would
        // otherwise swing the player's facing around the screen.
        if (ImGui::GetIO().WantCaptureMouse) {
          return;
        }

        const raylib::Vector2 center =
            screenPos + raylib::Vector2(hitbox.width, hitbox.height) / 2.0f;
        const raylib::Vector2 toCursor =
            raylib::Vector2(GetScreenToWorld2D(GetMousePosition(), camera)) -
            center;
        // A cursor exactly on the character has no direction to offer, so the
        // last one stands rather than being replaced by a zero vector.
        if (toCursor.Length() > 0.0f) {
          facing = Facing(toCursor.Normalize());
        }
      });

}

// Gives one character eyes and a record (docs/spatial-memory-design.md section 8,
// the agent phase).
//
// Both systems that consume these components are written per entity: the sampler
// keeps its sweep inside the component, the writer keys off that entity's own
// `lastVisionTick`, and derivation and consolidation are pure functions over one
// `SpatialMemory`. Attaching the components is therefore the whole change the
// player-only phase deferred -- there is no shared state to guard.
//
// The authored names arrive as seeds, not as knowledge: nothing resolves until
// the ground under the anchor has been observed (place-derivation doc section 8).
void Game::AttachPerception(flecs::entity entity) {
  if (!entity.is_alive()) {
    return;
  }

  const GameSettings::PerceptionSection &perceptionSettings =
      Settings().perception;
  Vision vision;
  vision.range = perceptionSettings.range;
  vision.angleDegrees = perceptionSettings.angleDegrees;
  vision.radarRadius = perceptionSettings.radarRadius;
  entity.set<Vision>(vision);
  // Empty on spawn: the Origin place is created by the first sample, which is
  // what makes "I have no idea where I am" the honest starting state.
  entity.set<SpatialMemory>({});

  SpatialMemory *memory = entity.get_mut<SpatialMemory>();
  if (memory == nullptr || map == nullptr) {
    return;
  }

  for (const PlaceSeed &authored : map->GetPlaceSeeds()) {
    PlaceNameSeed seed;
    seed.label = authored.label;
    seed.anchor = authored.position;
    seed.source = KnowledgeSource::Authored;
    seed.createsPlace = true;
    seed.radius = authored.radius;
    memory->nameSeeds.push_back(std::move(seed));
  }
  if (!memory->nameSeeds.empty()) {
    ++memory->contentRevision;
  }

  if (debugLog) {
    const std::string name = entity.name().c_str();
    debugLog->LogInfo("Spatial memory: " +
                      (name.empty() ? std::string("character") : name) + " got " +
                      std::to_string(memory->nameSeeds.size()) +
                      " name seed(s) from the map");
  }
}

// Fills each `Vision`'s cache (docs/spatial-memory-design.md section 4.4).
//
// The sweep is rebuilt only when it can have changed: the origin tile, the facing
// beyond a threshold (otherwise the cone would be recomputed for every degree of
// a turn), a tuning parameter, or the periodic ceiling that notices an obstacle
// appearing or being felled without tracking either event.
//
// The opaque set is rebuilt per sample rather than cached. A sample is a handful
// per second, and a stale set would be exactly the kind of quiet wrongness this
// whole feature exists to make visible. It is rebuilt *after* the staleness check,
// so a character standing still costs one comparison per frame, not a sweep.
void Game::ECSInitVisionSystems() {
  ecs.system<Vision, const Facing, const GamePosition>("VisionSampler")
      .each([this](flecs::entity, Vision &vision, const Facing &facing,
                   const GamePosition &origin) {
        const int nowTick = CurrentTaskTick(ecs);

        // A pending [LOOK around] asks for one 360-degree sweep. The request is
        // consumed here rather than by the action so line of sight has exactly one
        // implementation. `angleDegrees` is left alone: after this sample the
        // cached angle says 360, so the next frame sees the mismatch and restores
        // the character's own cone.
        const bool fullCircle = vision.fullCircleRequested;
        const float sampleAngle =
            fullCircle ? 360.0f : vision.angleDegrees;

        const float facingDot = facing.x * vision.cachedFacing.x +
                                facing.y * vision.cachedFacing.y;
        const bool stale =
            vision.fullCircleRequested || vision.lastVisible.empty() ||
            !(origin == vision.cachedOrigin) ||
            vision.range != vision.cachedRange ||
            vision.angleDegrees != vision.cachedAngleDegrees ||
            vision.radarRadius != vision.cachedRadarRadius ||
            facingDot <
                std::cos(DEFAULT_VISION_FACING_RESAMPLE_DEGREES * DEG2RAD) ||
            vision.lastSampleTick < 0 ||
            nowTick - vision.lastSampleTick >= DEFAULT_VISION_RESAMPLE_TICKS;
        if (!stale) {
          return;
        }

        // Sight is blocked by objects that carry BlocksVision, not by tiles: a
        // river blocks movement and nothing else, a glass wall blocks movement and
        // nothing else either. See the component.
        std::unordered_set<std::uint64_t> opaque;
        ecs.filter<const BlocksVision, const GamePosition>().each(
            [&opaque](const BlocksVision &, const GamePosition &pos) {
              opaque.insert(VisionQuery::CellKey(pos));
            });

        if (!map) {
          return;
        }

        VisionQuery::VisibleCells(*map, origin, facing, vision.range, sampleAngle,
                                  vision.radarRadius, opaque, vision.lastVisible);

        // A survey is one frame's answer; keep its own copy so the action that
        // asked for it can report it even after the cone has been restored (the
        // brain does not necessarily step the action on the very next frame).
        if (fullCircle) {
          vision.lastFullCircle = vision.lastVisible;
          vision.lastFullCircleTick = nowTick;
        }

        vision.cachedOrigin = origin;
        vision.cachedFacing = facing;
        vision.cachedRange = vision.range;
        // The angle the sweep actually used, not the component's standing value:
        // they differ for a survey, and the next frame's mismatch is what puts the
        // cone back.
        vision.cachedAngleDegrees = sampleAngle;
        vision.cachedRadarRadius = vision.radarRadius;
        vision.lastSampleTick = nowTick;
        vision.fullCircleRequested = false;
      });
}

// Records what each character has seen (docs/spatial-memory-design.md section 3.4).
//
// A consumer of the vision sweep, not a second sampler. It fires only when the
// sampler has produced a new set, so the geometry is computed once and the
// overlay, the memory and (from stage 3) the place derivation all read the same
// answer -- the same rule the placement validator follows.
void Game::ECSInitMemorySystems() {
  ecs.system<SpatialMemory, const Vision, const GamePosition>(
         "SpatialMemoryWriter")
      .each([this](flecs::entity entity, SpatialMemory &memory,
                   const Vision &vision, const GamePosition &origin) {
        // Nothing to write until the sampler has run, and no writing twice for
        // one sample -- the sweep is the event, not the frame.
        if (!map || vision.lastVisible.empty() || vision.lastSampleTick < 0 ||
            vision.lastSampleTick == memory.lastVisionTick) {
          return;
        }

        const int tick = vision.lastSampleTick;
        Memory::EnsureOrigin(memory, origin, tick);
        Memory::ObserveCells(memory, *map, vision.lastVisible, tick);
        Memory::ObserveObjects(memory, ecs, vision.lastVisible, entity, tick);
        Memory::EnforceObjectCap(memory);
        memory.lastVisionTick = tick;
      });

  // Places are derived from the cells the writer just produced (section 3.3).
  //
  // A separate system rather than a call inside the writer, because the two are
  // different jobs: the writer records what was seen, the deriver decides what it
  // belongs to. Keeping them apart is what lets the derivation be re-run, and
  // re-read, without re-observing anything.
  ecs.system<SpatialMemory, const Vision, const GamePosition>(
         "SpatialMemoryPlaces")
      .each([this](flecs::entity, SpatialMemory &memory, const Vision &vision,
                   const GamePosition &position) {
        if (vision.lastSampleTick < 0) {
          return;
        }
        // Once per sample, not once per frame: derivation and consolidation are
        // both more expensive than a frame's worth of work, and neither has
        // anything new to say until the sweep runs again.
        if (memory.lastPlaceTick >= vision.lastSampleTick) {
          return;
        }
        const PlaceCategoryRegistryResource *resource =
            ecs.get<PlaceCategoryRegistryResource>();
        if (resource == nullptr || resource->registry == nullptr) {
          return;  // no taxonomy: places cannot form
        }
        const auto started = std::chrono::steady_clock::now();
        Memory::DerivationProfile profile;
        Memory::DerivePlaces(memory, *resource->registry,
                             vision.lastSampleTick, &profile);
        const auto afterPlaces = std::chrono::steady_clock::now();
        if (Settings().regions.enabled) {
          Memory::DeriveRegions(
              memory, *resource->registry, position, vision.lastSampleTick,
              Settings().regions.nearSize, Settings().regions.nearRadius,
              Settings().regions.midSize, Settings().regions.midRadius,
              Settings().regions.farSize, Settings().regions.farRadius,
              Settings().regions.maxRegions, &profile);
        }
        const auto afterRegions = std::chrono::steady_clock::now();
        // Consolidation runs after derivation because a run is grouped by place:
        // it needs this sample's membership, not last sample's.
        Memory::Consolidate(memory, vision.lastSampleTick);
        // Reported only when it can be felt. This is the number to quote when a
        // frame-rate question turns into a guess: 16 ms is one frame at 60 Hz.
        // The split says which pass to look at; the sub-splits say which step of
        // that pass. Values are whole milliseconds.
        const auto finished = std::chrono::steady_clock::now();
        const auto ms = [](auto begin, auto end) {
          return static_cast<int>(
              std::chrono::duration<double, std::milli>(end - begin).count());
        };
        const int elapsed = ms(started, finished);
        if (elapsed > 8 && debugLog) {
          debugLog->LogWarning(
              "Spatial memory: sample pass took " + std::to_string(elapsed) +
              " ms (derivePlaces " + std::to_string(ms(started, afterPlaces)) +
              " [seed " + std::to_string(static_cast<int>(profile.placesSeedMs)) + ", cluster " +
              std::to_string(static_cast<int>(profile.placesClusterMs)) + " (union " +
              std::to_string(static_cast<int>(profile.placesUnionMs)) + ", flood " +
              std::to_string(static_cast<int>(profile.placesFloodMs)) + "), build " +
              std::to_string(static_cast<int>(profile.placesBuildMs)) + ", aspects " +
              std::to_string(static_cast<int>(profile.placesAspectsMs)) + ", facts " +
              std::to_string(static_cast<int>(profile.placesFactsMs)) + "]; deriveRegions " +
              std::to_string(ms(afterPlaces, afterRegions)) + " [cells " +
              std::to_string(static_cast<int>(profile.regionsCellsMs)) + ", build " +
              std::to_string(static_cast<int>(profile.regionsBuildMs)) + " (aspects " +
              std::to_string(static_cast<int>(profile.regionsAspectsMs)) + ", facts " +
              std::to_string(static_cast<int>(profile.regionsFactsMs)) + ", evict " +
              std::to_string(static_cast<int>(profile.regionsEvictMs)) + "), " +
              std::to_string(static_cast<int>(profile.regionsAspectRebuilds)) + "/" +
              std::to_string(static_cast<int>(profile.regionsRefreshed)) +
              " regions rebuilt]; consolidate " +
              std::to_string(ms(afterRegions, finished)) + " ms; " +
              std::to_string(memory.cells.size()) + " cells, " +
              std::to_string(static_cast<int>(profile.placesSeeds)) + " seeds, " +
              std::to_string(memory.places.size() + memory.regions.size()) +
              " places, " + std::to_string(memory.runs.size()) + " runs)");
        }
      });
}

// Writes one character's record to logs/ on demand (section 7.3). The file name
// carries the run's start time for the same reason an NPC transcript does: two
// runs must be comparable, not overwrite each other.
void Game::DumpSpatialMemory(flecs::entity entity, bool includeCells) {
  if (!entity.is_alive()) {
    return;
  }
  const SpatialMemory *memory = entity.get<SpatialMemory>();
  if (memory == nullptr) {
    if (debugLog) debugLog->LogWarning("Spatial memory dump: entity has no SpatialMemory.");
    return;
  }

  // The player names its entity but carries no DisplayName (only NPCs are given
  // one at spawn), so the fallback chain ends at the entity name before the file
  // is named "...-character-memory.json".
  std::string name = "character";
  if (entity.has<DisplayName>()) {
    name = entity.get<DisplayName>()->name;
  } else {
    // flecs::string_view is not a std::string and has no empty(); c_str() is the
    // only thing it offers that composes.
    const std::string entityName = entity.name().c_str();
    if (!entityName.empty()) {
      name = entityName;
    }
  }

  // JSON, not prose: the window renders the same string, so the file you diff and
  // the record you inspected are the same artifact (section 7.3).
  const std::string agentKey = LogPaths::AgentKey(name, entity.id());
  const std::string path =
      LogPaths::NpcFile(name, entity.id(),
                        TimeUtils::GetStartTimeString() + "-" + agentKey +
                            "-memory.json")
          .string();
  std::ofstream out(path);
  if (!out.is_open()) {
    if (debugLog) debugLog->LogError("Spatial memory dump: cannot open " + path);
    return;
  }
  out << Memory::DumpJson(*memory, name, CurrentTaskTick(ecs), includeCells);
  out.close();

  if (debugLog) debugLog->LogInfo("Spatial memory dumped to " + path);
}
