# NPC behavior system

The behavior system is a data-driven combat controller for non-player characters. It
sits above the skill system: a behavior chooses a movement policy and one skill
action; the skill supplies range, cooldown, damage, cast time and status effects.

## Data layout

```text
data/behaviors/wolf_pack.json
data/behaviors/zombie_shambler.json
data/behaviors/bandit_melee.json
data/behaviors/mage_kiter.json
```

An NPC template names one behavior:

```json
{
  "name": "Grey Wolf",
  "sprite": "wolf",
  "behavior": "wolf_pack",
  "skills": {
    "claw": { "level": 2 },
    "pounce": { "level": 1 }
  }
}
```

An NPC with no `behavior` keeps the old `Aggressive` / `CombatStrategy`
controllers. NPCs with a behavior are excluded from those systems, so there is
never a second controller moving or attacking the same body.

## Top-level behavior document

```json
{
  "name": "Wolf Pack",
  "description": "Close to melee, claw, and pounce from just outside reach.",
  "maxTargetDistance": 12,
  "repathSeconds": 0.35,
  "movement": {
    "default": {
      "mode": "chase",
      "desiredRange": 1,
      "tolerance": 0
    },
    "rules": []
  },
  "actions": []
}
```

Fields:

| field | meaning |
|---|---|
| `name` | Registry name; the id is the slug of this name. |
| `description` | Editor-facing prose. |
| `maxTargetDistance` | Target acquisition distance, reserved for the detailed targeting pass. |
| `repathSeconds` | Minimum delay between A* recomputes. |
| `movement.default` | Policy used when no reactive rule is active. |
| `movement.rules` | Priority-ordered reactive movement rules. |
| `actions` | Priority-ordered skill actions. |

## Movement policies

```json
{
  "mode": "chase",
  "desiredRange": 1,
  "tolerance": 0,
  "allowActions": true
}
```

Modes:

- `chase` — approach until `desiredRange`; move closer when farther away.
- `kite` — maintain `desiredRange`: retreat when closer than
  `desiredRange - tolerance`, approach when farther than
  `desiredRange + tolerance`.
- `flee` — move directly away from the target.
- `hold` — do not move.

`allowActions: false` disables skill use while the policy is active. Flee rules
use this so a fleeing bandit does not stop to slash.

## Reactive movement rules

Rules are evaluated by descending `priority`. A rule has separate enter and exit
conditions and a minimum duration:

```json
{
  "id": "low_health_flee",
  "priority": 100,
  "enterWhen": { "self.health.pctBelow": 0.30 },
  "exitWhen": { "self.health.pctAbove": 0.45 },
  "minDuration": 2.0,
  "cooldown": 5.0,
  "policy": {
    "mode": "flee",
    "allowActions": false
  }
}
```

Semantics:

1. An active rule stays active at least `minDuration`.
2. It exits when its `exitWhen` passes, or — if no exit condition was authored —
   when its enter condition stops being true.
3. On exit, `cooldown` prevents immediate re-entry.
4. While no rule is active, the highest-priority rule whose enter condition
   passes and whose cooldown is zero becomes active.
5. Otherwise the default policy is used.

This hysteresis prevents a bandit at exactly 30% HP from flickering between chase
and flee every frame.

## Conditions

Conditions are a closed vocabulary, not scripts. Multiple keys in one `when`
object are implicitly combined with AND. Use `all`, `any` and `not` for grouping:

```json
"when": {
  "all": [
    { "target.distanceAtMost": 3 },
    { "target.lacksStatus": "immobilized" }
  ]
}
```

Supported leaves:

| condition | meaning |
|---|---|
| `self.health.pctBelow` / `pctAbove` | Health percentage. |
| `self.resource.<id>.pctBelow` / `pctAbove` | Any generic resource, e.g. `self.resource.mana.pctBelow`. New resources work without code changes. |
| `target.distanceAtLeast` / `AtMost` | Tile distance. |
| `target.hasStatus` / `lacksStatus` | Status id on the target. |
| `self.hasStatus` | Status id on the NPC. |
| `alliesWithin` | `{ "range": 6, "countAtLeast": 1 }` |
| `enemiesWithin` | `{ "range": 4, "countAtLeast": 2 }` |

`alliesWithin` counts other non-player actors; `enemiesWithin` counts the player.
This is enough for pack context, outnumbered checks, and status-aware decisions.

## Actions

Actions are strict priority order. The first valid action wins:

```json
"actions": [
  {
    "skill": "pounce",
    "priority": 20,
    "minRange": 2
  },
  {
    "skill": "claw",
    "priority": 10
  }
]
```

- `skill` is the id in `data/skills`.
- `priority` is descending; first valid action wins.
- `minRange` is optional. The **maximum range comes from the skill's own
  `activation.range`**, so changing the skill range automatically changes the
  behavior.
- `when` uses the same condition vocabulary.

An action is valid when:

- the skill exists and is off cooldown;
- the target is within the skill range and outside `minRange`;
- the action's conditions pass;
- the actor is not casting, immobilized, or disabled by the movement policy.

`pounce` is therefore used at 2–3 tiles with a 3-tile skill range, while `claw`
handles 1 tile. If pounce is on cooldown, claw remains valid.

## Runtime ordering

For each aggressive NPC with a behavior:

1. If neutral, clear any path and do nothing.
2. If casting, stop moving and wait for the cast.
3. Update rule cooldowns and the active movement rule.
4. Try the prioritized actions.
5. If no action was used, apply the movement policy.
6. If immobilized, do not move.

LLM-controlled NPCs are skipped by the brain update while they are aggressive
and have a behavior. Their behavior controller takes over combat. Neutral
LLM NPCs continue to use their brain until combat starts.

## The four stress-test archetypes

### Wolf — chase, claw, pounce

```json
{
  "name": "Wolf Pack",
  "maxTargetDistance": 12,
  "repathSeconds": 0.35,
  "movement": {
    "default": { "mode": "chase", "desiredRange": 1, "tolerance": 0 },
    "rules": []
  },
  "actions": [
    { "skill": "pounce", "priority": 20, "minRange": 2 },
    { "skill": "claw", "priority": 10 }
  ]
}
```

### Zombie — chase, grab, bite

```json
{
  "name": "Zombie Shambler",
  "maxTargetDistance": 12,
  "repathSeconds": 0.5,
  "movement": {
    "default": { "mode": "chase", "desiredRange": 1, "tolerance": 0 },
    "rules": []
  },
  "actions": [
    {
      "skill": "grab",
      "priority": 30,
      "when": { "target.lacksStatus": "immobilized" }
    },
    { "skill": "bite", "priority": 10 }
  ]
}
```

`grab` applies `immobilized` for 5 seconds with a 30-second cooldown. The
condition prevents grabbing an already held target. Movement checks the
`immobilized` status tag, so the player cannot walk while held.

### Bandit — chase until low health, then flee

```json
{
  "name": "Bandit Melee",
  "maxTargetDistance": 12,
  "repathSeconds": 0.5,
  "movement": {
    "default": { "mode": "chase", "desiredRange": 1, "tolerance": 0 },
    "rules": [
      {
        "id": "low_health_flee",
        "priority": 100,
        "enterWhen": { "self.health.pctBelow": 0.30 },
        "exitWhen": { "self.health.pctAbove": 0.45 },
        "minDuration": 2.0,
        "cooldown": 5.0,
        "policy": { "mode": "flee", "allowActions": false }
      }
    ]
  },
  "actions": [
    { "skill": "slash", "priority": 10 }
  ]
}
```

### Mage — kite, then close when mana is low

```json
{
  "name": "Mage Kiter",
  "maxTargetDistance": 12,
  "repathSeconds": 0.4,
  "movement": {
    "default": { "mode": "kite", "desiredRange": 4, "tolerance": 1 },
    "rules": [
      {
        "id": "low_mana_melee",
        "priority": 100,
        "enterWhen": { "self.resource.mana.pctBelow": 0.20 },
        "exitWhen": { "self.resource.mana.pctAbove": 0.40 },
        "minDuration": 2.0,
        "cooldown": 5.0,
        "policy": { "mode": "chase", "desiredRange": 1, "allowActions": true }
      }
    ]
  },
  "actions": [
    { "skill": "bolt", "priority": 10, "minRange": 2 }
  ]
}
```

The mage has no melee skill yet by design, so the low-mana chase closes to melee
and then has no valid action. A future universal auto-attack/staff skill can fill
that slot without changing the behavior rule structure.

## Content Editor

- `Behaviors` is a Content Editor kind. Behaviors can be created and edited as
  JSON through the generic form.
- NPC templates expose `behavior` as a dropdown.
- Skill templates expose `hidden`; creature-only skills are hidden from the
  player's ability bar while remaining usable by NPC behavior.

## Verification

`--combat-check` activates the four map test mobs and checks:

```text
combat-check: behavior-wolf=1 behavior-bandit-flee=1 behavior-mage-low-mana=1 behavior-zombie-grab=1
combat-check: OK
```
