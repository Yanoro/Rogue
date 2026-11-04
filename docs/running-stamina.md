# Stamina and Running

## Stamina

`data/resources/stamina.json` defines a non-vital, spendable resource with a
base maximum of 100. Every player is given Stamina even if the race profile
forgets it; the human race also declares it explicitly.

Stamina is a normal resource pool, so it is visible in the player portrait and
available to the generic cost API. It does not cause death at zero.

## Running skill

`data/skills/running.json` is a **held** skill. It appears in the ability bar,
but clicking the slot does not cast anything; the HUD says to hold Left Shift.

```json
"held": {
  "resource": "stamina",
  "speedMultiplier": 1.6,
  "drainPerSecond": 10,
  "regenPerSecond": 6,
  "regenDelaySeconds": 1.0,
  "minResource": 10
}
```

Behavior:

- Hold Left Shift to sprint.
- Speed is multiplied by `speedMultiplier`.
- Moving drains Stamina at `drainPerSecond`.
- Standing still while holding Shift does not drain.
- Releasing Shift, standing, or running out of Stamina stops the speed bonus.
- Stamina regenerates after `regenDelaySeconds` at `regenPerSecond`.
- Running cannot restart below `minResource`.
- The ability-bar slot is outlined while the held skill is active.

The multiplier is a separate `SpeedMultiplier` component, so it does not
overwrite the player's configured `MaxSpeed`. Both the physics clamp and the
player movement command apply it.

## Generic skill cost

`SkillActivation` now supports a one-time cost:

```json
"cost": { "resource": "stamina", "amount": 10 }
```

`UseSkill` verifies and spends it after target validation and before the cast or
impact begins. This is the foundation for mana costs and, later, blood-magic
payment alternatives. Running uses its sustained drain rather than this cost.

## Current limits

- Running is player-only in this pass.
- No exhaustion status or forced walk state; reaching zero simply stops Running.
- No distance-based drain or terrain cost.
- The ability bar does not have a dedicated held-input prompt beyond the click
  message and active outline.
