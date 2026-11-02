// GameECSCombat.cpp — combat, crafting and skill resolution.
//
// Part of the GameECS group (see src/GameECSInternals.h): the resolution helpers
// (UseSkill, ApplyHit, ClaimRecipeInputs, HarvestResult, ...), the player combat
// entry points, and RunCombatSelfCheck. The agent systems in
// GameECSInitSystems.cpp and ECSInit in GameECSInit.cpp call into these.

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
                                   const CraftRecipe &recipe, int batches) {
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

// --- Combat ------------------------------------------------------------------
//
// A fight is ordinary timed state in the same world: a locked target, a cooldown
// per skill, and one damage fold. There is no encounter entity and no turn
// manager. The auto-cast system is what keeps a fight moving while the player
// (and, in a later slice, the agent) is deciding.

const SkillDef *FindSkillDef(flecs::world world, const std::string &id) {
  auto res = world.get<SkillRegistryResource>();
  if (!res || res->registry == nullptr) {
    return nullptr;
  }
  return res->registry->Get(id);
}

// A character's display name, falling back to the entity name and then to a
// caller-chosen noun. Used only for log lines.
std::string EntityLabel(flecs::entity entity, const char *fallback) {
  if (const DisplayName *name = entity.get<DisplayName>()) {
    if (!name->name.empty()) {
      return name->name;
    }
  }
  if (const char *entityName = entity.name().c_str()) {
    if (entityName[0] != '\0') {
      return std::string(entityName);
    }
  }
  return std::string(fallback);
}

int GridSteps(const GamePosition &a, const GamePosition &b) {
  return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

bool WithinActivationRange(flecs::entity actor, flecs::entity target,
                           float range) {
  const GamePosition *from = actor.get<GamePosition>();
  const GamePosition *to = target.get<GamePosition>();
  return from != nullptr && to != nullptr &&
         static_cast<float>(GridSteps(*from, *to)) <= range;
}

// Whole seconds left on a cooldown, rounded up, for a log line. Rounding up is
// the honest direction: "1s" while 0.1 seconds remain reads as a lie.
std::string CooldownLabel(float seconds) {
  const int whole = static_cast<int>(std::ceil(seconds));
  return std::to_string(whole) + "s";
}

// One activation of one skill, for one actor.
//
// This is the single door every path goes through -- the hotbar, auto-cast, and
// later an agent command -- so a cooldown, a range check or a training grant can
// never apply on one path and not another.
//
// It reports whether the skill was used and a human-readable line; it logs
// nothing itself, because auto-cast has to stay silent when an attack is merely
// out of range and a manual use has to say why it did nothing.
// Cancels a cast on `target`. Returns true when there was an interruptible cast
// to cancel.
//
// The action entity is destroyed through the world's defer queue: an interrupt
// is raised from inside a damage application, which may itself be inside a
// system iteration over actions, and destroying the action immediately would
// invalidate the iteration that is about to look at it.
bool InterruptCast(flecs::entity target) {
  Casting *casting = target.get_mut<Casting>();
  if (casting == nullptr || !casting->interruptible) {
    return false;
  }
  const flecs::entity action = casting->actionEntity;
  target.remove<Casting>();
  SpawnFloatingCallout(target, "Interrupted!",
                       DEFAULT_INTERRUPT_CALL_OUT_COLOR);
  if (const Busy *busy = target.get<Busy>()) {
    if (busy->actionEntity == action) {
      target.remove<Busy>();
    }
  }
  if (action.is_alive()) {
    flecs::world world = target.world();
    world.defer([action]() {
      if (action.is_alive()) {
        action.destruct();
      }
    });
  }
  return true;
}

// Cancels a committed cast for a reason other than damage: the target moved out
// of range or vanished. This is intentionally not gated on `interruptible` --
// interruptibility is about damage, while an invalid target is a hard range
// requirement. The cooldown already started at cast start and is not refunded,
// matching damage interrupts and the existing "a cast spends the skill" rule.
bool CancelCast(flecs::entity target, const std::string &callout) {
  Casting *casting = target.get_mut<Casting>();
  if (casting == nullptr) {
    return false;
  }
  const flecs::entity action = casting->actionEntity;
  target.remove<Casting>();
  SpawnFloatingCallout(target, callout, DEFAULT_INTERRUPT_CALL_OUT_COLOR);
  if (const Busy *busy = target.get<Busy>()) {
    if (busy->actionEntity == action) {
      target.remove<Busy>();
    }
  }
  if (action.is_alive()) {
    flecs::world world = target.world();
    world.defer([action]() {
      if (action.is_alive()) {
        action.destruct();
      }
    });
  }
  return true;
}

// A movement-locked cast forces the player to a stop before the wind-up starts.
// Clearing the path as well as velocity matters: a held direction and a clicked
// path are two different movement sources, and either one resuming cancels the
// cast in CastMovementCancelSystem.
void StopPlayerMovement(flecs::entity actor) {
  actor.remove<MOVE_THROUGH_PATH_ACTION>();
  actor.remove<PendingPlayerInteraction>();
  if (Velocity *velocity = actor.get_mut<Velocity>()) {
    *velocity = {0.0f, 0.0f};
  }
  if (Acceleration *acceleration = actor.get_mut<Acceleration>()) {
    *acceleration = {0.0f, 0.0f};
  }
}

// Applies one resolved hit: fold, armour, floating text, the dummy/player reset,
// and an optional interrupt.
//
// This is the single place damage lands, for instant skills, cast completions and
// the aggressive dummy's inline strike, so armour and the reset rules cannot
// differ between them.
std::string ApplyHit(flecs::entity actor, flecs::entity target, float baseDamage,
                     const std::string &subjectId, const std::string &label,
                     bool secondPerson, bool interrupts) {
  if (!target.is_alive() || !target.has<Vitals>()) {
    return "There is nothing there to hit.";
  }

  const ResolutionInputs inputs = MakeResolutionInputs(actor.world());
  DamageRequest request;
  request.activity = "attack";
  request.subjectId = subjectId;
  request.baseDamage = baseDamage;
  const FoldResult fold = ResolveDamage(actor, request, inputs.definitions);
  if (fold.blocked) {
    return fold.blockReason;
  }

  const int raw = std::max(1, static_cast<int>(std::lround(fold.value)));
  const float armour = ArmorOf(target, ItemsOf(actor.world()));
  const int points =
      std::max(1, raw - static_cast<int>(std::lround(armour)));

  const std::string actorLabel =
      secondPerson ? std::string("You") : EntityLabel(actor, "Someone");
  const std::string targetLabel = EntityLabel(target, "the target");

  Vitals *targetVitals = target.get_mut<Vitals>();
  targetVitals->health -= static_cast<float>(points);
  SpawnFloatingDamage(target, points);

  std::string message = actorLabel + (secondPerson ? " hit " : " hits ") +
                        targetLabel + " with " + label + " for " +
                        std::to_string(points);
  if (armour >= 0.5f) {
    message += " (armour " + std::to_string(static_cast<int>(std::lround(armour))) +
               ")";
  }
  message += ".";

  // Neutral + Retaliates is the defensive case: the first player hit flips the
  // stance, and the existing melee controller starts using the profile it was
  // spawned with. Neutral without the tag stays out of the fight.
  if (targetVitals->health > 0.0f && target.has<Retaliates>() &&
      actor.has<PlayerControlled>()) {
    if (NpcDisposition *disposition = target.get_mut<NpcDisposition>();
        disposition != nullptr &&
        disposition->value == Disposition::Neutral) {
      disposition->value = Disposition::Aggressive;
      message += " " + targetLabel + " turns on you.";
    }
  }

  if (targetVitals->health <= 0.0f) {
    if (target.has<TrainingDummy>()) {
      // A dummy exists to be hit forever; resetting is what "immortal" means.
      targetVitals->health = targetVitals->maxHealth;
      message += " The dummy shrugs it off.";
    } else if (target.has<PlayerControlled>()) {
      // No death system yet. Knocking the player out and restoring them keeps a
      // test session from ending in a corpse with no way to continue; a real
      // death and recovery loop is a later slice.
      targetVitals->health = targetVitals->maxHealth;
      message += " You are knocked out and recover.";
    } else {
      targetVitals->health = 0.0f;
      message += " " + targetLabel + " falls.";
    }
  }

  if (interrupts && InterruptCast(target)) {
    message += " The cast is interrupted.";
  }

  return message;
}

// Progression and wear, shared by the instant path and the cast-completion path
// so a skill cannot train on one and not the other.
std::string FinalizeSkillUse(flecs::entity actor, const SkillDef &def) {
  std::string message;

  if (def.activation.xpPerUse > 0) {
    if (auto skillRes = actor.world().get<SkillRegistryResource>()) {
      if (skillRes->registry != nullptr) {
        if (SkillBlock *block = actor.get_mut<SkillBlock>()) {
          const std::vector<SkillXp> grants{{def.id, def.activation.xpPerUse}};
          const std::vector<SkillGain> gains =
              ApplyTraining(*block, grants, *skillRes->registry, "use");
          const std::string training = FormatTrainingGains(gains);
          if (!training.empty()) {
            message += "\n" + training;
          }
        }
      }
    }
  }

  if (def.activation.useWeapon) {
    const WearResult wear =
        WearEquipment(actor, "attack", DEFAULT_ATTACK_DURABILITY_COST,
                      ItemsOf(actor.world()));
    if (!wear.broken.empty()) {
      message += "\n" + FormatBrokenTools(wear.broken);
    }
  }

  return message;
}

// The visual centre of an entity, in world pixels: its ScreenPosition is the
// top-left of its glyph or hitbox, and projectiles and floating text centre on
// the body rather than on the corner.
Vector2 ScreenCenterOf(flecs::entity entity) {
  const ScreenPosition *position = entity.get<ScreenPosition>();
  if (position == nullptr) {
    return {0.0f, 0.0f};
  }
  float width = static_cast<float>(DEFAULT_ENTITY_VISUAL_WIDTH);
  float height = static_cast<float>(DEFAULT_ENTITY_VISUAL_HEIGHT);
  if (const Hitbox *hitbox = entity.get<Hitbox>()) {
    width = static_cast<float>(hitbox->width);
    height = static_cast<float>(hitbox->height);
  } else if (const DrawAscii *ascii = entity.get<DrawAscii>()) {
    width = static_cast<float>(ascii->width);
    height = static_cast<float>(ascii->height);
  }
  return {position->x + width * 0.5f, position->y + height * 0.5f};
}

// The radius the swept collision tests against. Hitbox is the body; DrawAscii is
// only a fallback because a glyph can be smaller than the creature it draws.
float EntityHitRadius(flecs::entity entity) {
  if (const Hitbox *hitbox = entity.get<Hitbox>()) {
    return std::max(static_cast<float>(hitbox->width),
                    static_cast<float>(hitbox->height)) *
           0.5f;
  }
  return static_cast<float>(DEFAULT_ENTITY_VISUAL_WIDTH) * 0.5f;
}

// Launches the skill's projectile, if it has one. Returns false when the skill
// has no projectile id, the registry is missing, or the caster has no screen
// position -- callers then fall back to applying the impact directly, so a
// content error costs the flight, not the spell.
bool SpawnSkillProjectile(flecs::entity actor, flecs::entity target,
                          const SkillDef &def, float baseDamage,
                          bool playerCast) {
  if (def.activation.projectile.empty() || !actor.has<ScreenPosition>()) {
    return false;
  }
  auto registryRes = actor.world().get<ProjectileRegistryResource>();
  if (!registryRes || registryRes->registry == nullptr) {
    return false;
  }
  const ProjectileDef *projectileDef =
      registryRes->registry->Get(def.activation.projectile);
  if (projectileDef == nullptr) {
    return false;
  }

  Projectile projectile;
  projectile.owner = actor;
  projectile.target = target;
  projectile.skillId = def.id;
  projectile.label = def.name;
  projectile.sprite = projectileDef->sprite;
  projectile.baseDamage = baseDamage;
  projectile.speed = projectileDef->speed;
  projectile.size = projectileDef->size;
  projectile.playerCast = playerCast;
  projectile.interrupts = def.activation.interrupts;

  const Vector2 start = ScreenCenterOf(actor);
  ScreenPosition startPosition;
  startPosition.x = start.x;
  startPosition.y = start.y;

  flecs::entity entity = actor.world().entity();
  entity.set<ScreenPosition>(startPosition);
  entity.set<Projectile>(std::move(projectile));
  return entity.is_alive();
}

// Launches an aimed skillshot along `direction`. The path builder, not this
// function, decides whether the result is a line, an arc or a spread; this only
// copies the launch-time facts and the sampled path onto the entity.
bool SpawnSkillshot(flecs::entity actor, const SkillDef &def, float damage,
                    Vector2 direction, bool playerCast) {
  if (def.activation.skillshot.empty() || !actor.has<ScreenPosition>()) {
    return false;
  }
  auto registryRes = actor.world().get<SkillshotRegistryResource>();
  if (!registryRes || registryRes->registry == nullptr) {
    return false;
  }
  const SkillshotDef *skillshotDef =
      registryRes->registry->Get(def.activation.skillshot);
  if (skillshotDef == nullptr) {
    return false;
  }

  float tileWidth = 32.0f;
  if (const TileSizeResource *tileSize = actor.world().get<TileSizeResource>()) {
    tileWidth = tileSize->width;
  }
  const float rangePixels = def.activation.rangeTiles * tileWidth;
  std::vector<Vector2> path = BuildSkillshotPath(
      *skillshotDef, ScreenCenterOf(actor), direction, rangePixels);
  if (path.size() < 2) {
    return false;
  }

  Skillshot skillshot;
  skillshot.owner = actor;
  skillshot.skillId = def.id;
  skillshot.label = def.name;
  skillshot.sprite = skillshotDef->sprite;
  skillshot.path = std::move(path);
  skillshot.speed = skillshotDef->speed;
  skillshot.width = skillshotDef->width;
  skillshot.length = skillshotDef->length;
  skillshot.damage = damage;
  skillshot.playerCast = playerCast;
  skillshot.interrupts = def.activation.interrupts;

  flecs::entity entity = actor.world().entity();
  entity.set<Skillshot>(std::move(skillshot));
  return entity.is_alive();
}

// Starts one use of one skill.
//
// Instant skills resolve here. Cast-time skills commit the cast here: the
// cooldown and GCD start immediately, the caster is marked Casting, and an
// action entity carries the timer. CastResolutionSystem applies the effect when
// the timer completes.
SkillUseOutcome UseSkill(flecs::entity actor, const std::string &skillId,
                         flecs::entity target, bool secondPerson,
                         bool announce, Vector2 direction) {
  SkillUseOutcome outcome;
  if (!actor.is_alive() || !actor.has<Vitals>()) {
    return outcome;
  }

  const SkillDef *def = FindSkillDef(actor.world(), skillId);
  if (def == nullptr) {
    outcome.message = "Unknown skill '" + skillId + "'.";
    return outcome;
  }
  if (!SkillIsActivatable(def->type)) {
    outcome.message = def->name + " is a passive skill and has no button.";
    return outcome;
  }

  SkillCooldowns *cooldowns = actor.get_mut<SkillCooldowns>();
  GlobalCooldown *globalCooldown = actor.get_mut<GlobalCooldown>();
  if (cooldowns == nullptr || globalCooldown == nullptr) {
    return outcome;
  }

  if (const Casting *casting = actor.get<Casting>()) {
    outcome.message = "You are already casting " + casting->label + ".";
    return outcome;
  }

  const float readyIn = cooldowns->RemainingFor(skillId);
  if (readyIn > 0.0f) {
    outcome.message = def->name + " is not ready yet (" + CooldownLabel(readyIn) + ").";
    return outcome;
  }
  if (def->activation.gcd && globalCooldown->remaining > 0.0f) {
    outcome.message = "You are still recovering (" +
                      CooldownLabel(globalCooldown->remaining) + ").";
    return outcome;
  }

  if (def->activation.requiresTarget &&
      def->activation.skillshot.empty()) {
    if (!target.is_alive()) {
      outcome.message = "You have no target. Click a creature or press Tab.";
      return outcome;
    }
    if (target == actor) {
      outcome.message = "You cannot use " + def->name + " on yourself.";
      return outcome;
    }
    if (!target.has<Vitals>()) {
      outcome.message = "That is not something you can attack.";
      return outcome;
    }
    if (!WithinActivationRange(actor, target, def->activation.rangeTiles)) {
      outcome.message = "You are too far away to use " + def->name + ".";
      return outcome;
    }
  }

  const ResolutionInputs inputs = MakeResolutionInputs(actor.world());

  float baseDamage = def->activation.baseDamage;
  float cooldownSeconds = def->activation.cooldownSeconds;
  std::string weaponId;

  if (def->activation.useWeapon) {
    const EquippedWeapon weapon = FindEquippedWeapon(actor, inputs.definitions);
    if (weapon.definition != nullptr) {
      baseDamage = weapon.definition->damage;
      cooldownSeconds = weapon.definition->swingTime;
      weaponId = weapon.id;
    } else {
      baseDamage = DEFAULT_UNARMED_DAMAGE;
      cooldownSeconds = DEFAULT_UNARMED_SWING_SECONDS;
    }

    DurationRequest durationRequest;
    durationRequest.activity = "attack";
    durationRequest.subjectId = weaponId;
    durationRequest.baseSeconds = cooldownSeconds;
    const DurationResolution duration =
        ResolveDuration(actor, durationRequest, inputs.definitions, *inputs.hooks);
    if (duration.fold.blocked) {
      outcome.message = duration.fold.blockReason;
      return outcome;
    }
    cooldownSeconds = duration.fold.value;
  }

  // The cooldown starts now for both paths. A cast that is interrupted has still
  // spent the skill, which is what makes interrupting worth doing.
  cooldowns->Start(skillId, cooldownSeconds);
  if (def->activation.gcd) {
    // 0 is a deliberate "no global cooldown"; only the negative sentinel falls
    // back to the character default. This is what lets Ice Lance with
    // cooldown 0 / gcdSeconds 0 actually be usable again immediately.
    globalCooldown->remaining =
        def->activation.gcdSeconds >= 0.0f
            ? def->activation.gcdSeconds
            : DEFAULT_GLOBAL_COOLDOWN_SECONDS;
  }

  // An aimed skill needs a heading, not a target. A caller with no mouse (the
  // self-check, auto-cast) still gets a sensible one: toward the locked target,
  // then east as the last resort.
  Vector2 aim = direction;
  if (aim.x == 0.0f && aim.y == 0.0f && target.is_alive()) {
    const Vector2 from = ScreenCenterOf(actor);
    const Vector2 to = ScreenCenterOf(target);
    aim = {to.x - from.x, to.y - from.y};
  }
  if (aim.x == 0.0f && aim.y == 0.0f) {
    aim = {1.0f, 0.0f};
  }
  aim = SkillshotNormalize(aim);

  const std::string actorLabel =
      secondPerson ? std::string("You") : EntityLabel(actor, "Someone");

  if (def->activation.requiresTarget && def->activation.rangeTiles > 0.0f) {
    SkillRangeIndicator indicator;
    indicator.rangeTiles = def->activation.rangeTiles;
    indicator.remaining =
        def->activation.castSeconds > 0.0f
            ? def->activation.castSeconds +
                  DEFAULT_SKILL_RANGE_POST_CAST_SECONDS
            : DEFAULT_SKILL_RANGE_PULSE_SECONDS;
    indicator.target = target;
    indicator.movementArea = def->activation.canCastWhileMoving;
    actor.set<SkillRangeIndicator>(indicator);
  }

  if (def->activation.castSeconds > 0.0f) {
    CastAction committed{skillId,       def->name, baseDamage,
                          def->activation.interruptible, secondPerson, aim};
    committed.requiresTarget = def->activation.requiresTarget;
    committed.rangeTiles = def->activation.rangeTiles;
    flecs::entity action =
        actor.world()
            .entity()
            .set<ActionTimer>({def->activation.castSeconds})
            .set<ActionActor>({actor})
            .set<CastAction>(committed);
    if (def->activation.requiresTarget) {
      action.set<ActionTarget>({target});
    }
    const bool movementLocked =
        !def->activation.canCastWhileMoving && actor.has<PlayerControlled>();
    if (movementLocked) {
      StopPlayerMovement(actor);
    }
    actor.set<Casting>(
        {action, skillId, def->name, def->activation.interruptible,
         movementLocked});
    actor.set<Busy>({action});

    outcome.used = true;
    outcome.message = actorLabel +
                      (secondPerson ? " begin casting " : " begins casting ") +
                      def->name + ".";
    return outcome;
  }

  // An aimed skillshot is a path, not a target. It travels out to the skill's
  // range and hits the first Actor it sweeps; the path builder owns the shape.
  if (!def->activation.skillshot.empty()) {
    if (SpawnSkillshot(actor, *def, baseDamage, aim, secondPerson)) {
      outcome.message = actorLabel + (secondPerson ? " hurl " : " hurls ") +
                        def->name + ".";
      outcome.message += FinalizeSkillUse(actor, *def);
      if (announce) {
        SpawnFloatingCallout(actor, def->name, DEFAULT_ABILITY_CALL_OUT_COLOR);
      }
    } else {
      outcome.message =
          "The " + def->name + " fizzles: its skillshot is not loaded.";
    }
    outcome.used = true;
    return outcome;
  }

  // A projectile is a delivery, not an impact: the hit is applied by
  // ProjectileUpdateSystem when the object reaches the target. A failed launch
  // (missing registry/id/position) falls through to a direct hit so a content
  // error costs the flight, not the spell.
  if (!def->activation.projectile.empty() &&
      SpawnSkillProjectile(actor, target, *def, baseDamage, secondPerson)) {
    outcome.message = actorLabel + (secondPerson ? " hurl " : " hurls ") +
                      def->name + " at " +
                      EntityLabel(target, "the target") + ".";
    outcome.message += FinalizeSkillUse(actor, *def);
    if (announce) {
      SpawnFloatingCallout(actor, def->name, DEFAULT_ABILITY_CALL_OUT_COLOR);
    }
    outcome.used = true;
    return outcome;
  }

  if (def->activation.requiresTarget) {
    outcome.message = ApplyHit(actor, target, baseDamage, skillId, def->name,
                               secondPerson, def->activation.interrupts);
  } else {
    outcome.message =
        actorLabel + (secondPerson ? " use " : " uses ") + def->name + ".";
  }
  outcome.message += FinalizeSkillUse(actor, *def);

  // A deliberate instant use says its name over the caster. Auto-cast passes
  // `announce == false`: the basic attack repeats every swing, and a name over
  // the head on every one of those is noise, not feedback.
  if (announce) {
    SpawnFloatingCallout(actor, def->name, DEFAULT_ABILITY_CALL_OUT_COLOR);
  }

  outcome.used = true;
  return outcome;
}


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


// --- Player combat entry points ---------------------------------------------

void Game::SetPlayerTarget(flecs::entity target) {
  if (!playerEntity.is_alive()) {
    return;
  }
  if (!target.is_alive() || target == playerEntity) {
    playerEntity.remove<LockedTarget>();
    if (debugLog) {
      debugLog->LogCombat("Target cleared.");
    }
    return;
  }

  playerEntity.set<LockedTarget>({target});

  const std::string name = EntityLabel(target, "target");
  if (const Vitals *vitals = target.get<Vitals>()) {
    if (debugLog) {
      debugLog->LogCombat(
          "Targeting " + name + " (" +
          std::to_string(static_cast<int>(std::ceil(vitals->health))) + "/" +
          std::to_string(static_cast<int>(std::ceil(vitals->maxHealth))) + ").");
    }
  } else if (debugLog) {
    debugLog->LogCombat("Targeting " + name + " (it has no health to show).");
  }
}

void Game::CyclePlayerTarget() {
  if (!playerEntity.is_alive()) {
    return;
  }
  const GamePosition *playerPos = playerEntity.get<GamePosition>();
  if (playerPos == nullptr) {
    return;
  }

  // Anything with health is a candidate. Distance decides the order, and the
  // cap keeps a target across the map from being selected by a Tab press.
  std::vector<std::pair<int, flecs::entity>> candidates;
  ecs.filter<GamePosition, Vitals>().each(
      [&](flecs::entity entity, const GamePosition &pos, Vitals &) {
        if (entity == playerEntity) {
          return;
        }
        const int dx = pos.x - playerPos->x;
        const int dy = pos.y - playerPos->y;
        const int distance = dx * dx + dy * dy;
        if (distance > 15 * 15) {
          return;
        }
        candidates.push_back({distance, entity});
      });

  if (candidates.empty()) {
    if (debugLog) {
      debugLog->LogCombat("There is nothing to target nearby.");
    }
    return;
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const auto &a, const auto &b) {
              if (a.first != b.first) {
                return a.first < b.first;
              }
              return a.second.id() < b.second.id();
            });

  flecs::entity current = flecs::entity::null();
  if (const LockedTarget *lock = playerEntity.get<LockedTarget>()) {
    current = lock->target;
  }

  size_t next = 0;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (candidates[i].second == current) {
      next = (i + 1) % candidates.size();
      break;
    }
  }
  SetPlayerTarget(candidates[next].second);
}

void Game::RefreshPlayerSkillBar() {
  if (!playerEntity.is_alive()) {
    return;
  }

  SkillBar bar;
  AutoCastSkills autoCast;
  const AutoCastSkills *previous = playerEntity.get<AutoCastSkills>();
  for (const SkillDef *def : skillRegistry.GetAll()) {
    if (!SkillIsActivatable(def->type)) {
      continue;
    }
    bar.ids.push_back(def->id);
    // Preserve the human's "Always casts" choices exactly. The component only
    // records ON skills, so re-seeding from the authored autoCast default here
    // would turn the basic attack back on after the player deliberately turned
    // it off, every time any skill was saved.
    if (previous != nullptr && previous->Has(def->id)) {
      autoCast.ids.push_back(def->id);
    }
  }

  playerEntity.set<SkillBar>(std::move(bar));
  playerEntity.set<AutoCastSkills>(std::move(autoCast));
}

bool Game::ActivatePlayerSkill(int index, Vector2 direction) {
  if (!playerEntity.is_alive() || index < 0) {
    return false;
  }
  const SkillBar *bar = playerEntity.get<SkillBar>();
  if (bar == nullptr || index >= static_cast<int>(bar->ids.size())) {
    return false;
  }
  // Copy before UseSkill: committing a cast adds Casting/Busy, which moves the
  // player to a new archetype and invalidates a reference into SkillBar.
  const std::string skillId = bar->ids[static_cast<size_t>(index)];

  flecs::entity target = flecs::entity::null();
  if (const LockedTarget *lock = playerEntity.get<LockedTarget>()) {
    target = lock->target;
  }

  const SkillUseOutcome outcome =
      UseSkill(playerEntity, skillId, target, true, true, direction);
  if (debugLog && !outcome.message.empty()) {
    debugLog->LogCombat(outcome.message);
  }
  return outcome.used;
}

int Game::RunCombatSelfCheck() {
  if (!playerEntity.is_alive()) {
    std::cout << "combat-check: no player entity\n";
    return 1;
  }

  // The plain dummy, not the sparring one: the check drives the player, so the
  // opponent must not be acting on its own.
  flecs::entity dummy = flecs::entity::null();
  ecs.filter<GamePosition, Vitals>().each(
      [&](flecs::entity entity, const GamePosition &, Vitals &) {
        if (!dummy.is_alive() && entity.has<TrainingDummy>() &&
            !entity.has<Aggressive>()) {
          dummy = entity;
        }
      });
  if (!dummy.is_alive()) {
    std::cout << "combat-check: this map has no training dummy\n";
    return 1;
  }

  // Stand next to it, because every attack is a range check and a check that
  // teleported the target to the player would not be testing the range rule.
  const GamePosition dummyPos = *dummy.get<GamePosition>();
  GamePosition beside{dummyPos.x - 1, dummyPos.y};
  if (map != nullptr && !map->IsInBounds(static_cast<float>(beside.x),
                                         static_cast<float>(beside.y))) {
    beside = {dummyPos.x, dummyPos.y - 1};
  }
  playerEntity.set<GamePosition>(beside);
  if (map != nullptr) {
    playerEntity.set<ScreenPosition>(
        map->GameCoordsToScreenCoords(beside.x, beside.y));
  }

  SetPlayerTarget(dummy);

  const SkillBar *bar = playerEntity.get<SkillBar>();
  if (bar == nullptr || bar->ids.empty()) {
    std::cout << "combat-check: the player has no skill bar\n";
    return 1;
  }
  // Starting and canceling casts adds/removes Casting and Busy, moving the
  // player's archetype and invalidating a raw SkillBar* held across progress().
  const std::vector<std::string> barIds = bar->ids;

  // Slots are looked up by id, not by index: every new skill file re-sorts the
  // bar, and a check that hardcoded "slot 1" would silently start testing
  // Fireball the day Fireball was added.
  auto slotOf = [&](const std::string &id) {
    for (size_t i = 0; i < barIds.size(); ++i) {
      if (barIds[i] == id) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };
  const int attackSlot = slotOf("attack");
  const int heavySlot = slotOf("heavy_swing");
  const int kickSlot = slotOf("kick");
  const int fireballSlot = slotOf("fireball");
  const int iceSlot = slotOf("ice_lance");
  if (attackSlot < 0) {
    std::cout << "combat-check: the bar has no attack skill\n";
    return 1;
  }

  const float healthBefore = dummy.get<Vitals>()->health;
  const bool first = ActivatePlayerSkill(attackSlot);
  const float healthAfter = dummy.get<Vitals>()->health;
  const bool second = ActivatePlayerSkill(attackSlot);

  // The check is the only place that can drive a cast to completion without a
  // frame clock, so it does: clear the cooldowns, start Heavy Swing, expire its
  // timer by hand, and let the normal systems resolve it.
  auto clearCooldowns = [&]() {
    if (SkillCooldowns *cooldowns = playerEntity.get_mut<SkillCooldowns>()) {
      cooldowns->cooldowns.clear();
    }
    if (GlobalCooldown *gcd = playerEntity.get_mut<GlobalCooldown>()) {
      gcd->remaining = 0.0f;
    }
  };

  clearCooldowns();
  // The auto-cast would swing the basic attack during progress() and make the
  // cast's damage unattributable.
  if (AutoCastSkills *autoCast = playerEntity.get_mut<AutoCastSkills>()) {
    autoCast->ids.clear();
  }

  const bool hasCastSkill = heavySlot >= 0;
  const bool castStarted = hasCastSkill && ActivatePlayerSkill(heavySlot);
  bool castRangeIndicator = false;
  if (const SkillRangeIndicator *indicator =
          playerEntity.get<SkillRangeIndicator>()) {
    castRangeIndicator = indicator->rangeTiles == 1.0f &&
                         indicator->movementArea &&
                         indicator->target == dummy;
  }
  const float healthBeforeCast = dummy.get<Vitals>()->health;
  bool castResolved = false;
  if (const Casting *casting = playerEntity.get<Casting>()) {
    flecs::entity castAction = casting->actionEntity;
    if (castAction.is_alive()) {
      if (ActionTimer *timer = castAction.get_mut<ActionTimer>()) {
        timer->timeRemaining = 0.0f;
      }
      castAction.add<ActionCompleted>();
      ecs.progress();
      castResolved = dummy.get<Vitals>()->health < healthBeforeCast;
    }
  }

  // A targeted cast must be canceled when the target leaves its range during the
  // wind-up. Move both position components: a GamePosition-only edit is reverted
  // from ScreenPosition by the logic-phase sync.
  bool castRangeStarted = false;
  bool castRangeCanceled = !hasCastSkill;
  if (hasCastSkill) {
    clearCooldowns();
    SetPlayerTarget(dummy);
    castRangeStarted = ActivatePlayerSkill(heavySlot);
    if (castRangeStarted) {
      const GamePosition original = *dummy.get<GamePosition>();
      const GamePosition far = {beside.x + 6, beside.y + 6};
      dummy.set<GamePosition>(far);
      if (map != nullptr) {
        dummy.set<ScreenPosition>(map->GameCoordsToScreenCoords(far.x, far.y));
      }
      ecs.progress();
      castRangeCanceled = !playerEntity.has<Casting>();
      dummy.set<GamePosition>(original);
      if (map != nullptr) {
        dummy.set<ScreenPosition>(
            map->GameCoordsToScreenCoords(original.x, original.y));
      }
      clearCooldowns();
    }
  }

  // A movement-locked cast is canceled by resuming movement. Synthetic because
  // the loaded skill content leaves the option at its default (free movement).
  bool movementCanceled = false;
  {
    flecs::entity action =
        ecs.entity()
            .set<ActionTimer>({1.0f})
            .set<ActionActor>({playerEntity})
            .set<CastAction>(
                {std::string(), "Test Cast", 1.0f, true, false});
    playerEntity.set<Casting>(
        {action, std::string(), "Test Cast", true, true});
    playerEntity.set<Velocity>({10.0f, 0.0f});
    ecs.progress();
    movementCanceled = !playerEntity.has<Casting>();
    playerEntity.set<Velocity>({0.0f, 0.0f});
  }

  // Interrupt: put a cast on the dummy and kick it apart. The kick is instant and
  // flagged `interrupts`, so this exercises the same path an agent will use.
  clearCooldowns();
  const bool hasKick = kickSlot >= 0;
  bool kickUsed = false;
  bool interrupted = false;
  if (hasKick) {
    flecs::entity action =
        ecs.entity()
            .set<ActionTimer>({5.0f})
            .set<ActionActor>({dummy})
            .set<ActionTarget>({playerEntity})
            .set<CastAction>({std::string(), "Test Cast", 10.0f, true, false});
    dummy.set<Casting>({action, std::string(), "Test Cast", true});
    dummy.set<Busy>({action});
    kickUsed = ActivatePlayerSkill(kickSlot);
    interrupted = !dummy.has<Casting>();
    ecs.progress();  // flush the deferred action destruction
  }

  // Projectile: launch Fireball, then put the object on the target so the
  // impact is tested without depending on progress() advancing a frame clock.
  clearCooldowns();
  bool fireballUsed = false;
  bool fireballHit = false;
  if (fireballSlot >= 0) {
    const float beforeFireball = dummy.get<Vitals>()->health;
    fireballUsed = ActivatePlayerSkill(fireballSlot);

    flecs::entity projectile = flecs::entity::null();
    ecs.filter<Projectile>().each(
        [&](flecs::entity entity, Projectile &) {
          if (!projectile.is_alive()) {
            projectile = entity;
          }
        });
    if (projectile.is_alive()) {
      if (ScreenPosition *position = projectile.get_mut<ScreenPosition>()) {
        const Vector2 center = ScreenCenterOf(dummy);
        position->x = center.x;
        position->y = center.y;
      }
      ecs.progress();
      fireballHit = dummy.get<Vitals>()->health < beforeFireball;
    }
  }

  // Skillshot: launch Ice Lance at the locked dummy, then put its travel distance
  // at the dummy so the swept collision runs without relying on a frame clock.
  clearCooldowns();
  bool iceUsed = false;
  bool iceHit = false;
  if (iceSlot >= 0) {
    const float beforeIce = dummy.get<Vitals>()->health;
    iceUsed = ActivatePlayerSkill(iceSlot);

    flecs::entity skillshot = flecs::entity::null();
    ecs.filter<Skillshot>().each([&](flecs::entity entity, Skillshot &) {
      if (!skillshot.is_alive()) {
        skillshot = entity;
      }
    });
    if (skillshot.is_alive()) {
      if (Skillshot *shot = skillshot.get_mut<Skillshot>()) {
        const Vector2 origin = ScreenCenterOf(playerEntity);
        const Vector2 target = ScreenCenterOf(dummy);
        const float dx = target.x - origin.x;
        const float dy = target.y - origin.y;
        shot->distanceTraveled = std::sqrt(dx * dx + dy * dy);
      }
      ecs.progress();
      iceHit = dummy.get<Vitals>()->health < beforeIce;
    }
  }

  bool iceCooldownClear = true;
  if (iceSlot >= 0) {
    const SkillDef *ice = FindSkillDef(playerEntity.world(), "ice_lance");
    if (ice != nullptr && ice->activation.cooldownSeconds <= 0.0f &&
        ice->activation.gcd && ice->activation.gcdSeconds == 0.0f) {
      const SkillCooldowns *cooldowns = playerEntity.get<SkillCooldowns>();
      const GlobalCooldown *gcd = playerEntity.get<GlobalCooldown>();
      iceCooldownClear =
          cooldowns != nullptr &&
          cooldowns->RemainingFor("ice_lance") <= 0.0f &&
          gcd != nullptr && gcd->remaining <= 0.0f;
    }
  }

  // NPC spawn, stance, strategy, context, duplicate-name, corpse and icon checks.
  bool npcSpawned = false;
  bool npcRetaliated = false;
  bool npcBrainless = false;
  bool npcBrainDriven = false;
  bool npcDuplicateName = false;
  bool npcSprite = false;
  bool npcStrategy = false;
  bool npcEntityInfoWindow = false;
  bool npcBrainContextWindow = false;
  bool npcChase = false;
  bool npcPendingSpawn = false;
  bool corpseSpawned = false;
  bool corpseNonInteractable = false;
  bool corpseHoldsLoot = false;
  bool corpseHasDeathSprite = false;
  bool deathIconSwitch = false;
  bool iconRegistryReady = false;
  bool iconFallback = false;

  GamePosition npcPos = beside;
  if (map != nullptr) {
    const int offsets[][2] = {{-1, 0}, {1, 0},  {0, -1}, {0, 1},
                              {-1, -1}, {1, 1}, {-1, 1}, {1, -1}};
    for (const auto &offset : offsets) {
      const GamePosition candidate = {beside.x + offset[0],
                                      beside.y + offset[1]};
      if (map->IsInBounds(static_cast<float>(candidate.x),
                          static_cast<float>(candidate.y)) &&
          !map->IsBlocked(candidate.x, candidate.y)) {
        npcPos = candidate;
        break;
      }
    }
  }
  {
    NPCData data;
    data.position = npcPos;
    data.npcTemplate = "wolf";
    ResolveNpcTemplate(data);
    data.disposition = "neutral";
    data.retaliates = true;
    flecs::entity npc = SpawnNpc(data);
    if (npc.is_alive()) {
      npcSpawned = npc.has<Vitals>() && npc.has<MaxSpeed>() &&
                   npc.has<NpcDisposition>() && npc.has<Retaliates>() &&
                   npc.has<Aggressive>();
      npcBrainless = !npc.has<AgentBrainWrapper>();
      npcSprite = npc.has<Sprite>();
      const CombatStrategy *strategy = npc.get<CombatStrategy>();
      npcStrategy = strategy != nullptr && strategy->id == "chase";
      const WindowOnClick *clickWindow = npc.get<WindowOnClick>();
      npcEntityInfoWindow =
          clickWindow != nullptr &&
          clickWindow->type == WindowType::EntityInfoWindowType;
      if (npc.has<Vitals>()) {
        const float before = npc.get<Vitals>()->health;
        ApplyHit(playerEntity, npc, 1, "", "Test", true, false);
        const Vitals *after = npc.get<Vitals>();
        const NpcDisposition *stance = npc.get<NpcDisposition>();
        npcRetaliated = after != nullptr && after->health < before &&
                        stance != nullptr &&
                        stance->value == Disposition::Aggressive;
      }

      NPCData duplicate = data;
      flecs::entity second = SpawnNpc(duplicate);
      if (second.is_alive()) {
        npcDuplicateName = true;
        std::vector<flecs::entity> secondHeld;
        second.each<Holds>(
            [&](flecs::entity child) { secondHeld.push_back(child); });
        for (flecs::entity child : secondHeld) {
          if (child.is_alive()) child.destruct();
        }
        second.destruct();
      }

      std::vector<flecs::entity> held;
      npc.each<Holds>([&](flecs::entity child) { held.push_back(child); });
      for (flecs::entity child : held) {
        if (child.is_alive()) child.destruct();
      }
      npc.destruct();
    }
  }
  {
    pendingNpcSpawn = PendingNpcSpawn{};
    pendingNpcSpawn.active = true;
    pendingNpcSpawn.npcTemplate = "wolf";
    pendingNpcSpawn.suppressBrain = true;
    pendingNpcSpawn.aggressive = true;
    pendingNpcSpawn.retaliates = false;
    SpawnPendingNpcAt(npcPos);

    flecs::entity spawned = flecs::entity::null();
    ecs.filter<Actor, GamePosition, NpcDisposition>().each(
        [&](flecs::entity entity, const Actor&, const GamePosition& position,
            NpcDisposition& disposition) {
          if (spawned.is_alive() || position.x != npcPos.x ||
              position.y != npcPos.y) {
            return;
          }
          if (disposition.value == Disposition::Aggressive &&
              !entity.has<AgentBrainWrapper>()) {
            spawned = entity;
          }
        });
    if (spawned.is_alive()) {
      npcPendingSpawn = true;
      std::vector<flecs::entity> held;
      spawned.each<Holds>([&](flecs::entity child) { held.push_back(child); });
      for (flecs::entity child : held) {
        if (child.is_alive()) child.destruct();
      }
      spawned.destruct();
    }
  }
  {
    NPCData data;
    data.position = npcPos;
    data.npcTemplate = "blacksmith";
    ResolveNpcTemplate(data);
    data.disposition = "neutral";
    data.retaliates = false;
    flecs::entity npc = SpawnNpc(data);
    if (npc.is_alive()) {
      npcBrainDriven = npc.has<AgentBrainWrapper>() && npc.has<Vitals>();
      const WindowOnClick *clickWindow = npc.get<WindowOnClick>();
      npcBrainContextWindow =
          clickWindow != nullptr &&
          clickWindow->type == WindowType::NPCContextWindowType;
      std::vector<flecs::entity> held;
      npc.each<Holds>([&](flecs::entity child) { held.push_back(child); });
      for (flecs::entity child : held) {
        if (child.is_alive()) child.destruct();
      }
      npc.destruct();
    }
  }
  {
    GamePosition chasePos = beside;
    bool found = false;
    if (map != nullptr) {
      for (int radius = 3; radius <= 8 && !found; ++radius) {
        for (int dx = -radius; dx <= radius && !found; ++dx) {
          for (int dy = -radius; dy <= radius && !found; ++dy) {
            if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
            const GamePosition candidate = {beside.x + dx, beside.y + dy};
            if (!map->IsInBounds(static_cast<float>(candidate.x),
                                 static_cast<float>(candidate.y)) ||
                map->IsBlocked(candidate.x, candidate.y)) {
              continue;
            }
            chasePos = candidate;
            found = true;
          }
        }
      }
    }
    if (found) {
      NPCData data;
      data.position = chasePos;
      data.npcTemplate = "wolf";
      ResolveNpcTemplate(data);
      data.disposition = "aggressive";
      data.retaliates = false;
      data.combatStrategy = "chase";
      flecs::entity chaser = SpawnNpc(data);
      if (chaser.is_alive()) {
        ecs.progress();
        npcChase = chaser.has<CombatStrategy>() &&
                   chaser.has<MOVE_THROUGH_PATH_ACTION>();
        std::vector<flecs::entity> held;
        chaser.each<Holds>([&](flecs::entity child) { held.push_back(child); });
        for (flecs::entity child : held) {
          if (child.is_alive()) child.destruct();
        }
        chaser.destruct();
      }
    }
  }
  {
    NPCData data;
    data.position = npcPos;
    data.npcTemplate = "wolf";
    ResolveNpcTemplate(data);
    data.disposition = "neutral";
    data.retaliates = false;
    data.inventory.push_back({"bread", 1});
    flecs::entity victim = SpawnNpc(data);
    if (victim.is_alive()) {
      if (Vitals *vitals = victim.get_mut<Vitals>()) {
        vitals->health = 0.0f;
      }
      ecs.progress();

      flecs::entity deadCorpse = flecs::entity::null();
      ecs.filter<Corpse, GamePosition>().each(
          [&](flecs::entity entity, const Corpse &,
              const GamePosition &position) {
            if (!deadCorpse.is_alive() && position.x == npcPos.x &&
                position.y == npcPos.y) {
              deadCorpse = entity;
            }
          });
      if (deadCorpse.is_alive()) {
        corpseSpawned = true;
        corpseNonInteractable = !deadCorpse.has<Interactable>() &&
                                !deadCorpse.has<Obstacle>();
        const Sprite *corpseSprite = deadCorpse.get<Sprite>();
        corpseHasDeathSprite = corpseSprite != nullptr &&
                               corpseSprite->path.find("corpse.png") !=
                                   std::string::npos;
        int held = 0;
        deadCorpse.each<Holds>([&](flecs::entity) { ++held; });
        corpseHoldsLoot = held > 0;

        MutableSettings().combat.deathIcon = "skill_attack";
        ecs.progress();
        const Sprite *switched = deadCorpse.get<Sprite>();
        deathIconSwitch = switched != nullptr &&
                          switched->path.find("skill_attack") !=
                              std::string::npos;
        MutableSettings().combat.deathIcon = "death";

        std::vector<flecs::entity> corpseItems;
        deadCorpse.each<Holds>(
            [&](flecs::entity child) { corpseItems.push_back(child); });
        for (flecs::entity child : corpseItems) {
          if (child.is_alive()) child.destruct();
        }
        deadCorpse.destruct();
      }
    }
  }
  {
    iconRegistryReady = iconRegistry.Get("missing") != nullptr &&
                        iconRegistry.Get("death") != nullptr &&
                        iconRegistry.Get("skill_attack") != nullptr;
    std::string skillPath;
    const bool skillOk = IconPaths::ResolveIconPath(
        iconRegistry, "skill_attack", resourceManager, debugLog.get(), &skillPath);
    std::string missingPath;
    const bool missingOk = IconPaths::ResolveIconPath(
        iconRegistry, "does_not_exist", resourceManager, debugLog.get(),
        &missingPath);
    iconFallback = skillOk && skillPath.find("skill_attack") != std::string::npos &&
                   missingOk && missingPath.find("unknown") != std::string::npos;
  }

  // Print the combat lines the two calls produced, so the output shows the same
  // text a player would read in the Log window.
  if (debugLog) {
    for (const DebugLog::LogEntry &entry : debugLog->GetEntries()) {
      if (entry.category == DebugLog::LogCategory::Combat) {
        std::cout << "  [combat] " << entry.message << "\n";
      }
    }
  }

  std::cout << "combat-check: attack slot = " << attackSlot << " ("
            << barIds[static_cast<size_t>(attackSlot)] << ")\n";
  if (hasCastSkill) {
    std::cout << "combat-check: cast slot = " << heavySlot << " ("
              << barIds[static_cast<size_t>(heavySlot)] << ")\n";
  }
  if (hasKick) {
    std::cout << "combat-check: kick slot = " << kickSlot << " ("
              << barIds[static_cast<size_t>(kickSlot)] << ")\n";
  }
  if (fireballSlot >= 0) {
    std::cout << "combat-check: fireball slot = " << fireballSlot << " ("
              << barIds[static_cast<size_t>(fireballSlot)] << ")\n";
  }
  if (iceSlot >= 0) {
    std::cout << "combat-check: ice lance slot = " << iceSlot << " ("
              << barIds[static_cast<size_t>(iceSlot)] << ")\n";
  }
  std::cout << "combat-check: first swing used=" << first
            << " damage=" << (healthBefore - healthAfter)
            << " dummy=" << healthAfter << "/" << dummy.get<Vitals>()->maxHealth
            << "\n";
  std::cout << "combat-check: immediate second swing used=" << second
            << " (expected 0: cooldown)\n";
  std::cout << "combat-check: cast started=" << castStarted
            << " resolved=" << castResolved
            << " range-indicator=" << castRangeIndicator
            << " range-cancel-started=" << castRangeStarted
            << " range-canceled=" << castRangeCanceled
            << " movement-canceled=" << movementCanceled << "\n";
  std::cout << "combat-check: kick used=" << kickUsed
            << " interrupted=" << interrupted << "\n";
  std::cout << "combat-check: fireball used=" << fireballUsed
            << " impact=" << fireballHit << "\n";
  std::cout << "combat-check: ice lance used=" << iceUsed
            << " impact=" << iceHit
            << " cooldown-clear=" << iceCooldownClear << "\n";
  std::cout << "combat-check: npc spawned=" << npcSpawned
            << " retaliated=" << npcRetaliated
            << " brainless=" << npcBrainless
            << " brain-driven=" << npcBrainDriven
            << " pending-spawn=" << npcPendingSpawn
            << " duplicate-name=" << npcDuplicateName
            << " sprite=" << npcSprite
            << " strategy=" << npcStrategy
            << " entity-info=" << npcEntityInfoWindow
            << " brain-context=" << npcBrainContextWindow
            << " chase=" << npcChase
            << " corpse=" << corpseSpawned
            << " corpse-passive=" << corpseNonInteractable
            << " corpse-loot=" << corpseHoldsLoot
            << " corpse-sprite=" << corpseHasDeathSprite
            << " death-icon-switch=" << deathIconSwitch
            << " icons=" << iconRegistryReady
            << " icon-fallback=" << iconFallback << "\n";

  const bool ok = first && !second && healthAfter < healthBefore &&
                  castStarted && castResolved && !playerEntity.has<Casting>() &&
                  castRangeIndicator && castRangeStarted && castRangeCanceled &&
                  movementCanceled && kickUsed && interrupted &&
                  fireballUsed && fireballHit && iceUsed && iceHit &&
                  iceCooldownClear && npcSpawned && npcRetaliated &&
                  npcBrainless && npcBrainDriven && npcPendingSpawn &&
                  npcDuplicateName && npcSprite && npcStrategy &&
                  npcEntityInfoWindow && npcBrainContextWindow && npcChase &&
                  corpseSpawned && corpseNonInteractable && corpseHoldsLoot &&
                  corpseHasDeathSprite && deathIconSwitch &&
                  iconRegistryReady && iconFallback;
  std::cout << (ok ? "combat-check: OK\n" : "combat-check: FAILED\n");
  return ok ? 0 : 1;
}
