#include "EffectSystem.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "CombatResolver.hpp"
#include "Components.h"
#include "EffectRegistry.h"
#include "Game.h"
#include "ResourceSystem.h"
#include "GameECSInternals.h"
#include "SourceGatherer.h"

namespace {
std::string CasterName(flecs::entity caster) {
  if (!caster.is_alive()) return "the environment";
  if (const DisplayName *name = caster.get<DisplayName>()) return name->name;
  return "someone";
}

float ResolveEffectValue(flecs::entity caster, const EffectDef &effect) {
  if (!caster.is_alive()) return effect.base;
  const ResolutionInputs inputs = MakeResolutionInputs(caster.world());
  const StatView view = GatherSources(caster, inputs.definitions);
  ModifierList modifiers;
  for (const ActiveSource &source : view.Sources()) {
    if (!source.hasMagnitude) continue;
    SourceScaling scaling;
    if (!inputs.definitions.ScalingFor(source.id, scaling)) continue;
    for (const StatLink &link : inputs.definitions.EffectsFor(source.id)) {
      if (!LinkMatches(link, "effect", effect.id)) continue;
      modifiers.Add(MakeContribution(scaling, link, source.magnitude, source.id,
                                     SourceLabel(inputs.definitions, source)));
    }
  }
  const FoldRange range{effect.kind == EffectKind::Damage ? 1.0f : 0.0f,
                        std::numeric_limits<float>::infinity()};
  return Fold(effect.base, modifiers, range).value;
}
}  // namespace

EffectOrigin MakeEffectOrigin(flecs::entity caster, const std::string &causeId,
                              const std::vector<std::string> &causeTags) {
  EffectOrigin origin;
  origin.caster = caster;
  origin.casterId = caster.is_alive() ? caster.id() : 0;
  origin.casterName = CasterName(caster);
  origin.causeId = causeId;
  origin.causeTags = causeTags;
  return origin;
}

void ApplyEffectAt(flecs::world world, const EffectOrigin &origin,
                   const std::string &effectId, flecs::entity target,
                   GamePosition position) {
  auto runtime = world.get<GameRuntimeResource>();
  if (runtime == nullptr || runtime->game == nullptr) return;
  Game &game = *runtime->game;
  const EffectDef *effect = game.effectRegistry.Get(effectId);
  if (effect == nullptr) {
    if (game.debugLog) {
      game.debugLog->LogWarning("Effect: no definition '" + effectId + "'");
    }
    return;
  }

  switch (effect->kind) {
    case EffectKind::Damage: {
      const float amount = ResolveEffectValue(origin.caster, *effect);
      ApplyEffectDamage(origin.caster, target, amount, effect->name);
      break;
    }
    case EffectKind::Heal: {
      if (!target.is_alive()) break;
      Resources::EnsureInitialized(target);
      if (!Resources::IsDamageable(target)) break;
      const float amount = ResolveEffectValue(origin.caster, *effect);
      Resources::Heal(target, Resources::PrimaryVital(target), amount);
      break;
    }
    case EffectKind::ApplyStatus:
      game.ApplyStatus(target, effect->statusId, origin,
                       effect->statusDuration);
      break;
    case EffectKind::RemoveStatus:
      game.RemoveStatus(target, effect->statusId);
      break;
    case EffectKind::CreateSurface:
      game.AddSurface(position, effect->surfaceId, origin,
                      effect->surfaceDuration, effect->surfaceRadius);
      break;
    case EffectKind::RemoveSurface:
      game.RemoveSurface(position, effect->surfaceId);
      break;
  }
}
