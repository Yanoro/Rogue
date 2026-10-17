# The sprite prompt, for Gemini

One prompt to hand an image model so its output lands in `data/sprites/` without a
fight, and the measurements the wording is built on. The object list to paste into
it lives beside this file in [sprite-object-catalogue.md](sprite-object-catalogue.md).

This is not the in-game sprite designer's prompt. That one (`data/prompts/SPRITE_CONTRACT.md`)
makes an LLM write an ASCII glyph and two colours at runtime; this one makes an
image model draw a picture at authoring time. They share a palette and nothing
else.

**Vocabulary, corrected 2026-10-17.** A **tile is ground only** — a seamless floor, path, grass,
soil or water surface. **Walls, doors, gates, windows, roofs and furniture are objects, never
tiles**, even when they fill a cell edge to edge. See
[tile-and-object-vocabulary.md](tile-and-object-vocabulary.md). The wall/fence/rock art was renamed
to the object convention in the same migration.

---

## 1. What the current style actually is

Stated in numbers, because adjectives are what made the old prompts drift. Measured
over the 57 sprites in `data/sprites/`.

**Shape and size**

| | |
|---|---|
| file | PNG, 32×32, the size the game draws into a 32px cell |
| objects | RGBA, **binary alpha** — zero semi-transparent pixels in all 57 |
| object bounding box | median **27.5 × 30.5 px**, so a 1–3px margin and no more |
| object coverage | median **50%** of the cell's pixels opaque; the rest is empty |
| ground tiles | 4-bit **indexed**, 8–16 colours, full-bleed and opaque, no outline |
| drawn | nearest-neighbour, integer scale (`SpriteFitRect`), so 32×32 is 1x/2x/3x |

**Tone**

| | |
|---|---|
| median mean luminance | **89 / 255** — dark and muted, not bright |
| pixels below 60/255 | **29%** — the silhouette outline plus the shadow side |
| distinct RGB values per object | **200–780**, so the interior shading is smooth where hand-drawn pixel art is banded |

That last row is the honest caveat: the shipped objects *read* as 16-bit pixel art at
1x, but they are downscaled renders, not hand-placed pixels. If a sprite should look
crisper than these, ask for flat bands of colour explicitly — and expect it to look
slightly unlike the existing set.

**Palette**, from `data/palette.json` (128 colours, median-cut from the sprites), dark
to light:

```
timber       #381d10  #5e3620  #845334  #b3692f
stone        #3a3a3a  #515151  #6c6c6c  #8e8e8e  #a5a5a5
earth        #3c1f10  #542d18  #673920  #724226
grass        #3c6f17  #447a19  #53941a  #68b518  #7dca21
water        #0d427f  #114b89  #176ab1
sand/path    #875f3a  #b49270  #caa57b  #dbb98b  #ede2cb
iron         #1d1d1d  #3f4040  #5f6060  #838283
red cloth    #5a1c2e  #7f2b38  #ad2e2d
brass/gold   #c6943d  #d7b46a  #f2bc34
```

**Voice.** The art pack says it in prose already, in `packs/art/village/pack.json`, and
that paragraph is the canonical short statement of the style:

> A small European farming village, drawn in chunky retro pixel art at 32x32. Colours
> are muted and earthy: olive and grass greens, grey stone with warm mortar, brown
> timber and tilled earth, pale sand paths, deep blue water. Shading is flat and
> hand-placed, with a few tones per material rather than smooth gradients, and textures
> are grainy rather than clean … Objects and characters sit in the same palette with more
> contrast and a stronger silhouette, and they carry the only real outlines in the set.

**View.** A top-down world of ground tiles with objects standing on them, drawn in a
consistent three-quarter view — a chest shows its lid and its face, a table its top and
its front legs. No isometric diamonds, no horizon.

---

## 2. Block A — the style. Paste this first, once.

Send it as the first message, or as the system instruction if the client allows one.
Everything after it can be a bare list of objects.

```
You are drawing sprite assets for AIRogue, a top-down medieval village simulation:
the world is a grid of 32x32 pixel cells, people live in it and walk around, and every
building, object and person is drawn into one cell.

Draw PIXEL ART at a true 32x32 pixel scale -- the size the game actually draws at -- in
the style of a 1990s 16-bit game. Build each object from a few chunky masses and
deliberate single-pixel marks rather than fine detail, and judge your work at 32x32: if
it does not read at that size it is wrong, however good it looks large.

PALETTE. Muted, earthy and weathered, drawn from the game's existing art. Dark to light:
timber #381d10 #5e3620 #845334 #b3692f; stone #3a3a3a #515151 #6c6c6c #8e8e8e #a5a5a5;
earth #3c1f10 #542d18 #673920 #724226; grass #3c6f17 #447a19 #53941a #68b518 #7dca21;
water #0d427f #114b89 #176ab1; sand and parchment #875f3a #b49270 #caa57b #dbb98b
#ede2cb; iron #1d1d1d #3f4040 #5f6060 #838283; red cloth #5a1c2e #7f2b38 #ad2e2d;
brass and gold #c6943d #d7b46a #f2bc34. About a dozen tones for the whole object, two or
three per material: a shadow tone, a mid tone, a highlight. Nothing neon, nothing
glossy, no pure white, and pure black only in the outline.

LIGHT comes from the upper left. The top and left faces of a form take the highlight,
the bottom and right the shadow, and one darker tone marks where two forms meet -- a
plank edge, a lid seam, the joint between two stones. Colour is laid down in flat
hand-placed bands: no smooth gradient, no ambient shading blur, no specular highlight,
no glow, no dithering, no noise texture.

OUTLINE. Exactly one pixel of dark warm brown or near-black follows the entire
silhouette and never fades, breaks, or turns semi-transparent. Interior divisions get a
thinner darker line of the material's own colour. CRITICAL: every edge is hard -- no
anti-aliasing, no soft or feathered edge, no semi-transparent pixels anywhere, including
where the object meets the background.

VIEW. One consistent three-quarter view for everything, seen from slightly above and in
front: a table shows its top and its front legs, a chest its lid and its face, a bed its
mattress and its footboard. No isometric rotation, no vanishing point, no horizon, no
straight side-on elevation.

COMPOSITION. One isolated object, centred in its own cell, filling 26 to 30 of the 32
pixels in each direction and leaving a 1 to 3 pixel margin. It sits on nothing: no ground
plane, no floor patch, no plinth, no pedestal, no contact shadow, no cast shadow. Keep
real-world relative size honest across the set -- a door is taller than a crate, a crate
is wider than a jug.

BACKGROUND. One flat, uniform, fully saturated magenta (#FF00FF) filling all space around
and between the objects. No frame, border, grid lines, panel, vignette, paper texture,
label, letter, number or caption anywhere in the image.

Do not draw: a scene, a room, a street, a landscape, a shadow, a reflection, text, a
border, a 3D render, a photograph, a cartoon-with-outlines, a smooth gradient, or an
object built from many tiny details.
```

## 3. Block B — a batch. Paste one after Block A.

The grid is a contract: the sprite key is assigned by *position*, so an object in the
wrong cell is a sprite with the wrong name. Say so every time.

```
Draw these 18 objects in this order, as a strict grid of 6 columns by 3 rows, one object
centred in each cell, all at the same scale and in the same style:

 1) ...
 2) ...
 …
18) ...

Before you finish, check: exactly 18 objects, in exactly this order, none repeated, none
merged across a cell boundary, and every one drawn in the style above.
```

Rules of thumb:

- **18 per sheet** (6×3) is comfortable. 20 works but the model starts drifting mid-sheet;
  beyond that objects come back duplicated or missing.
- **Order is untrusted by the tooling.** `sprite_preview/slice_object_sheet.py` keeps an
  explicit cell-to-name map and checks it by eye precisely because a past sheet came back
  with 20 cells instead of 19, one object duplicated, and everything after it shifted down
  a slot. Always look at the labelled contact sheet before promoting anything.
- **Rerolls are cheap and reordering is not.** If one object on a sheet is wrong, the sheet
  is usually still usable — check the cell map first.

### One object on its own

When you want a single asset, or to reroll one, drop the grid sentence entirely:

```
Draw one object, exactly one, isolated and centred, on a flat magenta (#FF00FF)
background, in the style above: a wooden bed with a straw mattress, a grey blanket folded
at the foot and one pale pillow, on a heavy dark timber frame.
```

## 4. Variants of the ask

**Full-bleed ground tiles** (floors, water, paths, grass, soil) are not objects, and the differences
matter more than the similarities. A tile repeats, so it must be seamless; it is a surface, so it is
seen from directly above; and it must contain no prop, because a tree baked into a floor tile is
duplicated on every one of them.

**Walls, doors, roofs and other structure are objects, not tiles.** Ask for them with the object
prompt, even when they must fill the cell: a door is a cell-filling object, a wall section is a
cell-filling object, and neither belongs in the seamless-ground batch. Name their sprites with the
bare object key (`door.png`, `stone_wall.png`), not `tile_door.png` or `tile_stone_wall.png`. The
wall/fence/rock sprites were renamed to the object convention in the 2026-10-17 migration; see
[tile-and-object-vocabulary.md](tile-and-object-vocabulary.md) §4.

```
Draw these 6 seamless ground textures, as a strict grid of 3 columns by 2 rows. Each is a
top-down surface seen from directly above, flat and evenly lit, filling its cell edge to
edge: no perspective, no horizon, no cast shadow, no lighting gradient. Fill the whole cell:
these are full-bleed, not objects, so no outline, no frame, no vignette and no empty margin.
Every texture must be seamless and repeatable -- its left edge must continue perfectly into
its right edge and its top edge into its bottom edge, so repeated copies read as one
continuous surface with no seam. Uniform in tone across the cell, with no darker or lighter
edges. Use the palette and the chunky 16-bit pixel style of the existing set: [ ... ]
Do not draw any object, creature, plant, tool or character in any cell.
```

**Characters** need their own paragraph, because the rules that make furniture readable
make a person look like a doll:

```
A person, front-facing, standing with the feet on the bottom edge of the cell and the head
near the top: about 16 to 19 pixels wide and 30 tall, so the figure fills the cell
vertically and is thin. Feet, hands and face are a handful of pixels each -- the profession
is carried by the silhouette and the clothing colours, not by detail: [ farmer ] with [ ... ]
```

Only one facing exists in the art today (front). A four-direction walk is a separate job and
a sprite-sheet job, not a single-image job.

**State variants** are how a simulation becomes legible — the same object drawn twice or
three times. `Evolvable` already switches crop stages by template, so these fit the engine
directly: door closed / ajar / open; gate closed / open; window shuttered / open; lantern,
candle, hearth and forge unlit / lit; barrel full / empty; crate intact / broken; bed made /
slept-in; chest closed / open; pot empty / full.

## 5. Where the output goes

1. Land the sheet in `sprite_preview/raw/` — never straight into `data/sprites/`.
   Generated art must not feed back into the pack it is emulating, and promotion is a
   human decision (`docs/asset-generation-design.md` §1.2).
2. Slice and key it:

   ```bash
   nix-shell -p imagemagick --run "python3 sprite_preview/slice_object_sheet.py \
       --sheet sprite_preview/raw/<sheet>.png --out sprite_preview/<name>_out --number"
   ```

   That keys the magenta, crops each cell, remaps to `data/palette.png` and writes 32×32
   RGBA. Review at **1x**, not zoomed — a defect that is invisible at 32px is not a defect
   at 32px.
3. Promote by hand, to the name the engine will look for:
   - objects: `data/sprites/<template key>.png`, where the key is the object template's
     lowercase slug (`apple_tree.json` → `apple_tree.png`); walls, doors and other structure
     use this form, never the `tile_` one;
   - ground tiles: `data/sprites/tile_<slug>.png` — the prefix is mandatory for **ground**
     tiles, `TileSpriteId()` adds it, and a bare `water.png` would be ambiguous between a
     ground tile and an object.
4. Ask the engine what is still missing:

   ```bash
   ./AIRogue --gaps          # writes data/gaps.json: definitions with no sprite
   ```

   `coverage` in that file is the generation worklist. It is the only list that cannot
   drift from the code.

## 6. Defects this prompt is written against

Each of these was seen in output, and each has a sentence above aimed at it.

| defect | what it looks like | what catches it |
|---|---|---|
| soft contact shadow | a grey rim or smudge under the object at 1x | "sits on nothing", repeated in COMPOSITION and in "Do not draw" |
| the scene model | a barrel drawn in a street with a fence and a path | "one isolated object"; "not a scene" |
| cell drift | 20 cells, a duplicate, everything shifted a slot | "exactly 18, in this order"; the explicit cell map in the slicer |
| re-drawn as a diorama | furniture drawn as a small room around itself | "no ground plane, no floor patch, no plinth" |
| palette drift | garish, over-saturated, glossy | the hex ramps, and "nothing neon, nothing glossy" |
| mush at 32px | too much detail for the cell | "a few chunky masses", "judge at 32×32" |
| boundary merge | two neighbours fused into one blob | magenta background + the gutter rule for tiles |

One failure from the *local* ComfyUI path does not apply here and must not be imported:
that model had to be told *not* to ask for "chunky pixels", because it obeyed literally and
painted enormous blocks. Gemini with the template above produced the current pack with the
word "chunky" in it. Keep the language: it is proven on this model and only this model.
