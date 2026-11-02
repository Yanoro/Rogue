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

## The Designer domain

The rig's definition can be authored by the Designer. A request for a *person* (a
villager, a soldier, a smith) routes to the **Rig** domain; the reply is a name,
a description and one short brief per canonical part:

```json
{ "name": "Village Blacksmith", "designerNotes": "...", "width": 32, "height": 32,
  "parts": { "legs": "...", "body": "...", "arm_left": "...", "arm_right": "...",
             "head": "...", "hair": "..." } }
```

The model writes definitions, not pictures. The sprite-document pipeline draws each
layer from its brief plus the shared frame contract (`RigDesigner::FrameContract`): the
32x32 canvas and the six rectangles every part is drawn against. The Designer pane queues
one art request at a time — the document conversation is one-at-a-time by design — shows
the composed body as layers arrive, and lets any single layer be redrawn. Worn art for
existing equipment is requested the same way (a helmet hides `hair` by its item
definition, not by the prompt).

A drafted rig is saved with **Save rig** into `content.generatedRigs`
(`data/generated/rigs`), which loads after `data/rigs`:

- the hand-authored catalog is never written to;
- a generated rig with the same id replaces the catalog one in the world, and the pane
  says so before saving;
- deleting the generated file reverts to the catalog.

The pane also warns when a layer has no art yet; a blank preview is not a state the user
should have to diagnose.

## Equipment generation

The Designer's **Equipment** domain generates a worn item: its definition and the brief
for how it looks on a rig.

```json
{ "name": "Iron Kettle Helm", "slot": "head", "tier": 2, "armor": 2,
  "tags": ["armor", "iron"],
  "worn": { "brief": "a rounded iron cap with a nose guard", "hides": ["hair"] } }
```

- **Slots are the rig-wearable ones**: head, torso, legs, feet, hand. Anything else is
  refused with a reason, because there is no worn layer to draw.
- `worn.sprite` is `worn_<slug>`, `z` is fixed per slot (feet 45, legs 50, torso 60, head
  100, hand 110) so a model cannot layer a helmet behind the hair, and `hides` is validated
  against the canonical rig parts.
- Hand items may carry `damage`, `swingTime` and `durability`; a hand item with damage 0
  is a tool and gets a warning, not a refusal.
- The pane draws the worn layer on a chosen rig (dropdown) and queues the worn art through
  the same sprite-document path as a rig layer, respecting "Art automatically".
- **Save item** writes `content.generatedItems` (`data/generated/items`), loaded after
  `data/items`. An existing item with the same id is replaced in the world, and the pane
  says so.

The Content Editor lists the item registry under **Equipment**. The definition is editable
through the generic form -- the id is the selection, injected on save, so the form has no
id field -- and the preview shows the numbers, the worn sprite on its own, and the layer
composed on a chosen rig. The two views answer different questions: the plate is "is this
layer right", the composition is "does it fit the body".

The equipment tab uses the **same sprite asset section as an actor sprite**: the preview,
the document state, Regenerate, and the archive grid of every generation. Anything that has
been generated can be seen and made active again from that grid, so regenerating never
loses a look.

Regeneration archives before it asks: the current image is copied aside as a variant, *and
if the picture on screen is the built-in `data/sprites` file rather than a generated one,
that file is copied into the generated layer first so the archive keeps it too*. The
Designer then asks for a new document (a stored one is recompiled only when the source is
the DSL and nothing was asked to change) and the new PNG is noticed through the file's
write time rather than a guess about the queue.

A worn item carries its worn brief in `worn.brief`, so a regeneration redraws from the
intent that produced the layer instead of from a bare sprite id. Re-draws also say so in
the request: the sprite-document prompt is told this is a re-draw and must vary the
silhouette, materials or detail, because asked the same question a model reliably returns
the same document -- which looked exactly like "regenerated but unchanged". Hand-authored
items without a brief fall back to the item id plus the rig frame, and the existing helmet
and sword now carry one too.

The Designer's transcript can follow new output: the **Auto-scroll** toggle keeps the
newest reply in view while it streams, and only moves when the transcript actually grew, so
an idle scrollback is not yanked back. Generated items (`data/generated/items`), Designer overrides
(`data/editor/items`) and the hand catalog all land in the same registry, with the catalog
first.

What an equipment reply does **not** include is the world object template — the thing that
makes an item spawnable in a room. That format belongs to the object designer, and until it
exists a generated item can be worn in previews but not placed by an agent.

## What this slice does not do

- **No equipment lookup.** The preview is a manual outfit: it does not read `Equipped`
  slots from an entity. Equipping a helmet in the world will not change the pane until
  that lookup exists.
- **No world rendering.** Spawned actors still draw their single `Sprite`; nothing reads
  a rig for an entity yet. The renderer is ready, the attachment is not.
- **No animation, no anchors.** The rig has no pivots, and the DSL has no timing (see
  `docs/pixel-dsl.md` §8). A swing needs `arm_right` and the worn sword to move together.
- **No world object for generated equipment.** An item can be worn in a preview; it
  cannot spawn in a room until its object template is made with the Object domain.
- **No world rendering or equipment lookup** still: a saved rig is visible in the
  Content Editor and the Designer, not on a spawned actor.

## Next, in order

1. A component (`RigBody { rig; worn items }`) and a world render path, then choosing a
   rig instead of an actor sprite at spawn.
2. The equipment lookup: `Equipped` slots → worn layers, live.
3. Anchors/pivots per part, then clips whose keyframes rotate/offset parts by id.
4. An equipment domain: item definitions (slot, armour, hides) generated beside the worn
   art.
