// GameECSInit.cpp — Game::ECSInit: the world-start orchestration.
//
// Part of the GameECS group (see src/GameECSInternals.h). Loads content, registers
// hooks and the AI command DSL, calls LoadMap, wires the AI backends, registers
// the ECS systems and creates the player/window entities. The systems themselves
// live in GameECS.cpp and GameECSInitSystems.cpp.

#include "AI.h"
#include "OllamaAI.h"
#include "GeminiAI.h"
#include "OpenRouterAI.h"
#include "ActorArt.hpp"
#include "AsciiArtDraw.hpp"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
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

namespace {

std::string TrimCopy(const std::string &text) {
  size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
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

// Builds one AI backend from a settings block.
//
// Called twice in ECSInit: once for the NPC-facing text backend ("model") and
// once for the Vision Chat window's own backend ("vision"). The two are built by
// the same code on purpose -- a second hand-rolled copy is how the vision backend
// would silently stop honouring temperature or a new provider.
static std::unique_ptr<AI> BuildAIBackend(
    const GameSettings::ModelSection &modelSettings, DebugLog *debugLog) {
  std::unique_ptr<AI> ai;

  if (modelSettings.type == "gemini") {
    ai = std::make_unique<GeminiAI>(modelSettings.apiKey, modelSettings.model);
  } else if (modelSettings.type == "openrouter") {
    auto openrouter =
        std::make_unique<OpenRouterAI>(modelSettings.apiKey, modelSettings.model);
    if (modelSettings.hasTemperature) {
      openrouter->setOption("temperature", modelSettings.temperature);
    }
    if (modelSettings.hasMaxTokens) {
      openrouter->setOption("max_tokens", modelSettings.maxTokens);
    }
    if (modelSettings.hasProvider) {
      openrouter->setOption("provider", modelSettings.provider);
    }
    ai = std::move(openrouter);
  } else {
    auto ollama =
        std::make_unique<OllamaAI>(modelSettings.model, modelSettings.endpoint);
    if (modelSettings.hasTemperature) {
      ollama->setOption("temperature", modelSettings.temperature);
    }
    // Empty means "let Ollama decide", so a text backend is unaffected unless the
    // settings file asks for something.
    ollama->setKeepAlive(modelSettings.keepAlive);
    ai = std::move(ollama);
  }

  if (ai) {
    // A hosted backend with no key does not fail at construction: the request
    // still goes out, as `Authorization: Bearer ` with nothing after it, and the
    // provider answers 401 -- OpenRouter spells it "Missing Authentication
    // header". That arrives as an empty completion, so the symptom is a silent
    // empty reply and one line of API JSON in the debug log, which is not where
    // anyone looks for a settings mistake. Name the cause while the startup log
    // is still the obvious place to look.
    if (modelSettings.apiKey.empty() &&
        (modelSettings.type == "openrouter" || modelSettings.type == "gemini")) {
      const std::string err =
          "The " + modelSettings.type + " backend for model '" +
          modelSettings.model +
          "' has no apiKey, so the provider will reject every request. Set "
          "model.apiKey in settings.local.json (or give the block its own key).";
      if (debugLog) {
        debugLog->LogError(err);
      } else {
        std::cerr << err << std::endl;
      }
    }
    ai->setErrorLogger([debugLog](const std::string &err) {
      if (debugLog) {
        debugLog->LogError(err);
      } else {
        std::cerr << err << std::endl;
      }
    });
  }

  return ai;
}

void Game::ECSInit() {
  // Everything the run starts from comes out of the settings file; see
  // Settings.h. Held by reference so there is one source of truth.
  const GameSettings &settings = Settings();

  ecs.import <flecs::monitor>();
  // The REST server serves the live ECS world over unauthenticated HTTP. Handy
  // locally, so it stays on by default, but it is a setting because a shared
  // machine should be able to turn it off.
  if (settings.debug.restServer) {
    ecs.set<flecs::Rest>({});
  }

  objectFactory.SetDebugLog(debugLog.get());
  objectFactory.LoadTemplates(settings.content.objects);
  // Then the generated overlay, additively and after the catalog, exactly as the tile
  // overlay is loaded after the tile catalog -- so hand-authored content wins and a
  // created object survives a restart.
  //
  // The component registration has to come first, and calling it here rather than
  // reordering the whole of ECSInit is deliberate. Validating an overlay entry runs the
  // real component setters on a scratch entity, and flecs validates a component type by
  // name and size -- so a scratch world that registers `Trains` before this world does
  // makes this world's own `RegisterComponents` below abort with "already registered with
  // a different size or alignment". Loading the overlay is what needs the metadata, so
  // the metadata is what moves up. Registering twice is harmless; skipping it is a crash.
  //
  // Before the resource is published, because anything reading ObjectFactoryResource
  // would otherwise see a factory that is about to change under it.
  {
    RegisterComponents(ecs);
    const size_t generated = ObjectDesigner::LoadOverlayDirectory(
        settings.content.generatedObjects, objectFactory, debugLog.get());
    if (generated > 0) {
      debugLog->LogInfo("Object overlay: loaded " + std::to_string(generated) +
                        " created object(s)");
    }
  }
  ecs.set<ObjectFactoryResource>({&objectFactory});

  recipeRegistry.SetDebugLog(debugLog.get());
  recipeRegistry.LoadRecipes(settings.content.recipes);
  ecs.set<RecipeRegistryResource>({&recipeRegistry});

  statRegistry.SetDebugLog(debugLog.get());
  statRegistry.LoadStats(settings.content.stats);
  ecs.set<StatRegistryResource>({&statRegistry});

  skillRegistry.SetDebugLog(debugLog.get());
  skillRegistry.LoadSkills(settings.content.skills);
  ecs.set<SkillRegistryResource>({&skillRegistry});

  itemRegistry.SetDebugLog(debugLog.get());
  itemRegistry.LoadItems(settings.content.items);
  // Designer-authored equipment loads after the catalog, so deleting a generated file
  // reverts to the hand-authored item it was written over.
  itemRegistry.LoadItems(settings.content.generatedItems);
  ecs.set<ItemRegistryResource>({&itemRegistry});

  // The factory seeds per-instance durability from the item definitions, so it
  // needs the registry. Handed over here rather than pulled in at spawn time so
  // the factory keeps no dependency on the item system.
  objectFactory.SetItemRegistry(&itemRegistry);

  // Sprites are resolved while a template is applied, so the factory needs the
  // texture cache for the same reason it needs the item registry: injected rather
  // than reached for, to keep the dependency one-way.
  objectFactory.SetResourceManager(&resourceManager);

  raceRegistry.SetDebugLog(debugLog.get());
  raceRegistry.LoadRaces(settings.content.races);

  // The actor-sprite catalogue. Independent of races: a sprite says what something looks
  // like, not what body it wears.
  actorSpriteRegistry.SetDebugLog(debugLog.get());
  actorSpriteRegistry.Load(settings.content.actorSprites);
  ecs.set<RaceRegistryResource>({&raceRegistry});

  // Portraits are content like races, and the HUD is the only consumer. Loaded
  // before the map so a character spawned during LoadMap can be given its id
  // without the registry existing twice; an unknown id is the unknown sprite, so
  // this is not a hard dependency either way.
  portraitRegistry.SetDebugLog(debugLog.get());
  portraitRegistry.LoadPortraits(settings.content.portraits);

  // Rigged actor bodies, beside the single-image actor sprites rather than inside them:
  // a rig is a composition, and a wolf does not need one.
  rigRegistry.SetDebugLog(debugLog.get());
  rigRegistry.Load(settings.content.rigs);
  // Agent-authored rigs load after the catalog, so deleting one reverts to the
  // hand-authored body it was written over.
  rigRegistry.Load(settings.content.generatedRigs);

  // Named NPC definitions and the brains that drive them, loaded before the map so
  // a map entry can name one at spawn. Brains are loaded first: a template may
  // reference one.
  brainTemplateRegistry.SetDebugLog(debugLog.get());
  brainTemplateRegistry.Load(settings.content.brains);
  npcTemplateRegistry.SetDebugLog(debugLog.get());
  npcTemplateRegistry.Load(settings.content.npcs);

  // A template naming content the game does not have would fail one character at
  // a time at spawn. Report it once, here, where the template's id is still in
  // hand -- the same reason the projectile check below exists.
  {
    auto warn = [this](const std::string& message) {
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    };
    for (const NpcTemplate* definition : npcTemplateRegistry.GetAll()) {
      if (!definition->race.empty() &&
          raceRegistry.Get(definition->race) == nullptr) {
        warn("Warning: NPC template '" + definition->id +
             "' names unknown race '" + definition->race + "'");
      }
      if (!definition->sprite.empty() &&
          actorSpriteRegistry.Get(definition->sprite) == nullptr) {
        warn("Warning: NPC template '" + definition->id +
             "' names unknown actor sprite '" + definition->sprite + "'");
      }
      if (!definition->portrait.empty() &&
          portraitRegistry.Get(definition->portrait) == nullptr) {
        warn("Warning: NPC template '" + definition->id +
             "' names unknown portrait '" + definition->portrait + "'");
      }
      if (!definition->brain.empty() &&
          brainTemplateRegistry.Get(definition->brain) == nullptr) {
        warn("Warning: NPC template '" + definition->id +
             "' names unknown brain '" + definition->brain + "'");
      }
    }
    for (const BrainTemplate* brain : brainTemplateRegistry.GetAll()) {
      if (!brain->modelProfile.empty() &&
          Settings().ai.profiles.count(brain->modelProfile) == 0) {
        warn("Warning: brain '" + brain->id + "' names unknown ai profile '" +
             brain->modelProfile + "'");
      }
    }
  }

  // Projectiles are the delivery half of a ranged skill: sprite, speed and size
  // only. The impact stays on the skill's activation.
  projectileRegistry.SetDebugLog(debugLog.get());
  projectileRegistry.LoadProjectiles(settings.content.projectiles);
  ecs.set<ProjectileRegistryResource>({&projectileRegistry});

  // A skill naming a projectile nobody defines would look like a broken spell at
  // use time. Report it once here, where the skill's id is still available.
  for (const SkillDef *def : skillRegistry.GetAll()) {
    if (def->activation.projectile.empty()) {
      continue;
    }
    if (projectileRegistry.Get(def->activation.projectile) == nullptr) {
      const std::string message =
          "Warning: skill '" + def->id + "' names projectile '" +
          def->activation.projectile +
          "', which the projectile registry does not define; the skill will "
          "strike instantly instead.";
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    }
  }

  // Skillshots are the aimed delivery half: a sampled path plus the visual. The
  // skill owns range and impact; the registry owns only how the path looks and
  // moves.
  skillshotRegistry.SetDebugLog(debugLog.get());
  skillshotRegistry.LoadSkillshots(settings.content.skillshots);
  ecs.set<SkillshotRegistryResource>({&skillshotRegistry});

  for (const SkillDef *def : skillRegistry.GetAll()) {
    if (def->activation.skillshot.empty()) {
      continue;
    }
    if (skillshotRegistry.Get(def->activation.skillshot) == nullptr) {
      const std::string message =
          "Warning: skill '" + def->id + "' names skillshot '" +
          def->activation.skillshot +
          "', which the skillshot registry does not define; the skill will "
          "fizzle when used.";
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    }
  }

  // The place vocabulary. Loaded before LoadMap for the same reason the tile
  // catalog is: nothing in a map's content may depend on it being absent, and a
  // missing file must be a warning rather than a crash (places simply never form).
  placeCategoryRegistry.SetDebugLog(debugLog.get());
  placeCategoryRegistry.Load(settings.content.places);
  ecs.set<PlaceCategoryRegistryResource>({&placeCategoryRegistry});

  // The vocabulary and the templates load independently, so a template naming a
  // category the taxonomy does not define would be silent. Reported once, here,
  // with both names in the message.
  for (const auto &[templateKey, tmpl] : objectFactory.GetTemplates()) {
    if (!tmpl.contains("placeCategories") ||
        !tmpl["placeCategories"].is_object()) {
      continue;
    }
    for (auto it = tmpl["placeCategories"].begin();
         it != tmpl["placeCategories"].end(); ++it) {
      if (!placeCategoryRegistry.IsKnown(it.key()) && debugLog) {
        debugLog->LogWarning("Object template '" + templateKey +
                             "' uses unknown place category '" + it.key() +
                             "'.");
      }
    }

    // ApplyTemplate runs a setter only when the template's "components" array
    // names it, so a placeCategories key without "PlaceCategories" in that array
    // is silently inert -- the component is never attached and the seed never
    // exists. Reported here because it is the one mistake the data cannot catch
    // on its own.
    bool declared = false;
    if (tmpl.contains("components") && tmpl["components"].is_array()) {
      for (const auto &component : tmpl["components"]) {
        if (component.is_string() &&
            component.get<std::string>() == "PlaceCategories") {
          declared = true;
          break;
        }
      }
    }
    if (!declared && debugLog) {
      debugLog->LogWarning("Object template '" + templateKey +
                           "' has placeCategories but does not declare the "
                           "'PlaceCategories' component; it will seed nothing.");
    }
  }

  // The task shapes the engine knows how to check. Engine-defined rather than
  // loaded from JSON, because a check is C++ (docs/task-system-design.md section
  // 7): this is the place a genre adds one, in code. Seeded before LoadMap so
  // every agent spawned by it can already resolve a kind.
  taskKindRegistry.RegisterDefaults();
  ecs.set<TaskKindRegistryResource>({&taskKindRegistry});

  // The world's tile vocabulary. Loaded before LoadMap, because Map resolves its
  // tileInfo entries against it while building uniqueTiles -- a map's tileInfo is
  // an override set, not the sole definition. With no catalog present this is a
  // warning and maps stay self-sufficient.
  tileRegistry.SetDebugLog(debugLog.get());
  tileRegistry.LoadTiles(settings.content.tiles);

  // Then the generated overlay: agent-created tiles, additive on top of the
  // catalog and never inside it (docs/asset-generation-design.md sections 4.3 and
  // 4.5). It has to happen here rather than in any window -- a map loaded after
  // this resolves overlay names from the palette exactly as it resolves catalog
  // ones, and Map::CreateTile can only create what is already in the catalog.
  // A missing directory is the normal case and is not reported.
  {
    const size_t generated = TileDesigner::LoadOverlayDirectory(
        settings.content.generatedTiles, tileRegistry, debugLog.get());
    if (generated > 0 && debugLog) {
      debugLog->LogInfo("Tile overlay: loaded " + std::to_string(generated) +
                        " generated tile definition(s) from " +
                        settings.content.generatedTiles);
    }
  }
  ecs.set<TileRegistryResource>({&tileRegistry});

  // The Content Editor's overrides, last of all: a human edit outranks both the catalog
  // and the agent-generated overlay, so whatever a person decided is what the world runs.
  // Splitting this into its own pass rather than folding it into the overlay loaders keeps
  // the precedence readable in one place -- catalog, generated, editor -- instead of
  // hidden in two loaders that each know about their own layer.
  LoadContentOverrides();

  // Building designs. Loaded after the tile catalog and the object factory
  // because it validates every design's materials against both: a "paint" naming
  // a ground tile the world has never heard of would otherwise build invisible
  // ground, which reads exactly like a working design that places nothing.
  blueprintRegistry.SetDebugLog(debugLog.get());
  blueprintRegistry.SetTileRegistry(&tileRegistry);
  blueprintRegistry.SetObjectFactory(&objectFactory);
  blueprintRegistry.LoadBlueprints(settings.content.blueprints);
  ecs.set<BlueprintRegistryResource>({&blueprintRegistry});

  // Stage 0 of docs/asset-generation-design.md: make the art gaps visible rather
  // than implied. A summary line only -- the report is derived entirely from the
  // content, so a session cannot change it and there is nothing to gain by
  // rewriting the file on every launch. `AIRogue --gaps` writes it on demand, and
  // that is also the version a script or the generator can call.
  {
    const GapLog::Report gapReport =
        GapLog::Build(&tileRegistry, &objectFactory, &blueprintRegistry);
    if (debugLog) {
      debugLog->LogInfo(GapLog::Summary(gapReport));
    }
  }

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
    .format = "[WAIT $SECONDS]",
    .rule = "Wait before doing anything else. Examples: [WAIT 5], [WAIT 20].",
    .pattern = std::regex(R"(\[WAIT\s+(\d+(?:\.\d+)?)\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      float time = std::stof(match[1].str());
      return [time](flecs::entity) { return std::make_unique<WaitAction>(time); };
    },
    .help = {
      .description = "Pause and let time pass before you act again. Use it when the thing you are waiting for is not ready yet -- a crop that still has to grow, someone who is on their way -- instead of repeating a command that cannot succeed yet.",
      .examples = {"[WAIT 5]", "[WAIT 20]"},
      .usage = {"$SECONDS: how long to wait, in seconds. Fractions are allowed, e.g. [WAIT 0.5]."},
      .notes = {"You can do nothing else while you wait, so a long wait also delays every other plan you are holding."}
    }
  });

  // [MOVE_TO] is the verb for a target the agent can already name. In inline
  // navigation the verb for a step toward somewhere it cannot name is [GO], and
  // [NAVIGATE] is not part of the vocabulary, so the advertised line and the
  // help point at [GO] instead. The wording is chosen once at registration
  // because the mode is a restart-time setting.
  const bool inlineNavigation = Settings().navigation.agent == "inline";

  GlobalCommandRegistry::Register({
    .format = "[MOVE_TO $TARGET]",
    .rule = inlineNavigation
        ? "Go directly to one named target you already know: a character, an object, or a remembered place. Use it when that target is the destination -- it is one move, not a search. To walk toward somewhere you cannot name, use [GO] with a key from your spatial block. Examples: [MOVE_TO Pietro], [MOVE_TO Mill]."
        : "Go directly to one named target you already know: a character, an object, or a remembered place. Use it when that target is the destination -- it is one move, not a search. If your goal is beyond the target (a road you can see, a path), use [NAVIGATE <goal>] instead. Examples: [MOVE_TO Pietro], [MOVE_TO Mill].",
    .pattern = std::regex(R"(\[MOVE_TO\s+(.+?)\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string targetName = match[1].str();
      return [targetName](flecs::entity) { return std::make_unique<MoveToEntityAction>(targetName); };
    },
    .help = {
      .description = inlineNavigation
          ? "Walk to a named character, object or place you remember, pathfinding around whatever is in the way. This is one move to a target you already know: use it when the target is where you want to end up, or a known waypoint you have already chosen. It is not a search -- for a step toward somewhere you cannot name, use [GO] with a key from your spatial block. Arriving at an object lists the interactions it offers; arriving at a place stands you in it."
          : "Walk to a named character, object or place you remember, pathfinding around whatever is in the way. This is one move to a target you already know: use it when the target is where you want to end up, or a known waypoint you have already chosen. It is not a search -- if the goal is beyond the target, use [NAVIGATE]. Arriving at an object lists the interactions it offers; arriving at a place stands you in it.",
      .examples = {"[MOVE_TO Pietro]", "[MOVE_TO Mill]"},
      .usage = {"$TARGET: a name from [PLACES] or [CHARACTERS]. An unnamed remembered place must first be given a name with [NAME_PLACE #<number> <name>]; a [PLACES] number on its own is not a target. Matching ignores case."},
      .notes = {"Arriving at an object lists the interactions it offers and remembers it as your interaction target.",
                "Directions such as \"east\" and bare coordinates are not valid targets.",
                inlineNavigation
                    ? "If the name matches nothing, use [PLACES] + [NAME_PLACE] for a remembered place, or [GO] with a key from your spatial block for a step toward somewhere you cannot name."
                    : "If the name matches nothing, the failure names the two ways forward: [PLACES] + [NAME_PLACE] for a remembered place, or [NAVIGATE <goal>] for a goal you cannot name yet.",
                inlineNavigation
                    ? "Use [GO] when the place you want is not one you can name yet; [MOVE_TO] stops at the named target."
                    : "Use [NAVIGATE] when a target you can name or see (a road, a path) is only a step toward a goal beyond it; [MOVE_TO] stops at that target."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[LOOK $DIRECTION]",
    .rule = "Turn on the spot and report what you can now see, or turn a full circle to look all around. Sight is a cone in front of you. Examples: [LOOK north], [LOOK east], [LOOK around].",
    .pattern = std::regex(R"(\[LOOK\s+(.+?)\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string direction = match[1].str();
      return [direction](flecs::entity) { return std::make_unique<LookAction>(direction); };
    },
    .help = {
      .description = "Turn to face a direction without moving, and be told what is in sight that way. Sight is a cone in front of you, so turning is how you look along a road or across a clearing before deciding to walk -- at a junction, the only way to see the other arm. Use [LOOK around] for a full circle: it describes every compass direction at once and is what to use when you need to get your bearings.",
      .examples = {"[LOOK north]", "[LOOK east]", "[LOOK around]"},
      .usage = {"$DIRECTION: north, south, east, west, or a diagonal such as north-east. Or 'around' for a full circle."},
      .notes = {"Standing still keeps your last facing; walking overrides it. A full circle leaves your facing where it was.",
                "What you see is also remembered, so a direction you looked at appears in [PLACES] afterwards.",
                "The report only describes what your own eyes reached: distance, darkness and anything that blocks sight still limit it."}
    }
  });

  // [GO] -- one step of continuous navigation (navigation.agent = "inline").
  //
  // Hidden, because the spatial block in the wake prompt prints the exact keys
  // offered for the place the character is standing in. Advertising a generic
  // [GO $STEP] in every mode's command list would hand the agent a vocabulary
  // it cannot usefully apply there, and the block is the only place the valid
  // keys are ever stated.
  GlobalCommandRegistry::Register({
    .format = "[GO $STEP]",
    .rule = "Take one step from the spatial block: a compass run, a remembered place, follow, back or to:<n>.",
    .pattern = std::regex(R"(\[GO\s+([^\]]+?)\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) {
      std::string key = match[1].str();
      return [key](flecs::entity) -> std::unique_ptr<AgentAction> {
        return std::make_unique<GoAction>(key);
      };
    },
    .hidden = Settings().navigation.agent != "inline",
    .help = {
      .description = "Take one step from the spatial state at the top of your context: a compass run within the place you are standing in ([GO here:north]), a remembered place ([GO p17]), a linear feature underfoot ([GO follow] or [GO follow:east]), the step you just walked ([GO back]), or a step by its number in the map targets ([GO to:3]). The step walks one leg and hands the turn back to you; it is not a journey, and nothing continues without you. Keys are resolved against what you remember now, so use them exactly as the spatial block printed them: a key that is no longer offered fails and names the steps that are offered instead.",
      .examples = {"[GO here:north]", "[GO p17]", "[GO follow:east]", "[GO back]", "[GO to:3]"},
      .usage = {"$STEP: one key from the spatial block, e.g. here:north, p17, follow, follow:east, back or to:3."},
      .notes = {"[MOVE_TO] is for a character, object or place whose name you already know -- your farm, the market, a person. [GO] is for the steps the engine offers from where you stand, including stretches of road that have no name.",
                "A here:<direction> step is one compass run -- often several tiles -- not a single tile. follow goes further: it stays on the feature through forced turns to the next junction or end.",
                "You do not have to move: your other commands work on the same turn."}
    }
  });

  // [WATCH] -- the standing attention brief for continuous navigation. Hidden
  // for the same reason as [GO]: the spatial block is where it is advertised,
  // and the block echoes whatever is set. A bare [WATCH] clears the brief.
  GlobalCommandRegistry::Register({
    .format = "[WATCH $WHAT]",
    .rule = "Set what should make you stop and look while you walk; [WATCH] alone clears it.",
    .pattern = std::regex(R"(\[WATCH(?:\s+([^\]]*?))?\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) {
      std::string what;
      if (match.size() > 1 && match[1].matched) what = match[1].str();
      return [what](flecs::entity) -> std::unique_ptr<AgentAction> {
        return std::make_unique<WatchAction>(what);
      };
    },
    .hidden = Settings().navigation.agent != "inline",
    .help = {
      .description = "State what should make you stop and look while you walk: a signpost, water, a building, a person. It is your standing attention brief, echoed back to you in the spatial block each turn, and it holds until you replace it. A bare [WATCH] clears it. It is a priority, not a filter: something obviously worth your attention can still stop you even when it is not in the brief.",
      .examples = {"[WATCH look for a village, a signpost or water]", "[WATCH]"},
      .usage = {"$WHAT: a short phrase, e.g. look for a signpost or a village. Leave it out to clear."},
      .notes = {"Nothing is filtered out: the brief says what is worth interrupting for, and everything you see is still remembered and listed in your spatial state."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[NAVIGATE $GOAL]",
    .rule = "Search toward a goal you cannot pinpoint yet (find the village, follow a rumour). A strategist plans from what you have observed and a small pilot chooses each next move: a direction within the place you are standing in, or a remembered place. Use it when the goal is not a place you can name or locate -- a visible road or path is a step, not the goal. [MOVE_TO] is only for a target that is the destination. Example: [NAVIGATE find a village with a market].",
    .pattern = std::regex(R"(\[NAVIGATE\s+(.+?)\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string goal = match[1].str();
      return [goal](flecs::entity) -> std::unique_ptr<AgentAction> {
        const std::string mode = Settings().navigation.agent;
        if (mode == "inline") {
          return std::make_unique<NoticeAction>(
              "System: Navigation is continuous in this mode: you are always "
              "where you are, and each move is [GO <key>] from the spatial "
              "block. There is no separate journey to start or stop.\n");
        }
        if (mode == "single") {
          return std::make_unique<SingleNavigateAction>(goal);
        }
        return std::make_unique<NavigateAction>(goal);
      };
    },
    .hidden = Settings().navigation.agent == "inline",
    .help = {
      .description = "Use this for high-level travel: when you know what you are looking for but not where it is or which direction to take -- finding a village, following a signpost's rumour to a distant place, or looking for somewhere quiet to sleep when nothing you remember obviously matches. A strategist plans from your observed surroundings and may take several seconds; a small pilot then picks the most likely next move, walks one leg, and decides again on arrival. The strategist also asks the pilot a few yes/no questions (a signpost, a junction, a dead end, water) and re-plans when one of them says the situation has changed. A move is either a direction within the place you are standing in (follow the road north, cross the orchard east) or a remembered place. The compass is what lets it keep following a road or leave it deliberately, instead of only jumping between remembered places. It is not for a target you can name that is itself the destination: use [MOVE_TO] for that. Local knowledge of the next few tiles -- a road or path you can see -- does not make the goal known; if the named target is only a step toward the goal, this is the verb. It stops when it judges that it has arrived, when the strategist gives up, when the strategist call budget is exhausted, or when a resource guard (the episode's wall clock or a stuck leg) is reached; a pilot that cannot decide or has no remembered move hands back to the strategist instead of ending the navigation. It then tells you where you are, what places are nearby, and the strategy it used. Both models only see your own memory, so either can be wrong.",
      .examples = {"[NAVIGATE find a village with a market]",
                   "[NAVIGATE the village the signpost mentioned; it said north]",
                   "[NAVIGATE somewhere quiet to sleep]"},
      .usage = {"$GOAL: a short phrase describing the distant or uncertain goal you are searching for, e.g. a village with a market or the village a signpost mentioned. Name the goal, not the next visible step; if the destination itself is a place you already know, use [MOVE_TO] for it. What you learned from a sign or a person can be part of it."},
      .notes = {"Use [MOVE_TO] when the named target is the destination. If the goal is beyond a target you can name or see (a road, a path), that target is a step: use [NAVIGATE]. Every leg costs a decision, so a direct trip to a known destination is cheaper.",
                "Only places, directions, objects and relations you have observed are in the strategist's brief; nothing is revealed that you have not seen. Your goal and task are the only hints it gets; it does not read your conversation or the map.",
                "The strategist's questions are answered by the local pilot from the observation; an uncertain answer hands back to the strategist, which may re-plan or give up.",
                "A place can be unnamed; the model chooses it by the number you saw in [PLACES]. Local compass moves are shown as here:<direction>.",
                "When the pilot is unsure it turns a full circle once to look for a better lead; if that does not settle it, the strategist is asked again rather than guessing.",
                "If the strategist is unavailable, the last accepted strategy is kept (its monitors are dropped) and the local pilot continues; the strategist is asked again on the pilot's next uncertainty. There is no per-episode decision cap and no back-and-forth stop: a repeated target is reported to the strategist, which decides. A pilot that cannot choose/has no remembered move asks the strategist; the episode stops without arriving only on a strategist give-up, an exhausted strategist call budget, or a resource guard (wall clock or stuck leg)."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[TALK_TO $TARGET]",
    .rule = "Start or continue a conversation with a character. Example: [TALK_TO Pietro].",
    .pattern = std::regex(R"(\[TALK_TO\s+(.+?)\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string targetName = match[1].str();
      return [targetName](flecs::entity entity) { return std::make_unique<TalkAction>(entity, targetName); };
    },
    .help = {
      .description = "Begin or continue a conversation with another character. Conversation is where person-to-person exchanges happen, so most social plans start here.",
      .examples = {"[TALK_TO Pietro]"},
      .usage = {"$TARGET: the exact name of a character, as printed by [CHARACTERS] or seen with [LOOK]."},
      .notes = {"Trade verbs such as [OFFER] and [ACCEPT] only work while a conversation is active."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[CHARACTERS]",
    .rule = "List the characters nearby. Example: [CHARACTERS].",
    .pattern = std::regex(R"(\[CHARACTERS\])", std::regex_constants::icase),
    .createAction = [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<CharactersAction>(); };
    },
    .help = {
      .description = "List the other characters by name. Read-only, so it is safe to check whenever you need to remember who is in the world.",
      .examples = {"[CHARACTERS]"},
      .usage = {"Takes no arguments."},
      .notes = {"The names printed are the exact ones [TALK_TO] and [MOVE_TO] match on."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[PLACES]",
    .rule = "List the places you remember, nearest first, with what you think each is like. Example: [PLACES].",
    .pattern = std::regex(R"(\[PLACES\])", std::regex_constants::icase),
    .createAction = [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<PlacesAction>(); };
    },
    .help = {
      .description = "List the places you have seen and what you remember about them: their names if you have any, their aspects, and how far away they are. This is your own memory, not a map -- two characters can remember the same ground differently, and a name you were told may not match what you see.",
      .examples = {"[PLACES]"},
      .usage = {"Takes no arguments."},
      .notes = {"These are the place names [MOVE_TO] accepts, matched case-insensitively.",
                "A place you have never observed is not on this list, however well known it is to others.",
                "What a place is like is evaluated where you stand: a market beside an orchard is more orchard at one end."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[NAME_PLACE here $NAME] or [NAME_PLACE #$ID $NAME]",
    .rule = "Give a place a name. Naming the spot you stand on, or a place by its number from [PLACES]. Example: [NAME_PLACE here Market Square].",
    .pattern = std::regex(R"(\[NAME_PLACE\s+(here|#?\d+)\s+(.+?)\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string target = match[1].str();
      std::string label = match[2].str();
      return [target, label](flecs::entity) {
        return std::make_unique<NamePlaceAction>(target, label);
      };
    },
    .help = {
      .description = "Give a place a name you chose. Naming where you stand puts the name on the place covering you, or makes a small place if nothing does. A place keeps every name it is given, so two people can call the same ground different things.",
      .examples = {"[NAME_PLACE here Market Square]", "[NAME_PLACE #4 the old mill]"},
      .usage = {"$NAME: 1-48 characters, no brackets. The name is yours; the engine never invents one."},
      .notes = {"Names are how other characters can be told about a place, and what [MOVE_TO] matches on.",
                "Naming ground you have not observed still records the name, but no place appears until you have seen the ground."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[INVENTORY]",
    .rule = "List the items you are currently carrying. Example: [INVENTORY].",
    .pattern = std::regex(R"(\[INVENTORY\])", std::regex_constants::icase),
    .createAction = [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<InventoryAction>(); };
    },
    .help = {
      .description = "List what you are carrying, one line per distinct item, with a count when you hold more than one. Worn items are marked rather than hidden, so an equipped tool stays distinguishable from a spare in your pack.",
      .examples = {"[INVENTORY]"},
      .usage = {"Takes no arguments."},
      .notes = {"Use [EXAMINE_ITEM $ITEM_NAME] on a listed name to see what that item can do."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[TASKS]",
    .rule = "Show your own task list: your fixed life goals, your long-term goals, your short tasks with any facts the engine has observed, and the task kinds the engine can check. Example: [TASKS].",
    .pattern = std::regex(R"(\[TASKS\])", std::regex_constants::icase),
    .createAction = [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<TasksAction>(); };
    },
    .help = {
      .description = "Show your own task list. It prints the life goals you were born with (read-only), the long-term goals you authored, and the short tasks you are working through, with any facts the engine has observed and any rule warnings. It also lists the task kinds the engine can check, which is what the kind column of [SET_TASKS] accepts.",
      .examples = {"[TASKS]"},
      .usage = {"Takes no arguments."},
      .notes = {"The list is yours alone; no other character can read it.",
                "Use [SET_TASKS ...] to replace the part below your life goals.",
                "The engine may annotate a task with observed facts or a rule warning; it never changes the tier you set."}
    }
  });

  // Know-how lookups. Both are read-only queries against live content data, so
  // they grant nothing and store nothing: learning HOW is deliberately separate
  // from getting BETTER at it, which is the progression skill system.
  //
  // Registered here, with the other knowledge verbs, so they precede
  // [GENERIC_INTERACT] -- that catch-all matches [A-Z_]+ and would otherwise
  // swallow both.
  GlobalCommandRegistry::Register({
    .format = "[GUIDE $ACTIVITY]",
    .rule = "Ask how an activity is done in this world: every recipe that trains it and the full chain of ingredients down to raw materials, naming the station each step happens at. Omit the activity to list what guides exist. Examples: [GUIDE blacksmithing], [GUIDE].",
    .pattern = std::regex(R"(\[GUIDE(?:\s+(.+?))?\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string topic = match.size() > 1 ? match[1].str() : "";
      return [topic](flecs::entity) { return std::make_unique<GuideAction>(topic); };
    },
    .help = {
      .description = "Ask how an activity is practised in this world. The answer gives every recipe that trains it, the station each one happens at, and the ingredients expanded down to raw materials. Omit the activity to hear which guides exist.",
      .examples = {"[GUIDE blacksmithing]", "[GUIDE]"},
      .usage = {"$ACTIVITY: the activity to ask about, e.g. blacksmithing. Omit it to list the available guides."},
      .notes = {"Read-only: a guide tells you how something is done but trains no skill and changes nothing.",
                "To ask about a specific item rather than an activity, use [INFO $ITEM]."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[INFO $ITEM]",
    .rule = "General reference on an item TYPE: which recipe and station makes it, or, for something that is gathered rather than made, which object yields it. This describes the kind of item; to inspect one specific item you are carrying, use [EXAMINE_ITEM]. Omit the item to list what can be asked about. Examples: [INFO iron_ingot], [INFO iron_ore], [INFO].",
    .pattern = std::regex(R"(\[INFO(?:\s+(.+?))?\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string item = match.size() > 1 ? match[1].str() : "";
      return [item](flecs::entity) { return std::make_unique<ItemInfoAction>(item); };
    },
    .help = {
      .description = "Look up an item type: which recipe and station produce it, or -- for something gathered rather than crafted -- which object yields it. This describes the kind of item, not one particular copy you own.",
      .examples = {"[INFO iron_ingot]", "[INFO iron_ore]", "[INFO]"},
      .usage = {"$ITEM: the item type to ask about. Omit it to list the types that can be asked about."},
      .notes = {"To inspect one specific item you are carrying and what can be done with it, use [EXAMINE_ITEM $ITEM_NAME].",
                "The answer does not recurse; it names the ingredients so you can ask about them one step at a time."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[EXAMINE_ITEM $ITEM_NAME]",
    .rule = "See what actions you can perform with an item you are carrying. Example: [EXAMINE_ITEM Wheat].",
    .pattern = std::regex(R"(\[EXAMINE_ITEM\s+(.+?)\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string itemName = match[1].str();
      return [itemName](flecs::entity) { return std::make_unique<ExamineItemAction>(itemName); };
    },
    .help = {
      .description = "Ask what you can do with one item you are carrying. You get that item's available actions, plus how much durability it has left when it has any -- the way to find out what a tool or ingredient is good for before using it.",
      .examples = {"[EXAMINE_ITEM Wheat]", "[EXAMINE_ITEM Iron Scythe]"},
      .usage = {"$ITEM_NAME: the exact name of an item in your inventory, as printed by [INVENTORY]."},
      .notes = {"The item must be one you are holding; a name you do not carry is reported as invalid."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[EQUIP $ITEM_NAME]",
    .rule = "Wear or wield an item you are carrying, so its effects apply to you. Replaces whatever is already in the slot it needs. Example: [EQUIP Iron Scythe].",
    .pattern = std::regex(R"(\[EQUIP\s+(.+?)\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string itemName = match[1].str();
      return [itemName](flecs::entity) { return std::make_unique<EquipAction>(itemName); };
    },
    .help = {
      .description = "Put on or take up an item you are carrying so its effects apply to you. Whatever already occupies the slot it needs is swapped out and stays in your inventory.",
      .examples = {"[EQUIP Iron Scythe]"},
      .usage = {"$ITEM_NAME: the exact name of a carried item, as printed by [INVENTORY]."},
      .notes = {"If the item is not in your inventory the command reports that and nothing changes.",
                "Use [EQUIPMENT] to see what is worn and which slot it occupies."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[UNEQUIP $ITEM_NAME]",
    .rule = "Take off an item you are wearing. Example: [UNEQUIP Iron Scythe].",
    .pattern = std::regex(R"(\[UNEQUIP\s+(.+?)\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string itemName = match[1].str();
      return [itemName](flecs::entity) { return std::make_unique<UnequipAction>(itemName); };
    },
    .help = {
      .description = "Take off one worn item, leaving it in your inventory. Use it to free a slot before equipping something else.",
      .examples = {"[UNEQUIP Iron Scythe]"},
      .usage = {"$ITEM_NAME: the exact name of a worn item, as printed by [EQUIPMENT]."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[EQUIPMENT]",
    .rule = "List what you are wearing and which slot each item occupies. Example: [EQUIPMENT].",
    .pattern = std::regex(R"(\[EQUIPMENT\])", std::regex_constants::icase),
    .createAction = [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<EquipmentAction>(); };
    },
    .help = {
      .description = "List what you are currently wearing and which slot each item occupies. Read-only, so it is safe to check before and after changing gear.",
      .examples = {"[EQUIPMENT]"},
      .usage = {"Takes no arguments."},
      .notes = {"A worn item is still carried; [INVENTORY] marks it rather than hiding it."}
    }
  });

  // The agent's own task list, replaced wholesale (docs/task-system-
  // design.md section 9). Visible: the per-turn digest that used to teach the
  // verb is gone, so it is advertised in the spawn prompt like the other verbs.
  // The kind vocabulary is taught by [TASKS]. Kept with the visible verbs, and
  // before [GENERIC_INTERACT], whose "[A-Z_]+..." pattern would otherwise
  // swallow it.
  GlobalCommandRegistry::Register({
    .format = "[SET_TASKS $ENTRIES]",
    .rule = "Replace your task list (your life goals are fixed and are not part of it). "
    "One entry per line, entries separated by ';': "
    "id | description | horizon | kind | target | parent | tier, where horizon is "
    "longterm or short and tier is CRITICAL, HIGH, NORMAL, LOW or BACKGROUND. "
    "The task kinds the engine can check are listed by [TASKS].",
    .pattern = std::regex(R"(\[SET_TASKS\b([\s\S]*?)\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string payload = match.size() > 1 ? match[1].str() : "";
      return [payload](flecs::entity) { return std::make_unique<SetTasksAction>(payload); };
    },
    .help = {
      .description = "Replace your own task list wholesale. Use it to record what you are working towards and how it breaks down, so your intentions survive across turns. The list you send replaces everything below your fixed terminal goals, which are left untouched.",
      .examples = {"[SET_TASKS 3 | reap the wheat at the mill field | short | harvest | mill field | 2 | HIGH]",
                   "[SET_TASKS 2 | build standing through relationships | longterm | | | 1 | ; 3 | reap the wheat at the mill field | short | harvest | mill field | 2 | HIGH]"},
      .usage = {"$ENTRIES: one entry per line separated by ';', each written as id | description | horizon | kind | target | parent | tier.",
                "horizon is longterm or short; tier is CRITICAL, HIGH, NORMAL, LOW or BACKGROUND.",
                "Omitted parent, kind and target fields are left blank between pipes."},
      .notes = {"Parsing is tolerant: a malformed line is skipped with a warning and the rest of the list still applies.",
                "A list that cannot be read at all leaves your previous list exactly as it was.",
                "Terminal goals cannot be set this way; a terminal row is ignored with a warning."}
    }
  });

  // The vocabulary's own reference. Registered last among the visible verbs,
  // before the hidden ones and the [GENERIC_INTERACT] catch-all that would
  // otherwise swallow "[HELP ...]".
  //
  // Both forms live here: bare [HELP] reuses GlobalCommandRegistry's visible
  // list, and [HELP <COMMAND>] prints the long form registered with that verb.
  // Hidden verbs are answerable by name but never listed -- see HelpAction.
  GlobalCommandRegistry::Register({
    .format = "[HELP $COMMAND]",
    .rule = "Explain the command vocabulary: on its own it lists every command with a short description; with a command name it gives the full explanation. Examples: [HELP], [HELP MOVE_TO].",
    .pattern = std::regex(R"(\[HELP(?:\s+([^\]]*?))?\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string topic = match.size() > 1 ? match[1].str() : "";
      return [topic](flecs::entity) { return std::make_unique<HelpAction>(topic); };
    },
    .help = {
      .description = "Ask about the commands themselves. On its own it lists every command you can use, each with a short description of what it does. Given a command name it prints the full explanation: what the command is for, worked examples, what its arguments accept, and any caveats.",
      .examples = {"[HELP]", "[HELP MOVE_TO]"},
      .usage = {"$COMMAND: the verb to explain, e.g. MOVE_TO. Omit it to list every command.",
                "Only the verb matters and matching ignores case, so [HELP move_to] and [HELP MOVE_TO $TARGET] both work."},
      .notes = {"Commands taught inside a conversation, such as the trade verbs, are not listed here but can still be asked about by name."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[PLACE $ITEM AT X,Y ...]",
    .rule = "Sets an item you are carrying down in the world at one or more relative coordinates. What can be placed and where is up to the item.",
    .pattern = std::regex(R"(\[PLACE\s+(.+?)\s+AT\s+((?:-?\d+,-?\d+\s*)+)\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string itemName = match[1].str();
      std::string coordsStr = match[2].str();
      return [itemName, coordsStr](flecs::entity) { return std::make_unique<PlaceAtAction>(itemName, coordsStr); };
    },
    .hidden = true,
    .help = {
      .description = "Set one or more items you are carrying down in the world at relative coordinates. Every coordinate is checked before anything is consumed, so a rejected spot never costs you the item. What may be placed, how far away and what the placed form becomes are all decided by the item itself: planting is just placement where the placed form happens to grow.",
      .examples = {"[PLACE Wheat Seeds AT 1,0]", "[PLACE Wheat Seeds AT 1,0 -1,0 0,1]"},
      .usage = {"$ITEM: the exact name of a carried item.",
                "AT X,Y ...: one or more offsets from your own tile, +X East and +Y South, separated by spaces."},
      .notes = {"You cannot place on your own tile, outside the map, on ground the item does not accept, or on a tile something already occupies.",
                "Naming more coordinates than you hold items is not fatal: the extras are reported as \"no ... left\"."}
    }
  });

  // Trade verbs. Registered hidden so they are not dumped into every NPC's
  // spawn prompt; the protocol is taught in the conversation-start message and
  // each offer carries its own reply options. They sit before
  // [GENERIC_INTERACT] so an [OFFER ...] is never swallowed by the catch-all,
  // and they exist mainly to give a useful reply when a model uses one outside
  // a conversation.
  GlobalCommandRegistry::Register({
    .format = "[OFFER $GIVE FOR $RECEIVE]",
    .rule = "Propose a trade while talking to someone; separate multiple items with commas. Examples: [OFFER 3 Iron Ingot FOR 8 Flour], [OFFER 2 Flour, 5 Bronze Coins FOR 3 Iron Ingot].",
    .pattern = std::regex(R"(\[OFFER\b[^\]]*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<TradeOutOfContextAction>("OFFER"); };
    },
    .hidden = true,
    .help = {
      .description = "Propose a trade while you are in a conversation: what you will give, and what you want in return. Your partner is shown the offer and can accept it, decline it, or come back with a counter-offer.",
      .examples = {"[OFFER 3 Iron Ingot FOR 8 Flour]", "[OFFER 2 Flour, 5 Bronze Coins FOR 3 Iron Ingot]"},
      .usage = {"$GIVE: what you hand over -- one item name, or several separated by commas.",
                "FOR $RECEIVE: what you ask for in return, in the same format."},
      .notes = {"Only works inside a conversation started with [TALK_TO]; outside one you are told to start talking first.",
                "Nothing changes hands until both sides agree."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[COUNTER_OFFER $GIVE FOR $RECEIVE]",
    .rule = "Propose different terms while your partner's offer is pending; separate multiple items with commas. Examples: [COUNTER_OFFER 2 Iron Ingot FOR 10 Flour], [COUNTER_OFFER 2 Flour, 5 Bronze Coins FOR 3 Iron Ingot].",
    .pattern = std::regex(R"(\[COUNTER_OFFER\b[^\]]*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<TradeOutOfContextAction>("COUNTER_OFFER"); };
    },
    .hidden = true,
    .help = {
      .description = "Propose different terms while your partner's offer is still on the table, instead of accepting or declining it. Your counter replaces the pending offer and is put to the other side.",
      .examples = {"[COUNTER_OFFER 2 Iron Ingot FOR 10 Flour]", "[COUNTER_OFFER 2 Flour, 5 Bronze Coins FOR 3 Iron Ingot]"},
      .usage = {"$GIVE: what you now hand over, comma-separated if several.",
                "FOR $RECEIVE: what you now ask for in return."},
      .notes = {"Only works while a trade is pending inside a conversation."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[ACCEPT]",
    .rule = "Agree to the trade offer currently on the table.",
    .pattern = std::regex(R"(\[ACCEPT\])", std::regex_constants::icase),
    .createAction = [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<TradeOutOfContextAction>("ACCEPT"); };
    },
    .hidden = true,
    .help = {
      .description = "Agree to the trade offer currently on the table, completing the exchange on its stated terms.",
      .examples = {"[ACCEPT]"},
      .usage = {"Takes no arguments."},
      .notes = {"Only works while an offer is pending; used outside a conversation it replies that you are not talking to anyone."}
    }
  });

  GlobalCommandRegistry::Register({
    .format = "[DECLINE]",
    .rule = "Refuse the trade offer currently on the table.",
    .pattern = std::regex(R"(\[DECLINE\])", std::regex_constants::icase),
    .createAction = [](const std::smatch&) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      return [](flecs::entity) { return std::make_unique<TradeOutOfContextAction>("DECLINE"); };
    },
    .hidden = true,
    .help = {
      .description = "Refuse the trade offer currently on the table, leaving both sides' goods where they are.",
      .examples = {"[DECLINE]"},
      .usage = {"Takes no arguments."},
      .notes = {"Only works while an offer is pending; used outside a conversation it replies that you are not talking to anyone."}
    }
  });

  // The catch-all that turns any unclaimed [VERB ...] into an interaction with
  // whatever item or object it resolves against. It has no verb of its own and
  // an empty format, so no [HELP <COMMAND>] lookup can ever reach it and it
  // carries no help text: the verbs it actually serves, such as [HARVEST] and
  // [STORE], belong to the content they act on.
  GlobalCommandRegistry::Register({
    .format = "",
    .rule = "",
    .pattern = std::regex(R"(\[([A-Z_]+)(?:\s+(.+?))?\s*\])", std::regex_constants::icase),
    .createAction = [](const std::smatch& match) -> std::function<std::unique_ptr<AgentAction>(flecs::entity)> {
      std::string actionName = match[1].str();
      std::string args = match.size() > 2 ? match[2].str() : "";
      return [actionName, args](flecs::entity) { return std::make_unique<GenericInteractAction>(actionName, args); };
    },
    .hidden = true
  });

  // Component reflection has to exist before anything is spawned. LoadMap
  // creates the NPCs, and a component added to an entity before its registration
  // is auto-registered without metadata that a later registration cannot repair.
  // StatBlock is stamped on every character at spawn, so this can no longer wait
  // until after the map is loaded.
  RegisterComponents(ecs);

  // Seed every gameplay roll from one place, and log it. The seed is what makes
  // a run reproducible, so a surprising loot outcome can be replayed rather than
  // argued about. A seed of 0 means "pick one from the clock", which is the
  // original behaviour; any other value reproduces that exact run.
  {
    const bool seedFromClock = settings.world.seed == 0;
    const uint64_t seed =
        seedFromClock
            ? static_cast<uint64_t>(
                  std::chrono::high_resolution_clock::now().time_since_epoch().count())
            : settings.world.seed;
    ecs.set<WorldRandomness>({seed, 0});
    if (debugLog) {
      debugLog->Log("World RNG seed: " + std::to_string(seed) +
                    (seedFromClock ? " (from clock)" : " (from settings)"));
    }
  }

  LoadMap(settings.map.file, settings.map.spawnNPCs);
  if (map != nullptr) {
    ecs.set<TileSizeResource>({static_cast<float>(map->GetTileWidth()),
                               static_cast<float>(map->GetTileHeight())});
  }

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
  }, true);

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
  }, true);

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
  }, true);


  // The backend is built from the settings file's "model" block. The API key may
  // have come from settings.json, settings.local.json (merged on top) or the old
  // model.json, which LoadGameSettings still reads as a last resort.
  const GameSettings::ModelSection &modelSettings = settings.model;
  std::unique_ptr<AI> ai = BuildAIBackend(modelSettings, debugLog.get());

  // A dedicated backend for the single navigator. It uses either the NPC model
  // settings or its own `navigation.single.model` block, but always a separate
  // instance: reasoning is off by default, and the navigator's per-call
  // max_tokens must not leak into concurrent NPC requests through the shared
  // backend's options.
  std::unique_ptr<AI> navigationAI;
  if (settings.navigation.agent == "single" &&
      settings.navigation.single.dedicatedBackend) {
    const GameSettings::ModelSection &navigationModel =
        settings.navigation.single.useMainModel
            ? modelSettings
            : settings.navigation.single.model;
    navigationAI = BuildAIBackend(navigationModel, debugLog.get());
    if (navigationAI != nullptr) {
      if (settings.navigation.single.disableReasoning) {
        navigationAI->setReasoningEnabled(false);
      }
      navigationAI->setMaxTokens(settings.navigation.single.maxTokens);
      // Ollama's service default has been 4096 here. The navigator's prompt plus
      // a short history is wider than that, and an over-full context loses the
      // schema at the start. Set the request-level window explicitly.
      if (auto *ollama = dynamic_cast<OllamaAI *>(navigationAI.get())) {
        ollama->setOption("num_ctx",
                          settings.navigation.single.contextTokens);
      }
      if (debugLog) {
        debugLog->LogInfo(
            "Navigation backend: " + navigationAI->getAIName() + " / " +
            navigationAI->getModelName() +
            (settings.navigation.single.useMainModel
                 ? " (main model settings)"
                 : " (navigation model)") +
            (settings.navigation.single.disableReasoning ? " (reasoning off)"
                                                         : " (reasoning on)"));
      }
    } else if (debugLog) {
      debugLog->LogWarning(
          "Navigation backend could not be built; the single navigator will "
          "share the NPC backend.");
    }
  }

  // The Vision Chat window talks to its own backend, never to the NPC one.
  // OllamaAI keys its conversation context by contextId alone, so serving a
  // vision model and a text model from one instance would replay one model's
  // context into the other's request.
  if (settings.vision.enabled) {
    GameSettings::ModelSection visionModel;
    visionModel.type = "ollama";
    visionModel.model = settings.vision.model;
    visionModel.endpoint = settings.vision.endpoint;
    visionModel.hasTemperature = settings.vision.hasTemperature;
    visionModel.temperature = settings.vision.temperature;
    visionModel.keepAlive = settings.vision.keepAlive;
    visionAI = BuildAIBackend(visionModel, debugLog.get());
    if (debugLog && visionAI) {
      debugLog->LogInfo("Vision backend: " + visionAI->getAIName() + " / " +
                        visionAI->getModelName() + " at " + visionModel.endpoint +
                        " (keep_alive: " +
                        (visionModel.keepAlive.empty() ? "ollama default"
                                                       : visionModel.keepAlive) +
                        ")");
    }
  }

  // The Designer Chat window talks to its own backend. Its own section rather than
  // a flag on `model`, for two reasons: OllamaAI keys conversation context by
  // contextId alone, so one instance cannot serve two models, and producing valid
  // blueprint JSON is a different skill from roleplay, so it is worth being able
  // to point at a different model without retuning the NPCs.
  //
  // useMainModel leaves the designer sharing the NPC backend, which is the least
  // surprising default for an unconfigured run.
  {
    GameSettings::ModelSection designerModel =
        settings.designer.useMainModel ? modelSettings : settings.designer.model;
    // A hosted backend with no key of its own inherits the shared one. Without this, a
    // designer block that names a model but no key builds a backend that cannot
    // authenticate, and the window's only symptom is "No answer came back" -- which is
    // exactly how this was diagnosed. Naming a *different* provider still means naming
    // its key, because the only way to reach a key of a foreign provider is to set it.
    if (designerModel.apiKey.empty()) {
      designerModel.apiKey = settings.model.apiKey;
    }
    designerAI = BuildAIBackend(designerModel, debugLog.get());
    // The designer's own output budget, applied after the backend was built so it
    // overrides the max_tokens the model block just set. 0 means inherit, which
    // leaves both the agent shared case and a bespoke designer model exactly as
    // configured. See DesignerSection::maxTokens for why the two budgets differ.
    if (designerAI && settings.designer.maxTokens > 0) {
      designerAI->setMaxTokens(settings.designer.maxTokens);
    }
    if (debugLog && designerAI) {
      debugLog->LogInfo(
          "Designer backend: " + designerAI->getAIName() + " / " +
          designerAI->getModelName() +
          (settings.designer.useMainModel ? " (shares the NPC backend settings)"
                                          : " at " + designerModel.endpoint) +
          (settings.designer.maxTokens > 0
               ? ", output budget " +
                     std::to_string(settings.designer.maxTokens) + " tokens"
               : ""));
    }

    // The Improve button's backend. Built beside the designer's because it is the same
    // kind of thing -- per-window state Game owns for the run -- and because it
    // defaults to the designer's own model settings.
    //
    // Reasoning is switched off unless the owner configured their own rewriter and
    // said otherwise. That is not a micro-optimisation: a reasoning model spends its
    // completion budget thinking and can return an *empty* answer, which a user would
    // read as "the Improve button does nothing". Measured on this repo's default
    // backend before this line existed.
    GameSettings::ModelSection rewriterModel =
        settings.designer.rewriter.enabled ? settings.designer.rewriter.model
                                           : designerModel;
    // Same inheritance as the designer's: naming only a model is the common case, and a
    // backend that silently cannot authenticate reads as a dead button. The main key is
    // the one a person already had to set for the game to run at all.
    if (rewriterModel.apiKey.empty()) {
      rewriterModel.apiKey = settings.model.apiKey;
    }
    rewriterAI = BuildAIBackend(rewriterModel, debugLog.get());
    if (rewriterAI && settings.designer.rewriter.disableReasoning) {
      rewriterAI->setReasoningEnabled(false);
    }
    if (debugLog && rewriterAI) {
      debugLog->LogInfo("Rewriter backend: " + rewriterAI->getAIName() + " / " +
                        rewriterAI->getModelName() +
                        (settings.designer.rewriter.disableReasoning
                             ? " (reasoning off)"
                             : ""));
    }
  }

  // Text to speech for the designer's replies. Built here, beside the backend it
  // speaks for, because it is the same kind of thing: per-window state that Game
  // owns for the whole run.
  //
  // Every failure below is a log line and a silent game, never a startup failure.
  // A missing voice is an unfinished download, not a broken install, and nothing
  // else in the game depends on speech existing.
  if (settings.designer.tts.enabled) {
    std::string speechError;
    std::unique_ptr<PiperSpeech> piper = PiperSpeech::Create(
        settings.designer.tts.binary, settings.designer.tts.voice, speechError);
    if (!speechError.empty()) {
      // Also the non-fatal path: Create reports an unreadable voice config this
      // way and still returns a usable engine, because a guessed sample rate is
      // worth more than a silent window.
      if (debugLog) debugLog->LogWarning("Speech: " + speechError);
      else std::cerr << "Speech: " << speechError << std::endl;
    }

    if (piper) {
      if (debugLog) {
        debugLog->LogInfo("Speech: " + piper->getEngineName() + " / " +
                          piper->getVoiceName() + " @ " +
                          std::to_string(piper->SampleRate()) + " Hz, reading \"" +
                          settings.designer.tts.content + "\"");
      }
      speechBackend = std::move(piper);
      speechQueue = std::make_unique<SpeechQueue>(speechBackend.get(),
                                                  settings.designer.tts.volume);
      speechQueue->Init();
      // Init() failing is not fatal -- the game runs, it just cannot speak -- so
      // the reason is kept for the window to show. It carries the null-device case
      // too, where raylib reports a working device that produces no sound.
      if (!speechQueue->AudioWarning().empty()) {
        speechUnavailableReason = speechQueue->AudioWarning();
        if (debugLog) debugLog->LogWarning("Speech: " + speechUnavailableReason);
        else std::cerr << "Speech: " << speechUnavailableReason << std::endl;
      }
    } else {
      // Create() failed. speechError holds the reason unless it was only the
      // non-fatal unreadable-config case, in which case piper is null for another
      // reason and the message below says so.
      speechUnavailableReason =
          speechError.empty() ? std::string("the speech engine could not be "
                                            "started.")
                             : speechError;
    }
  }

  // A hosted backend with no key cannot answer, so say so now rather than
  // letting every NPC fail its first request with an opaque HTTP error.
  if (modelSettings.apiKey.empty() &&
      (modelSettings.type == "gemini" || modelSettings.type == "openrouter")) {
    const std::string message =
        "Settings: model.type is '" + modelSettings.type +
        "' but model.apiKey is empty. Set it in settings.local.json (gitignored) "
        "or model.json.";
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  }

  AIBackend backend;
  backend.ptr = std::move(ai);
  backend.navigation = std::move(navigationAI);
  ecs.set<AIBackend>(std::move(backend));

  // Stage 5: the out-of-band digest, off unless the setting says otherwise. A
  // character with no backend, or a missing prompt file, leaves the job
  // unavailable and the feature is a no-op -- the engine's own record is already
  // complete, so a missing model cannot change what a player sees.
  if (Settings().ai.memoryDigest) {
    GameSettings::ModelSection digestModel = Settings().model;
    digestModel.hasMaxTokens = true;
    digestModel.maxTokens = Settings().ai.memoryDigestMaxTokens;
    memoryDigestAI = BuildAIBackend(digestModel, debugLog.get());
    if (memoryDigestAI) {
      memoryDigestAI->setReasoningEnabled(false);
    }
    memoryDigestJob = std::make_unique<MemoryDigestJob>();
    memoryDigestJob->Configure(memoryDigestAI.get(),
                               "data/prompts/memory_digest.txt",
                               "memory_digest", debugLog.get());
    memoryDigestGraceUntilSeconds =
        GetTime() + static_cast<double>(
                        std::max(0, Settings().ai.memoryDigestStartupGraceSeconds));
    if (debugLog) {
      const std::string reason = memoryDigestJob->UnavailableReason();
      debugLog->LogInfo(reason.empty()
                            ? "Memory digest: enabled"
                            : "Memory digest: enabled but unavailable (" +
                                  reason + ")");
    }
  }

  ECSInitPhysicsSystems();
  ECSInitLogicSystems();
  ECSInitRenderSystems();
  ECSInitAgentSystems();
  ECSInitActionSystems();
  ECSInitTaskSystems();
  // After the action systems, because its staleness test reads the facing the
  // facing system derives in the same frame.
  ECSInitVisionSystems();
  // After the vision systems: the writer consumes a sample, it does not take one.
  ECSInitMemorySystems();
  ECSInitTileArtSystems();

  // Starting tile and identity come from the map when it authors one, and from
  // settings otherwise. The relocation pass inside LoadMap already moved every
  // colliding entity to open ground, so a spawn authored on a wall is corrected
  // rather than fatal.
  GamePosition startPlayerPos =
      map->HasPlayerStart()
          ? map->GetPlayerStart()
          : GamePosition{settings.player.startX, settings.player.startY};
  playerEntity = ecs.entity(settings.player.name.c_str());
  playerEntity.set<GamePosition>(startPlayerPos);

  playerEntity.set<ScreenPosition>(
      map->GameCoordsToScreenCoords(startPlayerPos.x, startPlayerPos.y));
  playerEntity.set<MaxSpeed>({settings.player.maxSpeed});
  playerEntity.set<Friction>({DEFAULT_PLAYER_FRICTION});
  playerEntity.set<Velocity>({0, 0});
  // The facing system derives from Velocity but never adds the component, so the
  // player has to be born with one exactly like an NPC -- and this is the line the
  // Vision Debug window's "characters without a Facing" count exists to catch if
  // it is ever missed.
  playerEntity.set<Facing>({});
  playerEntity.set<Acceleration>({0, 0});
  // Perception, exactly as an NPC gets it at spawn: the same components, the
  // same empty record, the same authored names as inert seeds. Both characters
  // then go through the same per-entity systems, and the Vision Debug window can
  // read either.
  AttachPerception(playerEntity);

  playerEntity.set<Hitbox>(
      {DEFAULT_PLAYER_HITBOX_WIDTH, DEFAULT_PLAYER_HITBOX_HEIGHT});
  // Tile-sized and art-driven for the same reasons an NPC is. The purple glyph
  // is the fallback, and that purple is what makes the player's tunic purple
  // while every NPC's is dark red: DefaultHumanoidArt() leaves the tunic
  // characters out of its palette on purpose.
  playerEntity.set<DrawAscii>({
      '@',
      {128, 0, 128, 255},
      {128, 128, 128, 0},
      static_cast<size_t>(map->GetTileWidth()),
      static_cast<size_t>(map->GetTileHeight()),
  });
  playerEntity.set<WindowOnClick>({WindowType::EntityInfoWindowType});
  playerEntity.add<Actor>();
  playerEntity.add<PlayerControlled>();
  if (auto registryRes = ecs.get<StatRegistryResource>()) {
    GiveDefaultStats(playerEntity, registryRes->registry);
  }
  if (auto skillRes = ecs.get<SkillRegistryResource>()) {
    GiveStartingSkills(playerEntity, skillRes->registry);
  }
  // The player gets the same default body every other character does -- slots and
  // look -- so [EQUIPMENT] and [EQUIP] work from the first frame and a humanoid
  // race's art applies to the player too. An authored race replaces both. No
  // sprite path is passed: the player has no sprite of its own, so it follows the
  // race convention and otherwise keeps the ASCII look, which is also what makes
  // the two render paths visible side by side in one scene.
  GiveBody(playerEntity, &raceRegistry, settings.player.race, {}, std::string(),
           resourceManager, debugLog.get());

  // The player's own look, when the settings name an actor sprite. Applied after
  // the race so it wins over the race convention; an id with no art leaves the
  // race's look in place and is reported once at spawn.
  if (!settings.player.actorSprite.empty()) {
    if (!AttachSprite(playerEntity, std::string(), settings.player.actorSprite,
                      resourceManager, debugLog.get())) {
      const std::string message =
          "Warning: player.actorSprite '" + settings.player.actorSprite +
          "' has no sprite art (data/sprites/" + settings.player.actorSprite +
          ".png); using the race's look.";
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    }
  }

  // The player's portrait, authored in settings with the race as the default.
  // Same rule as an NPC: an id, or no component at all, which the HUD reads as
  // the unknown sprite.
  {
    std::string portraitId = settings.player.portrait;
    if (portraitId.empty()) {
      const std::string raceId =
          settings.player.race.empty() ? "human" : settings.player.race;
      if (const RaceDef *race = raceRegistry.Get(raceId)) {
        portraitId = race->portrait;
      }
    }
    if (!portraitId.empty()) {
      playerEntity.set<Portrait>({portraitId});
    }
  }

  // --- Player combat state ---------------------------------------------------
  //
  // The bar is every activatable skill the registry currently holds, in the
  // registry's sorted order, so a new skill file appears on the bar without any
  // code change. A slot whose definition is later removed is skipped at draw and
  // activation time rather than crashing.
  if (auto skillRes = ecs.get<SkillRegistryResource>()) {
    if (skillRes->registry != nullptr) {
      SkillBar bar;
      AutoCastSkills autoCast;
      for (const SkillDef *def : skillRes->registry->GetAll()) {
        if (!SkillIsActivatable(def->type)) {
          continue;
        }
        bar.ids.push_back(def->id);
        if (def->activation.autoCast) {
          autoCast.ids.push_back(def->id);
        }
      }
      playerEntity.set<SkillBar>(bar);
      playerEntity.set<AutoCastSkills>(autoCast);
      playerEntity.set<SkillCooldowns>({});
      playerEntity.set<GlobalCooldown>({});
    }
  }

  // Health is resolved from the same sources everything else uses, so
  // Constitution (and later a race or a class) raises the ceiling through data.
  {
    const ResolutionInputs inputs = MakeResolutionInputs(playerEntity.world());
    const FoldResult maxHealth =
        ResolveMaxHealth(playerEntity, inputs.definitions);
    const float ceiling = std::max(1.0f, maxHealth.value);
    playerEntity.set<Vitals>({ceiling, ceiling});
  }

  // The map may author a starting weapon. It is spawned as a real held item and
  // equipped through the same planner [EQUIP] uses, so authored gear cannot
  // produce a state the player could not reach by hand. The single-item
  // "playerWeapon" spelling is the fallback for maps written before the list.
  std::vector<std::string> startingEquipment = map->GetPlayerEquipment();
  if (startingEquipment.empty() && !map->GetPlayerWeapon().empty()) {
    startingEquipment.push_back(map->GetPlayerWeapon());
  }
  if (!startingEquipment.empty()) {
    auto factoryRes = ecs.get<ObjectFactoryResource>();
    if (factoryRes && factoryRes->factory) {
      for (const std::string &typeId : startingEquipment) {
        flecs::entity item = factoryRes->factory->SpawnObject(
            ecs, playerEntity, nullptr, typeId);
        if (!item.is_alive()) {
          continue;
        }
        playerEntity.add<Holds>(item);
        const EquipOutcome outcome = EquipItem(playerEntity, item, &itemRegistry);
        if (!outcome.ok && debugLog) {
          debugLog->LogWarning("Warning: player could not equip '" + typeId +
                               "': " + outcome.message);
        }
      }
    }
  }

  // Initialize debug window entities
  debugConsoleWindowEntity = ecs.entity("Debug Console Window");
  tileInfoWindowEntity = ecs.entity("Tile Info Window");
  astarWindowEntity = ecs.entity("A* Window");
  entityOverviewWindowEntity = ecs.entity("Entity Overview Window");
  debugLogWindowEntity = ecs.entity("Log Window");
  mapReloadWindowEntity = ecs.entity("Map Reload Window");
  drawAsciiToggleWindowEntity = ecs.entity("DrawAscii Debug Window");
  fontSelectionWindowEntity = ecs.entity("Font Selection Window");
  mapEditorWindowEntity = ecs.entity("Map Editor Window");
  blueprintWindowEntity = ecs.entity("Blueprint Window");
  aiMenuWindowEntity = ecs.entity("AI Menu Window");
  npcMenuWindowEntity = ecs.entity("NPC Menu Window");
  taskBoardWindowEntity = ecs.entity("Task Board Window");
  systemOneWindowEntity = ecs.entity("System 1 Overview Window");
  visionDebugWindowEntity = ecs.entity("Vision Debug Window");
  visionChatWindowEntity = ecs.entity("Vision Chat Window");
  designerWindowEntity = ecs.entity("Designer Window");
  contentEditorWindowEntity = ecs.entity("Content Editor Window");
  settingsWindowEntity = ecs.entity("Settings Window");

  // Built once and kept, unlike the other debug windows: a rebuilt window would
  // throw away its transcript, and Game::Draw needs a stable pointer to hand the
  // "world only" capture to. ActiveWindow holds the same shared_ptr, so removing
  // it hides the window without destroying it.
  std::string visionLabel;
  if (visionAI) {
    visionLabel = visionAI->getAIName() + " / " + visionAI->getModelName();
  }
  visionChatWindow =
      std::make_shared<VisionChatWindow>(this, visionAI.get(), visionLabel);
  if (debugWindowState->GetShowVisionChatWindow()) {
    visionChatWindowEntity.set<ActiveWindow>({visionChatWindow});
  }

  // Built once and kept, so the designer conversation survives a toggle. Losing the
  // transcript on every close would make iterating on a design impossible.
  //
  // One window for all three domains, and one backend for all three: producing a valid
  // definition and producing a valid design are the same skill, and asking for a second
  // API key to do it would be a worse trade than sharing. It keeps its own provider
  // conversation per domain inside DesignerSession, so a tile and a building cannot see
  // each other's tokens.
  std::string designerLabel;
  if (designerAI) {
    designerLabel = designerAI->getAIName() + " / " + designerAI->getModelName();
  }
  designerWindow = std::make_shared<DesignerWindow>(
      this, designerAI.get(), designerLabel, designerLogSelfTest);
  if (debugWindowState->GetShowDesignerWindow()) {
    designerWindowEntity.set<ActiveWindow>({designerWindow});
  }

  // The Content Editor. Kept rather than rebuilt, because it holds the document being
  // edited and the selection it belongs to.
  contentEditorWindow = std::make_shared<ContentEditorWindow>(this);
  if (debugWindowState->GetShowContentEditorWindow()) {
    contentEditorWindowEntity.set<ActiveWindow>({contentEditorWindow});
  }

  // Live tile-art generation. Configured through Game so the callback and the
  // world-facing half stay in one translation unit; the queue itself is a Game
  // member, so it is per-world rather than a process global (see TileArtQueue.hpp).
  ECSConfigureTileArtQueue();

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
  if (debugWindowState->GetShowBlueprintWindow()) {
    blueprintWindowEntity.set<ActiveWindow>(
        {std::make_shared<BlueprintWindow>(this)});
  }
  if (debugWindowState->GetShowAIMenuWindow()) {
    aiMenuWindowEntity.set<ActiveWindow>(
        {std::make_shared<AIMenuWindow>(this)});
  }
  if (debugWindowState->GetShowNPCMenuWindow()) {
    npcMenuWindowEntity.set<ActiveWindow>(
        {std::make_shared<NPCMenuWindow>(this)});
  }
  if (debugWindowState->GetShowTaskBoardWindow()) {
    taskBoardWindowEntity.set<ActiveWindow>(
        {std::make_shared<TaskBoardWindow>(this)});
  }
  if (debugWindowState->GetShowSystemOneWindow()) {
    // Rebuilt on every toggle like the other debug windows; the records it shows
    // live on Game::systemOneRecorder, so nothing is lost by doing that.
    systemOneWindowEntity.set<ActiveWindow>(
        {std::make_shared<SystemOneWindow>(this, systemOneRecorder.get())});
  }
  if (debugWindowState->GetShowVisionDebugWindow()) {
    visionDebugWindowEntity.set<ActiveWindow>(
        {std::make_shared<VisionDebugWindow>(this)});
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
      // The texture cache is handed over so item rows can draw the same sprite
      // the item shows on the map; without it they fall back to the ASCII look.
      carrier.set<ActiveWindow>({std::make_shared<StorageWindow>(
          container, carrier, &resourceManager)});
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

void Game::OpenNavigationDebugWindow(flecs::entity character) {
  if (!character.is_alive()) {
    return;
  }
  // Same carrier pattern as the status window: the character's own ActiveWindow
  // slot belongs to its context/chat window, and several navigation views must
  // be able to stay open at once.
  ecs.defer([this, character]() {
    flecs::entity carrier = flecs::entity::null();
    ecs.filter<const NavigationDebugWindowTarget>().each(
        [&](flecs::entity owner, const NavigationDebugWindowTarget &target) {
          if (carrier.is_alive()) {
            return;
          }
          if (target.character == character) {
            carrier = owner;
          }
        });

    if (!carrier.is_alive()) {
      carrier = ecs.entity();
      carrier.set<NavigationDebugWindowTarget>({character});
    }

    if (!carrier.has<ActiveWindow>()) {
      carrier.set<ActiveWindow>(
          {std::make_shared<NavigationDebugWindow>(character, carrier)});
    }
  });
}
