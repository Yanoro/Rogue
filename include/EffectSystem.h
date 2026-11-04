#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <flecs.h>

#include "Components.h"

struct EffectContext {
  EffectOrigin origin;
  flecs::entity target = flecs::entity::null();
  GamePosition position{0, 0};
  std::string surfaceId;
  std::string statusId;
};

EffectOrigin MakeEffectOrigin(flecs::entity caster, const std::string &causeId,
                              const std::vector<std::string> &causeTags = {});

// Applies one atomic effect through the registry. Calculations happen in the
// shared effect fold; callers (surfaces, statuses) only choose the effect id.
void ApplyEffectAt(flecs::world world, const EffectOrigin &origin,
                   const std::string &effectId, flecs::entity target,
                   GamePosition position);
