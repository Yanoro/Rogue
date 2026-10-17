# Blueprints: hand-authored and agent-authored buildings

**Status: implemented, in part.** Design half is live and exercised end to end by an agent; the
build half is not. This document describes what the code does today, and marks the parts of the
original design that were never built rather than quietly dropping them.

Written 2026-10-14, rewritten 2026-10-16 against the shipped code. Where the implementation
diverged from the original proposal, the divergence is stated.

**Companion documents**, and the split between them:

| document | covers |
|---|---|
| `asset-generation-design.md` | the generated overlay: how art and tile definitions come to exist for material no hand-authored pack has |
| `designer-speech-design.md` | reading the designer's reply aloud |
| `placement-design.md` | the placement validator blueprints reuse for site legality |
| this document | the grammar, expansion, the registry, the designer agent, and the gaps |

---

## 1. What a blueprint is

A **blueprint** is a structure written down as data. It has no absolute position: every coordinate is
relative to the design's own origin, usually `[0,0]`, and where it ends up is decided later.

- **Design** — produce a blueprint. Expensive, happens once per structure, output is data, checked
  before anything is saved.
- **Construct** — stamp a blueprint into the world at a site. **Not implemented.** See §9.

Design's output is **data with a name, not a diff to the world.** That is the property that makes
the vocabulary check, the preview and a future construction pass able to treat all designs alike.

It also means **a hand-authored blueprint and an agent-authored one are the same artifact.** There is
no marker distinguishing them; `data/blueprints/` currently holds both, and nothing reads them
differently. The registry records a `sourcePath` for reporting and nothing more.

---

## 2. The grammar

Two ideas carry the language, and keeping them apart is what makes a design checkable:

- **A shape is a set of cells.** Nothing more.
- **A part is a shape filled with a material** (`paint`), something standing on those cells
  (`place`), an opening cut into them (`carve`), or the enclosed interior of a shape (`fill`).

A house is therefore a defined rectangle for the floor, a defined border for the walls, a carved
doorway and a door tile — not two hundred decisions.

**Filled or outline is always in the primitive's name.** There is no `filled` boolean anywhere, and
no `thickness`: one spelling per cell set is what keeps overlap unambiguous and a round trip exact.

### 2.1 Top-level fields

| field | required | meaning |
|---|---|---|
| `id` | **yes** | Short identifier, `[a-z0-9_]`. It is the filename and the reference. Saving under an existing `id` **overwrites**. |
| `name` | **yes** | Display name. Defaults to `id` when absent. |
| `parts` | **yes** | Non-empty array. Later parts overwrite earlier ones on the same cell. |
| `purpose` | no | One or two words. Persisted and round-tripped; nothing reads it yet. |
| `owner` | no | Persisted and round-tripped. **Nothing enforces it** — see §8. |
| `levels` | no | Defaults to 1. Parsed and written back. **Only level 0 is placeable** — see §7. |
| `regions` | no | Named shapes, so a shape used twice is written once. |

`designerNotes` is deliberately **not** in this table. The designer agent is required to produce it
(§6.3) and the engine erases it before saving, so it never appears in a file on disk.

### 2.2 Shapes

| primitive | body | notes |
|---|---|---|
| `point` | `at` | one cell |
| `line` | `at`, `to` | inclusive; axis-aligned or stepped (Bresenham) |
| `rect_fill` | `at`,`to` or `at`,`width`,`height` | solid rectangle |
| `rect_outline` | `at`,`to` or `at`,`width`,`height` | one-cell border — the wall primitive |
| `disk_fill` | `at`, `radius` | solid circle |
| `disk_outline` | `at`, `radius` | one-cell border |
| `polygon_fill` | `points` (3+) | solid polygon, may be concave; even-odd rasterisation |
| `polygon_outline` | `points` (3+) | its one-cell border |
| `region` | `ref` | a named shape from `regions` |
| `union` / `intersect` / `difference` | `of` (array of shapes) | set algebra |

`at` and `to` are `[x, y]` with **y growing downward**, and `to` is inclusive — `rect_fill` from
`[0,0]` to `[3,3]` is 16 cells. `at`/`to` are normalised, so either corner may be given first.

A one-cell-wide shape still has a border: `rect_outline` with `width: 1` is a line of cells, not
nothing. Outlines are a genuine border (every cell with an orthogonal neighbour outside the set),
not a scanline approximation — an earlier version used per-column min/max, which punched the top and
bottom rows of a solid rectangle through the middle and made every generated building's walls wrong.

### 2.3 Parts

```json
{ "id": "walls", "region": "outer_walls", "op": { "paint": "wooden wall" }, "role": "structural" }
```

| field | meaning |
|---|---|
| `id` | **required**, unique within the design. Names the part in every error message. |
| `shape` | a shape, inline: `"shape": { "rect_fill": { "at": [0,0], "to": [5,5] } }` |
| `region` | **or** a region's name as a **plain string**: `"region": "footprint"`. Exactly one of `shape`/`region`. |
| `op` | **required**, exactly one of the four below |
| `role` | optional: `structural`, `entrance`, `interior_door`, `fixture` |
| `level` | optional: `2`, or `[1,3]` for a span. |

`"region": { "ref": "footprint" }` is **wrong** and refused by name. An early draft of this document
showed that form; it is a natural mistake, it was written into two shipped designs, and it used to
surface only as an opaque `type must be a string` thrown out of the JSON parser. `ParsePart` now
catches it explicitly and says so.

The four operations:

- `"op": { "paint": "wooden floor" }` — write a tile into every cell
- `"op": { "place": "chest" }` — spawn an object into every cell
- `"op": { "carve": true }` — clear the cell
- `"op": { "fill": { "paint": "wooden floor" } }` — fill the **enclosed interior** of the shape

### 2.4 A complete, working example

Trimmed from a real agent-authored design in `data/blueprints/`:

```json
{
  "id": "small_house",
  "name": "Small House",
  "purpose": "dwelling",
  "levels": 1,
  "regions": {
    "footprint": { "rect_fill": { "at": [0, 0], "to": [6, 5] } },
    "outer_walls": { "rect_outline": { "at": [0, 0], "to": [6, 5] } }
  },
  "parts": [
    { "id": "floor", "region": "footprint", "op": { "paint": "wooden floor" } },
    { "id": "walls", "region": "outer_walls", "op": { "paint": "wooden wall" }, "role": "structural" },
    { "id": "doorway", "shape": { "point": { "at": [3, 5] } }, "op": { "carve": true } },
    { "id": "door", "shape": { "point": { "at": [3, 5] } }, "op": { "paint": "door" }, "role": "entrance" }
  ]
}
```

Order matters: floor first, then walls over it, the doorway carved out of the wall, then the door
written onto the carved cell.

Note what real designs actually use. Across the 14 blueprints in `data/blueprints/`, the primitives
are `point` (76 uses), `rect_fill` (11) and `line` (8) — **`rect_outline` is used only through named
regions, and `fill` is never used at all.** The designer builds walls out of many `point` and `line`
parts rather than composing `rect_outline`. That is legal and it works, but §6.4 explains why it is
worth steering the model away from.

---

## 3. Expansion: shapes to cells

`include/BlueprintExpansion.hpp` / `src/BlueprintExpansion.cpp`. Nothing here touches the world; it
turns a parsed design into cells and is separately testable (it was, during development, by linking
this translation unit alone).

```cpp
using Cell = std::pair<int,int>;          // ordered by (y, x)
using CellSet = std::set<Cell>;

CellSet RasterizeShape(shape, regions, depth, error);
CellSet RasterizePart(part, regions, error);
bool    Interior(const CellSet &region, CellSet &out);
void    ComputeBounds(blueprint, minX, minY, maxX, maxY);
Expansion Expand(const Blueprint &blueprint);
```

- **Per part, then merged in order.** Later parts win, which is what lets a carve open a wall.
- **`level != 0` parts are skipped** by both `Expand` and `ComputeBounds`. The world is
  single-layer, and a preview of a floor the map cannot show would mislead rather than inform.
- **`fill` uses `Interior`**, a 4-connected flood fill from outside the shape's bounding box:
  unreached free cells are the interior. A shape with no interior — solid, or a border that leaks —
  is reported as an error rather than silently producing zero cells. `Interior` is the same
  primitive a topological check would need; see §8.
- **Region references are depth-guarded** (`kRegionDepthCap = 8`) so a region that refers to itself
  terminates with an error instead of hanging an agent's turn.
- **Errors are collected, not fatal.** The designer's preview draws a partially valid design on
  purpose, so a refusal shows you what went wrong.

### 3.1 Cell kinds

Shape and material meet in exactly two kinds of output, mirroring the two occupancy systems the
world already has:

| kind | produced by | what it is |
|---|---|---|
| tile | `paint`, `fill{paint}` | a `TileDef` name resolved through the catalog. The **ground**. |
| object | `place`, `fill{place}` | an `ObjectFactory` template id. Something **standing on** the ground. |

They differ in cost and visibility: terrain is free to paint and invisible to `[SURROUNDINGS]`, an
object consumes a real item and gets a `DisplayName`. `carve` writes neither — it marks the cell as
an opening, which both renderers draw as a hole.

---

## 4. Serialisation

`BlueprintRegistry::WriteBlueprint` is the inverse of the parser, so a design written out and read
back is the same design. It is used instead of re-emitting the model's own text, because that text
carries fields the schema does not define — `designerNotes` above all — and the file on disk should
be exactly the schema.

Regions are written sorted, and a part that referenced one is written as `"region": "name"`;
everything else is flattened inline. The fill state survives a round trip because it is in the
primitive's own name rather than a flag.

---

## 5. The registry

`include/BlueprintRegistry.h` / `src/BlueprintRegistry.cpp`. Same shape as `StatRegistry` and
`SkillRegistry`: a content database loaded from a directory, injected where needed, reachable through
`BlueprintRegistryResource`.

| call | does |
|---|---|
| `LoadBlueprints(dir)` | reads `*.json`; **clears first**, so a reload replaces rather than accumulates |
| `ParseBlueprint(json, id, source, out, error, warnings)` | static; the same path for a file and for a model's reply |
| `SaveBlueprint(bp)` | writes `<dir>/<id>.json`, then reloads and verifies it came back |
| `DeleteBlueprint(id)` | removes the file, then reloads |
| `Reload()` / `Clear()` | re-read / drop everything |
| `Get`, `GetIds`, `GetAll` | lookup; see the lifetime note below |
| `WriteBlueprint(bp)` | serialise |

### 5.1 Lifetime, which has already caused one crash

`Get`/`GetAll` return pointers into the registry's map. They are valid **only until the registry
reloads** — that is, until `LoadBlueprints`, `Reload`, `SaveBlueprint` or `DeleteBlueprint`. The
header states this with the failing example, because the previous comment claimed pointers were
"stable for the registry's lifetime" and that stopped being true the moment `Reload` existed.

`SaveBlueprint` and `DeleteBlueprint` **copy their arguments on entry**. That is not style: callers
naturally pass a reference into the registry — `BlueprintWindow` passes a key, `DesignerChatWindow`
passes a design it just looked up — and `Reload()` at the end frees it. This was a real
heap-use-after-free (`DeleteBlueprint → LoadBlueprints::clear → Get(id)`), found by ASan and fixed by
copying at the boundary.

### 5.2 Rejections are errors, not warnings

A design missing from the window looks exactly like a design that was never authored. So a file that
fails to parse, fails `ParseBlueprint`, or expands to no cells is reported through `LogError`, with
the file named and — for a JSON type error — a plain-language hint about the usual cause.

Warnings are deduplicated per load by message, and the vocabulary check is grouped by material
rather than by part, so one wrong material used by six parts is **one** line naming all six. That
grouping was necessary: reporting per part produced six near-identical lines and buried the log.

---

## 6. The designer agent

`include/DesignerChatWindow.h` / `src/DesignerChatWindow.cpp`. A development window that turns a
request in plain language into a blueprint, so a model can be judged on *designing* before any of it
is wired to the in-game agents.

It is deliberately isolated, like `VisionChatWindow`: its own `contextId`, never touching
`NPCContext`, `AIRequest` or the agent thread pipeline, so it cannot disturb the NPC loop.

### 6.1 Backend

Its own settings section, `settings.designer`, defaulting to `useMainModel: true` so an unconfigured
run shares the NPC backend. Separate from `model` for two reasons: `OllamaAI` keys conversation
context by `contextId` alone so one instance cannot serve two models, and producing valid JSON is a
different skill from roleplay, so pointing it at a different model is a real thing to want.

### 6.2 Instructions are data

`data/blueprints/DESIGNER.md` holds the reply contract, the grammar, the validity rules and a worked
example. `Game::BuildDesignerSystemPrompt` appends the **material palette generated from the live
tile catalog and object templates**. Generated rather than written into the file because a
hand-written list drifts the moment a tile is added, and the failure is quiet: the designer asks for
a material that does not exist and every design is refused.

The `%MATERIALS%` placeholder is substituted when present and appended when absent, so a prompt file
that forgets it still gets the vocabulary.

### 6.3 The reply contract

1. Prose, then exactly one fenced ` ```json ` block.
2. The block must carry a non-empty **`designerNotes`** — the model's own critique of the design it
   just made. Required, not decorative: it is the thing the window exists to collect, and a reply
   without it is refused.
3. The engine extracts the block, strips `designerNotes`, parses, checks the vocabulary, and expands.
4. Valid → `SaveBlueprint` → reload → the design appears in the Blueprints window immediately.
5. Invalid → the reason is appended to the same conversation and it retries, up to
   `settings.designer.maxDesignAttempts` (default 3). **Every attempt is shown**, including
   refusals; hiding them would make a retry look like one slow success.

Inspection runs on the **main thread** after the transcript is drawn. Validating and saving content
from the worker would mean touching the ECS mid-frame.

### 6.4 What the model gets wrong, and the one structural fix still owed

The vocabulary is enforced and the expansion is checked. What is **not** checked is anything
topological: whether the walls seal, whether there is an entrance, whether the interior is
reachable. `role` is parsed and carried but **nothing enforces it**.

This matters because of how the model actually designs. Left to itself it builds walls from many
`point` and `line` parts (see §2.4), and a wall made of separate `line` parts is connected only
*diagonally* at corners, so a 4-connected flood fill leaks out through the corner. The
`DESIGNER.md` rules tell it to use `rect_outline` for walls; nothing checks that it did.

The fix is the enclosure check described in §8, using the `Interior` primitive that already exists.

### 6.5 Other behaviour

- **Preview pane** — the latest attempt is rendered beside the transcript through
  `include/BlueprintPreview.hpp`, at the game's native tile size by default, and is drawn even when
  the design was refused, because a debugging aid should show something rather than nothing.
- **Toggleable transcript** — reasoning and JSON can be hidden independently (`showThoughts`,
  `showJson`), with a persisted splitter for the pane width.
- **Speech** — the saved design's name and `designerNotes` are read aloud on the attempt that lands,
  never on intermediate refusals. **Off by default** (`settings.designer.tts.enabled`): it is a
  development window, and a feature that starts making noise on first launch gets turned off before
  it is judged. `settings.designer.tts.content` selects the payload; the JSON is never spoken except
  under the `everything` setting, which exists to debug that pipeline. See
  `designer-speech-design.md`.
- **Prompt rewriter** — an "Improve" affordance on the input box, briefed for building requests.
- **Session log** — every prompt, reply, save and refusal is appended to a log file, with the prompt
  file and session header recorded, so a bad design can be traced to what the model was actually
  told.

---

## 7. Placement and rendering

- **Ghost** — a design selected in the Blueprints window follows the mouse in the world pass
  (`GameDraw.cpp`), drawn as the real tile art at partial alpha, tinted green or red per cell.
- **Legality is deliberately cheap**: in bounds, not occupied by an object, not on a blocking tile.
  The full structural validator is not written, and a debug tool must be able to place a broken
  design in order to show it.
- **`PaintTile`** is shared with the map editor, so there is one implementation of "put this tile
  here and refresh the cell's visual".
- **Persistence** — objects are always stamped `MapAuthored`; painted terrain is written to the map
  file only when the window's checkbox is on.
- **Levels.** Only level 0 builds. `level` and `levels` are parsed and round-tripped so designs do
  not have to be re-authored when `Map` becomes multi-layer, and `ComputeBounds` ignores higher
  levels so the footprint stays honest.

---

## 8. Vocabulary gaps and generated content

Two things can be missing from a design, and they have separate machinery:

**Undefined vocabulary.** `FindMissingVocabulary` is a free function (static-parseable, and shared
with the report below) returning one `MissingMaterial` record per `(kind, material)`. A missing
material is a **warning, not a refusal**: it makes the design place fewer cells, and refusing would
leave the designer with no way to see what it got wrong.

**Missing art.** `GapLog` builds a report of coverage gaps (a tile or object with no resolvable
sprite) and vocabulary gaps, from the catalog, the object templates and every loaded blueprint.
`--gaps` prints it and exits without opening a window, which makes it scriptable: the art generator
drains a worklist rather than a human reading a list.

That worklist feeds `TileArtQueue`, a generated layer for tiles and art that hand-authored packs do
not cover. `TileRegistry::AddRuntime` is the only path by which a definition invented while the game
is running enters the catalog, and it validates identity hard: name present and free (or explicitly
replaced, and only for its own runtime layer), a glyph or a sprite so the tile is visible,
single-character glyphs, in-range colours, a positive hitbox, no `..` in a sprite path, and
**unknown keys refused** so a model that invents `blocksMovement` gets told rather than silently
ignored. A duplicate glyph is a warning only.

**A correction to the earlier draft of this section.** The catalog and the generated layer are *not*
separate directories today. `AddRuntime` is an in-memory layer over the same catalog, and the
distinction is "was this added at runtime" (`IsRuntimeAdded`), not "which directory did it come
from" — the file is still written under `data/tiles/`. `asset-generation-design.md` §4.4 argues the
overlay *should* be its own directory so a data pack a designer designs against never contains
agent-created content, and notes that the art tooling currently writes into the catalog; that move
is designed and not made. Reloading the layer is `RemoveRuntimeTiles` plus a re-add.

`TileDesignerWindow` is the art counterpart to the building designer: the same chat shape, plus a
tile editor (glyph, colours, hitbox, sprite), the art job's artifacts, a pixel view and a filmstrip.
It is reachable headlessly through `--tile-design`, with `--tile-art*` flags for the art pipeline.
See `asset-generation-design.md`.

**A schema boundary worth knowing:** blueprints reference tiles **by name**, and a name resolves
through the catalog. Agent-created tiles therefore do not live in `data/tiles/` and a design naming
one is only portable to a run where the overlay is loaded. Nothing resolves a design's materials
against a map, deliberately — a blueprint is map-independent and the designer has no map.

---

## 9. Not built

Stated plainly, because the grammar was designed to accommodate these and a reader should not infer
them from the schema:

- **Composite sugar.** `enclosed`, `room`, `connect`, `repeat` and `call` were proposed in the first
  draft and **none exist**. The parser rejects them as unknown primitives. They were the least
  certain part of the design; the equivalence rules (`room` = walls plus interior) were more
  machinery than the model needed.
- **Every topological check.** No sealing check, no entrance check, no interior-reachability check,
  no fixture-inside check. `role` is inert.
- **Construction.** Nothing materialises a design into the world over turns or consumes materials
  for it. The blueprint is painted directly by the debug window, for free. This is the largest gap
  between the design and the game.
- **Site search.** Nothing finds a valid site for a design. Placement is manual and
  anchor-at-the-mouse.
- **Ownership.** `owner` is written and read but nothing enforces or scopes it. The standing
  decision is that all agents can see and use every design, with a debug window as the later control
  point.
- **Rotation.** A design places at one orientation.
- **Multi-level building.** Parsed, preserved, not built.

---

## 10. Limits and known issues

- **`maxDesignAttempts`** bounds retries per request (default 3); the budget resets when you send a
  new message.
- **A design that parses but expands to nothing is refused before saving**, so it cannot appear in
  the list as an empty building.
- **A `place`d object in a preview is not drawn as art.** The pane has no instance of the template,
  so the cell is tinted and labelled with the object's name instead.
- **A tile's file stem does not have to match its `name`, and often will not.** Resolution is by
  `name` only (`"stone floor"` lives in `stone_floor_tile.json`), so a mismatch is harmless and in
  some cases deliberate — the filename is free to describe the art the file carries. It is not a
  rename waiting to happen. (Keying the registry on the stem instead *was* a real bug: it made every
  multi-word tile — `wooden wall`, `water trough` — unresolvable, which surfaced as blueprints
  reporting that walls were missing from the catalog.)
- **`Map`'s `positions` array and `tileInfo` can desynchronise** under a duplicate tile name; see the
  note in `todo`. Unrelated to blueprints, but it is the reason a saved map must not gain duplicate
  tile names.

---

## 11. References

- `include/Blueprint.hpp` — the data model, and the comments that state why the grammar is shaped
  this way
- `include/BlueprintExpansion.hpp`, `src/BlueprintExpansion.cpp` — rasterisation, `Interior`,
  bounds
- `include/BlueprintRegistry.h`, `src/BlueprintRegistry.cpp` — parse, save, delete, reload,
  serialise, `FindMissingVocabulary`
- `include/BlueprintPreview.hpp` — the shared preview renderer
- `include/DesignerChatWindow.h`, `src/DesignerChatWindow.cpp` — the designer agent
- `data/blueprints/DESIGNER.md` — the designer's instructions, including the reply contract
- `src/Game.cpp` — `BuildDesignerSystemPrompt` (prompt file plus generated palette),
  `BuildBlueprintPlacement`, `PaintTile`
- `src/GameDraw.cpp` — the world-space ghost
- `include/GapLog.h`, `src/GapLog.cpp` — coverage and vocabulary gap reports
- `include/TileRegistry.h`, `src/TileRegistry.cpp` — the catalog and its generated overlay
- `include/TileDesignerWindow.h` — the art counterpart
- `data/blueprints/*.json` — hand-authored and agent-authored designs, side by side
