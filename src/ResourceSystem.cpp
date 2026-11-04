#include "ResourceSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "CombatResolver.hpp"
#include "GameECSInternals.h"
#include "SourceGatherer.h"

namespace {
const ResourceRegistry *Registry(flecs::world world) {
  auto resource = world.get<ResourceRegistryResource>();
  return resource ? resource->registry : nullptr;
}

ResourcePools &Pools(flecs::entity entity) {
  if (!entity.has<ResourcePools>()) entity.set<ResourcePools>({});
  return *entity.get_mut<ResourcePools>();
}

void EnsurePrimary(ResourcePools &pools) {
  bool hasPrimary = false;
  for (const ResourcePool &pool : pools.pools) {
    if (pool.vital && pool.primary) {
      hasPrimary = true;
      break;
    }
  }
  if (hasPrimary) return;
  for (ResourcePool &pool : pools.pools) {
    if (pool.vital) {
      pool.primary = true;
      return;
    }
  }
}
}  // namespace

namespace Resources {

void Initialize(flecs::entity entity, const std::vector<ResourceSpec> &specs,
                bool grantDefaultHealth) {
  if (!entity.is_alive()) return;
  ResourcePools pools;
  const ResourceRegistry *registry = Registry(entity.world());

  std::vector<ResourceSpec> resolved = specs;
  if (resolved.empty() && grantDefaultHealth) {
    ResourceSpec health;
    health.id = "health";
    health.vital = true;
    health.primary = true;
    resolved.push_back(health);
  }

  for (const ResourceSpec &spec : resolved) {
    const ResourceDef *definition = registry ? registry->Get(spec.id) : nullptr;
    if (definition == nullptr) continue;
    ResourcePool pool;
    pool.id = spec.id;
    pool.vital = spec.vital;
    pool.primary = spec.primary;
    pool.shield = definition->shield;
    pool.maxOverride = spec.maxOverride;
    pools.pools.push_back(pool);
  }

  if (pools.pools.empty() && grantDefaultHealth && registry != nullptr) {
    if (const ResourceDef *health = registry->Get("health")) {
      ResourcePool pool;
      pool.id = "health";
      pool.vital = true;
      pool.primary = true;
      pool.shield = health->shield;
      pools.pools.push_back(pool);
    }
  }

  EnsurePrimary(pools);
  entity.set<ResourcePools>(std::move(pools));
  for (const ResourceSpec &spec : resolved) {
    ResourcePool *pool = Find(entity, spec.id);
    if (pool == nullptr) continue;
    pool->current = spec.current >= 0.0f ? spec.current : Max(entity, spec.id);
  }
}

void EnsureInitialized(flecs::entity entity) {
  if (!entity.is_alive()) return;
  if (entity.has<ResourcePools>()) return;
  if (const Vitals *vitals = entity.get<Vitals>()) {
    Initialize(entity, {}, true);
    if (ResourcePool *health = Find(entity, "health")) {
      health->current = vitals->health;
      health->maxOverride = vitals->maxHealth;
    }
    return;
  }
  Initialize(entity, {}, true);
}

ResourcePool *Find(flecs::entity entity, const std::string &id) {
  if (!entity.is_alive()) return nullptr;
  if (!entity.has<ResourcePools>()) return nullptr;
  return entity.get_mut<ResourcePools>()->Find(id);
}

float Max(flecs::entity entity, const std::string &id) {
  if (!entity.is_alive()) return 0.0f;
  if (!entity.has<ResourcePools>()) return 0.0f;
  const ResourcePools *pools = entity.get<ResourcePools>();
  const ResourcePool *pool = pools ? pools->Find(id) : nullptr;
  if (pool == nullptr) return 0.0f;
  if (pool->maxOverride > 0.0f) return pool->maxOverride;

  const ResourceRegistry *registry = Registry(entity.world());
  const ResourceDef *definition = registry ? registry->Get(id) : nullptr;
  if (definition == nullptr) return 0.0f;

  const ResolutionInputs inputs = MakeResolutionInputs(entity.world());
  const StatView view = GatherSources(entity, inputs.definitions);
  ModifierList modifiers;
  for (const ActiveSource &source : view.Sources()) {
    if (!source.hasMagnitude) continue;
    SourceScaling scaling;
    if (!inputs.definitions.ScalingFor(source.id, scaling)) continue;
    for (const StatLink &link : inputs.definitions.EffectsFor(source.id)) {
      if (!LinkMatches(link, "resource.max", id)) continue;
      modifiers.Add(MakeContribution(scaling, link, source.magnitude, source.id,
                                     SourceLabel(inputs.definitions, source)));
    }
  }
  const FoldRange range{0.0f, std::numeric_limits<float>::infinity()};
  return Fold(definition->baseMax, modifiers, range).value;
}

float Current(flecs::entity entity, const std::string &id) {
  const ResourcePool *pool = Find(entity, id);
  return pool ? pool->current : 0.0f;
}

bool Has(flecs::entity entity, const std::string &id) {
  return Find(entity, id) != nullptr;
}

bool IsDamageable(flecs::entity entity) {
  const ResourcePools *pools = entity.get<ResourcePools>();
  if (pools == nullptr) return false;
  for (const ResourcePool &pool : pools->pools) {
    if (pool.vital) return true;
  }
  return false;
}

bool IsAlive(flecs::entity entity) {
  const ResourcePools *pools = entity.get<ResourcePools>();
  if (pools == nullptr) return false;
  bool hasVital = false;
  for (const ResourcePool &pool : pools->pools) {
    if (!pool.vital) continue;
    hasVital = true;
    if (pool.current <= 0.0f) return false;
  }
  return hasVital;
}

std::string PrimaryVital(flecs::entity entity) {
  const ResourcePools *pools = entity.get<ResourcePools>();
  if (pools == nullptr) return {};
  for (const ResourcePool &pool : pools->pools) {
    if (pool.vital && pool.primary) return pool.id;
  }
  for (const ResourcePool &pool : pools->pools) {
    if (pool.vital) return pool.id;
  }
  return {};
}

float Change(flecs::entity entity, const std::string &id, float delta) {
  ResourcePool *pool = Find(entity, id);
  if (pool == nullptr) return 0.0f;
  const float before = pool->current;
  const float maximum = Max(entity, id);
  pool->current = std::clamp(pool->current + delta, 0.0f, maximum);
  return std::fabs(pool->current - before);
}

float Damage(flecs::entity source, flecs::entity target, float amount,
             const std::string &cause) {
  (void)source;
  (void)cause;
  if (!target.is_alive() || amount <= 0.0f || !target.has<ResourcePools>()) {
    return 0.0f;
  }
  ResourcePools *pools = target.get_mut<ResourcePools>();
  if (pools == nullptr) return 0.0f;

  float remaining = amount;
  // Shields are separate pools and absorb first.
  std::vector<ResourcePool *> shields;
  for (ResourcePool &pool : pools->pools) {
    if (pool.shield && pool.current > 0.0f) shields.push_back(&pool);
  }
  std::sort(shields.begin(), shields.end(),
            [](const ResourcePool *a, const ResourcePool *b) {
              return a->id < b->id;
            });
  for (ResourcePool *shield : shields) {
    if (remaining <= 0.0f) break;
    const float absorbed = std::min(shield->current, remaining);
    shield->current -= absorbed;
    remaining -= absorbed;
  }
  if (remaining <= 0.0f) return amount;

  ResourcePool *vital = nullptr;
  for (ResourcePool &pool : pools->pools) {
    if (pool.vital && pool.primary) {
      vital = &pool;
      break;
    }
  }
  if (vital == nullptr) {
    for (ResourcePool &pool : pools->pools) {
      if (pool.vital) {
        vital = &pool;
        break;
      }
    }
  }
  if (vital == nullptr) return amount - remaining;

  const float applied = std::min(vital->current, remaining);
  vital->current -= applied;
  return amount - remaining + applied;
}

void Set(flecs::entity entity, const std::string &id, float value) {
  ResourcePool *pool = Find(entity, id);
  if (pool == nullptr) return;
  pool->current = std::clamp(value, 0.0f, Max(entity, id));
}

void Heal(flecs::entity entity, const std::string &id, float amount) {
  if (amount <= 0.0f) return;
  Change(entity, id, amount);
}

bool CanPay(flecs::entity entity, const std::string &id, float amount) {
  const ResourcePool *pool = Find(entity, id);
  if (pool == nullptr || pool->current < amount) return false;
  const ResourceRegistry *registry = Registry(entity.world());
  const ResourceDef *definition = registry ? registry->Get(id) : nullptr;
  return definition == nullptr || definition->canBeSpendable;
}

bool Spend(flecs::entity entity, const std::string &id, float amount) {
  if (!CanPay(entity, id, amount)) return false;
  // Spending is allowed to reach zero even for a vital resource; the normal
  // death/depletion path decides what that means.
  Change(entity, id, -amount);
  return true;
}

void Grant(flecs::entity entity, const std::string &id, float amount) {
  if (!entity.is_alive()) return;
  ResourcePools &pools = Pools(entity);
  ResourcePool *pool = pools.Find(id);
  if (pool == nullptr) {
    const ResourceRegistry *registry = Registry(entity.world());
    const ResourceDef *definition = registry ? registry->Get(id) : nullptr;
    if (definition == nullptr) return;
    ResourcePool created;
    created.id = id;
    created.shield = definition->shield;
    pools.pools.push_back(created);
    pool = &pools.pools.back();
  }
  if (pool->shield) {
    // A shield's "maximum" is whatever it was granted. Keep it explicit so a
    // second grant can increase the barrier rather than being clamped to zero.
    pool->current += amount;
    pool->maxOverride = std::max(pool->maxOverride, pool->current);
    return;
  }
  const float maximum = Max(entity, id);
  pool->current = std::clamp(pool->current + amount, 0.0f, maximum);
}

}  // namespace Resources
