# Effect registry, origins and modifier resolution

This document defines the effect layer used by surfaces, statuses, skills and,
later, death/hit hooks. It replaces the first-pass surface action enum and the
flat `EffectSource` stat snapshot.

## The problem

A surface definition should say:

> fire damages anything that enters and ignites it.

It should not say:

> fire does 4 + 0.25 × the caster's intelligence, plus 15% if the caster wears a
> Cinder Ring, plus a class bonus, minus target resistance.

Those are separate questions. If the surface owns them, every new equipment piece,
class, buff or damage rule requires editing the surface. The surface becomes a
list of every modifier that might ever exist.

The same is true for statuses. Burning should say "this is a fire damage-over-time
status"; it should not know why this particular tick is stronger.

## The layers

```text
EffectOrigin          who/what caused this instance?
EffectDef             what is the intrinsic effect?
StatusEffectDef       what persistent actor condition exists?
SurfaceDef            what triggers happen on a tile?
SourceEffects         which modifier providers does the caster carry?
Modifier/Fold         how much does the effect actually do?
```

### EffectOrigin — provenance, not stats

An effect instance stores provenance:

```cpp
struct EffectOrigin {
  flecs::entity caster = flecs::entity::null();
  uint64_t casterId = 0;
  std::string casterName;
  std::string causeId;             // "fire", "burning", "fireball"
  std::vector<std::string> causeTags;
};
```

It does **not** store intelligence, strength, class level, equipment ids or any
derived numbers.

- `caster` is a live link used to gather live equipment/class/stat modifiers.
- `casterId` is a stable identity for logs and future lookup.
- `casterName` keeps attribution readable after the caster dies.
- `causeId`/`causeTags` describe what created the instance.

Surfaces and statuses store `EffectOrigin`. Reactions and spread surfaces pass
the same origin forward.

If the caster dies, the surface/status remains valid. Live modifier resolution
falls back to the effect definition's intrinsic base value; the origin still gives
the log and any snapshot policy a stable identity.

### EffectDef — intrinsic, atomic effects

Effects are atomic. There are no composed bundles in this pass.

```json
{
  "name": "Fire Damage",
  "kind": "damage",
  "base": 4,
  "damageType": "fire",
  "tags": ["fire", "damage"]
}
```

```json
{
  "name": "Ignite",
  "kind": "apply_status",
  "status": "burning",
  "duration": 5
}
```

Supported kinds:

- `damage`
- `heal`
- `apply_status`
- `remove_status`
- `create_surface`
- `remove_surface`

An effect contains **intrinsic defaults only**: base amount, damage type, status
id/duration, surface id/duration/radius. It contains no caster stats and no
modifier formulas.

If a surface needs a stronger variant, it references a different atomic effect
(`strong_fire_damage`) or, later, an effect bundle. It does not grow a coefficient.

### Surface references effects

```json
{
  "name": "Fire",
  "sprite": "surface_fire",
  "duration": 20,
  "tags": ["fire", "hazard"],
  "enter": ["fire_damage", "ignite"],
  "stay": ["fire_damage"],
  "reactions": [{ "withTag": "water", "removeSelf": true }]
}
```

The surface knows only that entering causes those effects. It does not know what
they cost, scale from, or are modified by.

### Status references effects too

```json
{
  "name": "Burning",
  "icon": "status_burning",
  "tags": ["fire", "burning"],
  "duration": 5,
  "tick": {
    "every": 1.0,
    "effects": ["burning_damage"]
  }
}
```

The status owns the persistent condition, duration and icon. Its tick effects
are ordinary effect ids.

## Resolution uses the existing fold

Effect values use the same `StatLink`/`SourceGatherer`/`ModifierList`/`Fold`
pipeline as damage, duration and loot resolution. There is no second math system.

A new link point is added:

```text
point:    "effect"
activity: effect id, or "*"
```

Examples:

```json
{
  "point": "effect",
  "activity": "fire_damage",
  "op": "pct",
  "perPoint": 0.15
}
```

on an item definition means "this item increases fire damage by 15% per tier".

```json
{
  "point": "effect",
  "activity": "fire_damage",
  "op": "flat",
  "perPoint": 0.25
}
```

on the `intelligence` stat means "intelligence adds 0.25 fire damage per point".

The resolver:

1. Gathers the caster's live sources (stats, skills, equipped items; classes are
   reserved in `SourceGatherer`).
2. Selects links whose point is `effect` and whose activity matches the effect id
   or `*`.
3. Builds contributions with the existing `MakeContribution`.
4. Folds the effect's intrinsic `base` through the existing `Fold`.
5. Applies the final value to the target.

The surface and status never see the fold.

### Equipment now, classes later

Equipment already flows through `SourceGatherer`, so an item can contribute to an
effect through its normal `effects` array. The same contract is where a class or
temporary buff will plug in later; only `SourceGatherer` grows a new source kind.

No class registry is introduced in this pass.

## Live modifier policy

Persistent effects resolve against the caster's **live** source state by default:

- removing a Cinder Ring weakens existing fire;
- equipping one strengthens it;
- a class/buff change applies while the surface/status exists;
- if the caster dies, live modifiers are unavailable and the intrinsic base is
  used.

A later `modifierPolicy: "snapshot"` can capture the resolved modifier state at
creation without changing surface/status definitions. It is deliberately not
implemented in this pass; the origin model already supports adding it.

## Icon and status data

`StatusEffectDef` gains:

- `icon`: an `IconRegistry` id (`status_burning`), reused by the HUD and Content
  Editor. A status icon is a UI icon, not a new sprite registry.
- `tick`: interval and effect ids.
- `onApply`/`onExpire`: optional effect ids.
- `stacking`/`maxStacks`: data fields for the status runtime.

The status registry owns identity, description and presentation. The effect
registry owns the atomic operations.

## Runtime flow

```text
Fireball impact
  → creates Fire surface with EffectOrigin(caster)
  → Fire enter triggers "fire_damage" and "ignite"
  → fire_damage:
       ResolveEffectValue(caster, "fire_damage")
       GatherSources(caster)
       apply "effect"/"fire_damage" links
       Fold(base=4) → final
       ApplyEffectDamage(caster, target, final)
  → ignite:
       ApplyStatus(target, "burning", same origin)
  → Burning tick:
       apply "burning_damage" using the same origin
```

The same origin travels through every step, so equipment/class modifiers from the
caster reach the burn without the surface knowing anything about them.

## Files

- `include/EffectRegistry.h`, `src/EffectRegistry.cpp` — atomic effect definitions.
- `include/EffectSystem.h`, `src/EffectSystem.cpp` — origin, context, handlers and
  fold-backed resolution.
- `include/StatusEffectRegistry.h`, `src/StatusEffectRegistry.cpp` — status data.
- `include/SurfaceRegistry.h`, `src/SurfaceRegistry.cpp` — surface triggers.
- `src/SurfaceSystem.cpp` — live surface/status instances and effect dispatch.
- `src/GameECSInitSystems.cpp` — status ticks and system order.
- `data/effects/`, `data/surfaces/`, `data/status_effects/`.
- `data/icons/` — status icons.

## Deliberately deferred

- Composed effect bundles.
- Class registry.
- Snapshot modifier policy.
- Target-side resistance fold for environmental damage.
- AI perception/pathfinding around hazards.
