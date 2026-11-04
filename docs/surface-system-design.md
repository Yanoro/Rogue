# Surface effects

## The rule

A surface never edits the tile beneath it. Tiles remain the map's terrain and keep
their own art; a surface is a separate runtime layer drawn over that tile and
triggered when a character changes tiles.

That means fire on grass, water on stone and oil on a wooden floor are all the
same kind of thing, and removing a surface restores the tile with no cleanup.

> The effect side of this document has been superseded by
> [effect-registry-design.md](effect-registry-design.md). Surfaces now reference
> atomic effect ids; their calculations live in the shared stat/fold pipeline.

## Content

### Surface sprites

`SurfaceSpriteRegistry` loads `data/surface_sprites/*.json`. A sprite entry is
only reusable art:

```json
{
  "name": "Surface Fire",
  "description": "Overlapping flames that cover a tile without hiding what is underneath."
}
```

The id is the slug of `name` (`surface_fire`). The Designer has a **Surface
sprite** mode that writes this entry and generates its PNG under the normal
sprite-art pipeline. Content Editor exposes the same registry as
`surface_sprites`, with Regenerate available.

### Surface definitions

`SurfaceRegistry` loads `data/surfaces/*.json`:

```json
{
  "name": "Fire",
  "sprite": "surface_fire",
  "duration": 20,
  "layer": 10,
  "alpha": 0.9,
  "tags": ["fire", "hot", "hazard"],
  "enter": ["fire_damage", "ignite"],
  "stay": {
    "every": 1.0,
    "effects": ["fire_damage"]
  },
  "reactions": [
    { "withTag": "water", "removeSelf": true }
  ]
}
```

The entries are **atomic EffectRegistry ids**. The surface owns where and when
something happens; it does not own damage formulas, caster scaling or equipment
modifiers. See [effect-registry-design.md](effect-registry-design.md).

`permanent: true` ignores `duration`. `duration` is seconds otherwise.

### Status effects

`StatusEffectRegistry` loads `data/status_effects/*.json`. A status has an icon,
tags, duration and optional effect ids:

```json
{
  "name": "Burning",
  "icon": "status_burning",
  "tags": ["fire", "burning"],
  "duration": 5,
  "tickSeconds": 1,
  "tick": ["burning_damage"]
}
```

Surfaces and statuses only reference effect ids. The effect fold owns caster
stats/equipment resolution; `StatusEffectSystem` owns duration, tick timing and
expiry. Water applies `Wet` and removes `Burning` through `soak` and `douse`
effects; there is no fire-specific branch in movement.
### Status effects

`StatusEffectRegistry` loads `data/status_effects/*.json`. A status has tags,
duration and optional periodic damage:

```json
{
  "name": "Burning",
  "tags": ["fire", "burning"],
  "duration": 5,
  "damagePerSecond": 2,
  "tickSeconds": 1
}
```

Surfaces only apply ids. `StatusEffectSystem` owns duration, tick damage and
expiry. Water applies `Wet` and removes `Burning`; this is data, not a
fire-specific branch in movement.

## Runtime

`SurfaceGrid` is a sparse `map<(x,y), vector<SurfaceInstance>>`. Each instance
holds:

- definition id
- seconds remaining (`-1` permanent)
- stay-tick clock
- `EffectOrigin` (provenance, not a stat snapshot)
- stacks

The map owns the grid, not the tile entity. Multiple distinct surfaces can share
a cell; reapplying the same definition refreshes it.

`LastTile` on each actor is the single movement event source. `TileTransitionSystem`
compares it with `GamePosition` after position sync and queues enter/exit
transitions. `SurfaceTriggerSystem` processes them after movement has settled.
`SurfaceUpdateSystem` advances durations and applies `stay` triggers.

This means walking, clicked paths, AI paths and teleports all trigger surfaces
through one path.

## Reactions

A reaction is evaluated when a surface is added. If another surface on the cell
carries `withTag`, `removeSelf`, `removeOther` and an optional `result` surface
are applied. The shipped water/fire reaction removes fire when water arrives.
The new surface inherits the origin of the triggering effect.

## Rendering

`SurfaceLayerRender` runs after `DrawTileLayer` and before `DrawEntityLayer`.
Every surface draws its sprite at the cell with its definition's alpha, sorted
by `layer`. The tile is always visible underneath, and actors draw above the
surface.

## Abilities and debug

A skill activation can declare:

```json
"surface": { "id": "fire", "duration": 20, "radius": 0 }
```

Projectile impacts create it at the target tile; cast completions and direct
targeted skills do the same. Fireball leaves fire and Water Ball leaves water.

The Debug Console has a **Surfaces** window. Select a definition, optionally set
an erase/radius/duration override, press **Paint on map**, then click tiles.
Right-click or Escape cancels painting.

## Deliberately deferred

- AI perception and pathfinding cost around hazards.
- Map-authored/persisted surfaces.
- Conditional actions (for example "only if the actor is not Wet").
- Actor footprints larger than one tile.
- Surface animation frames and particle effects.
