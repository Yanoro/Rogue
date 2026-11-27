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
#include "IconPaths.hpp"
#include "SkillshotPath.hpp"
#include "GapLog.h"
#include "LogPaths.h"
#include "Components.h"
#include "ResourceSystem.h"
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
      flecs::entity entity = it.entity(i);
      const NpcDisposition *disposition = entity.get<NpcDisposition>();
      if (entity.has<DashState>() ||
          (entity.has<NpcBehaviorId>() && disposition != nullptr &&
           disposition->value == Disposition::Aggressive)) {
        continue;
      }
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
  ecs.system<LockedTarget, ResourcePools, SkillCooldowns, GlobalCooldown,
             AutoCastSkills, SkillBar>("CombatAutoCastSystem")
      .each([this](flecs::entity entity, LockedTarget& lock, ResourcePools&,
                   SkillCooldowns& cooldowns, GlobalCooldown& globalCooldown,
                   AutoCastSkills& autoCast, SkillBar& bar) {
        flecs::entity target = lock.target;
        if (target.is_alive()) Resources::EnsureInitialized(target);
        if (!target.is_alive() || target == entity ||
            !Resources::IsDamageable(target)) {
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

        // Copy the ids: a successful UseSkill commits a cast, which adds
        // Casting/Busy and moves the entity's archetype, invalidating `bar`.
        const std::vector<std::string> barIds = bar.ids;
        for (const std::string& skillId : barIds) {
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

          // A committed cast adds Casting/Busy and moves the entity's
          // archetype; the cooldown references captured by this system are no
          // longer safe. One cast per frame is the intended cadence anyway.
          if (entity.has<Casting>()) {
            return;
          }
          // The GCD the skill just started blocks every other GCD skill; the
          // loop ends here rather than continuing to check them.
          if (def->activation.gcd) {
            break;
          }
        }
      });

  // A targeted cast is a commitment to a target, not just to a timer. Re-check
  // the distance every frame while the timer is live; if the target steps outside
  // the cast range (or the activation range when none is authored) the cast is
  // canceled immediately instead of waiting for the completion fizzle.
  ecs.system<ActionActor, CastAction, ActionTimer>("CastRangeCancelSystem")
      .each([this](flecs::entity actionEntity, ActionActor& actorRef,
                   CastAction& cast, const ActionTimer&) {
        if (!cast.requiresTarget) {
          return;
        }
        flecs::entity actor = actorRef.actor;
        if (!actor.is_alive()) {
          return;
        }
        const Casting* casting = actor.get<Casting>();
        if (casting == nullptr || casting->actionEntity != actionEntity) {
          return;
        }
        const ActionTarget* targetRef = actionEntity.get<ActionTarget>();
        const bool targetLost =
            targetRef == nullptr || !targetRef->target.is_alive();
        const float castRange =
            cast.castRangeTiles > 0.0f ? cast.castRangeTiles : cast.rangeTiles;
        const bool outOfRange =
            !targetLost &&
            !WithinActivationRange(actor, targetRef->target, castRange);
        if (!targetLost && !outOfRange) {
          return;
        }
        if (!CancelCast(actor, targetLost ? "Skill Canceled: Target Lost"
                                          : "Skill Canceled: Out of Range")) {
          return;
        }
        if (debugLog) {
          const std::string owner =
              cast.playerCast ? std::string("Your ")
                              : EntityLabel(actor, "Someone") + "'s ";
          debugLog->LogCombat(
              owner + cast.label +
              (targetLost ? " is canceled: the target is gone."
                          : " is canceled: the target moved out of range."));
        }
      });

  // A movement-locked cast stops the player at activation. If a movement key
  // was already held at that moment, it is ignored until it returns to neutral;
  // otherwise casting while walking would cancel itself on the next frame. Once
  // neutral, any new movement input (or a clicked path) cancels the cast.
  ecs.system<Casting>("CastMovementCancelSystem")
      .each([this](flecs::entity actor, Casting& casting) {
        if (!casting.movementLocked || !actor.has<PlayerControlled>()) {
          return;
        }
        const bool input = InputHandler::MovementInputDown();
        const bool path = actor.has<MOVE_THROUGH_PATH_ACTION>();
        const Velocity *velocity = actor.get<Velocity>();
        const bool actualMovement =
            velocity != nullptr &&
            (std::abs(velocity->x) > DEFAULT_MOVEMENT_EPSILON ||
             std::abs(velocity->y) > DEFAULT_MOVEMENT_EPSILON);
        if (!casting.movementInputArmed) {
          if (path) {
            // A clicked path is always a new movement instruction, even while a
            // held key from before the cast is still down.
          } else if (input) {
            return;
          } else {
            casting.movementInputArmed = true;
            return;
          }
        } else if (casting.allowSliding) {
          // A slide is not movement input. Only a new request cancels; the
          // velocity itself is allowed to carry through the cast.
          if (!input && !path) return;
        } else if (!input && !path && !actualMovement) {
          return;
        }
        if (!CancelCast(actor, "Skill Canceled: Moved")) {
          return;
        }
        if (debugLog) {
          debugLog->LogCombat("Your " + casting.label +
                              " is canceled: you moved.");
        }
      });

  // The range overlay is a short-lived visual fact, not a permanent component.
  ecs.system<SkillRangeIndicator>("SkillRangeIndicatorLifetime")
      .iter([](flecs::iter& it, SkillRangeIndicator* indicators) {
        const float dt = it.delta_time();
        for (auto i : it) {
          indicators[i].remaining =
              std::max(0.0f, indicators[i].remaining - dt);
          if (indicators[i].remaining <= 0.0f) {
            it.entity(i).remove<SkillRangeIndicator>();
          }
        }
      });

  // Non-LLM positioning. Only brainless Aggressive bodies execute a strategy;
  // an LLM's agent owns its own movement.
  ecs.system<CombatStrategy, Aggressive, GamePosition, ResourcePools>(
      "CombatStrategySystem")
      .without<AgentBrainWrapper>()
      .without<NpcBehaviorId>()
      .without<DashState>()
      .iter([this](flecs::iter& it, CombatStrategy* strategies,
                   Aggressive*, GamePosition* positions, ResourcePools*) {
        if (!playerEntity.is_alive()) {
          return;
        }
        const GamePosition* playerPosition = playerEntity.get<GamePosition>();
        auto registryRes = ecs.get<CombatStrategyRegistryResource>();
        if (playerPosition == nullptr || registryRes == nullptr ||
            registryRes->registry == nullptr || map == nullptr) {
          return;
        }
        const float dt = it.delta_time();
        for (auto i : it) {
          flecs::entity entity = it.entity(i);
          CombatStrategy& strategy = strategies[i];

          if (!Resources::IsAlive(entity) || entity.has<Casting>()) {
            entity.remove<MOVE_THROUGH_PATH_ACTION>();
            continue;
          }
          if (const NpcDisposition* disposition = entity.get<NpcDisposition>();
              disposition != nullptr &&
              disposition->value == Disposition::Neutral) {
            entity.remove<MOVE_THROUGH_PATH_ACTION>();
            continue;
          }

          const CombatStrategyDef* definition =
              registryRes->registry->Get(strategy.id);
          if (definition == nullptr || definition->behavior != "chase") {
            entity.remove<MOVE_THROUGH_PATH_ACTION>();
            continue;
          }

          const int dx = positions[i].x - playerPosition->x;
          const int dy = positions[i].y - playerPosition->y;
          const int steps = std::max(std::abs(dx), std::abs(dy));
          if (static_cast<float>(steps) <= definition->preferredRange) {
            entity.remove<MOVE_THROUGH_PATH_ACTION>();
            strategy.repathTimer = 0.0f;
            continue;
          }

          strategy.repathTimer = std::max(0.0f, strategy.repathTimer - dt);
          if (strategy.repathTimer > 0.0f &&
              entity.has<MOVE_THROUGH_PATH_ACTION>()) {
            continue;
          }

          strategy.repathTimer = definition->repathSeconds;
          const std::vector<GamePosition> path =
              AStar(map.get(), positions[i], *playerPosition);
          if (!path.empty()) {
            entity.set<MOVE_THROUGH_PATH_ACTION>({path});
          }
        }
      });

  // Skill-driven NPC behavior. Runs only for NPCs with a behavior id; the old
  // Aggressive/CombatStrategy controllers are excluded for those entities.
  ecs.system("NpcBehaviorSystem").iter([this](flecs::iter& it) {
    UpdateNpcBehaviors(it.delta_time());
  });

  // The corpse visual is a setting, not a property baked into the death event.
  ecs.system<Corpse>("CorpseIconSystem")
      .each([this](flecs::entity entity, const Corpse&) {
        std::string iconId = Settings().combat.deathIcon;
        if (const CorpseDeathIcon *custom = entity.get<CorpseDeathIcon>();
            custom != nullptr && !custom->icon.empty()) {
          iconId = custom->icon;
        }
        std::string iconPath;
        if (!IconPaths::ResolveIconPath(iconRegistry, iconId, resourceManager,
                                        debugLog.get(), &iconPath)) {
          return;
        }
        const Sprite *current = entity.get<Sprite>();
        if (current == nullptr || current->path != iconPath) {
          entity.set<Sprite>(Sprite{iconPath});
        }
      });

  // Reusable actor statuses. Surfaces only apply ids; this is the one place
  // duration, periodic damage and expiry are handled.
  ecs.system<StatusEffects>("StatusEffectSystem")
      .iter([this](flecs::iter &it, StatusEffects *statuses) {
        const float dt = it.delta_time();
        struct PendingEffects {
          flecs::entity actor;
          EffectOrigin origin;
          std::vector<std::string> effects;
          GamePosition position;
        };
        std::vector<PendingEffects> pending;
        for (auto i : it) {
          flecs::entity actor = it.entity(i);
          const GamePosition *position = actor.get<GamePosition>();
          std::vector<StatusEffectInstance> &effects = statuses[i].effects;
          for (size_t e = 0; e < effects.size();) {
            StatusEffectInstance &status = effects[e];
            const StatusEffectDef *definition =
                statusEffectRegistry.Get(status.id);
            if (definition == nullptr) {
              effects.erase(effects.begin() + e);
              continue;
            }
            status.remaining -= dt;
            status.calloutCooldown =
                std::max(0.0f, status.calloutCooldown - dt);
            if (status.remaining <= 0.0f) {
              if (position != nullptr && !definition->onExpireEffects.empty()) {
                pending.push_back({actor, status.origin,
                                   definition->onExpireEffects, *position});
              }
              effects.erase(effects.begin() + e);
              continue;
            }
            if (!definition->tickEffects.empty()) {
              status.tickTimer -= dt;
              if (status.tickTimer <= 0.0f) {
                status.tickTimer += definition->tickSeconds;
                if (position != nullptr) {
                  pending.push_back({actor, status.origin,
                                     definition->tickEffects, *position});
                }
              }
            }
            ++e;
          }
        }
        // Apply after the status vectors stop being iterated: a tick effect may
        // apply another status to the same actor, which would otherwise reallocate
        // the vector underneath its own loop.
        for (const PendingEffects &entry : pending) {
          if (!entry.actor.is_alive()) continue;
          for (const std::string &effectId : entry.effects) {
            ApplyEffectAt(entry.actor.world(), entry.origin, effectId,
                          entry.actor, entry.position);
          }
        }
      });

  // Death lifecycle: NPCs only. The player has a knockout/reset path and training
  // Death lifecycle: any character whose vital resources have all failed. The
  // player has a knockout/reset path and training dummies are immortal; this is
  // deliberately not an "is Health at zero" check.
  ecs.system<ResourcePools>("DeathSystem")
      .with<Actor>()
      .without<PlayerControlled>()
      .without<TrainingDummy>()
      .without<Dead>()
      .each([this](flecs::entity entity, ResourcePools&) {
        if (Resources::IsAlive(entity)) {
          return;
        }
        entity.add<Dead>();
        ecs.defer([this, entity]() { KillEntity(entity); });
      });

  // The smallest hostile controller: a timer, a range, and one telegraphed
  // strike. It exists so incoming damage and interrupts are testable without the
  // LLM path; a real hostile agent will not carry this component.
  ecs.system<Aggressive, ResourcePools, GamePosition>("AggressiveCombatSystem")
      .without<NpcBehaviorId>()
      .without<DashState>()
      .iter([this](flecs::iter& it, Aggressive* aggressive, ResourcePools*,
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
          if (!Resources::IsAlive(entity) || entity.has<Casting>()) {
            continue;
          }
          if (const NpcDisposition *disposition = entity.get<NpcDisposition>();
              disposition != nullptr &&
              disposition->value == Disposition::Neutral) {
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
          const float range = aggressive[i].range;
          aggressive[i].timer = aggressive[i].interval;

          ecs.defer([this, entity, label, castSeconds, damage, range]() {
            // The captured handle is const inside a defer lambda; a local copy
            // is what lets the deferred writes go through.
            flecs::entity caster = entity;
            CastAction inlineCast;
            inlineCast.label = label;
            inlineCast.baseDamage = damage;
            inlineCast.interruptible = true;
            inlineCast.playerCast = false;
            inlineCast.requiresTarget = true;
            inlineCast.rangeTiles = range;
            inlineCast.castRangeTiles = range;
            flecs::entity action =
                ecs.entity()
                    .set<ActionTimer>({castSeconds})
                    .set<ActionActor>({caster})
                    .set<ActionTarget>({playerEntity})
                    .set<CastAction>(inlineCast);
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
          const Casting* activeCast = actor.get<Casting>();
          if (activeCast == nullptr ||
              activeCast->actionEntity != actionEntity) {
            actionEntity.destruct();
            return;
          }
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
                        !WithinActivationRange(
                            actor, target,
                            cast.castRangeTiles > 0.0f
                                ? cast.castRangeTiles
                                : def->activation.rangeTiles))) {
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
                ApplySkillStatus(actor, target, *def);
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
          } else if (target.is_alive() && Resources::IsDamageable(target)) {
            message = ApplyHit(actor, target, cast.baseDamage, "", cast.label,
                               false, false);
            landed = true;
          } else {
            message = cast.label + " finds nothing.";
          }

          // A completed cast says its name over the caster, the same call-out an
          // instant use gives, so a landed cast is legible without reading the
          // log. A fizzle deliberately says nothing: no name, no reward.
          if (landed && !cast.surfaceId.empty() && target.is_alive() &&
              target.has<GamePosition>()) {
            const SurfaceDef *surface = surfaceRegistry.Get(cast.surfaceId);
            AddSurface(*target.get<GamePosition>(), cast.surfaceId,
                       MakeEffectOrigin(actor, cast.surfaceId,
                                        surface != nullptr
                                            ? surface->tags
                                            : std::vector<std::string>()),
                       cast.surfaceDuration, cast.surfaceRadius);
          }

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
            if (!projectile.surfaceId.empty()) {
              if (const GamePosition *impact =
                      projectile.target.get<GamePosition>()) {
                const SurfaceDef *surface =
                    surfaceRegistry.Get(projectile.surfaceId);
                AddSurface(*impact, projectile.surfaceId,
                           MakeEffectOrigin(projectile.owner, projectile.surfaceId,
                                            surface != nullptr
                                                ? surface->tags
                                                : std::vector<std::string>()),
                           projectile.surfaceDuration,
                           projectile.surfaceRadius);
              }
            }
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
        ecs.filter<Actor, ResourcePools, ScreenPosition>().each(
            [&](flecs::entity target, const Actor&, ResourcePools&,
                const ScreenPosition&) {
              Resources::EnsureInitialized(target);
              if (Resources::IsAlive(target)) targets.push_back(target);
            });

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
            if (!Resources::IsAlive(target)) {
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

bool Game::BehaviorConditionMatches(const BehaviorCondition &condition,
                                    flecs::entity self,
                                    flecs::entity target) const {
  const auto selfPosition = self.get<GamePosition>();
  const auto targetPosition = target.get<GamePosition>();
  const int distance =
      selfPosition != nullptr && targetPosition != nullptr
          ? GridSteps(*selfPosition, *targetPosition)
          : 9999;

  const auto resourcePercent = [](flecs::entity entity,
                                  const std::string &id) {
    const float max = Resources::Max(entity, id);
    return max > 0.0f ? Resources::Current(entity, id) / max : 0.0f;
  };

  switch (condition.kind) {
  case BehaviorCondition::Kind::Always:
    return true;
  case BehaviorCondition::Kind::All:
    for (const BehaviorCondition &child : condition.children) {
      if (!BehaviorConditionMatches(child, self, target)) return false;
    }
    return true;
  case BehaviorCondition::Kind::Any:
    for (const BehaviorCondition &child : condition.children) {
      if (BehaviorConditionMatches(child, self, target)) return true;
    }
    return false;
  case BehaviorCondition::Kind::Not:
    return !condition.children.empty() &&
           !BehaviorConditionMatches(condition.children.front(), self, target);
  case BehaviorCondition::Kind::SelfHealthPctBelow:
    return resourcePercent(self, "health") < condition.value;
  case BehaviorCondition::Kind::SelfHealthPctAbove:
    return resourcePercent(self, "health") > condition.value;
  case BehaviorCondition::Kind::SelfResourcePctBelow:
    return resourcePercent(self, condition.id) < condition.value;
  case BehaviorCondition::Kind::SelfResourcePctAbove:
    return resourcePercent(self, condition.id) > condition.value;
  case BehaviorCondition::Kind::TargetDistanceAtLeast:
    return static_cast<float>(distance) >= condition.value;
  case BehaviorCondition::Kind::TargetDistanceAtMost:
    return static_cast<float>(distance) <= condition.value;
  case BehaviorCondition::Kind::TargetHasStatus: {
    const StatusEffects *statuses = target.get<StatusEffects>();
    return statuses != nullptr && statuses->Has(condition.id);
  }
  case BehaviorCondition::Kind::TargetLacksStatus: {
    const StatusEffects *statuses = target.get<StatusEffects>();
    return statuses == nullptr || !statuses->Has(condition.id);
  }
  case BehaviorCondition::Kind::SelfHasStatus: {
    const StatusEffects *statuses = self.get<StatusEffects>();
    return statuses != nullptr && statuses->Has(condition.id);
  }
  case BehaviorCondition::Kind::AlliesWithin:
  case BehaviorCondition::Kind::EnemiesWithin: {
    int count = 0;
    if (selfPosition == nullptr) return false;
    ecs.filter<Actor, GamePosition>().each(
        [&](flecs::entity other, const Actor &, const GamePosition &position) {
          if (!other.is_alive() || other == self) return;
          if (static_cast<float>(GridSteps(*selfPosition, position)) >
              condition.range) {
            return;
          }
          if (condition.kind == BehaviorCondition::Kind::AlliesWithin) {
            if (!other.has<PlayerControlled>()) ++count;
          } else if (other.has<PlayerControlled>()) {
            ++count;
          }
        });
    return count >= condition.count;
  }
  }
  return false;
}

bool Game::NpcCanSeeTarget(flecs::entity entity,
                           flecs::entity target) const {
  if (!entity.is_alive() || !target.is_alive()) return false;
  const GamePosition *from = entity.get<GamePosition>();
  const GamePosition *to = target.get<GamePosition>();
  if (from == nullptr || to == nullptr) return false;
  const Vision *vision = entity.get<Vision>();
  const float range = vision != nullptr ? vision->range
                                        : Settings().perception.range;
  if (static_cast<float>(GridSteps(*from, *to)) > range) return false;
  std::unordered_set<std::uint64_t> opaque;
  ecs.filter<const BlocksVision, const GamePosition>().each(
      [&opaque](const BlocksVision &, const GamePosition &position) {
        opaque.insert(VisionQuery::CellKey(position));
      });
  return !VisionQuery::PathIsOpaque(opaque, *from, *to);
}

flecs::entity Game::FindPatrolHostileTarget(
    flecs::entity entity, const std::string &templateId) const {
  if (templateId.empty()) return flecs::entity::null();
  if (templateId == "player") {
    if (playerEntity.is_alive() && NpcCanSeeTarget(entity, playerEntity)) {
      return playerEntity;
    }
    return flecs::entity::null();
  }
  flecs::entity found = flecs::entity::null();
  ecs.filter<const NpcTemplateId, const GamePosition>().each(
      [&](flecs::entity other, const NpcTemplateId &id, const GamePosition &) {
        if (found.is_alive() || other == entity || id.id != templateId) return;
        if (!Resources::IsAlive(other)) return;
        if (NpcCanSeeTarget(entity, other)) found = other;
      });
  return found;
}

void Game::UpdateNeutralNpcBehavior(flecs::entity entity,
                                    const BehaviorDef &definition,
                                    NpcBehaviorState &state, float dt) {
  if (map == nullptr) return;
  const GamePosition *self = entity.get<GamePosition>();
  if (self == nullptr) return;

  auto nextRandom = [&]() -> std::uint32_t {
    if (state.randomState == 0) {
      state.randomState = static_cast<std::uint32_t>(entity.id()) * 2654435761u + 1u;
    }
    state.randomState = state.randomState * 1664525u + 1013904223u;
    return state.randomState;
  };
  const auto random01 = [&]() {
    return static_cast<float>(nextRandom() & 0x00FFFFFFu) /
           static_cast<float>(0x01000000u);
  };
  auto walkTo = [&](const GamePosition &destination) {
    if (destination == *self || map->IsBlocked(destination.x, destination.y)) {
      return false;
    }
    const std::vector<GamePosition> path =
        AStar(map.get(), *self, destination);
    if (path.empty()) return false;
    entity.set<MOVE_THROUGH_PATH_ACTION>({path});
    state.neutralDestination = destination;
    state.neutralTravelling = true;
    return true;
  };
  auto finishTrip = [&]() {
    state.neutralTravelling = false;
    const NpcTemplateId *templateId = entity.get<NpcTemplateId>();
    const NpcTemplate *npc = templateId != nullptr
                                 ? npcTemplateRegistry.Get(templateId->id)
                                 : nullptr;
    const float minWait = npc != nullptr ? npc->wanderMinWait : 2.0f;
    const float maxWait = npc != nullptr ? npc->wanderMaxWait : 6.0f;
    state.neutralWait = minWait + random01() * std::max(0.0f, maxWait - minWait);
  };

  if (definition.kind == "wander") {
    if (state.neutralTravelling) {
      if (!entity.has<MOVE_THROUGH_PATH_ACTION>()) finishTrip();
      return;
    }
    if (state.neutralWait > 0.0f) {
      state.neutralWait = std::max(0.0f, state.neutralWait - dt);
      return;
    }
    const NpcTemplateId *templateId = entity.get<NpcTemplateId>();
    const NpcTemplate *npc = templateId != nullptr
                                 ? npcTemplateRegistry.Get(templateId->id)
                                 : nullptr;
    const float radius = npc != nullptr ? std::max(1.0f, npc->wanderRadius) : 6.0f;
    const int maxX = static_cast<int>(map->GetWidth()) - 1;
    const int maxY = static_cast<int>(map->GetHeight()) - 1;
    for (int attempt = 0; attempt < 24; ++attempt) {
      const float angle = random01() * 6.2831853f;
      const float distance = std::sqrt(random01()) * radius;
      const GamePosition destination{
          std::clamp(self->x + static_cast<int>(std::lround(std::cos(angle) * distance)),
                     0, maxX),
          std::clamp(self->y + static_cast<int>(std::lround(std::sin(angle) * distance)),
                     0, maxY)};
      if (walkTo(destination)) return;
    }
    finishTrip();
    state.neutralWait = std::max(0.5f, state.neutralWait);
    return;
  }

  if (definition.kind != "patrol") return;

  const auto nearestWalkable = [&](int x, int y) {
    const int maxX = static_cast<int>(map->GetWidth()) - 1;
    const int maxY = static_cast<int>(map->GetHeight()) - 1;
    for (int radius = 0; radius < 64; ++radius) {
      for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
          if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
          const int px = std::clamp(x + dx, 0, maxX);
          const int py = std::clamp(y + dy, 0, maxY);
          if (!map->IsBlocked(px, py)) return GamePosition{px, py};
        }
      }
    }
    return GamePosition{x, y};
  };

  if (state.patrolRoute.empty()) {
    const int maxX = static_cast<int>(map->GetWidth()) - 1;
    const int maxY = static_cast<int>(map->GetHeight()) - 1;
    const GamePosition corners[] = {
        nearestWalkable(0, 0), nearestWalkable(maxX, 0),
        nearestWalkable(maxX, maxY), nearestWalkable(0, maxY)};
    for (const GamePosition &corner : corners) {
      if (map->IsBlocked(corner.x, corner.y)) continue;
      if (!state.patrolRoute.empty() &&
          state.patrolRoute.back() == corner) {
        continue;
      }
      state.patrolRoute.push_back(corner);
    }
    state.patrolIndex = 0;
  }
  if (state.patrolRoute.empty()) return;

  if (state.neutralTravelling) {
    if (!entity.has<MOVE_THROUGH_PATH_ACTION>()) {
      state.neutralTravelling = false;
      state.patrolIndex = (state.patrolIndex + 1) % state.patrolRoute.size();
    }
    return;
  }

  const GamePosition waypoint = state.patrolRoute[state.patrolIndex];
  if (waypoint == *self) {
    state.patrolIndex = (state.patrolIndex + 1) % state.patrolRoute.size();
    return;
  }
  if (!walkTo(waypoint)) {
    state.patrolIndex = (state.patrolIndex + 1) % state.patrolRoute.size();
  }
}

void Game::UpdateNpcBehaviors(float dt) {
  if (map == nullptr) return;

  std::vector<flecs::entity> actors;
  ecs.filter<NpcBehaviorId>().each(
      [&](flecs::entity entity, NpcBehaviorId &) {
        if (entity.is_alive()) actors.push_back(entity);
      });

  for (flecs::entity entity : actors) {
    const NpcBehaviorId *behaviorId = entity.get<NpcBehaviorId>();
    NpcBehaviorState *state = entity.get_mut<NpcBehaviorState>();
    const GamePosition *selfPosition = entity.get<GamePosition>();
    const NpcDisposition *disposition = entity.get<NpcDisposition>();
    if (behaviorId == nullptr || state == nullptr || selfPosition == nullptr) {
      continue;
    }
    const BehaviorDef *definition = behaviorRegistry.Get(behaviorId->id);
    if (definition == nullptr) continue;

    const NpcBehaviorProfileId *profileId =
        entity.get<NpcBehaviorProfileId>();
    const BehaviorProfileDef *profile =
        profileId != nullptr ? behaviorProfileRegistry.Get(profileId->id)
                             : nullptr;
    const auto profileBehavior = [&](const std::string &role) -> std::string {
      if (profile == nullptr) return {};
      const auto found = profile->behaviors.find(role);
      return found != profile->behaviors.end() ? found->second : std::string();
    };
    if (profile != nullptr) {
      const bool neutral =
          disposition != nullptr &&
          disposition->value == Disposition::Neutral;
      std::string desired = neutral ? profileBehavior("neutral")
                                    : profileBehavior("aggressive");
      if (desired.empty() && neutral) desired = profileBehavior(profile->initial);
      if (desired.empty() && !profile->behaviors.empty()) {
        desired = profile->behaviors.begin()->second;
      }
      if (!desired.empty() && behaviorId->id != desired) {
        entity.set<NpcBehaviorId>({desired});
        behaviorId = entity.get<NpcBehaviorId>();
        definition = behaviorRegistry.Get(desired);
        state->activeRule.clear();
        state->activeSeconds = 0.0f;
        state->ruleCooldowns.clear();
        state->repathTimer = 0.0f;
        state->neutralTravelling = false;
        state->patrolRoute.clear();
        state->patrolIndex = 0;
      }
    }
    const NpcNeutralBehaviorId *neutralId =
        entity.get<NpcNeutralBehaviorId>();
    const NpcAggressiveBehaviorId *aggressiveId =
        entity.get<NpcAggressiveBehaviorId>();
    if (profile == nullptr && neutralId != nullptr) {
      const std::string desired =
          disposition != nullptr &&
                  disposition->value == Disposition::Neutral
              ? neutralId->id
              : (aggressiveId != nullptr ? aggressiveId->id : neutralId->id);
      if (behaviorId->id != desired) {
        entity.set<NpcBehaviorId>({desired});
        behaviorId = entity.get<NpcBehaviorId>();
        definition = behaviorRegistry.Get(desired);
        state->activeRule.clear();
        state->activeSeconds = 0.0f;
        state->ruleCooldowns.clear();
        state->repathTimer = 0.0f;
        state->neutralTravelling = false;
        state->patrolRoute.clear();
        state->patrolIndex = 0;
      }
    }
    if (definition == nullptr) continue;

    // A patrol looks for its configured template while neutral. On detection it
    // stores the target and switches to its aggressive behavior.
    if (disposition != nullptr &&
        disposition->value == Disposition::Neutral &&
        definition->kind == "patrol") {
      const NpcTemplateId *templateId = entity.get<NpcTemplateId>();
      const NpcTemplate *npcTemplate =
          templateId != nullptr ? npcTemplateRegistry.Get(templateId->id)
                                : nullptr;
      if (npcTemplate != nullptr &&
          !npcTemplate->patrolHostileTemplate.empty()) {
        flecs::entity found = FindPatrolHostileTarget(
            entity, npcTemplate->patrolHostileTemplate);
        if (found.is_alive()) {
          entity.set<NpcCombatTarget>({found});
          if (NpcDisposition *mutableDisposition =
                  entity.get_mut<NpcDisposition>()) {
            mutableDisposition->value = Disposition::Aggressive;
          }
          const std::string aggressiveBehavior =
              profileBehavior("aggressive").empty()
                  ? (aggressiveId != nullptr ? aggressiveId->id : std::string())
                  : profileBehavior("aggressive");
          if (!aggressiveBehavior.empty()) {
            entity.set<NpcBehaviorId>({aggressiveBehavior});
            behaviorId = entity.get<NpcBehaviorId>();
            definition = behaviorRegistry.Get(aggressiveBehavior);
          }
          state->activeRule.clear();
          state->activeSeconds = 0.0f;
          state->ruleCooldowns.clear();
          state->repathTimer = 0.0f;
          state->neutralTravelling = false;
          state->patrolRoute.clear();
          state->patrolIndex = 0;
          if (debugLog) {
            debugLog->LogCombat("Patrol spotted a hostile target.");
          }
        }
      }
    }

    // If a patrol's target dies or leaves sight, drop the target and return to
    // the neutral behavior. Check the component first: get_mut on flecs would
    // add an empty target to every NPC and immediately reset its disposition.
    if (entity.has<NpcCombatTarget>()) {
      NpcCombatTarget *combatTarget = entity.get_mut<NpcCombatTarget>();
      if (combatTarget == nullptr ||
          !combatTarget->target.is_alive() ||
          !NpcCanSeeTarget(entity, combatTarget->target)) {
        entity.remove<NpcCombatTarget>();
        if (NpcDisposition *mutableDisposition =
                entity.get_mut<NpcDisposition>()) {
          mutableDisposition->value = Disposition::Neutral;
        }
        const std::string neutralBehavior =
            profileBehavior("neutral").empty()
                ? (neutralId != nullptr ? neutralId->id : std::string())
                : profileBehavior("neutral");
        if (!neutralBehavior.empty()) {
          entity.set<NpcBehaviorId>({neutralBehavior});
          behaviorId = entity.get<NpcBehaviorId>();
          definition = behaviorRegistry.Get(neutralBehavior);
        }
        state->activeRule.clear();
        state->activeSeconds = 0.0f;
        state->ruleCooldowns.clear();
        state->repathTimer = 0.0f;
        state->neutralTravelling = false;
        state->patrolRoute.clear();
        state->patrolIndex = 0;
        continue;
      }
    }

    if (disposition == nullptr ||
        disposition->value == Disposition::Neutral) {
      if (definition->kind == "wander" || definition->kind == "patrol") {
        state->inCombat = false;
        UpdateNeutralNpcBehavior(entity, *definition, *state, dt);
        continue;
      }
      entity.remove<MOVE_THROUGH_PATH_ACTION>();
      if (entity.has<StatusEffects>()) {
        RemoveStatus(entity, "fleeing");
      }
      state->inCombat = false;
      state->activeRule.clear();
      continue;
    }
    if (!Resources::IsAlive(entity)) continue;

    flecs::entity target = playerEntity;
    if (const NpcCombatTarget *storedTarget = entity.get<NpcCombatTarget>()) {
      if (storedTarget->target.is_alive()) target = storedTarget->target;
    }
    const GamePosition *targetPosition = target.get<GamePosition>();
    if (targetPosition == nullptr) continue;

    state->inCombat = true;
    if (entity.has<DashState>()) {
      entity.remove<MOVE_THROUGH_PATH_ACTION>();
      continue;
    }
    if (entity.has<Casting>()) {
      entity.remove<MOVE_THROUGH_PATH_ACTION>();
      continue;
    }

    const int targetDistance =
        GridSteps(*selfPosition, *targetPosition);
    for (NpcBehaviorRuleCooldown &cooldown : state->ruleCooldowns) {
      cooldown.remaining = std::max(0.0f, cooldown.remaining - dt);
    }

    // Update the active movement rule first. It owns the body until its exit
    // condition passes and its minimum duration has elapsed.
    const BehaviorMovementRule *activeRule = nullptr;
    for (const BehaviorMovementRule &rule : definition->rules) {
      if (rule.id == state->activeRule) {
        activeRule = &rule;
        break;
      }
    }
    if (activeRule != nullptr) {
      state->activeSeconds += dt;
      const bool enter =
          BehaviorConditionMatches(activeRule->enterWhen, entity, target);
      const bool exit =
          activeRule->hasExit
              ? BehaviorConditionMatches(activeRule->exitWhen, entity,
                                         target)
              : !enter;
      const bool expired =
          activeRule->duration > 0.0f &&
          state->activeSeconds >= activeRule->duration;
      if (expired ||
          (state->activeSeconds >= activeRule->minDuration && exit)) {
        state->SetCooldown(activeRule->id, activeRule->cooldown);
        state->activeRule.clear();
        state->activeSeconds = 0.0f;
        activeRule = nullptr;
      }
    }
    if (activeRule == nullptr) {
      for (const BehaviorMovementRule &rule : definition->rules) {
        if (state->CooldownFor(rule.id) > 0.0f) continue;
        if (!BehaviorConditionMatches(rule.enterWhen, entity, target)) {
          continue;
        }
        state->activeRule = rule.id;
        state->activeSeconds = 0.0f;
        activeRule = &rule;
        break;
      }
    }

    const MovementPolicy &policy =
        activeRule != nullptr ? activeRule->policy : definition->defaultMovement;

    // A visible, purely informational marker for the player. The behavior owns
    // its lifetime; the status itself has no effects.
    if (policy.mode == "flee") {
      const StatusEffects *statuses = entity.get<StatusEffects>();
      if (statuses == nullptr || !statuses->Has("fleeing")) {
        const float duration =
            activeRule != nullptr && activeRule->duration > 0.0f
                ? activeRule->duration
                : 10.0f;
        ApplyStatus(entity, "fleeing",
                    MakeEffectOrigin(entity, "behavior:flee", {}), duration);
      }
    } else if (entity.has<StatusEffects>()) {
      RemoveStatus(entity, "fleeing");
    }

    // Choose one action by strict priority. The skill owns its maximum range;
    // the behavior can only raise the minimum (pounce must not fire at 1 tile).
    if (policy.allowActions && !HasStatusTag(entity, "immobilized")) {
      SkillCooldowns *cooldowns = entity.get_mut<SkillCooldowns>();
      for (const BehaviorAction &action : definition->actions) {
        const SkillDef *skill = FindSkillDef(entity.world(), action.skillId);
        if (skill == nullptr || cooldowns == nullptr ||
            !cooldowns->IsReady(action.skillId)) {
          continue;
        }
        const float range = skill->activation.rangeTiles;
        if (static_cast<float>(targetDistance) > range ||
            static_cast<float>(targetDistance) < action.minRange) {
          continue;
        }
        if (!BehaviorConditionMatches(action.when, entity, target)) {
          continue;
        }
        const SkillUseOutcome outcome =
            UseSkill(entity, action.skillId, target, false, false);
        if (!outcome.used) continue;
        entity.remove<MOVE_THROUGH_PATH_ACTION>();
        state->repathTimer = 0.0f;
        if (debugLog && !outcome.message.empty()) {
          debugLog->LogCombat(outcome.message);
        }
        break;
      }
      if (entity.has<Casting>()) continue;
    }

    if (HasStatusTag(entity, "immobilized")) {
      entity.remove<MOVE_THROUGH_PATH_ACTION>();
      continue;
    }

    if (policy.mode == "hold") {
      entity.remove<MOVE_THROUGH_PATH_ACTION>();
      continue;
    }

    if (state->repathTimer > 0.0f) {
      state->repathTimer = std::max(0.0f, state->repathTimer - dt);
      if (entity.has<MOVE_THROUGH_PATH_ACTION>()) continue;
    }

    const float distance = static_cast<float>(targetDistance);
    auto clearPath = [&]() {
      entity.remove<MOVE_THROUGH_PATH_ACTION>();
      state->repathTimer = definition->repathSeconds;
    };
    auto walkTo = [&](const GamePosition &destination) {
      if (destination == *selfPosition) {
        clearPath();
        return;
      }
      const std::vector<GamePosition> path =
          AStar(map.get(), *selfPosition, destination);
      if (path.empty()) {
        clearPath();
        return;
      }
      entity.set<MOVE_THROUGH_PATH_ACTION>({path});
      state->repathTimer = definition->repathSeconds;
    };
    auto awayDestination = [&](float desiredRange) {
      int dx = selfPosition->x - targetPosition->x;
      int dy = selfPosition->y - targetPosition->y;
      if (dx == 0 && dy == 0) dx = 1;
      const int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
      const int sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
      const int step = std::max(1, static_cast<int>(std::lround(desiredRange)));
      const int maxX = static_cast<int>(map->GetWidth()) - 1;
      const int maxY = static_cast<int>(map->GetHeight()) - 1;
      return GamePosition{
          std::clamp(targetPosition->x + sx * step, 0, maxX),
          std::clamp(targetPosition->y + sy * step, 0, maxY)};
    };

    if (policy.mode == "chase") {
      if (distance <= policy.desiredRange + policy.tolerance) {
        clearPath();
      } else {
        walkTo(*targetPosition);
      }
    } else if (policy.mode == "kite") {
      if (distance < policy.desiredRange - policy.tolerance) {
        walkTo(awayDestination(policy.desiredRange));
      } else if (distance > policy.desiredRange + policy.tolerance) {
        walkTo(*targetPosition);
      } else {
        clearPath();
      }
    } else if (policy.mode == "flee") {
      // Flee is a sustained state, not "run to one point and stop". Keep
      // choosing a far point away from the target on every repath; if the direct
      // away direction hits the map edge or a wall, fan out to alternate angles.
      const int mapWidth = static_cast<int>(map->GetWidth());
      const int mapHeight = static_cast<int>(map->GetHeight());
      const int step = std::max(8, std::max(mapWidth, mapHeight));
      int awayX = selfPosition->x - targetPosition->x;
      int awayY = selfPosition->y - targetPosition->y;
      if (awayX == 0 && awayY == 0) awayX = 1;
      const float baseAngle = std::atan2(
          static_cast<float>(awayY), static_cast<float>(awayX));
      static const float kAngles[] = {0.0f,  0.785f,  -0.785f,
                                      1.571f, -1.571f, 2.356f,
                                      -2.356f, 3.141f};
      bool fled = false;
      for (float offset : kAngles) {
        const float angle = baseAngle + offset;
        const int destX = std::clamp(
            selfPosition->x +
                static_cast<int>(std::lround(std::cos(angle) * step)),
            0, mapWidth - 1);
        const int destY = std::clamp(
            selfPosition->y +
                static_cast<int>(std::lround(std::sin(angle) * step)),
            0, mapHeight - 1);
        if (destX == selfPosition->x && destY == selfPosition->y) continue;
        if (map->IsBlocked(destX, destY)) continue;
        const GamePosition destination{destX, destY};
        if (AStar(map.get(), *selfPosition, destination).empty()) continue;
        walkTo(destination);
        fled = true;
        break;
      }
      if (!fled) clearPath();
    }
  }
}
