#pragma once

// Declarations shared by the GameECS translation units: GameECS.cpp,
// GameECSCombat.cpp, GameECSInitSystems.cpp and GameECSInit.cpp.
//
// These were file-local (static / anonymous namespace) helpers when all of this
// lived in one 5.7k-line GameECS.cpp. That single translation unit cost ~54 s and
// 2.7 GiB to compile, dominated the whole build, and got compilers OOM-killed at
// high -j; it is now split. Everything here is internal to the GameECS group and
// is deliberately NOT in include/ -- do not include it from elsewhere.

#include "Components.h"
#include "DebugLog.h"
#include "Defaults.h"
#include "Hooks.hpp"
#include "ObjectFactory.h"
#include "RaceRegistry.h"
#include "RecipeRegistry.h"
#include "ResourceManager.h"
#include "Rng.hpp"
#include "SkillRegistry.h"
#include "SkillTraining.hpp"
#include "StartingSkills.hpp"
#include "StatRegistry.h"

// --- Character setup helpers, defined in GameECS.cpp --------------------------
void GiveDefaultStats(flecs::entity character, const StatRegistry *registry);
void GiveStartingSkills(flecs::entity character, const SkillRegistry *registry);
void ApplyStartingStats(flecs::entity character,
                        const std::vector<StatValue> &starting,
                        const StatRegistry *registry, const std::string &name,
                        DebugLog *debugLog);
void ApplyStartingSkills(flecs::entity character, const SkillRegistry *registry,
                         const std::vector<StartingSkill> &startingSkills,
                         DebugLog *debugLog);
void GiveBody(flecs::entity character, const RaceRegistry *races,
              const std::string &raceId, const std::vector<SlotSpec> &authored,
              const std::string &authorizedSprite, ResourceManager &resources,
              DebugLog *debugLog);

// --- Combat / crafting helpers, defined in GameECSCombat.cpp ------------------

// Every input entity a craft is about to spend, plus the human-readable shortage
// when the actor cannot pay the full cost. `items` is empty exactly when
// `missing` is non-empty.
struct RecipeInputClaim {
  std::vector<flecs::entity> items;
  std::string missing;
};

// The registries every resolution needs, gathered in one place so a call site
// cannot forget one and silently resolve as if nothing had any effect. Any of
// them may be absent in a half-initialised world; the fallbacks keep the failure
// mode "nothing changes" rather than "nothing works".
struct ResolutionInputs {
  SourceDefinitions definitions;
  const HookRegistry *hooks = nullptr;
};

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


struct SkillUseOutcome {
  bool used = false;
  std::string message;
};

// What one execution of a recipe produced, plus any skill progress it earned.
struct RecipeOutcome {
  std::string produced; // empty when every chance failed
  std::vector<SkillGain> training;
};

RecipeInputClaim ClaimRecipeInputs(flecs::entity actor,
                                   const CraftRecipe &recipe, int batches = 1);
Rng NextRollStream(flecs::world world);
std::vector<std::string> RollLootTable(flecs::entity actor,
                                       const std::vector<LootEntry> &entries,
                                       ObjectFactory *factory, Rng &rng);
ResolutionInputs MakeResolutionInputs(flecs::world world);
std::string JoinItemTypes(const std::vector<std::string> &itemTypes);
std::vector<SkillGain> ApplySubjectTraining(flecs::entity actor,
                                            const std::vector<SkillXp> &grants,
                                            const std::string &activity);
std::vector<std::string> HarvestDrops(flecs::entity actor, flecs::entity target,
                                      const std::vector<LootDrop> &base);
HarvestOutcome HarvestResult(flecs::entity actor, flecs::entity target,
                             const std::vector<LootDrop> &base);
std::string FormatBrokenTools(const std::vector<std::string> &broken);
const SkillDef *FindSkillDef(flecs::world world, const std::string &id);
std::string EntityLabel(flecs::entity entity, const char *fallback);
int GridSteps(const GamePosition &a, const GamePosition &b);
bool WithinActivationRange(flecs::entity actor, flecs::entity target,
                           float range);
std::string CooldownLabel(float seconds);
bool InterruptCast(flecs::entity target);
// Cancels a committed cast for an invalid target or movement. Not gated on
// damage-interruptibility; the cooldown is already spent.
bool CancelCast(flecs::entity target, const std::string &callout);
// Stops both movement sources (held direction and clicked path) before a
// movement-locked cast begins.
void StopPlayerMovement(flecs::entity actor);
// Free-function surface entry points for skill/projectile resolution, which has
// no Game pointer. They resolve the GameRuntimeResource and delegate to Game.
void AddSurfaceAt(flecs::world world, GamePosition position,
                  const std::string &surfaceId, flecs::entity source,
                  float durationOverride = 0.0f, int radius = 0);
void RemoveSurfaceAt(flecs::world world, GamePosition position,
                     const std::string &surfaceId = std::string());
// Environmental damage bypasses armour/fold but still carries the origin source
// for retaliation, floating text and the normal death system.
void ApplyEffectDamage(flecs::entity source, flecs::entity target, float amount,
                       const std::string &label = std::string());
std::string ApplyHit(flecs::entity actor, flecs::entity target, float baseDamage,
                     const std::string &subjectId, const std::string &label,
                     bool secondPerson, bool interrupts);
std::string FinalizeSkillUse(flecs::entity actor, const SkillDef &def);
Vector2 ScreenCenterOf(flecs::entity entity);
float EntityHitRadius(flecs::entity entity);
bool SpawnSkillProjectile(flecs::entity actor, flecs::entity target,
                          const SkillDef &def, float baseDamage,
                          bool playerCast);
bool SpawnSkillshot(flecs::entity actor, const SkillDef &def, float damage,
                    Vector2 direction, bool playerCast);
SkillUseOutcome UseSkill(flecs::entity actor, const std::string &skillId,
                         flecs::entity target, bool secondPerson,
                         bool announce, Vector2 direction = {});
RecipeOutcome ProduceRecipeOutputs(flecs::entity actor, const CraftRecipe &recipe,
                                   ObjectFactory *factory, Rng &rng);
std::string FormatBatchedLabel(int batches, const std::string &label);
