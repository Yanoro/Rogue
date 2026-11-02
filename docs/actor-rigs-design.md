# Actor sprites and actor rigs

Two ways to draw a being, and the line between them.

## The two kinds

**Actor sprite** — `data/actor_sprites/<id>.json`. One image per being, resolved as
`<id>.png`. It is what a wolf, a slime or a training dummy needs: an appearance and
nothing else. This kind stays; it is not a legacy step to be migrated away from.

**Actor rig** — `data/rigs/<id>.json`. A body made of parts, each its own whole-rig-sized
transparent PNG, drawn in z-order:

```json
{
  "name": "Villager",
  "width": 32, "height": 32,
  "parts": [
    { "id": "legs",      "sprite": "rig_villager_legs",      "z": 10 },
    { "id": "body",      "sprite": "rig_villager_body",      "z": 20 },
    { "id": "arm_left",  "sprite": "rig_villager_arm_left",  "z": 25 },
    { "id": "arm_right", "sprite": "rig_villager_arm_right", "z": 25 },
    { "id": "head",      "sprite": "rig_villager_head",      "z": 30 },
    { "id": "hair",      "sprite": "rig_villager_hair",      "z": 40 }
  ]
}
```

A rig pays for its layering with several textures and a shared frame. That cost buys the
two things a single image cannot do: equipment drawn *over* the body, and one part hidden
without redrawing another.

Parts are full-canvas layers, not cut-outs. Alignment is therefore by construction — a
helmet drawn at the head's position composites correctly with no pivot arithmetic. Pivots
and anchors become necessary when parts start to move (animation), and are deliberately
not in this slice.

## Worn equipment

An item definition may name how it looks worn, separately from what it looks like in the
world:

```json
// data/items/iron_helmet.json
{ "slot": "head", "occupies": 1, "armor": 2,
  "worn": { "sprite": "worn_iron_helmet", "z": 100, "hides": ["hair"] } }
```

`z` orders it among the rig's worn layers; `hides` removes rig parts it covers. The helmet
hides the hair and leaves the face, which is the whole demonstration: the base body is
still underneath, and only what the equipment actually replaces disappears.

The sword's worn art is drawn in the right hand rather than reused from its world sprite,
because a sword on the ground and a sword in a hand are different pictures.

## The first slice (what exists now)

- `RigRegistry` loads `data/rigs`, same contract as the other content registries
  (`Load`, `Get`, `GetAll`, `ToJson`, `AddOrReplace`, `IdFor`), so it is a row in the
  Content Editor (kind **Actor rigs**) rather than a window of its own.
- `RigRender` owns drawing: parts and worn layers in one z-sorted list, hidden parts
  removed. One function, because the preview and the future world renderer must not
  disagree about order.
- The Content Editor's **Actor rigs** pane shows the composed body at 224 px, with a
  checkbox per item that has worn art. It defaults to everything worn, so the first look
  shows the layering rather than an empty body, and the layer readout lists the final
  draw order and what is hidden.
- The villager rig's parts and the helmet/sword worn art are pixel-DSL documents under
  `data/rigs/villager/` and `data/rigs/worn/`, compiled with
  `--sprite-compile <doc> --sprite-compile-out data/sprites/<id>.png`. The art is a
  placeholder whose job is to prove alignment, not to be the reference style.

## What this slice does not do

- **No equipment lookup.** The preview is a manual outfit: it does not read `Equipped`
  slots from an entity. Equipping a helmet in the world will not change the pane until
  that lookup exists.
- **No world rendering.** Spawned actors still draw their single `Sprite`; nothing reads
  a rig for an entity yet. The renderer is ready, the attachment is not.
- **No animation, no anchors.** The rig has no pivots, and the DSL has no timing (see
  `docs/pixel-dsl.md` §8). A swing needs `arm_right` and the worn sword to move together.
- **No generation.** The Designer does not design rigs or worn art; the villager was
  authored by hand in the DSL.

## Next, in order

1. A component (`RigBody { rig; worn items }`) and a world render path, then choosing a
   rig instead of an actor sprite at spawn.
2. The equipment lookup: `Equipped` slots → worn layers, live.
3. Anchors/pivots per part, then clips whose keyframes rotate/offset parts by id.
4. Designer support for rigs and worn art against the rig's frame.
