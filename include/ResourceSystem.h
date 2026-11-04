#pragma once

#include <string>
#include <vector>

#include <flecs.h>

#include "Components.h"
#include "ResourceRegistry.h"

// Resource values and operations. All damage/healing/spending should go through
// this layer; writing ResourcePool::current directly outside it is how a second
// health system gets created.
namespace Resources {

void Initialize(flecs::entity entity, const std::vector<ResourceSpec> &specs,
                bool grantDefaultHealth = true);
// Converts the legacy Vitals seed (objects/dummies) into a health pool.
void EnsureInitialized(flecs::entity entity);

ResourcePool *Find(flecs::entity entity, const std::string &id);
float Max(flecs::entity entity, const std::string &id);
float Current(flecs::entity entity, const std::string &id);
bool Has(flecs::entity entity, const std::string &id);
bool IsDamageable(flecs::entity entity);
bool IsAlive(flecs::entity entity);
std::string PrimaryVital(flecs::entity entity);

// Applies a signed change, clamped to [0, Max]. Returns the amount actually
// applied (positive magnitude).
float Change(flecs::entity entity, const std::string &id, float delta);
float Damage(flecs::entity source, flecs::entity target, float amount,
             const std::string &cause = std::string());
void Heal(flecs::entity entity, const std::string &id, float amount);
// Explicit setter for resets such as a training dummy or a knocked-out player.
void Set(flecs::entity entity, const std::string &id, float value);
bool CanPay(flecs::entity entity, const std::string &id, float amount);
bool Spend(flecs::entity entity, const std::string &id, float amount);
void Grant(flecs::entity entity, const std::string &id, float amount);

}  // namespace Resources
