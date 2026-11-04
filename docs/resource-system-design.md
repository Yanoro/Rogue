# Resources, vital pools and shields

## The rule

A resource definition describes a kind of pool. A character carries the pools its
race/template grants. **Vitality is per-character, not a global property of the
resource**: a human's Mana is not vital, while a ghost's Mana is.

Health is therefore not special. It is one resource definition, one pool, and one
possible vital resource.

## Layers

```text
ResourceRegistry    definitions: icon, color, base max, shield, spendable, ...
ResourcePools       per-character current values and vital/primary flags
ResourceService     the only mutation path: damage, heal, spend, grant
Stat/Fold           resource.max links and future resource-derived modifiers
```

### Resource definitions

Loaded from `data/resources/*.json`:

```json
{
  "name": "Mana",
  "icon": "resource_mana",
  "color": "#3b82f6",
  "baseMax": 50,
  "canBeVital": true,
  "canBeSpendable": true,
  "displayOrder": 10,
  "regenPerSecond": 1.0
}
```

A definition does not say "this resource is always vital". The character profile
does that.

### Character resource profiles

Races, NPC templates and map instances can author:

```json
"resources": [
  { "id": "health", "vital": true, "primary": true },
  { "id": "mana", "vital": false }
]
```

A ghost can declare:

```json
"resources": [
  { "id": "mana", "vital": true, "primary": true }
]
```

with no Health entry at all. The ghost has no HP bar and dies when its Mana
reaches zero. Ordinary damage routes to its primary vital resource, Mana.

Resolution precedence is map instance → NPC template → race → default Health.
Empty means "inherit the next level"; an entirely empty profile gets Health.

### Pools

```cpp
struct ResourcePool {
  std::string id;
  float current;
  bool vital;
  bool primary;
  bool shield;
  float maxOverride;
};
```

Current values are stored. Maximum is resolved from the resource definition's
`baseMax` plus `resource.max` links, unless a pool has an authored override
(training dummies). Constitution uses:

```json
{ "point": "resource.max", "activity": "health", "op": "flat", "perPoint": 4 }
```

### Vitality and death

- Any vital pool at zero makes the character dead.
- `primary` decides which vital resource ordinary damage reduces.
- If no primary is named, the first vital pool becomes primary.
- Spending a vital resource is allowed to reach zero; the normal depletion/death
  path decides the consequence.
- The player still uses the existing knockout/reset behavior; NPCs use the death
  lifecycle.

`DeathSystem` no longer checks `Vitals.health`; it asks
`Resources::IsAlive(entity)`, which checks every vital pool.

### Shields

Shields are separate resource pools with `"shield": true` in their definition.
`Resources::Damage` consumes shield pools first, then applies the remainder to
the primary vital resource. Shields are not alternate Health and do not affect
the vital/death check.

`Resources::Grant` creates/extends a shield pool. Content sources for shields are
deliberately left for a later slice; the damage pipeline already supports them.

## Service API

All changes go through `Resources`:

- `Initialize`
- `EnsureInitialized`
- `Find`, `Current`, `Max`, `Has`
- `IsDamageable`, `IsAlive`, `PrimaryVital`
- `Change`, `Damage`, `Heal`, `Set`
- `CanPay`, `Spend`, `Grant`

No system should write `ResourcePool::current` outside the service.

## What comes next

The service is the extension point:

- **Blood magic**: skill cost alternatives that can spend Health when Mana is
  insufficient, or a class conversion rule.
- **Second-life ring**: a pre-change gate that intercepts a fatal resource
  change, sets the vital pool to 1, and consumes a charge/durability.
- **Percent-HP damage**: a resource query in the damage fold
  (`resource: health`, `measure: percent_missing`), so classes/items can scale
  damage without changing `ApplyHit`.
- **Regeneration**: resource definitions already carry `regenPerSecond`; a later
  tick can respect recent-spend delays.
- **Mana shields**: a pre-change redirect from the primary vital resource to a
  separate shield pool.
- **AI**: later, resources can be exposed in the agent's self/status context.

## Migration note

`Vitals` remains only as an object-template seed for legacy training dummies. It
is converted into a real Health pool at spawn. Runtime combat, targeting, HUD,
death and status effects use `ResourcePools`.

## Deferred

- Resource-change pre/post rule registry (the second-life/blood-magic hooks).
- Resource-derived damage modifiers.
- Regeneration/decay ticks.
- Shield-granting content.
- A race with no Health in `data/races` (the model supports it; no ghost content
  is shipped yet).
