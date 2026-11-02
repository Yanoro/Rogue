// GameECSInitSystems.cpp — agent, task and tile-art system registration.
//
// Part of the GameECS group (see src/GameECSInternals.h). Hosts the AI response
// queue (g_AIResponseMutex / g_AIResponseQueue): the background-thread handoff
// the agent systems drain on the main thread.

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

static std::mutex g_AIResponseMutex;
static std::vector<std::function<void()>> g_AIResponseQueue;
// The main-thread half of the tile-art queue: a swap happens here and nowhere else.
//
// Registered unconditionally, even when generation is off, so the queue's state
// machine has exactly one pump whether or not anything is in it -- a system that
// only exists when a setting is on is a system whose behaviour differs between two
// otherwise identical runs.
//
// One iter system rather than an each: this is a per-frame pump, not per-entity, and
// it is placed here (with the agent systems) because the AI handoff is the shape it
// mirrors: a worker thread produces, the main thread applies.
void Game::ECSInitTileArtSystems() {
  ecs.system("TileArtQueuePump").iter([this](flecs::iter &it) {
    // Unscaled seconds, matching the convention the other per-frame systems use
    // (see the delta-time note in .agents/known-issues.md #3). The queue only
    // accumulates this against a batch window, so the unit is local to it.
    PumpTileArtQueue(it.delta_time());
  });
}

void Game::ECSInitTaskSystems() {
  // The checking pass (docs/task-system-design.md section 8). A logic system, so
  // it runs in progress() with the other update systems and never in the render
  // pipeline.
  //
  // Interval rather than every frame: each pass resolves a target by name, which
  // is a world-wide query per task, and nothing here is time-critical -- the
  // result is shown to the agent only when it wakes. The modulo is on the world
  // clock, so it needs no per-agent timer component and cannot drift.
  ecs.system<TaskList>("TaskVerificationSystem")
      .each([](flecs::entity agent, TaskList &tasks) {
        if (DEFAULT_TASK_VERIFY_INTERVAL_TICKS > 0 &&
            CurrentTaskTick(agent.world()) % DEFAULT_TASK_VERIFY_INTERVAL_TICKS !=
                0) {
          return;
        }
        VerifyAgentTaskList(agent, tasks, CurrentTaskTick(agent.world()));
      });
}

void Game::ECSInitAgentSystems() {  // System to flush background AI thread callbacks on the main thread safely
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

  // Cooldowns tick before auto-cast in registration order, so a skill that came
  // off cooldown this frame can be used this frame rather than next.
  ecs.system<SkillCooldowns, GlobalCooldown>("SkillCooldownSystem")
      .iter([](flecs::iter& it, SkillCooldowns* cooldowns,
               GlobalCooldown* globalCooldowns) {
        const float dt = it.delta_time();
        for (auto i : it) {
          for (SkillCooldown& entry : cooldowns[i].cooldowns) {
            if (entry.remaining > 0.0f) {
              entry.remaining = std::max(0.0f, entry.remaining - dt);
            }
          }
          if (globalCooldowns[i].remaining > 0.0f) {
            globalCooldowns[i].remaining =
                std::max(0.0f, globalCooldowns[i].remaining - dt);
          }
        }
      });

  // "Always casts": fire every auto-cast skill whose target is valid, in bar
  // order. One global-cooldown skill per frame is enough -- the GCD it starts
  // blocks the rest on its own -- while the basic attack, being exempt, can fire
  // alongside it.
  //
  // This is what lets a fight continue while the player is deciding, and it is
  // the same mechanism an agent will use later, so it is written against
  // components rather than against the player entity.
  ecs.system<LockedTarget, Vitals, SkillCooldowns, GlobalCooldown,
             AutoCastSkills, SkillBar>("CombatAutoCastSystem")
      .each([this](flecs::entity entity, LockedTarget& lock, Vitals&,
                   SkillCooldowns& cooldowns, GlobalCooldown& globalCooldown,
                   AutoCastSkills& autoCast, SkillBar& bar) {
        flecs::entity target = lock.target;
        if (!target.is_alive() || target == entity || !target.has<Vitals>()) {
          // A dead or destroyed target is a normal end of a lock, not an error:
          // drop it instead of holding a dangling handle.
          if (!target.is_alive()) {
            lock.target = flecs::entity::null();
          }
          return;
        }

        // A committed cast owns the character until it resolves; auto-casting
        // another skill would either queue behind it or silently cancel it.
        if (entity.has<Casting>()) {
          return;
        }

        for (const std::string& skillId : bar.ids) {
          if (!autoCast.Has(skillId)) {
            continue;
          }
          const SkillDef* def = FindSkillDef(entity.world(), skillId);
          if (def == nullptr || !SkillIsActivatable(def->type)) {
            continue;
          }
          if (!cooldowns.IsReady(skillId)) {
            continue;
          }
          if (def->activation.gcd && globalCooldown.remaining > 0.0f) {
            continue;
          }
          if (def->activation.requiresTarget &&
              !WithinActivationRange(entity, target,
                                     def->activation.rangeTiles)) {
            continue;
          }

          const SkillUseOutcome outcome =
              UseSkill(entity, skillId, target, entity == playerEntity, false);
          if (!outcome.used) {
            continue;
          }

          if (debugLog) {
            debugLog->LogCombat(outcome.message);
          }
          if (entity != playerEntity && entity.has<AgentBrainWrapper>()) {
            if (auto* wrapper = entity.get_mut<AgentBrainWrapper>()) {
              if (wrapper->agBrain) {
                wrapper->agBrain->appendContext(
                    "user", "System: " + outcome.message + "\n");
              }
            }
          }

          // The GCD the skill just started blocks every other GCD skill; the
          // loop ends here rather than continuing to check them.
          if (def->activation.gcd) {
            break;
          }
        }
      });

  // The smallest hostile controller: a timer, a range, and one telegraphed
  // strike. It exists so incoming damage and interrupts are testable without the
  // LLM path; a real hostile agent will not carry this component.
  ecs.system<Aggressive, Vitals, GamePosition>("AggressiveCombatSystem")
      .iter([this](flecs::iter& it, Aggressive* aggressive, Vitals* vitals,
                   GamePosition* positions) {
        if (!playerEntity.is_alive()) {
          return;
        }
        const GamePosition* playerPosition = playerEntity.get<GamePosition>();
        if (playerPosition == nullptr) {
          return;
        }
        const float dt = it.delta_time();
        for (auto i : it) {
          flecs::entity entity = it.entity(i);
          if (vitals[i].health <= 0.0f || entity.has<Casting>()) {
            continue;
          }
          if (aggressive[i].timer > 0.0f) {
            aggressive[i].timer = std::max(0.0f, aggressive[i].timer - dt);
            if (aggressive[i].timer > 0.0f) {
              continue;
            }
          }
          const int dx = positions[i].x - playerPosition->x;
          const int dy = positions[i].y - playerPosition->y;
          const int steps = std::max(std::abs(dx), std::abs(dy));
          if (static_cast<float>(steps) > aggressive[i].range) {
            continue;
          }

          const std::string label = aggressive[i].label;
          const float castSeconds = aggressive[i].castSeconds;
          const float damage = aggressive[i].damage;
          aggressive[i].timer = aggressive[i].interval;

          ecs.defer([this, entity, label, castSeconds, damage]() {
            // The captured handle is const inside a defer lambda; a local copy
            // is what lets the deferred writes go through.
            flecs::entity caster = entity;
            flecs::entity action =
                ecs.entity()
                    .set<ActionTimer>({castSeconds})
                    .set<ActionActor>({caster})
                    .set<ActionTarget>({playerEntity})
                    .set<CastAction>(
                        {std::string(), label, damage, true, false});
            caster.set<Casting>({action, std::string(), label, true});
            caster.set<Busy>({action});
          });

          if (debugLog) {
            debugLog->LogCombat(EntityLabel(entity, "The opponent") +
                                " begins " + label + ".");
          }
        }
      });

  // Applies the effect of a cast whose timer completed. Any caster can carry a
  // CastAction -- the player's skill cast and the aggressive dummy's inline
  // strike share this one resolution path.
  ecs.system<ActionActor, CastAction>("CastResolutionSystem")
      .with<ActionCompleted>()
      .each([this](flecs::entity actionEntity, ActionActor& a,
                   CastAction& cast) {
        flecs::entity actor = a.actor;
        flecs::entity target = flecs::entity::null();
        if (const ActionTarget* t = actionEntity.get<ActionTarget>()) {
          target = t->target;
        }

        if (actor.is_alive()) {
          actor.remove<Casting>();
          if (const Busy* busy = actor.get<Busy>()) {
            if (busy->actionEntity == actionEntity) {
              actor.remove<Busy>();
            }
          }

          std::string message;
          bool landed = false;
          if (!cast.skillId.empty()) {
            const SkillDef* def = FindSkillDef(actor.world(), cast.skillId);
            if (def == nullptr) {
              message = "The cast fizzles: the skill no longer exists.";
            } else if (def->activation.requiresTarget &&
                       (!target.is_alive() ||
                        !WithinActivationRange(actor, target,
                                               def->activation.rangeTiles))) {
              message = "The " + def->name +
                        " fizzles: the target is out of reach.";
            } else if (!def->activation.skillshot.empty()) {
              // An aimed cast launches its path here. The direction was locked
              // when the cast began, so moving the cursor during the wind-up
              // cannot steer a committed lance.
              if (SpawnSkillshot(actor, *def, cast.baseDamage, cast.direction,
                                 cast.playerCast)) {
                message = (cast.playerCast ? std::string("You")
                                           : EntityLabel(actor, "Someone")) +
                          " finish casting " + def->name + ".";
                message += FinalizeSkillUse(actor, *def);
              } else {
                message = "The " + def->name + " fizzles: its skillshot is not loaded.";
              }
            } else if (def->activation.requiresTarget) {
              // A cast that launches a projectile finishes here but lands later:
              // the flight is the delivery, and ProjectileUpdateSystem applies
              // the hit. No completion call-out yet -- the object is still in the
              // air.
              if (!def->activation.projectile.empty() &&
                  SpawnSkillProjectile(actor, target, *def, cast.baseDamage,
                                       cast.playerCast)) {
                message = (cast.playerCast ? std::string("You")
                                           : EntityLabel(actor, "Someone")) +
                          (cast.playerCast ? " hurl " : " hurls ") +
                          def->name + " at " +
                          EntityLabel(target, "the target") + ".";
                message += FinalizeSkillUse(actor, *def);
              } else {
                message = ApplyHit(actor, target, cast.baseDamage, cast.skillId,
                                   def->name, cast.playerCast,
                                   def->activation.interrupts);
                message += FinalizeSkillUse(actor, *def);
                landed = true;
              }
            } else {
              message = (cast.playerCast ? std::string("You")
                                         : EntityLabel(actor, "Someone")) +
                        " finish casting " + def->name + ".";
              message += FinalizeSkillUse(actor, *def);
              landed = true;
            }
          } else if (target.is_alive() && target.has<Vitals>()) {
            message = ApplyHit(actor, target, cast.baseDamage, "", cast.label,
                               false, false);
            landed = true;
          } else {
            message = cast.label + " finds nothing.";
          }

          // A completed cast says its name over the caster, the same call-out an
          // instant use gives, so a landed cast is legible without reading the
          // log. A fizzle deliberately says nothing: no name, no reward.
          if (landed) {
            SpawnFloatingCallout(actor, cast.label,
                                 DEFAULT_ABILITY_CALL_OUT_COLOR);
          }

          if (debugLog && !message.empty()) {
            debugLog->LogCombat(message);
          }
          if (actor != playerEntity && actor.has<AgentBrainWrapper>()) {
            if (auto* wrapper = actor.get_mut<AgentBrainWrapper>()) {
              if (wrapper->agBrain) {
                wrapper->agBrain->appendContext(
                    "user", "System: " + message + "\n");
              }
            }
          }
        }

        actionEntity.destruct();
      });

  // Moves every projectile toward its target and applies the impact on arrival.
  // Delivery only: the damage, the interrupt and the floating numbers all come
  // from ApplyHit, exactly as a melee hit does.
  ecs.system<Projectile, ScreenPosition>("ProjectileUpdateSystem").iter(
      [this](flecs::iter& it, Projectile* projectiles,
             ScreenPosition* positions) {
        const float dt = it.delta_time();
        for (auto i : it) {
          flecs::entity entity = it.entity(i);
          Projectile& projectile = projectiles[i];

          if (!projectile.target.is_alive() ||
              !projectile.target.has<ScreenPosition>()) {
            if (debugLog) {
              debugLog->LogCombat(projectile.label + " fizzles.");
            }
            entity.destruct();
            continue;
          }

          const Vector2 targetCenter = ScreenCenterOf(projectile.target);
          Vector2 position = {positions[i].x, positions[i].y};
          const Vector2 delta = {targetCenter.x - position.x,
                                 targetCenter.y - position.y};
          const float distance =
              std::sqrt(delta.x * delta.x + delta.y * delta.y);
          const float step = projectile.speed * std::max(0.0f, dt);

          if (distance <= projectile.size * 0.5f ||
              (step > 0.0f && distance <= step)) {
            const std::string message = ApplyHit(
                projectile.owner, projectile.target, projectile.baseDamage,
                projectile.skillId, projectile.label, projectile.playerCast,
                projectile.interrupts);
            if (debugLog) {
              debugLog->LogCombat(message);
            }
            entity.destruct();
            continue;
          }

          if (step > 0.0f && distance > 0.0f) {
            positions[i].x += delta.x / distance * step;
            positions[i].y += delta.y / distance * step;
          }
        }
      });

  // Sweeps every skillshot along its sampled path and hits the first Actor whose
  // body the segment crosses. Path-agnostic: an arc or a spread from a future
  // generator goes through the same loop because both are just polylines.
  ecs.system<Skillshot>("SkillshotUpdateSystem").iter(
      [this](flecs::iter& it, Skillshot* skillshots) {
        const float dt = it.delta_time();

        std::vector<flecs::entity> targets;
        ecs.filter<Actor, Vitals, ScreenPosition>().each(
            [&](flecs::entity target, const Actor&, Vitals&,
                const ScreenPosition&) { targets.push_back(target); });

        for (auto i : it) {
          flecs::entity entity = it.entity(i);
          Skillshot& skillshot = skillshots[i];

          const float totalLength = SkillshotPathLength(skillshot.path);
          if (totalLength <= 0.001f) {
            entity.destruct();
            continue;
          }

          const float oldDistance =
              std::clamp(skillshot.distanceTraveled, 0.0f, totalLength);
          const float step = std::max(0.0f, skillshot.speed * dt);
          const float newDistance =
              std::min(totalLength, oldDistance + step);
          const Vector2 start = SkillshotPointAt(skillshot.path, oldDistance);
          const Vector2 end = SkillshotPointAt(skillshot.path, newDistance);

          flecs::entity hit = flecs::entity::null();
          float bestT = 2.0f;
          for (flecs::entity target : targets) {
            if (target == skillshot.owner || !target.is_alive()) {
              continue;
            }
            const Vitals *vitals = target.get<Vitals>();
            if (vitals == nullptr || vitals->health <= 0.0f) {
              continue;
            }
            float t = 0.0f;
            const float distance = SkillshotPointSegmentDistance(
                ScreenCenterOf(target), start, end, &t);
            const float radius =
                EntityHitRadius(target) + skillshot.width * 0.5f;
            if (distance <= radius && t < bestT) {
              bestT = t;
              hit = target;
            }
          }

          if (hit.is_alive()) {
            const std::string message = ApplyHit(
                skillshot.owner, hit, skillshot.damage, skillshot.skillId,
                skillshot.label, skillshot.playerCast, skillshot.interrupts);
            if (debugLog) {
              debugLog->LogCombat(message);
            }
            entity.destruct();
            continue;
          }

          skillshot.distanceTraveled = newDistance;
          if (skillshot.distanceTraveled >= totalLength) {
            entity.destruct();
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
