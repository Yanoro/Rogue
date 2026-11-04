#include "SurfaceSystem.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "Game.h"
#include "GameECSInternals.h"
#include "Map.h"
#include "SpritePaths.hpp"

namespace {
void AppendUnique(std::vector<std::string> &out, const std::string &value) {
  if (value.empty()) return;
  if (std::find(out.begin(), out.end(), value) == out.end()) out.push_back(value);
}

}  // namespace

void AddSurfaceAt(flecs::world world, GamePosition position,
                  const std::string &surfaceId, flecs::entity source,
                  float durationOverride, int radius) {
  auto runtime = world.get<GameRuntimeResource>();
  if (runtime == nullptr || runtime->game == nullptr) return;
  const SurfaceDef *definition = runtime->game->surfaceRegistry.Get(surfaceId);
  std::vector<std::string> tags;
  if (definition != nullptr) tags = definition->tags;
  runtime->game->AddSurface(position, surfaceId,
                            MakeEffectOrigin(source, surfaceId, tags),
                            durationOverride, radius);
}

void RemoveSurfaceAt(flecs::world world, GamePosition position,
                     const std::string &surfaceId) {
  auto runtime = world.get<GameRuntimeResource>();
  if (runtime == nullptr || runtime->game == nullptr) return;
  runtime->game->RemoveSurface(position, surfaceId);
}

void Game::AddSurface(GamePosition position, const std::string &surfaceId,
                      const EffectOrigin &origin, float durationOverride,
                      int radius) {
  const SurfaceDef *definition = surfaceRegistry.Get(surfaceId);
  if (definition == nullptr) {
    if (debugLog) debugLog->LogWarning("Surface: no definition '" + surfaceId + "'");
    return;
  }
  const int clampedRadius = std::max(0, radius);
  for (int dx = -clampedRadius; dx <= clampedRadius; ++dx) {
    for (int dy = -clampedRadius; dy <= clampedRadius; ++dy) {
      if (std::max(std::abs(dx), std::abs(dy)) > clampedRadius) continue;
      const GamePosition cell{position.x + dx, position.y + dy};
      if (map != nullptr &&
          !map->IsInBounds(static_cast<float>(cell.x), static_cast<float>(cell.y))) {
        continue;
      }
      std::vector<SurfaceInstance> &instances = surfaceGrid.At(cell);

      // Reapplying the same surface refreshes it rather than stacking a second
      // copy of the same hazard on the cell.
      bool refreshed = false;
      for (SurfaceInstance &existing : instances) {
        if (existing.defId != surfaceId) continue;
        existing.remaining = definition->permanent
                                 ? -1.0f
                                 : (durationOverride > 0.0f
                                        ? durationOverride
                                        : definition->durationSeconds);
        existing.stayTimer = definition->stayInterval;
        existing.origin = origin;
        existing.stacks = std::min(99, existing.stacks + 1);
        refreshed = true;
        break;
      }
      if (refreshed) continue;

      std::vector<std::string> results;
      bool removeNew = false;
      std::vector<size_t> removeExisting;
      for (size_t i = 0; i < instances.size(); ++i) {
        const SurfaceDef *other = surfaceRegistry.Get(instances[i].defId);
        if (other == nullptr) continue;
        for (const SurfaceReaction &reaction : definition->reactions) {
          if (!other->HasTag(reaction.withTag)) continue;
          if (reaction.removeSelf) removeNew = true;
          if (reaction.removeOther) removeExisting.push_back(i);
          AppendUnique(results, reaction.result);
        }
        for (const SurfaceReaction &reaction : other->reactions) {
          if (!definition->HasTag(reaction.withTag)) continue;
          if (reaction.removeSelf) removeExisting.push_back(i);
          if (reaction.removeOther) removeNew = true;
          AppendUnique(results, reaction.result);
        }
      }
      std::sort(removeExisting.rbegin(), removeExisting.rend());
      for (const size_t index : removeExisting) {
        if (index < instances.size()) instances.erase(instances.begin() + index);
      }
      if (!removeNew) {
        SurfaceInstance instance;
        instance.defId = surfaceId;
        instance.remaining = definition->permanent
                                 ? -1.0f
                                 : (durationOverride > 0.0f
                                        ? durationOverride
                                        : definition->durationSeconds);
        instance.stayTimer = definition->stayInterval;
        instance.origin = origin;
        instances.push_back(instance);
      }
      for (const std::string &result : results) {
        AddSurface(cell, result, origin, 0.0f, 0);
      }
      if (instances.empty()) {
        surfaceGrid.cells.erase({cell.x, cell.y});
      }
    }
  }
}

void Game::RemoveSurface(GamePosition position, const std::string &surfaceId) {
  auto it = surfaceGrid.cells.find({position.x, position.y});
  if (it == surfaceGrid.cells.end()) return;
  std::vector<SurfaceInstance> &instances = it->second;
  if (surfaceId.empty()) {
    instances.clear();
  } else {
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                                   [&](const SurfaceInstance &instance) {
                                     return instance.defId == surfaceId;
                                   }),
                    instances.end());
  }
  if (instances.empty()) surfaceGrid.cells.erase(it);
}

bool Game::CellHasSurfaceTag(GamePosition position, const std::string &tag) const {
  const std::vector<SurfaceInstance> *instances = surfaceGrid.Find(position);
  if (instances == nullptr) return false;
  for (const SurfaceInstance &instance : *instances) {
    const SurfaceDef *definition = surfaceRegistry.Get(instance.defId);
    if (definition != nullptr && definition->HasTag(tag)) return true;
  }
  return false;
}

void Game::ApplyStatus(flecs::entity target, const std::string &statusId,
                       const EffectOrigin &origin, float durationOverride) {
  if (!target.is_alive()) return;
  const StatusEffectDef *definition = statusEffectRegistry.Get(statusId);
  if (definition == nullptr) {
    if (debugLog) debugLog->LogWarning("Status: no definition '" + statusId + "'");
    return;
  }
  if (!target.has<StatusEffects>()) target.set<StatusEffects>({});
  StatusEffects *statusContainer = target.get_mut<StatusEffects>();
  if (statusContainer == nullptr) return;
  StatusEffects &statuses = *statusContainer;
  StatusEffectInstance *existing = nullptr;
  for (StatusEffectInstance &status : statuses.effects) {
    if (status.id == statusId) {
      existing = &status;
      break;
    }
  }
  const float duration =
      durationOverride > 0.0f ? durationOverride : definition->durationSeconds;
  if (existing != nullptr) {
    existing->remaining = std::max(existing->remaining, duration);
    existing->origin = origin;
    return;
  }
  StatusEffectInstance instance;
  instance.id = statusId;
  instance.remaining = duration;
  instance.tickTimer = definition->tickSeconds;
  instance.origin = origin;
  statuses.effects.push_back(std::move(instance));
  if (!definition->onApplyEffects.empty()) {
    if (const GamePosition *position = target.get<GamePosition>()) {
      for (const std::string &effectId : definition->onApplyEffects) {
        ApplyEffectAt(target.world(), origin, effectId, target, *position);
      }
    }
  }
}

void Game::RemoveStatus(flecs::entity target, const std::string &statusId) {
  if (!target.is_alive()) return;
  StatusEffects *statuses = target.get_mut<StatusEffects>();
  if (statuses == nullptr) return;
  const GamePosition *position = target.get<GamePosition>();
  EffectOrigin origin;
  std::vector<std::string> expireEffects;
  for (const StatusEffectInstance &status : statuses->effects) {
    if (status.id != statusId) continue;
    origin = status.origin;
    const StatusEffectDef *definition = statusEffectRegistry.Get(status.id);
    if (definition != nullptr) expireEffects = definition->onExpireEffects;
    break;
  }
  statuses->effects.erase(
      std::remove_if(statuses->effects.begin(), statuses->effects.end(),
                     [&](const StatusEffectInstance &status) {
                       return status.id == statusId;
                     }),
      statuses->effects.end());
  if (position != nullptr) {
    for (const std::string &effectId : expireEffects) {
      ApplyEffectAt(target.world(), origin, effectId, target, *position);
    }
  }
}

bool Game::HasStatusTag(flecs::entity target, const std::string &tag) const {
  const StatusEffects *statuses = target.get<StatusEffects>();
  if (statuses == nullptr) return false;
  for (const StatusEffectInstance &status : statuses->effects) {
    const StatusEffectDef *definition = statusEffectRegistry.Get(status.id);
    if (definition != nullptr && definition->HasTag(tag)) return true;
  }
  return false;
}

void Game::ProcessSurfaceTransitions() {
  if (pendingSurfaceTransitions.empty()) return;
  std::vector<SurfaceTransition> transitions = std::move(pendingSurfaceTransitions);
  pendingSurfaceTransitions.clear();
  for (const SurfaceTransition &transition : transitions) {
    if (!transition.actor.is_alive()) continue;

    const auto applyAt = [&](GamePosition position,
                             const std::vector<std::string> &effects,
                             const EffectOrigin &origin) {
      for (const std::string &effectId : effects) {
        if (!transition.actor.is_alive()) return;
        ApplyEffectAt(transition.actor.world(), origin, effectId,
                      transition.actor, position);
      }
    };

    if (transition.from.x != transition.to.x ||
        transition.from.y != transition.to.y) {
      if (const std::vector<SurfaceInstance> *from =
              surfaceGrid.Find(transition.from)) {
        const std::vector<SurfaceInstance> snapshot = *from;
        for (const SurfaceInstance &instance : snapshot) {
          const SurfaceDef *definition = surfaceRegistry.Get(instance.defId);
          if (definition != nullptr) {
            applyAt(transition.from, definition->onExit, instance.origin);
          }
        }
      }
    }

    const std::vector<SurfaceInstance> *to = surfaceGrid.Find(transition.to);
    if (to == nullptr) continue;
    const std::vector<SurfaceInstance> snapshot = *to;
    for (const SurfaceInstance &instance : snapshot) {
      const SurfaceDef *definition = surfaceRegistry.Get(instance.defId);
      if (definition == nullptr) continue;
      applyAt(transition.to, definition->onEnter, instance.origin);
    }
  }
}

void Game::UpdateSurfaces(float dt) {
  if (surfaceGrid.cells.empty()) return;

  // Collect once rather than scanning every actor for every surface cell.
  std::map<std::pair<int, int>, std::vector<flecs::entity>> actorsByCell;
  ecs.filter<Actor, GamePosition>().each(
      [&](flecs::entity actor, const Actor &, const GamePosition &position) {
        if (actor.is_alive()) actorsByCell[{position.x, position.y}].push_back(actor);
      });

  struct PendingStayEffect {
    flecs::entity actor;
    EffectOrigin origin;
    std::string effectId;
    GamePosition position;
  };
  std::vector<PendingStayEffect> pendingStayEffects;

  for (auto cellIt = surfaceGrid.cells.begin(); cellIt != surfaceGrid.cells.end();) {
    std::vector<SurfaceInstance> &instances = cellIt->second;
    for (size_t i = 0; i < instances.size();) {
      SurfaceInstance &instance = instances[i];
      const SurfaceDef *definition = surfaceRegistry.Get(instance.defId);
      if (definition == nullptr) {
        instances.erase(instances.begin() + i);
        continue;
      }
      if (instance.remaining >= 0.0f) {
        instance.remaining -= dt;
        if (instance.remaining <= 0.0f) {
          instances.erase(instances.begin() + i);
          continue;
        }
      }

      if (!definition->onStay.empty()) {
        instance.stayTimer -= dt;
        if (instance.stayTimer <= 0.0f) {
          instance.stayTimer += definition->stayInterval;
          auto actorsIt = actorsByCell.find(cellIt->first);
          if (actorsIt != actorsByCell.end()) {
            for (flecs::entity actor : actorsIt->second) {
              if (!actor.is_alive()) continue;
              for (const std::string &effectId : definition->onStay) {
                pendingStayEffects.push_back(
                    {actor, instance.origin, effectId,
                     GamePosition{cellIt->first.first,
                                  cellIt->first.second}});
              }
            }
          }
        }
      }
      ++i;
    }
    if (instances.empty()) {
      cellIt = surfaceGrid.cells.erase(cellIt);
    } else {
      ++cellIt;
    }
  }

  // Stay effects can create or remove surfaces. Apply them only after the grid
  // iteration, so an effect cannot reallocate the vector it was read from.
  for (const PendingStayEffect &pending : pendingStayEffects) {
    if (!pending.actor.is_alive()) continue;
    ApplyEffectAt(pending.actor.world(), pending.origin, pending.effectId,
                  pending.actor, pending.position);
  }
}

void Game::DrawSurfaces() {
  if (map == nullptr || surfaceGrid.cells.empty()) return;
  const int tileWidth = map->GetTileWidth();
  const int tileHeight = map->GetTileHeight();

  for (const auto &cell : surfaceGrid.cells) {
    const GamePosition position{cell.first.first, cell.first.second};
    const ScreenPosition screen = map->GameCoordsToScreenCoords(
        static_cast<float>(position.x), static_cast<float>(position.y));

    std::vector<const SurfaceInstance *> ordered;
    ordered.reserve(cell.second.size());
    for (const SurfaceInstance &instance : cell.second) {
      ordered.push_back(&instance);
    }
    std::sort(ordered.begin(), ordered.end(),
              [&](const SurfaceInstance *a, const SurfaceInstance *b) {
                const SurfaceDef *da = surfaceRegistry.Get(a->defId);
                const SurfaceDef *db = surfaceRegistry.Get(b->defId);
                return (da ? da->layer : 0) < (db ? db->layer : 0);
              });

    for (const SurfaceInstance *instance : ordered) {
      const SurfaceDef *definition = surfaceRegistry.Get(instance->defId);
      if (definition == nullptr) continue;
      const std::string spriteId =
          definition->sprite.empty() ? definition->id : definition->sprite;
      const Texture2D *texture =
          ResolveSpriteTexture(std::string(), spriteId, resourceManager,
                               debugLog.get());
      if (texture == nullptr) continue;
      const Rectangle source{0.0f, 0.0f, static_cast<float>(texture->width),
                             static_cast<float>(texture->height)};
      const Rectangle destination{screen.x, screen.y,
                                  static_cast<float>(tileWidth),
                                  static_cast<float>(tileHeight)};
      const unsigned char alpha = static_cast<unsigned char>(
          std::clamp(definition->alpha, 0.0f, 1.0f) * 255.0f);
      DrawTexturePro(*texture, source, destination, {0.0f, 0.0f}, 0.0f,
                     Color{255, 255, 255, alpha});
    }
  }
}
