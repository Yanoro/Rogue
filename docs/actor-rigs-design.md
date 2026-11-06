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

## Animation

### Movements

A movement is the shared half of an animation: what "walking", "running" or "a kick" is, in
words and timing. It lives in `data/movements/<id>.json` and is listed in the Content Editor
under **Movements**. The id is the slug of its name (`"Sword Swing"` -> `sword_swing`), and
that id is what a skill names (`"animation": "kick"`) and what a rig clip stores.

Fields: `category`, `description` (the whole-body motion the part requests are cut from),
`frames`, `fps`, `loop`, `parts` (which rig part ids participate; empty means all),
`requires` (capabilities such as `weapon` or `focus`), and `poses` (one line per frame). A
movement that names participants animates only those; the rest keep their static sprite,
which is how "a kick does not animate the hair" is expressed.

`requires` is a gate for a capability the rig cannot supply. The grip path now draws the
equipped hand item, so `sword_swing` and `spell_cast` no longer declare `weapon`/`focus`;
they generate like any other clip and the weapon follows the arm on the grip path. A
movement that still declares a requirement is refused until that requirement exists.

### Clips

A clip is a rig-level row (`clips` in the rig JSON: id, optional `movement`, frame count,
fps, loop, the motion brief and one pose line per frame) plus, per part, a frame list
(`parts[].clips.<id>`) of ordinary sprite conventions. Creating a clip from a movement
copies its frames, fps, loop, brief and poses and stores the movement id; the copy is
editable, so a rig can override the cadence without inventing the motion again. Editing a
movement later does not rewrite existing clips -- a new clip picks up the new text, and
regenerating an existing one uses the clip's own copied intent. A part with no entry for a clip keeps its static `sprite`, which is also the
idle pose -- there is no separate idle animation yet.

Frame names are deterministic: `<part sprite>_<clip>_<generation>_f1..fN` for a stored
generation (`v1`, `v2`, ...), and `<part sprite>_<clip>_f1..fN` for the original generation
that existed before generations were kept. The rig JSON declares them, and the animation
document (requested under `<part sprite>_<clip>_<generation>`) is written to those names,
so the rig can point at frames before any art exists. The base part sprite is never
overwritten.

### Stored generations

A clip keeps every generation it has produced. The rig JSON stores a `variants` array on
the clip; each entry has an `id`, a display `name` (Original, Generation 1, ...), a
`created` timestamp and a `parts` map from part id to that generation's frame list. The
active generation is `activeVariant`; the per-part `clips.<clip>` lists mirror it, so the
renderer, the previews and skill playback read one active set and need no new lookup.

- **Generating creates a generation, it does not overwrite.** `Generate <clip> for all
  layers` allocates the next `vN`, declares its per-part frames, appends the variant,
  makes it active and saves. The previous generation's PNGs and `.sprite` documents stay
  on disk.
- **Selecting is a copy, not a regeneration.** The Content Editor and Designer clip
  sections have a generation selector. Choosing one copies its frame lists into the
  parts and saves (Content Editor) or updates the draft (Designer); no art requests are
  queued.
- **The original generation** is materialized by the rig loader from the frames that
  existed before variants, so existing rigs gain an `Original` entry and can still be
  selected.
- **Animating one layer alone** (Designer layer list) forks a new generation that inherits
  the other layers from the active one, then regenerates just that layer. The old
  generation is untouched.
- Skills and the world use the active generation. Switching a clip's generation changes
  what plays in game; it does not delete anything.

Generation is one request per part for the whole clip, queued behind the same one-at-a-time
art pump as every other layer:

1. **Create a clip** picks a movement (walk, run, idle, kick...) and copies its timing and
   pose plan onto the draft.
2. **Generate <movement> for all layers** declares the participating parts' frame sprites,
   then asks the model for that part's `N`-frame document. The prompt names the frames it must produce
   (`f1..fN`), prints the pose lines, and says every frame is a complete drawing of the
   same layer, anchored to the same pixels that do not move.
3. Acceptance validates the document's frame count and names, refuses a mismatch through
   the normal retry budget, writes the `.sprite` document, and writes one PNG per frame
   (`<base>_f1.png`..). The texture memo is forgotten per frame, so the preview picks them
   up as they land.
4. A part can be redrawn alone with **Animate walk** in the layer list.

The preview plays the selected clip at its fps with Play/Pause, and the base sprites show
through wherever a part has no frames. The world now plays the same clips through the rig
renderer; the preview is the authoring view of them, with the manual outfit standing in
for live equipment.

Limits, stated rather than hidden:

- **No anchors.** Frames are kept registered by the prompt's wording and the fixed 32x32
  frame; the compiler cannot measure joint drift. The preview is how you catch it.
- **No motion validation.** The compiler checks frame count/names and each frame's pixels;
  it cannot tell a walk from a shuffle.
- **Hand items follow the grip path; other worn equipment is static.** A sword or focus is
  drawn at the hand read from the arm frame; a helmet or cuirass keeps its z and does not
  follow the part
  it sits on.
- **The clip spec and the movement text are separate copies.** Editing a movement changes
  what new clips are created with; existing clips keep their own brief and poses (the
  movement id is kept for the participant/requirement gate). A "reapply from movement"
  action is the obvious follow-up.
- **Skills name a movement but nothing plays it yet.** A skill's `animation` field is
  validated against the registry at load and shown in the Content Editor, but until the
  world renders a rig there is no playback path; an actor whose rig lacks the clip still
  falls back to its static look.
- Changing a clip's frame count does not rewrite the existing per-part frame lists; the
  next Generate does, and the registry warns on a saved mismatch.

### Animating an existing rig

A rig's JSON now stores each part's `brief` (the prose it was drawn from), so a saved rig
can be animated without re-drafting the whole body. In the Content Editor's **Actor rigs**
pane:

1. select the rig;
2. **Animation → Add clip** picks a movement and writes the clip plus its deterministic
   per-part frame sprites into the live registry and the editor document, then saves the
   rig immediately -- the frames take seconds to generate and a restart between Add and a
   later Save would leave the PNGs with no clip pointing at them;
3. **Generate <clip> for all layers** queues one multi-frame document per participating
   part through the Designer's art session (the Designer window opens to pump the replies);
   the frames appear in the pane as they land, Play/Pause previews the clip, and a frame
   scrubber inspects it. The preview **loops** one-shot clips and restarts the clock when
   frames arrive: generation is slow relative to a 0.3s kick, so without that the first
   thing seen is the last frame;
4. **Save** writes the clip declarations to the Content Editor's rig override
   (`content.editor/rigs/<id>.json`), leaving the catalog file untouched.

The prompt gets the part's `brief`, the movement's description and pose plan, and -- when
one exists -- the layer's current `.sprite` document under `<base-document>`. The model is
told to produce the frames by **editing that document**: keep its palette and every
operation the pose does not need to change. That is the identity fix for the base being
shirtless while the animation invented leather armour; the model starts from the existing
art rather than from the brief. Generated rigs keep their source beside the sprite; a
hand-authored rig like the villager falls back to `data/rigs/<rig>/<part>.sprite`. There is
**no automatic palette guard yet** -- a disobedient reply can still drift, and the base
document costs prompt tokens per part. The movement's `requires` gate still applies to any
movement that declares one; the shipped weapon/cast movements no longer do, because the
grip path draws the hand item.

The segmented-cutout and per-weapon-frame experiments were throwaway debug windows and
have been removed; the grip path below is the production result.

### Rig geometry and style proportions

A rig's proportions are no longer only prose in the part briefs. `RigGeometry` (in
`include/RigRegistry.h`) records the frame size, the head/hair/body/arm/leg rectangles,
the right-hand point and the foot point. The default is the canonical 32x32 humanoid, so
existing rigs are unchanged. A rig may carry its own `"geometry"` object, or name a
`"style"` whose `ArtStyleDef` carries one; `ArtStyleRegistry::GeometryFor` resolves
rig -> style -> default.

`RigDesigner::FrameContract(geometry)` prints the dynamic contract, and the rig part,
animation, worn-equipment and item worn-art prompts are cut from the same geometry. The
Designer rig pane has an **Art style** picker; picking a style with a geometry changes
every layer request for that draft. The `data/art_styles/ragnarok_online.json` sample is a
chibi profile (large head, small body) and `data/rigs/chibi_test.json` is the matching
hand-authored test rig.

Slice 2 uses the geometry in the world and in the previews. `DrawRigEntity` draws from
`RigGeometry.footX/footY`, not the cell's top-left, so a frame larger than 32x32 extends
above the cell; the collision and selection cell stays 32. The entity pass is sorted by
those feet, so a tall rig's head is not covered by a neighbour's cell. Worn equipment is
placed by slot: the item's worn art is authored in its item style's frame, and the rig's
geometry supplies the target head/torso/legs/feet rectangle; the renderer transforms the
full worn frame between them. Hand items stay on the grip path, with the grip scaled by the
rig's frame. The same transform runs in the Content Editor, the Designer and the world, so
the preview and the game agree.

### Grips

A hand item carries `worn.grip` in its definition:

```json
"worn": { "sprite": "worn_iron_sword", "z": 110,
          "grip": { "x": 24, "y": 22, "angle": 0 } }
```

- `x`/`y` are the pixel in the item's 32x32 worn canvas where the hand closes.
- `angle` is the item's rest orientation relative to the arm; the grip path rotates by the
  arm direction plus this angle.
- The equipment designer **requires** the grip for `slot: hand`, validates 0..31, and the
  worn-art prompt tells the painter to draw the handle at that point. The Content Editor's
  item pane has X / Y / rest-angle controls with a **live arm-frame preview** and a **Save
  grip** button, so a hold that is slightly off is corrected at runtime and written to
  the item override.

The grip path reads the hand (bottom of the arm frame) and the arm direction
(shoulder-to-hand) from the **arm animation frame** and places the static worn art there.
The world renderer now does this for equipped hand items: they follow the arm frame of the
clip instead of sitting at a fixed z. It is a heuristic, not an authored anchor -- the next
upgrade is a hand point per arm frame if a clip reads badly. The grip is definition-level:
every instance of the item shares it. Helmets and other non-hand worn items have no grip
and keep their z.

## World rendering and playback

`RigBody { rig }` on an actor makes the world draw it part by part instead of as a single
`Sprite`. It is assigned by `player.rig` (a setting; restart to apply), an NPC template's
`"rig"`, or a map NPC's `"rig"`. An unknown id is reported at spawn and the actor keeps its
sprite, so the fallback is visible rather than an invisible body. `DrawRigEntity` builds the
worn layers from the items the actor actually has equipped -- `Equipped` on `Holds`
children, `ItemType` -> `ItemDef` -> worn sprite/z/hides -- and draws through the same
ordering and hiding the previews use.

A `RigAnimationSystem` chooses the locomotion clip from the actor's real speed: `idle` when
still, `run` at or above 60% of `MaxSpeed` (120 when the actor has none), `walk` otherwise,
and advances the clip's clock. A rig with none of those clips -- including one with only a
`kick` -- draws its static sprites, because a one-shot has no business being chosen by a
speed test. The renderer only reads `RigAnim`; the clock is the game's, so a frame that
skips the draw does not lose animation time.

To try it: give the player or an NPC a rig id (`player.rig` in settings, `"rig"` on a
template or map NPC), make sure that rig has `idle`/`walk`/`run` clips (generate them in the
Designer and save), and move. Equipment is live: the player's map starting equipment and an
NPC template's `equipped` list appear as worn layers.

## What this slice does not do

- **Hand items have a grip, not authored hand anchors.** The grip path reads the hand from
  the arm frame, so a sword or focus follows the arm through a swing or cast. It is a
  heuristic; other worn equipment (helmets, armour) still has no per-frame attachment. A
  hand point per arm frame is the upgrade if a clip puts the hand where the heuristic does
  not expect it.
- **No directional art.** Rigs are drawn front-facing; there is no mirroring or
  four-direction vocabulary, so a character walking left looks like one walking right.
- **Skill playback is a one-shot at activation.** `UseSkill` starts the skill's movement
  clip on the actor's rig (after the target/range/cooldown checks pass), and the animation
  system plays it once and returns to locomotion. The weapon skills now name their clips:
  Attack, Heavy Swing and Power Strike play `sword_swing`; Fireball and Ice Lance play
  `spell_cast`. The equipped hand item follows on the grip path, so the clip only has to
  contain the body and arms. It requires the actor to be rigged and
  its rig to have that clip; an actor with no rig, or a rig without the clip, still uses
  the skill -- it simply has no animation for that body. Cast-time skills start the
  animation at activation, not when the effect lands, so the visual is not synced to the
  cast bar or the hit.
- **The previews are still a manual outfit.** The Content Editor and Designer previews do
  not read `Equipped`; the world does. A pane showing a helmet you have not toggled is not a
  bug, it is the preview's deliberate control.
- **No world object for generated equipment.** An item can be worn in the world if its item
  definition exists, but a generated item still cannot spawn in a room until its object
  template is made with the Object domain.

## Next, in order

1. Per-frame hand anchors (the grip path's arm-frame heuristic is the current stand-in) or
   directional weapon art for large swings; `sword_swing` and `spell_cast` already generate
   and play with the grip path.
2. Directional rig art, or at least a horizontal flip, and the facing wiring.
3. The previews read the live equipment lookup instead of a manual outfit; the rig previews
   now draw equipped hand items on the grip path.
4. Cast-time skills could hold their cast clip until the cast completes, instead of the
   one-shot starting at activation.
