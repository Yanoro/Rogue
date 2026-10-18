# Art packs and data packs: re-skinning a world, and growing art to fit what the agents build

**Status: design note. Nothing in here is implemented except where it cites existing tooling.**
Written 2026-10-15, revised 2026-10-15 for the art pack / data pack split. It answers two questions:

1. *When an agent builds something the art pack has no art for, how does the art come to exist, in
   the art pack's own style?*
2. *What has to be true for an art pack and a data pack to be swapped independently?*

Read `blueprint-design.md` first — the designer agent that emits the structures this document has to
dress. Read `TileRegistry.h` for the **ground-tile** catalog that is the world's material vocabulary;
walls, doors and furniture come from object templates, not from that catalog. Read
`tools/README.md` for the design-time tile generator and the measurements it already took.

**Vocabulary, corrected 2026-10-17.** A **tile is ground only**; walls, doors, roofs, fixtures and
props are **objects**, never tiles. The data migration landed the same day, so the `wooden_wall`
example below is historical; the canonical rule and migration record are in
[tile-and-object-vocabulary.md](tile-and-object-vocabulary.md).

---

## 1. Two kinds of pack

The word "pack" was doing two jobs, and separating them is the load-bearing decision in this
document.

| | **data pack** | **art pack** |
|---|---|---|
| owns | gameplay-critical definitions: materials, objects, items, recipes, stats, skills, races, blueprints | the art style: sprites, palette, glyph *appearance*, fonts, chat colours |
| changes what | an agent can name, and what the world *does* | what the world *looks like* |
| example | a village economy; a cyberpunk workshop set | high fantasy; dark fantasy; Pixel Crawler |
| file today | `data/{tiles,objects,items,recipes,stats,skills,races,blueprints}/` | `data/sprites/` (+ colours scattered in the data files) |

The two are **orthogonal**, joined at exactly one place: **the name.**

That join key is not a design to be invented — it already exists everywhere:

- `TileRegistry::Get(name)` resolves a **ground** material by string
- a blueprint says `"paint": "stone floor"` — a ground tile name, not an id or a pointer. A wall or
  door is an object and is referenced by `"place": "<object id>"` instead
- `SpritePaths.cpp` resolves the convention path `data/sprites/tile_<slugified name>.png` for a
  **ground tile**; an object resolves the bare `<slug>.png`
- `ObjectFactory` keys templates by id and falls back to `data/sprites/<id>.png`
- `GetSurroundingsRadar` prints a legend mapping glyph -> `tile->name` for the ground layer
  (`include/AgentActions.h:550`)

So an art pack is **a partial map from name to appearance**, and the data pack is **the set of names
and what they mean**. The split is not new machinery. It is un-fusing two halves of a record that are
currently stored in the same file — `data/tiles/wooden_wall.json` says both `"blocksTile": true` and
`"characterColor": [200,200,200,255]`. That file is also the misclassification this correction
records: a wall is an **object**, not a ground tile, so it should carry components and an object
template, not a `TileDef`. It is debt (see `tile-and-object-vocabulary.md` §4), not the model for
the split.

### 1.1 What the split buys

The two axes recombine, and each combination is meaningful:

- **one data pack, many art packs** — the same village economy rendered high fantasy, then dark
  fantasy. Gameplay identical, world unrecognisable.
- **one art pack, many data packs** — a village economy and a cyberpunk workshop set both drawn by
  the same hand, so two mechanically different worlds read as one coherent product.
- **the gap-filling pipeline is what makes the second case practical.** See §3 — this is the part
  that is not obvious, and it is why this document still exists after the split.

### 1.2 Three runtime layers, and the generated layer is never a reference

Whatever else changes, the art pack the player selected is **the style**, and the generator's job is
to emulate it. That only works if generated output can never feed back into the thing being emulated.
So resolution is layered, and the layers are separate on disk:

| layer | contains | authority |
|---|---|---|
| **data pack** | definitions: identity, mechanics, glyph, description | the vocabulary. Agents design against it. |
| **art pack** | sprites, palette, fonts, LoRA | **the style.** Authoritative appearance. |
| **generated layer** | generated sprite overlays + generated definitions | additive. Fills what the layer above lacks. |

Two rules make the first two layers stay a clean reference:

1. **The art pack always wins.** The generated layer is strictly additive — it supplies an appearance
   only for names the art pack has none for, and it **yields silently** if the art pack later ships
   the real sprite. A generated image can never shadow hand-authored art, so "loaded a pack with more
   art" is always a strict improvement.
2. **The generated layer is never an input to anything that models style.** Not the LoRA training
   set, not the palette derivation (§7.3), not the style reference images. This is what "undiluted"
   means operationally, and it is why the layer is a sibling directory rather than a subdirectory of
   the art pack's `sprites/`: `sprites/` stays a hand-curated set that a script may safely read.

**The generated layer is scoped to the art pack, and persists.** One `bed` invented in one world looks
the same in the next world that uses that art pack, and is not regenerated. The cost is that agent
invention is a property of the pack rather than of a save — accepted deliberately, because the
alternative is the same object looking different in every world.

The same two rules apply **symmetrically to definitions** (§4.2). Agent-created vocabulary lives in a
generated overlay and never in the hand-authored catalog, so the data pack a designer designs against
stays exactly as written.

---

## 2. What separates cleanly, and the one thing that does not

Going field by field through the definitions, most of it falls on one side without argument:

| field | today lives in | belongs to | why |
|---|---|---|---|
| `name` | tiles, objects, races | **data** | the join key *and* the legend value in the placement radar |
| `description` | objects | **data** | the LLM reads it; it is meaning, not pixels |
| `components`, `capacity` | objects | **data** | behaviour (`Interactable`, `Obstacle`, `Storage`, `Workstation`) |
| `blocksTile`, `hitboxSize` | tiles | **data** | collision, walkability, placement validation |
| `slots`, `accepts` | races | **data** | equipment layout |
| `sprite` | tiles, objects, races | **art** | a path; nothing reads it but the renderer |
| `art` (multi-line grid), `palette` | objects, races | **art** | render-only, see below |
| `characterColor`, `backgroundColor`, `nameTagColor` | tiles, objects | **art** | render-only |
| font, chat colours | `Defaults.h`, `render.fontPath` | **art** | legibility and mood, not mechanics |

### 2.1 The exception: the single ASCII glyph is not art

`character` — the one-character field — **must stay in the data pack**, and this is the finding worth
reading carefully.

It is not a cosmetic fallback. It is **the encoding the agents perceive the world through.**
`GetSurroundingsRadar` (`include/AgentActions.h:524-575`) builds a map of glyph -> name and prints a
character grid plus its legend into the prompt:

```cpp
tileChar = tile->ascii->ch;
// Terrain is what placement validation checks against ("tilled soil", "path"),
// so the grid has to name it.
legend[tileChar] = tile->name;
...
result += std::string(1, pair.first) + " = " + pair.second + "\n";
```

So the glyph is a **symbol table key** with a correctness invariant. `extract_sheet.py` states it
outright: *"Characters are deliberately unique because the placement radar prints a glyph grid and
a collision makes that grid unreadable."*

Two concrete consequences:

- **An art pack that changed `character` would change agent behaviour.** Not rendering — reasoning.
  Two art packs over one data pack would produce different agent transcripts from the same seed.
  That contradicts the split's whole point.
- **An art pack that collided two glyphs would corrupt the placement radar's grid** into something
  ambiguous, which is a gameplay bug caused by a reskin.

The multi-line `art` grid and `palette` are different: nothing reads them for prompts. Confirmed —
their only consumers are the world renderer (`GameECS.cpp:405`), the storage window
(`StorageWindow.cpp:166`) and the editor's template palette (`DebugWindows.cpp:1140`), all through
`AsciiArtDraw.hpp`. **`art`/`palette` are art; `character` is data.**

The rule that follows: **the art pack owns how a thing is drawn; the data pack owns how a thing is
identified.** They are different fields, and the split must respect that they are different *kinds*
of field, not just different files.

### 2.2 Resolution is genuinely free

`DrawSpriteFitted` (`include/SpriteDraw.hpp:55`) fits any texture into the entity's `DrawAscii` box via
`SpriteFitRect`, and `TileDef` states the logical size in `hitboxSize` which `Map` scales to that
map's tile size.

So an art pack can ship 16x16, 32x32 or 48x48 sprites without touching a single gameplay value, and a
map with different `tileWidth`/`tileHeight` still works. **Tile size is not a coupling point**, which
is better than this document originally assumed.

---

## 3. The consequence nobody wants to hear: art packs are systematically incomplete

If a data pack defines 200 materials and an art pack covers 60 of them, then every combination of
that data pack with that art pack has **140 missing sprites — by construction, not by oversight.**

This is not a defect to be fixed by authoring more art. It is arithmetic: `N` data packs x `M` art
packs is `N*M` pairings, and no authoring effort keeps `M` current with every `N`.

What saves it is that **the engine already treats a missing sprite as normal**, and that is the
single most important existing property this whole design leans on:

- `SpritePaths.cpp` resolves explicit path -> convention path -> **nothing**, and the entity falls
  back to its ASCII look with a warning: *"sprite ... does not exist; using the ASCII look instead."*
- `Sprite` only exists as a component when the image loaded (`Components.h:119-135`), so "no sprite"
  is not a special case in the renderer.
- The `character` glyph is in the **data** pack (§2.1), so a mixed pack always has a legible
  fallback and a working placement radar regardless of which art pack is loaded.

So recombination is **safe but visibly partial** — half sprites, half glyphs. Which is exactly the
problem the generation pipeline solves, and it reframes what the pipeline is for:

> **The pipeline is not a convenience. It is what makes independently swappable art packs viable at
> all.** An art pack without it is frozen at whatever coverage it shipped with, and every new data
> pack degrades it further.

### 3.1 The LoRA is the art pack's style model

This is the corrected framing of the original idea. A LoRA is not trained "on a data pack", and it is
not a global model — it is **the art pack's own style model**, and its job is to extend that art
pack's coverage over *any* data pack's vocabulary.

- Owned by, versioned with, and loaded from the art pack (§8.3).
- Trained on the **art pack's** source art (§8.1) — for a Pixel Crawler art pack, that is the 26
  atlas-grade pages in `data/tilesets/`, not the data pack's 32x32 crops.
- Applied to close gaps between that art pack and whichever data pack is loaded.

A LoRA does not create a style, it **amplifies** the consistency of its training set (§8). An
art pack that is already incoherent produces a confident, reproducible incoherence.

### 3.2 The rule that keeps the split honest

**An art pack must never gate the data pack's vocabulary.**

The designer agent's `%MATERIALS%` block is built from the **data** pack's **ground-tile catalog and
object templates**. If it were instead built from the *intersection* of the data catalog and the art
pack's coverage, then swapping from a dark fantasy art pack to a high fantasy one would change **what
can be built** — contradicting "data packs contain critical gameplay information" and silently
coupling the two axes.

So: the designer designs against the data pack. Missing art is a rendering concern that the pipeline
addresses, never a design constraint. If an art pack wants to *bias* the designer's taste, that is a
prompt hint at most (§11), and it must remain advisory rather than a gate.

---

## 4. Creating content: two writers, the overlay, and the prerequisites

This section is the one that changed most when agent-created content was put in scope. The engine must
support **two different writers** with **different powers**, and it must do so without letting either
one corrupt the vocabulary the other depends on.

### 4.1 The hazard that already exists

`gen_tile.py --emit-tile-json` writes **`data/tiles/<name>.json`**, and `import_church.py` writes both
`data/tiles/` and `data/objects/`. So the art tooling today writes into the *data* catalog — it is
already the first writer, by accident, without the rules.

Left alone under the split, that causes two failures:

1. **It violates §3.2** — art generation silently adds to the vocabulary the designer designs against.
2. **It renumbers tiles.** Per `todo`'s "Map/tile id bug", a map stores terrain as `"positions"` whose
   values are integer **indices**, and `Map::uniqueTiles` order *is* tile identity. Injecting
   definitions changes what an existing index means.

So the boundary has to be explicit, and it has to be enforced in code rather than by convention.

### 4.2 Writer one: the bulk pipeline can write art, and only art

| the bulk pipeline may write | the bulk pipeline may never write |
|---|---|
| `sprites/<name>.png` in the generated layer | `tiles/*.json` |
| a `palette` entry | `objects/*.json` `components` |
| a record of which data-pack name the sprite answers to | any definition it did not create deliberately |

A generated PNG is cheap and reversible. A generated *mechanic* is neither. So the bulk pipeline never
touches vocabulary, and `--emit-tile-json` should be removed rather than merely discouraged.

### 4.3 Writer two: a designer agent may create vocabulary, gated

The bedroom case is real and in scope: a designer told to build a bedroom invents a bed. That is a
**deliberate, single-item, agent-driven** act — different in kind from a batch of 40 sprites, and it
gets different rules.

**What it may create:** a new object or **ground tile**, in the generated overlay (§1.2), never in the
hand-authored catalog.

**What it may attach to an object** — an allowlist, not a denylist. Ten components are reachable from
JSON via `RegisterComponent<T>` in `src/ObjectFactory.cpp:21-132`, and every one of them is in use
somewhere in `data/objects/`. Exactly two are permitted:

| component | what it does | created objects |
|---|---|---|
| `Interactable` | examination and social interaction | ✅ **permitted** |
| `Obstacle` | blocks movement; affects pathing, not resources — furniture that can be walked through reads as broken | ✅ **permitted** |
| `Portable` | carried in an inventory | ❌ economic |
| `Storage` | holds items, with capacity | ❌ economic |
| `Harvestable` | yields loot | ❌ economic |
| `Workstation` | provides crafting recipes | ❌ economic |
| `Evolvable` | grows into a `nextStageTemplate` | ❌ economic |
| `Placeable` | an item that becomes an object on a surface — this is the planting mechanic | ❌ economic |
| `Requires` | gates use on equipped item tags | ❌ access control |
| `Trains` | grants skill XP | ❌ **progression** — the sharpest of the lot |

That is the line "decorative only, no balance effect" drawn precisely: a created bed may be examined
and may block movement, and it can never hold, harvest, craft, grow, be carried, gate access, or award
XP.

> A stale note worth correcting while here: `airogue-direction` describes this set as eight components
> including `Seed`. There is no `struct Seed` in `Components.h` and no `"Seed"` anywhere in `data/` —
> it was replaced by `Placeable` (`data/objects/wheat_seed.json` is `Portable` + `Placeable`, with
> `placement.becomes: "wheat_seed_planted"` and `surface: ["tilled soil"]`). The real set is the ten
> above, and `Trains` and `Requires` are easy to miss when reasoning about balance because they are not
> resource components.

**A ground tile needs no allowlist at all.** A `TileDef` carries `name`, `blocksTile`, `hitboxSize`,
`sprite` and its glyph plus foreground/background colours (`Components.h:181-201`) — there is no
economy component a tile could smuggle in. So a created **ground tile** is **balance-safe by
construction**, and the real risk with tiles is *identity*, which is why §4.5 is a prerequisite
rather than a nicety. This argument does **not** extend to walls, doors or furniture: those are
objects, go through the object allowlist above, and must never be created as tiles to dodge it.

**Refusals are cheap and must be explained.** The designer already has a bounded re-prompt loop
(`designer.maxDesignAttempts`, `include/Settings.h`), and `DESIGNER.md` already specifies that a
refused design is re-sent whole with the engine's reason. A created object carrying `Storage` should
come back as a refusal naming the component, in exactly that channel — not as a silent downgrade,
which would teach the agent nothing.

### 4.4 Glyph allocation is part of creation

Per §2.1 the single `character` is the LLM's symbol table and must be unique across the world. So
creating an object is not only "generate a picture" — it must **allocate an unused glyph** from the
data pack's existing set, and the allocator has to treat a collision as a hard failure.

The bed cannot be `#`, `w`, `.` or any of the 20 catalog glyphs already in use. Practically: pick from
a reserved decorative range, check against the union of catalog glyphs *and* the generated overlay's,
and record the allocation so two created objects cannot race to the same character. `extract_sheet.py`
already states the consequence of getting this wrong: *"a collision makes that grid unreadable."*

### 4.5 Prerequisite: give tiles a stable identity

Agent-created tiles are only safe once tile identity stops being a fragile array index. This is
`todo`'s "Map/tile id bug". The fix below is the one specified here; it was implemented on
2026-10-15 and `src/Map.cpp` now carries the reasoning inline.

**The old contract.** `Map::GetTileId(const Tile*)` returns the tile's index in `uniqueTiles`, and its
own comment said so: *"This is the id the map file's `positions` array stores."* Load built
`uniqueTiles` one entry per `tileInfo` entry in file order, with `allTileInfo` kept parallel.

**The defect.** `BuildMapJson` produced the two arrays by **two different loops over two different
containers**: `positions` by searching `uniqueTiles` for each cell's pointer (correct), and `tileInfo`
from `allTileInfo` **filtered by `tileOverridden`**. A catalog tile the map never overrode was
omitted, so the written `tileInfo` came out *shorter* than `uniqueTiles` while the indices in
`positions` still addressed the longer array. The **next** load renumbered the map — every wall cell
became floor. (The walls in that old data were themselves misclassified tiles; the identity bug still
applies to the ground beneath them.)

Reading `positions` was also **unchecked**: `uniqueTiles[pos]` on a shortened array is an
out-of-bounds `std::vector::operator[]`, so the same defect was undefined behaviour and an ASan
report waiting to happen, not only silent corruption.

**The fix as implemented — one container, one loop, both views derived from it.**

- The map file's terrain is declared by **`tilePalette`**, one entry per `uniqueTiles` slot, in the
  order `positions` indexes. An entry is **either** a bare catalog name (`"grass"`) **or** an object
  carrying a name plus the fields this map overrides (`{ "name": "path", "character": ":" }`).
- **The palette and `positions` are both written from `uniqueTiles`**, so they cannot disagree. That
  is the root cause fix; correcting the ordering alone would have left the bug reachable.
- **`tileInfo` is gone from new files** — `BuildMapJson` erases it. It was the older positional
  spelling of the same list, and a file carrying both is a file with two sources of truth.
- An entry is written as a bare name when the tile is unmodified catalog and as the authored override
  object otherwise, so a save still cannot re-duplicate the shared vocabulary into the map file, and
  an override keeps exactly the keys it was authored with.
- On load: cell value -> palette index -> (bare name -> `TileRegistry::Get(name)`, or the override
  object resolved over its catalog definition). The resolve logic is one lambda used by both the
  palette path and the legacy path.
- **Migration:** a map with no `tilePalette` is read from its positional `tileInfo` exactly as before,
  and is written back in the new shape on its next save. Every map in `maps/` loads unchanged and
  needs no hand-editing. `MapReloader::IsJsonMapFile` accepts either key, so old maps still list.
- **Reading is now bounds-checked.** An out-of-range index warns once and falls back to the first
  tile; an empty palette stops the layout walk rather than building null cells. A file that previously
  read out of bounds is now a warning.

Two things this buys beyond the bug fix:

- **An agent adding a tile appends a palette entry and cannot renumber anything.** Every existing cell
  keeps its meaning, whether or not the generated tile is present on a later load.
- **A missing tile becomes a named, reportable gap** rather than a silently different floor. The
  loader knows *which* name it wanted, so a generated tile whose definition is gone is a gap-log entry
  (§6) instead of corruption.

One deliberate non-goal: **duplicate names remain distinct tiles.** Two palette entries may both be
"path" and are still two `uniqueTiles` slots, because identity is the palette index and both arrays
now come from one container. `todo` raised this as an ambiguity; with the arrays aligned it is a
non-issue, and keying overrides by name (an earlier idea here) would have *introduced* a bug by
collapsing two genuinely different overrides of the same material.

Agent-created tiles also already have their insertion point: `Map::CreateTile(name)` exists for exactly
this case — *"when something needs a tile the map did not already have"* — and returns `nullptr` for a
name not in the catalog, with the comment that inventing a tile with no appearance *"would paint an
invisible wall."* Read together with §4.3, that is the whole contract: the name must be in a catalog
(the generated overlay's counts), and the appearance must exist or be scheduled.

### 4.6 The creation path, end to end

```
designer decides it needs a "bed"
        |
        v
[ ALLOWLIST ]  components in {Interactable, Obstacle}?   -- no --> refuse, reason fed back
        |                                                            (designer re-prompts)
        yes
        v
[ GLYPH ]      allocate an unused character (§4.4)        -- collision --> hard fail
        |
        v
[ DEFINITION ] write into the generated overlay, never the hand-authored catalog
        |
        v
[ ART ]        generate a sprite in the art pack's style -> generated layer
        |         (pack-level, persists, art pack still wins if it has one)
        v
[ BALANCE ]    (not built yet -- §4.7) assess the addition, then accept or refuse
        |
        v
the bed exists, is visible, is examinable and blocks movement -- and does nothing else
```

The order matters: the allowlist and the glyph check come **before** art generation, because both are
cheap and deterministic, while generating art for something that will be refused is the expensive
mistake.

### 4.7 The Game Balance agent (planned, not now)

The intended second line of defence once the pipeline is real. It receives a proposed new object —
definition, components, description, and optionally art — and returns **accept / reject / amend**.

One design constraint is worth recording now, because it shapes the brief. The project's hybrid-economy
decision means **prices and deals emerge from agent dialogue**, so there is no price table for a
balance agent to consult. It cannot reason about numbers that do not exist. What it *can* check is
structural:

- does the proposal smuggle in a forbidden component (§4.3), or a **second** route to one
- does its description **promise a mechanic that does not exist** — the hallucination boundary, which
  becomes load-bearing the moment the engine builds what an agent names
- does an allowed component have a **disallowed effect** — an `Obstacle` that seals the only doorway
  is decorative in name and a trap in practice
- does it **duplicate or undercut** an existing object by name or role

Start it advisory — log a verdict and let the creation through — and promote it to blocking once its
judgements have been checked against a few dozen real creations. A gate nobody has calibrated is worse
than no gate.

---

## 5. What already exists

**An earlier version of this section listed five scripts. There are eight, plus a validator, and the
pipelines they encode are more mature than that list implied** — each carries a *measured* reason for
the thing it does, which is why the correct move here is composition rather than replacement.
Verified 2026-10-15 by reading them.

### Source (generate)

| file | what it does |
|---|---|
| `sprite_preview/gen_object_sheet.py` | prompts `google/gemini-2.5-flash-image` for **one** sheet of many objects, with `data/sprites/farmer.png` as a **style reference image**. Reads its key from `model.json`; the image model is a hardcoded constant, not `model.json`'s `model` |
| `sprite_preview/gen_tile_sheet.py` | the tile equivalent, with four constraints each tied to a named failure: **seamless** (else a visible grid across a floor), **flat top-down** (else a stack of pictures), **no props** (else the tree repeats on every tile), **clear gutter** (so full-bleed cells can be separated at all) |
| `sprite_preview/gen_lineup.py`, `gen_sprite.py` | the agent sprites |
| `tools/gen_tile.py` | SD 1.5 + a pixel-art LoRA via diffusers, 512px, quantise-then-`NEAREST`, contact sheet at true game scale |
| `tools/workflows/pixelart_sd15.json` | the same graph for ComfyUI |

### Cut (slice a sheet into 32px sprites)

Three different matte strategies, and which is right depends on the source. See §7.2.

| file | strategy |
|---|---|
| `sprite_preview/slice_object_sheet.py` | direct colour match + despill along the silhouette only |
| `sprite_preview/slice_tile_sheet.py` | crop to blob then **inset**; no keying needed inside a full-bleed tile |
| `sprite_preview/make_sheet.py` | corner flood fill + connected-component slicing clustered by x-range |
| `tools/sprites/extract_sheet.py` | the original hand-mapped slicer that produced the church set. **Superseded** for objects by `slice_object_sheet.py` |

### Catalog and palette

| file | what it does |
|---|---|
| `tools/sprites/import_church.py` | writes object/tile templates with ASCII glyph fallbacks and palette entries. Writes *both halves* of the record, so it is the clearest example of the §1 conflation |
| **`tools/palette.py`** | **new**: derives one shared palette from a sprite set, and snaps sprites to it. See §7.3 |

### Detect and verify — do not rebuild these either

| where | what it already does |
|---|---|
| `CheckVocabulary` (`src/BlueprintRegistry.cpp:455`) | already finds vocabulary misses and **already deduplicates them** into `{kind, material, partIds}` groups, keyed by `kind\|material` so a tile and an object of the same name stay separate problems. It runs at load and its output currently becomes prose warnings — §6 needs the records, not a new detector |
| `gen_tile.py --dry-run` + `build_contact_sheet` | a contact sheet at true game scale, and a dry run that needs no model |
| `slice_tile_sheet.py`'s 3x3 tiling proof | every tile printed as a 3x3 repeat, because a seam or a frame is invisible on one tile and obvious on a floor |
| `make_sheet.py`'s fringe metric | the share of semi-transparent pixels, as an objective number for the chroma-key rim |

Measured facts that govern the design and are not re-litigated here:

- **1 usable tile in 5–10**, at **10–30 s each** on the 8 GB RTX 3050. Fine for design time,
  unusable for anything live (§10.1).
- **Quantise at 512, then `NEAREST` downscale.** Quantising *after* downscaling produces blur.
- One image request costs the same regardless of subject count (**1,290 image tokens per 1024px
  output** — measured), so **batching many assets into one sheet is a ~19x saving** and, more
  importantly, makes the batch internally consistent by construction.

**Neither slicer trusts the model's cell order.** Both say so explicitly: the model returned 20 cells
when asked for 19 and for 11, duplicating and shifting entries, so the cell-to-name mapping is
verified by eye against a labelled index sheet (`--number`) rather than assumed from prompt order.
Any pipeline built on these must keep that step, or replace it with something equally explicit.

---

## 6. The gap: three kinds of unresolved reference

An earlier draft of this document claimed "agents never need a new asset in the abstract", and split
every unresolved reference into a coverage miss or a mistake. **That was wrong**, and the bedroom case
is the counter-example: a designer tasked with a bedroom legitimately needs a bed that does not exist
yet. With creation in scope (§4.3) there are three cases, with three different responses.

A blueprint references:

- **ground materials** by string — `"op": { "paint": "stone floor" }`, resolved against the data
  pack's **ground-tile** catalog (`TileRegistry::Get` returns `nullptr` for an unknown name by
  contract). A wall or door is not a ground material: it is an object template, below.
- **objects** by string — `"op": { "place": "altar" }`, resolved against the data pack's
  `objects/*.json`. Walls and doors belong here, once their templates exist.

and separately, the **art pack** is asked for an appearance for each. The three outcomes:

| case | means | response |
|---|---|---|
| **coverage miss** | the definition exists; the art pack has no sprite for it | generate art, in the art pack's style (§7) |
| **creation** | no definition exists, and the designer legitimately needs one | the gated path in §4.3 — deliberate, single-item, allowlisted |
| **invalid reference** | the agent named something that is neither, and should not be built | refuse, and feed the reason back |

Conflating any two of them is the mistake §4 exists to prevent:

- Reading **creation as a coverage miss** would let the bulk pipeline invent vocabulary — §4.2 forbids
  exactly that, for the tile-renumbering reason in §4.1.
- Reading **creation as invalid** blocks the bedroom case outright.
- Reading **invalid as creation** is the hallucination risk, and it is the most dangerous of the three:
  before this feature, a hallucinated name safely failed to resolve; once the engine will *build* what
  an agent names, the guard has to be structural (§4.3's allowlist, §4.7's balance agent).

**Telling coverage from creation is mechanical.** Does `TileRegistry::Get(name)` / the object template
map resolve? `nullptr` means creation-or-invalid. Separating those two is a judgement about the
*designer's* intent against what the engine permits — which is precisely why §4.3 is an allowlist plus
an explained refusal rather than a free-for-all, and why the balance agent (§4.7) is the real answer at
scale.

**The gap log records coverage misses only.** They are a batch worklist to drain. Creation events are a
separate log — what an agent invented, when, and later what the balance agent judged — because they are
single deliberate acts with a review trail. Merging them would make a 40-sprite batch
indistinguishable from 40 agent inventions.

Detecting coverage misses needs no new plumbing: `ResourceManager::IsKnownMissing` is public
(`include/ResourceManager.h`), and `SpritePaths.cpp` already distinguishes *"authored but absent"* from
the convention probe that was always expected to fail.

Output: an append-only `gap.log` plus a deduplicated `gap.json`, **in the generated layer, keyed by
the art pack and data pack pair** (§9.3) — because coverage is a property of that pair, not of either
alone.

---

## 7. The pipeline

Four stages, each with a working ancestor. The change is that they become one command over an art
pack instead of four scripts run by hand.

```
  gap.json                 one worklist, from §6
      |
      v
  [ SOURCE ]  ------------> sheet PNG (1024px) + raw 512px renders
      |         LoRA or API, batched, style-conditioned
      v
  [ CUT ]     ------------> 32x32 RGBA PNGs
      |         flood-fill bg, crop-to-content, NEAREST downscale
      v
  [ PALETTE ] ------------> 32x32 PNGs snapped to the art pack palette
      |         deterministic, no ML, cheap
      v
  [ CATALOG ] ------------> art pack only: sprites/, palette, pack.json
                NEVER data definitions -- see §4
```

### 7.1 SOURCE

Two interchangeable backends behind one interface. This is the seam that keeps the LoRA decision
reversible:

| backend | how | when |
|---|---|---|
| **sheet API** | `gen_object_sheet.py` as it stands: one 1024px sheet, art pack sprites passed as reference images | default; cheapest, fastest, no training, consistent by construction |
| **local LoRA** | `gen_tile.py`'s diffusers path with the art pack's adapter loaded | when offline/deterministic reproduction matters, or the art pack must not depend on an API |

Both take the same inputs — the gap list, the art pack's style block — and both emit a sheet plus raw
renders. Raw renders are kept, always (§11.2).

**Batch, never one-at-a-time.** One request per asset costs the same as one request for nineteen and
loses the internal consistency that comes from a single generation. The gap log is accumulated
precisely so it can be drained in batches.

### 7.2 CUT — three strategies, not one

**This section previously said "edge-seeded flood fill, not colour replacement" as a universal rule.
That was wrong, and it was wrong in the direction of confidently stating the opposite of what the
newer tooling does.** There is no single matte strategy; which is right depends on the source, and
each of the three carries a measurement:

| source | strategy | why, measured |
|---|---|---|
| **silhouetted objects** | **direct colour match**, then despill along the silhouette only (`slice_object_sheet.py`) | a flood fill chains through the anti-aliased rim and eats into the art once the fuzz is high enough to clear it — *"measured on the agents: 15% leaves pockets, 20% reaches skin"*. A direct match cannot chain. Despill only along the silhouette because the same rule applied everywhere erased real artwork (the trader's tunic) |
| **full-bleed tiles** | crop to blob, then **inset** a few pixels (`slice_tile_sheet.py`) | there is nothing to key *inside* a tile — the magenta is only the gutter between cells. The inset removes the anti-aliased rim and any frame the model drew. A frame is fatal for a tile in a way it never is for an object: it repeats on every cell and reads as a grid across the whole floor |
| **agent lineups** | corner flood fill + connected components clustered by x-range (`make_sheet.py`) | clustering rather than "the four biggest blobs" because a held prop can detach into its own blob and would otherwise be sliced off as a separate sprite |

Shared and still load-bearing in all three: **crop to content before downscaling**, or a tall thin
sprite like the cross shrinks to fit padding and loses its pixels. And **`NEAREST`** wherever the
intent is hard pixel edges.

`tools/sprites/extract_sheet.py` is the flood-fill one, and it is the source of the correction above:
it produced the church set, and `slice_object_sheet.py` was written afterwards precisely because that
approach failed on objects. Treat `extract_sheet.py` as superseded for objects.

Its other weakness is a hand-maintained `NAMES` dict for the cell-to-name mapping — see §11.1. The
newer slicers replaced that with an explicit index-and-eyeball step, which is more honest about the
model's unreliable cell ordering (§5) but is still manual.

### 7.3 PALETTE — implemented, and the numbers changed the guidance

**Status: built.** `tools/palette.py`, with three subcommands:

```
derive  --sprites <dir> [--colors N] [--out stem]   # -> <stem>.json + <stem>.png
lock    --palette <json> --sprites <in> --out <dir> # snap, never in place
report  --palette <json> --sprites <dir>            # measure without writing
```

Snapping is nearest-colour in **OKLab**, memoised by source colour, and it **binarises alpha** at the
same time (the same anti-aliased rim `make_sheet.py` measures as a fringe metric). `tools/gen_tile.py`
reuses `palette.py`'s conversion and search rather than reimplementing them, and both slicers now pass
ImageMagick `-remap palette.png` instead of `-colors 16` — so all three backends snap by one rule.

#### What measuring the existing art showed

`data/sprites/`: **17,050 distinct colours across 34,756 opaque pixels**, and **46 of 57 sprites use
more than 16 distinct colours** (top offenders ~700–780, i.e. nearly every pixel a unique colour).

| palette size | covers this art | verdict against the original |
|---|---|---|
| 32 | **0.0%** | destroys texture — grass flattens to a single green, flagstones and leaves merge |
| 64 | 9.4% | still visibly flat |
| 128 | 17.5% | reasonable compromise |
| 256 | 23.9% | close to the original |

So the numbers that decide the palette size are the *art's own*, and this pack is **smooth-shaded, not
limited-palette**. The earlier guidance here ("32–48 colours is right for this art") was wrong for it.

Two consequences, and the second is the important one:

1. **Size the palette to the pack, not to pixel-art orthodoxy.** A limited-palette pack wants 16–32; a
   smooth-shaded one needs 128–256 or the lock is a reskin. `derive` prints the coverage number and
   warns when the palette is small relative to the art, so the choice is informed rather than guessed.
2. **Palette lock is aimed at GENERATED art, never at the pack.** Median-cut picks *representative*
   colours, so at 32 colours a 17,050-colour pack has 0% of its pixels in the palette — snapping it
   recolours essentially every pixel. Applied to the pack that is destruction; applied to a generated
   sprite it is exactly the intended harmonisation, and it costs the pack nothing. `lock` therefore
   refuses an `--out` equal to its input, which is the tool enforcing §1.2 rather than trusting a
   convention.

`DEFAULT_ALPHA_THRESHOLD` is 128: a pixel is opaque or it is not. Measured on the current art, this
changes nothing — all 57 sprites already have binary alpha — but the generators produce a soft rim, so
it is the stage that removes one.

**A bug worth recording, because it was in this document's own metric.** The first version of the
recolour share divided by *all* pixels rather than opaque ones, and read 59.5% for a palette that in
fact contained none of the art's colours. The denominator mattered more than the numerator: a 32x32
sprite fills about 60% of its cell. Any per-sprite colour statistic must divide by opaque pixels.

The payoff is unchanged: a weak LoRA with palette lock still reads as belonging to the set, and a
strong LoRA without it can still look imported. If only one of the two is built, build this one.

### 7.4 CATALOG — and its hard boundary

`import_church.py` generalised, with §4.2's boundary enforced in code. For each gap the pipeline may
write **only** into the generated layer:

- `sprites/<name>.png`
- a `palette` entry
- an entry recording which data-pack name the sprite answers to

It may **not** write `components`, `blocksTile`, `description`, `slots` or `character`, and it may not
write into the art pack's own `sprites/` — doing so would put generated output inside the set the LoRA
and the palette are derived from, which is what §1.2 forbids. Nor may it write definitions: that is
§4.3's separate, gated path, not this one. A generator that infers `Interactable` vs `Obstacle` from a
picture is guessing at mechanics, and generated art is cheap while generated mechanics are not.

**Nothing enters the generated layer unreviewed.** Unreviewed art is drawn to the player and to the
designer's editor immediately, and a bad sprite in the layer is a permanent invitation to the next
generation run to imitate it — for as long as the layer persists (§1.2).

---

## 8. Training the art pack's LoRA

### 8.1 Dataset

Prepared from the **art pack's own source art**, not from the data pack's `data/sprites/`. For a Pixel
Crawler art pack, measured from `data/tilesets/Pixel Crawler - Free Pack/`:

| what | count | note |
|---|---|---|
| PNGs total | 289 | |
| atlas-grade (both dims >= 400px) | **26** | the aggregated sheets and tileset pages |
| at least one dim >= 256px | **199** | buckets down cleanly for SD 1.5 |
| small images (<= 128px wide) | 74 | animation frames; weak as style |

- **include** the 26 atlas-grade pages whole, and the other images measuring at least 256px on one
  axis (199 files clear that threshold)
- **exclude** the animation strips — 74 files are 128px wide or narrower, most only 32–48px tall.
  They teach motion frames, not style.
- **do not** include `data/sprites/`'s 32x32 crops: derived, mixed-origin, and below the resolution
  the base model works at

The decisive fact is the **atlas pages**. `Furniture.png` (800x864), `Wall_Tiles.png` (400x400),
`Floors_Tiles.png` (400x416), `Resources.png` (400x400), the five `Structures/Buildings/` pages,
`Icons/Resources.png` — large, coherent, multi-subject pages drawn in one hand. Exactly what a style
LoRA wants, and no upscaling needed.

Rough target for a style adapter is 30–100 images. The 26 large pages plus a selection of the medium
ones lands in range. Augmenting a set this small is *not* recommended: flips break asymmetric objects,
and crops of crops teach nothing.

### 8.2 Captioning: one trigger, no content

For a **style** adapter the captions should be maximally boring. Tag every image with one unique
trigger token and nothing else:

```
pxcrawler_pixel_art
```

A single token shared by every image leaves the model no content words to bind appearance to, so the
trigger collects *style* — palette, outline weight, shading idiom, dithering. Captioning the content
("a stone wall, a wooden bench") instead teaches the trigger to mean "many unrelated things" and
dilutes exactly what is wanted. Content comes from the prompt at generation time; style comes from the
trigger.

### 8.3 Configuration

Starting values, in the shape `kohya_ss/sd-scripts` `train_network.py` expects. **Community defaults
for pixel-art style adapters, not measured on this machine** — a first run to be tuned, judged by
§8.4, not by loss.

```
--network_module=networks.lora
--network_dim=32 --network_alpha=16     # dim 16/alpha 8 if the style over-fits
--network_train_unet_only               # style lives in the UNet; leave the text encoder alone
--learning_rate=1e-4
--optimizer_type=AdamW8bit
--lr_scheduler=cosine_with_restarts --lr_scheduler_num_cycles=2
--resolution=512,512
--enable_bucket --min_bucket_reso=256 --max_bucket_reso=768
--batch_size=1 --gradient_accumulation_steps=4
--mixed_precision=fp16 --cache_latents
--max_train_epochs=10 --save_every_n_epochs=2
```

The 8 GB card is the binding constraint; `--cache_latents`, `AdamW8bit` and batch 1 with accumulation
are what make it fit. Aspect-ratio bucketing is what lets the 400x416 and 896x640 pages train without
being squashed into squares.

The diffusers equivalent is `train_dreambooth_lora_sd15_advanced.py`. The FHS environment in
`tools/README.md` §1 is reusable for training as-is — the same `buildFHSEnv` shell, the same
`LD_LIBRARY_PATH` line. Training does not need the game's `nix-shell`.

**The adapter lives in the art pack, never in a global models directory**, because it *is* the art
pack's style:

```json
{
  "id": "pixel_crawler",
  "name": "Pixel Crawler",
  "lora": "lora/pxcrawler.safetensors",
  "trigger": "pxcrawler_pixel_art",
  "loraScale": 0.8,
  "negative": "blurry, smooth gradient, antialiasing, 3d render, photograph",
  "references": ["sprites/farmer.png"]
}
```

`loraScale` around 0.8 is the starting point from `gen_tile.py`'s tuning table; too high destroys
texture. `references` feeds the sheet-API backend, so both backends read the same block and swapping
between them is a config change rather than a code change.

### 8.4 Judging the adapter, in this order

1. **Contact sheet at true game scale**, as `gen_tile.py` already produces. Never judge at 512px.
2. **A 10x10 painted patch** in the map editor. A single tile cannot show a seam; a patch will.
3. **Side by side with the art pack's real sprites in the same field, at the same zoom.**
4. Only then: does it look like the same artist made it?

Good enough means a generated `altar` can sit next to the art pack's existing `altar.png` and the
difference is not the first thing you see. If it never gets there, the sheet-API backend already
passes that bar and is where the time should have gone.

**Judge the art pack against a real data pack, not in isolation.** The failure mode this catches is
one the channel split newly creates: an adapter that looks fine on its own pages but produces sprites
that clash with the *data pack's* glyph fallbacks around them. A field that is half sprites and half
ASCII is the normal recombined state (§3), so that mix is what must look deliberate.

---

## 9. The two roots on disk

### 9.1 It is already nearly free

`GameSettings::ContentSection` (`include/Settings.h`) is already a set of path strings read from
`settings.json` at launch. The header says outright that the file holds *"where the content registries
live"*:

```cpp
std::string objects    = "data/objects";
std::string recipes    = "data/recipes";
std::string stats      = "data/stats";
std::string skills     = "data/skills";
std::string items      = "data/items";
std::string races      = "data/races";
std::string tiles      = "data/tiles";
std::string blueprints = "data/blueprints";
```

and these are **wired, not merely declared**: `src/GameECS.cpp:1920-1968` passes each into its
registry, and `src/Settings.cpp:265-273` populates them from the file. The prompts are externalised on
the same principle — `designer.promptFile` -> `data/blueprints/DESIGNER.md` (`src/Game.cpp:638`), and
`ai.npcPromptFile` for the NPC prompt (`src/Game.cpp:197`). `src/main.cpp` already parses
`--settings <file.json>`, with `settings.local.json` merged on top by an RFC 7396 patch.

So **a data pack is already almost expressible as a settings file today**:

| thing | today | status |
|---|---|---|
| objects, recipes, stats, skills, items, races, tiles, blueprints | `settings.json` `content.*` -> `src/GameECS.cpp:1920-1968` | already wired ✅ |
| designer prompt | `designer.promptFile` -> `data/blueprints/DESIGNER.md` | already data ✅ |
| NPC prompt | `ai.npcPromptFile` (empty = compiled-in) | already externalisable ✅ |
| map | `map.file`, plus the `argv[1]` override | already per-launch ✅ |
| **sprite convention path** | was `inline constexpr const char *SPRITE_DIRECTORY`; now `GameSettings::ArtSection` + `SpriteCandidatePaths` | **done** ✅ (§9.2) |

### 9.2 The art root — implemented 2026-10-15

Built as described here, and it was as small as this section predicted: `SPRITE_DIRECTORY` turned out
to have exactly **one** use site (`src/SpritePaths.cpp`), so the constexpr survives only as the
no-art-pack fallback.

What was added:

| where | what |
|---|---|
| `GameSettings::ArtSection` (`include/Settings.h`) | `art.pack` and `art.generated`, parsed at `src/Settings.cpp` and documented in `SETTINGS.md` under a new `## art` heading |
| `SpriteCandidatePaths(conventionId)` (`include/SpritePaths.hpp`) | the layering, in one place, **as paths** |
| `ResolveSpriteTexture` | iterates the candidates instead of building one string |
| `src/main.cpp` | `--art-pack <dir>`, same shape as the existing `--settings` |

**Why the candidates are exposed as paths rather than textures.** §6's gap report has to ask "would
this resolve?" without loading an image, and it must ask in the same order the renderer does or its
report is a lie. Fusing path resolution to texture loading — which is what `TryPath` does internally —
would have forced the report to reimplement the rules and drift from them.

The order, and an empty `art.pack` meaning "exactly as before":

```
explicit "sprite" path        (warns when missing -- somebody meant it)
  -> <pack>/sprites/<id>.png  (the art pack: wins)
  -> <generated>/<id>.png     (additive: fills only what the pack lacks)
  -> data/sprites/<id>.png    (only when art.pack is empty -- the pre-split behaviour)
```

`art.generated` defaults to `<pack's grandparent>/generated/<pack dir name>/sprites`, so
`packs/art/pixel_crawler` gets `packs/generated/pixel_crawler/sprites` with no configuration. The
roots are read once and cached: the editor's template palette asks for a candidate list per template
per frame, which is the same reason `ResourceManager` memoises absent paths.

**One flag, `--art-pack`, and it must be set before the first sprite resolves.** That is not a
limitation worth fixing — settings are read once at startup — but it is worth knowing.

That the whole thing was one string is the point this section made before it was written, and it held
up: the registries were already built for this and only the sprite half was missed, which is exactly
the half an art pack needs.

### 9.3 Proposed layout

```
packs/
  data/
    village/                 # the current data/ minus sprites
      settings.json          # content.* pointed at this directory
      tiles/  objects/  items/  recipes/  stats/  skills/  races/  blueprints/
      prompts/
        designer.md          # from data/blueprints/DESIGNER.md
        npc.md               # from DEFAULT_NPC_PROMPT
  art/
    pixel_crawler/           # PRISTINE: nothing here is ever written by the pipeline
      pack.json              # style block (§8.3)
      sprites/               # 57 PNGs, 32x32 -- the only thing a LoRA or palette is derived from
      palette.json           # derived once: tools/palette.py derive. Reviewable, versioned
      palette.png            # the same list as a swatch sheet -- what ImageMagick -remap takes
      lora/
        pxcrawler.safetensors
        train/
          dataset/
          config.toml
      fonts/                 # optional: render.fontPath, chat colours
    dark_fantasy/            # may cover only part of the village data pack -- normal (§3)
      pack.json
      sprites/
      palette.json
      palette.png
      lora/
      fonts/
  generated/                 # the generated layer, keyed by art pack id, outside the pack (§1.2)
    pixel_crawler/
      sprites/               # additive; the art pack's own sprites always win
      definitions/           # agent-created objects/tiles from §4.3, never the hand-authored catalog
      glyphs.json            # glyph allocations (§4.4), so two creations cannot collide
      gaps/
        village.json         # coverage misses: a property of the PAIR, not of either pack alone
        cyberpunk_shop.json
      creations.log          # what an agent invented, when, and any balance verdict (§6, §4.7)
```

The split between `art/<pack>/sprites/` and `generated/<pack>/sprites/` is the operational meaning of
"undiluted": **only the former is ever read as a style reference.** Everything that models style — the
LoRA training set, the palette derivation, the reference images passed to the sheet API — reads
`art/<pack>/`, and nothing else. The pack directory therefore stays exactly as its author curated it,
and can be replaced or upgraded wholesale without losing generated work or migrating it.

`gaps/` is per-pair because the *inventory of misses* depends on which data pack was loaded. A
generated `bed` sprite is not, so it is stored flat and reused across data packs (§1.2).

`--data-pack village --art-pack dark_fantasy` expands to `--settings packs/data/village/settings.json`
plus an `art.pack` override, added to the same `for` loop in `src/main.cpp` that already handles
`--settings`. Keeping `data/` as the default data pack and not requiring an art pack means nothing
breaks before the split exists.

The new `GameSettings` section is deliberately tiny, because everything else already exists:

```cpp
struct ArtSection {
  // Art pack directory. Empty means "sprites straight from the data pack", which
  // is today's behaviour and keeps an art-pack-less run working.
  std::string pack;
  // Generated layer root. Empty means "generated/ beside packs/", which is the
  // default; never inside the art pack (§1.2).
  std::string generated;
} art;
```

From `art.pack`, `sprites/`, `palette.json`, `pack.json` and `lora/` are found by convention — the
same "convention over configuration" the sprite lookup already uses.

---

## 10. Design-time by default, runtime only as a queue

### 10.1 The measurements forbid the frame loop

`tools/README.md` already answered this: **1 usable in 5–10, 10–30 s each** on this card. In the frame
loop an agent stalls mid-action for minutes to produce something probably rejected, while sharing VRAM
with whatever is serving the NPC LLM.

So the default mode is **offline**: run the game with a data pack and an art pack, collect `gap.json`,
drain it between sessions. The art pack grows; the frame rate does not notice. This is also why §6's
gap log comes first — it makes offline mode *sufficient*, because the work is captured rather than
lost.

### 10.2 If it must be live: the queue

The engine is well-shaped for this, because the fallback already exists and is already the normal
case — and after §3, a partially-covered art pack is the *expected* state rather than an edge case.

```
agent asks for an object; the art pack has no sprite for it
        |
        v
entity spawns with its data-pack glyph  <- already happens today, already warns, no crash
        |
        v
coverage gap enqueued at low priority
        |
        v
worker generates + cuts  ->  PNG lands in the generated layer's sprites/
        |
        v
texture cache invalidated  ->  next frame draws the sprite
```

The glyph is not a placeholder to be tolerated; it is a legitimate rendering, and it comes from the
data pack so it is present regardless of art coverage. An agent that builds a confessional and gets a
readable placement-radar glyph immediately, with art arriving a minute later, is a coherent
experience. The game never blocks on the image model.

**The one concrete blocker**, documented in `include/ResourceManager.h`: missing paths are memoised so
that a world renderer looking up a texture per entity per frame does not re-stat the filesystem sixty
times a second. The comment states the consequence plainly — *"a sprite file added while the game is
running is not noticed until the next map load."* Live mode needs that memo invalidated for one path
on completion. `MapReloader` already establishes the precedent for hot-reloading content.

**Runtime mode should only ever run a local model.** A queue that depends on a remote API is a queue
that stalls when the network does, mid-session.

---

## 11. Improvements worth folding in

### 11.1 Author the atlas slices once, instead of hand-writing cell maps forever

Art packs ship a `.json` beside every atlas — `Environment/Tilesets/Wall_Tiles.json`,
`Icons/Resources.json`, and so on. **Checked, and it is not what it looks like:** each is a
single-frame Aseprite export describing the *whole* image (one frame rect covering 400x400, then
`meta`). No slice names, no per-tile rects, so nothing can be read from it yet.

What the pack does ship is the `.aseprite` **source** files. That makes a one-time authoring pass
possible: define slices in Aseprite, re-export, and the cell names and rects become data.

Why it is worth the afternoon:

- `extract_sheet.py` currently carries a hand-maintained `NAMES` dict mapping `(row, col)` to a name.
  Exactly the kind of thing that silently rots. The dict's own comment already encodes a design
  decision — that cell (0,0) is *"the church building, which is a multi-tile object and cannot be
  expressed in the current engine — skipped"* — which belongs in data, not a comment.
- Aseprite slices describe rectangles, not grid cells, so a multi-tile building is expressible as one
  named slice. A step toward `todo`'s "Allow multi-tile objects" rather than orthogonal to it.
- The cut stage stops being pack-specific and becomes "read the named rects" — the precondition for
  running the pipeline against an art pack it has never seen, which is the whole point of §3.

### 11.2 Keep the raw renders, always

`gen_tile.py` already defaults `--raw-dir` to `/tmp/air_tile_raw` and `tools/README.md` says to keep
them: *"When a result looks wrong, the first question is always 'is it the model or the downscale?',
and they answer it."* The pipeline must not regress this into a `tempfile` that is cleaned up.

### 11.3 A coverage report is a product feature, not just tooling

Once §3 is accepted, "this art pack covers 62% of the village data pack" is a number worth showing. It
tells an author what to generate next, and it tells a player whether a combination will look
half-finished before they start. `gap.json` is that number; it should probably be surfaced rather than
left in a file.

---

## 12. Staging

Each stage is useful alone, and none requires the next.

| stage | status | what it delivers | depends on |
|---|---|---|---|
| **1. Art root** | ✅ **done** (§9.2) | `art.pack` + `art.generated` + `--art-pack`, replacing the one `constexpr` | nothing |
| **2. Palette lock** | ✅ **done** (§7.3) | `tools/palette.py`; both slicers and `gen_tile.py` snap to one shared palette | stage 1 for the art root, but independently testable — it accepts any sprite directory |
| **4. Tile identity** | ✅ **done** (§4.5) | `tilePalette` + one-loop load/save, bounds-checked; legacy `tileInfo` still read | nothing |
| **0. Gap log** | next | a worklist of coverage misses, and the count from §11.3 | nothing. Vocabulary half already exists in `CheckVocabulary` (§5) |
| **3. Drained pipeline** | | `gap.json` -> sheet -> cut -> palette, one command, writing art only | stages 0–2 |
| **5. Field split** | | `sprite`/`art`/colours moved out of data definitions into the art pack | stage 1, and a format decision |
| **6. Creation path** | | allowlist + glyph allocation + generated definitions (§4.3–4.6) | stages 4 and 5 — 4 is now done |
| **7. LoRA training** | | offline, deterministic, API-free style reproduction | a coherent art pack with source-res art |
| **8. Live queue** | | art arrives during play | `ResourceManager` memo invalidation |
| **9. Balance agent** | | a second opinion on every creation (§4.7) | stage 6, and a few dozen real creations to calibrate on |

**The order actually taken was 1, 2, 4 — not 0 first.** That reverses the note that used to sit here,
and the reason is that §5 turned out to be wrong about stage 0: `CheckVocabulary` already detects and
deduplicates vocabulary misses, so the gap log is mostly a matter of *exposing records that already
exist* rather than building a detector. Stages 1, 2 and 4 were all smaller than their sections
predicted, and 2 and 4 are independently testable without any pack existing at all — which made them
the better things to do while the shape of the gap log was still open.

Notes on the remaining ordering:

- **Stage 0 next.** Cheap, invisible when idle, and the only stage that says what the feature is
  actually for. One church is one data point; whether agents ask for ten more buildings or ten
  thousand props changes the design, and today that is unknown.
- **Stage 5 is still deliberately after the pipeline.** Moving the art fields out of the data
  definitions is the "proper" version of the split, but it is a **format migration** across 20 tile, 38
  object and 1 race file, and it buys nothing that stage 1 does not already buy — stage 1 made the
  *sprite root* independent, which is the part that changes what recombines. The 20 tile files
  include several that are misclassified objects (walls, fence, anvil, forge, trough); the count will
  shrink when the vocabulary debt is retired, so do not use it as a target for new tile definitions.
- **Stage 7 is deliberately late.** Most expensive, least urgent: the sheet-API backend reaches the
  same bar without it, and it is only worth the GPU hours once an art pack is coherent, has
  source-resolution art, and the volume justifies removing the API.
- **Stage 9 last, and advisory first.** §4.7 — a gate nobody has calibrated is worse than no gate.

---

## 13. Open questions

Deliberately not resolved here.

1. **Where does a generated definition lose to a hand-authored one?** §1.2 says the packs always win
   for *art*. For definitions the case is sharper: if an agent generated a `bed`, and a later data pack
   ships its own `bed` with different components, the hand-authored one must win — but the generated
   `bed` may then have been built into a saved world. Whether that is a benign substitution or a
   compatibility problem is unexamined.
2. **Does a generated tile survive the data pack it was made for?** Generated tiles are decorations,
   so they are reusable in principle. But tile ids interact with the catalog in a way object ids do
   not, and §4.5's migration has to decide whether a generated tile is part of the *palette* or an
   overlay on top of it.
3. **Does moving the art fields (stage 5) use a per-name override file or per-definition split?**
   A single `overrides.json` keyed by name is easy to layer and diff; splitting every definition into
   `<name>.data.json` + `<name>.art.json` is more honest about the model but triples the file count and
   needs a merge story. Stage 1 defers it safely.
4. **Does the art pack own the font and chat colours?** `render.fontPath` is already a setting and
   `Defaults.h` holds chat colours. A dark fantasy pack plausibly wants both; they are art by §2's
   test, but they are also *legibility*, which is closer to accessibility than to style.
5. **Who reviews a generated batch?** §7.4 says nothing enters the generated layer unreviewed. Human
   at a contact sheet, or a second model asked "does this match the art pack?" — open, and the contact
   sheet already exists so the human path is nearly free.
6. **Should the designer agent see the gap log?** Today `%MATERIALS%` is the data pack's catalog.
   Showing the designer the art pack's coverage would let it prefer materials that already have art —
   but §3.2 forbids letting that become a gate. Advisory-only is the constraint; whether it is worth
   doing at all is open.
7. **Does an art pack carry a semantic hint at all?** A "dark fantasy" pack might reasonably want the
   designer to favour certain materials. That is taste, not coverage, and it must not reduce the
   vocabulary. If it happens, it belongs in the art pack's `pack.json` as prose added to the designer
   prompt, clearly marked advisory.
8. **What happens to the generated layer when the art pack is replaced?** The layer is keyed by pack
   id, so a pack upgraded in place inherits generated art made against the *old* style. That is
   probably wrong — the layer may need to be keyed by pack id **and version**.
9. **Multi-art-pack layering** (a base pack plus a small override pack) falls out of name-keyed
   overrides almost for free. Worth one line in `pack.json` when someone wants it; not now.

---

## 14. What not to do

- **Do not let an art pack change `character`.** It is the LLM's symbol table, not a sprite (§2.1).
  Changing it changes agent reasoning; colliding it corrupts the placement radar.
- **Do not let an art pack gate the data pack's vocabulary.** Swapping art must never change what can
  be built (§3.2).
- **Do not let the bulk art pipeline write data definitions.** §4.2 — and note that `gen_tile.py
  --emit-tile-json` and `import_church.py` both do exactly this today. Removing that is the first fix.
- **Do not write generated art into the art pack.** It goes in the generated layer, outside the pack
  (§1.2). Anything a style model reads must be hand-curated.
- **Do not train a LoRA or derive a palette from the generated layer.** That is the dilution this
  whole layering exists to prevent, and it is the standard way a model collapses into its own output.
- **Do not let the generated layer shadow the art pack.** Additive only; the pack always wins, and a
  generated sprite yields silently when the pack ships the real one.
- **Do not let a created object carry an economy component.** Allowlist is `Interactable` +
  `Obstacle`, nothing else (§4.3). Refuse with the reason; never silently downgrade.
- **Do not allocate a glyph without checking the whole world's `character` set.** §4.4.
- **Do not allow agent-created tiles before tile identity is fixed.** §4.5 — the corruption is silent
  and turns every wall cell into floor. (And do not create a wall as a tile in the first place: it is
  an object — see `tile-and-object-vocabulary.md`.)
- **Do not generate one asset per request.** Measured at 1,290 image tokens per output regardless of
  subject count, and internal consistency is lost. Batch.
- **Do not train on `data/sprites/`.** Derived, mixed-origin, 32x32. Train on the art pack's source.
- **Do not caption content on a style adapter.** One trigger token, §8.2.
- **Do not quantise after downscaling.** Quantise at 512, then `NEAREST` (§5).
- **Do not judge at 512px, or in an image viewer.** Contact sheet, then a 10x10 patch, then in-game.
- **Do not judge an art pack in isolation.** Judge it over a real data pack, because a half-covered
  field is the normal recombined state (§8.4).
- **Do not put diffusion in the frame loop.** §10.1.
- **Do not let generation write components it inferred.** `Interactable` vs `Obstacle` vs `Storage` is
  a behavioural decision. §7.4.
- **Do not let this touch `GameECS.cpp`'s system list, `AgentBrain`, or the command DSL.** It is a
  content pipeline. It reaches the game through the sprite root and the catalogs, and nothing else.
